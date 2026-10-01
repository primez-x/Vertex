#include "sketch/boundary_entity.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/document.hpp"
#include "sketch/ifc_project_exchange.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/ifc_native_geometry.hpp"
#include "sketch/project_import_worker.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Deliberately independent of the production STEP parser: inspect the exported
// graph and field cardinalities rather than proving correctness by round-trip.
struct Record { std::string type; std::vector<std::string> fields; };
std::vector<std::string> fields(const std::string& text) {
    std::vector<std::string> result;
    bool quoted = false;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') {
            if (quoted && i + 1 < text.size() && text[i + 1] == '\'') ++i;
            else quoted = !quoted;
        } else if (!quoted) {
            if (text[i] == '(') ++depth;
            if (text[i] == ')') --depth;
            if (text[i] == ',' && depth == 0) {
                result.push_back(text.substr(start, i - start)); start = i + 1;
            }
        }
    }
    check(!quoted && depth == 0, "independent reader requires balanced STEP fields");
    result.push_back(text.substr(start));
    return result;
}
std::map<std::string, Record> records(const std::string& step) {
    std::map<std::string, Record> result;
    std::istringstream input(step);
    std::string line;
    const std::regex pattern(R"((#[0-9]+)=([A-Z0-9]+)\((.*)\);)");
    while (std::getline(input, line)) {
        if (!line.starts_with('#')) continue;
        std::smatch match;
        check(std::regex_match(line, match, pattern), "independent reader requires valid record syntax");
        check(result.emplace(match[1].str(), Record{match[2].str(), fields(match[3].str())}).second,
              "record ids must be unique");
    }
    return result;
}
std::vector<std::string> list(const std::string& value) {
    check(value.front() == '(' && value.back() == ')', "expected STEP list");
    return fields(value.substr(1, value.size() - 2));
}

void verify_export_graph(const std::string& step) {
    const auto graph = records(step);
    const std::map<std::string, std::size_t> arity{
        {"IFCPERSON",8}, {"IFCORGANIZATION",5}, {"IFCPROJECT",9}, {"IFCSITE",14},
        {"IFCBUILDING",12}, {"IFCBUILDINGSTOREY",10}, {"IFCWALL",9}, {"IFCWALLTYPE",10},
        {"IFCMATERIAL",3}, {"IFCMATERIALLAYER",7}, {"IFCMATERIALLAYERSET",3}};
    std::set<std::string> guids;
    std::set<std::pair<std::string, std::string>> spatial_links;
    std::set<std::string> contained;
    const auto real_literal = [](const std::string& value) {
        return value.find_first_of(".eE") != std::string::npos;
    };
    for (const auto& [id, record] : graph) {
        if (record.type == "IFCEXTRUDEDAREASOLID")
            check(real_literal(record.fields.at(3)), "IFC extrusion depth must use STEP REAL lexical syntax");
        if (record.type == "IFCDOOR" || record.type == "IFCWINDOW")
            check(real_literal(record.fields.at(8)) && real_literal(record.fields.at(9)),
                  "IFC fill dimensions must use STEP REAL lexical syntax");
        if (record.type == "IFCCARTESIANPOINT" || record.type == "IFCDIRECTION")
            for (const auto& coordinate : list(record.fields.at(0)))
                check(real_literal(coordinate), "IFC coordinates and directions must use STEP REAL lexical syntax");
        if (record.type == "IFCCARTESIANPOINTLIST3D")
            for (const auto& row : list(record.fields.at(0)))
                for (const auto& coordinate : list(row))
                    check(real_literal(coordinate), "IFC tessellation coordinates must use STEP REAL lexical syntax");
        if (const auto expected = arity.find(record.type); expected != arity.end())
            check(record.fields.size() == expected->second, "IFC4 entity attribute count must match schema");
        if (record.type == "IFCPROJECT") {
            const auto contexts = list(record.fields[7]);
            check(contexts.size() == 1 && graph.at(contexts[0]).type == "IFCGEOMETRICREPRESENTATIONCONTEXT",
                  "project must own the geometric representation context");
            check(graph.at(record.fields[8]).type == "IFCUNITASSIGNMENT", "project must own units");
        }
        if (record.type == "IFCSHAPEREPRESENTATION") {
            check(graph.at(record.fields[0]).type == "IFCGEOMETRICREPRESENTATIONCONTEXT",
                  "every representation must reference a geometric context");
        }
        if (record.type == "IFCRELAGGREGATES") {
            for (const auto& child : list(record.fields[5]))
                spatial_links.emplace(graph.at(record.fields[4]).type, graph.at(child).type);
        }
        if (record.type == "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
            check(graph.at(record.fields[5]).type == "IFCBUILDINGSTOREY", "containment must target a storey");
            for (const auto& product : list(record.fields[4]))
                check(contained.insert(product).second, "products must have unique spatial containment");
        }
        if (record.type == "IFCWALL") {
            const auto& shape = graph.at(record.fields[6]);
            const auto& representation = graph.at(list(shape.fields[2]).at(0));
            check(representation.fields[1] == "'Body'" && representation.fields[2] == "'SweptSolid'",
                  "wall needs a real swept body representation");
            const auto& solid = graph.at(list(representation.fields[3]).at(0));
            check(solid.type == "IFCEXTRUDEDAREASOLID" && std::stod(solid.fields[3]) == 2.5,
                  "wall body must extrude to native height");
            const auto& profile = graph.at(solid.fields[0]);
            const auto& polyline = graph.at(profile.fields[2]);
            const auto points = list(polyline.fields[0]);
            check(points.size() == 5, "wall must have a closed rectangular profile");
            double min_y = 1e9, max_y = -1e9;
            for (const auto& point : points) {
                const auto coordinates = list(graph.at(point).fields[0]);
                check(coordinates.size() == 2, "swept profile coordinates must be 2D");
                min_y = std::min(min_y, std::stod(coordinates[1]));
                max_y = std::max(max_y, std::stod(coordinates[1]));
            }
            check(std::abs(max_y - min_y - 0.2) < 1e-9, "wall profile must use native thickness");
        }
        if (record.type.starts_with("IFCREL") || record.type == "IFCWALL" || record.type == "IFCWALLTYPE" ||
            record.type == "IFCPROJECT" || record.type == "IFCSITE" || record.type == "IFCBUILDING" ||
            record.type == "IFCBUILDINGSTOREY" || record.type == "IFCSLAB" || record.type == "IFCOPENINGELEMENT" ||
            record.type == "IFCBUILDINGELEMENTPROXY" || record.type == "IFCPROPERTYSET") {
            const auto& guid = record.fields[0];
            check(guid.size() == 24 && guid[1] >= '0' && guid[1] <= '3' && guids.insert(guid).second,
                  "root GUIDs must be valid compressed width and unique");
        }
    }
    check(spatial_links == std::set<std::pair<std::string,std::string>>{
        {"IFCPROJECT","IFCSITE"}, {"IFCSITE","IFCBUILDING"}, {"IFCBUILDING","IFCBUILDINGSTOREY"}},
        "spatial decomposition must connect project through storey");
    for (const auto& [id, record] : graph) {
        if (record.type == "IFCWALL" || record.type == "IFCSLAB" || record.type == "IFCBUILDINGELEMENTPROXY")
            check(contained.contains(id), "all physical products must be contained");
        if (record.type == "IFCOPENINGELEMENT") check(!contained.contains(id), "void features must use their host relationship");
    }
}

