#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/dxf_exchange.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/hosted_opening_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    check(exported.drawing.blocks.size() == 2, "wall and opening must export native plan blocks");
    check(std::any_of(exported.drawing.blocks.begin(), exported.drawing.blocks.end(),
        [](const auto& block) { return block.arcs.size() == 1 && block.lines.size() == 1; }),
          "door must export an analytical swing arc and leaf");
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
    check(std::none_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "wall-1";
    }), "wall semantics must be retained by native metadata");
    check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "slab-1" && item.code == "slab_3d_semantics_not_representable";
    }), "slab 3D semantics must be explicit in the DXF fidelity report");
    check(std::none_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "opening-1";
    }), "opening host relationship must be retained by native metadata");

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

sketch::Document native_hosted_document(bool curved, bool manufactured = false) {
    using namespace sketch;
    Entity wall{"native-wall", "wall", {{"baseline", {{"start", {2, 3}}, {"end", {10, 9}},
        {"sweep_radians", curved ? std::acos(-1.0) / 3 : 0.0}}},
        {"thickness_m", 0.3}, {"height_m", 3.0}, {"elevation_m", 0.4}}, false,
        {{"opaque", "retained"}}};
    std::vector<Entity> entities{wall};
    for (const auto kind : {"door", "window", "opening"}) {
        const double offset = std::string(kind) == "door" ? 1 : std::string(kind) == "window" ? 3 : 6;
        Entity opening{"native-" + std::string(kind), "opening", {{"wall_id", wall.id},
            {"opening_kind", kind}, {"offset_m", offset}, {"width_m", 1.2},
            {"sill_m", std::string(kind) == "window" ? 0.8 : 0.0}, {"height_m", 2.0}}, false,
            {{"opaque", nlohmann::json::array({1, 2, 3})}}};
        if (manufactured && std::string(kind) != "opening") {
            auto assembly = default_opening_assembly(std::string(kind) == "door" ?
                OpeningAssemblyKind::door : OpeningAssemblyKind::window);
            opening.properties["opening_assembly"] = opening_assembly_json(assembly);
        }
        if (std::string(kind) == "door")
            opening.properties["door_operation"] = encode_door_operation({true, false, 65});
        entities.push_back(opening);
    }
    return Document::create(std::move(entities));
}

