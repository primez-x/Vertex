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
    Entity opening{"opening-1", "opening",
                   {{"wall_id", "wall-1"}, {"opening_kind", "door"},
                    {"offset_m", 1.0}, {"width_m", 1.0}, {"sill_m", 0.0},
                    {"height_m", 2.0}},
                   false, nlohmann::json::object()};
    AnnotationState annotations;
    auto label = instantiate_label(default_label_templates().front(), "label-1");
    label.content = "Kitchen";
    label.placement.position = {1, 1};
    annotations.labels.push_back(label);
    annotations.symbols.push_back(
        {"symbol-1", "toilet-w3-d3", {{2.0, 1.5}, 0.35, 1.4}, {}, true});
    annotations.symbols.push_back(
        {"symbol-2", "double-bed-w2-d2", {{7.0, 1.5}, -0.2, 0.75}, {}, true});
    annotations.symbols.push_back(
        {"symbol-3", "sofa-w2-d2", {{10.0, 1.5}, 0.6, 1.25}, {}, true});
    annotations.symbols.push_back(
        {"symbol-4", "checkout-counter-w2-d2", {{13.0, 1.5}, 1.1, 0.9}, {}, true});
    return Document::create({std::move(boundary), std::move(dimension), std::move(wall),
                             std::move(opening), std::move(slab),
                             make_annotation_entity("annotations-1", annotations)});
}

void run() {
    using namespace sketch;
    auto document = make_document();
    const auto annotations = decode_annotation_entity(
        document.snapshot().entities().at("annotations-1"));
    const auto exported = export_project_dxf(document.snapshot());
    check(exported.drawing.insertion_units == 6, "project units must be SI metres");
    check(exported.drawing.polylines.size() >= 2, "boundary and slab geometry must export");
    check(exported.drawing.lines.size() >= 1, "wall geometry must export");
    check(std::count_if(exported.drawing.lines.begin(), exported.drawing.lines.end(),
                        [](const auto& line) { return line.layer == "Openings"; }) == 3,
          "hosted openings must export deterministic plan markers");
    check(exported.drawing.labels.size() == 1, "annotation label must export");
    const auto symbol_lines = [&] {
        std::vector<sketch::DxfLine> result;
        for (const auto& line : exported.drawing.lines)
            if (line.layer == "Symbols") result.push_back(line);
        return result;
    }();
    const auto expected_symbol_lines = [&] {
        std::size_t count = 0;
        const auto catalog = default_symbol_catalog();
        for (const auto& instance : annotations.symbols) {
            const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& definition) {
                return definition.id == instance.symbol_id;
            });
            check(found != catalog.end(), "DXF symbol fixture must use a catalog definition");
            count += placed_symbol_preview(*found, instance.placement).size();
        }
        return count;
    }();
    check(symbol_lines.size() == expected_symbol_lines,
          "DXF output must retain every vector stroke for every symbol instance");
    const auto catalog = default_symbol_catalog();
    const auto first_symbol_definition = std::find_if(
        catalog.begin(), catalog.end(),
        [](const auto& definition) { return definition.id == "toilet-w3-d3"; });
    check(first_symbol_definition != catalog.end(),
          "DXF fixture must retain its toilet definition");
    const auto expected_first_stroke = placed_symbol_preview(
        *first_symbol_definition, annotations.symbols.front().placement).front();
    check(std::abs(symbol_lines.front().start.x - expected_first_stroke.start.x) < 1e-12 &&
              std::abs(symbol_lines.front().start.y - expected_first_stroke.start.y) < 1e-12 &&
              std::abs(symbol_lines.front().end.x - expected_first_stroke.end.x) < 1e-12 &&
              std::abs(symbol_lines.front().end.y - expected_first_stroke.end.y) < 1e-12,
          "DXF symbol coordinates must preserve the shared resize and rotation transform");
    check(exported.drawing.dimensions.size() == 1, "identified dimension must export");
    check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "wall-1" && item.code == "wall_3d_semantics_not_representable";
    }), "wall 3D semantics must be explicit in the DXF fidelity report");
    check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "slab-1" && item.code == "slab_3d_semantics_not_representable";
    }), "slab 3D semantics must be explicit in the DXF fidelity report");
    check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "opening-1" &&
               item.code == "opening_host_relationship_not_representable";
    }), "opening host loss must be explicit in the DXF fidelity report");

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
    const auto dimension_candidate = std::find_if(
        imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
            return entity.type == "boundary" &&
                   entity.properties.value("classification", "") == "dxf_dimension_extension";
        });
    check(dimension_candidate != imported.entities.end(),
          "imported dimension must retain its extension candidate");
    check(dimension_candidate->extensions.contains("dxf_dimension") &&
              dimension_candidate->extensions.at("dxf_dimension").value("dimension_line",
                                                                          nlohmann::json{}) ==
                  nlohmann::json::array({2.0, 1.0}) &&
              dimension_candidate->extensions.at("dxf_dimension").value("annotation_id", "") ==
                  "dxf-dimension-2",
          "dimension line and reconstructed annotation link must be retained");
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

