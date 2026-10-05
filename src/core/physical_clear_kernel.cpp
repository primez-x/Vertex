#include "sketch/physical_clear_kernel.hpp"
#include "sketch/architecture.hpp"
#include "sketch/wall_plan_network.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <NCollection_List.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr double tolerance = default_geometry_tolerance_metres;
constexpr std::size_t maximum_sources = 2048;
constexpr std::size_t maximum_edges = 16384;
constexpr std::size_t maximum_contacts = 65536;
constexpr double full_turn = 2 * std::numbers::pi;

[[noreturn]] void reject(const std::string& message) {
    throw std::invalid_argument("Physical wall space: " + message);
}
auto segment_key(const Segment& s) {
    return std::tuple{s.start.x,s.start.y,s.end.x,s.end.y,s.sweep_radians};
}
bool boundary_less(const Boundary& a, const Boundary& b) {
    return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
        [](const auto& x,const auto& y){return segment_key(x)<segment_key(y);});
}
void canonicalize(Boundary& boundary, bool counterclockwise) {
    const auto area=signed_area(boundary);
    if (!std::isfinite(area) || std::abs(area)<=tolerance*tolerance)
        reject("clear boundary collapsed or has unrepresentable area");
    if ((area>0)!=counterclockwise) {
        std::reverse(boundary.begin(),boundary.end());
        for (auto& s:boundary) { std::swap(s.start,s.end); s.sweep_radians=-s.sweep_radians; }
    }
    const auto seed=std::min_element(boundary.begin(),boundary.end(),
        [](const auto& a,const auto& b){return segment_key(a)<segment_key(b);});
    std::rotate(boundary.begin(),seed,boundary.end());
}
Boundary translated(Boundary boundary, Vec2 origin, bool to_local) {
    const auto sign=to_local ? -1.0 : 1.0;
    for (auto& s:boundary) for (auto* p:{&s.start,&s.end}) {
        p->x+=sign*origin.x; p->y+=sign*origin.y;
    }
    return boundary;
}
bool overlap(const Bounds2& a,const Bounds2& b) {
    return a.minimum.x<=b.maximum.x+tolerance && b.minimum.x<=a.maximum.x+tolerance &&
        a.minimum.y<=b.maximum.y+tolerance && b.minimum.y<=a.maximum.y+tolerance;
}
double area_tolerance(double area) { return std::max(1e-9,std::abs(area)*1e-9); }
TopoDS_Face checked_face(const Boundary& boundary,Vec2 origin) {
    const auto local=translated(boundary,origin,true);
    if (const auto issues=validate_boundary(local); !issues.empty()) reject(issues.front().message);
    const auto area=std::abs(signed_area(local));
    if (!(area>tolerance*tolerance) || !std::isfinite(area)) reject("source footprint collapsed");
    auto face=make_planar_face(local);
    if (std::abs(surface_area(face)-area)>area_tolerance(area))
        reject("source analytical area disagrees with the planar kernel");
    return face;
}
TopoDS_Shape subtract(const TopoDS_Face& face,const NCollection_List<TopoDS_Shape>& tools) {
    if (tools.IsEmpty()) return face;
    BRepAlgoAPI_Cut operation;
    NCollection_List<TopoDS_Shape> arguments; arguments.Append(face);
    operation.SetArguments(arguments); operation.SetTools(tools);
    operation.SetRunParallel(false);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors()) reject("wall material subtraction failed");
    auto result=operation.Shape();
    if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid())
        reject("wall material subtraction produced invalid geometry");
    return result;
}
struct ExtractionBudget {
    std::size_t edges{};
    std::size_t checks{};
    void edge() {
        if (++edges>maximum_edges) reject("clear geometry exceeds the analytical edge budget");
    }
    void contacts(std::size_t count) {
        if (count>maximum_contacts-checks) reject("clear geometry exceeds the topology-check budget");
        checks+=count;
    }
};
Boundary read_wire(const TopoDS_Wire& wire,const TopoDS_Face& face,ExtractionBudget& budget) {
    Boundary boundary;
    std::size_t visited=0, expected=0;
    for (TopExp_Explorer edge(wire,TopAbs_EDGE); edge.More(); edge.Next()) ++expected;
    for (BRepTools_WireExplorer explorer(wire,face); explorer.More(); explorer.Next()) {
        ++visited;
        const auto edge=explorer.Current();
        if (edge.Orientation()!=TopAbs_FORWARD && edge.Orientation()!=TopAbs_REVERSED)
            reject("clear wire has unsupported edge orientation");
        const BRepAdaptor_Curve curve(edge);
        double first=curve.FirstParameter(), last=curve.LastParameter();
        if (edge.Orientation()==TopAbs_REVERSED) std::swap(first,last);
        if (!std::isfinite(first) || !std::isfinite(last)) reject("clear edge parameter is non-finite");
        const auto start_vertex=TopExp::FirstVertex(edge,true), end_vertex=TopExp::LastVertex(edge,true);
        if (start_vertex.IsNull() || end_vertex.IsNull()) reject("clear edge has no shared endpoint vertices");
        const auto vertex_start=BRep_Tool::Pnt(start_vertex), vertex_end=BRep_Tool::Pnt(end_vertex);
        if (curve.Value(first).Distance(vertex_start)>tolerance || curve.Value(last).Distance(vertex_end)>tolerance)
            reject("shared clear intersection vertex disagrees with its analytical curve");
        double sweep=0;
        std::size_t pieces=1;
        if (curve.GetType()==GeomAbs_Circle) {
            const auto axis=curve.Circle().Axis().Direction();
            if (std::abs(std::abs(axis.Z())-1)>1e-10) reject("clear circular edge is not horizontal");
            sweep=(last-first)*(axis.Z()>0 ? 1.0 : -1.0);
            if (!std::isfinite(sweep) || std::abs(sweep)>full_turn+1e-10 || sweep==0)
                reject("clear circular edge has unsupported sweep");
            // A full circle cannot be represented as one chord arc. Two exact
            // semicircles retain the original curve, with no tessellation.
            if (std::abs(sweep)>=full_turn-1e-10) pieces=2;
        } else if (curve.GetType()!=GeomAbs_Line) reject("clear wire contains a non-line/non-circle curve");
        for (std::size_t piece=0;piece<pieces;++piece) {
            const auto a=first+(last-first)*static_cast<double>(piece)/static_cast<double>(pieces);
            const auto b=first+(last-first)*static_cast<double>(piece+1)/static_cast<double>(pieces);
            // Use the kernel's shared intersection vertices for adjoining
            // curves, rather than independently rounded curve evaluations.
            // The baseline graph remains exact and unchanged. Full-circle
            // subdivision uses the same analytical parameter at both sides.
            const auto start=piece==0 ? vertex_start : curve.Value(a);
            const auto end=piece+1==pieces ? vertex_end : curve.Value(b);
            if (!std::isfinite(start.X()) || !std::isfinite(start.Y()) || !std::isfinite(end.X()) ||
                !std::isfinite(end.Y()) || std::abs(start.Z())>tolerance || std::abs(end.Z())>tolerance)
                reject("clear wire left its finite horizontal plane");
            Segment segment{{start.X(),start.Y()},{end.X(),end.Y()},sweep/static_cast<double>(pieces)};
            if (!(segment_length(segment)>tolerance)) reject("clear wire contains a collapsed edge");
            budget.edge(); boundary.push_back(segment);
        }
    }
    if (!visited || visited!=expected) reject("clear wire is disconnected or branched");
    for (std::size_t i=0;i<boundary.size();++i) {
        const auto end=boundary[i].end, start=boundary[(i+1)%boundary.size()].start;
        if (end.x!=start.x || end.y!=start.y) reject("clear wire lacks exact shared endpoint continuity");
    }
    return boundary;
}
struct ClearComponent { Boundary outer; std::vector<Boundary> holes; double area{}; };
std::vector<ClearComponent> read_components(const TopoDS_Shape& shape,Vec2 origin,
    ExtractionBudget& budget) {
    std::vector<ClearComponent> components;
    for (TopExp_Explorer faces(shape,TopAbs_FACE); faces.More(); faces.Next()) {
        const auto face=TopoDS::Face(faces.Current());
        const auto outer=BRepTools::OuterWire(face);
        if (outer.IsNull()) reject("clear face has no outer wire");
        ClearComponent component;
        component.outer=read_wire(outer,face,budget);
        canonicalize(component.outer,true);
        for (TopExp_Explorer wires(face,TopAbs_WIRE); wires.More(); wires.Next()) {
            const auto wire=TopoDS::Wire(wires.Current());
            if (wire.IsSame(outer)) continue;
            auto hole=read_wire(wire,face,budget); canonicalize(hole,false);
            component.holes.push_back(std::move(hole));
        }
        std::sort(component.holes.begin(),component.holes.end(),boundary_less);
        std::size_t count=component.outer.size();
        for (const auto& hole:component.holes) count+=hole.size();
        budget.contacts(count*(count-1)/2);
        if (const auto error=validate_boundary_holes(component.outer,component.holes)) reject(*error);
        component.area=signed_area(component.outer);
        for (const auto& hole:component.holes) component.area-=std::abs(signed_area(hole));
        if (!std::isfinite(component.area) || !(component.area>tolerance*tolerance))
            reject("wall thickness collapses a clear component");
        if (std::abs(component.area-surface_area(face))>area_tolerance(component.area))
            reject("extracted analytical geometry disagrees with the clear planar face");
        component.outer=translated(std::move(component.outer),origin,false);
        for (auto& hole:component.holes) hole=translated(std::move(hole),origin,false);
        if (const auto error=validate_boundary_holes(component.outer,component.holes)) reject(*error);
        double represented_area=signed_area(component.outer);
        for (const auto& hole:component.holes) represented_area-=std::abs(signed_area(hole));
        if (std::abs(represented_area-component.area)>area_tolerance(component.area))
            reject("global coordinates cannot retain the analytical clear area");
        component.area=represented_area;
        components.push_back(std::move(component));
    }
    if (components.empty()) reject("wall material collapses a bounded room");
    std::sort(components.begin(),components.end(),[](const auto& a,const auto& b){return boundary_less(a.outer,b.outer);});
    double total=0; for (const auto& component:components) total+=component.area;
    if (std::abs(total-surface_area(shape))>area_tolerance(total))
        reject("clear component extraction omitted planar material");
    return components;
}
}