void test_manufactured_native_depiction() {
    using namespace sketch;
    for (const bool curved : {false, true}) {
        auto source = native_hosted_document(curved, true).snapshot();
        const auto exported = export_project_dxf(source);
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
        check(exported.diagnostics.empty(), "manufactured horizontal cuts must export without loss");
        if (const auto* capture = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
            std::filesystem::create_directories(capture);
            std::ofstream output(std::filesystem::path(capture) /
                (curved ? "native-curved-manufactured.dxf" : "native-rotated-manufactured.dxf"), std::ios::binary);
            output << export_dxf_ascii(exported.drawing);
        }
        const auto block_for = [](const DxfDrawing& drawing, const std::string& id) -> const DxfBlock& {
            const auto block = std::find_if(drawing.blocks.begin(), drawing.blocks.end(), [&](const auto& value) {
                return !value.vertex_entity_json.empty() && nlohmann::json::parse(value.vertex_entity_json).at("id") == id;
            });
            check(block != drawing.blocks.end(), "manufactured block must retain bounded native metadata");
            return *block;
        };
        const auto& window = block_for(exported.drawing, "native-window");
        check(nlohmann::json::parse(window.vertex_entity_json).at("depiction") == "MANUFACTURED_PLAN_V1",
              "profile blocks require an explicit manufactured depiction contract");
        check(curved ? window.arcs.size() >= 6 : window.lines.size() >= 12,
              "horizontal cut must depict frame, sash and glass profiles");
        check(window.polylines.empty(), "manufactured circular profiles must remain exact ARC records");
        const auto& door = block_for(exported.drawing, "native-door");
        check(std::any_of(door.arcs.begin(), door.arcs.end(), [](const auto& arc) {
            return arc.radius > 1.0 && arc.radius < 1.1;
        }), "door swing must use the physical clear leaf width, including curved frame fitting");
        auto closed = source.entities();
        closed.at("native-door").properties.erase("door_operation");
        std::vector<Entity> closed_entities;
        for (const auto& [id, entity] : closed) { (void)id; closed_entities.push_back(entity); }
        const auto closed_export = export_project_dxf(Document::create(closed_entities).snapshot());
        check(closed_export.diagnostics.empty(), "unspecified handing must preserve the closed manufactured leaf");
        const auto& closed_door = block_for(closed_export.drawing, "native-door");
        check(std::none_of(closed_door.arcs.begin(), closed_door.arcs.end(), [](const auto& arc) {
            return arc.radius < 2.0;
        }), "closed leaf must not fabricate an operation or a nominal swing arc");
        const auto closed_import = import_project_dxf(export_dxf_ascii(closed_export.drawing));
        check(closed_import.complete() && closed_import.entities.size() == 4,
              "closed manufactured leaf must regenerate its native graph without an invented operation");
        const auto fallback = [](DxfDrawing drawing) {
            const auto imported = import_project_dxf(export_dxf_ascii(drawing));
            check(imported.source_retention_required && std::none_of(imported.entities.begin(), imported.entities.end(),
                [](const auto& entity) { return entity.type == "wall" || entity.type == "opening"; }),
                "mismatched manufactured depiction must never activate any member of its host graph");
        };
        auto changed = source.entities();
        auto profile = parse_opening_assembly(changed.at("native-window").properties.at("opening_assembly"));
        profile.panel_thickness_m += 0.01; profile.inset_m = curved ? -0.035 : 0.035;
        changed.at("native-window").properties["opening_assembly"] = opening_assembly_json(profile);
        std::vector<Entity> entities;
        for (const auto& [id, entity] : changed) { (void)id; entities.push_back(entity); }
        const auto varied = export_project_dxf(Document::create(entities).snapshot());
        check(varied.diagnostics.empty(), "signed manufactured inset must be admitted");
        auto original_geometry = window; original_geometry.vertex_entity_json.clear();
        auto varied_geometry = block_for(varied.drawing, "native-window"); varied_geometry.vertex_entity_json.clear();
        DxfDrawing original_probe, varied_probe;
        original_probe.blocks.push_back(original_geometry); varied_probe.blocks.push_back(varied_geometry);
        check(export_dxf_ascii(original_probe) != export_dxf_ascii(varied_probe),
              "manufactured dimensions and inset must change visible DXF primitives");
        for (const auto* key : {"frame_width_m", "frame_depth_m", "panel_thickness_m", "glazing_thickness_m", "inset_m"}) {
            auto dimension_change = source.entities();
            auto& dimensions = dimension_change.at("native-window").properties["opening_assembly"];
            dimensions[key] = dimensions.at(key).get<double>() + 0.005;
            std::vector<Entity> changed_entities;
            for (const auto& [id, entity] : dimension_change) { (void)id; changed_entities.push_back(entity); }
            const auto dimension_export = export_project_dxf(Document::create(changed_entities).snapshot());
            check(dimension_export.diagnostics.empty(), "valid independent profile dimensions must be admitted");
            auto dimension_geometry = block_for(dimension_export.drawing, "native-window");
            dimension_geometry.vertex_entity_json.clear();
            DxfDrawing dimension_probe; dimension_probe.blocks.push_back(dimension_geometry);
            check(export_dxf_ascii(original_probe) != export_dxf_ascii(dimension_probe),
                  "each independent manufactured profile dimension must change primitive geometry");
        }
        for (const bool hinge_at_end : {false, true}) for (const bool swing_left : {false, true}) {
            auto handed = source.entities();
            handed.at("native-door").properties["door_operation"] = encode_door_operation({hinge_at_end, swing_left, 65});
            handed.at("native-door").properties["opening_assembly"]["inset_m"] = swing_left ? 0.03 : -0.03;
            std::vector<Entity> handed_entities;
            for (const auto& [id, entity] : handed) { (void)id; handed_entities.push_back(entity); }
            const auto handed_export = export_project_dxf(Document::create(handed_entities).snapshot());
            check(handed_export.diagnostics.empty(), "all door handing and signed inset combinations must be admitted");
            const auto handed_import = import_project_dxf(export_dxf_ascii(handed_export.drawing));
            check(handed_import.complete() && handed_import.entities.size() == 4,
                  "physical door handing must independently regenerate and activate its native graph");
        }
        auto tampered = exported.drawing;
        auto& block = *std::find_if(tampered.blocks.begin(), tampered.blocks.end(), [](const auto& value) {
            return nlohmann::json::parse(value.vertex_entity_json).at("id") == "native-window";
        });
        auto payload = nlohmann::json::parse(block.vertex_entity_json);
        payload.erase("depiction"); block.vertex_entity_json = payload.dump(); fallback(tampered);
        tampered = exported.drawing;
        auto& profile_block = *std::find_if(tampered.blocks.begin(), tampered.blocks.end(), [](const auto& value) {
            return nlohmann::json::parse(value.vertex_entity_json).at("id") == "native-window";
        });
        payload = nlohmann::json::parse(profile_block.vertex_entity_json);
        payload["properties"]["opening_assembly"]["panel_thickness_m"] = 0.07;
        profile_block.vertex_entity_json = payload.dump(); fallback(tampered);
        payload["properties"]["opening_assembly"]["frame_depth_m"] = 0.5;
        profile_block.vertex_entity_json = payload.dump(); fallback(tampered);
        auto repeat = import_project_dxf(export_dxf_ascii(varied.drawing));
        for (int cycle = 0; cycle < 3; ++cycle) {
            check(repeat.complete() && repeat.entities.size() == 4, "manufactured graph must round trip repeatedly");
            const auto next = export_project_dxf(Document::create(repeat.entities).snapshot());
            repeat = import_project_dxf(export_dxf_ascii(next.drawing));
        }
#else
        check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
            return item.code == "manufactured_plan_geometry_unavailable";
        }), "core-only export must explicitly report unavailable manufactured geometry");
        const auto imported = import_project_dxf(export_dxf_ascii(exported.drawing));
        check(imported.source_retention_required && std::none_of(imported.entities.begin(), imported.entities.end(),
            [](const auto& entity) { return entity.type == "wall" || entity.type == "opening"; }),
            "core-only symbols must never claim admitted manufactured native geometry");
