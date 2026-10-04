#include "sketch/measurement_area_definition.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/area_subtraction.hpp"
#include "sketch/calculations.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument("Define measured areas: "+message);
}
std::string trimmed(std::string value) {
    const auto blank=[](unsigned char c){return std::isspace(c)!=0;};
    const auto start=std::find_if_not(value.begin(),value.end(),blank);
    const auto end=std::find_if_not(value.rbegin(),value.rend(),blank).base();
    return start<end?std::string(start,end):std::string{};
}
Json geometry_json(const Boundary& boundary) {
    auto result=Json::array();
    for(const auto& edge:boundary)result.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    return result;
}
Json face_lineage(const MeasurementAreaGraph& graph,const DerivedMeasurementFace& face) {
    auto result=Json::array();
    for(const auto& traversal:face.edge_uses) {
        auto inputs=Json::array();
        for(const auto& use:graph.edges.at(traversal.edge_index).source_uses)
            inputs.push_back({{"owner_id",use.owner_id},{"segment_id",use.segment_id},
                {"parameter_start",use.parameter_start},{"parameter_end",use.parameter_end},
                {"reversed",use.reversed!=traversal.reversed}});
        result.push_back(std::move(inputs));
    }
    return result;
}
bool same_segment(const Segment& a,const Segment& b) {
    return a.start.x==b.start.x && a.start.y==b.start.y && a.end.x==b.end.x &&
        a.end.y==b.end.y && a.sweep_radians==b.sweep_radians;
}
bool same_boundary(const Boundary& a,const Boundary& b) {
    if(a.empty() || a.size()!=b.size())return false;
    for(std::size_t start=0;start<b.size();++start)for(const bool reverse:{false,true}) {
        bool matches=true;
        for(std::size_t i=0;i<a.size();++i) {
            auto edge=b[reverse?(start+b.size()-i)%b.size():(start+i)%b.size()];
            if(reverse){std::swap(edge.start,edge.end);edge.sweep_radians=-edge.sweep_radians;}
            if(!same_segment(a[i],edge)){matches=false;break;}
        }
        if(matches)return true;
    }
    return false;
}
std::vector<std::string> deductions(const Entity& entity) {
    const auto found=entity.properties.find("deduction_ids");
    if(found==entity.properties.end())return {};
    if(!found->is_array())invalid("Existing deduction IDs are malformed.");
    std::vector<std::string> ids;std::set<std::string> seen;
    for(const auto& id:*found) {
        if(!id.is_string() || id.get_ref<const std::string&>().empty() || !seen.insert(id.get<std::string>()).second)
            invalid("Existing deduction IDs must be unique nonempty identities.");
        ids.push_back(id.get<std::string>());
    }
    return ids;
}
bool appraisal_workflow(const Entity& property) {
    const auto found=property.properties.find("calculation_workflow");
    if(found==property.properties.end())return false;
    if(!found->is_string() || (*found!="measurement" && *found!="appraisal"))
        invalid("Calculation workflow must be measurement or appraisal.");
    return *found=="appraisal";
}
std::set<std::string,std::less<>> phase_eligible(const DocumentSnapshot& source) {
    std::set<std::string,std::less<>> eligible;
    for(const auto& [id,entity]:source.entities())eligible.insert(id);
    bool found=false;
    for(const auto& [id,entity]:source.entities())if(entity.type=="model_phases") {
        if(found)invalid("Multiple design phase registries require resolution before detecting areas.");
        found=true;
        const auto phases=ModelPhases::from_json(entity.properties.at("model"));
        const auto active=phases.active_state();
        for(const auto& member:phases.entity_ids()) {
            const auto state=active.find(member);
            if(state==active.end() || state->second==ModelPhase::demolished)eligible.erase(member);
        }
    }
    return eligible;
}
Entity create_area(const DetectedMeasurementAreas& detection,const DerivedMeasurementFace& face,
    const std::string& classification,bool appraisal) {
    const auto& context=detection.context;
    Entity area{"measured-area-"+make_stable_id(),"measurement_boundary",{
        {"property_id",context.property_id},{"building_id",context.building_id},
        {"floor_id",context.floor_id},{"layer_id",context.layer_id},
        {"segments",geometry_json(face.boundary)},
        {"classification",classification},{"measurement_classification",classification},
        {"factor",1.0},{"factor_expression","1"},{"factor_numerator",1},{"factor_denominator",1}},false,
        {{"measurement_linework_sources",face_lineage(detection.graph,face)}}};
    if(appraisal) {
        if(classification.starts_with("role:")) {
            const auto role=parse_boundary_role(classification.substr(5));
            if(!role || *role==BoundaryRole::measured_area)invalid("Choose a supported explicit deduction role.");
            // This is the user's explicit role, not inferred eligibility facts.
            area.properties["appraisal_facts"]={{"boundary_role",std::string(boundary_role_name(*role))}};
        } else if(const auto category=parse_appraisal_category(classification);category && *category!=AppraisalAreaCategory::none) {
            area.properties["appraisal_category"]=classification;
            area.properties["classification"]="measurement";
            area.properties["measurement_classification"]="measurement";
        }
    }
    return upgrade_legacy_boundary_entity(area);
}
}

