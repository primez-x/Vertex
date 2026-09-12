#include "sketch/project_exchange.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"
#include "support/noninteractive_errors.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void test_translation_export(const std::filesystem::path& root) {
  sketch::BoundaryConstructionRecord record;
  record.schema_version = sketch::boundary_receipt_schema_version_v2;
  record.boundary_id = "boundary";
  const sketch::Vec2 points[]{{0, 0}, {2, 0}, {2, 1}, {0, 1}};
  sketch::IdentifiedBoundary boundary{record.boundary_id, "measurement_boundary", {}};
  for (std::size_t i = 0; i < 4; ++i) {
    const auto edge = "edge-" + std::to_string(i);
    const auto start = "vertex-" + std::to_string(i);
    const auto end = "vertex-" + std::to_string((i + 1) % 4);
    sketch::ConstructionReceipt receipt;
    receipt.segment_id = edge;
    receipt.kind = sketch::BoundaryConstructionKind::line_to_point;
    receipt.start = points[i]; receipt.chord_end = points[(i + 1) % 4];
    record.edges.push_back({edge, start, end, receipt});
    boundary.segments.push_back({edge, start, end, {points[i], points[(i + 1) % 4], 0}});
  }
  auto entity = sketch::encode_identified_boundary_entity(boundary);
  entity.properties["boundary_authoring"] = sketch::encode_boundary_receipt_envelope(record);
  auto document = sketch::Document::create({entity});
  document.apply(sketch::TranslateBoundary{0, {"boundary", {8, -4}}});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "translation");
  std::ifstream input(root / "translation" / "project.json");
  const auto json = nlohmann::json::parse(input);
  check(json.at("exchange_version") == 2, "translation export must advertise its proof schema");
  const auto& rows = json.at("revisions");
  check(rows.size() == 3 && !rows[0].contains("boundary_translation") &&
        !rows[2].contains("boundary_translation"), "proof belongs only to its command revision");
  const auto proof = sketch::decode_boundary_translation(rows[1].at("boundary_translation"));
  check(proof.boundary_id == "boundary" && proof.offset.x == 8 && proof.offset.y == -4,
        "export must retain proof even in undone history");
  document.redo(document.revision());
  sketch::PlanarTransform transform;
  transform.pivot = {1, 0.5}; transform.rotation_radians = 0.25;
  transform.flip_horizontal = true; transform.offset = {2, 3};
  document.apply(sketch::TransformBoundary{document.revision(), {"boundary", transform}});
  // A later translation must not overwrite the newer exchange version.
  document.apply(sketch::TranslateBoundary{document.revision(), {"boundary", {1, 2}}});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "transform");
  std::ifstream transformed_input(root / "transform" / "project.json");
  const auto transformed_json = nlohmann::json::parse(transformed_input);
  check(transformed_json.at("exchange_version") == 3, "mixed transform export must advertise version3");
  const auto& transformed_rows = transformed_json.at("revisions");
  check(transformed_rows[4].at("boundary_transform") ==
        sketch::encode_boundary_transform({"boundary", transform}), "export must preserve every transform parameter");
  check(!transformed_rows[5].contains("boundary_transform") &&
        transformed_rows[5].contains("boundary_translation") &&
        !transformed_rows[6].contains("boundary_transform"), "export must keep proof roles separate");
}
} // namespace
int main() {
  sketch::testing::noninteractive_errors();
  auto root = std::filesystem::temp_directory_path() /
              ("property-exchange-" + sketch::make_stable_id());
  std::filesystem::create_directory(root);
  try {
    const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{0},
                                       std::byte{255}};
    auto asset =
        sketch::Asset::create("asset-a", "application/octet-stream", bytes);
    auto document = sketch::Document::create({}, {asset});
    auto duplicate = asset;
    duplicate.id = "asset-b";
    document.apply(
        sketch::ApplyEntityChanges{document.revision(),
                                   {},
                                   {sketch::AssetChange::upsert(duplicate)},
                                   "add repeated content"});
    const auto target = root / L"export-\u4F4F\u5B85";
    sketch::extract_project(document.snapshot(), target);
    std::ifstream input(target / "project.json");
    auto json = nlohmann::json::parse(input);
    input.close();
    auto rows = json.at("revisions");
    check(json.at("exchange_version") == 1 && !rows[0].contains("boundary_translation"),
          "proof-free exports must preserve the existing format");
    check(rows.size() == 2, "All revisions must be extracted");
    auto asset_path = rows[1].at("assets")[0].at("path").get<std::string>();
    check(asset_path == "assets/" + asset.sha256 + ".bin",
          "Asset path must be portable and content-addressed");
    std::ifstream asset_input(target / std::filesystem::path(asset_path),
                              std::ios::binary);
    std::vector<char> actual((std::istreambuf_iterator<char>(asset_input)), {});
    asset_input.close();
    check(actual.size() == bytes.size() &&
              static_cast<unsigned char>(actual.back()) == 255,
          "Extracted bytes must match");
    check(std::distance(std::filesystem::directory_iterator(target / "assets"),
                        {}) == 1,
          "Repeated content must be deduplicated");
    bool rejected = false;
    try {
      sketch::extract_project(document.snapshot(), target);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(rejected && std::filesystem::exists(target / "project.json"),
          "Existing extraction must remain untouched");
    for (auto fault : {sketch::ExtractFaultStage::after_assets,
                       sketch::ExtractFaultStage::before_publish,
                       sketch::ExtractFaultStage::after_publish}) {
      rejected = false;
      try {
        sketch::extract_project(document.snapshot(), root / "fault", {fault});
      } catch (const std::exception &) {
        rejected = true;
      }
      check(rejected, "Injected extraction failure must report an error");
      check(!std::filesystem::exists(root / "fault"),
            "Failure must not publish a partial destination");
      check(std::distance(std::filesystem::directory_iterator(root), {}) == 1,
            "Failure must clean all private staging data");
    }
    // A live extraction owns directory and payload identities until
    // publication.
    sketch::ExtractOptions tamper;
    tamper.stage_observer = [&](auto stage,
                                const std::filesystem::path &staging) {
      if (stage != sketch::ExtractFaultStage::before_publish)
        return;
      const auto displaced = root / L"displaced";
      check(!MoveFileExW(staging.c_str(), displaced.c_str(), 0),
            "Staging directory must deny rename-and-plant interference");
      check(!CreateDirectoryW(staging.c_str(), nullptr),
            "Reserved staging path must remain occupied");
      check(!MoveFileExW((staging / L"assets").c_str(),
                         (root / L"moved-assets").c_str(), 0),
            "Assets directory must deny rename during construction");
      for (const auto &path :
           {staging / L"project.json", staging / asset_path}) {
        const auto writer =
            CreateFileW(path.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writer != INVALID_HANDLE_VALUE)
          CloseHandle(writer);
        check(writer == INVALID_HANDLE_VALUE,
              "Payload must deny mutation before publication");
        check(!DeleteFileW(path.c_str()), "Payload must deny deletion");
        check(!MoveFileExW(path.c_str(), (root / L"moved-payload").c_str(), 0),
              "Payload must deny rename");
      }
    };
    sketch::extract_project(document.snapshot(), root / "protected", tamper);
    check(std::filesystem::exists(root / "protected" / "project.json"),
          "Protected directory must still publish successfully");

    // Once the exact root has been renamed, every expected descendant is
    // reopened relative to that retained root and revalidated. A mutation in
    // the unavoidable close-to-rename interval must therefore roll back.
    std::filesystem::path post_publish_staging;
    sketch::ExtractOptions post_publish_tamper;
    post_publish_tamper.stage_observer =
        [&](auto stage, const std::filesystem::path &path) {
          if (stage == sketch::ExtractFaultStage::before_publish) {
            post_publish_staging = path;
          } else if (stage == sketch::ExtractFaultStage::after_publish) {
            check(path == root / L"post-publish-tamper",
                  "Post-publication observer must receive the destination");
            std::ofstream output(path / L"project.json",
                                 std::ios::binary | std::ios::app);
            check(static_cast<bool>(output),
                  "Post-publication fixture must be able to mutate payload");
            output << "tamper";
          }
        };
    rejected = false;
    try {
      sketch::extract_project(document.snapshot(),
                              root / L"post-publish-tamper",
                              post_publish_tamper);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(rejected &&
              !std::filesystem::exists(root / L"post-publish-tamper") &&
              !std::filesystem::exists(post_publish_staging),
          "Post-publication tamper must be detected, rolled back, and cleaned");

    // If another object occupies the reserved staging leaf after publication,
    // rollback must fail closed and identify the contaminated destination.
    std::filesystem::path rollback_staging;
    const auto rollback_target = root / L"rollback-collision";
    sketch::ExtractOptions rollback_collision;
    rollback_collision.stage_observer =
        [&](auto stage, const std::filesystem::path &path) {
          if (stage == sketch::ExtractFaultStage::before_publish) {
            rollback_staging = path;
          } else if (stage == sketch::ExtractFaultStage::after_publish) {
            std::ofstream output(path / L"project.json",
                                 std::ios::binary | std::ios::app);
            check(static_cast<bool>(output),
                  "Rollback fixture must mutate the published payload");
            output << "tamper";
            output.close();
            check(CreateDirectoryW(rollback_staging.c_str(), nullptr),
                  "Rollback fixture must reserve the old staging leaf");
            std::ofstream(rollback_staging / L"foreign-marker.txt") << "keep";
          }
        };
    bool rollback_reported = false;
    try {
      sketch::extract_project(document.snapshot(), rollback_target,
                              rollback_collision);
    } catch (const sketch::ExtractionCleanupError &error) {
      rollback_reported = true;
      check(error.residual_path() == rollback_target,
            "Blocked rollback must report the contaminated destination");
      check(std::string(error.what()).find("post-publication") !=
                std::string::npos,
            "Blocked rollback must preserve the validation failure");
    }
    check(rollback_reported &&
              std::filesystem::exists(rollback_target / L"project.json") &&
              std::filesystem::exists(rollback_staging /
                                      L"foreign-marker.txt"),
          "Blocked rollback must preserve destination and foreign collision");
    std::filesystem::remove_all(rollback_target);
    std::filesystem::remove_all(rollback_staging);

    std::filesystem::path residual;
    {
      sketch::ExtractOptions options;
      options.fault_stage = sketch::ExtractFaultStage::before_publish;
      options.stage_observer = [&](auto stage,
                                   const std::filesystem::path &staging) {
        if (stage != sketch::ExtractFaultStage::before_publish)
          return;
        std::ofstream(staging / L"foreign-marker.txt")
            << "must survive cleanup";
      };
      bool reported = false;
      try {
        sketch::extract_project(document.snapshot(), root / "cleanup-denied",
                                options);
      } catch (const sketch::ExtractionCleanupError &error) {
        reported = true;
        residual = error.residual_path();
        check(std::string(error.what())
                          .find("Injected extraction write failure") !=
                      std::string::npos &&
                  std::string(error.what()).find("cleanup failed") !=
                      std::string::npos,
              "Cleanup error must preserve both failure causes");
        check(residual.parent_path() == std::filesystem::canonical(root) &&
                  std::filesystem::exists(residual / L"foreign-marker.txt"),
              "Cleanup failure must identify the exact remaining private "
              "staging directory");
      }
      check(reported,
            "Foreign contents must survive with an exact residual diagnostic");
      check(!std::filesystem::exists(root / "cleanup-denied"),
            "Blocked cleanup must never publish partial output");
    }
    check(residual.parent_path() == std::filesystem::canonical(root),
          "Cleanup retry must remain within this test's unique directory");
    std::filesystem::remove_all(residual);
    check(!std::filesystem::exists(residual),
          "Cleanup succeeds once the blocking handle is released");
    // A creator racing the final rename owns its destination. Failed
    // publication must clean only our released, identity-checked children.
    sketch::ExtractOptions collision;
    collision.stage_observer = [&](auto stage, const auto &) {
      if (stage == sketch::ExtractFaultStage::before_publish) {
        std::filesystem::create_directory(root / L"racing-destination");
        std::ofstream(root / L"racing-destination" / L"marker") << "keep";
      }
    };
    rejected = false;
    const auto before_collision =
        std::distance(std::filesystem::directory_iterator(root), {});
    try {
      sketch::extract_project(document.snapshot(), root / L"racing-destination",
                              collision);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(
        rejected &&
            std::filesystem::exists(root / L"racing-destination" / L"marker") &&
            !std::filesystem::exists(root / L"racing-destination" /
                                     L"project.json"),
        "Racing destination must survive unchanged");
    check(std::distance(std::filesystem::directory_iterator(root), {}) ==
              before_collision + 1,
          "Failed publication must clean its released descendants by identity");

    // Foreign additions are rejected even without an injected write fault.
    sketch::ExtractOptions addition;
    addition.stage_observer = [&](auto stage, const auto &staging) {
      if (stage == sketch::ExtractFaultStage::before_publish)
        std::ofstream(staging / L"foreign-marker.txt") << "keep";
    };
    residual.clear();
    try {
      sketch::extract_project(document.snapshot(), root / L"foreign-addition",
                              addition);
    } catch (const sketch::ExtractionCleanupError &error) {
      residual = error.residual_path();
      check(std::string(error.what()).find("Unexpected content") !=
                std::string::npos,
            "Unexpected staging contents must block publication");
    }
    check(!residual.empty() &&
              std::filesystem::exists(residual / L"foreign-marker.txt") &&
              !std::filesystem::exists(root / L"foreign-addition"),
          "Foreign additions must remain unpublished and preserved");
    check(residual.parent_path() == std::filesystem::canonical(root),
          "Foreign-marker fixture cleanup must stay inside the unique test "
          "directory");
    std::filesystem::remove_all(residual);
    // Only this test's unique, resolved temporary tree is removed.
    test_translation_export(root);
    check(std::filesystem::equivalent(
              std::filesystem::canonical(root).parent_path(),
              std::filesystem::temp_directory_path()),
          "Unexpected test cleanup path");
    std::filesystem::remove_all(root);
    std::cout << "Project exchange tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    if (std::filesystem::equivalent(
            std::filesystem::canonical(root).parent_path(),
            std::filesystem::temp_directory_path()))
      std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