#endif
    }
}

void test_window_layout_native_correspondence() {
#ifdef SKETCH_DXF_NATIVE_GEOMETRY
    using namespace sketch;
    // This fixture has a rotated straight host, so moving profiles must retain
    // their world-space placement as well as their persisted operation.
    const auto source = native_hosted_document(false, true).snapshot();
    const auto window_block = [](DxfDrawing& drawing) -> DxfBlock& {
        const auto found = std::find_if(drawing.blocks.begin(), drawing.blocks.end(), [](const auto& block) {
            return !block.vertex_entity_json.empty() &&
                nlohmann::json::parse(block.vertex_entity_json).at("id") == "native-window";
        });
        check(found != drawing.blocks.end(), "window layout must export a native block");
        return *found;
    };
    const auto geometry_bytes = [](DxfBlock block) {
        block.vertex_entity_json.clear();
        DxfDrawing probe;
        probe.blocks.push_back(std::move(block));
        return export_dxf_ascii(probe);
    };
    auto fixed = export_project_dxf(source).drawing;
    const auto fixed_geometry = geometry_bytes(window_block(fixed));
    std::vector<std::string> layout_geometry;
    for (const auto layout : {WindowLayoutKind::double_fixed, WindowLayoutKind::triple_fixed,
                              WindowLayoutKind::casement, WindowLayoutKind::sliding}) {
        auto profile = default_opening_assembly(OpeningAssemblyKind::window);
        profile.window_layout = layout;
        if (layout == WindowLayoutKind::casement) {
            profile.window_hinge_at_end = true;
            profile.window_open_left = false;
            profile.window_angle_degrees = 70.0;
        }
        if (layout == WindowLayoutKind::sliding) {
            profile.window_hinge_at_end = true;
            profile.window_open_left = false;
            profile.window_slide_fraction = 0.5;
        }
        auto entities = source.entities();
        entities.at("native-window").properties["opening_assembly"] = opening_assembly_json(profile);
        std::vector<Entity> fixture;
        for (const auto& [id, entity] : entities) { (void)id; fixture.push_back(entity); }
        auto exported = export_project_dxf(Document::create(std::move(fixture)).snapshot());
        check(exported.diagnostics.empty(), "supported window layout must export without semantic loss");
        auto& block = window_block(exported.drawing);
        const auto geometry = geometry_bytes(block);
        check(geometry != fixed_geometry &&
                  std::find(layout_geometry.begin(), layout_geometry.end(), geometry) == layout_geometry.end(),
              "split and moving window layouts must each change actual DXF primitives");
        layout_geometry.push_back(geometry);
        const auto imported = import_project_dxf(export_dxf_ascii(exported.drawing));
        check(imported.complete() && imported.entities.size() == 4,
              "matching layout geometry must activate the complete native host graph");
        const auto window = std::find_if(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
            return entity.type == "opening" &&
                entity.extensions.at("vertex_dxf_source").at("id") == "native-window";
        });
        const auto host = std::find_if(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
            return entity.type == "wall";
        });
        check(host != imported.entities.end(), "matching window layout must restore its native host");
        auto expected_properties = entities.at("native-window").properties;
        expected_properties["wall_id"] = host->id;
        check(window != imported.entities.end() && window->type == "opening" &&
                  window->properties == expected_properties &&
                  parse_opening_assembly(window->properties.at("opening_assembly")) == profile,
              "window layout and operation must remain editable only after geometry correspondence");
        const auto reject_false_model = [](const DxfDrawing& drawing) {
            const auto result = import_project_dxf(export_dxf_ascii(drawing));
            check(result.source_retention_required &&
                      std::none_of(result.entities.begin(), result.entities.end(), [](const auto& entity) {
                          return entity.type == "wall" || entity.type == "opening";
                      }),
                  "tampered window layout must never activate an editable native host graph");
        };
        auto metadata_tamper = exported.drawing;
        auto& metadata_block = window_block(metadata_tamper);
        auto payload = nlohmann::json::parse(metadata_block.vertex_entity_json);
        auto false_profile = profile;
        false_profile.window_layout = WindowLayoutKind::fixed;
        false_profile.window_hinge_at_end = false;
        false_profile.window_open_left = true;
        false_profile.window_angle_degrees = 90.0;
        false_profile.window_slide_fraction = 0.0;
        payload["properties"]["opening_assembly"] = opening_assembly_json(false_profile);
        metadata_block.vertex_entity_json = payload.dump();
        reject_false_model(metadata_tamper);
        auto stroke_tamper = exported.drawing;
        auto& stroke_block = window_block(stroke_tamper);
        check(!stroke_block.lines.empty(), "rotated window layout must contain physical profile strokes");
        stroke_block.lines.front().end.x += 0.025;
        reject_false_model(stroke_tamper);
    }