PhysicalClearGeometry derive_physical_clear_geometry(
    const std::vector<MeasurementGraphSource>& sources,
    const std::map<std::string,Wall,std::less<>>& input_walls) {
    try {
        if (sources.size()>maximum_sources || input_walls.size()>maximum_sources)
            reject("kernel wall count exceeds the source budget");
        if (sources.size()!=input_walls.size()) reject("graph and material wall count differs");
        std::set<std::string,std::less<>> owners;
        auto walls=input_walls;
        const auto equal=[](const Segment& a,const Segment& b) {
            return a.start.x==b.start.x && a.start.y==b.start.y && a.end.x==b.end.x &&
                a.end.y==b.end.y && a.sweep_radians==b.sweep_radians;
        };
        std::optional<double> plane;
        for (const auto& source:sources) {
            const auto found=walls.find(source.owner_id);
            if (source.segment_id!="baseline" || found==walls.end() || !owners.insert(source.owner_id).second)
                reject("graph does not uniquely name admitted physical baselines");
            auto& wall=found->second;
            if (wall.id!=source.owner_id) reject("material wall identity differs from its owner");
            validate_wall_semantics(wall);
            auto reverse=wall.baseline;
            std::swap(reverse.start,reverse.end); reverse.sweep_radians=-reverse.sweep_radians;
            if (!equal(source.geometry,wall.baseline) && !equal(source.geometry,reverse))
                reject("graph baseline differs from its material source");
            if (plane && wall.elevation!=*plane) reject("kernel requires one admitted effective plane");
            plane=wall.elevation;
            wall.openings.clear();
        }
        PhysicalClearGeometry result;
        result.graph=build_measurement_area_graph(sources);
        if (result.graph.faces.empty()) return result;
        std::map<std::string,Json,std::less<>> domains;
        for (const auto& [id,wall]:walls) { (void)wall; domains.emplace(id,Json(nullptr)); }
        const auto plans=wall_plan_network_geometry(walls,domains);
        struct Footprint { Boundary boundary; Bounds2 bounds; };
        std::vector<Footprint> footprints;
        for (const auto& [id,wall]:walls) {
            (void)wall;
            const auto plan=plans.find(id);
            if (plan==plans.end() || plan->second.footprint.empty()) reject("validated wall has no uncut physical footprint: "+id);
            footprints.push_back({plan->second.footprint,boundary_bounds(plan->second.footprint)});
        }
        std::vector<std::vector<std::size_t>> children(result.graph.faces.size());
        for (std::size_t i=0;i<result.graph.faces.size();++i)
            if (const auto parent=result.graph.faces[i].parent_face_index) children.at(*parent).push_back(i);
        std::size_t tool_count=0;
        ExtractionBudget budget;
        for (std::size_t i=0;i<result.graph.faces.size();++i) {
            const auto& baseline=result.graph.faces[i].boundary;
            const auto bounds=boundary_bounds(baseline);
            const auto origin=bounds.minimum;
            auto face=checked_face(baseline,origin);
            NCollection_List<TopoDS_Shape> tools;
            const auto append_tool=[&](const Boundary& tool) {
                if (++tool_count>maximum_contacts) reject("face/material combinations exceed the Boolean budget");
                tools.Append(checked_face(tool,origin));
            };
            for (const auto child:children[i]) {
                append_tool(result.graph.faces[child].boundary);
            }
            for (const auto& footprint:footprints) if (overlap(bounds,footprint.bounds)) append_tool(footprint.boundary);
            const auto clear=subtract(face,tools);
            const auto components=read_components(clear,origin,budget);
            if (surface_area(clear)>result.graph.faces[i].area_square_metres+area_tolerance(result.graph.faces[i].area_square_metres))
                reject("wall subtraction increased a baseline face area");
            for (std::size_t component_index=0;component_index<components.size();++component_index) {
                const auto& component=components[component_index];
                result.spaces.push_back({i,component_index,component.outer,component.holes,component.area});
            }
        }
        return result;
    } catch (const Standard_Failure& error) {
        reject(std::string("analytical planar kernel failed: ")+error.what());
    }
}
} // namespace sketch
