#include "sketch/measurement_area_graph.hpp"
#include "sketch/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void require_near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-9, message);
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid measurement graph input accepted");
}
bool same(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
bool same(const Segment& a, const Segment& b) {
    return same(a.start,b.start) && same(a.end,b.end) && a.sweep_radians == b.sweep_radians;
}
MeasurementGraphSource source(const char* id, Segment geometry, const char* owner = "wall") {
    return {owner, id, geometry};
}
std::vector<MeasurementGraphSource> square() {
    return {source("a",{{0,0},{4,0},0}), source("b",{{4,0},{4,4},0}),
        source("c",{{4,4},{0,4},0}), source("d",{{0,4},{0,0},0})};
}
void check_faces(const MeasurementAreaGraph& graph, std::size_t count, double total_area) {
    require(graph.faces.size() == count, "wrong bounded face count");
    double total = 0;
    for (const auto& face : graph.faces) {
        require(face.edge_uses.size() == face.boundary.size(), "face lineage must cover every edge");
        require(validate_boundary(face.boundary).empty(), "derived face is not a simple valid boundary");
        require_near(signed_area(face.boundary), face.area_square_metres, "face area differs from analytical geometry");
        for (std::size_t i = 0; i < face.boundary.size(); ++i) {
            const auto use = face.edge_uses[i];
            require(use.edge_index < graph.edges.size(), "face references missing edge");
            auto edge = graph.edges[use.edge_index].geometry;
            if (use.reversed) { std::swap(edge.start, edge.end); edge.sweep_radians = -edge.sweep_radians; }
            require(same(edge, face.boundary[i]), "face traversal does not preserve derived edge");
        }
        total += face.area_square_metres;
    }
    require_near(total, total_area, "wrong total bounded area");
}
void crossing_and_stubs() {
    auto input = square();
    input.push_back(source("separator",{{2,-1},{2,5},0},"measurement"));
    check_faces(build_measurement_area_graph(input),2,16);
    input.push_back(source("horizontal",{{-1,2},{5,2},0},"measurement"));
    check_faces(build_measurement_area_graph(input),4,16);
    input = square();
    input.push_back(source("stub",{{2,0},{2,2},0}));
    check_faces(build_measurement_area_graph(input),1,16);
    check_faces(build_measurement_area_graph({source("x",{{0,0},{4,0},0}),
        source("t",{{2,0},{2,2},0})}),0,0);
}
void overlap_and_provenance() {
    auto input = square();
    input.push_back(source("reverse",{{4,0},{0,0},0},"other"));
    input.push_back(source("partial",{{1,0},{3,0},0},"other"));
    const auto before = input;
    const auto graph = build_measurement_area_graph(input);
    check_faces(graph,1,16);
    require(graph.edges.size() == 6, "overlapping lines must split and deduplicate");
    bool middle = false;
    for (const auto& edge : graph.edges) {
        if (same(edge.geometry.start,Vec2{1,0}) && same(edge.geometry.end,Vec2{3,0})) {
            require(edge.source_uses.size() == 3, "deduplicated overlap must retain all owners");
            for (const auto& use : edge.source_uses) {
                if (use.segment_id == "a" || use.segment_id == "reverse") {
                    require_near(use.parameter_start,.25,"wrong overlap start parameter");
                    require_near(use.parameter_end,.75,"wrong overlap end parameter");
                    require(use.reversed == (use.segment_id == "reverse"),"wrong overlap direction");
                }
            }
            middle = true;
        }
    }
    require(middle,"missing shared middle edge");
    for (std::size_t i=0;i<input.size();++i) require(same(input[i].geometry,before[i].geometry),"source geometry changed");
    std::reverse(input.begin(),input.end());
    const auto permuted = build_measurement_area_graph(input);
    require(permuted.edges.size() == graph.edges.size(),"permutation changes edge count");
    for (std::size_t i=0;i<graph.edges.size();++i) {
        require(same(graph.edges[i].geometry,permuted.edges[i].geometry),"edge order is input dependent");
        const auto& a=graph.edges[i].source_uses;
        const auto& b=permuted.edges[i].source_uses;
        require(a.size()==b.size(),"lineage changes after permutation");
        for (std::size_t j=0;j<a.size();++j) require(a[j].owner_id==b[j].owner_id &&
            a[j].segment_id==b[j].segment_id && a[j].parameter_start==b[j].parameter_start &&
            a[j].parameter_end==b[j].parameter_end && a[j].reversed==b[j].reversed,"lineage order is input dependent");
    }
}
void analytical_curves() {
    const double pi=std::numbers::pi;
    auto input=std::vector<MeasurementGraphSource>{source("arc",{{-2,0},{2,0},pi}),
        source("diameter",{{2,0},{-2,0},0}), source("cut",{{0,-3},{0,1},0})};
    const auto graph=build_measurement_area_graph(input);
    check_faces(graph,2,2*pi);
    double sweep=0;
    for (const auto& edge:graph.edges) if(edge.geometry.sweep_radians!=0) {
        sweep+=std::abs(edge.geometry.sweep_radians);
        require_near(std::abs(edge.geometry.sweep_radians),pi/2,"arc not split analytically");
    }
    require_near(sweep,pi,"arc sweep was lost");
    // Tangent line only touches the lower semicircle; it adds no enclosed area.
    input.back()=source("tangent",{{-3,-2},{3,-2},0});
    check_faces(build_measurement_area_graph(input),1,2*pi);
    // Two complete circles, represented by semicircles, enclose three regions.
    input={source("l0",{{-2,0},{2,0},pi}),source("l1",{{2,0},{-2,0},pi}),
        source("r0",{{0,0},{4,0},pi}),source("r1",{{4,0},{0,0},pi})};
    check_faces(build_measurement_area_graph(input),3,16*pi/3+2*std::sqrt(3.0));
    // Opposite semicircles form a true lens although they share endpoint pairs.
    check_faces(build_measurement_area_graph({source("lower",{{-2,0},{2,0},pi}),
        source("upper",{{2,0},{-2,0},pi})}),1,4*pi);
    // Coincident partial arcs retain source intervals and do not create a face.
    input={source("base",{{-2,0},{2,0},pi}),source("partial",{{0,-2},{2,0},pi/2})};
    const auto overlap=build_measurement_area_graph(input);
    check_faces(overlap,0,0);
    require(overlap.edges.size()==2,"coincident partial arcs were not deduplicated");
    require(std::any_of(overlap.edges.begin(),overlap.edges.end(),[](const auto& edge){return edge.source_uses.size()==2;}),"arc overlap lineage lost");
}
void strict_inputs() {
    require(build_measurement_area_graph({}).edges.empty(),"empty input should produce empty graph");
    auto input=square();
    // Same endpoint pairs with a different represented sweep are distinct
    // measurements, even when their difference is inside a roundoff band.
    rejects([] { (void)build_measurement_area_graph({
        source("semicircle", {{-2,0},{2,0},std::numbers::pi}),
        source("different", {{-2,0},{2,0},std::nextafter(std::numbers::pi,0.0)})}); });
    rejects([] { (void)build_measurement_area_graph({
        source("base", {{-2,0},{2,0},std::numbers::pi}),
        source("partial", {{0,-2},{2,0},std::nextafter(std::numbers::pi/2,0.0)})}); });
    rejects([&]{(void)build_measurement_area_graph(input,0);});
    rejects([&]{(void)build_measurement_area_graph(input,-1);});
    rejects([&]{(void)build_measurement_area_graph(input,std::numeric_limits<double>::infinity());});
    input[0].owner_id.clear(); rejects([&]{(void)build_measurement_area_graph(input);});
    input=square(); input[0].segment_id.clear(); rejects([&]{(void)build_measurement_area_graph(input);});
    input=square(); input.push_back(input[0]); rejects([&]{(void)build_measurement_area_graph(input);});
    input=square(); input[0].geometry.start.x=std::numeric_limits<double>::quiet_NaN(); rejects([&]{(void)build_measurement_area_graph(input);});
    input=square(); input[0].geometry.end=input[0].geometry.start; rejects([&]{(void)build_measurement_area_graph(input);});
    input=square(); input[0].geometry.sweep_radians=2*std::numbers::pi; rejects([&]{(void)build_measurement_area_graph(input);});
    rejects([]{(void)build_measurement_area_graph({source("tiny",{{0,0},{1e-9,0},0})});});
    // Geometric tolerance is not a license to merge nearby parallel measurements.
    const auto parallel=build_measurement_area_graph({source("one",{{0,0},{4,0},0}),
        source("two",{{0,1e-8},{4,1e-8},0})});
    require(parallel.edges.size()==2,"nearby parallel lines were silently merged");
    // Heavy retracing must hit a bounded contact budget before unbounded
    // station work, even though its final deduplicated edge count is tiny.
    std::vector<MeasurementGraphSource> repeated;
    for (int i=0;i<300;++i)
        repeated.push_back({"retrace", "edge-"+std::to_string(i), {{0,0},{4,0},0}});
    rejects([&] { (void)build_measurement_area_graph(repeated); });
    // A split that produces an unresolvable tiny piece fails explicitly.
    rejects([]{(void)build_measurement_area_graph({source("base",{{0,0},{4,0},0}),
        source("tiny-cut",{{1e-9,-1},{1e-9,1},0})});});
}
}
int main() {
    sketch::runtime::configure_noninteractive_errors();
    try {
        crossing_and_stubs(); overlap_and_provenance(); analytical_curves(); strict_inputs();
        std::cout << "measurement area graph tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
