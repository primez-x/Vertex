#include "sketch/closed_boundary_detection.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool equal_point(Vec2 left, Vec2 right) {
    return left.x==right.x && left.y==right.y;
}
bool equal_boundary(const Boundary& left, const Boundary& right) {
    if (left.size()!=right.size()) return false;
    for (std::size_t i=0;i<left.size();++i)
        if (!equal_point(left[i].start,right[i].start) ||
            !equal_point(left[i].end,right[i].end) || left[i].sweep_radians!=right[i].sweep_radians)
            return false;
    return true;
}
void require_closed_ccw(const Boundary& boundary) {
    require(!boundary.empty() && signed_area(boundary)>0,"detected face must have positive winding");
    for (std::size_t i=0;i<boundary.size();++i)
        require(equal_point(boundary[i].end,boundary[(i+1)%boundary.size()].start),
            "detected face must retain exact endpoint closure");
    require(validate_boundary(boundary).empty(),"detected face must pass analytical geometry validation");
}
template<class Operation> void rejects(Operation operation, std::string_view expected={}) {
    try { operation(); }
    catch (const std::invalid_argument& error) {
        require(expected.empty() || expected==error.what(),"detector changed its invalid-input diagnostic");
        return;
    }
    throw std::runtime_error("detector admitted invalid segment input");
}
Boundary rectangle(double x, double width) {
    return {{{x,0},{x+width,0},0},{{x+width,0},{x+width,2},0},
            {{x+width,2},{x,2},0},{{x,2},{x,0},0}};
}

void assembly_preserves_source_seed_and_refuses_incomplete_topology() {
    const Boundary unordered{{{4,3},{0,3},0},{{0,0},{4,0},0},
        {{0,3},{0,0},0},{{4,0},{4,3},0}};
    const auto assembled=assemble_boundary_from_segments(unordered,1);
    require(assembled.size()==4 && equal_point(assembled.front().start,{0,0}) &&
        equal_point(assembled.front().end,{4,0}) && std::abs(signed_area(assembled)-12)<1e-12,
        "unordered assembly lost source seed orientation or analytical area");
    require_closed_ccw(assembled);
    const auto alternate=assemble_boundary_from_segments(unordered,0);
    require(equal_point(alternate.front().start,unordered[0].start) &&
        equal_point(alternate.front().end,unordered[0].end) &&
        std::abs(perimeter(alternate)-perimeter(assembled))<1e-12,
        "assembly did not retain the requested first source segment");
    auto branched=unordered;branched.push_back({{4,0},{8,0},0});
    rejects([&]{(void)assemble_boundary_from_segments(branched);},
        "Existing segments must form one closed cycle with exactly two edges at each vertex");
    auto disconnected=unordered;const auto other=rectangle(10,2);
    disconnected.insert(disconnected.end(),other.begin(),other.end());
    rejects([&]{(void)assemble_boundary_from_segments(disconnected);},
        "Existing segments contain a disconnected cycle");
    auto gapped=unordered;gapped[3].start.x+=1e-10;
    rejects([&]{(void)assemble_boundary_from_segments(gapped);},
        "Existing segments must form one closed cycle with exactly two edges at each vertex");
    rejects([&]{(void)assemble_boundary_from_segments(unordered,unordered.size());},
        "Existing-segment seed index is out of range");
    rejects([]{(void)assemble_boundary_from_segments({{{0,0},{1,0},0}});},
        "At least three existing segments are required");
}

void shared_rectangles_ignore_stubs_and_sort_area_ties() {
    const Boundary graph{
        {{2,0},{2,2},0}, // Both faces have the same first source edge.
        {{0,0},{2,0},0},{{2,2},{0,2},0},{{0,2},{0,0},0},
        {{2,0},{6,0},0},{{6,0},{6,2},0},{{6,2},{2,2},0},
        {{2,2},{2,3},0}, // Connected unfinished wall stub.
        {{12,12},{13,12},0}}; // Disconnected unfinished geometry.
    const auto faces=detect_closed_boundaries(graph);
    require(faces.size()==2,"shared wall graph must detect exactly its two bounded rooms");
    require(std::abs(signed_area(faces[0])-4)<1e-12 && std::abs(signed_area(faces[1])-8)<1e-12,
        "faces sharing a first source edge must retain ascending area order");
    require(equal_point(faces[0].front().start,{0,0}) && equal_point(faces[1].front().start,{2,0}),
        "detected faces must retain their stable coordinate start");
    for (const auto& face:faces) {
        require(face.size()==4,"unfinished stubs must not become bounded face edges");
        require_closed_ccw(face);
    }
    auto reversed=graph;
    for (auto& edge:reversed) {
        std::swap(edge.start,edge.end);edge.sweep_radians=-edge.sweep_radians;
    }
    const auto reversed_faces=detect_closed_boundaries(reversed);
    require(reversed_faces.size()==faces.size() && equal_boundary(reversed_faces[0],faces[0]) &&
        equal_boundary(reversed_faces[1],faces[1]),"reversed graph edges changed retained bounded faces");
}

