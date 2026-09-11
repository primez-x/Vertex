#include "sketch/geometry_operations.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("geometry operation assertion failed"); }
template<class F> void rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid operation accepted");
}
bool near(double a, double b) { return std::abs(a-b) < 1e-8; }
sketch::IdentifiedBoundary square() {
    return {"b", "boundary", {{"e0","v0","v1",{{0,0},{4,0},0}},
        {"e1","v1","v2",{{4,0},{4,3},0}}, {"e2","v2","v3",{{4,3},{0,3},0}},
        {"e3","v3","v0",{{0,3},{0,0},0}}}};
}
}
int main() {
    try {
        using namespace sketch;
        const auto source = square();
        const auto rotated = rotate_boundary(source, {1,1}, std::numbers::pi/2);
        require(near(rotated.segments[0].segment.start.x,2));
        require(near(signed_area(boundary_geometry(rotated)),12));
        require(source == square());
        const auto flipped = flip_boundary(source,{1,1},BoundaryFlipAxis::horizontal);
        require(near(signed_area(boundary_geometry(flipped)),-12));
        require(flipped.segments[0].segment_id == "e0");
        const auto inserted = insert_boundary_vertex(source,"e0",0.25,"vx","ex");
        require(inserted.segments.size()==5 && inserted.segments[0].segment.end.x==1);
        require(inserted.segments[1].start_vertex_id=="vx");
        require(near(signed_area(boundary_geometry(inserted)),12));
        rejects([&]{ (void)insert_boundary_vertex(source,"e0",0,"vx","ex"); });
        rejects([&]{ (void)insert_boundary_vertex(source,"e0",0.5,"v0","ex"); });
        rejects([&]{ (void)insert_boundary_vertex(source,"e0",0.5,"vx","e1"); });
        rejects([&]{ (void)insert_boundary_vertex(source,"missing",0.5,"vx","ex"); });
        rejects([&]{ (void)rotate_boundary(source,{0,0},std::numeric_limits<double>::infinity()); });
        auto curved=source;
        curved.segments[0].segment.sweep_radians=std::numbers::pi/2;
        const auto split=insert_boundary_vertex(curved,"e0",0.3,"vx","ex");
        require(near(perimeter(boundary_geometry(curved)),perimeter(boundary_geometry(split))));
        require(near(signed_area(boundary_geometry(curved)),signed_area(boundary_geometry(split))));
        require(flip_boundary(curved,{0,0},BoundaryFlipAxis::vertical).segments[0].segment.sweep_radians<0);
        const LegacyBoundaryIdentityOptions ids{{"a","b","c","d"},{"w","x","y","z"}};
        const auto cloned=clone_boundary(source,"copy",ids,{10,20});
        require(cloned.id=="copy" && cloned.segments[0].segment.start.x==10);
        require(near(signed_area(boundary_geometry(cloned)),12));
        rejects([&]{ (void)clone_boundary(source,"b",ids,{0,0}); });
        rejects([&]{ (void)clone_boundary(source,"copy",{}, {0,0}); });
        auto bad_ids=ids; bad_ids.vertex_ids[0]="v0";
        rejects([&]{ (void)clone_boundary(source,"copy",bad_ids,{0,0}); });
        require(jump_to_boundary_vertex(source,"v2").y==3);
        rejects([&]{ (void)jump_to_boundary_vertex(source,"missing"); });
        auto open=boundary_geometry(source); open.pop_back();
        require(automatically_close_boundary(open).size()==4);
        require(automatically_close_boundary(boundary_geometry(source)).size()==4);
        open[1].start.x+=0.00000001;
        rejects([&]{ (void)automatically_close_boundary(open); });
        rejects([&]{ (void)automatically_close_boundary({{{0,0},{1,0},0}}); });
        require(complete_bay_window({0,0},{1,-1},{3,-1},{4,0}).size()==3);
        rejects([&]{ (void)complete_bay_window({0,0},{3,-1},{1,-1},{4,0}); });
        require(apex_command_id("Auto Close")=="boundary.auto_close");
        require(apex_command_id("unknown").empty());
        require(shortcut_conflicts(apex_operation_preset()).empty());
        require(shortcut_conflicts({{"R","rotate"},{"R","flip"}})==std::vector<std::string>{"R"});
        std::cout << "geometry operations tests passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