void test_non_linear_dimensions_are_not_flattened() {
    using namespace sketch;
    auto source = make_document();
    const auto snapshot = source.snapshot();
    std::vector<Entity> entities;
    entities.reserve(snapshot.entities().size() + 2);
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        entities.push_back(entity);
    }
    entities.push_back(encode_boundary_dimension_entity(BoundaryDimension{
        .id = "dimension-angle",
        .boundary_id = "boundary-1",
        .segment_id = "edge-1",
        .text_position = {2.0, -0.5},
        .placement = BoundaryDimensionPlacement::manual,
        .automatic_placement_version = std::nullopt,
        .presentation = std::nullopt,
        .kind = BoundaryDimensionKind::angle,
        .vertex_id = "vertex-2",
        .secondary_segment_id = "edge-2"}));
    entities.push_back(encode_boundary_dimension_entity(BoundaryDimension{
        .id = "dimension-area",
        .boundary_id = "boundary-1",
        .segment_id = {},
        .text_position = {2.0, 1.5},
        .placement = BoundaryDimensionPlacement::manual,
        .automatic_placement_version = std::nullopt,
        .presentation = std::nullopt,
        .kind = BoundaryDimensionKind::area,
        .vertex_id = {},
        .secondary_segment_id = {}}));
    const auto mapped = export_project_dxf(Document::create(std::move(entities)).snapshot());
    check(mapped.drawing.dimensions.size() == 1,
          "only linear boundary dimensions may become DXF DIMENSION records");
    const auto has_diagnostic = [&](const char* id) {
        return std::any_of(mapped.diagnostics.begin(), mapped.diagnostics.end(), [&](const auto& item) {
            return item.source_id == id && item.source_kind == "dimension" &&
                   item.code == "dimension_semantics_not_representable";
        });
    };
    check(has_diagnostic("dimension-angle"),
          "angle dimensions must report their unsupported DXF semantics");
    check(has_diagnostic("dimension-area"),
          "area dimensions must report their unsupported DXF semantics");
}

void test_hidden_linear_dimensions_stay_hidden_in_dxf_output() {
    using namespace sketch;
    auto source = make_document();
    const auto snapshot = source.snapshot();
    std::vector<Entity> entities;
    entities.reserve(snapshot.entities().size() + 1);
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        entities.push_back(entity);
    }
    entities.push_back(encode_boundary_dimension_entity(BoundaryDimension{
        .id = "dimension-hidden",
        .boundary_id = "boundary-1",
        .segment_id = "edge-2",
        .text_position = {4.5, 1.5},
        .placement = BoundaryDimensionPlacement::manual,
        .automatic_placement_version = std::nullopt,
        .presentation = BoundaryDimensionPresentation{
            .text_height_mm = 2.5,
            .color = "#263241",
            .bold = false,
            .italic = false,
            .visible = false,
            .rotation_radians = 0.0},
        .kind = BoundaryDimensionKind::segment_length}));
    const auto mapped = export_project_dxf(Document::create(std::move(entities)).snapshot());
    check(mapped.drawing.dimensions.size() == 1,
          "hidden linear dimensions must not be emitted to DXF output");
    check(std::none_of(mapped.diagnostics.begin(), mapped.diagnostics.end(), [](const auto& item) {
        return item.source_id == "dimension-hidden";
    }), "hidden dimensions should not create export diagnostics");
}

