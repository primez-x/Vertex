#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/boundary_edit.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "support/noninteractive_errors.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

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

  sketch::BoundaryGeometryEdit edit;
  edit.boundary_id = "boundary";
  edit.kind = sketch::BoundaryGeometryEditKind::move_vertex;
  edit.target_id = "vertex-1";
  const auto current = sketch::decode_identified_boundary_entity(
      document.snapshot().entities().at("boundary"));
  edit.target_position = current.segments[1].segment.start;
  edit.target_position.x += 0.25;
  document.apply(sketch::EditBoundaryGeometry{document.revision(), edit});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "geometry-edit");
  std::ifstream edit_input(root / "geometry-edit" / "project.json");
  const auto edit_json = nlohmann::json::parse(edit_input);
  check(edit_json.at("exchange_version") == 4,
        "boundary edit export must advertise version 4");
  const auto& edit_rows = edit_json.at("revisions");
  check(edit_rows[7].at("boundary_geometry_edit") ==
            sketch::encode_boundary_geometry_edit(edit) &&
        !edit_rows[8].contains("boundary_geometry_edit"),
        "exchange must preserve an undone boundary edit proof on only its command revision");
  sketch::ApplyBoundaryConstraintChanges transaction{document.revision(), {edit}, {}, "constrained edit"};
  document.apply(transaction);
  document.apply(sketch::EditBoundaryGeometry{document.revision(), edit});
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "constraint-edit");
  std::ifstream constraint_input(root / "constraint-edit" / "project.json");
  const auto constraint_json = nlohmann::json::parse(constraint_input);
  check(constraint_json.at("exchange_version") == 5,
        "later geometry edit must not downgrade constraint exchange version");
  check(constraint_json.at("revisions")[9].at("boundary_constraint_changes") ==
            sketch::command_to_json(transaction) &&
        !constraint_json.at("revisions")[10].contains("boundary_constraint_changes"),
        "exchange must preserve exact typed constraint proof only on its command row");

  sketch::Entity label{"group-label", "label", {{"text", "moved with boundary"}},
                       false, {{"vendor", "retain"}}};
  sketch::TranslateBoundaries batch{document.revision(), {{"boundary", {4, -2}}},
      {sketch::EntityChange::upsert(label)}, "Move measured group"};
  const auto batch_revision = document.apply(batch);
  const auto moved_boundary = sketch::decode_identified_boundary_entity(
      document.snapshot().entities().at("boundary"));
  sketch::BoundaryGeometryEdit later_edit;
  later_edit.boundary_id = "boundary";
  later_edit.kind = sketch::BoundaryGeometryEditKind::move_vertex;
  later_edit.target_id = "vertex-3";
  later_edit.target_position = moved_boundary.segments[3].segment.start;
  later_edit.target_position.x -= 0.1;
  sketch::ApplyBoundaryConstraintChanges later_constraint{
      document.revision(), {later_edit}, {}, "Later constrained edit"};
  const auto constraint_revision = document.apply(later_constraint);
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "translation-group");
  std::ifstream batch_input(root / "translation-group" / "project.json");
  const auto batch_json = nlohmann::json::parse(batch_input);
  check(batch_json.at("exchange_version") == 6,
        "later constraint row must not downgrade translation group exchange version");
  check(batch_json.at("revisions")[batch_revision].at("boundary_translations") == sketch::command_to_json(batch) &&
        batch_json.at("revisions")[constraint_revision].at("boundary_constraint_changes") == sketch::command_to_json(later_constraint) &&
        !batch_json.at("revisions").back().contains("boundary_translations"),
        "exchange must retain exact mixed proofs only on their command rows");
}