sketch::Document make_document(double elevation = 0.0) {
    using namespace sketch;
    const IdentifiedBoundary model{
        "boundary-1", "boundary",
        {{"edge-1", "vertex-1", "vertex-2", {{0, 0}, {4, 0}, 0}},
         {"edge-2", "vertex-2", "vertex-3", {{4, 0}, {4, 3}, 0}},
         {"edge-3", "vertex-3", "vertex-4", {{4, 3}, {0, 3}, 0}},
         {"edge-4", "vertex-4", "vertex-1", {{0, 3}, {0, 0}, 0}}}};
    auto boundary = encode_identified_boundary_entity(model);
    Entity wall{"wall-1", "wall",
                {{"baseline", {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.0}}},
                 {"thickness_m", 0.2}, {"height_m", 2.5}, {"elevation_m", elevation}},
                false, nlohmann::json::object()};
    Entity slab{"slab-1", "slab",
                {{"boundary", {{{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", 0.0}},
                                {{"start", {4, 0}}, {"end", {4, 3}}, {"sweep_radians", 0.0}},
                                {{"start", {4, 3}}, {"end", {0, 3}}, {"sweep_radians", 0.0}},
                                {{"start", {0, 3}}, {"end", {0, 0}}, {"sweep_radians", 0.0}}}},
                 {"holes", nlohmann::json::array()}, {"thickness_m", 0.15},
                 {"elevation_m", elevation}, {"element_kind", elevation == 0.0 ? "floor" : "ceiling"}},
                false, nlohmann::json::object()};
    const nlohmann::json opening_assembly{
        {"version", 1}, {"kind", "door"}, {"frame_width_m", 0.08},
        {"frame_depth_m", 0.12}, {"panel_thickness_m", 0.04},
        {"glazing_thickness_m", 0.0}, {"inset_m", 0.0}};
    Entity opening{"opening-1", "opening",
                   {{"wall_id", "wall-1"}, {"opening_kind", "door"},
                    {"offset_m", 1.0}, {"width_m", 1.0}, {"sill_m", 0.1},
                    {"height_m", 2.0}, {"opening_assembly", opening_assembly}},
                   false, nlohmann::json::object()};
    return Document::create({std::move(boundary), std::move(wall),
                             std::move(opening), std::move(slab)});
}

#ifdef SKETCH_IFC_NATIVE_GEOMETRY
void verify_worker_candidate(const sketch::IfcProjectImportResult& imported) {
    sketch::ProjectImportCandidate candidate;
    candidate.kind = sketch::ProjectImportKind::ifc;
    candidate.entities = imported.entities;
    candidate.source_retention_required = imported.source_retention_required;
    for (const auto& diagnostic : imported.diagnostics)
        candidate.diagnostics.push_back({diagnostic.source_id, diagnostic.source_kind, diagnostic.code});
    check(!sketch::encode_project_import_candidate(candidate).empty(),
          "native IFC candidate must satisfy the strict worker protocol before isolation transport");
}

void desktop_hosted_worker_protocol() {
    using namespace sketch;
    auto door_wall = make_document().snapshot().entities().at("wall-1");
    auto door = make_document().snapshot().entities().at("opening-1");
    door_wall.properties["baseline"]["end"] = {8, 0};
    door_wall.properties["baseline"]["sweep_radians"] = 0.5;
    door_wall.properties["height_m"] = 2.4384;
    door_wall.properties["thickness_m"] = 0.3;
    door.properties["width_m"] = 0.9;
    door.properties["sill_m"] = 0;
    door.properties["door_operation"] = encode_door_operation(DoorOperation{true, false, 67.0});
    door.properties["opening_assembly"]["frame_depth_m"] = 0.25;
    door.properties["opening_assembly"]["panel_thickness_m"] = 0.035;
    door.properties["opening_assembly"]["glazing_thickness_m"] = 0.012;
    door.properties["opening_assembly"]["inset_m"] = -0.015;
    auto window_wall = door_wall;
    window_wall.id = "window-wall";
    window_wall.properties["baseline"] = {{"start", {0, 5}}, {"end", {12, 5}}, {"sweep_radians", 0.0}};
    auto window = door;
    window.id = "window-opening";
    window.properties["wall_id"] = window_wall.id;
    window.properties["opening_kind"] = "window";
    window.properties["offset_m"] = 9.0;
    window.properties["width_m"] = 1.0;
    window.properties["sill_m"] = 0.8;
    window.properties["height_m"] = 1.2;
    window.properties.erase("door_operation");
    window.properties["opening_assembly"] = {{"version", 1}, {"kind", "window"},
        {"frame_width_m", 0.07}, {"frame_depth_m", 0.24}, {"panel_thickness_m", 0.03},
        {"glazing_thickness_m", 0.014}, {"inset_m", 0.01}};
    // Desktop projects also export source organization objects as inert native
    // references. The exact reference property shape is part of the protocol.
    const Entity source_property{"source-property", "property", {{"name", "Source property"}}};
    const auto imported = import_project_ifc(export_project_ifc(Document::create(
        {door_wall, door, window_wall, window, source_property}).snapshot()).step);
    check(std::count_if(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.type == "wall" || entity.type == "opening";
    }) == 4, "desktop-like IFC fixture must restore both hosts and both fills");
    check(std::count_if(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.type == "ifc_reference" && entity.properties.size() == 2 &&
            entity.properties.contains("ifc_name") && entity.properties.contains("ifc_type");
    }) == 1, "source organization references must keep the canonical inert protocol shape");
    verify_worker_candidate(imported);
    if (const auto* path = std::getenv("VERTEX_TEST_INPUT_IFC")) {
        std::ifstream input(path, std::ios::binary);
        check(input.good(), "captured desktop IFC must be readable");
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        verify_worker_candidate(import_project_ifc(bytes));
    }
}

void native_door_mechanisms() {
    using namespace sketch;
    for (const auto kind : {DoorOperationKind::double_hinged, DoorOperationKind::sliding}) {
        auto wall = make_document().snapshot().entities().at("wall-1");
        auto opening = make_document().snapshot().entities().at("opening-1");
        opening.properties["opening_kind"] = "door";
        opening.properties["opening_assembly"] = opening_assembly_json(default_opening_assembly(OpeningAssemblyKind::door));
        opening.properties["door_operation"] = encode_door_operation(
            DoorOperation{false, true, 90.0, kind, kind == DoorOperationKind::sliding ? 0.5 : 0.0});
        const auto exported = export_project_ifc(Document::create({wall, opening}).snapshot());
        bool found = false;
        for (const auto& [id, record] : records(exported.step)) {
            (void)id;
            if (record.type != "IFCDOOR") continue;
            found = true;
            check(record.fields[11] == (kind == DoorOperationKind::double_hinged
                ? ".DOUBLE_DOOR_SINGLE_SWING." : ".USERDEFINED."),
                "IFC mechanism must match manufactured multi-panel door");
            check(kind != DoorOperationKind::sliding || record.fields[12].find("fixed panel") != std::string::npos,
                "two-track fixed/moving door needs an accurate user-defined IFC description");
        }
        check(found, "multi-panel door emits an IFC fill");
        const auto imported = import_project_ifc(exported.step);
        const auto restored = std::find_if(imported.entities.begin(), imported.entities.end(),
            [](const Entity& entity) { return entity.type == "opening"; });
        check(restored != imported.entities.end() && restored->extensions.contains("ifc_fill_source") &&
                  restored->properties.at("door_operation") ==
                  opening.properties.at("door_operation"),
              "IFC verifies manufactured geometry and restores the full mechanism");
    }
}