#endif
}

void test_native_hosted_roundtrip_and_fallback() {
    using namespace sketch;
    for (const bool curved : {false, true}) {
        const auto source = native_hosted_document(curved).snapshot();
        const auto mapped = export_project_dxf(source);
        check(mapped.diagnostics.empty() && mapped.drawing.blocks.size() == 4 && mapped.drawing.inserts.size() == 4,
              "all native wall/opening plan blocks must export without semantic loss");
        const auto block_for = [&](const char* id) -> const DxfBlock& {
            const auto found = std::find_if(mapped.drawing.blocks.begin(), mapped.drawing.blocks.end(),
                [&](const auto& block) { return nlohmann::json::parse(block.vertex_entity_json).at("id") == id; });
            check(found != mapped.drawing.blocks.end(), "native block ID must exist");
            return *found;
        };
        const auto& door = block_for("native-door");
        check(door.lines.size() == 1 && door.arcs.size() == 1,
              "door operation must produce an analytic leaf and swing");
        const auto& window = block_for("native-window");
        check(curved ? window.arcs.size() == 2 && window.lines.size() == 2 : window.lines.size() == 4,
              "window rails must preserve curved or rotated hosts");
        const auto& bare = block_for("native-opening");
        check(curved ? bare.lines.size() == 2 && bare.arcs.size() == 1 : bare.lines.size() == 3,
              "bare opening must have two jambs and exact threshold");
        const auto bytes = export_dxf_ascii(mapped.drawing);
        if (const auto* capture = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
            std::filesystem::create_directories(capture);
            std::ofstream output(std::filesystem::path(capture) / (curved ? "native-curved.dxf" : "native-rotated.dxf"), std::ios::binary);
            output << bytes;
        }
        const auto imported = import_project_dxf(bytes);
        check(imported.complete() && !imported.source_retention_required && imported.entities.size() == 4,
              "valid native metadata must replace fallback symbols with complete active graph");
        auto restored = Document::create(imported.entities);
        const auto restored_snapshot = restored.snapshot();
        const auto host = std::find_if(imported.entities.begin(), imported.entities.end(), [](const auto& entity) { return entity.type == "wall"; });
        check(host != imported.entities.end() && host->id != "native-wall" && host->properties == source.entities().at("native-wall").properties,
              "native wall properties and fresh identity must survive");
        for (const auto& entity : imported.entities) {
            check(entity.extensions.contains("vertex_dxf_source"), "opaque native source identity must be retained");
            if (entity.type == "opening") {
                const auto original = entity.extensions.at("vertex_dxf_source").at("id").get<std::string>();
                auto expected = source.entities().at(original).properties;
                expected["wall_id"] = host->id;
                check(entity.properties == expected, "host, kind, sill, profile and swing must remain active");
            }
        }
        auto repeat = imported.entities;
        std::size_t previous_size = 0;
        for (int cycle = 0; cycle < 4; ++cycle) {
            const auto again = export_project_dxf(Document::create(repeat).snapshot());
            const auto next = import_project_dxf(export_dxf_ascii(again.drawing));
            check(next.complete() && next.entities.size() == 4, "repeated native exchange must remain bounded and active");
            const auto same_source = std::find_if(again.drawing.blocks.begin(), again.drawing.blocks.end(),
                [](const auto& block) {
                    return nlohmann::json::parse(block.vertex_entity_json).at("extensions")
                        .at("vertex_dxf_source").at("id") == "native-door";
                });
            check(same_source != again.drawing.blocks.end(), "same retained native identity must remain present");
            const auto size = same_source->vertex_entity_json.size();
            if (cycle > 0) check(size == previous_size, "source retention must not nest on repeated round trips");
            previous_size = size; repeat = next.entities;
        }
        const auto fallback = [&](DxfDrawing altered) {
            const auto result = import_project_dxf(export_dxf_ascii(altered));
            check(result.source_retention_required && !result.diagnostics.empty() &&
                std::none_of(result.entities.begin(), result.entities.end(), [](const auto& entity) { return entity.type == "wall" || entity.type == "opening"; }) &&
                std::any_of(result.entities.begin(), result.entities.end(), [](const auto& entity) { return entity.type == "boundary"; }),
                "unsafe native metadata must preserve visual fallback and explicit loss diagnostics");
        };
        auto changed = mapped.drawing;
        changed.blocks.front().lines.front().end.x += 0.02; fallback(changed);
        changed = mapped.drawing; changed.blocks.front().vertex_entity_json = "{"; fallback(changed);
        changed = mapped.drawing; changed.inserts.front().rotation_degrees = 20; fallback(changed);
        changed = mapped.drawing; changed.inserts.erase(changed.inserts.begin()); fallback(changed);
        changed = mapped.drawing; changed.insertion_units = 4; fallback(changed);
        changed = mapped.drawing;
        auto payload = nlohmann::json::parse(changed.blocks.front().vertex_entity_json);
        payload["version"] = 2; changed.blocks.front().vertex_entity_json = payload.dump(); fallback(changed);
        changed = mapped.drawing;
        payload = nlohmann::json::parse(changed.blocks.front().vertex_entity_json);
        payload["type"] = "reference_asset"; changed.blocks.front().vertex_entity_json = payload.dump(); fallback(changed);
        changed = mapped.drawing;
        changed.blocks.front().vertex_entity_json.insert(1, "\"version\":1,"); fallback(changed);
        changed = mapped.drawing;
        auto nested = nlohmann::json::object();
        for (int depth = 0; depth < 20; ++depth) nested = {{"nested", nested}};
        payload = nlohmann::json::parse(changed.blocks.front().vertex_entity_json);
        payload["extensions"] = nested; changed.blocks.front().vertex_entity_json = payload.dump(); fallback(changed);
    }
}