void test_curved_constraint_export_floor(const std::filesystem::path& root) {
  sketch::Entity wall{"curve-wall", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.4}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}}, false,
      nlohmann::json::object()};
  auto source = wall;
  const auto source_baseline = sketch::arc_from_chord_arc_length({0, 0}, {4, 0}, 5.0, false);
  source.properties["baseline"]["sweep_radians"] = source_baseline.sweep_radians;
  const auto angle = sketch::angle_from_radians(source_baseline.sweep_radians);
  source.extensions["curve_input"] = {{"version", 2}, {"construction", "arc_length"},
      {"measure", "5 m"}, {"normalized_measure", "5 m"}, {"measure_value", 5.0},
      {"clockwise", false}, {"start", {0, 0}}, {"end", {4, 0}},
      {"sweep", angle.original_expression}, {"normalized_sweep", angle.normalized_expression},
      {"radians", source_baseline.sweep_radians}, {"vendor", "preserve"}};
  const auto derived = sketch::replay_constraint_wall_edit(source,
      {source.id, {{0, 0}, {4.5, 0}, source_baseline.sweep_radians}, std::nullopt, 2});
  const auto imported = sketch::Document::create({derived});
  sketch::extract_project(imported.snapshot(), root / "imported-curve-derivation");
  std::ifstream imported_input(root / "imported-curve-derivation" / "project.json");
  const auto imported_json = nlohmann::json::parse(imported_input);
  check(imported_json.at("exchange_version") == 7 &&
        !imported_json.at("revisions")[0].contains("boundary_constraint_changes"),
        "imported curve derivation must advertise exchange7 without originating command history");
  const auto collision = sketch::Document::create({{"vendor-label", "label", {{"text", "opaque"}},
      false, {{"curve_input_derivation", derived.extensions.at("curve_input_derivation")}}}});
  sketch::extract_project(collision.snapshot(), root / "opaque-curve-derivation-collision");
  std::ifstream collision_input(root / "opaque-curve-derivation-collision" / "project.json");
  check(nlohmann::json::parse(collision_input).at("exchange_version") == 1,
        "generic vendor curve derivation extension must retain its historical exchange floor");
  sketch::PersistentConstraint relation;
  relation.id = "curve-horizontal";
  relation.relation = sketch::ConstraintRelationKind::horizontal;
  relation.bindings = {{wall.id, sketch::WallEndpointRole::start},
                       {wall.id, sketch::WallEndpointRole::end}};
  auto state = sketch::Document::create({wall, sketch::encode_constraint_entity(relation)});
  sketch::extract_project(state.snapshot(), root / "curve-state");
  std::ifstream state_input(root / "curve-state" / "project.json");
  const auto state_json = nlohmann::json::parse(state_input);
  check(state_json.at("exchange_version") == 7 &&
        !state_json.at("revisions")[0].contains("boundary_constraint_changes"),
        "already satisfied curved relation must advertise exchange7 without a wall edit proof");
  auto undone = sketch::Document::create({wall});
  undone.apply(sketch::ApplyEntityChanges{0,
      {sketch::EntityChange::upsert(sketch::encode_constraint_entity(relation))}, {}, "curve relation"});
  undone.undo(undone.revision());
  sketch::extract_project(undone.snapshot(), root / "curve-state-undone");
  std::ifstream undone_input(root / "curve-state-undone" / "project.json");
  const auto undone_json = nlohmann::json::parse(undone_input);
  check(undone_json.at("exchange_version") == 7 && undone_json.at("revisions").size() == 3,
        "undone curve-bound relation must retain exchange7 and all states");

  auto proof = sketch::Document::create({wall});
  sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "curved wall chord"};
  command.wall_edits.push_back({wall.id, {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
  proof.apply(command);
  sketch::extract_project(proof.snapshot(), root / "curve-proof-head");
  std::ifstream head_input(root / "curve-proof-head" / "project.json");
  check(nlohmann::json::parse(head_input).at("exchange_version") == 7,
        "curved wall proof at head must advertise exchange7 without relations");
  // Later ordinary rows and undo must not lower the proof's retained reader floor.
  proof.apply(sketch::ApplyEntityChanges{proof.revision(),
      {sketch::EntityChange::upsert({"label", "label", {{"text", "later"}}, false,
                                  nlohmann::json::object()})}, {}, "later row"});
  proof.undo(proof.revision());
  proof.undo(proof.revision());
  sketch::extract_project(proof.snapshot(), root / "curve-proof-undone");
  std::ifstream proof_input(root / "curve-proof-undone" / "project.json");
  const auto proof_json = nlohmann::json::parse(proof_input);
  check(proof_json.at("exchange_version") == 7,
        "retained undone curved wall proof must advertise exchange7");
  const auto& rows = proof_json.at("revisions");
  check(rows[1].at("boundary_constraint_changes").at("version") == 3 &&
        rows[1].at("boundary_constraint_changes").at("wall_edits")[0].at("version") == 2 &&
        !rows.back().contains("boundary_constraint_changes"),
        "exchange must retain curved proof versions only on their command row");

  sketch::IdentifiedBoundary boundary{"curved-boundary", "measurement_boundary", {
      {"edge-0", "vertex-0", "vertex-1", {{0, 0}, {4, 0}, 0.2}},
      {"edge-1", "vertex-1", "vertex-2", {{4, 0}, {4, 3}, 0}},
      {"edge-2", "vertex-2", "vertex-3", {{4, 3}, {0, 3}, 0}},
      {"edge-3", "vertex-3", "vertex-0", {{0, 3}, {0, 0}, 0}}}};
  relation.relation = sketch::ConstraintRelationKind::vertical;
  relation.bindings = {{boundary.id, sketch::WallEndpointRole::start, "edge-1", "vertex-1"},
                       {boundary.id, sketch::WallEndpointRole::end, "edge-1", "vertex-2"}};
  auto straight = sketch::Document::create({sketch::encode_identified_boundary_entity(boundary),
                                           sketch::encode_constraint_entity(relation)});
  sketch::extract_project(straight.snapshot(), root / "straight-edge-curved-owner");
  std::ifstream straight_input(root / "straight-edge-curved-owner" / "project.json");
  check(nlohmann::json::parse(straight_input).at("exchange_version") == 1,
        "straight edge binding on curved boundary must retain historical exchange floor");
  relation.relation = sketch::ConstraintRelationKind::horizontal;
  relation.bindings = {{boundary.id, sketch::WallEndpointRole::start, "edge-0", "vertex-0"},
                       {boundary.id, sketch::WallEndpointRole::end, "edge-0", "vertex-1"}};
  auto curve = sketch::Document::create({sketch::encode_identified_boundary_entity(boundary),
                                        sketch::encode_constraint_entity(relation)});
  sketch::extract_project(curve.snapshot(), root / "curved-edge-relation");
  std::ifstream curve_input(root / "curved-edge-relation" / "project.json");
  check(nlohmann::json::parse(curve_input).at("exchange_version") == 7,
        "bound curved boundary edge must advertise exchange7 without a wall edit proof");
}

void test_safe_curved_wall_history_exchange_replays(const std::filesystem::path& root) {
  sketch::Entity source{"curve-wall", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.4}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}}, false,
      nlohmann::json::object()};
  auto obstacle = source;
  obstacle.id = "crossing-obstacle";
  obstacle.properties["baseline"] = {{"start", {6, -10}}, {"end", {6, 10}}, {"sweep_radians", 0}};
  auto document = sketch::Document::create({source, obstacle});
  const auto initial = document.snapshot().entities();
  sketch::ApplyBoundaryConstraintChanges command{0, {}, {}, "safe curved endpoint edit"};
  command.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0.4}, std::nullopt, 2});
  document.apply(command);
  const auto edited = document.snapshot().entities();
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "safe-curved-wall-history");
  std::ifstream input(root / "safe-curved-wall-history" / "project.json");
  const auto exchanged = nlohmann::json::parse(input);
  const auto& rows = exchanged.at("revisions");
  check(exchanged.at("exchange_version") == 7 && rows.size() == 3,
        "valid undone curved wall history must preserve its reader floor and all revisions");
  std::vector<sketch::Entity> source_entities;
  for (const auto& value : rows[0].at("entities"))
    source_entities.push_back({value.at("id").get<std::string>(), value.at("type").get<std::string>(),
        value.at("properties"), value.at("required").get<bool>(), value.at("extensions")});
  auto replayed = sketch::Document::create(std::move(source_entities));
  check(replayed.snapshot().entities() == initial, "exchange must reconstruct the exact source wall state");
  replayed.apply(sketch::command_from_json(rows[1].at("boundary_constraint_changes")));
  check(replayed.snapshot().entities() == edited,
        "exchanged curved command must replay exact safe geometry after topology admission");
  replayed.undo(replayed.revision());
  check(replayed.snapshot().entities() == initial,
        "exchanged safe curved command must undo exactly");
  replayed.redo(replayed.revision());
  check(replayed.snapshot().entities() == edited,
        "exchanged safe curved command must redo exactly");

  auto forged_exchange = exchanged;
  auto& forged_rows = forged_exchange.at("revisions");
  auto& forged_proof = forged_rows[1].at("boundary_constraint_changes");
  forged_proof["wall_edits"][0]["baseline"]["end"][0] = 7.0;
  for (auto& value : forged_rows[1].at("entities"))
    if (value.at("id") == source.id) value["properties"]["baseline"]["end"][0] = 7.0;
  const auto forged_command = sketch::command_from_json(forged_proof);
  const auto& typed = std::get<sketch::ApplyBoundaryConstraintChanges>(forged_command);
  const auto forged_wall = sketch::replay_constraint_wall_edit(source, typed.wall_edits.front());
  std::size_t matched_walls = 0;
  for (const auto& value : forged_rows[1].at("entities"))
    if (value.at("id") == source.id) {
      ++matched_walls;
      check(value.at("properties") == forged_wall.properties && value.at("extensions") == forged_wall.extensions,
            "forged exchanged proof and retained wall result must agree exactly");
    }
  check(matched_walls == 1, "forged exchanged result must contain exactly one matching wall");
  // Exchange has no history importer: exercise its decoded typed command at
  // the same document admission boundary used by history restoration.
  auto refused = sketch::Document::create({source, obstacle});
  const auto before_refusal = refused.snapshot();
  const auto source_path = root / "safe-curved-wall-history" / "project.json";
  const auto source_hash = sketch::ProjectStore::file_sha256(source_path);
  bool rejected = false;
  try {
    refused.apply(forged_command);
  } catch (const sketch::DocumentError& error) {
    rejected = std::string_view(error.what()).find("topology") != std::string_view::npos;
  }
  check(rejected && refused.snapshot().entities() == before_refusal.entities() &&
        refused.snapshot().revision() == before_refusal.revision() &&
        refused.snapshot().history().size() == before_refusal.history().size(),
        "matching forged exchanged proof/result must refuse new crossing atomically");
  check(sketch::ProjectStore::file_sha256(source_path) == source_hash,
        "refusing forged exchanged command must preserve exact source artifact bytes");
}

