#include "sketch/physical_clear_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Walls = std::map<std::string, Wall, std::less<>>;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
void assert_near(double actual, double expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-9,
            "clear kernel known area disagrees");
}
void wall(Walls& walls, std::string id, Segment baseline, double thickness = .2) {
    walls.emplace(id, Wall{id, baseline, thickness, 3.0, 0.0, {}, {}});
}
Walls box() {
    Walls result;
    wall(result, "bottom", {{0,0},{4,0},0});
    wall(result, "right", {{4,0},{4,3},0});
    wall(result, "top", {{4,3},{0,3},0});
    wall(result, "left", {{0,3},{0,0},0});
    return result;
}
std::vector<MeasurementGraphSource> sources(const Walls& walls) {
    std::vector<MeasurementGraphSource> result;
    for (const auto& [id, value] : walls) result.push_back({id,"baseline",value.baseline});
    return result;
}
void exact(const PhysicalClearGeometry& geometry) {
    for (const auto& space : geometry.spaces) {
        const auto check = [](const Boundary& boundary) {
            for (std::size_t i=0; i<boundary.size(); ++i) {
                const auto a=boundary[i].end, b=boundary[(i+1)%boundary.size()].start;
                require(a.x==b.x && a.y==b.y, "kernel endpoints must join exactly");
            }
        };
        check(space.boundary);
        for (const auto& hole : space.holes) check(hole);
        require(!validate_boundary_holes(space.boundary, space.holes), "kernel loops must validate");
    }
}
void rooms_and_material() {
    auto walls=box();
    auto result=derive_physical_clear_geometry(sources(walls), walls);
    require(result.spaces.size()==1, "box has one clear component");
    assert_near(result.spaces.front().area_square_metres, 10.64);
    wall(walls, "island", {{1,1.5},{3,1.5},0});
    auto baselines=sources(walls);
    result=derive_physical_clear_geometry(baselines, walls);
    require(result.spaces.size()==1 && result.spaces.front().holes.size()==1,
            "isolated material creates a room hole");
    assert_near(result.spaces.front().area_square_metres, 10.24);
    exact(result);
    std::reverse(baselines.begin(), baselines.end());
    const auto reordered=derive_physical_clear_geometry(baselines, walls);
    require(reordered.spaces.size()==1, "source order cannot change component count");
    assert_near(reordered.spaces.front().area_square_metres, 10.24);
    exact(reordered);
    // Uncut material deliberately retains a room limit across door thresholds.
    walls.at("bottom").openings.push_back({"door", 1.0, .9, 0.0, 2.0});
    const auto doorway=derive_physical_clear_geometry(baselines, walls);
    assert_near(doorway.spaces.front().area_square_metres, 10.24);
}
void curved_and_open() {
    Walls walls;
    wall(walls, "bottom", {{0,0},{4,0},0});
    wall(walls, "right", {{4,0},{4,3},std::numbers::pi});
    wall(walls, "top", {{4,3},{0,3},0});
    wall(walls, "left", {{0,3},{0,0},std::numbers::pi});
    const auto curved=derive_physical_clear_geometry(sources(walls), walls);
    require(curved.spaces.size()==1, "capsule forms one clear space");
    assert_near(curved.spaces.front().area_square_metres, 4*2.8+std::numbers::pi*1.4*1.4);
    require(std::any_of(curved.spaces.front().boundary.begin(), curved.spaces.front().boundary.end(),
        [](const auto& edge){return edge.sweep_radians!=0;}), "kernel retains analytical arcs");
    exact(curved);
    walls=box(); walls.erase("right");
    require(derive_physical_clear_geometry(sources(walls), walls).spaces.empty(), "open chain has no clear room");
}
void reject_mismatched_sources() {
    auto walls=box(); auto baselines=sources(walls);
    baselines.front().geometry.start.x+=.5;
    bool rejected=false;
    try { (void)derive_physical_clear_geometry(baselines, walls); }
    catch (const std::invalid_argument&) { rejected=true; }
    require(rejected, "graph and material cannot come from different physical sources");
}
}
int main() {
    try { rooms_and_material(); curved_and_open(); reject_mismatched_sources();
        std::cout << "physical_clear_kernel_tests: pass\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