void native_window_layouts() {
    using namespace sketch;
    const auto replace_record = [](std::string bytes, const std::string& id, const Record& record) {
        const auto start = bytes.find(id + "="), end = bytes.find(';', start);
        std::string row = id + "=" + record.type + "(";
        for (const auto& value : record.fields) { if (row.back() != '(') row += ','; row += value; }
        row += ");";
        bytes.replace(start, end - start + 1, row);
        return bytes;
    };
    const auto inactive = [](const std::string& bytes) {
        const auto imported = import_project_ifc(bytes);
        check(imported.source_retention_required && std::none_of(imported.entities.begin(), imported.entities.end(),
            [](const Entity& entity) { return entity.type == "opening" && entity.properties.contains("opening_assembly"); }),
            "contradictory window partition or fill geometry must not activate a native profile");
    };
    for (const auto layout : {WindowLayoutKind::fixed, WindowLayoutKind::double_fixed,
            WindowLayoutKind::triple_fixed, WindowLayoutKind::casement, WindowLayoutKind::sliding, WindowLayoutKind::bay}) {
        auto wall = make_document().snapshot().entities().at("wall-1");
        auto opening = make_document().snapshot().entities().at("opening-1");
        opening.properties["opening_kind"] = "window";
        auto profile = default_opening_assembly(OpeningAssemblyKind::window);
        profile.window_layout = layout;
        if (layout == WindowLayoutKind::casement) {
            profile.window_hinge_at_end = true;
            profile.window_open_left = false;
            profile.window_angle_degrees = 37.0;
        }
        if (layout == WindowLayoutKind::sliding) profile.window_slide_fraction = 0.35;
        if (layout == WindowLayoutKind::bay) {
            profile.window_bay_projection_m = 0.6;
            profile.window_bay_front_fraction = 0.5;
            profile.window_open_left = false;
            wall.properties["baseline"] = {{"start", {1, 2}}, {"end", {9, 8}}, {"sweep_radians", 0.0}};
        }
        opening.properties["opening_assembly"] = opening_assembly_json(profile);
        const auto exported = export_project_ifc(Document::create({wall, opening}).snapshot());
        const auto graph = records(exported.step);
        std::string fill_id;
        for (const auto& [id, record] : graph) if (record.type == "IFCWINDOW") fill_id = id;
        check(!fill_id.empty(), "each window layout must export a physical fill");
        const auto& fill = graph.at(fill_id);
        check(fill.fields[11] == (layout == WindowLayoutKind::bay ? ".USERDEFINED." : layout == WindowLayoutKind::triple_fixed ? ".TRIPLE_PANEL_VERTICAL." :
            layout == WindowLayoutKind::double_fixed || layout == WindowLayoutKind::sliding
            ? ".DOUBLE_PANEL_VERTICAL." : ".SINGLE_PANEL."),
            "IFC partition must describe actual side-by-side panel arrangement");
        check(fill.fields[12] == (layout == WindowLayoutKind::bay ? "'BAY_WINDOW'" : "$"),
            "bay partition label must truthfully identify a projecting multi-facet window");
        if (layout == WindowLayoutKind::bay) {
            auto wrong_label = fill;
            wrong_label.fields[12] = "'FLAT_WINDOW'";
            inactive(replace_record(exported.step, fill_id, wrong_label));
            wrong_label.fields[12] = "$";
            inactive(replace_record(exported.step, fill_id, wrong_label));
        }
        const auto imported = import_project_ifc(exported.step);
        verify_worker_candidate(imported);
        const auto restored = std::find_if(imported.entities.begin(), imported.entities.end(), [](const Entity& entity) {
            return entity.type == "opening" && entity.properties.contains("opening_assembly");
        });
        check(restored != imported.entities.end() && restored->extensions.contains("ifc_fill_source") &&
            restored->properties.at("opening_assembly") == opening.properties.at("opening_assembly"),
            "native IFC must verify actual fill mesh and preserve complete window profile");
        auto wrong_partition = fill;
        wrong_partition.fields[11] = layout == WindowLayoutKind::triple_fixed
            ? ".SINGLE_PANEL." : ".TRIPLE_PANEL_VERTICAL.";
        inactive(replace_record(exported.step, fill_id, wrong_partition));
        if (layout != WindowLayoutKind::fixed) {
            wrong_partition.fields[11] = ".NOTDEFINED.";
            inactive(replace_record(exported.step, fill_id, wrong_partition));
        }
        const auto& shape = graph.at(fill.fields[6]);
        const auto& representation = graph.at(list(shape.fields[2])[0]);
        if (layout == WindowLayoutKind::bay) {
            double projected_extent = 0.0;
            for (const auto& item : list(representation.fields[3]))
                for (const auto& coordinate : list(graph.at(graph.at(item).fields[0]).fields[0])) {
                    const auto point = list(coordinate);
                    projected_extent = std::max(projected_extent, -std::stod(point[1]));
                }
            check(projected_extent > wall.properties.at("thickness_m").get<double>() / 2.0 + 0.55,
                "bay IFC fill tessellation must physically project beyond the host face");
        }
        const auto& mesh = graph.at(list(representation.fields[3]).back());
        const auto points_id = mesh.fields[0];
        auto points = graph.at(points_id);
        const auto vertex_end = points.fields[0].find(',', 2);
        points.fields[0].replace(2, vertex_end - 2, "0.123456");
        inactive(replace_record(exported.step, points_id, points));
        if (layout == WindowLayoutKind::casement || layout == WindowLayoutKind::sliding || layout == WindowLayoutKind::bay) {
            // Both metadata copies agree, but the physical mesh still represents the original movement.
            const std::string key = layout == WindowLayoutKind::bay ? "window_bay_projection_m" : layout == WindowLayoutKind::casement ? "window_angle_degrees" : "window_slide_fraction";
            const std::string original = "\"" + key + "\":" + opening.properties.at("opening_assembly").at(key).dump();
            const std::string replacement = "\"" + key + "\":" + nlohmann::json(layout == WindowLayoutKind::bay ? 0.8 : layout == WindowLayoutKind::casement ? 63.0 : 0.65).dump();
            auto altered = exported.step;
            std::size_t count = 0, position = 0;
            while ((position = altered.find(original, position)) != std::string::npos) {
                altered.replace(position, original.size(), replacement);
                position += replacement.size();
                ++count;
            }
            check(count >= 2, "moving profile tamper must update both fill and void metadata copies");
            inactive(altered);
        }
        if (layout == WindowLayoutKind::fixed) {
            // Previous native exports used NOTDEFINED with the exact v1 fixed profile.
            auto legacy_profile = opening.properties.at("opening_assembly");
            legacy_profile["version"] = 1;
            for (const auto* key : {"window_layout", "window_hinge_at_end", "window_open_left", "window_angle_degrees", "window_slide_fraction"})
                legacy_profile.erase(key);
            opening.properties["opening_assembly"] = legacy_profile;
            const auto legacy_export = export_project_ifc(Document::create({wall, opening}).snapshot());
            const auto legacy_graph = records(legacy_export.step);
            std::string legacy_id;
            for (const auto& [id, record] : legacy_graph) if (record.type == "IFCWINDOW") legacy_id = id;
            auto legacy_fill = legacy_graph.at(legacy_id);
            legacy_fill.fields[11] = ".NOTDEFINED.";
            const auto legacy_import = import_project_ifc(replace_record(legacy_export.step, legacy_id, legacy_fill));
            check(std::any_of(legacy_import.entities.begin(), legacy_import.entities.end(), [&](const Entity& entity) {
                return entity.type == "opening" && entity.properties.contains("opening_assembly") &&
                    entity.properties.at("opening_assembly") == legacy_profile;
            }), "proven legacy v1 fixed window must remain editable after import");
        }
    }
}

void native_historic_fixed_windows() {
    using namespace sketch;
    for (const auto* name : {"straight.ifc", "curved-cw.ifc", "curved-ccw.ifc"}) {
        const auto path = std::filesystem::path(VERTEX_IFC_LEGACY_WINDOW_FIXTURES) / name;
        std::ifstream input(path, std::ios::binary);
        check(input.good(), "unchanged historic window IFC fixture must be readable");
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto imported = import_project_ifc(bytes);
        verify_worker_candidate(imported);
        check(std::count_if(imported.entities.begin(), imported.entities.end(), [](const Entity& entity) {
            return entity.type == "opening" && entity.extensions.contains("ifc_fill_source") &&
                entity.properties.contains("opening_assembly") &&
                entity.properties.at("opening_assembly").at("version") == 1 &&
                entity.properties.at("opening_assembly").at("kind") == "window";
        }) == 1, "unchanged pre-layout IFC window must restore an editable verified native assembly");
    }
}