void first_source_order_precedes_coordinate_order() {
    auto graph=rectangle(10,3);
    const auto left=rectangle(0,2);
    graph.insert(graph.end(),left.begin(),left.end());
    const auto faces=detect_closed_boundaries(graph);
    require(faces.size()==2 && equal_point(faces[0].front().start,{10,0}) &&
        equal_point(faces[1].front().start,{0,0}),"disconnected faces lost first-source-edge ordering");
    require(std::abs(signed_area(faces[0])-6)<1e-12 && std::abs(signed_area(faces[1])-4)<1e-12,
        "source ordering must precede face area and stable coordinate ordering");
    auto repeat=detect_closed_boundaries(graph);
    require(equal_boundary(repeat[0],faces[0]) && equal_boundary(repeat[1],faces[1]),
        "identical graph input did not retain deterministic output");
}

void circular_edges_remain_analytical() {
    // The curved edge is supplied backwards, so detection must reverse its
    // signed sweep while retaining the exact circle and chord coordinates.
    const Boundary graph{{{2,2},{0,2},0},{{0,0},{0,2},-std::numbers::pi/2},
        {{2,0},{2,2},0},{{0,0},{2,0},0}};
    const auto faces=detect_closed_boundaries(graph);
    require(faces.size()==1 && faces.front().size()==4,"analytical curved loop must produce one four-edge face");
    const auto& face=faces.front();require_closed_ccw(face);
    const auto arc=std::find_if(face.begin(),face.end(),[](const Segment& edge){return edge.sweep_radians!=0;});
    require(arc!=face.end() && arc->sweep_radians==std::numbers::pi/2 &&
        equal_point(arc->start,{0,2}) && equal_point(arc->end,{0,0}),
        "detector changed the analytical arc chord or signed sweep");
    require(std::abs(perimeter(face)-(6+std::numbers::pi/std::sqrt(2.0)))<1e-12 &&
        std::abs(signed_area(face)-(3+std::numbers::pi/2))<1e-12,
        "detected curve lost analytical perimeter or circular-segment area");
}

void exact_closure_and_invalid_geometry_keep_existing_behavior() {
    require(detect_closed_boundaries({}).empty(),"empty graph must produce no bounded faces");
    require(detect_closed_boundaries({{{0,0},{1,0},0}}).empty(),"unfinished segment must produce no face");
    auto open=rectangle(0,2);open.pop_back();
    require(detect_closed_boundaries(open).empty(),"open chain must produce no bounded face");
    auto gapped=rectangle(0,2);gapped.back().end.x=1e-10;
    require(detect_closed_boundaries(gapped).empty(),"sub-tolerance endpoint gap must not be snapped into a graph node");
    const Boundary crossed{{{0,0},{2,2},0},{{2,2},{0,2},0},{{0,2},{2,0},0},{{2,0},{0,0},0}};
    require(detect_closed_boundaries(crossed).empty(),"self-intersecting cycle must be excluded from bounded faces");
    auto duplicate=rectangle(0,2);duplicate.push_back({{2,0},{0,0},0});
    rejects([&]{(void)detect_closed_boundaries(duplicate);},"Detected segment graph contains duplicate geometry");
    rejects([]{(void)detect_closed_boundaries({{{0,0},{0,0},0}});},"Detected segment endpoints must be distinct");
    rejects([]{(void)detect_closed_boundaries({{{0,0},{1,0},std::numeric_limits<double>::infinity()}});},
        "Detected segment sweep must be finite");
    rejects([]{(void)detect_closed_boundaries({{{std::numeric_limits<double>::quiet_NaN(),0},{1,0},0}});},
        "Point must be finite");
    rejects([]{(void)detect_closed_boundaries({{{0,0},{1e-8,0},0}});});
    rejects([]{(void)detect_closed_boundaries({{{0,0},{1,0},2*std::numbers::pi}});});
}
}

int main() {
    try {
        assembly_preserves_source_seed_and_refuses_incomplete_topology();
        shared_rectangles_ignore_stubs_and_sort_area_ties();
        first_source_order_precedes_coordinate_order();
        circular_edges_remain_analytical();
        exact_closure_and_invalid_geometry_keep_existing_behavior();
    } catch (const std::exception& error) {
        std::cerr<<"closed_boundary_detection_tests: "<<error.what()<<'\n';return 1;
    }
    std::cout<<"Document-free closed boundary detection passed\n";
}
