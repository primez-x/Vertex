#include "sketch/boundary_entity.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/document.hpp"
#include "sketch/ifc_project_exchange.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
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
    for (const auto& [id, record] : graph) {
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

void run() {
    using namespace sketch;
    const auto exported = export_project_ifc(make_document().snapshot());
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
    check(std::any_of(exported.diagnostics.begin(), exported.diagnostics.end(), [](const auto& item) {
        return item.source_id == "opening-1" && item.code == "opening_assembly_not_exported";
    }), "opening assembly loss must be explicit in the IFC fidelity report");

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
    check(opening.properties.at("opening_kind") == "door" &&
          opening.extensions.at("ifc_vertex_properties").contains("opening_assembly"),
          "opening kind must recover while unmapped assembly metadata remains retained");
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
        return entity.type == "boundary";
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
        return entity.type == "boundary";
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
    const auto corner = retraced.find("IFCCARTESIANPOINT((2,0.10000000000000001))");
    check(corner != std::string::npos, "fixture must contain opening corner");
    retraced.replace(corner, std::string("IFCCARTESIANPOINT((2,0.10000000000000001))").size(),
                     "IFCCARTESIANPOINT((1,-0.10000000000000001))");
    const auto invalid_rectangle = import_project_ifc(retraced);
    check(std::none_of(invalid_rectangle.entities.begin(), invalid_rectangle.entities.end(), [](const auto& entity) {
        return entity.type == "opening";
    }), "retraced rectangle edges must not become an editable opening");

    bool rejected = false;
    try { (void)import_project_ifc("ISO-10303-21;\nDATA;\nENDSEC;\n"); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "malformed IFC must fail closed");
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