void native_assemblies() {
    using namespace sketch;
    for (const double sweep : {0.0, 1.0, -1.0}) {
        const bool curved = sweep != 0;
        for (const bool window : {false, true}) {
            for (const bool hinge_end : {false, true}) {
              for (const bool swing_left : {false, true}) {
                auto wall = make_document().snapshot().entities().at("wall-1");
                auto opening = make_document().snapshot().entities().at("opening-1");
                wall.properties["elevation_m"] = 3.0;
                wall.properties["baseline"]["start"] = {5, -3};
                wall.properties["baseline"]["end"] = {13, 3};
                wall.properties["baseline"]["sweep_radians"] = sweep;
                opening.properties["opening_kind"] = window ? "window" : "door";
                opening.properties["opening_assembly"]["kind"] = window ? "window" : "door";
                opening.properties["opening_assembly"]["glazing_thickness_m"] = 0.012;
                opening.properties["opening_assembly"]["inset_m"] = -0.015;
                if (!window) opening.properties["door_operation"] =
                    encode_door_operation(DoorOperation{hinge_end, swing_left, 67.0});
                const auto exported = export_project_ifc(Document::create({wall, opening}).snapshot());
                const auto graph = records(exported.step);
                const double radius=curved ? 5/std::sin(0.5) : 0;
                const double overall_width=curved ? 2*(radius+0.1)*std::sin(1/(2*radius)) : 1.0;
                std::string void_id, fill_id, host_id;
                bool large_aggregate = false;
                for (const auto& [id, record] : graph) {
                    if (record.type == "IFCWALL") host_id = id;
                    if (record.type == "IFCOPENINGELEMENT") void_id = id;
                    if (record.type == (window ? "IFCWINDOW" : "IFCDOOR")) {
                        fill_id = id;
                        check(record.fields.size() == 13, "IFC4 fill must contain exactly 13 explicit attributes");
                        check(std::stod(record.fields[8]) == 2.0 && std::abs(std::stod(record.fields[9])-overall_width)<1e-9,
                              "IFC fill width must match opening-body local X envelope rather than curved arc stations");
                        if (curved) check(std::abs(std::stod(record.fields[9])-1.0)>0.001,
                              "curved standard fill width must differ from retained one-metre arc station width");
                        if (!window) check(record.fields[11] ==
                            (hinge_end == swing_left ? ".SINGLE_SWING_RIGHT." : ".SINGLE_SWING_LEFT."),
                            "both native handings must map to corresponding IFC door operations");
                        const auto& placement = graph.at(record.fields[5]);
                        check(placement.type == "IFCLOCALPLACEMENT" && placement.fields[0] == "$",
                              "fill must declare an explicit absolute local placement");
                        const auto& axis = graph.at(placement.fields[1]);
                        check(axis.type == "IFCAXIS2PLACEMENT3D" && axis.fields[1] != "$" && axis.fields[2] != "$",
                              "fill placement must declare width and up directions");
                        const auto location = list(graph.at(axis.fields[0]).fields[0]);
                        const auto up = list(graph.at(axis.fields[1]).fields[0]);
                        const auto x = list(graph.at(axis.fields[2]).fields[0]);
                        const Segment baseline{{5,-3},{13,3},sweep};
                        // Independently evaluate the fixture's jamb stations;
                        // the production opening-span helper is not the oracle.
                        const auto station = [sweep](double distance) -> Vec2 {
                            if (sweep==0) return {5+0.8*distance,-3+0.6*distance};
                            const double factor=0.5/std::tan(sweep/2);
                            const Vec2 center{9-6*factor,8*factor};
                            const double radius=5/std::abs(std::sin(sweep/2));
                            const double angle=(sweep>0 ? 1.0 : -1.0)*distance/radius;
                            const double dx=5-center.x,dy=-3-center.y;
                            return {center.x+dx*std::cos(angle)-dy*std::sin(angle),
                                    center.y+dx*std::sin(angle)+dy*std::cos(angle)};
                        };
                        const Segment span{station(1),station(2),0};
                        const bool reverse = !window && !swing_left;
                        const auto origin = reverse ? span.end : span.start;
                        const double sign = reverse ? -1.0 : 1.0;
                        const double chord = std::hypot(span.end.x-span.start.x,span.end.y-span.start.y);
                        check(std::abs(std::stod(location[0])-origin.x)<1e-9 &&
                              std::abs(std::stod(location[1])-origin.y)<1e-9 &&
                              std::abs(std::stod(location[2])-3.1)<1e-9,
                              "fill origin must be its local left jamb at the opening sill");
                        check(std::stod(up[0])==0 && std::stod(up[1])==0 && std::stod(up[2])==1 &&
                              std::abs(std::stod(x[0])-sign*(span.end.x-span.start.x)/chord)<1e-9 &&
                              std::abs(std::stod(x[1])-sign*(span.end.y-span.start.y)/chord)<1e-9 && std::stod(x[2])==0,
                              "fill positive width axis must follow the jamb chord with right-handed swing-side Y");
                        if (!window) {
                            const auto symbol = door_plan_symbol(baseline,1.0,1.0,{hinge_end,swing_left,67.0});
                            const auto hinge = symbol.front().start, tip = symbol.front().end;
                            const double hx=(hinge.x-origin.x)*std::stod(x[0])+(hinge.y-origin.y)*std::stod(x[1]);
                            const double ty=-(tip.x-hinge.x)*std::stod(x[1])+(tip.y-hinge.y)*std::stod(x[0]);
                            check(ty>0 && ((record.fields[11]==".SINGLE_SWING_LEFT." && std::abs(hx)<1e-9) ||
                                  (record.fields[11]==".SINGLE_SWING_RIGHT." && std::abs(hx-chord)<1e-9)),
                                  "IFC operation enum and positive-Y swing must agree with physical hinge and leaf");
                        }
                    }
                    if (record.type == "IFCCARTESIANPOINTLIST3D")
                        large_aggregate = large_aggregate || record.fields[0].size() > 4096;
                    if (record.type != "IFCTRIANGULATEDFACESET") continue;
                    check(record.fields.size() == 5 && record.fields[2] == ".T.",
                          "native mesh must use the IFC4 closed triangulated schema");
                    const auto coordinates = list(graph.at(record.fields[0]).fields[0]);
                    std::vector<std::array<double, 3>> vertices;
                    for (const auto& coordinate : coordinates) {
                        const auto point = list(coordinate);
                        vertices.push_back({std::stod(point[0]), std::stod(point[1]), std::stod(point[2])});
                    }
                    std::map<std::pair<std::size_t, std::size_t>, std::pair<int, int>> edges;
                    double volume = 0;
                    for (const auto& row : list(record.fields[3])) {
                        const auto columns = list(row);
                        const std::array<std::size_t, 3> triangle{std::stoull(columns[0]) - 1,
                            std::stoull(columns[1]) - 1, std::stoull(columns[2]) - 1};
                        const auto& a = vertices.at(triangle[0]);
                        const auto& b = vertices.at(triangle[1]);
                        const auto& c = vertices.at(triangle[2]);
                        volume += (a[0] * (b[1]*c[2]-b[2]*c[1]) +
                            a[1] * (b[2]*c[0]-b[0]*c[2]) + a[2] * (b[0]*c[1]-b[1]*c[0])) / 6;
                        for (std::size_t i = 0; i < 3; ++i) {
                            const auto first = triangle[i], second = triangle[(i+1)%3];
                            auto& edge = edges[{std::min(first, second), std::max(first, second)}];
                            ++edge.first; edge.second += first < second ? 1 : -1;
                        }
                    }
                    check(volume > 1e-8, "every native part must enclose a positive physical volume");
                    for (const auto& [key, edge] : edges)
                        check(edge.first == 2 && edge.second == 0, "closed meshes require opposite shared edge incidence");
                }
                check(!void_id.empty() && !fill_id.empty() && !host_id.empty(), "fill must preserve separate host and void products");
                const auto replace_fields = [](std::string bytes, const std::string& id, const Record& record) {
                    const auto begin=bytes.find(id+"="), end=bytes.find(';',begin);
                    std::string row=id+"="+record.type+"(";
                    for (const auto& value : record.fields) { if (row.back()!='(') row+=','; row+=value; }
                    row+=");";
                    bytes.replace(begin,end-begin+1,row);
                    return bytes;
                };
                const auto real=[](double value) {
                    std::ostringstream output; output.precision(17); output.setf(std::ios::showpoint);
                    output<<value; return output.str();
                };
                const auto rejected_fill = [](const std::string& bytes) {
                    const auto imported=import_project_ifc(bytes);
                    check(std::none_of(imported.entities.begin(),imported.entities.end(),[](const auto& entity) {
                        return entity.type=="opening" && entity.properties.contains("opening_assembly");
                    }) && imported.source_retention_required,
                    "placement, geometry or handing contradiction must leave native assembly inactive");
                };
                const auto& placement=graph.at(graph.at(fill_id).fields[5]);
                const auto axis_id=placement.fields[1];
                const auto& axis=graph.at(axis_id);
                const auto location=list(graph.at(axis.fields[0]).fields[0]);
                const auto x=list(graph.at(axis.fields[2]).fields[0]);
                const double xx=std::stod(x[0]),xy=std::stod(x[1]);
                if (curved) {
                    const auto& void_shape=graph.at(graph.at(void_id).fields[6]);
                    const auto& void_rep=graph.at(list(void_shape.fields[2])[0]);
                    double minimum=1e9,maximum=-1e9;
                    for (const auto& item : list(void_rep.fields[3]))
                        for (const auto& coordinate : list(graph.at(graph.at(item).fields[0]).fields[0])) {
                            const auto point=list(coordinate);
                            const double local_x=(std::stod(point[0])-std::stod(location[0]))*xx+
                                (std::stod(point[1])-std::stod(location[1]))*xy;
                            minimum=std::min(minimum,local_x); maximum=std::max(maximum,local_x);
                        }
                    check(std::abs(maximum-minimum-overall_width)<1e-9,
                          "independent actual void mesh local X bounds must agree with standard fill OverallWidth");
                    auto arc_width=graph.at(fill_id); arc_width.fields[9]="1.";
                    rejected_fill(replace_fields(exported.step,fill_id,arc_width));
                }
                const auto& fill_shape=graph.at(graph.at(fill_id).fields[6]);
                const auto& fill_rep=graph.at(list(fill_shape.fields[2])[0]);
                const auto items=list(fill_rep.fields[3]);
                const Wall native_host{wall.id,{{5,-3},{13,3},sweep},0.2,2.5,3.0};
                const HostedOpening native_cut{opening.id,1.0,1.0,0.1,2.0};
                const auto expected=ifc_native_fill_mesh(native_host,native_cut,
                    parse_opening_assembly(opening.properties.at("opening_assembly")),
                    window ? std::optional<DoorOperation>{} : std::optional<DoorOperation>{{hinge_end,swing_left,67.0}},
                    IfcExchangeLimits{}.max_mesh_vertices,IfcExchangeLimits{}.max_mesh_triangles);
                check(items.size()==expected.size(),"local export must preserve every actual native solid");
                for (std::size_t i=0;i<items.size();++i) {
                    const auto coordinates=list(graph.at(graph.at(items[i]).fields[0]).fields[0]);
                    check(coordinates.size()==expected[i].vertices.size(),"local export must preserve native tessellation vertices");
                    for (std::size_t j=0;j<coordinates.size();++j) {
                        const auto point=list(coordinates[j]);
                        const double px=std::stod(point[0]),py=std::stod(point[1]);
                        const std::array<double,3> world{std::stod(location[0])+xx*px-xy*py,
                            std::stod(location[1])+xy*px+xx*py,std::stod(location[2])+std::stod(point[2])};
                        for (std::size_t k=0;k<3;++k)
                            check(std::abs(world[k]-expected[i].vertices[j][k])<1e-8,
                                  "local placement must preserve native curved frame and actual swung leaf world geometry");
                    }
                }
                if (!window) {
                    auto wrong_enum=graph.at(fill_id);
                    wrong_enum.fields[11]=wrong_enum.fields[11]==".SINGLE_SWING_LEFT." ? ".SINGLE_SWING_RIGHT." : ".SINGLE_SWING_LEFT.";
                    rejected_fill(replace_fields(exported.step,fill_id,wrong_enum));
                }
                if (!window && !curved && !hinge_end && swing_left) {
                    auto wrong_origin=graph.at(axis.fields[0]);
                    wrong_origin.fields[0]="("+real(std::stod(location[0])+0.125)+","+location[1]+","+location[2]+")";
                    const auto translated=replace_fields(exported.step,axis.fields[0],wrong_origin);
                    rejected_fill(translated);
                    // Even compensating every local vertex to preserve world
                    // shape must not bless an incorrect standard door frame.
                    auto compensated=translated;
                    for (const auto& item : items) {
                        const auto points_id=graph.at(item).fields[0];
                        auto points=graph.at(points_id);
                        std::string coordinates="(";
                        for (const auto& coordinate : list(points.fields[0])) {
                            const auto point=list(coordinate);
                            if (coordinates.size()>1) coordinates+=',';
                            coordinates+="("+real(std::stod(point[0])-xx*0.125)+","+
                                real(std::stod(point[1])+xy*0.125)+","+point[2]+")";
                        }
                        coordinates+=')'; points.fields[0]=coordinates;
                        compensated=replace_fields(compensated,points_id,points);
                    }
                    rejected_fill(compensated);
                    for (const auto& direction : {std::string("(0.,0.,0.)"), std::string("(1.6,1.2,0.)"),
                         std::string("(-0.8,-0.6,0.)"),std::string("(0.8,0.6,0.1)")}) {
                        auto wrong_x=graph.at(axis.fields[2]); wrong_x.fields[0]=direction;
                        rejected_fill(replace_fields(exported.step,axis.fields[2],wrong_x));
                    }
                    auto wrong_axis=axis; wrong_axis.fields[1]="#999975";
                    auto tilted=replace_fields(exported.step,axis_id,wrong_axis);
                    tilted.insert(tilted.find("ENDSEC;\nEND-ISO-10303-21;"),"#999975=IFCDIRECTION((0.,0.,-1.));\n");
                    rejected_fill(tilted);
                    auto partial_axis=axis; partial_axis.fields[1]="$";
                    rejected_fill(replace_fields(exported.step,axis_id,partial_axis));
                    const auto points_id=graph.at(items.front()).fields[0];
                    auto wrong_points=graph.at(points_id);
                    const auto vertex_end=wrong_points.fields[0].find(',',2);
                    wrong_points.fields[0].replace(2,vertex_end-2,"0.123456");
                    rejected_fill(replace_fields(exported.step,points_id,wrong_points));
                }
                // Equivalent parent +90 degree rotation and translation must
                // compose before native comparison, for all host/handing cases.
                const auto absolute=import_project_ifc(exported.step);
                check(std::any_of(absolute.entities.begin(),absolute.entities.end(),[](const auto& entity) {
                    return entity.type=="opening" && entity.properties.contains("opening_assembly");
                }),"absolute oblique fill must reconstruct before equivalent parent placement is tested");
                auto relative_placement=placement; relative_placement.fields[0]="#999974";
                auto relative=replace_fields(exported.step,graph.at(fill_id).fields[5],relative_placement);
                auto relative_origin=graph.at(axis.fields[0]);
                relative_origin.fields[0]="("+real(std::stod(location[1])+5)+","+
                    real(2-std::stod(location[0]))+","+real(std::stod(location[2])-1)+")";
                relative=replace_fields(relative,axis.fields[0],relative_origin);
                auto relative_x=graph.at(axis.fields[2]);
                relative_x.fields[0]="("+x[1]+","+real(-xx)+",0.)";
                relative=replace_fields(relative,axis.fields[2],relative_x);
                relative.insert(relative.find("ENDSEC;\nEND-ISO-10303-21;"),
                    "#999970=IFCCARTESIANPOINT((2.,-5.,1.));\n#999971=IFCDIRECTION((0.,0.,1.));\n"
                    "#999972=IFCDIRECTION((0.,1.,0.));\n#999973=IFCAXIS2PLACEMENT3D(#999970,#999971,#999972);\n"
                    "#999974=IFCLOCALPLACEMENT($,#999973);\n");
                const auto composed=import_project_ifc(relative);
                check(std::any_of(composed.entities.begin(),composed.entities.end(),[](const auto& entity) {
                    return entity.type=="opening" && entity.properties.contains("opening_assembly");
                }),"proper parent placement rotation and translation must reconstruct the same native fill");
                bool fills = false, voids = false;
                for (const auto& [id, record] : graph) {
                    if (record.type == "IFCRELFILLSELEMENT") fills = record.fields[4] == void_id && record.fields[5] == fill_id;
                    if (record.type == "IFCRELVOIDSELEMENT") voids = record.fields[4] == host_id && record.fields[5] == void_id;
                }
                check(fills && voids, "native host/void/fill relationships must remain connected");
                if (curved) check(large_aggregate, "curved roundtrip fixture must exceed scalar string aggregate limit");
                const auto imported = import_project_ifc(exported.step);
                verify_worker_candidate(imported);
                check(imported.entities.size() == 2, "validated fill must not produce a duplicate generic product");
                const auto imported_wall = std::find_if(imported.entities.begin(), imported.entities.end(),
                    [](const auto& entity) { return entity.type == "wall"; });
                const auto imported_opening = std::find_if(imported.entities.begin(), imported.entities.end(),
                    [](const auto& entity) { return entity.type == "opening"; });
                check(imported_wall != imported.entities.end() && imported_opening != imported.entities.end(),
                      "native curved/straight bodies must reconstruct an editable hosted graph");
                check(imported_wall->properties.at("baseline") == wall.properties.at("baseline") &&
                      imported_opening->properties.at("width_m")==opening.properties.at("width_m") &&
                      imported_opening->properties.at("opening_assembly") == opening.properties.at("opening_assembly") &&
                      imported_opening->properties.at("opening_kind") == opening.properties.at("opening_kind"),
                      "exact native baseline and signed assembly profile must survive roundtrip");
                if (!window) check(imported_opening->properties.at("door_operation") == opening.properties.at("door_operation"),
                    "both handings and exact swing angle must survive roundtrip");
                const auto saved_graph = Document::create(imported.entities);
                const auto second_import = import_project_ifc(export_project_ifc(saved_graph.snapshot()).step);
                (void)Document::create(second_import.entities);
                const auto capture_name = std::string(curved ? (sweep>0 ? "curved-ccw-" : "curved-cw-") : "straight-") +
                    (window ? "window-" : "door-") + (hinge_end ? "end-" : "start-") + (swing_left ? "left" : "right");
                if (const auto* directory = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
                    std::filesystem::create_directories(directory);
                    std::ofstream output(std::filesystem::path(directory) / (capture_name + ".ifc"), std::ios::binary);
                    output << exported.step;
                    check(output.good(), "capture export must be written completely");
                }
                auto contradictory = exported.step;
                const auto fill_start = contradictory.find(fill_id + "=");
                const auto fill_end = contradictory.find(';', fill_start);
                auto replacement = graph.at(fill_id).fields;
                replacement[8] = "9.";
                std::string row = fill_id + "=" + graph.at(fill_id).type + "(";
                for (const auto& field : replacement) { if (row.back() != '(') row += ','; row += field; }
                row += ");";
                contradictory.replace(fill_start, fill_end - fill_start + 1, row);
                const auto rejected = import_project_ifc(contradictory);
                check(std::none_of(rejected.entities.begin(), rejected.entities.end(), [](const auto& entity) {
                    return entity.type == "opening" && entity.properties.contains("opening_assembly");
                }) && rejected.source_retention_required, "contradictory IFC fill dimensions must not activate native assembly metadata");
                auto transformed_context = exported.step;
                std::string context_id;
                for (const auto& [id, record] : graph)
                    if (record.type == "IFCGEOMETRICREPRESENTATIONCONTEXT") context_id = id;
                const auto context_start = transformed_context.find(context_id + "=");
                const auto context_end = transformed_context.find(';', context_start);
                auto context_fields = graph.at(context_id).fields;
                context_fields[4] = "#999997";
                std::string context_row = context_id + "=IFCGEOMETRICREPRESENTATIONCONTEXT(";
                for (const auto& field : context_fields) { if (context_row.back() != '(') context_row += ','; context_row += field; }
                context_row += ");";
                transformed_context.replace(context_start, context_end-context_start+1, context_row);
                transformed_context.insert(transformed_context.find("ENDSEC;\nEND-ISO-10303-21;"),
                    "#999996=IFCCARTESIANPOINT((20.,0.,0.));\n#999997=IFCAXIS2PLACEMENT3D(#999996,$,$);\n");
                const auto transformed = import_project_ifc(transformed_context);
                check(std::none_of(transformed.entities.begin(), transformed.entities.end(), [](const auto& entity) {
                    return entity.type == "opening" && entity.properties.contains("opening_assembly");
                }) && transformed.source_retention_required, "context-only translation must not activate world-coordinate native meshes");
                IfcExchangeLimits small;
                small.max_mesh_vertices = 4;
                bool bounded = false;
                try { (void)export_project_ifc(Document::create({wall, opening}).snapshot(), small); }
                catch (const std::invalid_argument&) { bounded = true; }
                check(bounded, "insufficient native tessellation budget must fail closed");
              }
            }
        }
    }
}