DetectedMeasurementAreas detect_measurement_areas(const DocumentSnapshot& source,std::string_view stroke_id) {
    const auto selected=source.entities().find(stroke_id);
    if(selected==source.entities().end() || selected->second.type!="measurement_linework")
        invalid("Select a measured stroke first.");
    const auto eligible=phase_eligible(source);
    if(!eligible.contains(std::string(stroke_id)))invalid("The selected measured stroke is not in the active design phase.");
    const auto organization=organize_project(source);
    const auto context=organization.drawing_context(stroke_id);
    if(!context || !context->complete())invalid("The measured stroke has no resolved drawing context.");
    std::vector<MeasurementGraphSource> segments;
    for(const auto& [id,entity]:source.entities()) {
        if(entity.type!="measurement_linework" || !eligible.contains(id) || organization.drawing_context(id)!=context)continue;
        const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
        if(!decoded.supported())invalid("The layer contains an unsupported measured stroke.");
        for(const auto& edge:replay_measurement_linework(*decoded.model).edges)
            segments.push_back({id,edge.segment_id,edge.segment});
    }
    DetectedMeasurementAreas result{*context,build_measurement_area_graph(segments),{}};
    if(result.graph.faces.empty())invalid("No closed area was found in this layer's measured lines.");
    result.existing_area_ids.resize(result.graph.faces.size());
    result.existing_group_ids.resize(result.graph.faces.size());
    const auto source_checks=measurement_linework_source_checks(source.entities(),&eligible);
    for(const auto& [id,existing]:source.entities()) {
        if(existing.type!="measurement_boundary" || !eligible.contains(id) || organization.drawing_context(id)!=context)continue;
        if(const auto check=source_checks.find(id);check!=source_checks.end() && !check->second.current)
            invalid("An existing area has stale source lines. Review or redefine it before detecting more areas: "+id);
        if(existing.extensions.contains("measurement_linework_group")) {
            const auto check=source_checks.find(id);
            if(check==source_checks.end() || check->second.group_face_indices.size()<2)
                invalid("An existing combined area has unresolved membership: "+id);
            for(const auto index:check->second.group_face_indices) {
                if(index>=result.graph.faces.size() || result.existing_group_ids[index] || result.existing_area_ids[index])
                    invalid("Existing area definitions overlap combined membership. Resolve the definitions first.");
                result.existing_group_ids[index]=id;
            }
            continue;
        }
        const auto geometry=boundary_geometry(decode_identified_boundary_entity(existing));
        for(std::size_t i=0;i<result.graph.faces.size();++i)if(same_boundary(geometry,result.graph.faces[i].boundary)) {
            if(result.existing_area_ids[i] || result.existing_group_ids[i])invalid("Multiple existing areas define the same region. Resolve the duplicate areas first.");
            result.existing_area_ids[i]=id;
        }
    }
    return result;
}

