#include "sketch/performance_workload.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <set>

namespace sketch {
namespace {
constexpr double mesh_deflection_m = 0.001;
constexpr double mesh_angle_rad = 0.1;
std::string identity(const char* prefix, std::size_t index) {
    return std::string(prefix) + std::to_string(index);
}
Asset image_asset(std::size_t index, std::size_t size) {
    // Valid P6 image, with deterministic padding in a legal PPM comment. The
    // payload is real RGB pixels; both asset bytes and file bytes are reported.
    const std::size_t width = size >= 100000 ? 1024 : 16;
    const std::size_t height = (size - 64) / (3 * width);
    const std::string suffix = "\n" + std::to_string(width) + " " +
        std::to_string(height) + "\n255\n";
    const std::size_t pixels = width * height * 3;
    const std::string header = "P6\n#" + std::string(size - pixels - suffix.size() - 4, ' ') + suffix;
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < header.size(); ++i)
        bytes[i] = static_cast<std::byte>(header[i]);
    std::uint32_t state = static_cast<std::uint32_t>(index + 1);
    for (std::size_t i = header.size(); i < size; ++i) {
        state = state * 1664525U + 1013904223U;
        bytes[i] = static_cast<std::byte>(state >> 24);
    }
    return Asset::create(identity("reference-", index), "image/x-portable-pixmap",
        std::move(bytes), {{"sheet_id", identity("sheet-", index)},
        {"generator", "vertex-representative-workload-v1"}, {"width", width}, {"height", height}});
}
}

Document make_performance_workload(const PerformanceWorkloadOptions& options) {
    const auto limit = options.kind == PerformanceWorkloadKind::drawing ? 50000U :
        options.kind == PerformanceWorkloadKind::architecture ? 10000U : 20U;
    if (options.count == 0 || options.count > limit)
        throw std::invalid_argument("workload count outside bounded fixture range");
    if (options.kind != PerformanceWorkloadKind::drawing &&
        options.kind != PerformanceWorkloadKind::architecture &&
        options.kind != PerformanceWorkloadKind::sheets)
        throw std::invalid_argument("unknown workload kind");
    std::vector<Entity> entities;
    std::vector<Asset> assets;
    if (options.kind == PerformanceWorkloadKind::sheets) {
        if (options.asset_bytes_per_sheet < 1024 ||
            options.asset_bytes_per_sheet > 250000000ULL / options.count)
            throw std::invalid_argument("sheet assets must be 1 KiB minimum and 250 MB total maximum");
        std::vector<DrawingSheet> sheets;
        std::vector<CoordinatedView> views;
        for (std::size_t i = 0; i < options.count; ++i) {
            auto asset = image_asset(i, options.asset_bytes_per_sheet);
            const auto reference_id = identity("sheet-reference-", i);
            // Use the same persisted reference properties consumed by the
            // desktop sheet renderer. Each viewport selects its own image.
            const double pixel_scale_m = std::min(
                36.0 / asset.metadata.at("width").get<double>(),
                23.0 / asset.metadata.at("height").get<double>());
            auto reference = Entity::create("reference_asset",
                {{"asset_id", asset.id}, {"render_asset_id", asset.id},
                 {"source_path", asset.id + ".ppm"}, {"mime_type", asset.media_type},
                 {"position_m", {0.0, 0.0}}, {"metres_per_source_unit", pixel_scale_m},
                 {"scale", 1.0}, {"rotation_degrees", 0.0},
                 {"flip_horizontal", false}, {"flip_vertical", false},
                 {"intensity", 1.0}, {"visible", true}});
            reference.id = reference_id;
            entities.push_back(std::move(reference));
            CoordinatedView plan{identity("fixture-plan-", i), "Reference plan"};
            plan.object_ids.push_back(reference_id);
            DrawingSheet sheet;
            sheet.id = identity("sheet-", i);
            sheet.number = identity("A", i + 100);
            sheet.title_block = {"Vertex representative workload", sheet.number,
                                "Deterministic fixture generator", "2000-01-01"};
            sheet.viewports.push_back({identity("viewport-", i), plan.id,
                                       {10, 10, 380, 250}, 100});
            sheets.push_back(std::move(sheet));
            assets.push_back(std::move(asset));
            views.push_back(std::move(plan));
        }
        entities.push_back(make_sheet_view_entity("fixture-sheets",
            SheetViewModel::create(std::move(views), std::move(sheets))));
    } else {
        entities.reserve(options.count);
        for (std::size_t i = 0; i < options.count; ++i) {
            const double x = static_cast<double>(i % 250) * 5;
            const double y = static_cast<double>(i / 250) * 5;
            if (options.kind == PerformanceWorkloadKind::architecture) {
                entities.push_back(encode_building_entity(CircularColumn{
                    identity("column-", i), {x, y, 0}, 0.25 + (i % 5) * 0.025, 3.0}));
            } else {
                const auto id = identity("drawing-", i);
                IdentifiedBoundary boundary{id, "measurement_boundary", {}};
                // Three identified edges keep the 50k fixture below the real
                // ProjectStore aggregate JSON-value limit without weakening it.
                const Vec2 points[] = {{x, y}, {x + 4, y}, {x, y + 3}};
                for (std::size_t j = 0; j < 3; ++j) {
                    boundary.segments.push_back({id + identity("-segment-", j),
                        id + identity("-vertex-", j), id + identity("-vertex-", (j + 1) % 3),
                        {points[j], points[(j + 1) % 3], 0}});
                }
                entities.push_back(encode_identified_boundary_entity(boundary));
            }
        }
    }
    return Document::create(std::move(entities), std::move(assets));
}

