#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/dxf_exchange.hpp"
#include "sketch/dxf_project_exchange.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string wrapped_entities(std::string entities) {
    return "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1027\n9\n$INSUNITS\n70\n6\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n" +
        entities + "0\nENDSEC\n0\nEOF\n";
}

sketch::Document make_document() {
    using namespace sketch;
    const IdentifiedBoundary model{
        "boundary-1", "boundary",
        {{"edge-1", "vertex-1", "vertex-2", {{0, 0}, {4, 0}, 0}},
         {"edge-2", "vertex-2", "vertex-3", {{4, 0}, {4, 3}, 0}},
         {"edge-3", "vertex-3", "vertex-4", {{4, 3}, {0, 3}, 0}},
         {"edge-4", "vertex-4", "vertex-1", {{0, 3}, {0, 0}, 0}}}};
    auto boundary = encode_identified_boundary_entity(model);
    auto dimension = encode_boundary_dimension_entity(
        BoundaryDimension{"dimension-1", "boundary-1", "edge-1", {2, 1.0},
                          BoundaryDimensionPlacement::manual, std::nullopt, std::nullopt});
    Entity wall{"wall-1", "wall",
                {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.0}}},
                 {"thickness_m", 0.2}, {"height_m", 2.5}, {"elevation_m", 0.0}},
                false, nlohmann::json::object()};
    Entity slab{"slab-1", "slab",
                {{"boundary", {{{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.0}},
                                {{"start", {4, 0}}, {"end", {4, 3}}, {"sweep_radians", 0.0}},
                                {{"start", {4, 3}}, {"end", {0, 3}}, {"sweep_radians", 0.0}},
                                {{"start", {0, 3}}, {"end", {0, 0}}, {"sweep_radians", 0.0}}}},
                 {"holes", nlohmann::json::array()}, {"thickness_m", 0.15},
                 {"elevation_m", 0.0}, {"element_kind", "floor"}},
                false, nlohmann::json::object() };
    AnnotationState annotations;
    auto label = instantiate_label(default_label_templates().front(), "label-1");
    label.content = "Kitchen";
    label.placement.position = {1, 1};
    annotations.labels.push_back(label);
    return Document::create({std::move(boundary), std::move(dimension), std::move(wall),
                             std::move(slab), make_annotation_entity("annotations-1", annotations)});
}

void run() {
    using namespace sketch;
    auto document = make_document();
    const auto exported = export_project_dxf(document.snapshot());
    check(exported.drawing.insertion_units == 6, "project units must be SI metres");
    check(exported.drawing.polylines.size() >= 2, "boundary and slab geometry must export");
    check(exported.drawing.lines.size() >= 1, "wall geometry must export");
    check(exported.drawing.labels.size() == 1, "annotation label must export");
    check(exported.drawing.dimensions.size() == 1, "identified dimension must export");

    const auto bytes = export_dxf_ascii(exported.drawing);
    const auto imported = import_project_dxf(bytes);
    check(imported.entities.size() >= 5, "DXF geometry must reconstruct native candidates");
    check(std::any_of(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.type == "annotation_state";
    }), "DXF labels must reconstruct a native annotation entity");
    check(std::any_of(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.type == "boundary" && entity.properties.value("classification", "") ==
               "dxf_polyline_closed";
    }), "closed polyline must reconstruct a closed native boundary candidate");
    check(std::any_of(imported.diagnostics.begin(), imported.diagnostics.end(), [](const auto& item) {
        return item.source_kind == "DIMENSION" && item.code == "dimension_associativity_unbound";
    }), "unbound imported dimensions must be explicit");
    check(imported.source_retention_required, "diagnostics require source retention");

    DxfDrawing blocks;
    blocks.insertion_units = 6;
    blocks.blocks.push_back({"Door", {0, 0}, {{{0, 0}, {1, 0}, "Door"}}, {}, {}, {}});
    blocks.inserts.push_back({"Door", {10, 2}, 1, 1, 90, "Inserted"});
    const auto inserted = import_project_dxf(export_dxf_ascii(blocks));
    check(std::any_of(inserted.entities.begin(), inserted.entities.end(), [](const auto& entity) {
        return entity.properties.value("classification", "") == "dxf_insert_line";
    }), "block inserts must reconstruct transformed native geometry");

    const auto unsupported = import_project_dxf(wrapped_entities(
        "0\nCIRCLE\n10\n0\n20\n0\n40\n2\n"));
    check(!unsupported.diagnostics.empty() && unsupported.source_retention_required,
          "unsupported DXF entities must produce retention diagnostics");
    bool rejected = false;
    try { (void)import_project_dxf("0\nSECTION\n2\nENTITIES\n0\nENDSEC\n"); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "malformed DXF must fail closed");
}

} // namespace

int main() {
    try {
        run();
        std::cout << "DXF project exchange tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