void test_source_units_are_normalized_to_metres() {
    using namespace sketch;
    for (const auto units : {1, 2, 4, 5, 6, 7}) {
        const double source_length = units == 1 ? 12.0 : units == 2 ? 1.0 :
            units == 4 ? 1000.0 : units == 5 ? 100.0 : units == 7 ? 0.001 : 1.0;
        const double expected = units == 1 || units == 2 ? 0.3048 : 1.0;
        DxfDrawing drawing;
        drawing.insertion_units = units;
        drawing.lines.push_back({{0, 0}, {source_length, 0}});
        const auto imported = import_project_dxf(export_dxf_ascii(drawing));
        check(imported.complete() && !imported.source_retention_required,
              "supported units must import without fidelity diagnostics");
        check(std::abs(imported.entities.at(0).properties.at("boundary").at(0)
                           .at("end").at(0).get<double>() - expected) < 1e-12,
              "source-unit line must be converted to metres");
    }
}

void test_units_cover_primitives_annotations_and_blocks() {
    using namespace sketch;
    DxfDrawing drawing;
    drawing.insertion_units = 4;
    drawing.arcs.push_back({{1000, 2000}, 500, 0, 90});
    drawing.polylines.push_back({{{{1000, 2000}, 1}, {{3000, 2000}, 0}}, false});
    drawing.hatches.push_back({{{0, 0}, {1000, 0}, {0, 1000}}});
    drawing.labels.push_back({{1000, 2000}, 250, 30, "text"});
    drawing.dimensions.push_back({{0, 0}, {1000, 0}, {500, 200}, {500, 300}, 30, "1 m"});
    drawing.blocks.push_back({"fixture", {1000, 2000},
        {{{1000, 2000}, {2000, 2000}}},
        {{{1000, 2000}, 500, 0, 90}},
        {{{{{1000, 2000}, 1}, {{2000, 2000}, 0}}, false}},
        {{{1000, 2000}, 100, 30, "block"}}});
    drawing.inserts.push_back({"fixture", {10000, 20000}, 2, 2, 90});
    const auto imported = import_project_dxf(export_dxf_ascii(drawing));
    const auto boundary = [&](const char* classification) -> const Entity& {
        const auto found = std::find_if(imported.entities.begin(), imported.entities.end(),
            [&](const auto& entity) { return entity.properties.value("classification", "") == classification; });
        check(found != imported.entities.end(), "expected primitive candidate must exist");
        return *found;
    };
    const auto near = [](const auto& value, double expected) {
        check(std::abs(value.template get<double>() - expected) < 1e-10,
              "all primitive lengths must be in metres and angles unchanged");
    };
    const auto segment = [&](const char* classification) {
        return boundary(classification).properties.at("boundary").at(0);
    };
    near(segment("dxf_arc")["start"][0], 1.5);
    near(segment("dxf_arc")["end"][1], 2.5);
    near(segment("dxf_arc")["sweep_radians"], std::acos(-1.0) / 2);
    near(segment("dxf_polyline_open")["start"][1], 2);
    near(segment("dxf_polyline_open")["end"][0], 3);
    near(segment("dxf_polyline_open")["sweep_radians"], std::acos(-1.0));
    near(segment("dxf_hatch_solid")["end"][0], 1);
    near(segment("dxf_dimension_extension")["end"][0], 1);
    const auto& dimension = boundary("dxf_dimension_extension").extensions.at("dxf_dimension");
    near(dimension["dimension_line"][0], 0.5);
    near(dimension["dimension_line"][1], 0.2);
    near(dimension["text_position"][1], 0.3);
    near(dimension["rotation_degrees"], 30);
    near(segment("dxf_insert_line")["start"][0], 10);
    near(segment("dxf_insert_line")["start"][1], 20);
    near(segment("dxf_insert_line")["end"][1], 22);
    near(segment("dxf_insert_arc")["start"][1], 21);
    near(segment("dxf_insert_arc")["end"][0], 9);
    near(segment("dxf_insert_polyline_open")["end"][1], 22);
    near(segment("dxf_insert_polyline_open")["sweep_radians"], std::acos(-1.0));
    const auto annotations = decode_annotation_entity(imported.entities.back());
    check(annotations.labels.size() == 3, "text and dimension labels must remain editable");
    check(std::abs(annotations.labels[0].style.text_height_metres - 0.25) < 1e-12 &&
          std::abs(annotations.labels[0].placement.position.y - 2) < 1e-12 &&
          std::abs(annotations.labels[0].placement.rotation_radians - std::acos(-1.0) / 6) < 1e-12 &&
          std::abs(annotations.labels[1].placement.position.y - 0.3) < 1e-12 &&
          std::abs(annotations.labels[2].style.text_height_metres - 0.2) < 1e-12 &&
          std::abs(annotations.labels[2].placement.position.x - 10) < 1e-12 &&
          std::abs(annotations.labels[2].placement.position.y - 20) < 1e-12,
          "text height, positions and insert scale must normalize without scaling angles");
    check(imported.source_retention_required, "dimension fidelity diagnostics must still retain source");
}

