#include "sketch/physical_wall_spaces.hpp"

#include "sketch/architecture.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/document_wall_plan.hpp"
#include "sketch/model_phases.hpp"

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
constexpr std::size_t maximum_lineage_bytes = 16 * 1024 * 1024;
constexpr double full_turn = 2 * std::numbers::pi;

[[noreturn]] void reject(const std::string& message) {
    throw std::invalid_argument("Physical wall space: " + message);
}
Json segment_json(const Segment& segment) {
    return {{"start",{segment.start.x,segment.start.y}},
        {"end",{segment.end.x,segment.end.y}}, {"sweep_radians",segment.sweep_radians}};
}
auto point_key(Vec2 p) { return std::pair{p.x,p.y}; }
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
Json face_lineage(const MeasurementAreaGraph& graph,std::size_t index) {
    Json edges=Json::array();
    std::size_t entries=0, estimated_bytes=0;
    for (const auto& edge_use:graph.faces.at(index).edge_uses) {
        Json uses=Json::array();
        for (const auto& source:graph.edges.at(edge_use.edge_index).source_uses) {
            const auto bytes=6*(source.owner_id.size()+source.segment_id.size())+256;
            if (++entries>maximum_contacts || bytes>maximum_lineage_bytes-estimated_bytes)
                reject("baseline provenance exceeds the lineage budget");
            estimated_bytes+=bytes;
            uses.push_back({{"owner_id",source.owner_id},{"segment_id",source.segment_id},
                {"parameter_start",source.parameter_start},{"parameter_end",source.parameter_end},
                {"reversed",source.reversed!=edge_use.reversed}});
        }
        edges.push_back({{"edge_index",edge_use.edge_index},{"source_uses",std::move(uses)}});
    }
    return {{"baseline_face_index",index},{"edges",std::move(edges)}};
}
Wall read_wall(const Entity& entity) {
    Wall wall; std::string error;
    if (!read_document_wall(entity,{},wall,error)) reject("wall "+entity.id+": "+error);
    validate_wall_semantics(wall); return wall;
}
}