void test_legacy_native_wall_candidates_are_canonical() {
    using namespace sketch;
    Entity wall{"legacy-wall", "wall", {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}},
        {"sweep_radians", 0.0}}}, {"thickness", 0.2}, {"height", 3.0}, {"elevation", 0.5}},
        false, nlohmann::json::object()};
    const auto mapped = export_project_dxf(Document::create({wall}).snapshot());
    const auto imported = import_project_dxf(export_dxf_ascii(mapped.drawing));
    check(imported.complete() && imported.entities.size() == 1 && imported.entities[0].type == "wall",
          "legacy wall fields must retain native semantics");
    const auto& candidate = imported.entities[0];
    check(candidate.properties.at("thickness_m") == 0.2 && candidate.properties.at("height_m") == 3.0 &&
          candidate.properties.at("elevation_m") == 0.5 && !candidate.properties.contains("thickness") &&
          candidate.extensions.at("vertex_dxf_source").at("properties") == wall.properties,
          "active candidate must use canonical SI fields while original aliases remain opaque source evidence");
}

void test_native_architectural_sources_are_bounded() {
    using namespace sketch;
    const auto source = native_hosted_document(false).snapshot();
    auto changed = source.entities();
    changed.at("native-wall").properties["baseline"]["start"][0] = 2e7;
    changed.at("native-wall").properties["baseline"]["end"][0] = 2e7 + 10;
    std::vector<Entity> entities;
    for (const auto& [id, entity] : changed) { (void)id; entities.push_back(entity); }
    const auto exported = export_project_dxf(Document::create(entities).snapshot());
    check(exported.drawing.blocks.empty() && exported.diagnostics.size() == 4,
          "unbounded source coordinates must be rejected before native solid or section work");
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
        test_native_hosted_roundtrip_and_fallback();
        test_manufactured_native_depiction();
        test_window_layout_native_correspondence();
        test_legacy_native_wall_candidates_are_canonical();
        test_native_architectural_sources_are_bounded();
        std::cout << "DXF project exchange tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
