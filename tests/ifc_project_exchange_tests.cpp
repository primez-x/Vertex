#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/ifc_project_exchange.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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
    check(exported.step.find("ISO-10303-21;") == 0, "IFC export must have a STEP envelope");
    check(exported.step.find("FILE_SCHEMA(('IFC4'))") != std::string::npos,
          "IFC export must declare IFC4");
    check(exported.step.find("IFCWALLSTANDARDCASE") != std::string::npos,
          "wall axis must export as an IFC wall product");
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
    check(!exported.diagnostics.empty(), "lossy wall axis metadata must be diagnosed");
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
    const auto corner = retraced.find("IFCCARTESIANPOINT((2,0.10000000000000001,0.))");
    check(corner != std::string::npos, "fixture must contain opening corner");
    retraced.replace(corner, std::string("IFCCARTESIANPOINT((2,0.10000000000000001,0.))").size(),
                     "IFCCARTESIANPOINT((1,-0.10000000000000001,0.))");
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