PhysicalWallSpaces detect_physical_wall_spaces(const DocumentSnapshot& document,
    std::string_view selected_wall_id) {
    try {
        const auto& entities=document.entities();
        const auto selected=entities.find(selected_wall_id);
        if (selected==entities.end() || selected->second.type!="wall") reject("selected source is not a wall");
        const auto organization=organize_project(document);
        const auto context=organization.drawing_context(selected->first);
        if (!context || !context->complete()) reject("selected wall needs a complete drawing context");
        std::set<std::string,std::less<>> context_owners;
        for (const auto& [id,entity]:entities) {
            if (entity.type!="wall") continue;
            const auto candidate_context=organization.drawing_context(id);
            if (!candidate_context || !candidate_context->complete() || *candidate_context!=*context) continue;
            if (context_owners.size()==maximum_sources) reject("context wall count exceeds the source budget");
            context_owners.insert(id);
        }
        std::set<std::string,std::less<>> inactive;
        Json phase_models=Json::array();
        std::size_t source_snapshot_bytes=0;
        std::size_t source_snapshot_entries=0;
        const auto snapshot_budget=[&](const Json& record) {
            const auto bytes=record.dump().size();
            if (++source_snapshot_entries>maximum_contacts || bytes>maximum_lineage_bytes-source_snapshot_bytes)
                reject("source snapshots exceed the lineage entry/byte budget");
            source_snapshot_bytes+=bytes;
        };
        for (const auto& [id,entity]:entities) {
            if (entity.type!="model_phases") continue;
            const auto phases=ModelPhases::from_json(entity.properties.at("model"));
            const auto active=phases.active_state();
            const std::set<std::string,std::less<>> registered(phases.entity_ids().begin(),phases.entity_ids().end());
            Json relevant=Json::array();
            for (const auto& owner:context_owners) {
                if (!registered.contains(owner)) continue;
                const auto state=active.find(owner);
                if (state==active.end() || state->second==ModelPhase::demolished) inactive.insert(owner);
                Json record{{"owner_id",owner},{"active_state",state==active.end() ? "absent" : phase_name(state->second)}};
                snapshot_budget(record); relevant.push_back(std::move(record));
            }
            if (!relevant.empty()) {
                Json record{{"id",id},{"active_alternative",phases.active_alternative() ? Json(*phases.active_alternative()) : Json(nullptr)},
                    {"owners",std::move(relevant)}};
                snapshot_budget(record); phase_models.push_back(std::move(record));
            }
        }
        if (inactive.contains(selected->first)) reject("selected wall is inactive in the semantic phase");
        std::vector<std::string> candidate_ids;
        for (const auto& id:context_owners) if (!inactive.contains(id)) candidate_ids.push_back(id);
        const auto placements=resolve_vertical_placements(entities,candidate_ids);
        const auto selected_wall=read_wall(placements.at(selected->first));
        std::map<std::string,Wall,std::less<>> walls;
        std::map<std::string,Entity,std::less<>> uncut_entities;
        std::vector<MeasurementGraphSource> sources;
        Json physical_sources=Json::array();
        double minimum_plane=selected_wall.elevation,maximum_plane=selected_wall.elevation;
        for (const auto& id:candidate_ids) {
            auto wall=read_wall(placements.at(id));
            if (std::abs(wall.elevation-selected_wall.elevation)>tolerance) continue;
            minimum_plane=std::min(minimum_plane,wall.elevation); maximum_plane=std::max(maximum_plane,wall.elevation);
            if (maximum_plane-minimum_plane>tolerance) reject("wall effective planes form an ambiguous tolerance chain");
            sources.push_back({id,"baseline",wall.baseline});
            const auto raw_wall=read_wall(entities.at(id));
            Json snapshot{{"owner_id",id},{"segment_id","baseline"},
                {"baseline",segment_json(wall.baseline)},{"thickness_m",wall.thickness},
                {"source_elevation_m",raw_wall.elevation},{"effective_elevation_m",wall.elevation},
                {"source_context",{{"property_id",entities.at(id).properties.value("property_id",Json(nullptr))},
                    {"building_id",entities.at(id).properties.value("building_id",Json(nullptr))},
                    {"floor_id",entities.at(id).properties.value("floor_id",Json(nullptr))},
                    {"layer_id",entities.at(id).properties.value("layer_id",Json(nullptr))}}},
                {"vertical_placement",entities.at(id).properties.value("vertical_placement",Json(nullptr))}};
            snapshot_budget(snapshot); physical_sources.push_back(std::move(snapshot));
            // Same resolved hierarchy is the material contact authority. Using
            // derived copies avoids differences in redundant persisted context
            // references or existing/proposed labels suppressing real joins.
            auto physical=placements.at(id);
            physical.properties["property_id"]=context->property_id;
            physical.properties["building_id"]=context->building_id;
            physical.properties["floor_id"]=context->floor_id;
            physical.properties["layer_id"]=context->layer_id;
            physical.properties.erase("phase_id");
            uncut_entities.emplace(id,std::move(physical));
            wall.openings.clear();
            wall.elevation=selected_wall.elevation;
            if (point_key(wall.baseline.end)<point_key(wall.baseline.start)) {
                std::swap(wall.baseline.start,wall.baseline.end); wall.baseline.sweep_radians=-wall.baseline.sweep_radians;
            }
            walls.emplace(id,std::move(wall));
        }
        PhysicalWallSpaces result; result.context=*context;
        result.graph=build_measurement_area_graph(sources);
        if (result.graph.faces.empty()) return result;
        const auto plans=document_wall_plan_geometry(uncut_entities,walls);
        struct Footprint { Boundary boundary; Bounds2 bounds; };
        std::vector<Footprint> footprints;
        for (const auto& [id,wall]:walls) {
            (void)wall;
            const auto plan=plans.find(id);
            if (plan==plans.end() || plan->second.footprint.empty()) reject("validated wall has no uncut physical footprint: "+id);
            footprints.push_back({plan->second.footprint,boundary_bounds(plan->second.footprint)});
        }
        Json context_json{{"property_id",context->property_id},{"building_id",context->building_id},
            {"floor_id",context->floor_id},{"layer_id",context->layer_id},{"level_id",context->level_id}};
        Json common_lineage{{"version",1},{"basis","physical_wall_clear"},{"context",std::move(context_json)},
            {"physical_sources",std::move(physical_sources)},{"semantic_phases",std::move(phase_models)}};
        const auto common_lineage_bytes=common_lineage.dump().size();
        if (common_lineage_bytes>maximum_lineage_bytes) reject("common lineage exceeds the byte budget");
        std::vector<std::vector<std::size_t>> children(result.graph.faces.size());
        for (std::size_t i=0;i<result.graph.faces.size();++i)
            if (const auto parent=result.graph.faces[i].parent_face_index) children.at(*parent).push_back(i);
        std::size_t tool_count=0;
        std::size_t lineage_owner_count=0;
        std::size_t emitted_lineage_bytes=0;
        ExtractionBudget budget;
        for (std::size_t i=0;i<result.graph.faces.size();++i) {
            const auto& baseline=result.graph.faces[i].boundary;
            const auto bounds=boundary_bounds(baseline);
            const auto origin=bounds.minimum;
            auto face=checked_face(baseline,origin);
            NCollection_List<TopoDS_Shape> tools;
            Json holes_lineage=Json::array();
            const auto append_tool=[&](const Boundary& tool) {
                if (++tool_count>maximum_contacts) reject("face/material combinations exceed the Boolean budget");
                tools.Append(checked_face(tool,origin));
            };
            for (const auto child:children[i]) {
                append_tool(result.graph.faces[child].boundary);
                holes_lineage.push_back(face_lineage(result.graph,child));
            }
            for (const auto& footprint:footprints) if (overlap(bounds,footprint.bounds)) append_tool(footprint.boundary);
            const auto clear=subtract(face,tools);
            const auto components=read_components(clear,origin,budget);
            if (surface_area(clear)>result.graph.faces[i].area_square_metres+area_tolerance(result.graph.faces[i].area_square_metres))
                reject("wall subtraction increased a baseline face area");
            const auto outer_lineage=face_lineage(result.graph,i);
            const auto encoded_bytes=common_lineage_bytes+outer_lineage.dump().size()+holes_lineage.dump().size()+128;
            for (std::size_t component_index=0;component_index<components.size();++component_index) {
                if (sources.size()>maximum_contacts-lineage_owner_count)
                    reject("clear source snapshots exceed the lineage budget");
                lineage_owner_count+=sources.size();
                if (encoded_bytes>maximum_lineage_bytes-emitted_lineage_bytes)
                    reject("emitted room lineage exceeds the aggregate byte budget");
                emitted_lineage_bytes+=encoded_bytes;
                const auto& component=components[component_index];
                // Bound aggregate encoded payload before copying the shared
                // source snapshots into each independent returned component.
                Json lineage=common_lineage;
                lineage["outer"]=outer_lineage; lineage["holes"]=holes_lineage;
                lineage["component_index"]=component_index;
                result.spaces.push_back({i,component.outer,component.holes,component.area,std::move(lineage)});
            }
        }
        return result;
    } catch (const Standard_Failure& error) {
        reject(std::string("analytical planar kernel failed: ")+error.what());
    } catch (const Json::exception& error) {
        reject(std::string("malformed wall/context/phase source: ")+error.what());
    }
}
} // namespace sketch