void closed_leaf_without_operation() {
    using namespace sketch;
    for (const double sweep : {0.0,1.0,-1.0}) {
        auto wall=make_document().snapshot().entities().at("wall-1");
        auto opening=make_document().snapshot().entities().at("opening-1");
        wall.properties["baseline"]={{"start",{5,-3}},{"end",{13,3}},{"sweep_radians",sweep}};
        wall.properties["elevation_m"]=3.0;
        check(!opening.properties.contains("door_operation"),"closed-leaf fixture must have unspecified operation");
        const auto exported=export_project_ifc(Document::create({wall,opening}).snapshot());
        const auto graph=records(exported.step);
        std::string fill_id;
        for (const auto& [id,record] : graph) {
            if (record.type!="IFCDOOR") continue;
            fill_id=id;
            check(record.fields[11]==".USERDEFINED." &&
                  record.fields[12]=="'Closed leaf; hinge and swing unspecified'",
                  "closed leaf with unknown handing must use IFC user-defined operation with explicit description");
            const auto& shape=graph.at(record.fields[6]);
            const auto& representation=graph.at(list(shape.fields[2])[0]);
            check(list(representation.fields[3]).size()>=4,
                  "unspecified operation must preserve the physical leaf as well as the frame solids");
        }
        check(!fill_id.empty(),"closed leaf must export an actual IFC door");
        const auto imported=import_project_ifc(exported.step);
        check(std::any_of(imported.entities.begin(),imported.entities.end(),[](const auto& entity) {
            return entity.type=="opening" && entity.properties.contains("opening_assembly") &&
                !entity.properties.contains("door_operation");
        }),"closed native profile must roundtrip without inventing hinge or swing metadata");
        verify_worker_candidate(imported);
        const auto repeated=import_project_ifc(export_project_ifc(Document::create(imported.entities).snapshot()).step);
        check(std::any_of(repeated.entities.begin(),repeated.entities.end(),[](const auto& entity) {
            return entity.type=="opening" && entity.properties.contains("opening_assembly") &&
                !entity.properties.contains("door_operation");
        }),"closed leaf operation must remain unspecified after repeated roundtrip");
        if (const auto* directory=std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
            std::filesystem::create_directories(directory);
            const auto name=std::string("closed-door-")+(sweep==0 ? "straight" : sweep>0 ? "ccw" : "cw")+".ifc";
            std::ofstream output(std::filesystem::path(directory)/name,std::ios::binary);
            output<<exported.step;
            check(output.good(),"closed leaf external qualification capture must be written completely");
        }
        for (const auto& replacement : {std::string("$"),std::string("'Different operation'")}) {
            auto altered=exported.step;
            const auto start=altered.find(fill_id+"="),end=altered.find(");\n",start)+1;
            auto record=graph.at(fill_id); record.fields[12]=replacement;
            std::string row=fill_id+"=IFCDOOR(";
            for (const auto& value : record.fields) { if (row.back()!='(') row+=','; row+=value; }
            row+=");"; altered.replace(start,end-start+1,row);
            const auto rejected=import_project_ifc(altered);
            check(std::none_of(rejected.entities.begin(),rejected.entities.end(),[](const auto& entity) {
                return entity.type=="opening" && entity.properties.contains("opening_assembly");
            }) && rejected.source_retention_required,
            "contradictory user-defined closed-leaf description must not activate native assembly");
        }
    }
}
#endif

