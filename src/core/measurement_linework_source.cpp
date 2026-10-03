#include "sketch/measurement_linework_source.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_organization.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Uses=std::vector<std::vector<MeasurementSourceUse>>;
std::string text(const Json& object,const char* key) {
    const auto& value=object.at(key);
    if(!value.is_string() || value.get_ref<const std::string&>().empty())
        throw std::invalid_argument("Measured area source identity is malformed.");
    return value.get<std::string>();
}
void sort_uses(std::vector<MeasurementSourceUse>& uses) {
    std::sort(uses.begin(),uses.end(),[](const auto& a,const auto& b) {
        return std::tie(a.owner_id,a.segment_id,a.reversed,a.parameter_start,a.parameter_end)<
            std::tie(b.owner_id,b.segment_id,b.reversed,b.parameter_start,b.parameter_end);
    });
}
Uses read_uses(const Json& value,std::size_t count) {
    if(!value.is_array() || value.size()!=count)
        throw std::invalid_argument("Measured area source lineage does not match its boundary edges.");
    Uses result;
    for(const auto& edge:value) {
        if(!edge.is_array() || edge.empty()) throw std::invalid_argument("Measured area edge has no source lineage.");
        std::vector<MeasurementSourceUse> uses;
        for(const auto& use:edge) {
            if(!use.is_object() || use.size()!=5 || !use.contains("owner_id") || !use.contains("segment_id") ||
                !use.contains("parameter_start") || !use.contains("parameter_end") || !use.contains("reversed") ||
                !use.at("parameter_start").is_number() || !use.at("parameter_end").is_number() || !use.at("reversed").is_boolean())
                throw std::invalid_argument("Measured area source lineage is malformed.");
            const auto lo=use.at("parameter_start").get<double>(),hi=use.at("parameter_end").get<double>();
            if(!std::isfinite(lo) || !std::isfinite(hi) || lo<0 || hi>1 || !(lo<hi))
                throw std::invalid_argument("Measured area source interval is invalid.");
            uses.push_back({text(use,"owner_id"),text(use,"segment_id"),lo,hi,use.at("reversed").get<bool>()});
        }
        sort_uses(uses);result.push_back(std::move(uses));
    }
    return result;
}
Uses face_uses(const MeasurementAreaGraph& graph,const DerivedMeasurementFace& face) {
    Uses result;
    for(const auto& traversal:face.edge_uses) {
        auto uses=graph.edges.at(traversal.edge_index).source_uses;
        for(auto& use:uses)use.reversed=use.reversed!=traversal.reversed;
        sort_uses(uses);result.push_back(std::move(uses));
    }
    return result;
}
Json encode_uses(const Uses& uses) {
    Json result=Json::array();
    for(const auto& edge:uses) {
        Json values=Json::array();
        for(const auto& use:edge)values.push_back({{"owner_id",use.owner_id},{"segment_id",use.segment_id},
            {"parameter_start",use.parameter_start},{"parameter_end",use.parameter_end},{"reversed",use.reversed}});
        result.push_back(std::move(values));
    }
    return result;
}
bool same_segment(const Segment& a,const Segment& b) {
    return a.start.x==b.start.x && a.start.y==b.start.y && a.end.x==b.end.x && a.end.y==b.end.y && a.sweep_radians==b.sweep_radians;
}
bool same_uses(const std::vector<MeasurementSourceUse>& a,const std::vector<MeasurementSourceUse>& b,bool intervals) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i) {
        if(a[i].owner_id!=b[i].owner_id || a[i].segment_id!=b[i].segment_id || a[i].reversed!=b[i].reversed)return false;
        if(intervals && (a[i].parameter_start!=b[i].parameter_start || a[i].parameter_end!=b[i].parameter_end))return false;
    }
    return true;
}
struct GraphState {std::optional<MeasurementAreaGraph> graph;std::string diagnostic;};
struct AlignedFace {Boundary boundary;Uses uses;};
AlignedFace align(const DerivedMeasurementFace& face,const Uses& uses,std::size_t start,bool reverse) {
    AlignedFace result;const auto count=face.boundary.size();
    for(std::size_t i=0;i<count;++i) {
        const auto index=reverse?(start+count-i)%count:(start+i)%count;
        auto edge=face.boundary[index];auto inputs=uses[index];
        if(reverse) {std::swap(edge.start,edge.end);edge.sweep_radians=-edge.sweep_radians;for(auto& use:inputs)use.reversed=!use.reversed;sort_uses(inputs);}
        result.boundary.push_back(edge);result.uses.push_back(std::move(inputs));
    }
    return result;
}
}
std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>
measurement_linework_source_checks(const std::map<std::string,Entity,std::less<>>& entities,
    const std::set<std::string,std::less<>>* semantic_visible) {
    std::map<std::string,MeasurementLineworkSourceCheck,std::less<>> result;
    if(std::none_of(entities.begin(),entities.end(),[](const auto& item){return item.second.extensions.contains("measurement_linework_sources");}))return result;
    std::optional<ProjectOrganization> organization;
    std::string organization_error;
    try{organization=organize_project(entities);}catch(const std::exception& error){organization_error=error.what();}
    std::map<std::string,GraphState,std::less<>> graphs;
    for(const auto& [id,area]:entities) if(area.extensions.contains("measurement_linework_sources")) {
        auto& check=result[id];
        try {
            if(!organization)throw std::invalid_argument(organization_error);
            if(area.type!="measurement_boundary")throw std::invalid_argument("Measured linework lineage requires a measurement boundary.");
            const auto context=organization->drawing_context(id);
            if(!context)throw std::invalid_argument("Measured area has no resolved drawing context.");
            const auto saved=boundary_geometry(decode_identified_boundary_entity(area));
            const auto expected=read_uses(area.extensions.at("measurement_linework_sources"),saved.size());
            for(const auto& edge:expected)for(const auto& use:edge) {
                const auto owner=entities.find(use.owner_id);
                if(owner==entities.end() || owner->second.type!="measurement_linework")throw std::invalid_argument("A measured area source was deleted or replaced: "+use.owner_id);
                if(organization->drawing_context(use.owner_id)!=context)throw std::invalid_argument("A measured area source moved to a different drawing context.");
                if(semantic_visible && !semantic_visible->contains(use.owner_id))throw std::invalid_argument("A measured area source is unavailable in the active design phase.");
                const auto decoded=decode_measurement_linework_model(owner->second.properties.at("model"));
                if(!decoded.supported())throw std::invalid_argument("A measured area source has an unsupported model.");
                if(std::none_of(decoded.model->edges.begin(),decoded.model->edges.end(),[&](const auto& value){return value.segment_id==use.segment_id;}))
                    throw std::invalid_argument("A measured area source segment no longer exists.");
            }
            auto found=graphs.find(context->layer_id);
            if(found==graphs.end()) {
                GraphState state;
                try {
                    std::vector<MeasurementGraphSource> sources;
                    for(const auto& [owner_id,owner]:entities) {
                        if(owner.type!="measurement_linework" || organization->drawing_context(owner_id)!=context ||
                            (semantic_visible && !semantic_visible->contains(owner_id)))continue;
                        const auto decoded=decode_measurement_linework_model(owner.properties.at("model"));
                        if(!decoded.supported())throw std::invalid_argument("The source layer contains unsupported measured geometry.");
                        for(const auto& edge:replay_measurement_linework(*decoded.model).edges)sources.push_back({owner_id,edge.segment_id,edge.segment});
                    }
                    state.graph=build_measurement_area_graph(sources);
                }catch(const std::exception& error){state.diagnostic=error.what();}
                found=graphs.emplace(context->layer_id,std::move(state)).first;
            }
            if(!found->second.graph)throw std::invalid_argument("Measured source graph could not be resolved: "+found->second.diagnostic);
            std::vector<AlignedFace> proposals;
            for(const auto& face:found->second.graph->faces) {
                if(face.boundary.size()!=saved.size())continue;
                const auto uses=face_uses(*found->second.graph,face);
                for(std::size_t start=0;start<saved.size();++start)for(const bool reverse:{false,true}) {
                    auto candidate=align(face,uses,start,reverse);
                    bool signature=true,exact=true;
                    for(std::size_t i=0;i<saved.size();++i) {
                        signature=signature && same_uses(expected[i],candidate.uses[i],false);
                        exact=exact && same_uses(expected[i],candidate.uses[i],true) && same_segment(saved[i],candidate.boundary[i]);
                    }
                    if(exact) {
                        check.current=true;check.proposed_boundary=candidate.boundary;check.proposed_lineage=encode_uses(candidate.uses);break;
                    }
                    if(signature)proposals.push_back(std::move(candidate));
                }
                if(check.current)break;
            }
            if(!check.current) {
                if(proposals.size()==1) {
                    check.proposed_boundary=std::move(proposals.front().boundary);check.proposed_lineage=encode_uses(proposals.front().uses);
                    check.diagnostic="Measured area is stale; refresh it from its current source lines.";
                } else check.diagnostic="Measured area source topology changed or face assignment is ambiguous; review and redefine the area.";
            }
        }catch(const std::exception& error){check.diagnostic=error.what();}
    }
    return result;
}
bool measurement_linework_sources_visible(const Entity& area,const std::set<std::string,std::less<>>* semantic_visible) {
    if(!area.extensions.contains("measurement_linework_sources"))return true;
    try {
        const auto uses=read_uses(area.extensions.at("measurement_linework_sources"),decode_identified_boundary_entity(area).segments.size());
        if(semantic_visible)for(const auto& edge:uses)for(const auto& use:edge)if(!semantic_visible->contains(use.owner_id))return false;
        return true;
    }catch(const std::exception&){return false;}
}
} // namespace sketch