MeasurementAreaDefinition prepare_measurement_area_definition(const DocumentSnapshot& source,
    std::string_view stroke_id,const std::vector<MeasurementAreaChoice>& choices) {
    if(!source.is_editable())invalid("The captured project is read-only.");
    const auto detection=detect_measurement_areas(source,stroke_id);
    if(choices.size()!=detection.graph.faces.size())invalid("Review every detected outline before applying.");
    const bool appraisal=appraisal_workflow(source.entities().at(detection.context.property_id));
    const auto organization=organize_project(source);
    const auto eligible=phase_eligible(source);
    MeasurementAreaDefinition result{{source.revision(),{}, {},"Define measured areas from linework"},{}};
    std::vector<std::optional<std::string>> ids(choices.size());
    std::map<std::string,Entity,std::less<>> changed;
    std::map<std::size_t,std::vector<std::size_t>> groups;
    std::map<std::string,std::vector<std::size_t>,std::less<>> retained_groups;
    for(std::size_t i=0;i<choices.size();++i) {
        if(choices[i].combine_group)groups[*choices[i].combine_group].push_back(i);
        if(detection.existing_group_ids[i])retained_groups[*detection.existing_group_ids[i]].push_back(i);
    }
    for(const auto& [id,members]:retained_groups) {
        const auto disposition=choices[members.front()].disposition;
        if(disposition!=MeasurementAreaDisposition::reference_only && disposition!=MeasurementAreaDisposition::define_area)
            invalid("An existing combined area must be kept or used as a whole. Edit its deductions separately.");
        for(const auto index:members) {
            if(choices[index].disposition!=disposition)
                invalid("Keep or use every member of an existing combined area together.");
            ids[index]=id;
        }
    }
    for(const auto& [token,members]:groups) {
        (void)token;
        if(members.size()<2 || members.size()>2048)invalid("A combined area requires between two and 2048 selected regions.");
        const auto retained=detection.existing_group_ids[members.front()];
        if(retained) {
            if(retained_groups.at(*retained)!=members)
                invalid("A retained combined area's membership cannot be changed implicitly.");
            continue;
        }
        const auto classification=trimmed(choices[members.front()].classification);
        if(classification.empty())invalid("Choose an explicit classification for the combined area.");
        for(const auto index:members) {
            if(detection.existing_area_ids[index] || detection.existing_group_ids[index])
                invalid("Combining existing definitions requires an explicit ownership change. Keep them or remove the old definitions first.");
            if(choices[index].disposition!=MeasurementAreaDisposition::define_area ||
                trimmed(choices[index].classification)!=classification)
                invalid("Combined regions must all be defined with the same explicit classification.");
        }
        const auto combined=combine_measurement_faces(detection.graph,members);
        for(const auto& [id,existing]:source.entities()) {
            if(existing.type=="measurement_boundary" && eligible.contains(id) &&
                organization.drawing_context(id)==detection.context &&
                same_boundary(boundary_geometry(decode_identified_boundary_entity(existing)),combined.boundary))
                invalid("The combined exterior is already owned by an existing measured area: "+id);
        }
        auto entity=create_area(detection,combined,classification,appraisal);
        auto lineage=Json::array();
        for(const auto index:members)lineage.push_back(face_lineage(detection.graph,detection.graph.faces[index]));
        entity.extensions["measurement_linework_group"]={{"version",1},{"members",std::move(lineage)}};
        for(const auto index:members)ids[index]=entity.id;
        changed.emplace(entity.id,std::move(entity));
    }
    std::set<std::string> active_ids;
    for(std::size_t i=0;i<choices.size();++i) {
        const auto& choice=choices[i];
        switch(choice.disposition) {
        case MeasurementAreaDisposition::reference_only:
            if(!ids[i])ids[i]=detection.existing_area_ids[i];
            continue;
        case MeasurementAreaDisposition::define_area:
        case MeasurementAreaDisposition::deduct_from_parent: break;
        default: invalid("Unknown area disposition.");
        }
        if(!ids[i] && detection.existing_area_ids[i])ids[i]=detection.existing_area_ids[i];
        if(!ids[i]) {
            const auto classification=trimmed(choice.classification);
            if(classification.empty())invalid("Choose an explicit classification for every new defined area.");
            auto entity=create_area(detection,detection.graph.faces[i],classification,appraisal);
            ids[i]=entity.id;changed.emplace(entity.id,std::move(entity));
        }
        if(active_ids.insert(*ids[i]).second)result.area_ids.push_back(*ids[i]);
    }
    result.face_area_ids=ids;
    const auto command_for=[&] {
        auto command=result.command;
        for(const auto& [id,entity]:changed) {
            const auto old=source.entities().find(id);
            if(old==source.entities().end() || old->second!=entity)command.entity_changes.push_back(EntityChange::upsert(entity));
        }
        return command;
    };
    auto initial=source;
    const auto additions=command_for();
    if(!additions.entity_changes.empty())initial=Document::preview_command(source,additions);
    std::vector<std::pair<std::string,std::string>> links;
    for(std::size_t i=0;i<choices.size();++i) {
        if(choices[i].disposition!=MeasurementAreaDisposition::deduct_from_parent)continue;
        const auto parent=detection.graph.faces[i].parent_face_index;
        if(!parent || *parent>=choices.size() || !ids[*parent])invalid("A deduction requires its containing outline to be defined.");
        if(choices[*parent].disposition==MeasurementAreaDisposition::deduct_from_parent)
            invalid("Nested deduction chains are not supported. Keep the deeper outline as a reference or define it independently.");
        const auto& subtractor=initial.entities().at(*ids[i]);
        // Reuse the authoritative TYPE/context/containment checks. Collect all
        // siblings before final validation, rather than publishing intermediate
        // definitions or rebuilding retained history for each sibling.
        const auto checked=prepare_area_subtraction_target(initial,subtractor,*ids[*parent]);
        auto [target,inserted]=changed.try_emplace(checked.id,checked);
        if(!inserted) {
            auto union_ids=deductions(target->second);
            for(const auto& id:deductions(checked))if(std::find(union_ids.begin(),union_ids.end(),id)==union_ids.end())union_ids.push_back(id);
            target->second.properties["deduction_ids"]=std::move(union_ids);
        }
        links.emplace_back(*ids[i],*ids[*parent]);
    }
    result.command=command_for();
    if(!result.command.entity_changes.empty()) {
        const auto final=Document::preview_command(source,result.command);
        const auto available=phase_eligible(final);
        const auto checks=measurement_linework_source_checks(final.entities(),&available);
        for(const auto& [id,entity]:changed) {
            if(entity.extensions.contains("measurement_linework_group") &&
                !measurement_linework_source_current(checks,entity))
                invalid("The combined area does not have valid complete source provenance: "+
                    (checks.contains(id)?checks.at(id).diagnostic:std::string("Missing group source proof.")));
        }
        // Validate the complete combined set, including pre-existing deductions,
        // union area, and any target that is already a deduction elsewhere.
        for(const auto& [child,parent]:links)
            (void)prepare_area_subtraction_target(final,final.entities().at(child),parent);
    }
    return result;
}
} // namespace sketch
