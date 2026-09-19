#include "sketch/performance_workload.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include <QImage>
#include <algorithm>
#include "support/noninteractive_errors.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("vertex-workload-test-" + sketch::make_stable_id());
    Scratch() { std::filesystem::create_directory(path); }
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void verify_sheet_references(const sketch::DocumentSnapshot& snapshot) {
    const auto model = sketch::decode_sheet_view_entity(snapshot.entities().at("fixture-sheets"));
    for (const auto& sheet : model.sheets()) {
        require(sheet.viewports.size() == 1, "each sheet must place a reference viewport");
        const auto& viewport = sheet.viewports.front();
        const auto view = std::find_if(model.views().begin(), model.views().end(),
            [&](const auto& candidate) { return candidate.id == viewport.view_id; });
        require(view != model.views().end() && view->object_ids.size() == 1,
                "each sheet must explicitly select its own reference");
        const auto& reference = snapshot.entities().at(view->object_ids.front());
        require(reference.type == "reference_asset" && reference.properties.at("visible") == true,
                "sheet must select a visible renderable reference entity");
        const auto& asset = snapshot.assets().at(reference.properties.at("asset_id").get<std::string>());
        require(reference.properties.at("render_asset_id") == asset.id,
                "sheet renderer must use the measured asset bytes");
        const auto decoded = QImage::fromData(reinterpret_cast<const uchar*>(asset.bytes.data()),
                                              static_cast<int>(asset.bytes.size()));
        require(!decoded.isNull(), "real Qt image decoder must accept sheet reference bytes");
        const double paper_mm_per_pixel = reference.properties.at("metres_per_source_unit").get<double>() *
            reference.properties.at("scale").get<double>() * 1000.0 / viewport.scale_denominator;
        require(decoded.width() * paper_mm_per_pixel <= viewport.bounds.width_mm &&
                decoded.height() * paper_mm_per_pixel <= viewport.bounds.height_mm,
                "reference image must fit on its sheet viewport at persisted scale");
    }
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        Scratch scratch;
        for (auto kind : {sketch::PerformanceWorkloadKind::drawing,
                          sketch::PerformanceWorkloadKind::architecture,
                          sketch::PerformanceWorkloadKind::sheets}) {
            const sketch::PerformanceWorkloadOptions options{kind, 3, 1024};
            auto first = sketch::make_performance_workload(options);
            auto second = sketch::make_performance_workload(options);
            const auto before = first.snapshot();
            const auto facts = sketch::inspect_performance_workload(before);
            require(facts == sketch::inspect_performance_workload(second.snapshot()),
                    "semantic fixture generation must be deterministic across document IDs");
            require(facts.at("entities") == 3 || kind == sketch::PerformanceWorkloadKind::sheets,
                    "drawing and architectural counts must come from decoded entities");
            if (kind == sketch::PerformanceWorkloadKind::architecture) {
                require(facts.at("objects") == 3 && facts.at("triangles").get<unsigned>() > 0,
                        "architecture must contain decoded objects and real tessellation");
            }
            if (kind == sketch::PerformanceWorkloadKind::sheets) {
                require(facts.at("entities") == 4 && facts.at("sheets") == 3,
                        "sheet graph container must not be counted as one sheet");
                require(facts.at("sheet_reference_placements") == 3 &&
                        facts.at("placed_asset_bytes") == 3072 && facts.at("unplaced_assets") == 0,
                        "all measured assets must be placed in rendered sheet views");
                verify_sheet_references(before);
                auto unlinked_model = sketch::decode_sheet_view_entity(before.entities().at("fixture-sheets"));
                auto unlinked_view = unlinked_model.views().front();
                unlinked_view.object_ids.clear();
                unlinked_model = unlinked_model.with_view(unlinked_view);
                std::vector<sketch::Entity> unlinked_entities;
                std::vector<sketch::Asset> retained_assets;
                for (const auto& [id, entity] : before.entities())
                    unlinked_entities.push_back(id == "fixture-sheets"
                        ? sketch::make_sheet_view_entity(id, unlinked_model) : entity);
                for (const auto& [id, asset] : before.assets()) retained_assets.push_back(asset);
                const auto unlinked = sketch::Document::create(unlinked_entities, retained_assets);
                bool missing_selection_rejected = false;
                try { (void)sketch::inspect_performance_workload(unlinked.snapshot()); }
                catch (const std::runtime_error&) { missing_selection_rejected = true; }
                require(missing_selection_rejected,
                        "retained asset bytes cannot substitute for an explicit sheet placement");
                require(facts.at("asset_bytes") == 3072 && facts.at("assets").size() == 3,
                        "asset manifest must contain actual byte sizes and hashes");
                for (const auto& [id, asset] : before.assets()) {
                    const std::string content(reinterpret_cast<const char*>(asset.bytes.data()),
                                              asset.bytes.size());
                    std::istringstream input(content);
                    std::string magic, comment;
                    std::getline(input, magic);
                    std::getline(input, comment);
                    std::size_t width = 0, height = 0, maximum = 0;
                    input >> width >> height >> maximum;
                    input.get();
                    require(magic == "P6" && comment.starts_with('#') && width > 0 &&
                            height > 0 && maximum == 255 &&
                            static_cast<std::size_t>(input.tellg()) + width * height * 3 == asset.bytes.size(),
                            "fixture reference assets must be valid complete RGB PPM images");
                }
                auto damaged_assets = before.assets();
                damaged_assets.begin()->second.bytes[0] = std::byte{0};
                std::vector<sketch::Asset> assets;
                for (const auto& [id, asset] : damaged_assets) assets.push_back(asset);
                std::vector<sketch::Entity> entities;
                for (const auto& [id, entity] : before.entities()) entities.push_back(entity);
                bool rejected = false;
                try { (void)sketch::Document::create(entities, assets); }
                catch (const sketch::DocumentError&) { rejected = true; }
                require(rejected, "asset mutation without hash update must be rejected");
            }
            const auto path = scratch.path / (std::to_string(static_cast<int>(kind)) + ".bldproj");
            const auto receipt = sketch::ProjectStore::save(path, before);
            auto reopened = sketch::ProjectStore::load(path);
            require(receipt.file_sha256 == reopened.file_sha256, "saved file fingerprint mismatch");
            require(sketch::document_authoring_source_digest_v1(before) ==
                    sketch::document_authoring_source_digest_v1(reopened.document.snapshot()),
                    "save/open must retain complete authoring snapshot including identity and history");
            require(facts == sketch::inspect_performance_workload(reopened.document.snapshot()),
                    "save/open changed decoded counts, semantic hash or asset manifest");
            const auto second_path = std::filesystem::path(path.string() + ".roundtrip.bldproj");
            (void)sketch::ProjectStore::save(second_path, reopened.document.snapshot());
            const auto second_reopen = sketch::ProjectStore::load(second_path).document.snapshot();
            require(before.assets() == reopened.document.snapshot().assets() &&
                    before.assets() == second_reopen.assets(),
                    "actual asset bytes and metadata must survive both archive roundtrips");
            if (kind == sketch::PerformanceWorkloadKind::sheets) {
                verify_sheet_references(reopened.document.snapshot());
                verify_sheet_references(second_reopen);
            }
            require(sketch::document_authoring_source_digest_v1(before) ==
                    sketch::document_authoring_source_digest_v1(second_reopen) &&
                    facts == sketch::inspect_performance_workload(second_reopen),
                    "second save/open must retain authoring snapshot and semantic manifest");
            bool refused = false;
            try { (void)sketch::ProjectStore::save(path, second.snapshot()); }
            catch (const sketch::StorageError& error) {
                refused = error.code() == sketch::StorageErrorCode::destination_exists;
            }
            require(refused && sketch::ProjectStore::file_sha256(path) == receipt.file_sha256,
                    "existing project must remain unchanged without explicit CAS authority");
        }
        for (const auto size : {std::size_t{0}, std::size_t{12500001}}) {
            bool rejected = false;
            try { (void)sketch::make_performance_workload(
                {sketch::PerformanceWorkloadKind::sheets, 20, size}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "invalid asset payload bounds must fail before allocating");
        }
        // Exercise both image-width branches and non-aligned payload sizes,
        // including one production-sized reference without allocating 20 sheets.
        for (const auto size : {std::size_t{1025}, std::size_t{99999},
                               std::size_t{100000}, std::size_t{12500000}}) {
            const auto image_document = sketch::make_performance_workload(
                {sketch::PerformanceWorkloadKind::sheets, 1, size});
            verify_sheet_references(image_document.snapshot());
            require(sketch::inspect_performance_workload(image_document.snapshot()).at("placed_asset_bytes") == size,
                    "all requested image bytes must belong to the placed reference");
        }
        for (const auto count : {std::size_t{0}, std::size_t{50001}}) {
            bool rejected = false;
            try { (void)sketch::make_performance_workload(
                {sketch::PerformanceWorkloadKind::drawing, count, 0}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "invalid workload bounds must fail before allocating");
        }
        std::cout << "performance workload tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