void test_straight_wall_only_authoring_exchange_v8(const std::filesystem::path& root) {
  sketch::Entity source{"straight-wall", "wall",
      {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0}}},
       {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0}}, false,
      nlohmann::json::object()};
  auto document = sketch::Document::create({source});
  const auto initial = document.snapshot().entities();
  sketch::ApplyBoundaryConstraintChanges intent{document.revision(), {}, {}, "Resize straight wall"};
  intent.wall_edits.push_back({source.id, {{0, 0}, {5, 0}, 0}, sketch::parse_quantity("5 m")});
  document.apply(intent);
  const auto edited = document.snapshot().entities();
  document.undo(document.revision());
  sketch::extract_project(document.snapshot(), root / "straight-wall-only-v8");
  std::ifstream input(root / "straight-wall-only-v8" / "project.json");
  const auto exchanged = nlohmann::json::parse(input);
  const auto& rows = exchanged.at("revisions");
  check(exchanged.at("exchange_version") == 8 && rows[1].at("boundary_constraint_changes").at("version") == 4 &&
        !rows[2].contains("boundary_constraint_changes"),
        "undone straight wall-only authoring must retain exchange8 and originating version4 proof");
  auto replayed = sketch::Document::create({source});
  replayed.apply(sketch::command_from_json(rows[1].at("boundary_constraint_changes")));
  check(replayed.snapshot().entities() == edited, "exchanged version4 must replay exact straight geometry and receipt");
  replayed.undo(replayed.revision());
  check(replayed.snapshot().entities() == initial, "exchanged version4 must undo to exact source");
  replayed.redo(replayed.revision());
  check(replayed.snapshot().entities() == edited, "exchanged version4 must redo exact guarded endpoint intent");
  auto curve = source;
  curve.id = "later-curve";
  curve.properties["baseline"] = {{"start", {10, 0}}, {"end", {14, 0}}, {"sweep_radians", 0.4}};
  document.apply(sketch::ApplyEntityChanges{document.revision(),
      {sketch::EntityChange::upsert(curve)}, {}, "Add later curve"});
  sketch::ApplyBoundaryConstraintChanges curve_edit{document.revision(), {}, {}, "Edit later curve"};
  curve_edit.wall_edits.push_back({curve.id, {{10, 0}, {15, 0}, 0.4}, std::nullopt, 2});
  document.apply(curve_edit);
  sketch::extract_project(document.snapshot(), root / "mixed-straight-curved-v8");
  std::ifstream mixed_input(root / "mixed-straight-curved-v8" / "project.json");
  const auto mixed = nlohmann::json::parse(mixed_input);
  check(mixed.at("exchange_version") == 8 &&
        mixed.at("revisions").back().at("boundary_constraint_changes").at("version") == 3,
        "later curved proof must not lower exchange8 required by retained straight wall-only intent");
}
} // namespace
int main() {
  sketch::testing::noninteractive_errors();
  auto root = std::filesystem::temp_directory_path() /
              ("property-exchange-" + sketch::make_stable_id());
  std::filesystem::create_directory(root);
  try {
    test_straight_wall_only_authoring_exchange_v8(root);
    test_safe_curved_wall_history_exchange_replays(root);
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
    check(json.at("exchange_format") == "vertex-json-assets",
          "New exports must use the Vertex exchange identifier");
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
    const auto directory_names = [&] {
      std::set<std::filesystem::path> names;
      for (const auto& entry : std::filesystem::directory_iterator(root))
        names.insert(entry.path().filename());
      return names;
    };
    const auto retained_entries = directory_names();
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
      check(directory_names() == retained_entries,
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
    test_curved_constraint_export_floor(root);
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