void run() {
    using namespace sketch;
    const auto exported = export_project_ifc(make_document().snapshot());
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    check(exported.step.find("IFCDOOR(") != std::string::npos &&
          exported.step.find("IFCRELFILLSELEMENT(") != std::string::npos &&
          exported.step.find("IFCTRIANGULATEDFACESET(") != std::string::npos,
          "native door assembly must export a real fill separate from its hosted void");
#endif
    verify_export_graph(exported.step);
    const auto repeated = export_project_ifc(make_document().snapshot());
    check(repeated.step == exported.step && repeated.diagnostics == exported.diagnostics,
          "equivalent snapshots must export deterministically");
    auto layered_wall = make_document().snapshot().entities().at("wall-1");
    const Entity catalog{"catalog-a", "assembly_model", {{"version",1},
        {"model", AssemblyModel::create({{"brick", "Brick"}}, {}, {}).to_json()}}};
    layered_wall.properties["layers"] = nlohmann::json::array({
        {{"id", "outer"}, {"thickness_m", 0.05}, {"material_assignment",
            {{"version", 1}, {"catalog_id", "catalog-a"}, {"material_id", "brick"}}}},
        {{"id", "core"}, {"thickness_m", 0.15}}});
    const auto layered = export_project_ifc(Document::create({layered_wall, catalog}).snapshot());
    check(std::any_of(layered.diagnostics.begin(), layered.diagnostics.end(), [](const auto& item) {
        return item.source_id == "wall-1" &&
               item.code == "wall_layer_placement_not_exported";
    }), "direct IFC layer-set association must disclose omitted occurrence placement");
    verify_export_graph(layered.step);
    const auto graph = records(layered.step);
    std::string wall_id, type_id, set_id;
    for (const auto& [id, record] : graph) {
        if (record.type == "IFCWALL") wall_id = id;
        if (record.type == "IFCWALLTYPE") type_id = id;
        if (record.type == "IFCMATERIALLAYERSET") {
            set_id = id;
            const auto layers = list(record.fields[0]);
            check(layers.size() == 2, "native layer order and count must survive IFC export");
            const auto& first = graph.at(layers[0]);
            const auto& second = graph.at(layers[1]);
            check(std::abs(std::stod(first.fields[1]) + std::stod(second.fields[1]) - 0.2) < 1e-9,
                  "IFC material layers must total the geometric wall thickness");
            check(graph.at(first.fields[0]).fields[0] == "'brick'" && second.fields[0] == "$",
                  "known materials must map; unassigned layer materials must remain unknown");
        }
    }
    bool typed = false, associated = false;
    for (const auto& [id, record] : graph) {
        if (record.type == "IFCRELDEFINESBYTYPE")
            typed = list(record.fields[4]) == std::vector<std::string>{wall_id} && record.fields[5] == type_id;
        if (record.type == "IFCRELASSOCIATESMATERIAL")
            associated = list(record.fields[4]) == std::vector<std::string>{wall_id, type_id} && record.fields[5] == set_id;
    }
    check(typed && associated, "wall occurrence, type and layer set must form connected IFC relationships");
    const auto layered_import = import_project_ifc(layered.step);
    const auto imported_wall = std::find_if(layered_import.entities.begin(), layered_import.entities.end(),
        [](const auto& entity) { return entity.type == "wall"; });
    check(imported_wall != layered_import.entities.end() &&
          imported_wall->properties.at("layers").size() == 2 &&
          !imported_wall->properties.at("layers")[0].contains("material_assignment") &&
          imported_wall->extensions.at("ifc_vertex_properties").at("layers") == layered_wall.properties.at("layers") &&
          layered_import.source_retention_required,
          "editable layer dimensions must round-trip; unresolved catalog identities must remain retained");
    (void)Document::create(layered_import.entities);
    auto homogeneous_wall = layered_wall;
    homogeneous_wall.properties["material_assignment"] = layered_wall.properties.at("layers")[0].at("material_assignment");
    homogeneous_wall.properties.erase("layers");
    const auto homogeneous = export_project_ifc(Document::create({homogeneous_wall, catalog}).snapshot());
    const auto homogeneous_graph = records(homogeneous.step);
    bool homogeneous_material = false;
    for (const auto& [id, record] : homogeneous_graph) {
        if (record.type == "IFCRELASSOCIATESMATERIAL")
            homogeneous_material = homogeneous_graph.at(record.fields[5]).type == "IFCMATERIAL";
    }
    check(homogeneous_material, "homogeneous native material assignments must use a standard IFC association");

    auto oblique_wall = layered_wall;
    oblique_wall.properties["baseline"]["start"] = {1, 2};
    oblique_wall.properties["baseline"]["end"] = {4, 6};
    oblique_wall.properties["elevation_m"] = 3.0;
    const auto oblique_export = export_project_ifc(Document::create({oblique_wall, catalog}).snapshot());
    check(std::any_of(oblique_export.diagnostics.begin(), oblique_export.diagnostics.end(), [](const auto& item) {
        return item.source_id == "wall-1" &&
               item.code == "wall_layer_placement_not_exported";
    }), "oblique asymmetric layers must retain the placement-fidelity diagnostic");
    const auto oblique = import_project_ifc(oblique_export.step);
    const auto oblique_imported_wall = std::find_if(oblique.entities.begin(), oblique.entities.end(),
        [](const auto& entity) { return entity.type == "wall"; });
    check(oblique_imported_wall != oblique.entities.end() &&
          oblique_imported_wall->properties.at("baseline") == oblique_wall.properties.at("baseline") &&
          oblique_imported_wall->properties.at("elevation_m") == 3.0,
          "oblique elevated bodies must preserve baseline and elevation");

    auto sloped_wall = layered_wall;
    sloped_wall.properties.erase("layers");
    sloped_wall.required = true;
    sloped_wall.properties["slope_rise_m"] = 1.0;
    sloped_wall.extensions["source_note"] = std::string(1500, 'p');
    const auto sloped = export_project_ifc(Document::create({sloped_wall}).snapshot());
    check(sloped.step.find("IFCWALL(") == std::string::npos &&
          std::any_of(sloped.diagnostics.begin(), sloped.diagnostics.end(), [](const auto& item) {
              return item.code == "native_reference_only";
          }), "unsupported sloped wall must not silently become a flat wall");
    const auto reference_import = import_project_ifc(sloped.step);
    check(reference_import.entities.size() == 1 && reference_import.entities[0].type == "ifc_reference" &&
          reference_import.entities[0].extensions.at("ifc_vertex_properties").at("native_entity").at("properties") == sloped_wall.properties &&
          reference_import.entities[0].extensions.at("ifc_vertex_properties").at("native_entity").at("extensions") == sloped_wall.extensions &&
          reference_import.source_retention_required, "unsupported native semantics must remain identifiable and retained");
    const auto repeated_reference = export_project_ifc(Document::create(reference_import.entities).snapshot());
    check(std::none_of(repeated_reference.diagnostics.begin(), repeated_reference.diagnostics.end(),
        [](const auto& item) { return item.code == "vertex_properties_not_exported"; }),
        "unchanged reference payload must remain below the retention limit on re-export");
    const auto repeated_reference_import = import_project_ifc(repeated_reference.step);
    check(repeated_reference_import.entities.size() == 1 &&
          repeated_reference_import.entities[0].type == "ifc_reference" &&
          repeated_reference_import.entities[0].extensions.at("ifc_vertex_properties") ==
              reference_import.entities[0].extensions.at("ifc_vertex_properties"),
          "reference-only native payload must remain byte-semantically stable across repeated round trips");
    const auto hosted_reference = export_project_ifc(Document::create({sloped_wall,
        make_document().snapshot().entities().at("opening-1")}).snapshot());
    check(hosted_reference.step.find("IFCOPENINGELEMENT(") == std::string::npos &&
          hosted_reference.step.find("IFCRELVOIDSELEMENT(") == std::string::npos,
          "unsupported hosts must not leave orphan IFC opening features");
    check(import_project_ifc(hosted_reference.step).entities.size() == 2,
          "unsupported host and opening must both retain native references");

    const auto old_axis = import_project_ifc(
        "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
        "#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);\n"
        "#2=IFCCARTESIANPOINT((0.,0.,0.));\n#3=IFCCARTESIANPOINT((4.,0.,0.));\n"
        "#4=IFCPOLYLINE((#2,#3));\n#5=IFCSHAPEREPRESENTATION($,'Axis','Curve2D',(#4));\n"
        "#6=IFCPRODUCTDEFINITIONSHAPE($,$,(#5));\n"
        "#7=IFCWALLSTANDARDCASE('legacy',$,'legacy',$,$,$,#6,$);\n"
        "#8=IFCPROPERTYSINGLEVALUE('Properties',$,IFCTEXT('{\"thickness_m\":0.2,\"height_m\":2.5}'),$);\n"
        "#9=IFCPROPERTYSET('pset',$,'Pset_VertexExchange_v1',$,(#8));\n"
        "#10=IFCRELDEFINESBYPROPERTIES('link',$,$,$,(#7),#9);\nENDSEC;\nEND-ISO-10303-21;\n");
    check(old_axis.entities.size() == 1 && old_axis.entities[0].type == "wall" &&
          old_axis.entities[0].properties.at("baseline").at("end") == nlohmann::json({4,0}),
          "legacy axis-only native IFC exports must remain editable on import");

    auto inconsistent_body = layered.step;
    const auto extrusion = inconsistent_body.find("=IFCEXTRUDEDAREASOLID");
    const auto end_solid = inconsistent_body.find(");", extrusion);
    const auto last_comma = inconsistent_body.rfind(',', end_solid);
    inconsistent_body.replace(last_comma + 1, end_solid - last_comma - 1, "8.");
    const auto inconsistent = import_project_ifc(inconsistent_body);
    check(std::none_of(inconsistent.entities.begin(), inconsistent.entities.end(), [](const auto& entity) {
              return entity.type == "wall";
          }) &&
          inconsistent.source_retention_required,
          "native metadata must not override contradictory wall body geometry");
    check(exported.step.find("IFCPROJECT(") != std::string::npos &&
          exported.step.find("IFCSITE(") != std::string::npos &&
          exported.step.find("IFCBUILDING(") != std::string::npos &&
          exported.step.find("IFCBUILDINGSTOREY(") != std::string::npos,
          "export must provide a project/site/building/storey hierarchy");
    check(exported.step.find("ISO-10303-21;") == 0, "IFC export must have a STEP envelope");
    check(exported.step.find("FILE_SCHEMA(('IFC4'))") != std::string::npos,
          "IFC export must declare IFC4");
    check(exported.step.find("IFCWALL(") != std::string::npos,
          "wall body must export as an IFC wall product");
    check(exported.step.find("IFCSLAB") != std::string::npos,
          "slab footprint must export as an IFC slab product");
    check(exported.step.find("IFCOPENINGELEMENT") != std::string::npos,
          "hosted opening must export as an IFC opening product");
    check(exported.step.find("IFCRELVOIDSELEMENT") != std::string::npos,
          "hosted opening must retain an IFC wall void relationship");
    check(exported.step.find("(0.,0.,0.10000000000000001)") != std::string::npos,
          "opening sill must be represented by a local IFC placement");
    check(exported.step.find("IFCEXTRUDEDAREASOLID") != std::string::npos,
          "slab thickness must export as a swept solid");
    check(std::none_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.code == "wall_thickness_height_axis_only";
    }), "solid walls must no longer report axis-only export");
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    check(std::none_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "opening-1" && item.code == "opening_assembly_not_exported";
    }), "native fills must no longer report assembly loss");
