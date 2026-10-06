#include "sketch/assembly_geometry.hpp"
#include "sketch/architecture.hpp"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) { std::cerr << "assembly_geometry_tests: " << message << '\n'; std::exit(1); }
}
bool near(double actual, double expected) { return std::abs(actual - expected) < 1e-6; }
sketch::Boundary rectangle(double x, double y, double width, double height) {
    return {{{x, y}, {x + width, y}}, {{x + width, y}, {x + width, y + height}},
        {{x + width, y + height}, {x, y + height}}, {{x, y + height}, {x, y}}};
}
sketch::AssemblyType leaf() {
    sketch::AssemblyType type;
    type.id = "leaf"; type.name = "Leaf";
    type.materials["core"] = "wood";
    type.profiles.push_back({"panel", rectangle(0, 0, 2, 1),
        {rectangle(0.25, 0.25, 0.5, 0.5)}, 0.4, 0.2, "core"});
    return type;
}
void independent_local_solid() {
    using namespace sketch;
    AssemblyInstance instance; instance.id = "single"; instance.type_id = "leaf";
    const auto model = AssemblyModel::create({{"wood", "Wood"}}, {leaf()}, {instance});
    const auto expansion = model.expand("single");
    const auto geometry = make_assembly_geometry(expansion);
    require(geometry.solids.size() == 1 && !geometry.shape.IsNull(), "independent profile produces a solid");
    require(near(geometry.volume_m3, 0.35), "hole is removed from actual solid volume");
    const auto& solid = geometry.solids.front();
    require(solid.source.part_path.empty() && solid.source.type_id == "leaf" &&
        solid.source.profile == expansion.profiles.front().profile &&
        solid.source.transform == expansion.profiles.front().transform &&
        solid.source.material_id == "wood", "local geometry and resolved provenance retained");
    require(near(solid.volume_m3, expansion.volume_m3) &&
        near(solid_volume(solid.shape), solid.source.volume_m3), "analytical and OCCT volumes agree");
}
void nested_scaled_position() {
    using namespace sketch;
    AssemblyType parent; parent.id = "parent"; parent.name = "Parent";
    AssemblyPart first; first.id = "a"; first.type_id = "leaf";
    AssemblyPart second; second.id = "b"; second.type_id = "leaf";
    second.transform = {{10, 20, 30}, std::numbers::pi / 2, 2};
    parent.parts = {first, second};
    AssemblyInstance instance; instance.id = "pair"; instance.type_id = "parent";
    const auto model = AssemblyModel::create({{"wood", "Wood"}}, {leaf(), parent}, {instance});
    const auto expansion = model.expand("pair");
    const auto geometry = make_assembly_geometry(expansion);
    require(geometry.solids.size() == 2 && near(geometry.volume_m3, 3.15), "two leaf solids include cubic scaling");
    require(near(geometry.volume_m3, expansion.volume_m3), "nested analytical and OCCT totals agree");
    const auto& scaled = geometry.solids[1];
    require(scaled.source.part_path == std::vector<std::string>{"b"} &&
        scaled.source.transform == second.transform && near(scaled.volume_m3, 2.8), "scaled part provenance retained");
    Bnd_Box bounds; BRepBndLib::Add(scaled.shape, bounds);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    require(near(xmin, 8) && near(xmax, 10) && near(ymin, 20) && near(ymax, 24) &&
        near(zmin, 30.8) && near(zmax, 31.2), "scale then yaw then XYZ translation reaches native bounds");
    require(near(solid_volume(geometry.shape), 3.15), "compound preserves separate solid volumes");
}
void empty_and_invalid_transform() {
    using namespace sketch;
    require(make_assembly_geometry({}).shape.IsNull(), "empty expansion has no compound");
    AssemblyExpansion expansion;
    expansion.profiles.push_back({{}, "leaf", leaf().profiles.front(), {}, "wood", 0.35});
    expansion.profiles.front().transform.scale = 0;
    try { (void)make_assembly_geometry(expansion); }
    catch (const std::invalid_argument&) { return; }
    require(false, "invalid transform refused at geometry boundary");
}
} // namespace

int main() {
    independent_local_solid(); nested_scaled_position(); empty_and_invalid_transform();
    std::cout << "assembly_geometry_tests passed\n";
}
