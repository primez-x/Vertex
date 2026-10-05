#include "sketch/measurement_area_graph.hpp"
#include "sketch/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
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
template<class Face> std::optional<std::size_t> parent_of(const Face& face) {
    if constexpr (requires { face.parent_face_index; }) return face.parent_face_index;
    else throw std::runtime_error("derived measured face must expose its immediate containing parent");
}
void append_rectangle(std::vector<MeasurementGraphSource>& input, const std::string& owner,
                      Vec2 minimum, Vec2 maximum) {
    const Vec2 points[]{minimum,{maximum.x,minimum.y},maximum,{minimum.x,maximum.y}};
    for (std::size_t i=0;i<4;++i) input.push_back({owner,"edge-"+std::to_string(i),{points[i],points[(i+1)%4],0}});
}
std::size_t face_with_area(const MeasurementAreaGraph& graph,double area) {
    const auto found=std::find_if(graph.faces.begin(),graph.faces.end(),[&](const auto& face){return std::abs(face.area_square_metres-area)<1e-9;});
    require(found!=graph.faces.end(),"missing exact gross measured outline");
    return static_cast<std::size_t>(found-graph.faces.begin());
}
template<class Graph> DerivedMeasurementFace combine_for_test(const Graph& graph,const std::vector<std::size_t>& indices) {
    if constexpr (requires { combine_measurement_faces(graph,indices); }) return combine_measurement_faces(graph,indices);
    else throw std::runtime_error("adjacent detected measured faces must support exact analytical combination");
}
void check_combined(const MeasurementAreaGraph& graph,const DerivedMeasurementFace& face,double area) {
    require(!face.parent_face_index,"combined face must not inherit a source containment parent");
    check_faces(MeasurementAreaGraph{graph.edges,{face}},1,area);
    require(face.area_square_metres>0,"combined loop must be counter-clockwise");
}
std::size_t face_at(const MeasurementAreaGraph& graph,Vec2 minimum) {
    const auto found=std::find_if(graph.faces.begin(),graph.faces.end(),[&](const auto& face) {
        return same(boundary_bounds(face.boundary).minimum,minimum);
    });
    require(found!=graph.faces.end(),"missing fixture graph cell"); return static_cast<std::size_t>(found-graph.faces.begin());
}
void combine_adjacent_faces() {
    std::vector<MeasurementGraphSource> input; append_rectangle(input,"left",{0,0},{2,2}); append_rectangle(input,"right",{2,0},{4,2});
    const auto graph=build_measurement_area_graph(input); check_faces(graph,2,8);
    const auto combined=combine_for_test(graph,{0,1}); check_combined(graph,combined,8);
    require(combined.boundary.size()==6,"combine must cancel exactly the shared edge without merging measured collinear edges");
    const auto permuted=combine_for_test(graph,{1,0});
    require(permuted.boundary.size()==combined.boundary.size(),"selection order changes combined outline size");
    for (std::size_t i=0;i<combined.boundary.size();++i)
        require(same(combined.boundary[i],permuted.boundary[i]) && combined.edge_uses[i].edge_index==permuted.edge_uses[i].edge_index &&
            combined.edge_uses[i].reversed==permuted.edge_uses[i].reversed,"selection order changes exact combined geometry or lineage");
    std::reverse(input.begin(),input.end());
    const auto reordered_graph=build_measurement_area_graph(input);
    const auto reordered=combine_for_test(reordered_graph,{1,0});
    require(reordered.boundary.size()==combined.boundary.size(),"source order changes combined outline size");
    for (std::size_t i=0;i<combined.boundary.size();++i)
        require(same(reordered.boundary[i],combined.boundary[i]) && reordered.edge_uses[i].edge_index==combined.edge_uses[i].edge_index &&
            reordered.edge_uses[i].reversed==combined.edge_uses[i].reversed,"source permutation changes combined analytical lineage");
    input=square(); input.push_back(source("vertical",{{2,0},{2,4},0})); input.push_back(source("horizontal",{{0,2},{4,2},0}));
    const auto quarters=build_measurement_area_graph(input); check_faces(quarters,4,16);
    const auto whole=combine_for_test(quarters,{0,1,2,3}); check_combined(quarters,whole,16);
    require(whole.boundary.size()==8,"four regions retain all measured outer pieces after internal cancellation");
    const auto concave=combine_for_test(quarters,{face_at(quarters,{0,0}),face_at(quarters,{2,0}),face_at(quarters,{0,2})});
    check_combined(quarters,concave,12); require(concave.boundary.size()==8,"L combine retains its exact concave outer traversal");
}
void combine_analytical_curves() {
    const double pi=std::numbers::pi;
    const auto graph=build_measurement_area_graph({source("lower",{{-2,0},{2,0},pi}),
        source("upper",{{2,0},{-2,0},pi}),source("divider",{{-2,0},{2,0},pi/2})});
    check_faces(graph,2,4*pi);
    const auto combined=combine_for_test(graph,{1,0}); check_combined(graph,combined,4*pi);
    double sweep=0;
    for (std::size_t i=0;i<combined.boundary.size();++i) {
        require(combined.boundary[i].sweep_radians!=0,"combined curved outline must remain analytical arcs");
        sweep+=combined.boundary[i].sweep_radians;
        const auto& edge=graph.edges[combined.edge_uses[i].edge_index];
        require(std::none_of(edge.source_uses.begin(),edge.source_uses.end(),[](const auto& use){return use.segment_id=="divider";}),
            "oppositely traversed curved divider must be cancelled by graph edge identity");
    }
    require_near(sweep,2*pi,"combined circle must preserve its exact outer sweep");
}
void combine_rejections() {
    auto input=square(); input.push_back(source("vertical",{{2,0},{2,4},0})); input.push_back(source("horizontal",{{0,2},{4,2},0}));
    const auto graph=build_measurement_area_graph(input);
    rejects([&] { (void)combine_for_test(graph,{}); }); rejects([&] { (void)combine_for_test(graph,{0}); });
    rejects([&] { (void)combine_for_test(graph,{0,0}); }); rejects([&] { (void)combine_for_test(graph,{0,graph.faces.size()}); });
    rejects([&] { (void)combine_for_test(graph,{face_at(graph,{0,0}),face_at(graph,{2,2})}); });
    input.clear(); append_rectangle(input,"first",{0,0},{2,2}); append_rectangle(input,"second",{4,0},{6,2});
    const auto disjoint=build_measurement_area_graph(input); rejects([&] { (void)combine_for_test(disjoint,{0,1}); });
    input.clear(); append_rectangle(input,"outer",{0,0},{10,10}); append_rectangle(input,"inner",{2,2},{6,6});
    const auto nested=build_measurement_area_graph(input); rejects([&] { (void)combine_for_test(nested,{0,1}); });
    input.clear(); append_rectangle(input,"frame",{0,0},{3,3});
    for (int station=1;station<3;++station) {
        input.push_back({"vertical","edge-"+std::to_string(station),{{double(station),0},{double(station),3},0}});
        input.push_back({"horizontal","edge-"+std::to_string(station),{{0,double(station)},{3,double(station)},0}});
    }
    const auto grid=build_measurement_area_graph(input); check_faces(grid,9,9);
    std::vector<std::size_t> ring; const auto center=face_at(grid,{1,1});
    for (std::size_t i=0;i<grid.faces.size();++i) if (i!=center) ring.push_back(i);
    rejects([&] { (void)combine_for_test(grid,ring); });
    auto malformed=graph; malformed.faces[0].boundary[0].start.x+=.25;
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.faces[0].edge_uses[0].edge_index=graph.edges.size();
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.faces[0].area_square_metres+=1;
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.faces[0].edge_uses.pop_back();
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.edges[0].source_uses.clear();
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.edges[0].source_uses[0].parameter_end=std::numeric_limits<double>::quiet_NaN();
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.faces[0].parent_face_index=graph.faces.size();
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    // Malformed unselected evidence is still rejected at the graph boundary.
    malformed=graph; malformed.faces[3].boundary[0].end.x+=.25;
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
    malformed=graph; malformed.edges.resize(16385);
    rejects([&] { (void)combine_for_test(malformed,{0,1}); });
}
void nested_outlines() {
    std::vector<MeasurementGraphSource> input;
    append_rectangle(input,"outer",{0,0},{10,10});
    append_rectangle(input,"inner",{2,2},{6,6});
    auto graph=build_measurement_area_graph(input);
    check_faces(graph,2,116);
    auto outer=face_with_area(graph,100),inner=face_with_area(graph,16);
    require(!parent_of(graph.faces[outer]) && parent_of(graph.faces[inner])==outer,
        "inner sixteen outline must retain outer hundred as immediate strict parent");
    append_rectangle(input,"deeper",{3,3},{4,4});
    append_rectangle(input,"sibling",{7,7},{9,9});
    append_rectangle(input,"separate",{20,20},{23,23});
    graph=build_measurement_area_graph(input); check_faces(graph,5,130);
    outer=face_with_area(graph,100); inner=face_with_area(graph,16);
    require(parent_of(graph.faces[inner])==outer && parent_of(graph.faces[face_with_area(graph,1)])==inner &&
        parent_of(graph.faces[face_with_area(graph,4)])==outer && !parent_of(graph.faces[face_with_area(graph,9)]),
        "deeper outlines and siblings choose immediate parent while separate outline stays root");
    const auto before=input; std::reverse(input.begin(),input.end());
    const auto permuted=build_measurement_area_graph(input);
    require(permuted.faces.size()==graph.faces.size() && permuted.edges.size()==graph.edges.size(),"nested permutation changes graph size");
    for (std::size_t i=0;i<graph.faces.size();++i) {
        const auto& left=graph.faces[i]; const auto& right=permuted.faces[i];
        require(parent_of(left)==parent_of(right) && left.area_square_metres==right.area_square_metres && left.boundary.size()==right.boundary.size(),
            "final parent indexes and gross areas must be deterministic under source permutation");
        for (std::size_t j=0;j<left.boundary.size();++j)
            require(same(left.boundary[j],right.boundary[j]) && left.edge_uses[j].edge_index==right.edge_uses[j].edge_index &&
                left.edge_uses[j].reversed==right.edge_uses[j].reversed,"nested permutation loses exact geometry or lineage");
    }
    for (std::size_t i=0;i<input.size();++i) require(same(input[input.size()-1-i].geometry,before[i].geometry),"nested source geometry was modified");
    for (const auto& edge:graph.edges) {
        require(edge.source_uses.size()==1,"separate nested outline must retain exact source lineage");
        require(edge.source_uses.front().parameter_start==0 && edge.source_uses.front().parameter_end==1,
            "nested straight outline must preserve its complete original source interval");
    }
    input.clear(); append_rectangle(input,"frame",{0,0},{10,10});
    append_rectangle(input,"room-left",{2,2},{4,4}); append_rectangle(input,"room-right",{4,2},{6,4});
    const auto subdivision=build_measurement_area_graph(input); check_faces(subdivision,3,108);
    const auto frame=face_with_area(subdivision,100);
    require(!parent_of(subdivision.faces[frame]),"enclosing outline stays root around an adjacent subdivision");
    for (std::size_t i=0;i<subdivision.faces.size();++i) if (i!=frame)
        require(parent_of(subdivision.faces[i])==frame,"strict containing outline can parent each adjacent inner graph region");
}
void nested_curves_and_separate_bounds() {
    const double pi=std::numbers::pi;
    std::vector<MeasurementGraphSource> input{
        source("lower",{{0,5},{10,5},pi},"outer-circle"),source("upper",{{10,5},{0,5},pi},"outer-circle"),
        source("lower",{{3,5},{7,5},pi},"inner-circle"),source("upper",{{7,5},{3,5},pi},"inner-circle")};
    append_rectangle(input,"center",{4.5,4.5},{5.5,5.5});
    const auto graph=build_measurement_area_graph(input); check_faces(graph,3,29*pi+1);
    const auto outer=face_with_area(graph,25*pi),inner=face_with_area(graph,4*pi),center=face_with_area(graph,1);
    require(!parent_of(graph.faces[outer]) && parent_of(graph.faces[inner])==outer && parent_of(graph.faces[center])==inner,
        "analytic curved outlines choose immediate parent without subtracting gross area");
    double outer_sweep=0,inner_sweep=0;
    for (const auto& edge:graph.edges) if (edge.geometry.sweep_radians!=0) {
        require(edge.source_uses.size()==1,"nested arc lineage changed");
        const auto& use=edge.source_uses.front();
        require(use.parameter_end-use.parameter_start==.5,"circle subdivision preserves exact original half-arc interval");
        if (use.owner_id=="outer-circle") outer_sweep+=std::abs(edge.geometry.sweep_radians);
        else if (use.owner_id=="inner-circle") inner_sweep+=std::abs(edge.geometry.sweep_radians);
        else require(false,"nested arc has unknown source owner");
    }
    require_near(outer_sweep,2*pi,"outer circle sweep was lost"); require_near(inner_sweep,2*pi,"inner circle sweep was lost");
    // Their bounding boxes overlap; the complete analytical outlines do not.
    input={source("lower",{{-2,0},{2,0},pi},"first"),source("upper",{{2,0},{-2,0},pi},"first"),
        source("lower",{{1,3},{5,3},pi},"second"),source("upper",{{5,3},{1,3},pi},"second")};
    const auto separate=build_measurement_area_graph(input); check_faces(separate,2,8*pi);
    require(std::all_of(separate.faces.begin(),separate.faces.end(),[](const auto& face){return !parent_of(face);}),
        "overlapping bounds cannot create guessed containment");
}
void touching_and_ambiguous_outlines() {
    std::vector<MeasurementGraphSource> input;
    append_rectangle(input,"first",{0,0},{4,4}); append_rectangle(input,"corner-touch",{4,4},{6,6});
    rejects([&] { (void)build_measurement_area_graph(input); });
    input.clear(); append_rectangle(input,"outer",{0,0},{10,10}); append_rectangle(input,"near-inner",{5e-8,2},{2,4});
    rejects([&] { (void)build_measurement_area_graph(input); });
    // Near a curved extremum, vertex-only tests would falsely claim separation.
    input={source("arc",{{3,0},{1,0},std::numbers::pi},"half-disk"),source("base",{{1,0},{3,0},0},"half-disk")};
    append_rectangle(input,"near-arc",{1.9999,.9},{2.0001,1-5e-8});
    rejects([&] { (void)build_measurement_area_graph(input); });
    input.resize(2); append_rectangle(input,"clear-arc",{1.9999,.9},{2.0001,1-5e-7});
    const auto clear=build_measurement_area_graph(input);
    require(clear.faces.size()==2,"unambiguous curved containment must remain supported");
    const auto parent=face_with_area(clear,std::numbers::pi/2);
    require(!parent_of(clear.faces[parent]) && parent_of(clear.faces[1-parent])==parent,
        "curved clearance above tolerance proves an exact containing parent");
}
void crossing_and_stubs() {
    auto input = square();
    input.push_back(source("separator",{{2,-1},{2,5},0},"measurement"));
    check_faces(build_measurement_area_graph(input),2,16);
    input.push_back(source("horizontal",{{-1,2},{5,2},0},"measurement"));
    const auto crossing=build_measurement_area_graph(input); check_faces(crossing,4,16);
    require(std::all_of(crossing.faces.begin(),crossing.faces.end(),[](const auto& face){return !parent_of(face);}),
        "shared-edge crossing faces including diagonal vertex contacts remain roots");
    std::vector<MeasurementGraphSource> adjacent; append_rectangle(adjacent,"left",{0,0},{2,2}); append_rectangle(adjacent,"right",{2,0},{4,2});
    const auto shared=build_measurement_area_graph(adjacent); check_faces(shared,2,8);
    require(std::all_of(shared.faces.begin(),shared.faces.end(),[](const auto& face){return !parent_of(face);}),
        "adjacent shared-edge outlines remain roots");
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
void rotated_separated_supports() {
    std::vector<MeasurementGraphSource> input;
    append_rectangle(input,"room",{0,0},{4,3});
    input.push_back({"divider","baseline",{{2,0},{2,3},0}});
    const double c=std::cos(0.37), s=std::sin(0.37);
    for (auto& wall:input) for (auto* p:{&wall.geometry.start,&wall.geometry.end}) {
        const auto old=*p; *p={c*old.x-s*old.y,s*old.x+c*old.y};
    }
    const auto graph=build_measurement_area_graph(input);
    check_faces(graph,2,12);
    for (const auto& face:graph.faces) require_near(face.area_square_metres,6,
        "rotated separated wall supports changed partition area");
    require(graph.edges.size()==7,"rotated partition lost original source topology");
    // Tiny determinant alone cannot prove a non-contact. An actual interior
    // crossing inside that arithmetic band must still fail explicitly.
    rejects([]{(void)build_measurement_area_graph({
        source("a",{{0,0},{4,4},0}),
        source("b",{{0,1e-14},{4,4-1e-14},0})});});
    // These represented endpoints are exactly collinear (y=1.5*x), but
    // subtracting their common endpoint rounds the directions differently.
    // A noisy determinant must not turn their true overlap into one contact.
    try {
        (void)build_measurement_area_graph({
            source("long",{{1,1.5},{9007199254740988.0,13510798882111482.0},0}),
            source("short",{{1,1.5},{4503599627370492.0,6755399441055738.0},0})});
    } catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find("nearly parallel")!=std::string::npos,
            "rounded collinear overlap must fail at the uncertain line proof");
        return;
    }
    throw std::runtime_error("rounded collinear overlap was reduced to one shared endpoint");
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
    std::vector<MeasurementGraphSource> too_many;
    for (int i=0;i<2049;++i) too_many.push_back({"limit","edge-"+std::to_string(i),{{0,0},{4,0},0}});
    rejects([&] { (void)build_measurement_area_graph(too_many); });
    // A split that produces an unresolvable tiny piece fails explicitly.
    rejects([]{(void)build_measurement_area_graph({source("base",{{0,0},{4,0},0}),
        source("tiny-cut",{{1e-9,-1},{1e-9,1},0})});});
}
}
int main() {
    sketch::runtime::configure_noninteractive_errors();
    try {
        combine_adjacent_faces(); combine_analytical_curves(); combine_rejections();
        nested_outlines(); nested_curves_and_separate_bounds(); touching_and_ambiguous_outlines();
        crossing_and_stubs(); overlap_and_provenance(); analytical_curves();
        rotated_separated_supports(); strict_inputs();
        std::cout << "measurement area graph tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
