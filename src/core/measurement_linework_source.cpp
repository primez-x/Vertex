#include "sketch/measurement_linework_source.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/model_phases.hpp"
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
std::vector<Uses> read_group(const Json& value) {
    if(!value.is_object() || value.size()!=2 || !value.contains("version") ||
       !value.at("version").is_number_integer() || value.at("version")!=1 || !value.contains("members") ||
       !value.at("members").is_array() || value.at("members").size()<2 || value.at("members").size()>2048)
        throw std::invalid_argument("Measured area group schema or version is unsupported.");
    std::size_t edges=0,uses=0;
    for(const auto& member:value.at("members")) {
        if(!member.is_array() || member.empty() || member.size()>16384-edges)
            throw std::invalid_argument("Measured area group exceeds its member edge budget.");
        edges+=member.size();
        for(const auto& edge:member) {
            if(!edge.is_array() || edge.size()>65536-uses)
                throw std::invalid_argument("Measured area group exceeds its source use budget.");
            uses+=edge.size();
        }
    }
    std::vector<Uses> result;
    std::set<std::string> member_keys;
    for(const auto& member:value.at("members")) {
        auto parsed=read_uses(member,member.size());
        for(const auto& edge:parsed)for(std::size_t i=1;i<edge.size();++i)
            if(same_uses({edge[i-1]},{edge[i]},true))
                throw std::invalid_argument("Measured area group contains duplicate source uses.");
        if(!member_keys.insert(encode_uses(parsed).dump()).second)
            throw std::invalid_argument("Measured area group contains duplicate members.");
        result.push_back(std::move(parsed));
    }
    return result;
}
Uses read_group_outer(const Json& value,std::size_t count) {
    if(count>16384 || !value.is_array() || value.size()>16384)
        throw std::invalid_argument("Measured area group outer lineage exceeds its edge budget.");
    std::size_t uses=0;
    for(const auto& edge:value) {
        if(!edge.is_array() || edge.size()>65536-uses)
            throw std::invalid_argument("Measured area group outer lineage exceeds its source use budget.");
        uses+=edge.size();
    }
    return read_uses(value,count);
}
struct GroupMatchWork {
    std::size_t remaining=2'000'000;
    void charge(std::size_t amount) {
        if(amount>remaining)throw std::invalid_argument("Measured area group matching work budget exhausted; review and redefine a smaller group.");
        remaining-=amount;
    }
    std::size_t face_weight(const MeasurementAreaGraph& graph,const DerivedMeasurementFace& face) {
        std::size_t result=face.edge_uses.size();
        for(const auto& edge:face.edge_uses) {
            const auto size=graph.edges.at(edge.edge_index).source_uses.size();
            if(size>remaining || result>remaining-size) {
                charge(remaining+1);
            }
            result+=size;
        }
        return result;
    }
};
Json encode_group(const std::vector<Uses>& members) {
    std::vector<Json> ordered;
    for(const auto& member:members)ordered.push_back(encode_uses(member));
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.dump()<b.dump();});
    return {{"version",1},{"members",ordered}};
}
bool has_source(const Entity& area) {
    return area.extensions.contains("measurement_linework_sources") ||
        (area.type=="measurement_boundary" && area.extensions.contains("measurement_linework_group"));
}
struct GraphState {std::optional<MeasurementAreaGraph> graph;std::string diagnostic;std::size_t last_use{};};
using SourceOwners=std::set<std::string,std::less<>>;
using CohortKey=std::pair<std::string,SourceOwners>;
// Bound additional resident graphs and total reconstruction work independently
// of the unchanged legacy per-layer path. Failed builds consume work too.
struct CohortWork {
    static constexpr std::size_t maximum_cached_graphs=16;
    std::size_t builds_remaining=256;
    std::size_t sources_remaining=65536;
    std::size_t comparisons_remaining=16'000'000;
    void begin() {
        if(!builds_remaining)throw std::invalid_argument("Measured copy source cohort graph build budget exhausted.");
        --builds_remaining;
    }
    void charge_sources(std::size_t count) {
        if(count>sources_remaining)throw std::invalid_argument("Measured copy source cohort segment budget exhausted.");
        sources_remaining-=count;
    }
    void charge_graph(std::size_t count) {
        // The caller has already enforced the graph's 2048-segment limit.
        const auto comparisons=count<2 ? 0 : count*(count-1)/2;
        if(comparisons>comparisons_remaining)
            throw std::invalid_argument("Measured copy source cohort contact work budget exhausted.");
        comparisons_remaining-=comparisons;
    }
};
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
bool measurement_linework_copy_isolated(const Entity& entity) {
    if(!entity.extensions.contains("measurement_linework_copy_scope"))return false;
    const auto& marker=entity.extensions.at("measurement_linework_copy_scope");
    if(entity.type!="measurement_linework" || !marker.is_object() || marker.size()!=1 ||
       !marker.contains("version") || !marker.at("version").is_number_integer() || marker.at("version")!=1)
        throw std::invalid_argument("Measured linework copy scope schema, version or owner type is unsupported.");
    return true;
}
std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>
measurement_linework_source_checks(const std::map<std::string,Entity,std::less<>>& entities,
    const std::set<std::string,std::less<>>* semantic_visible) {
    std::map<std::string,MeasurementLineworkSourceCheck,std::less<>> result;
    if(std::none_of(entities.begin(),entities.end(),[](const auto& item){return has_source(item.second);}))return result;
    std::optional<ProjectOrganization> organization;
    std::string organization_error;
    try{organization=organize_project(entities);}catch(const std::exception& error){organization_error=error.what();}
    SourceOwners copy_owners;
    std::string copy_scope_error;
    try {
        for(const auto& [id,entity]:entities)
            if(measurement_linework_copy_isolated(entity))copy_owners.insert(id);
    }catch(const std::exception& error){copy_scope_error=error.what();}
    std::map<std::string,GraphState,std::less<>> graphs;
    std::map<CohortKey,GraphState> cohort_graphs;
    CohortWork cohort_work;
    std::size_t cohort_access=0;
    for(const auto& [id,area]:entities) if(has_source(area)) {
        auto& check=result[id];
        try {
            if(!organization)throw std::invalid_argument(organization_error);
            if(!copy_scope_error.empty())throw std::invalid_argument(copy_scope_error);
            if(area.type!="measurement_boundary")throw std::invalid_argument("Measured linework lineage requires a measurement boundary.");
            const auto context=organization->drawing_context(id);
            if(!context)throw std::invalid_argument("Measured area has no resolved drawing context.");
            const auto saved=boundary_geometry(decode_identified_boundary_entity(area));
            const bool grouped=area.type=="measurement_boundary" && area.extensions.contains("measurement_linework_group");
            const auto members=grouped ? read_group(area.extensions.at("measurement_linework_group")) : std::vector<Uses>{};
            if(!area.extensions.contains("measurement_linework_sources"))
                throw std::invalid_argument("Measured area group requires retained outer source lineage.");
            const auto expected=grouped ? read_group_outer(area.extensions.at("measurement_linework_sources"),saved.size()) :
                read_uses(area.extensions.at("measurement_linework_sources"),saved.size());
            // An owner is immutable for this area's complete lineage check.
            // Reuse its validated segment identities instead of decoding the
            // same canonical stroke for every exterior source use. Ordinary
            // areas retain at most 16 owners; grouped checks keep their
            // existing complete member-validation cache.
            std::map<std::string,std::set<std::string>,std::less<>> validated_sources;
            SourceOwners source_owners;
            const auto validate_sources=[&](const Uses& inputs) {
            for(const auto& edge:inputs)for(const auto& use:edge) {
                source_owners.insert(use.owner_id);
                const auto validated=validated_sources.find(use.owner_id);
                if(validated!=validated_sources.end()) {
                    if(!validated->second.contains(use.segment_id))
                        throw std::invalid_argument("A measured area source segment no longer exists.");
                    continue;
                }
                const auto owner=entities.find(use.owner_id);
                if(owner==entities.end() || owner->second.type!="measurement_linework")throw std::invalid_argument("A measured area source was deleted or replaced: "+use.owner_id);
                if(organization->drawing_context(use.owner_id)!=context)throw std::invalid_argument("A measured area source moved to a different drawing context.");
                if(semantic_visible && !semantic_visible->contains(use.owner_id))throw std::invalid_argument("A measured area source is unavailable in the active design phase.");
                const auto decoded=decode_measurement_linework_model(owner->second.properties.at("model"));
                if(!decoded.supported())throw std::invalid_argument("A measured area source has an unsupported model.");
                if(std::none_of(decoded.model->edges.begin(),decoded.model->edges.end(),[&](const auto& value){return value.segment_id==use.segment_id;}))
                    throw std::invalid_argument("A measured area source segment no longer exists.");
                if(grouped || validated_sources.size()<16) {
                    auto& ids=validated_sources[use.owner_id];
                    for(const auto& segment:decoded.model->edges)ids.insert(segment.segment_id);
                }
            }
            };
            validate_sources(expected);
            for(const auto& member:members)validate_sources(member);
            const bool isolated=std::any_of(source_owners.begin(),source_owners.end(),
                [&](const auto& owner){return copy_owners.contains(owner);});
            GraphState* selected_graph=nullptr;
            if(isolated) {
                const CohortKey key{context->layer_id,source_owners};
                auto found=cohort_graphs.find(key);
                if(found==cohort_graphs.end()) {
                    GraphState state;
                    try {
                        cohort_work.begin();
                        std::vector<MeasurementGraphSource> sources;
                        for(const auto& owner_id:source_owners) {
                            const auto& owner=entities.at(owner_id);
                            const auto decoded=decode_measurement_linework_model(owner.properties.at("model"));
                            if(!decoded.supported())throw std::invalid_argument("A measured area source has an unsupported model.");
                            if(decoded.model->edges.size()>2048-sources.size())
                                throw std::invalid_argument("Measurement area graph: source limit of 2048 segments exceeded");
                            cohort_work.charge_sources(decoded.model->edges.size());
                            for(const auto& edge:replay_measurement_linework(*decoded.model).edges)
                                sources.push_back({owner_id,edge.segment_id,edge.segment});
                        }
                        cohort_work.charge_graph(sources.size());
                        state.graph=build_measurement_area_graph(sources);
                    }catch(const std::exception& error){state.diagnostic=error.what();}
                    if(cohort_graphs.size()==CohortWork::maximum_cached_graphs) {
                        const auto oldest=std::min_element(cohort_graphs.begin(),cohort_graphs.end(),
                            [](const auto& a,const auto& b){return a.second.last_use<b.second.last_use;});
                        cohort_graphs.erase(oldest);
                    }
                    found=cohort_graphs.emplace(key,std::move(state)).first;
                }
                found->second.last_use=++cohort_access;
                selected_graph=&found->second;
            } else {
                auto found=graphs.find(context->layer_id);
                if(found==graphs.end()) {
                    GraphState state;
                    try {
                        std::vector<MeasurementGraphSource> sources;
                        for(const auto& [owner_id,owner]:entities) {
                            if(owner.type!="measurement_linework" || organization->drawing_context(owner_id)!=context ||
                                (semantic_visible && !semantic_visible->contains(owner_id)))continue;
                            if(copy_owners.contains(owner_id))continue;
                            const auto decoded=decode_measurement_linework_model(owner.properties.at("model"));
                            if(!decoded.supported())throw std::invalid_argument("The source layer contains unsupported measured geometry.");
                            for(const auto& edge:replay_measurement_linework(*decoded.model).edges)sources.push_back({owner_id,edge.segment_id,edge.segment});
                        }
                        state.graph=build_measurement_area_graph(sources);
                    }catch(const std::exception& error){state.diagnostic=error.what();}
                    found=graphs.emplace(context->layer_id,std::move(state)).first;
                }
                selected_graph=&found->second;
            }
            if(!selected_graph->graph)throw std::invalid_argument("Measured source graph could not be resolved: "+selected_graph->diagnostic);
            if(grouped) {
                const auto& graph=*selected_graph->graph;
                GroupMatchWork work;
                std::set<std::size_t> assigned;
                std::vector<Uses> proposed_members;
                bool members_exact=true;
                for(const auto& member:members) {
                    struct MemberMatch {std::size_t face;Uses uses;};
                    std::vector<MemberMatch> matches;
                    for(std::size_t face_index=0;face_index<graph.faces.size();++face_index) {
                        work.charge(1);
                        const auto& face=graph.faces[face_index];
                        if(face.boundary.size()!=member.size())continue;
                        const auto weight=work.face_weight(graph,face);
                        work.charge(weight);
                        const auto uses=face_uses(graph,face);
                        for(std::size_t start=0;start<member.size();++start)for(const bool reverse:{false,true}) {
                            work.charge(weight);
                            auto candidate=align(face,uses,start,reverse);
                            bool signature=true;
                            for(std::size_t i=0;i<member.size();++i)signature=signature && same_uses(member[i],candidate.uses[i],false);
                            if(signature)matches.push_back({face_index,std::move(candidate.uses)});
                        }
                    }
                    if(matches.size()!=1 || !assigned.insert(matches.front().face).second)
                        throw std::invalid_argument("Measured group member assignment is missing, duplicate or ambiguous.");
                    for(std::size_t i=0;i<member.size();++i)
                        members_exact=members_exact && same_uses(member[i],matches.front().uses[i],true);
                    proposed_members.push_back(std::move(matches.front().uses));
                }
                const std::vector<std::size_t> indices(assigned.begin(),assigned.end());
                const auto combined=combine_measurement_faces(graph,indices);
                if(combined.boundary.size()!=saved.size())
                    throw std::invalid_argument("Measured group outer topology changed; review and redefine the area.");
                const auto outer_weight=work.face_weight(graph,combined);
                work.charge(outer_weight);
                const auto outer_uses=face_uses(graph,combined);
                std::vector<AlignedFace> proposals;
                for(std::size_t start=0;start<saved.size();++start)for(const bool reverse:{false,true}) {
                    work.charge(outer_weight);
                    auto candidate=align(combined,outer_uses,start,reverse);
                    bool signature=true;
                    for(std::size_t i=0;i<saved.size();++i)signature=signature && same_uses(expected[i],candidate.uses[i],false);
                    if(signature)proposals.push_back(std::move(candidate));
                }
                if(proposals.size()!=1)
                    throw std::invalid_argument("Measured group outer lineage does not uniquely match its member union.");
                auto& candidate=proposals.front();
                bool exact=members_exact;
                for(std::size_t i=0;i<saved.size();++i)
                    exact=exact && same_uses(expected[i],candidate.uses[i],true) && same_segment(saved[i],candidate.boundary[i]);
                const auto proposed_lineage=encode_uses(candidate.uses);
                (void)read_group_outer(proposed_lineage,candidate.boundary.size());
                const auto proposed_group=encode_group(proposed_members);
                check.current=exact;
                check.proposed_boundary=std::move(candidate.boundary);
                check.proposed_lineage=proposed_lineage;
                check.proposed_group=proposed_group;
                check.group_face_indices=indices;
                if(!exact)check.diagnostic="Measured group is stale; refresh its outer boundary and member provenance together.";
                continue;
            }
            std::vector<AlignedFace> proposals;
            for(const auto& face:selected_graph->graph->faces) {
                if(face.boundary.size()!=saved.size())continue;
                const auto uses=face_uses(*selected_graph->graph,face);
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
    if(!has_source(area))return true;
    try {
        const bool grouped=area.type=="measurement_boundary" && area.extensions.contains("measurement_linework_group");
        const auto members=grouped ?
            read_group(area.extensions.at("measurement_linework_group")) : std::vector<Uses>{};
        if(!area.extensions.contains("measurement_linework_sources"))return false;
        const auto count=decode_identified_boundary_entity(area).segments.size();
        const auto uses=grouped ?
            read_group_outer(area.extensions.at("measurement_linework_sources"),count) :
            read_uses(area.extensions.at("measurement_linework_sources"),count);
        if(semantic_visible)for(const auto& edge:uses)for(const auto& use:edge)if(!semantic_visible->contains(use.owner_id))return false;
        if(semantic_visible)for(const auto& member:members)for(const auto& edge:member)for(const auto& use:edge)
            if(!semantic_visible->contains(use.owner_id))return false;
        return true;
    }catch(const std::exception&){return false;}
}
std::map<std::string,Entity,std::less<>> complete_measurement_linework_sources(
    const std::map<std::string,Entity,std::less<>>& before,
    const std::map<std::string,Entity,std::less<>>& candidate) {
    const auto semantic_visibility=[](const auto& entities) {
        std::set<std::string,std::less<>> visible;
        for(const auto& [id,entity]:entities) { (void)entity; visible.insert(id); }
        // The document admits one persisted registry; ignore no view masks.
        for(const auto& [id,entity]:entities) {
            (void)id;
            if(entity.type!="model_phases") continue;
            const auto phases=ModelPhases::from_json(entity.properties.at("model"));
            const auto active=phases.active_state();
            for(const auto& member:phases.entity_ids()) {
                const auto found=active.find(member);
                if(found==active.end() || found->second==ModelPhase::demolished)visible.erase(member);
            }
            break;
        }
        return visible;
    };
    const auto before_visible=semantic_visibility(before);
    const auto old_checks=measurement_linework_source_checks(before,&before_visible);
    if(old_checks.empty())return candidate;
    const auto after_visible=semantic_visibility(candidate);
    const auto new_checks=measurement_linework_source_checks(candidate,&after_visible);
    auto result=candidate;
    std::set<std::string,std::less<>> refreshed;
    for(const auto& [id,check]:new_checks) {
        const auto old=old_checks.find(id);
        if(check.current || old==old_checks.end() || !old->second.current || !check.proposed_boundary)continue;
        const auto& area=candidate.at(id);
        // Authored receipts need their own typed reconstruction; an inferred
        // source update must not fabricate replacement authoring evidence.
        if(area.properties.contains("boundary_authoring") || area.extensions.contains("boundary_geometry_derivation"))continue;
        auto boundary=decode_identified_boundary_entity(area);
        if(boundary.segments.size()!=check.proposed_boundary->size())continue;
        if(area.extensions.contains("measurement_linework_group") && !check.proposed_group.is_object())continue;
        for(std::size_t i=0;i<boundary.segments.size();++i)boundary.segments[i].segment=check.proposed_boundary->at(i);
        auto replacement=encode_identified_boundary_entity(boundary,&area);
        replacement.extensions["measurement_linework_sources"]=check.proposed_lineage;
        if(area.extensions.contains("measurement_linework_group"))replacement.extensions["measurement_linework_group"]=check.proposed_group;
        result.at(id)=std::move(replacement);refreshed.insert(id);
    }
    if(!refreshed.empty()) {
        const auto verified=measurement_linework_source_checks(result,&after_visible);
        for(const auto& id:refreshed) {
            const auto found=verified.find(id);
            if(found==verified.end() || !found->second.current)
                throw std::invalid_argument("Measured source completion did not reproduce its current source geometry: "+id);
        }
    }
    return result;
}
} // namespace sketch