nlohmann::json inspect_performance_workload(const DocumentSnapshot& snapshot) {
    std::uint64_t drawings = 0, objects = 0, triangles = 0, sheets = 0, asset_bytes = 0;
    std::uint64_t references = 0, placements = 0, placed_asset_bytes = 0;
    std::set<std::string> placed_assets;
    for (const auto& [id, entity] : snapshot.entities()) {
        if (can_recognize_boundary_entity_type(entity.type)) {
            (void)decode_identified_boundary_entity(entity);
            ++drawings;
        } else if (can_recognize_building_entity_type(entity.type)) {
            auto shape = make_building_shape(decode_building_entity(entity));
            BRepMesh_IncrementalMesh mesh(shape, mesh_deflection_m, false, mesh_angle_rad, false);
            if (!mesh.IsDone()) throw std::runtime_error("fixture tessellation failed");
            ++objects;
            for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
                TopLoc_Location location;
                const auto mesh_face = BRep_Tool::Triangulation(TopoDS::Face(faces.Current()), location);
                if (mesh_face.IsNull()) throw std::runtime_error("fixture face lacks tessellation");
                triangles += static_cast<std::uint64_t>(mesh_face->NbTriangles());
            }
        } else if (entity.type == kSheetViewEntityType) {
            const auto model = decode_sheet_view_entity(entity);
            sheets += model.sheets().size();
            for (const auto& sheet : model.sheets()) {
                for (const auto& viewport : sheet.viewports) {
                    const auto view = std::find_if(model.views().begin(), model.views().end(),
                        [&](const auto& candidate) { return candidate.id == viewport.view_id; });
                    if (view == model.views().end() || view->object_ids.empty())
                        throw std::runtime_error("fixture viewport lacks explicit reference selection");
                    for (const auto& reference_id : view->object_ids) {
                        const auto& reference = snapshot.entities().at(reference_id);
                        if (reference.type != "reference_asset" ||
                            !reference.properties.at("visible").get<bool>())
                            throw std::runtime_error("fixture viewport reference is not visible");
                        const auto asset_id = reference.properties.at("asset_id").get<std::string>();
                        if (reference.properties.at("render_asset_id") != asset_id)
                            throw std::runtime_error("fixture rendering does not use measured reference asset");
                        const auto& asset = snapshot.assets().at(asset_id);
                        ++placements;
                        if (placed_assets.insert(asset_id).second) placed_asset_bytes += asset.bytes.size();
                    }
                }
            }
        } else if (entity.type == "reference_asset") {
            ++references;
        } else {
            throw std::invalid_argument("unexpected entity in representative fixture");
        }
    }
    nlohmann::json manifest = nlohmann::json::array();
    for (const auto& [id, asset] : snapshot.assets()) {
        const auto actual = sha256_hex(asset.bytes);
        if (actual != asset.sha256) throw std::runtime_error("fixture asset hash mismatch");
        asset_bytes += asset.bytes.size();
        manifest.push_back({{"id", id}, {"bytes", asset.bytes.size()}, {"sha256", actual},
                            {"media_type", asset.media_type}, {"metadata", asset.metadata}});
    }
    const nlohmann::json semantic = {{"entity_map_sha256", entity_map_digest(snapshot.entities())},
                                    {"assets", manifest}};
    const auto encoded = semantic.dump();
    const auto bytes = std::as_bytes(std::span(encoded.data(), encoded.size()));
    return {{"entities", snapshot.entities().size()}, {"drawing_entities", drawings},
        {"objects", objects}, {"triangles", triangles}, {"sheets", sheets},
        {"reference_entities", references}, {"sheet_reference_placements", placements},
        {"placed_asset_bytes", placed_asset_bytes},
        {"unplaced_assets", snapshot.assets().size() - placed_assets.size()},
        {"asset_bytes", asset_bytes}, {"assets", manifest}, {"semantic_sha256", sha256_hex(bytes)},
        {"mesh_deflection_m", mesh_deflection_m}, {"mesh_angle_rad", mesh_angle_rad}};
}
}
