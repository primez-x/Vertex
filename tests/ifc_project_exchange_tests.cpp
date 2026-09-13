#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/ifc_project_exchange.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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
        return entity.type == "boundary" && entity.extensions.contains("ifc_source");
    }), "reconstructed candidates must retain IFC source metadata");
    check(std::any_of(imported.entities.begin(), imported.entities.end(), [](const auto& entity) {
        return entity.properties.value("classification", "") == "ifc_wall_axis";
    }), "wall product classification must survive import");

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
