#include "sketch/boundary_entity.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/document.hpp"
#include "sketch/ifc_project_exchange.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/ifc_native_geometry.hpp"
#include "sketch/project_import_worker.hpp"
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
#include "sketch/building_entity.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/vertical_levels.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <span>
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
void verify_worker_candidate(const sketch::IfcProjectImportResult& imported,
                             const char* fixture = "native IFC candidate") {
    sketch::ProjectImportCandidate candidate;
    candidate.kind = sketch::ProjectImportKind::ifc;
    candidate.entities = imported.entities;
    candidate.source_retention_required = imported.source_retention_required;
    for (const auto& diagnostic : imported.diagnostics)
        candidate.diagnostics.push_back({diagnostic.source_id, diagnostic.source_kind, diagnostic.code});
    try {
        check(!sketch::encode_project_import_candidate(candidate).empty(),
              "native IFC candidate must satisfy the strict worker protocol before isolation transport");
    } catch (const std::invalid_argument& error) {
        std::ostringstream detail;
        detail << fixture << ": " << error.what() << "; retention=" << candidate.source_retention_required;
        for (const auto& entity : candidate.entities) {
            detail << "; entity=" << entity.id << '/' << entity.type << ", required=" << entity.required << ", properties=";
            for (const auto& [key,value] : entity.properties.items()) { (void)value; detail << key << ','; }
            detail << " extensions=";
            for (const auto& [key,value] : entity.extensions.items()) { (void)value; detail << key << ','; }
            if (entity.type == "roof" || entity.type == "room" || entity.type == "ifc_reference") {
                try {
                    sketch::project_import_detail::GeometryBudget budget;
                    if (entity.type == "roof") sketch::project_import_detail::validate_roof(entity,budget);
                    else if (entity.type == "room") sketch::project_import_detail::validate_room(entity,budget);
                    else sketch::project_import_detail::validate_ifc_reference(entity);
                    detail << " individual-admission=valid";
                } catch (const std::exception& admission_error) {
                    detail << " individual-admission=" << admission_error.what();
                }
            }
        }
        for (const auto& diagnostic : candidate.diagnostics)
            detail << "; diagnostic=" << diagnostic.source_id << '/' << diagnostic.source_kind << '/' << diagnostic.code;
        throw std::invalid_argument(detail.str());
    }
}

