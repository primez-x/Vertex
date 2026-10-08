#include "sketch/physical_wall_spaces.hpp"
#include "sketch/physical_clear_kernel.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json=nlohmann::json;
constexpr double tolerance=default_geometry_tolerance_metres;
constexpr std::size_t maximum_sources=2048;
constexpr std::size_t maximum_contacts=65536;
constexpr std::size_t maximum_lineage_bytes=16*1024*1024;
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Physical wall space: "+reason);
}
Json segment_json(const Segment& segment) {
    return {{"start",{segment.start.x,segment.start.y}},
        {"end",{segment.end.x,segment.end.y}}, {"sweep_radians",segment.sweep_radians}};
}
auto point_key(Vec2 p) { return std::pair{p.x,p.y}; }
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
PhysicalWallSpaces discover_spaces(
    const std::map<std::string,Entity,std::less<>>& entities,
    std::string_view selected_wall_id, const DrawingContext* supplied_context,
    double effective_elevation_m, const PhysicalWallPhaseSelection* selection=nullptr) {
    try {
        const auto selected=entities.find(selected_wall_id);
        if (!supplied_context && (selected==entities.end() || selected->second.type!="wall"))
            reject("selected source is not a wall");
        const auto organization=organize_project(entities);
        const auto context=organization.drawing_context(supplied_context ? supplied_context->layer_id : selected->first);
        if (supplied_context) {
            const auto layer=entities.find(supplied_context->layer_id);
            if (!supplied_context->complete() || layer==entities.end() || layer->second.type!="layer" ||
                !context || *context!=*supplied_context)
                reject("supplied drawing context must resolve to its exact layer hierarchy");
            if (!std::isfinite(effective_elevation_m)) reject("effective elevation must be finite");
        }
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
        const auto capture_phase=[&](const std::string& id,
            const std::optional<std::string>& alternative,
            const std::vector<std::string>& registered,
            const std::map<std::string,ModelPhase,std::less<>>& active) {
            Json relevant=Json::array();
            for (const auto& owner:context_owners) {
                if (!std::binary_search(registered.begin(),registered.end(),owner)) continue;
                const auto state=active.find(owner);
                if (state==active.end() || state->second==ModelPhase::demolished) inactive.insert(owner);
                Json record{{"owner_id",owner},{"active_state",state==active.end() ? "absent" : phase_name(state->second)}};
                snapshot_budget(record); relevant.push_back(std::move(record));
            }
            if (!relevant.empty()) {
                Json record{{"id",id},{"active_alternative",alternative ? Json(*alternative) : Json(nullptr)},
                    {"owners",std::move(relevant)}};
                snapshot_budget(record); phase_models.push_back(std::move(record));
            }
        };
        if (selection) {
            for (const auto& phase:physical_wall_phase_states(entities,*selection))
                capture_phase(phase.registry_id,phase.alternative_id,phase.registered_entity_ids,phase.states);
        } else {
            for (const auto& [id,entity]:entities) {
                if (entity.type!="model_phases") continue;
                const auto phases=ModelPhases::from_json(entity.properties.at("model"));
                capture_phase(id,phases.active_alternative(),phases.entity_ids(),phases.active_state());
            }
        }
        if (!supplied_context && inactive.contains(selected->first)) reject("selected wall is inactive in the semantic phase");
        std::vector<std::string> candidate_ids;
        for (const auto& id:context_owners) if (!inactive.contains(id)) candidate_ids.push_back(id);
        const auto placements=resolve_vertical_placements(entities,candidate_ids);
        const auto plane=supplied_context ? effective_elevation_m : read_wall(placements.at(selected->first)).elevation;
        std::map<std::string,Wall,std::less<>> walls;
        std::vector<MeasurementGraphSource> sources;
        Json physical_sources=Json::array();
        double minimum_plane=plane,maximum_plane=plane;
        for (const auto& id:candidate_ids) {
            auto wall=read_wall(placements.at(id));
            if (std::abs(wall.elevation-plane)>tolerance) continue;
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
            wall.openings.clear();
            wall.elevation=plane;
            if (point_key(wall.baseline.end)<point_key(wall.baseline.start)) {
                std::swap(wall.baseline.start,wall.baseline.end); wall.baseline.sweep_radians=-wall.baseline.sweep_radians;
            }
            walls.emplace(id,std::move(wall));
        }
        PhysicalWallSpaces result; result.context=*context;
        if (sources.empty()) return result;
        auto geometry=derive_physical_clear_geometry(sources,walls);
        result.graph=std::move(geometry.graph);
        if (result.graph.faces.empty()) return result;
        Json context_json{{"property_id",context->property_id},{"building_id",context->building_id},
            {"floor_id",context->floor_id},{"layer_id",context->layer_id},{"level_id",context->level_id}};
        Json common_lineage{{"version",1},{"basis","physical_wall_clear"},{"context",std::move(context_json)},
            {"physical_sources",std::move(physical_sources)},{"semantic_phases",std::move(phase_models)}};
        const auto common_lineage_bytes=common_lineage.dump().size();
        if (common_lineage_bytes>maximum_lineage_bytes) reject("common lineage exceeds the byte budget");
        std::vector<std::vector<std::size_t>> children(result.graph.faces.size());
        for (std::size_t i=0;i<result.graph.faces.size();++i)
            if (const auto parent=result.graph.faces[i].parent_face_index) children.at(*parent).push_back(i);
        std::size_t lineage_owner_count=0,emitted_lineage_bytes=0;
        for (auto& component:geometry.spaces) {
            const auto i=component.baseline_face_index;
            const auto outer_lineage=face_lineage(result.graph,i);
            Json holes_lineage=Json::array();
            for (const auto child:children.at(i)) holes_lineage.push_back(face_lineage(result.graph,child));
            const auto encoded_bytes=common_lineage_bytes+outer_lineage.dump().size()+holes_lineage.dump().size()+128;
            if (sources.size()>maximum_contacts-lineage_owner_count)
                reject("clear source snapshots exceed the lineage budget");
            lineage_owner_count+=sources.size();
            if (encoded_bytes>maximum_lineage_bytes-emitted_lineage_bytes)
                reject("emitted room lineage exceeds the aggregate byte budget");
            emitted_lineage_bytes+=encoded_bytes;
            Json lineage=common_lineage;
            lineage["outer"]=outer_lineage; lineage["holes"]=std::move(holes_lineage);
            lineage["component_index"]=component.component_index;
            result.spaces.push_back({i,std::move(component.boundary),std::move(component.holes),
                component.area_square_metres,std::move(lineage)});
        }
        return result;
    } catch (const Json::exception& error) {
        reject(std::string("malformed wall/context/phase source: ")+error.what());
    }
}
} // namespace

PhysicalWallSpaces detect_physical_wall_spaces(
    const std::map<std::string,Entity,std::less<>>& entities,
    std::string_view selected_wall_id) {
    return discover_spaces(entities,selected_wall_id,nullptr,0.0);
}

PhysicalWallSpaces detect_physical_wall_spaces(
    const std::map<std::string,Entity,std::less<>>& entities,
    const DrawingContext& context, double effective_elevation_m) {
    return discover_spaces(entities,{},&context,effective_elevation_m);
}

PhysicalWallSpaces detect_physical_wall_spaces(
    const std::map<std::string,Entity,std::less<>>& entities,
    const DrawingContext& context, double effective_elevation_m,
    const PhysicalWallPhaseSelection& selection) {
    return discover_spaces(entities,{},&context,effective_elevation_m,&selection);
}
} // namespace sketch