#else
    check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "opening-1" && item.code == "opening_assembly_not_exported";
    }), "opening assembly loss must be explicit in the IFC fidelity report");
#endif

    const auto imported = import_project_ifc(exported.step);
    check(imported.entities.size() >= 3, "IFC products must reconstruct editable candidates");
    check(std::all_of(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.extensions.contains("ifc_source");
    }), "reconstructed candidates must retain IFC source metadata");
    check(std::any_of(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.properties.value("classification", "") == "ifc_wall_axis";
    }), "wall product classification must survive import");
    const auto by_type = [&](const char* type) -> const Entity& {
        const auto found = std::find_if(imported.entities.begin(), imported.entities.end(),
            [&](const auto& entity) { return entity.type == type; });
        check(found != imported.entities.end(), "recoverable IFC product must be typed");
        return *found;
    };
    const auto& wall = by_type("wall");
    check(wall.properties.at("baseline").at("end") == nlohmann::json({4, 0}),
          "wall axis must become an editable baseline");
    check(wall.properties.value("height_m", 0.0) == 2.5 && wall.properties.value("thickness_m", 0.0) == 0.2,
          "wall dimensions must roundtrip through Vertex IFC properties");
    const auto& slab = by_type("slab");
    check(slab.properties.at("thickness_m") == 0.15 && slab.properties.at("element_kind") == "floor",
          "slab must recover extrusion thickness and kind");
    const auto& opening = by_type("opening");
    check(opening.properties.at("wall_id") == wall.id &&
          std::abs(opening.properties.at("sill_m").get<double>() - 0.1) < 1e-9 &&
          opening.properties.at("width_m") == 1.0 && opening.properties.at("offset_m") == 1.0,
          "opening must recover host, sill, width and offset");
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    check(opening.properties.at("opening_kind") == "door" &&
          opening.properties.at("opening_assembly") == make_document().snapshot().entities().at("opening-1").properties.at("opening_assembly"),
          "validated native assembly must round-trip into active opening properties");