void verify_worker_candidate_rejected(const sketch::Entity& entity) {
    sketch::IfcProjectImportResult imported;
    imported.entities = {entity};
    bool refused = false;
    try { verify_worker_candidate(imported); }
    catch (const std::invalid_argument&) { refused = true; }
    check(refused, "malformed or excessive native roof/room candidates must be refused by the worker protocol");
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

#ifdef SKETCH_IFC_NATIVE_GEOMETRY
double independently_read_mesh_volume(const std::map<std::string, Record>& graph,
    const Record& product) {
    double total = 0;
    const auto& shape = graph.at(product.fields.at(6));
    check(shape.type == "IFCPRODUCTDEFINITIONSHAPE", "semantic product must own its actual body");
    for (const auto& representation_id : list(shape.fields.at(2))) {
        const auto& representation = graph.at(representation_id);
        check(representation.type == "IFCSHAPEREPRESENTATION" && representation.fields.at(1) == "'Body'" &&
              representation.fields.at(2) == "'Tessellation'", "roof/room body must be an IFC4 tessellation");
        for (const auto& item_id : list(representation.fields.at(3))) {
            const auto& mesh = graph.at(item_id);
            check(mesh.type == "IFCTRIANGULATEDFACESET" && mesh.fields.size() == 5 && mesh.fields.at(2) == ".T.",
                "roof/room solid must declare a closed triangulation");
            std::vector<std::array<double,3>> points;
            for (const auto& row : list(graph.at(mesh.fields.at(0)).fields.at(0))) {
                const auto coordinates = list(row);
                check(coordinates.size() == 3, "native world points require three coordinates");
                points.push_back({std::stod(coordinates[0]),std::stod(coordinates[1]),std::stod(coordinates[2])});
            }
            double volume = 0;
            std::map<std::pair<std::size_t,std::size_t>,std::pair<int,int>> incidence;
            for (const auto& row : list(mesh.fields.at(3))) {
                const auto indices = list(row);
                const std::array<std::size_t,3> triangle{std::stoull(indices.at(0))-1,
                    std::stoull(indices.at(1))-1,std::stoull(indices.at(2))-1};
                const auto& a=points.at(triangle[0]); const auto& b=points.at(triangle[1]); const auto& c=points.at(triangle[2]);
                volume += (a[0]*(b[1]*c[2]-b[2]*c[1])+a[1]*(b[2]*c[0]-b[0]*c[2])+a[2]*(b[0]*c[1]-b[1]*c[0]))/6;
                for (std::size_t i=0;i<3;++i) {
                    const auto first=triangle[i], second=triangle[(i+1)%3];
                    auto& edge=incidence[{std::min(first,second),std::max(first,second)}];
                    ++edge.first; edge.second += first<second ? 1 : -1;
                }
            }
            check(volume > 0, "native semantic mesh must enclose positive physical volume");
            for (const auto& [key,edge] : incidence)
                check(edge.first == 2 && edge.second == 0, "native semantic mesh must close with opposite edge incidence");
            total += volume;
        }
    }
    return total;
}

std::string replace_ifc_record(std::string bytes, const std::string& id, const Record& record) {
    const auto begin=bytes.find(id+'='); const auto end=bytes.find(';',begin);
    check(begin != std::string::npos && end != std::string::npos, "replacement record must exist");
    std::string row=id+'='+record.type+'(';
    for (const auto& field : record.fields) { if (row.back() != '(') row+=','; row+=field; }
    row += ");"; bytes.replace(begin,end-begin+1,row); return bytes;
}

// Read carriers through their property sets, independently of the importer.
// Re-sign chunked payloads so refusal tests exercise semantic proof rejection,
// rather than an accidentally broken transport checksum.
template<class Mutation>
std::string mutate_native_metadata(const std::string& bytes, Mutation mutate) {
    using Json = nlohmann::json;
    const auto graph = records(bytes);
    auto altered = bytes;
    std::size_t witnesses = 0;
    const auto text_value = [](const Record& r) {
        check(r.type == "IFCPROPERTYSINGLEVALUE" && r.fields.size() == 4 &&
            r.fields[2].starts_with("IFCTEXT('") && r.fields[2].ends_with("')"),
            "metadata fixture requires an IFC text property");
        auto value = r.fields[2].substr(9,r.fields[2].size()-11);
        for (std::size_t i=0;(i=value.find("''",i))!=std::string::npos;++i) value.erase(i,1);
        return value;
    };
    const auto replace_text = [&](const std::string& id, const std::string& value) {
        auto changed = graph.at(id);
        std::string escaped;
        for (const auto c : value) { escaped += c; if (c == '\'') escaped += c; }
        changed.fields[2] = "IFCTEXT('"+escaped+"')";
        altered = replace_ifc_record(std::move(altered),id,changed);
    };
    for (const auto& [id,pset] : graph) {
        if (pset.type != "IFCPROPERTYSET" || pset.fields.size() != 5 ||
            (pset.fields[2] != "'Pset_VertexExchange_v1'" &&
             pset.fields[2] != "'Pset_VertexExchange_v2'")) continue;
        std::string property_id, manifest_id, payload;
        std::map<std::size_t,std::string> chunks;
        for (const auto& property : list(pset.fields[4])) {
            const auto& r = graph.at(property);
            check(r.type == "IFCPROPERTYSINGLEVALUE" && r.fields.size() == 4,
                "metadata fixture requires single-value properties");
            if (r.fields[0] == "'Properties'") property_id = property;
            else if (r.fields[0] == "'MetadataManifest'") manifest_id = property;
            else if (r.fields[0].starts_with("'PropertiesChunk:") && r.fields[0].ends_with("'")) {
                const auto index = std::stoull(r.fields[0].substr(17,r.fields[0].size()-18));
                check(chunks.emplace(index,property).second,"metadata chunks must have unique indices");
            }
        }
        Json manifest;
        if (pset.fields[2] == "'Pset_VertexExchange_v1'") {
            check(!property_id.empty(),"v1 metadata fixture requires Properties");
            payload = text_value(graph.at(property_id));
        } else {
            check(!manifest_id.empty() && !chunks.empty(),"v2 metadata fixture requires manifest and chunks");
            manifest = Json::parse(text_value(graph.at(manifest_id)));
            check(manifest.at("chunks").get<std::size_t>() == chunks.size(),"manifest must count all chunks");
            for (std::size_t i=0;i<chunks.size();++i) {
                check(chunks.contains(i),"metadata chunks must be contiguous");
                payload += text_value(graph.at(chunks.at(i)));
            }
            check(manifest.at("bytes").get<std::size_t>() == payload.size() &&
                manifest.at("sha256") == sketch::sha256_hex(std::as_bytes(std::span(payload.data(),payload.size()))),
                "original chunked fixture must have valid byte count and hash");
        }
        auto metadata = Json::parse(payload);
        const auto original_metadata = metadata;
        if (!mutate(metadata)) continue;
        const auto changed = metadata.dump(-1,' ',true);
        check(metadata != original_metadata && changed != payload,
            "negative metadata fixture must change its selected carrier");
        ++witnesses;
        if (!property_id.empty()) replace_text(property_id,changed);
        else {
            // Preserve the graph and chunk cardinality while distributing the
            // modified payload in numeric chunk order.
            const auto chunk_size = (changed.size()+chunks.size()-1)/chunks.size();
            for (std::size_t i=0;i<chunks.size();++i)
                replace_text(chunks.at(i),changed.substr(std::min(i*chunk_size,changed.size()),chunk_size));
            manifest["bytes"] = changed.size();
            manifest["sha256"] = sketch::sha256_hex(std::as_bytes(std::span(changed.data(),changed.size())));
            replace_text(manifest_id,manifest.dump());
        }
    }
    check(witnesses == 1 && altered != bytes,"negative metadata fixture must mutate exactly one intended carrier");
    return altered;
}

void native_stairs_and_railings() {
    using namespace sketch;
    using Json = nlohmann::json;
    for (const auto turn : {StairTurn::straight, StairTurn::left_quarter, StairTurn::right_quarter,
        StairTurn::left_half, StairTurn::right_half}) for (const bool multiple : {false, true}) {
        StairFlight stair{"stair", {2,3,4}, .3, 8, 1.6, .3, 1.0, StairLanding{1.0,.15}};
        if (multiple) {
            stair.flights = {{"lower-flight",4},{"upper-flight",4}};
            stair.landings = {{"middle-landing",1.3,.15,turn,
                turn == StairTurn::left_half || turn == StairTurn::right_half ? .25 : 0.0}};
        }
        if (!multiple) {
            const auto independent = export_project_ifc(Document::create({encode_building_entity(stair),
                encode_building_entity(Railing{"free-v1",{7,8,3},.25,2,1,.04,.45})}).snapshot());
            const auto independent_result = import_project_ifc(independent.step);
            verify_worker_candidate(independent_result,"independent v1 stair and v1 railing");
            check(independent_result.entities.size() == 2 &&
                std::count_if(independent_result.entities.begin(),independent_result.entities.end(),[](const auto& e){
                    return e.type == "stair" && e.properties.at("version") == 1;
                }) == 1 && std::count_if(independent_result.entities.begin(),independent_result.entities.end(),[](const auto& e){
                    return e.type == "railing" && e.properties.at("version") == 1;
                }) == 1, "independent version-1 families must recover without inventing unsupported host authority");
            continue;
        }
        auto stair_entity = encode_building_entity(stair);
        stair_entity.extensions["human"] = {{"note","stair"},{"phase",{{"id","historic-phase"}}}};
        if (multiple && turn == StairTurn::left_half)
            stair_entity.extensions["human"]["long_note"] = std::string(5000,'s');
        Railing rail;
        rail.id = "rail"; rail.height = 1.0; rail.thickness = .04; rail.post_spacing = .45;
        rail.host = StairRailingHost{"stair",multiple ? "upper-flight" : "stair",StairRailingSide::right,.1,.9};
        auto rail_entity = encode_building_entity(rail);
        rail_entity.extensions["human"] = {{"note","rail"}};
        Railing free_rail{"free-rail",{7,8,3},.25,2,1,.04,.45};
        auto source_entities = project_import_detail::detached_ifc_validation_entities(
            {stair_entity,rail_entity,encode_building_entity(free_rail)});
        for (auto& e : source_entities) if (e.type == "stair" || (e.type == "railing" && e.properties.contains("host")))
            e.properties["phase_id"] = "source-phase";
        const auto exported = export_project_ifc(Document::create(source_entities).snapshot());
        check(std::none_of(exported.diagnostics.begin(),exported.diagnostics.end(),[](const auto& d){
            return d.source_kind == "stair" || d.source_kind == "railing";
        }), "canonical stair and actual railing solids must export without physical degradation");
        const auto graph = records(exported.step);
        std::string stair_id, rail_id, aggregate_id;
        std::size_t stair_products = 0, rail_products = 0;
        for (const auto& [id,r] : graph) {
            if (r.type == "IFCSTAIR") { stair_id = id; ++stair_products; }
            if (r.type == "IFCRAILING") {
                ++rail_products;
                if (r.fields[2] == "'rail'") rail_id = id;
                check(independently_read_mesh_volume(graph,r) > 0, "rail must contain actual closed positive-volume members");
            }
            check(r.type != "IFCSTAIRFLIGHT", "solid parts must not produce duplicate metadata-less child products");
        }
        check(stair_products == 1 && rail_products == 2, "stair/rail products require exact physical cardinality");
        const auto layout = derive_stair_layout(stair);
        double expected_volume = 0;
        const double h = stair.total_rise / stair.riser_count;
        for (const auto& flight : layout.flights) {
            const auto n = static_cast<double>(flight.treads.size());
            expected_volume += stair.width * stair.going * h * n * (n+1) / 2;
        }
        for (const auto& landing : layout.landings) {
            double twice_area = 0;
            for (std::size_t i=0; i<4; ++i) {
                const auto& a=landing.footprint[i]; const auto& b=landing.footprint[(i+1)%4];
                twice_area += a.x*b.y-b.x*a.y;
            }
            expected_volume += std::abs(twice_area) / 2 * landing.thickness;
        }
        check(std::abs(independently_read_mesh_volume(graph,graph.at(stair_id))-expected_volume) < 1e-7,
            "independent stair volume must include every true riser and connecting/top landing");
        for (const auto& [id,r] : graph) if (r.type == "IFCRELAGGREGATES" && r.fields[4] == stair_id) {
            check(list(r.fields[5]) == std::vector<std::string>{rail_id}, "host aggregate must name the exact physical rail");
            aggregate_id = id;
        }
        check(!aggregate_id.empty(), "hosted rail requires a physical stair aggregate");
        const auto imported = import_project_ifc(exported.step);
        verify_worker_candidate(imported,"canonical stair/railing native candidate");
        check(std::count_if(imported.entities.begin(),imported.entities.end(),[](const auto& e){
            return e.type == "stair" || e.type == "railing";
        }) == 3, "native stair/rail exchange must not add physical measurement duplicates");
        const Entity* recovered_stair=nullptr; const Entity* recovered_rail=nullptr;
        for (const auto& e : imported.entities) {
            if (e.type == "stair") recovered_stair = &e;
            if (e.type == "railing" && e.properties.contains("host")) recovered_rail = &e;
        }
        check(recovered_stair && recovered_rail && recovered_stair->properties == stair_entity.properties,
            "stair must recover exact ordered topology and native manufacturing parameters");
        check(recovered_rail->properties.at("host").at("stair_id") == recovered_stair->id &&
            recovered_rail->properties.at("host").at("flight_id") == (multiple ? "upper-flight" : recovered_stair->id),
            "only proved host identities may remap; version-2 child identity must stay exact");
        check(recovered_stair->extensions.at("human") == stair_entity.extensions.at("human"),
            "nested phase/human source metadata must remain unchanged");
        auto native_entities = imported.entities;
        std::erase_if(native_entities,[](const auto& e){return e.type != "stair" && e.type != "railing";});
        const auto repeated = import_project_ifc(export_project_ifc(Document::create(
            project_import_detail::detached_ifc_validation_entities(native_entities)).snapshot()).step);
        check(std::count_if(repeated.entities.begin(),repeated.entities.end(),[](const auto& e){return e.type == "railing";}) == 2 &&
            std::count_if(repeated.entities.begin(),repeated.entities.end(),
            [](const auto& e){return e.type == "stair";}) == 1, "repeated native exchange must preserve editable physical families");
        for (const auto& e : repeated.entities) if (e.type == "stair")
            check(e.extensions.at("ifc_vertex_properties") == recovered_stair->extensions.at("ifc_vertex_properties"),
                "unchanged exchange must retain the original source proof without recursive metadata growth");
        auto edited_entities = native_entities;
        for (auto& e : edited_entities) if (e.type == "stair") e.properties["going_m"] = .35;
        const auto edited = import_project_ifc(export_project_ifc(Document::create(
            project_import_detail::detached_ifc_validation_entities(edited_entities)).snapshot()).step);
        check(std::count_if(edited.entities.begin(),edited.entities.end(),[](const auto& e){return e.type == "stair";}) == 1 &&
            std::count_if(edited.entities.begin(),edited.entities.end(),[](const auto& e){return e.type == "railing";}) == 2,
            "a legitimate host edit must export fresh same-model host proof and keep the native rail editable");
        std::size_t refusal_case = 0;
        const auto declined = [&](const std::string& bytes, bool stair_declined) {
            const auto ordinal = refusal_case++;
            const auto result = [&] {
                try { return import_project_ifc(bytes); }
                catch (const std::exception& error) {
                    throw std::runtime_error("Stair/rail refusal fixture import: turn=" +
                        std::to_string(static_cast<int>(turn)) + " case=" + std::to_string(ordinal) +
                        " cause=" + error.what());
                }
            }();
            if (!result.source_retention_required || !std::none_of(result.entities.begin(),result.entities.end(),
                [&](const auto& e){ return stair_declined ? e.type == "stair" : e.type == "railing" && e.properties.contains("host"); }))
                throw std::runtime_error("Unproved stair/rail carrier remained active: turn=" +
                    std::to_string(static_cast<int>(turn)) + " case=" + std::to_string(ordinal));
            verify_worker_candidate(result,"declined stair/rail candidate");
        };
        auto ambiguous = exported.step;
        const auto& aggregate = graph.at(aggregate_id);
        std::string row = "#900001=IFCRELAGGREGATES(";
        for (const auto& f : aggregate.fields) { if (row.back() != '(') row += ','; row += f; }
        ambiguous.insert(ambiguous.find("ENDSEC;\nEND-ISO"),row+");\n");
        declined(ambiguous,false);
        auto wrong_kind = graph.at(stair_id); wrong_kind.fields[8] = ".STRAIGHT_RUN_STAIR.";
        declined(replace_ifc_record(exported.step,stair_id,wrong_kind),true);
        auto nonmetre = exported.step;
        const auto metre = nonmetre.find(".LENGTHUNIT.,$,.METRE.");
        nonmetre.replace(metre, std::string(".LENGTHUNIT.,$,.METRE.").size(), ".LENGTHUNIT.,.MILLI.,.METRE.");
        declined(nonmetre,true);
        auto tampered_mesh = exported.step;
        for (const auto& [id,r] : graph) if (r.type == "IFCCARTESIANPOINTLIST3D") {
            auto moved = r; auto rows = list(r.fields[0]); auto point = list(rows[0]);
            point[0] = std::to_string(std::stod(point[0]) + .2);
            rows[0] = "("+point[0]+","+point[1]+","+point[2]+")";
            moved.fields[0] = "(";
            for (const auto& p : rows) { if (moved.fields[0].size()>1) moved.fields[0]+=','; moved.fields[0]+=p; }
            moved.fields[0]+=')'; tampered_mesh=replace_ifc_record(std::move(tampered_mesh),id,moved);
        }
        declined(tampered_mesh,true);
        if (turn == StairTurn::straight) {
            // Repeated duplicate-identity carriers share the actual geometry
            // but each attempted regeneration consumes the file-wide ledger.
            std::string property_relation;
            for (const auto& [id,r] : graph) if (r.type == "IFCRELDEFINESBYPROPERTIES" &&
                list(r.fields[4]) == std::vector<std::string>{stair_id}) property_relation=id;
            check(!property_relation.empty(), "stair work fixture requires native property relation");
            auto repeated_work=exported.step;
            std::string additions;
            const auto append=[&](int id, const Record& r) {
                std::string row="#"+std::to_string(id)+"="+r.type+"(";
                for (const auto& f:r.fields) { if (row.back() != '(') row+=','; row+=f; }
                additions+=row+");\n";
            };
            for (int i=0;i<8;++i) {
                const int id=900000+2*i;
                append(id,graph.at(stair_id));
                auto relation=graph.at(property_relation); relation.fields[4]="(#"+std::to_string(id)+")";
                append(id+1,relation);
            }
            repeated_work.insert(repeated_work.find("ENDSEC;\nEND-ISO"),additions);
            IfcExchangeLimits bounded; bounded.max_mesh_vertices=2200; bounded.max_mesh_triangles=2200;
            const auto exhausted=import_project_ifc(repeated_work,bounded);
            check(std::count_if(exhausted.entities.begin(),exhausted.entities.end(),[](const auto& e){return e.type == "stair";}) == 1 &&
                std::any_of(exhausted.diagnostics.begin(),exhausted.diagnostics.end(),[](const auto& d) {
                    return d.code == "native_stair_or_railing_reconstruction_budget_exceeded";
                }), "failed duplicate carriers must not refund expected mesh work or add active products");
            verify_worker_candidate(exhausted,"aggregate failed stair reconstruction work");
        }
        if (turn == StairTurn::straight) for (const auto mutation : {"unknown-version", "source-only", "physical-change"}) {
            const auto altered = mutate_native_metadata(exported.step,[&](Json& metadata) {
                if (!metadata.contains("_vertex_ifc_mesh") || metadata.at("_vertex_ifc_mesh").at("role") != "stair") return false;
                if (std::string_view(mutation) == "unknown-version") {
                    metadata["version"] = 99; metadata["_vertex_ifc_entity"]["properties"]["version"] = 99;
                } else {
                    metadata["_vertex_ifc_entity"]["properties"]["width_m"] = 1.1;
                    if (std::string_view(mutation) == "physical-change") metadata["width_m"] = 1.1;
                }
                return true;
            });
            declined(altered,true);
        }
        if (turn == StairTurn::straight) for (const auto key : {"property_id","building_id","floor_id","layer_id","phase_id"}) {
            const auto altered = mutate_native_metadata(exported.step,[&](Json& metadata) {
                if (!metadata.contains("_vertex_ifc_mesh") || metadata.at("_vertex_ifc_mesh").at("role") != "railing" ||
                    !metadata.contains("host")) return false;
                const Json contradiction = std::string_view(key) == "layer_id" ? Json(17) : Json("contradictory-source");
                metadata[key]=contradiction; metadata["_vertex_ifc_entity"]["properties"][key]=contradiction;
                return true;
            });
            declined(altered,false);
        }
        if (multiple && turn == StairTurn::left_half) if (const auto* directory = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
            std::filesystem::create_directories(directory);
            std::ofstream output(std::filesystem::path(directory)/"native-stair-railing.ifc",std::ios::binary);
            output << exported.step;
            check(output.good(), "stair/railing worker capture must be written completely");
        }
    }
}

void native_landing_railing_exchange() {
    using namespace sketch; using Json=nlohmann::json;
    for (const auto role : {StairLandingRole::connecting,StairLandingRole::top}) {
        StairFlight stair{"landing-stair",{2,3,4},.37,8,1.6,.3,1,StairLanding{1.1,.15}};
        stair.flights={{"incoming-flight",4},{"outgoing-flight",4}};
        stair.landings={{"connecting-landing",1.3,.15,StairTurn::straight,0}};
        Railing rail{"landing-rail",{},0,0,.9,.04,.3};
        rail.landing_host=StairLandingRailingHost{stair.id,role,
            role==StairLandingRole::connecting?"connecting-landing":"",
            role==StairLandingRole::connecting?"incoming-flight":"outgoing-flight",
            role==StairLandingRole::connecting?"outgoing-flight":"",0,.1,.9};
        const auto canonical=encode_railing_properties(rail);
        const auto source=Document::create(project_import_detail::detached_ifc_validation_entities(
            {encode_building_entity(stair),encode_building_entity(rail)}));
        const auto exported=export_project_ifc(source.snapshot());
        const auto imported=import_project_ifc(exported.step);
        verify_worker_candidate(imported,"canonical v3 landing railing candidate");
        const auto recovered=std::find_if(imported.entities.begin(),imported.entities.end(),[](const auto& e){return e.type=="railing";});
        const auto host=std::find_if(imported.entities.begin(),imported.entities.end(),[](const auto& e){return e.type=="stair";});
        check(recovered!=imported.entities.end() && host!=imported.entities.end() &&
            recovered->properties.at("version")==3 && recovered->properties.at("form")=="stair_landing_railing",
            "exact canonical landing railing must recover as a native editable v3 family");
        auto expected=canonical; expected["host"]["stair_id"]=host->id;
        check(recovered->properties==expected, "landing host witnesses and fractions must remain exact through owner remapping");
        for (const auto mutation : {"unknown-version","wrong-witness","blocked-edge","missing-phase"}) {
            const auto altered = mutate_native_metadata(exported.step,[&](Json& metadata) {
                if(!metadata.contains("_vertex_ifc_mesh")||metadata.at("_vertex_ifc_mesh").at("role")!="railing") return false;
                if(std::string_view(mutation)=="unknown-version") {
                    metadata["version"]=4; metadata["_vertex_ifc_entity"]["properties"]["version"]=4;
                } else if(std::string_view(mutation)=="missing-phase") {
                    metadata["phase_id"]="unexpected-phase";
                    metadata["_vertex_ifc_entity"]["properties"]["phase_id"]="unexpected-phase";
                } else {
                    const auto key=std::string_view(mutation)=="wrong-witness"?"incoming_flight_id":"edge_index";
                    const Json value=std::string_view(mutation)=="wrong-witness"?Json("retired-flight"):Json(3);
                    metadata["host"][key]=value; metadata["_vertex_ifc_entity"]["properties"]["host"][key]=value;
                }
                return true;
            });
            const auto refused=import_project_ifc(altered);
            check(refused.source_retention_required && std::none_of(refused.entities.begin(),refused.entities.end(),
                [](const auto& e){return e.type=="railing";}), "unproved landing host/version/phase must stay inert with original source retained");
            verify_worker_candidate(refused,"declined v3 landing railing carrier");
        }
        if(const auto* directory=std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
            std::filesystem::create_directories(directory);
            std::ofstream output(std::filesystem::path(directory)/(role==StairLandingRole::top?
                "native-top-landing-railing.ifc":"native-connecting-landing-railing.ifc"),std::ios::binary);
            output<<exported.step;check(output.good(),"v3 landing IFC capture must be written completely");
        }
    }
}

void native_stair_export_cluster_proofs() {
    using namespace sketch;
    StairFlight stair{"cluster-stair",{2,3,4},.37,8,1.6,.3,1,StairLanding{1.1,.15}};
    stair.flights = {{"cluster-lower",4},{"cluster-upper",4}};
    stair.landings = {{"cluster-middle",1.3,.15,StairTurn::straight,0}};
    Railing flight{"cluster-flight",{},0,0,1,.04,.45};
    flight.host = StairRailingHost{stair.id,"cluster-upper",StairRailingSide::right,.1,.9};
    Railing guard{"cluster-guard",{},0,0,.9,.04,.3};
    guard.landing_host = StairLandingRailingHost{stair.id,StairLandingRole::connecting,
        "cluster-middle","cluster-lower","cluster-upper",0,.1,.9};
    auto flight_entity = encode_building_entity(flight);
    flight_entity.properties["host"]["landing_id"] = "cluster-middle";
    flight_entity.properties["host"]["incoming_flight_id"] = "cluster-lower";
    flight_entity.properties["host"]["outgoing_flight_id"] = "cluster-upper";
    auto source_entities = project_import_detail::detached_ifc_validation_entities({
        encode_building_entity(stair),flight_entity,encode_building_entity(guard),
        encode_building_entity(StairFlight{"independent-stair",{20,20,0},0,4,.8,.3,1})});
    const auto imported = import_project_ifc(export_project_ifc(Document::create(source_entities).snapshot()).step);
    std::vector<Entity> live;
    for (auto entity : imported.entities) {
        if (entity.type != "stair" && entity.type != "railing") continue;
        const auto source_id = entity.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("id");
        if (source_id == "cluster-stair") {
            entity.id = "live-stair";
            entity.properties["flights"][0]["id"] = "live-lower";
            entity.properties["flights"][1]["id"] = "live-upper";
            entity.properties["landings"][0]["id"] = "live-middle";
        } else if (source_id == "cluster-flight") {
            entity.id = "live-flight";
            entity.properties["host"]["stair_id"] = "live-stair";
            entity.properties["host"]["flight_id"] = "live-upper";
        } else if (source_id == "cluster-guard") {
            entity.id = "live-guard";
            entity.properties["host"]["stair_id"] = "live-stair";
            entity.properties["host"]["landing_id"] = "live-middle";
            entity.properties["host"]["incoming_flight_id"] = "live-lower";
            entity.properties["host"]["outgoing_flight_id"] = "live-upper";
        }
        for (const auto* key : {"property_id","building_id","floor_id","layer_id"})
            entity.properties[key] = std::string("destination-") + key;
        live.push_back(std::move(entity));
    }
    check(live.size() == 4,"cluster fixture must recover two stairs and both v2/v3 hosted rails");
    const auto recover = [&](const std::vector<Entity>& active) {
        auto entities = active;
        entities.push_back(Entity{"destination-property_id","property"});
        entities.push_back(Entity{"destination-building_id","building",{{"property_id","destination-property_id"}}});
        entities.push_back(Entity{"destination-floor_id","floor",{{"building_id","destination-building_id"}}});
        entities.push_back(Entity{"destination-layer_id","layer",{{"floor_id","destination-floor_id"}}});
        const auto document = Document::create(entities);
        check(document.snapshot().is_editable(),"remapped cluster fixture must retain valid destination authority");
        const auto result = import_project_ifc(export_project_ifc(document.snapshot()).step);
        verify_worker_candidate(result,"coherent remapped stair export cluster");
        check(document.snapshot().entities() == Document::create(entities).snapshot().entities(),
            "cluster proof preparation must not mutate live entities or immutable retained evidence");
        std::map<std::string,Entity> recovered;
        for (const auto& entity : result.entities) {
            if (entity.type != "stair" && entity.type != "railing") continue;
            const auto id = entity.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("id").get<std::string>();
            check(recovered.emplace(id,entity).second,"cluster re-export must preserve unique active families");
        }
        check(recovered.size() == 4,"cluster member edits must preserve two active stairs and both hosted railings");
        return recovered;
    };
    const auto recovered_entity = [](const std::map<std::string,Entity>& recovered,
                                     const std::string& id, const char* phase) -> const Entity& {
        const auto found = recovered.find(id);
        if (found != recovered.end()) return found->second;
        std::string keys;
        for (const auto& [key, entity] : recovered) {
            (void)entity;
            if (!keys.empty()) keys += ',';
            keys += key;
        }
        throw std::runtime_error(std::string{"cluster proof phase="} + phase +
            " missing source id=" + id + " recovered source ids=" + keys);
    };
    const auto unchanged = recover(live);
    for (const auto& entity : live) {
        const auto& prior = entity.extensions.at("ifc_vertex_properties");
        check(recovered_entity(unchanged,prior.at("_vertex_ifc_entity").at("id").get<std::string>(),"unchanged").extensions.at("ifc_vertex_properties") == prior,
            "an unchanged remapped cluster must retain every exact source proof and opaque v2 host extra");
    }
    for (const auto* change : {"opaque","rail-human","guard-human","stair-human","rail-required",
        "stair-required","rail-height","guard-height","stair-elevation"}) {
        auto edited = live;
        for (auto& entity : edited) {
            if (entity.id == "live-flight") {
                if (std::string_view(change) == "opaque") entity.properties["host"]["landing_id"] = "live-middle";
                if (std::string_view(change) == "rail-human") entity.extensions["human"] = {{"reviewed_note","rail"}};
                if (std::string_view(change) == "rail-required") entity.required = true;
                if (std::string_view(change) == "rail-height") entity.properties["height_m"] = 1.1;
            }
            if (entity.id == "live-stair") {
                if (std::string_view(change) == "stair-human") entity.extensions["human"] = {{"reviewed_note","stair"}};
                if (std::string_view(change) == "stair-required") entity.required = true;
                if (std::string_view(change) == "stair-elevation") entity.properties["base_position_m"][2] = 4.5;
            }
            if (entity.id == "live-guard") {
                if (std::string_view(change) == "guard-human") entity.extensions["human"] = {{"reviewed_note","guard"}};
                if (std::string_view(change) == "guard-height") entity.properties["height_m"] = 1.1;
            }
        }
        const auto recovered = recover(edited);
        const auto& host_proof = recovered_entity(recovered,"live-stair",change).extensions.at("ifc_vertex_properties");
        for (const auto& entity : edited) {
            const auto& prior = entity.extensions.at("ifc_vertex_properties");
            const auto source_id = prior.at("_vertex_ifc_entity").at("id").get<std::string>();
            if (source_id == "independent-stair") {
                // The recovered map uses proof identities. An unchanged
                // unrelated carrier retains its source ID even when the
                // imported live entity received an IFC occurrence identity.
                check(recovered_entity(recovered,source_id,change).extensions.at("ifc_vertex_properties") == prior,
                    "a changed hosted cluster must preserve unrelated stair source evidence");
                continue;
            }
            const auto& metadata = recovered_entity(recovered,entity.id,change).extensions.at("ifc_vertex_properties");
            auto extensions = entity.extensions;
            extensions.erase("ifc_source"); extensions.erase("ifc_vertex_properties");
            const auto& proof = metadata.at("_vertex_ifc_entity");
            check(proof.at("id") == entity.id && proof.at("properties") == entity.properties &&
                proof.at("required") == entity.required && proof.at("extensions") == extensions,
                "one changed member must export current authoritative proofs for its entire hosted cluster");
            if (entity.type == "railing") check(metadata.at("_vertex_ifc_host") == host_proof,
                "each v2/v3 hosted rail must carry the exact emitted stair proof");
        }
        check(recovered.at("live-flight").properties.at("host").at("landing_id") ==
            (std::string_view(change) == "opaque" ? "live-middle" : "cluster-middle"),
            "v2 unknown host fields must remain opaque through coherent proof regeneration");
    }
}

void native_stair_source_context() {
    using namespace sketch;
    StairFlight stair{"level-stair",{2,3,4},.2,8,1.6,.3,1.0};
    stair.flights = {{"connected-flight",8}};
    stair.level_connection = StairLevelConnection{"levels","floor-link","lower","upper"};
    auto entity = encode_building_entity(stair);
    entity.required = true;
    entity.properties["property_id"] = "property";
    entity.properties["building_id"] = "building";
    entity.properties["floor_id"] = "floor";
    entity.properties["layer_id"] = "layer";
    entity.properties["parent_id"] = "layer";
    entity.properties["vertical_placement"] = {{"version",1},{"mode","level"},{"offset_m",.5}};
    entity.extensions["human"] = {{"phase",{{"id","original-phase"}}},{"nested",{{"source_id","level-stair"}}}};
    const VerticalLevelGraph levels({{"lower",10.0},{"upper",11.6}},{{"floor-link","lower","upper"}});
    const auto document = Document::create({entity, Entity{"property","property"},
        Entity{"building","building",{{"property_id","property"}}},
        Entity{"floor","floor",{{"building_id","building"},
            {"vertical_level_binding",VerticalLevelBinding{"levels","lower"}.to_json()}}},
        Entity{"layer","layer",{{"floor_id","floor"}}},
        Entity{"levels","vertical_levels",{{"model",nlohmann::json::parse(levels.serialize())}}}});
    const auto result = import_project_ifc(export_project_ifc(document.snapshot()).step);
    verify_worker_candidate(result,"source-context detached stair");
    const auto recovered = std::find_if(result.entities.begin(),result.entities.end(),[](const auto& e){return e.type == "stair";});
    check(recovered != result.entities.end() && recovered->properties.at("base_position_m").at(2) == 14.5 &&
        !recovered->required && !recovered->properties.contains("floor_id") && !recovered->properties.contains("layer_id") &&
        !recovered->properties.contains("vertical_placement") && !recovered->properties.contains("level_connection") &&
        recovered->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("properties") == entity.properties &&
        recovered->extensions.at("human") == entity.extensions.at("human"),
        "resolved source stair geometry must detach active context and preserve its immutable original envelope");
    check(document.snapshot().entities().at(entity.id).properties == entity.properties,
        "native IFC source preparation must not mutate original stair authoring");
    const auto repeated = import_project_ifc(export_project_ifc(Document::create(
        project_import_detail::detached_ifc_validation_entities({*recovered})).snapshot()).step);
    const auto repeated_stair = std::find_if(repeated.entities.begin(), repeated.entities.end(),
        [](const auto& e) { return e.type == "stair"; });
    check(repeated_stair != repeated.entities.end() && !repeated_stair->required &&
        repeated_stair->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity") ==
            recovered->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity"),
        "untouched detached required-source stair must retain its exact original required/context envelope on re-export");
}

void native_roofs_and_spaces() {
    using namespace sketch;
    using Json=nlohmann::json;
    const double slope=0.5, secant=std::sqrt(1+slope*slope), thickness=.15, overhang=.2;
    for (const auto form : {"sloped_roof_panel","gable_roof","hip_roof"}) {
        for (const bool cut : {false,true}) {
            const std::vector<RoofOpening> openings=cut ? std::vector<RoofOpening>{{"skylight",.5,.5,.7,.8}}
                                                      : std::vector<RoofOpening>{};
            Entity entity;
            double expected;
            std::string ifc_kind;
            if (std::string_view(form) == "sloped_roof_panel") {
                entity=encode_building_entity(SlopedRoofPanel{"roof",{2,3,4},.3,4,3,2,std::atan(slope),overhang,thickness,openings});
                expected=(4+2*overhang)*(3+2*overhang)*secant*thickness;
                ifc_kind=".SHED_ROOF.";
            } else if (std::string_view(form) == "gable_roof") {
                entity=encode_building_entity(GableRoof{"roof",{2,3,4},.3,8,6,1.5,std::atan(slope),overhang,thickness,openings});
                const double inward=slope*thickness/secant;
                expected=2*(8+2*overhang)*(3+overhang-inward/2)*thickness*secant;
                ifc_kind=".GABLE_ROOF.";
            } else {
                entity=encode_building_entity(HipRoof{"roof",{2,3,4},.3,8,6,1.5,std::atan(slope),overhang,thickness,openings});
                expected=(8+2*overhang)*(6+2*overhang)*thickness*secant;
                ifc_kind=".HIP_ROOF.";
            }
            if (cut) expected-=.7*.8*thickness*secant;
            entity.properties["name"]="roof";
            entity.extensions["human"]={{"name","roof"},{"nested",{{"description",std::string(2400,'h')}}}};
            if (std::string_view(form) == "gable_roof" && cut)
                entity.extensions["human"]["nested"]["description"]=std::string(5000,'h');
            const auto exported=export_project_ifc(Document::create({entity}).snapshot());
            check(exported.diagnostics.empty(), "native roofs must export actual geometry and complete metadata");
            if (std::string_view(form) == "gable_roof" && cut) {
                check(exported.step.find("Pset_VertexExchange_v2") != std::string::npos,
                    "long roof carrier must exercise chunked native property metadata");
                if (const auto* directory = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
                    std::filesystem::create_directories(directory);
                    std::ofstream output(std::filesystem::path(directory) / "native-gable-roof-v2.ifc",std::ios::binary);
                    output << exported.step;
                    check(output.good(), "long roof integration capture must be written completely");
                }
            }
            const auto graph=records(exported.step);
            std::string product_id;
            for (const auto& [id,record] : graph) if (record.type == "IFCROOF") product_id=id;
            check(!product_id.empty() && graph.at(product_id).fields.size() == 9 &&
                  graph.at(product_id).fields.at(8) == ifc_kind, "canonical native roof requires matching IFC4 roof type");
            check(std::abs(independently_read_mesh_volume(graph,graph.at(product_id))-expected)<1e-7,
                "independent roof mesh volume must agree with slope, normal thickness, overhang and vertical skylight cut");
            bool contained=false;
            for (const auto& [id,record] : graph) if (record.type == "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
                const auto products=list(record.fields.at(4));
                contained=contained || std::find(products.begin(),products.end(),product_id)!=products.end();
            }
            check(contained, "roof must have a spatial containment relationship");
            const auto imported=import_project_ifc(exported.step);
            verify_worker_candidate(imported);
            check(imported.entities.size() == 1 && imported.entities[0].type == "roof" &&
                  imported.entities[0].properties == entity.properties &&
                  imported.entities[0].extensions.at("human") == entity.extensions.at("human"),
                "verified roof must reconstruct editable authored parameters and untouched nested human metadata");
            if (std::string_view(form) == "gable_roof" && cut)
                if (const auto* directory = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
                    std::ofstream expected(std::filesystem::path(directory) / "native-gable-roof-v2.expected.json",std::ios::binary);
                    expected << Json{{"source_entity_id",entity.id},{"active_entities",Json::array({
                        {{"id",imported.entities[0].id},{"type","roof"}}})},{"diagnostic_count",imported.diagnostics.size()}}.dump(2);
                    check(expected.good(), "roof capture expectations must be written completely");
                }
            auto malformed=imported.entities[0];
            malformed.properties["version"]=3; verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["form"]="unsupported_roof"; verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["base_position_m"]={2,3}; verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["span_m"]=-1; verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["orientation_rad"]=std::numeric_limits<double>::infinity();
            verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["pitch_rad"]=.25; verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["thickness_m"]=1e7; verify_worker_candidate_rejected(malformed);
            malformed=imported.entities[0]; malformed.properties["version"]=2;
            malformed.properties["roof_openings"]=Json::array({{{"id","bad"},{"x_m",1e6},{"y_m",0},{"width_m",1},{"depth_m",1}}});
            verify_worker_candidate_rejected(malformed);
            malformed.properties["roof_openings"]=Json::array({{{"id","cut"},{"x_m",.5},{"y_m",.5},{"width_m",.1},{"depth_m",.1}},
                {{"id","other"},{"x_m",.5},{"y_m",.5},{"width_m",.1},{"depth_m",.1}}});
            verify_worker_candidate_rejected(malformed);
            malformed.properties["roof_openings"]=Json::array();
            for (int i=0; i<257; ++i) malformed.properties["roof_openings"].push_back(
                {{"id","cut-"+std::to_string(i)},{"x_m",.5},{"y_m",.5},{"width_m",.1},{"depth_m",.1}});
            verify_worker_candidate_rejected(malformed);
            const auto repeated=import_project_ifc(export_project_ifc(Document::create(imported.entities).snapshot()).step);
            check(repeated.entities.size() == 1 && repeated.entities[0].type == "roof" &&
                  repeated.entities[0].properties == entity.properties &&
                  repeated.entities[0].extensions.at("ifc_vertex_properties").dump().size() ==
                    imported.entities[0].extensions.at("ifc_vertex_properties").dump().size(),
                "roof re-export must preserve manufacturing parameters without recursive metadata growth");
            if (!cut) {
                auto product=graph.at(product_id); product.fields[8]=".FLAT_ROOF.";
                const auto wrong=import_project_ifc(replace_ifc_record(exported.step,product_id,product));
                check(std::none_of(wrong.entities.begin(),wrong.entities.end(),[](const auto& e){return e.type == "roof";}) &&
                    wrong.source_retention_required, "contradictory IFC roof type must remain inert with source retention");
            }
        }
    }
    const auto rectangle=[](double x,double y,double width,double depth) {
        return Json::array({{{"start",{x,y}},{"end",{x+width,y}},{"sweep_radians",0}},
            {{"start",{x+width,y}},{"end",{x+width,y+depth}},{"sweep_radians",0}},
            {{"start",{x+width,y+depth}},{"end",{x,y+depth}},{"sweep_radians",0}},
            {{"start",{x,y+depth}},{"end",{x,y}},{"sweep_radians",0}}});
    };
    Entity room{"room","room",{{"boundary",rectangle(2,3,4,3)},{"holes",Json::array({rectangle(3,4,1,1)})},
        {"height_m",2.5},{"elevation_m",4.25},{"name","room"},{"classification","office"}},false,
        {{"human",{{"note","room"},{"nested",Json::array({"roof","room"})}}}}};
    const auto exported=export_project_ifc(Document::create({room}).snapshot());
    check(exported.diagnostics.empty(), "authored holed room must export a genuine space");
    const auto graph=records(exported.step);
    std::string space_id;
    for (const auto& [id,record] : graph) if (record.type == "IFCSPACE") space_id=id;
    check(!space_id.empty() && graph.at(space_id).fields.size() == 11 && graph.at(space_id).fields[9] == ".INTERNAL.",
        "room must export the IFC4 space schema");
    check(std::abs(independently_read_mesh_volume(graph,graph.at(space_id))-27.5)<1e-7,
        "space mesh must represent independently calculated net area times authored height");
    bool aggregated=false;
    for (const auto& [id,record] : graph) {
        if (record.type == "IFCRELAGGREGATES" && record.fields[5] == "("+space_id+")")
            aggregated=graph.at(record.fields[4]).type == "IFCBUILDINGSTOREY";
        if (record.type == "IFCRELCONTAINEDINSPATIALSTRUCTURE")
            check(record.fields[4].find(space_id) == std::string::npos, "spaces must decompose storeys instead of element containment");
    }
    check(aggregated, "space must decompose its storey");
    const auto imported=import_project_ifc(exported.step);
    verify_worker_candidate(imported);
    check(imported.entities.size() == 1 && imported.entities[0].type == "room" && imported.entities[0].properties == room.properties &&
          imported.entities[0].extensions.at("human") == room.extensions.at("human"),
        "space must reconstruct its exact editable holed room and human metadata");
    auto malformed_room=imported.entities[0]; malformed_room.properties.erase("height_m");
    verify_worker_candidate_rejected(malformed_room);
    malformed_room=imported.entities[0]; malformed_room.properties["height_m"]=-1;
    verify_worker_candidate_rejected(malformed_room);
    malformed_room=imported.entities[0]; malformed_room.properties["elevation_m"]=std::numeric_limits<double>::infinity();
    verify_worker_candidate_rejected(malformed_room);
    malformed_room=imported.entities[0]; malformed_room.properties["layer_id"]="foreign-layer";
    verify_worker_candidate_rejected(malformed_room);
    malformed_room=imported.entities[0]; malformed_room.properties["holes"]=Json::array({rectangle(20,20,1,1)});
    verify_worker_candidate_rejected(malformed_room);
    // Four individually bounded 256-edge rooms exceed the aggregate analytical
    // pair budget. The mapper must keep excess carriers inert, while a forged
    // worker response that activates all four is rejected before transport.
    std::vector<Entity> many_rooms;
    for (int room_index=0; room_index<4; ++room_index) {
        auto many=room;
        many.id="many-room-"+std::to_string(room_index);
        many.properties["holes"]=Json::array(); many.properties["boundary"]=Json::array();
        for (int edge=0; edge<256; ++edge) {
            const auto a=2*std::acos(-1.0)*edge/256, b=2*std::acos(-1.0)*(edge+1)/256;
            many.properties["boundary"].push_back({{"start",{10*room_index+2*std::cos(a),2*std::sin(a)}},
                {"end",{10*room_index+2*std::cos(b),2*std::sin(b)}},{"sweep_radians",0}});
        }
        many_rooms.push_back(std::move(many));
    }
    IfcProjectImportResult forged_many; forged_many.entities=many_rooms;
    bool aggregate_refused=false;
    try { verify_worker_candidate(forged_many); } catch (const std::invalid_argument&) { aggregate_refused=true; }
    check(aggregate_refused, "aggregate native room topology must be bounded across the whole worker response");
    const auto many_step=export_project_ifc(Document::create(many_rooms).snapshot()).step;
    const auto many_import=import_project_ifc(many_step);
    verify_worker_candidate(many_import);
    check(std::count_if(many_import.entities.begin(),many_import.entities.end(),[](const auto& e){return e.type == "room";}) == 3 &&
        std::count_if(many_import.entities.begin(),many_import.entities.end(),[](const auto& e){return e.type == "ifc_reference" &&
            e.extensions.at("ifc_vertex_properties").contains("_vertex_ifc_entity");}) == 1 &&
        std::any_of(many_import.diagnostics.begin(),many_import.diagnostics.end(),[](const auto& d) {
            return d.code == "native_roof_or_room_candidate_budget_exceeded" ||
                d.code == "native_roof_or_room_reconstruction_budget_exceeded";
        }), "overbudget IFC space must remain inert with exact authored source metadata and explicit diagnosis");
    for (const auto& retained : many_import.entities) if (retained.type == "ifc_reference") {
        const auto& payload=retained.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity");
        check(std::any_of(many_rooms.begin(),many_rooms.end(),[&](const auto& authored) {
            return payload.at("properties") == authored.properties && payload.at("extensions") == authored.extensions;
        }), "declined space must retain its exact original authored fields and nested metadata");
    }
    auto mismatched_many=many_step;
    for (const auto& [id,record] : records(many_step)) if (record.type == "IFCSHAPEREPRESENTATION" &&
        record.fields[1] == "'Body'" && record.fields[2] == "'Tessellation'") {
        auto tiny=record; tiny.fields[3]="(#950002)";
        mismatched_many=replace_ifc_record(std::move(mismatched_many),id,tiny);
    }
    mismatched_many.insert(mismatched_many.find("ENDSEC;\nEND-ISO-10303-21;"),
        "#950001=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(1.,1.,0.),(0.,1.,0.),"
        "(0.,0.,1.),(1.,0.,1.),(1.,1.,1.),(0.,1.,1.)));\n"
        "#950002=IFCTRIANGULATEDFACESET(#950001,$,.T.,((1,3,2),(1,4,3),(5,6,7),(5,7,8),"
        "(1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,4,8),(3,8,7),(4,1,5),(4,5,8)),$);\n");
    const auto declined_many=import_project_ifc(mismatched_many);
    verify_worker_candidate(declined_many,"cumulative mismatched room reconstruction attempts");
    check(declined_many.entities.size() == 4 &&
        std::all_of(declined_many.entities.begin(),declined_many.entities.end(),[](const auto& e){return e.type == "ifc_reference";}) &&
        std::any_of(declined_many.diagnostics.begin(),declined_many.diagnostics.end(),[](const auto& d) {
            return d.code == "native_roof_or_room_reconstruction_budget_exceeded";
        }), "tiny mismatching carriers must consume cumulative native reconstruction budget before fallback");
    for (const auto& retained : declined_many.entities) {
        const auto& payload=retained.extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity");
        check(std::any_of(many_rooms.begin(),many_rooms.end(),[&](const auto& authored) {
            return payload.at("properties") == authored.properties && payload.at("extensions") == authored.extensions;
        }), "failed reconstruction attempts must retain exact original authoring after budget exhaustion");
    }
    malformed_room=imported.entities[0]; malformed_room.properties["boundary"]=Json::array();
    for (int i=0; i<513; ++i) malformed_room.properties["boundary"].push_back(room.properties["boundary"][0]);
    verify_worker_candidate_rejected(malformed_room);
    malformed_room=imported.entities[0]; malformed_room.properties["holes"]=Json::array();
    for (int i=0; i<257; ++i) malformed_room.properties["holes"].push_back(rectangle(3,4,1,1));
    verify_worker_candidate_rejected(malformed_room);
    std::string points_id;
    for (const auto& [id,record] : graph) if (record.type == "IFCCARTESIANPOINTLIST3D") points_id=id;
    auto points=graph.at(points_id); const auto position=points.fields[0].find("4.25");
    check(position != std::string::npos, "room world mesh must contain its actual elevation");
    points.fields[0].replace(position,4,"4.75");
    const auto changed=import_project_ifc(replace_ifc_record(exported.step,points_id,points));
    check(std::none_of(changed.entities.begin(),changed.entities.end(),[](const auto& e){return e.type == "room";}) &&
          changed.source_retention_required, "contradictory space mesh must not activate authored room metadata");
    // Rebase unchanged world geometry into a genuine translated/rotated local
    // frame, then compose that frame through a parent placement.
    auto rebased=exported.step;
    for (const auto& [id,record] : graph) if (record.type == "IFCCARTESIANPOINTLIST3D") {
        auto local=record; std::ostringstream coordinates; coordinates.precision(17); coordinates << '(';
        bool first=true;
        for (const auto& row : list(record.fields[0])) {
            const auto p=list(row); const double dx=std::stod(p[0])-20,dy=std::stod(p[1])-30,z=std::stod(p[2])-4;
            if (!first) coordinates << ','; first=false;
            coordinates << '(' << dy << ',' << -dx << ',' << z << ')';
        }
        coordinates << ')'; local.fields[0]=coordinates.str();
        rebased=replace_ifc_record(std::move(rebased),id,local);
    }
    auto local_space=graph.at(space_id); local_space.fields[5]="#900006";
    rebased=replace_ifc_record(std::move(rebased),space_id,local_space);
    std::string origin_id;
    for (const auto& [id,record] : graph)
        if (record.type == "IFCCARTESIANPOINT" && record.fields[0] == "(0.,0.,0.)") { origin_id=id; break; }
    check(!origin_id.empty(), "export context must contain its exact world origin");
    rebased.insert(rebased.find("ENDSEC;\nEND-ISO-10303-21;"),
        "#900001=IFCCARTESIANPOINT((20.,30.,4.));\n#900002=IFCAXIS2PLACEMENT3D(#900001,$,$);\n"
        "#900003=IFCLOCALPLACEMENT($,#900002);\n#900004=IFCDIRECTION((0.,1.,0.));\n"
        "#900005=IFCAXIS2PLACEMENT3D("+origin_id+",#900007,#900004);\n#900006=IFCLOCALPLACEMENT(#900003,#900005);\n"
        "#900007=IFCDIRECTION((0.,0.,1.));\n");
    const auto rebased_graph=records(rebased);
    check(rebased_graph.at("#900005").fields[1] == "#900007" &&
        rebased_graph.at("#900005").fields[2] == "#900004",
        "positive rigid placement must provide both the Z axis and horizontal reference direction");
    // Independently compose this explicit parent translation and child's
    // quarter-turn. Geometry comparison must pass because the actual world
    // vertices are unchanged, rather than because metadata alone is trusted.
    for (const auto& [id,record] : graph) if (record.type == "IFCCARTESIANPOINTLIST3D") {
        const auto original_rows=list(record.fields[0]);
        const auto local_rows=list(rebased_graph.at(id).fields[0]);
        check(local_rows.size() == original_rows.size(), "rebasing must preserve native mesh vertices");
        for (std::size_t i=0; i<original_rows.size(); ++i) {
            const auto world=list(original_rows[i]), local=list(local_rows[i]);
            check(std::abs(20-std::stod(local[1])-std::stod(world[0])) < 1e-10 &&
                std::abs(30+std::stod(local[0])-std::stod(world[1])) < 1e-10 &&
                std::abs(4+std::stod(local[2])-std::stod(world[2])) < 1e-10,
                "independent rigid composition must restore every original world-space vertex");
        }
    }
    const auto composed=import_project_ifc(rebased);
    check(composed.entities.size() == 1 && composed.entities[0].type == "room" && composed.entities[0].properties == room.properties,
        "proper composed rigid placement must preserve reliable native space geometry");
    verify_worker_candidate(composed,"composed rigid room placement");
    auto incomplete_axis=rebased_graph.at("#900005"); incomplete_axis.fields[1]="$";
    const auto incomplete_frame=import_project_ifc(replace_ifc_record(rebased,"#900005",incomplete_axis));
    check(std::none_of(incomplete_frame.entities.begin(),incomplete_frame.entities.end(),[](const auto& e){return e.type == "room";}) &&
        incomplete_frame.source_retention_required &&
        std::any_of(incomplete_frame.diagnostics.begin(),incomplete_frame.diagnostics.end(),[](const auto& d) {
            return d.code == "native_roof_or_room_geometry_metadata_inconsistent";
        }), "one-sided axis/reference direction must remain inert with explicit geometry inconsistency diagnosis");
    verify_worker_candidate(incomplete_frame,"one-sided rigid room axis retained candidate");
    auto moved_context=exported.step;
    for (const auto& [id,record] : graph) if (record.type == "IFCGEOMETRICREPRESENTATIONCONTEXT") {
        auto context=record; context.fields[4]="#900002";
        moved_context=replace_ifc_record(std::move(moved_context),id,context);
    }
    moved_context.insert(moved_context.find("ENDSEC;\nEND-ISO-10303-21;"),
        "#900001=IFCCARTESIANPOINT((20.,30.,4.));\n#900002=IFCAXIS2PLACEMENT3D(#900001,$,$);\n");
    const auto unsupported_context=import_project_ifc(moved_context);
    check(std::none_of(unsupported_context.entities.begin(),unsupported_context.entities.end(),[](const auto& e){return e.type == "room";}) &&
        unsupported_context.source_retention_required, "context transformations must not activate native space semantics");
    auto nonmetre=exported.step; const auto metre=nonmetre.find(".LENGTHUNIT.,$,.METRE.");
    check(metre != std::string::npos,"fixture requires metre units");
    nonmetre.replace(metre,std::string(".LENGTHUNIT.,$,.METRE.").size(),".LENGTHUNIT.,.MILLI.,.METRE.");
    const auto unsupported_units=import_project_ifc(nonmetre);
    check(std::none_of(unsupported_units.entities.begin(),unsupported_units.entities.end(),[](const auto& e){return e.type == "room";}),
        "unconverted units must not activate native space semantics");
    const auto verify_project_unit_links=[](const Entity& authored,const std::string& step) {
        const auto unit_graph=records(step);
        std::string project_id,unit_id;
        for (const auto& [id,record] : unit_graph) {
            if (record.type == "IFCPROJECT") project_id=id;
            if (record.type == "IFCSIUNIT" && record.fields[1] == ".LENGTHUNIT.") unit_id=id;
        }
        check(!project_id.empty() && !unit_id.empty(), "unit fixture must have a real project and length unit");
        const auto verify_declined=[&](const std::string& bytes,const char* fixture) {
            const auto result=import_project_ifc(bytes);
            verify_worker_candidate(result,fixture);
            check(result.entities.size() == 1 && result.entities[0].type == "ifc_reference" &&
                result.source_retention_required && std::any_of(result.diagnostics.begin(),result.diagnostics.end(),[](const auto& d) {
                    return d.code == "native_project_length_units_not_reconstructed";
                }) && result.entities[0].extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("properties") == authored.properties &&
                result.entities[0].extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("extensions") == authored.extensions,
                "unresolved actual project length units must retain the exact native carrier without activation");
        };
        auto missing_project=step;
        const auto project_begin=missing_project.find(project_id+"=");
        missing_project.erase(project_begin,missing_project.find('\n',project_begin)-project_begin+1);
        verify_declined(missing_project,"orphan metre unit without IFC project");
        auto project=unit_graph.at(project_id); project.fields[8]="$";
        verify_declined(replace_ifc_record(step,project_id,project),"missing project UnitsInContext with orphan metre");
        project.fields[8]="#960001";
        auto unrelated=replace_ifc_record(step,project_id,project);
        unrelated.insert(unrelated.find("ENDSEC;\nEND-ISO-10303-21;"),
            "#960001=IFCUNITASSIGNMENT((#960002));\n#960002=IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.);\n");
        verify_declined(unrelated,"project assignment unrelated to orphan metre length unit");
        auto ambiguous=unit_graph.at(unit_graph.at(project_id).fields[8]);
        ambiguous.fields[0]="("+unit_id+","+unit_id+")";
        verify_declined(replace_ifc_record(step,unit_graph.at(project_id).fields[8],ambiguous),
            "ambiguous duplicate project length units");
        auto unrelated_unit=step;
        unrelated_unit.insert(unrelated_unit.find("ENDSEC;\nEND-ISO-10303-21;"),
            "#960002=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n");
        const auto linked_supported=import_project_ifc(unrelated_unit);
        verify_worker_candidate(linked_supported,"valid linked metre assignment with unrelated orphan unit");
        check(linked_supported.entities.size() == 1 && linked_supported.entities[0].type == authored.type &&
            linked_supported.entities[0].properties == authored.properties &&
            std::none_of(linked_supported.diagnostics.begin(),linked_supported.diagnostics.end(),[](const auto& d) {
                return d.code == "native_project_length_units_not_reconstructed";
            }), "native units must derive from the referenced project assignment rather than unrelated declarations");
        auto alternate=unit_graph.at(project_id); alternate.fields[8]="#960001";
        auto linked_nonmetre=replace_ifc_record(step,project_id,alternate);
        linked_nonmetre.insert(linked_nonmetre.find("ENDSEC;\nEND-ISO-10303-21;"),
            "#960001=IFCUNITASSIGNMENT((#960002));\n#960002=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n");
        verify_declined(linked_nonmetre,"unsupported referenced length scale despite orphan metre");
    };
    verify_project_unit_links(room,exported.step);
    const auto unit_roof=encode_building_entity(SlopedRoofPanel{"unit-roof",{2,3,4},.3,4,3,2,
        std::atan(.5),.2,.15,{}});
    verify_project_unit_links(unit_roof,export_project_ifc(Document::create({unit_roof}).snapshot()).step);
    auto curved=room; curved.properties["holes"]=Json::array();
    const double quarter=std::acos(-1.0)/2;
    curved.properties["boundary"]=Json::array({
        {{"start",{2,0}},{"end",{0,2}},{"sweep_radians",quarter}},
        {{"start",{0,2}},{"end",{-2,0}},{"sweep_radians",quarter}},
        {{"start",{-2,0}},{"end",{0,-2}},{"sweep_radians",quarter}},
        {{"start",{0,-2}},{"end",{2,0}},{"sweep_radians",quarter}}});
    const auto curved_export=export_project_ifc(Document::create({curved}).snapshot());
    const auto curved_graph=records(curved_export.step);
    for (const auto& [id,record] : curved_graph) if (record.type == "IFCSPACE")
        check(std::abs(independently_read_mesh_volume(curved_graph,record)-10*std::acos(-1.0))<.04,
            "curved space tessellation must follow the actual analytical circular room within its stated deviation");
    const auto curved_import=import_project_ifc(curved_export.step);
    check(curved_import.entities.size() == 1 && curved_import.entities[0].type == "room" &&
        curved_import.entities[0].properties == curved.properties, "validated curved space must reconstruct exact analytical room parameters");
    auto captured_room=curved;
    captured_room.id="capture-room";
    check(captured_room.extensions.at("human").is_object() &&
        captured_room.extensions.at("human").at("nested").is_array(),
        "native-curved-room-v2 capture fixture must retain its authored human object and nested array");
    captured_room.extensions["human"]["capture"]={{"description",std::string(5000,'r')}};
    const auto room_capture=export_project_ifc(Document::create({captured_room}).snapshot());
    check(room_capture.diagnostics.empty() && room_capture.step.find("Pset_VertexExchange_v2") != std::string::npos,
        "curved room integration fixture must have valid native geometry and chunked authored metadata");
    const auto captured_import=import_project_ifc(room_capture.step);
    verify_worker_candidate(captured_import,"long chunked curved room carrier");
    check(captured_import.entities.size() == 1 && captured_import.entities[0].type == "room" &&
        captured_import.entities[0].properties == captured_room.properties &&
        captured_import.entities[0].extensions.at("human") == captured_room.extensions.at("human"),
        "long chunked room fixture must preserve exact editable authoring and nested human metadata");
    if (const auto* directory = std::getenv("VERTEX_TEST_CAPTURE_DIR")) {
        std::filesystem::create_directories(directory);
        std::ofstream output(std::filesystem::path(directory) / "native-curved-room-v2.ifc",std::ios::binary);
        output << room_capture.step;
        check(output.good(), "long curved room integration capture must be written completely");
        std::ofstream expected(std::filesystem::path(directory) / "native-curved-room-v2.expected.json",std::ios::binary);
        expected << Json{{"source_entity_id",captured_room.id},{"active_entities",Json::array({
            {{"id",captured_import.entities[0].id},{"type","room"}}})},{"diagnostic_count",captured_import.diagnostics.size()}}.dump(2);
        check(expected.good(), "room capture expectations must be written completely");
    }
    auto level_room=room;
    level_room.required=true;
    level_room.properties["layer_id"]="layer";
    level_room.properties["property_id"]="property";
    level_room.properties["building_id"]="building";
    level_room.properties["floor_id"]="floor";
    level_room.properties["parent_id"]="layer";
    level_room.properties["vertical_placement"]={{"version",1},{"mode","level"},{"offset_m",.5}};
    const VerticalLevelGraph levels({{"upper",10.0}},{});
    const auto level_document=Document::create({level_room,Entity{"property","property"},
        Entity{"building","building",{{"property_id","property"}}},
        Entity{"floor","floor",{{"building_id","building"},
            {"vertical_level_binding",VerticalLevelBinding{"levels","upper"}.to_json()}}},
        Entity{"layer","layer",{{"floor_id","floor"}}},Entity{"levels","vertical_levels",{{"model",nlohmann::json::parse(levels.serialize())}}}});
    const auto level_export=export_project_ifc(level_document.snapshot());
    const auto level_import=import_project_ifc(level_export.step);
    const auto imported_level_room=std::find_if(level_import.entities.begin(),level_import.entities.end(),
        [](const auto& e){return e.type == "room";});
    check(imported_level_room != level_import.entities.end() && imported_level_room->properties.at("elevation_m") == 14.75 &&
        !imported_level_room->required && !imported_level_room->properties.contains("vertical_placement") &&
        !imported_level_room->properties.contains("layer_id") && !imported_level_room->properties.contains("property_id") &&
        !imported_level_room->properties.contains("building_id") && !imported_level_room->properties.contains("floor_id") &&
        !imported_level_room->properties.contains("parent_id") &&
        imported_level_room->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("properties") == level_room.properties &&
        imported_level_room->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("required") == true,
        "resolved level geometry must import once with original context and authored relative placement retained exactly");
    check(std::any_of(level_import.diagnostics.begin(),level_import.diagnostics.end(),[](const auto& d) {
        return d.code == "native_context_retained_not_reconstructed";
    }), "foreign native context must be explicitly diagnosed");
    verify_worker_candidate(level_import,"detached source level room candidate");
    const auto level_repeated=import_project_ifc(export_project_ifc(Document::create(level_import.entities).snapshot()).step);
    verify_worker_candidate(level_repeated,"re-exported detached source level room candidate");
    const auto repeated_room=std::find_if(level_repeated.entities.begin(),level_repeated.entities.end(),
        [](const auto& e){return e.type == "room";});
    check(repeated_room != level_repeated.entities.end() && repeated_room->properties == imported_level_room->properties &&
        repeated_room->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("properties") == level_room.properties &&
        repeated_room->extensions.at("ifc_vertex_properties").at("_vertex_ifc_entity").at("required") == true,
        "re-export must retain exact original context and required state without reactivating either");
    check(level_document.snapshot().entities().at("room").properties == level_room.properties,
        "IFC preparation must not mutate source level authoring");
    auto invalid=room; invalid.properties.erase("height_m");
    const auto rejected=export_project_ifc(Document::create({invalid}).snapshot());
    check(rejected.step.find("=IFCSPACE(") == std::string::npos && !rejected.diagnostics.empty(),
        "historical plan-only room must retain an inert reference with explicit diagnostics instead of invented height");
    auto budget=IfcExchangeLimits{}; budget.max_mesh_vertices=10; budget.max_mesh_triangles=10;
    const auto excessive=export_project_ifc(Document::create({room}).snapshot(),budget);
    check(excessive.step.find("=IFCSPACE(") == std::string::npos && std::any_of(excessive.diagnostics.begin(),excessive.diagnostics.end(),
        [](const auto& d){return d.code == "native_mesh_budget_exceeded";}), "room meshing must respect storage limits before building");
}

void current_physical_room_spaces() {
    using namespace sketch; using Json=nlohmann::json;
    const auto wall=[](std::string id,Vec2 a,Vec2 b) {
        return Entity{std::move(id),"wall",{{"baseline",{{"start",{a.x,a.y}},{"end",{b.x,b.y}},{"sweep_radians",0}}},
            {"thickness_m",.2},{"height_m",3},{"elevation_m",7.0},{"layer_id","layer"}}};
    };
    auto document=Document::create({Entity{"property","property"},Entity{"building","building",{{"property_id","property"}}},
        Entity{"floor","floor",{{"building_id","building"}}},Entity{"layer","layer",{{"floor_id","floor"}}},
        wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0}),
        wall("island",{1,1},{3,1})});
    document.apply(prepare_physical_wall_rooms(document.snapshot(),"bottom",{0},"office"));
    const auto exported=export_project_ifc(document.snapshot());
    const auto graph=records(exported.step);
    std::size_t spaces=0;
    for (const auto& [id,record] : graph) if (record.type == "IFCSPACE") {
        ++spaces;
        const auto& placement=graph.at(record.fields[5]);
        const auto& axis=graph.at(placement.fields[1]);
        check(graph.at(axis.fields[0]).fields[0] == "(0.,0.,7.)", "physical space must retain its actual source-wall elevation");
        const auto& shape=graph.at(record.fields[6]);
        const auto& representation=graph.at(list(shape.fields[2])[0]);
        check(representation.fields[1] == "'Footprint'" && representation.fields[2] == "'Curve3D'" &&
            list(representation.fields[3]).size() == 2, "physical space must represent current net outer and island hole loops without invented height");
        double area=0;
        for (const auto& loop : list(representation.fields[3])) {
            const auto point_ids=list(graph.at(loop).fields[0]); double signed_area=0;
            for (std::size_t i=0;i+1<point_ids.size();++i) {
                const auto a=list(graph.at(point_ids[i]).fields[0]),b=list(graph.at(point_ids[i+1]).fields[0]);
                signed_area+=(std::stod(a[0])*std::stod(b[1])-std::stod(b[0])*std::stod(a[1]))/2;
            }
            area += loop == list(representation.fields[3])[0] ? std::abs(signed_area) : -std::abs(signed_area);
        }
        check(std::abs(area-10.24)<1e-7,"independent physical space net footprint must deduct the current wall island");
    }
    check(spaces == 1,"current physical room must export one genuine IFCSPACE");
    auto changed=document.snapshot().entities().at("bottom"); changed.properties["thickness_m"] = .4;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(changed)}, {},"Thicken source"});
    const auto stale=export_project_ifc(document.snapshot());
    check(stale.step.find("=IFCSPACE(") == std::string::npos && std::any_of(stale.diagnostics.begin(),stale.diagnostics.end(),
        [](const auto& d){return d.code == "physical_room_source_stale_or_runtime_unavailable";}),
        "stale calculated physical room must retain source metadata while withholding its space geometry");
}
#endif