void test_unspecified_or_unsupported_units_fail_closed() {
    using namespace sketch;
    for (const int units : {0, 3, 20}) {
        DxfDrawing drawing;
        drawing.insertion_units = units;
        drawing.lines.push_back({{0, 0}, {1000, 0}});
        const auto imported = import_project_dxf(export_dxf_ascii(drawing));
        check(imported.entities.empty() && !imported.complete() && imported.source_retention_required,
              "unresolved units must not silently create metre geometry");
        check(std::any_of(imported.diagnostics.begin(), imported.diagnostics.end(), [&](const auto& item) {
            return item.source_kind == "HEADER" && item.code ==
                (units == 0 ? "source_units_unspecified" : "source_units_unsupported");
        }), "unresolved source units must have an actionable fidelity diagnostic");
    }
    auto missing = wrapped_entities("0\nCIRCLE\n10\n0\n20\n0\n40\n2\n");
    missing.erase(missing.find("9\n$INSUNITS\n70\n6\n"), std::string("9\n$INSUNITS\n70\n6\n").size());
    const auto unspecified = import_project_dxf(missing);
    check(unspecified.entities.empty() && unspecified.source_retention_required &&
          unspecified.diagnostics.size() == 2,
          "missing units must preserve both units and unsupported-record diagnostics");
    auto unknown = wrapped_entities("0\nLINE\n10\n0\n20\n0\n11\n1000\n21\n0\n");
    unknown.replace(unknown.find("$INSUNITS\n70\n6"), std::string("$INSUNITS\n70\n6").size(), "$INSUNITS\n70\n99");
    bool rejected = false;
    try { (void)import_project_dxf(unknown); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "unknown unit codes must fail closed");
}

} // namespace

int main() {
    try {
        run();
        test_source_units_are_normalized_to_metres();
        test_units_cover_primitives_annotations_and_blocks();
        test_unspecified_or_unsupported_units_fail_closed();
        test_non_linear_dimensions_are_not_flattened();
        test_hidden_linear_dimensions_stay_hidden_in_dxf_output();
        std::cout << "DXF project exchange tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