#else
    check(opening.properties.at("opening_kind") == "opening" &&
          opening.extensions.at("ifc_vertex_properties").contains("opening_assembly"),
          "unavailable native assembly must retain opaque metadata without activating assembly kind");
#endif
    const auto elevated = import_project_ifc(export_project_ifc(make_document(3.0).snapshot()).step);
    for (const auto& entity : elevated.entities) {
        if (entity.type == "wall" || entity.type == "slab")
            check(entity.properties.at("elevation_m") == 3.0, "product placement elevation must survive solid origin traversal");
        if (entity.type == "slab")
            check(entity.properties.at("element_kind") == "ceiling", "native slab kind must survive retained properties");
        if (entity.type == "opening")
            check(std::abs(entity.properties.at("sill_m").get<double>() - 0.1) < 1e-9,
                  "opening sill must be relative to elevated host");
    }
    auto legacy = exported.step;
    std::size_t legacy_position = 0;
    while ((legacy_position = legacy.find("Pset_VertexExchange_v1", legacy_position)) != std::string::npos) {
        legacy.replace(legacy_position, 22, "Pset_UnknownExchange_v1");
        legacy_position += 22;
    }
    const auto legacy_import = import_project_ifc(legacy);
    check(std::none_of(legacy_import.entities.begin(), legacy_import.entities.end(), [](const auto& entity) {
        return entity.type == "wall" || entity.type == "opening";
    }) && legacy_import.source_retention_required, "legacy axis without dimensions must remain a boundary candidate");

    auto with_unsupported = exported.step;
    const auto marker = std::string("ENDSEC;\nEND-ISO-10303-21;");
    const auto position = with_unsupported.find(marker);
    check(position != std::string::npos, "export must have a replaceable STEP terminator");
    with_unsupported.insert(position, "#999999=IFCRELAGGREGATES($,$,$,$);\n");
    const auto diagnosed = import_project_ifc(with_unsupported);
    check(diagnosed.source_retention_required, "unreconstructed relationships require source retention");
    check(std::any_of(diagnosed.diagnostics.begin(), diagnosed.diagnostics.end(), [](const auto& item) {
        return item.source_kind == "IFCRELAGGREGATES" && item.code == "relationship_not_reconstructed";
    }), "relationship gaps must be identifiable in the fidelity diagnostics");

    auto nonvertical = exported.step;
    const auto direction = nonvertical.find("IFCDIRECTION((0.,0.,1.))");
    check(direction != std::string::npos, "fixture must contain extrusion direction");
    nonvertical.replace(direction, std::string("IFCDIRECTION((0.,0.,1.))").size(),
                        "IFCDIRECTION((1.,0.,0.))");
    const auto tilted = import_project_ifc(nonvertical);
    check(std::none_of(tilted.entities.begin(), tilted.entities.end(), [](const auto& entity) {
        return entity.type == "slab" || entity.type == "opening";
    }) && tilted.source_retention_required, "nonvertical extrusion must not become a typed floor or opening");

    auto millimetres = exported.step;
    const auto unit = millimetres.find(".LENGTHUNIT.,$,.METRE.");
    check(unit != std::string::npos, "fixture must declare length units");
    millimetres.replace(unit, std::string(".LENGTHUNIT.,$,.METRE.").size(), ".LENGTHUNIT.,.MILLI.,.METRE.");
    const auto unsupported_units = import_project_ifc(millimetres);
    check(std::all_of(unsupported_units.entities.begin(), unsupported_units.entities.end(), [](const auto& entity) {
        return entity.type == "boundary" || entity.type == "ifc_reference";
    }) && unsupported_units.source_retention_required, "unscaled units must not yield typed metre entities");
    auto compound = exported.step;
    std::size_t shape_cursor = 0;
    while ((shape_cursor = compound.find("IFCPRODUCTDEFINITIONSHAPE($,$,(", shape_cursor)) != std::string::npos) {
        const auto list_start = compound.find('#', shape_cursor);
        const auto list_end = compound.find(')', list_start);
        compound.insert(list_end, "," + compound.substr(list_start, list_end - list_start));
        shape_cursor = compound.find(';', list_end);
    }
    const auto compounds = import_project_ifc(compound);
    check(std::all_of(compounds.entities.begin(), compounds.entities.end(), [](const auto& entity) {
        return entity.type == "boundary" || entity.type == "ifc_reference";
    }), "multiple product representations must not silently become one typed entity");

    auto malformed_payload = exported.step;
    const auto payload_start = malformed_payload.find("IFCTEXT('{\"");
    check(payload_start != std::string::npos, "fixture must contain native property payload");
    malformed_payload[payload_start + 9] = '[';
    bool payload_rejected = false;
    try { (void)import_project_ifc(malformed_payload); }
    catch (const std::invalid_argument&) { payload_rejected = true; }
    check(payload_rejected, "malformed native property payload must fail closed");

    auto dangling = exported.step;
    dangling.insert(dangling.find(marker), "#999999=IFCRELVOIDSELEMENT($,$,$,$,#999998,#999997);\n");
    bool dangling_rejected = false;
    try { (void)import_project_ifc(dangling); }
    catch (const std::invalid_argument&) { dangling_rejected = true; }
    check(dangling_rejected, "dangling host relationships must fail closed");

    auto ambiguous = exported.step;
    const auto void_start = ambiguous.rfind('#', ambiguous.find("=IFCRELVOIDSELEMENT"));
    const auto void_end = ambiguous.find(';', void_start);
    const auto args_start = ambiguous.find('=', void_start);
    ambiguous.insert(ambiguous.find(marker), "#999999" + ambiguous.substr(args_start, void_end - args_start + 1) + "\n");
    const auto duplicate_hosts = import_project_ifc(ambiguous);
    check(std::none_of(duplicate_hosts.entities.begin(), duplicate_hosts.entities.end(), [](const auto& entity) {
        return entity.type == "opening";
    }) && duplicate_hosts.source_retention_required, "ambiguous void relationships must remain unbound");

    auto retraced = exported.step;
    const auto corner = retraced.find("IFCCARTESIANPOINT((2.,0.10000000000000001))");
    check(corner != std::string::npos, "fixture must contain opening corner");
    retraced.replace(corner, std::string("IFCCARTESIANPOINT((2.,0.10000000000000001))").size(),
                     "IFCCARTESIANPOINT((1.,-0.10000000000000001))");
    const auto invalid_rectangle = import_project_ifc(retraced);
    check(std::none_of(invalid_rectangle.entities.begin(), invalid_rectangle.entities.end(), [](const auto& entity) {
        return entity.type == "opening";
    }), "retraced rectangle edges must not become an editable opening");

    bool rejected = false;
    try { (void)import_project_ifc("ISO-10303-21;\nDATA;\nENDSEC;\n"); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "malformed IFC must fail closed");
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    native_door_mechanisms();
        native_window_layouts();
        native_historic_fixed_windows();
    native_assemblies();
    closed_leaf_without_operation();
    desktop_hosted_worker_protocol();
#endif
}

} // namespace

int main() {
    try {
        run();
        std::cout << "IFC project exchange tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