void bounded_inert_provenance() {
    using namespace sketch;
    using Json = nlohmann::json;
    const Entity property{"source-property", "property", {{"name", "Source property"}}, false,
        {{"opaque", std::string(6000, 'p')}, {"quoted", "O'Brien"}}};
    const Entity receipt{"source-receipt", "ifc_source", {{"diagnostics", Json::array({
        {{"code", "native_reference_only"}, {"detail", std::string(6000, 'r')}}})}}};
    const auto exported = export_project_ifc(Document::create({property, receipt}).snapshot());
    check(exported.step.find("Pset_VertexExchange_v2") != std::string::npos &&
        std::none_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "vertex_properties_not_exported";
        }), "large property and source receipt descriptors must use the bounded metadata carrier");
    std::multiset<std::string> expected;
    for (const auto& entity : {property, receipt})
        expected.insert(Json{{"native_entity", {{"id", entity.id}, {"type", entity.type},
            {"required", entity.required}, {"properties", entity.properties}, {"extensions", entity.extensions}}}}.dump());
    const auto inspect = [&](const IfcProjectImportResult& imported) {
        std::multiset<std::string> descriptors;
        for (const auto& entity : imported.entities) {
            check(entity.type == "ifc_reference" && !entity.required &&
                !entity.properties.contains("property_id") && !entity.properties.contains("asset_id"),
                "large organization and receipt carriers must remain inert");
            descriptors.insert(entity.extensions.at("ifc_vertex_properties").dump());
        }
        check(imported.source_retention_required && descriptors == expected,
            "large provenance must retain exact identity, type, properties, and extensions without extras");
    };
    const auto imported = import_project_ifc(exported.step);
    inspect(imported);
    inspect(import_project_ifc(export_project_ifc(Document::create(imported.entities).snapshot()).step));
    auto quoted_property = property;
    quoted_property.id = "quoted-property";
    quoted_property.extensions["opaque"] = std::string(6000, '\'');
    const auto quoted_export = export_project_ifc(Document::create({quoted_property}).snapshot());
    const auto quoted_import = import_project_ifc(quoted_export.step);
    const Json quoted_descriptor{{"native_entity", {{"id", quoted_property.id}, {"type", quoted_property.type},
        {"required", quoted_property.required}, {"properties", quoted_property.properties},
        {"extensions", quoted_property.extensions}}}};
    check(quoted_import.entities.size() == 1 && quoted_import.entities[0].type == "ifc_reference" &&
        quoted_import.entities[0].extensions.at("ifc_vertex_properties") == quoted_descriptor,
        "apostrophe-heavy chunks must include escaped field overhead and preserve exact source metadata");
    const auto quoted_reimport = import_project_ifc(export_project_ifc(Document::create(quoted_import.entities).snapshot()).step);
    check(quoted_reimport.entities.size() == 1 && quoted_reimport.entities[0].type == "ifc_reference" &&
        quoted_reimport.entities[0].extensions.at("ifc_vertex_properties") == quoted_descriptor,
        "apostrophe-heavy inert metadata must survive repeated exact round trips");
    auto altered = exported.step;
    const auto note = altered.find(std::string(32, 'p'));
    check(note != std::string::npos, "chunk corruption fixture must find opaque source text");
    altered[note] = 'q';
    bool rejected = false;
    try { (void)import_project_ifc(altered); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "large inert provenance must reject a changed chunk without a matching hash");
    const auto graph = records(exported.step);
    std::vector<std::string> proxies;
    for (const auto& [id, record] : graph)
        if (record.type == "IFCBUILDINGELEMENTPROXY") proxies.push_back(id);
    check(proxies.size() == 2, "owner refusal fixture must have exactly two inert products");
    for (const auto& [id, record] : graph) {
        if (record.type != "IFCRELDEFINESBYPROPERTIES") continue;
        auto changed = record;
        changed.fields[4] = "(" + proxies[0] + "," + proxies[1] + ")";
        std::string row = id + '=' + changed.type + '(';
        for (const auto& field : changed.fields) { if (row.back() != '(') row += ','; row += field; }
        row += ");";
        altered = exported.step;
        const auto begin = altered.find(id + '=');
        altered.replace(begin, altered.find(';', begin) - begin + 1, row);
        rejected = false;
        try { (void)import_project_ifc(altered); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "chunked inert metadata must retain its exact single-product owner restriction");
        break;
    }
    IfcExchangeLimits restricted;
    restricted.max_string_bytes = 256;
    const auto withheld = export_project_ifc(Document::create({property}).snapshot(), restricted);
    check(std::any_of(withheld.diagnostics.begin(), withheld.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source_id == "source-property" && diagnostic.code == "vertex_properties_not_exported";
    }) && import_project_ifc(withheld.step).entities.empty(),
        "unsupported small string limits must diagnose metadata loss without inventing an empty reference");
    // A valid source bounds properties and extensions separately at 1 MiB.
    // Their combined carrier can still exceed the 4096-chunk transport budget.
    auto many_chunks = property;
    many_chunks.properties["opaque"] = std::string(600 * 1024, 'p');
    many_chunks.extensions["opaque"] = std::string(600 * 1024, 'p');
    const auto bounded_source = Document::create({many_chunks}).snapshot();
    restricted.max_string_bytes = 512;
    const auto too_large = export_project_ifc(bounded_source, restricted);
    check(std::any_of(too_large.diagnostics.begin(), too_large.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source_id == "source-property" && diagnostic.code == "vertex_properties_not_exported";
    }) && too_large.step.find("Pset_VertexExchange_v2") == std::string::npos,
        "valid inert provenance above the chunk-count budget must remain explicitly withheld");
    auto oversized = property;
    oversized.extensions["opaque"] = std::string(1024 * 1024, 'p');
    rejected = false;
    try { (void)Document::create({oversized}); }
    catch (const DocumentError& error) {
        rejected = error.code() == DocumentErrorCode::invalid_entity &&
            std::string(error.what()) == "entity extensions exceeds the encoded size limit";
    }
    check(rejected, "source descriptors must respect the authoritative document extension limit before IFC export");
    auto deep_property = property;
    deep_property.id = "deep-property";
    Json nested = std::string(6000, 'd');
    for (int i = 0; i < 40; ++i) nested = Json::array({nested});
    deep_property.extensions["nested"] = std::move(nested);
    const auto depth_limited = export_project_ifc(Document::create({deep_property, receipt}).snapshot());
    check(std::any_of(depth_limited.diagnostics.begin(), depth_limited.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source_id == "deep-property" && diagnostic.code == "vertex_properties_not_exported";
    }), "valid document metadata deeper than the IFC parser limit must be explicitly withheld");
    const auto depth_import = import_project_ifc(depth_limited.step);
    check(depth_import.entities.size() == 1 && depth_import.entities[0].type == "ifc_reference" &&
        depth_import.entities[0].extensions.at("ifc_vertex_properties").at("native_entity").at("id") == receipt.id &&
        expected.contains(depth_import.entities[0].extensions.at("ifc_vertex_properties").dump()),
        "withholding deep metadata must preserve the remaining valid source receipt");
    std::vector<Entity> budget_properties;
    std::multiset<std::string> accepted_budget_descriptors;
    for (int i = 0; i < 6; ++i) {
        auto candidate = property;
        candidate.id = "budget-" + std::to_string(i);
        candidate.properties["opaque"] = Json::array();
        for (int j = 0; j < 90000; ++j) candidate.properties["opaque"].push_back(0);
        if (i < 5) accepted_budget_descriptors.insert(Json{{"native_entity", {{"id", candidate.id},
            {"type", candidate.type}, {"required", candidate.required}, {"properties", candidate.properties},
            {"extensions", candidate.extensions}}}}.dump());
        budget_properties.push_back(std::move(candidate));
    }
    const auto budget_export = export_project_ifc(Document::create(std::move(budget_properties)).snapshot());
    check(std::count_if(budget_export.diagnostics.begin(), budget_export.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "vertex_properties_not_exported";
    }) == 1 && std::any_of(budget_export.diagnostics.begin(), budget_export.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.source_id == "budget-5" && diagnostic.code == "vertex_properties_not_exported";
    }), "export must withhold the exact descriptor exceeding the shared aggregate metadata budget");
    const auto budget_import = import_project_ifc(budget_export.step);
    std::multiset<std::string> retained_budget_descriptors;
    for (const auto& entity : budget_import.entities) {
        check(entity.type == "ifc_reference", "aggregate metadata limiting must not activate source properties");
        retained_budget_descriptors.insert(entity.extensions.at("ifc_vertex_properties").dump());
    }
    check(retained_budget_descriptors == accepted_budget_descriptors,
        "aggregate-limited export must remain importable with exactly the accepted source metadata");
}

void run() {
    using namespace sketch;
    bounded_inert_provenance();
    const auto import_case = [](const char* name, const std::string& bytes) {
        try { return import_project_ifc(bytes); }
        catch (const std::exception& error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
    };
    const auto exported = export_project_ifc(make_document().snapshot());
#ifdef SKETCH_IFC_NATIVE_GEOMETRY
    const auto native_case = [](const char* name, auto operation) {
        try { operation(); }
        catch (const std::exception& error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
    };
    native_case("roofs and spaces", native_roofs_and_spaces);
    native_case("stairs and railings", native_stairs_and_railings);
    native_case("stair export cluster proofs", native_stair_export_cluster_proofs);
    native_case("stair source context", native_stair_source_context);
    native_case("landing railings", native_landing_railing_exchange);
    native_case("physical room spaces", current_physical_room_spaces);
    check(exported.step.find("IFCDOOR(") != std::string::npos &&
          exported.step.find("IFCRELFILLSELEMENT(") != std::string::npos &&
          exported.step.find("IFCTRIANGULATEDFACESET(") != std::string::npos,
          "native door assembly must export a real fill separate from its hosted void");
#else
    const auto unavailable=export_project_ifc(Document::create({Entity{"roof","roof"},Entity{"room","room"}}).snapshot());
    check(unavailable.step.find("=IFCROOF(") == std::string::npos && unavailable.step.find("=IFCSPACE(") == std::string::npos &&
        std::count_if(unavailable.diagnostics.begin(),unavailable.diagnostics.end(),
            [](const auto& d){return d.code == "native_roof_or_room_runtime_unavailable";}) == 2,
        "unavailable native geometry must explicitly retain inert roof/room references");
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
    const auto layered_import = import_case("layered wall", layered.step);
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
    const auto oblique = import_case("oblique wall", oblique_export.step);
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
    const auto reference_import = import_case("sloped wall reference", sloped.step);
    check(reference_import.entities.size() == 1 && reference_import.entities[0].type == "ifc_reference" &&
          reference_import.entities[0].extensions.at("ifc_vertex_properties").at("native_entity").at("properties") == sloped_wall.properties &&
          reference_import.entities[0].extensions.at("ifc_vertex_properties").at("native_entity").at("extensions") == sloped_wall.extensions &&
          reference_import.source_retention_required, "unsupported native semantics must remain identifiable and retained");
    const auto repeated_reference = export_project_ifc(Document::create(reference_import.entities).snapshot());
    check(std::none_of(repeated_reference.diagnostics.begin(), repeated_reference.diagnostics.end(),
        [](const auto& item) { return item.code == "vertex_properties_not_exported"; }),
        "unchanged reference payload must remain below the retention limit on re-export");
    const auto repeated_reference_import = import_case("repeated sloped wall reference", repeated_reference.step);
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
    check(import_case("unsupported host and opening reference", hosted_reference.step).entities.size() == 2,
          "unsupported host and opening must both retain native references");

    const auto old_axis = import_case("legacy wall axis",
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
    const auto inconsistent = import_case("inconsistent wall body", inconsistent_body);
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
    const auto base_graph = records(exported.step);
    const auto spatial_relation = std::find_if(base_graph.begin(),base_graph.end(),
        [](const auto& item) { return item.second.type == "IFCRELAGGREGATES"; });
    check(spatial_relation != base_graph.end(), "relationship retention fixture needs actual spatial owners");
    auto retained_relation = spatial_relation->second;
    retained_relation.fields[0] = "'3aaaaaaaaaaaaaaaaaaaaa'";
    std::string retained_row = "#999999=IFCRELAGGREGATES(";
    for (const auto& field : retained_relation.fields) {
        if (retained_row.back() != '(') retained_row += ',';
        retained_row += field;
    }
    with_unsupported.insert(position, retained_row + ");\n");
    const auto diagnosed = import_case("unreconstructed spatial relationship", with_unsupported);
    check(diagnosed.source_retention_required, "unreconstructed relationships require source retention");
    check(std::any_of(diagnosed.diagnostics.begin(), diagnosed.diagnostics.end(), [](const auto& item) {
        return item.source_kind == "IFCRELAGGREGATES" && item.code == "relationship_not_reconstructed";
    }), "relationship gaps must be identifiable in the fidelity diagnostics");
    auto malformed_relation = exported.step;
    malformed_relation.insert(malformed_relation.find(marker), "#999999=IFCRELAGGREGATES($,$,$,$);\n");
    bool malformed_relation_rejected = false;
    try { (void)import_project_ifc(malformed_relation); }
    catch (const std::invalid_argument&) { malformed_relation_rejected = true; }
    check(malformed_relation_rejected, "malformed aggregate arity must fail closed before native host binding");

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
    native_case("door mechanisms", native_door_mechanisms);
    native_case("window layouts", native_window_layouts);
    native_case("historic fixed windows", native_historic_fixed_windows);
    native_case("assemblies", native_assemblies);
    native_case("closed door leaf", closed_leaf_without_operation);
    native_case("hosted worker protocol", desktop_hosted_worker_protocol);
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
