#include "sketch/appraisal_area_partition.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/calculations.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json=nlohmann::json;
[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument("ANSI appraisal partition: "+message);
}
bool measurement(const Entity& entity) {
    return entity.type=="boundary" || entity.type=="measurement_boundary";
}
std::string text(const Json& value,const char* key) {
    const auto found=value.find(key);
    if(found==value.end())return {};
    if(!found->is_string())invalid(std::string(key)+" must be a string");
    return found->get<std::string>();
}
void keys(const Json& value,std::initializer_list<std::string_view> allowed) {
    if(!value.is_object())invalid("policy declaration must be an object");
    for(const auto& [key,ignored]:value.items()) {
        (void)ignored;
        if(std::find(allowed.begin(),allowed.end(),key)==allowed.end())
            invalid("unknown policy declaration: "+key);
    }
}
template<class Parser> void supplied_token(const Json& value,const char* key,Parser parser) {
    if(value.contains(key) && !parser(text(value,key)))invalid(std::string("unknown ")+key);
}
DrawingContext context(const ProjectOrganization& organization,const Entity& entity) {
    if(!measurement(entity) || entity.id.empty())invalid("choose an identified measurement boundary");
    // Saved measurement boundaries may have a floor without a drawing layer.
    // Resolve the physical owners through that floor and check only supplied
    // redundant boundary references; missing layers are not authored here.
    const auto floor=text(entity.properties,"floor_id");
    const auto node=organization.nodes.find(floor);
    if(node==organization.nodes.end() || node->second.type!="floor" || !node->second.issues.empty())
        invalid("boundary has unresolved property, building or floor: "+entity.id);
    const auto& resolved=node->second.context;
    if(resolved.property_id.empty() || resolved.building_id.empty() || resolved.floor_id!=floor)
        invalid("boundary has unresolved property, building or floor: "+entity.id);
    for(const auto& [key,expected]:std::vector<std::pair<const char*,std::string>>{
        {"property_id",resolved.property_id},{"building_id",resolved.building_id}}) {
        const auto explicit_id=text(entity.properties,key);
        if(!explicit_id.empty() && explicit_id!=expected)invalid(std::string(key)+" disagrees with the boundary floor");
    }
    const auto layer=text(entity.properties,"layer_id");
    if(!layer.empty()) {
        const auto layer_node=organization.nodes.find(layer);
        const auto layer_context=organization.drawing_context(layer);
        if(layer_node==organization.nodes.end() || layer_node->second.type!="layer" ||
            !layer_context || layer_context->property_id!=resolved.property_id ||
            layer_context->building_id!=resolved.building_id || layer_context->floor_id!=floor)
            invalid("boundary layer disagrees with its floor: "+entity.id);
    }
    return resolved;
}
bool ansi_policy(const Entity& property) {
    const auto workflow=text(property.properties,"calculation_workflow");
    if(workflow.empty() || workflow=="measurement")return false;
    if(workflow!="appraisal")invalid("unsupported calculation workflow");
    const auto found=property.properties.find("appraisal_policy");
    if(found==property.properties.end())return false;
    if(!found->is_object())invalid("appraisal_policy must be an object");
    const auto kind=text(*found,"policy_kind");
    if(kind.empty())return false;
    const auto parsed=parse_appraisal_policy_kind(kind);
    if(!parsed)invalid("unknown appraisal policy kind");
    if(*parsed!=AppraisalPolicyKind::ansi_z765_2021)return false;
    keys(*found,{"policy_kind","version","property_kind","measurement_basis","ansi"});
    const auto version=found->find("version");
    if(version==found->end() || !version->is_number_integer() || (*version!=1 && *version!=2))
        invalid("ANSI policy requires supported version 1 or 2");
    supplied_token(*found,"property_kind",parse_property_kind);
    supplied_token(*found,"measurement_basis",parse_measurement_basis);
    if(found->contains("ansi")) {
        const auto& declarations=found->at("ansi");
        keys(declarations,{"interior_inspected","direct_measurement","acquisition_increment","limitations_statement"});
        for(const auto* key:{"interior_inspected","direct_measurement"})
            if(declarations.contains(key) && !declarations.at(key).is_boolean())invalid(std::string(key)+" must be boolean");
        supplied_token(declarations,"acquisition_increment",parse_acquisition_increment);
        (void)text(declarations,"limitations_statement");
    }
    return true;
}
std::vector<std::string> deductions(const Entity& entity) {
    const auto found=entity.properties.find("deduction_ids");
    if(found==entity.properties.end())return {};
    if(!found->is_array())invalid("deduction_ids must be an array: "+entity.id);
    std::vector<std::string> result;
    std::set<std::string> unique;
    for(const auto& id:*found) {
        if(!id.is_string() || id.get_ref<const std::string&>().empty() || !unique.insert(id.get<std::string>()).second)
            invalid("deduction IDs must be unique nonempty identities: "+entity.id);
        result.push_back(id.get<std::string>());
    }
    return result;
}
bool site(const Entity& entity) {
    const auto declared=text(entity.properties,"calculation_scope");
    if(!declared.empty()) {
        if(declared!="building" && declared!="site")invalid("unsupported calculation scope: "+entity.id);
        return declared=="site";
    }
    auto classification=text(entity.properties,"measurement_classification");
    if(classification.empty())classification=text(entity.properties,"classification");
    return classification=="survey";
}
Boundary geometry(const Entity& entity) {
    const auto format=inspect_boundary_entity_version(entity);
    if(format.format==BoundaryEntityFormat::unsupported_version)invalid(format.diagnostic);
    const auto identified=format.format==BoundaryEntityFormat::identified_v1
        ? entity : upgrade_legacy_boundary_entity(entity);
    return boundary_geometry(decode_identified_boundary_entity(identified));
}
std::set<std::string,std::less<>> available_entities(const DocumentSnapshot& source) {
    std::set<std::string,std::less<>> available;
    bool registry=false;
    for(const auto& [id,entity]:source.entities())available.insert(id);
    for(const auto& [id,entity]:source.entities())if(entity.type=="model_phases") {
        (void)id;
        if(registry)invalid("multiple design phase registries require resolution");
        registry=true;
        const auto phases=ModelPhases::from_json(entity.properties.at("model"));
        const auto active=phases.active_state();
        for(const auto& member:phases.entity_ids()) {
            const auto state=active.find(member);
            if(state==active.end() || state->second==ModelPhase::demolished)available.erase(member);
        }
    }
    return available;
}
} // namespace

bool ansi_appraisal_partition_context(const DocumentSnapshot& source,std::string_view boundary_id) {
    const auto found=source.entities().find(boundary_id);
    if(found==source.entities().end())invalid("measurement boundary is unavailable");
    const auto organization=organize_project(source);
    const auto owner=context(organization,found->second);
    return ansi_policy(source.entities().at(owner.property_id));
}

std::vector<Entity> prepare_ansi_appraisal_partition_targets(const DocumentSnapshot& source,
    const std::vector<AppraisalPartitionAssignment>& assignments) {
    if(assignments.empty())return {};
    std::map<std::string,Entity,std::less<>> replacements;
    for(const auto& assignment:assignments) {
        const auto found=source.entities().find(assignment.parent_id);
        if(assignment.parent_id.empty() || found==source.entities().end() || !measurement(found->second))
            invalid("assignment parent must be an available measurement boundary");
        auto parent=found->second;
        if(assignment.deduction_ids.empty())parent.properties.erase("deduction_ids");
        else parent.properties["deduction_ids"]=assignment.deduction_ids;
        (void)deductions(parent);
        const auto parent_id=parent.id;
        if(!replacements.emplace(parent_id,std::move(parent)).second)invalid("duplicate parent assignment");
    }
    const auto organization=organize_project(source);
    const auto available=available_entities(source);
    const auto linework=measurement_linework_source_checks(source.entities(),&available);
    const auto entity_for=[&](const std::string& id)->const Entity& {
        if(const auto replaced=replacements.find(id);replaced!=replacements.end())return replaced->second;
        const auto original=source.entities().find(id);
        if(original==source.entities().end())invalid("deduction is unavailable: "+id);
        return original->second;
    };
    struct Node { DrawingContext owner; Boundary boundary; std::vector<std::string> children; };
    std::map<std::string,Node,std::less<>> nodes;
    std::map<std::string,bool,std::less<>> policies;
    const auto node_for=[&](const std::string& id)->const Node& {
        if(const auto cached=nodes.find(id);cached!=nodes.end())return cached->second;
        const auto& entity=entity_for(id);
        const auto owner=context(organization,entity);
        auto policy=policies.find(owner.property_id);
        if(policy==policies.end())policy=policies.emplace(owner.property_id,ansi_policy(source.entities().at(owner.property_id))).first;
        if(!policy->second)invalid("partition requires an explicit ANSI appraisal policy");
        if(site(entity))invalid("site geometry cannot be an appraisal building partition: "+id);
        if(!available.contains(id))invalid("partition is unavailable in the active design phase: "+id);
        if(!measurement_linework_source_current(linework,entity) || !wall_measurement_source_current(source,entity))
            invalid("partition has stale measurement sources: "+id);
        if(entity.properties.contains("wall_measurement_source"))
            for(const auto& record:entity.properties.at("wall_measurement_source").at("walls"))
                if(!available.contains(record.at("id").get<std::string>()))
                    invalid("partition has phase-hidden exterior measurement sources: "+id);
        return nodes.emplace(id,Node{owner,geometry(entity),deductions(entity)}).first->second;
    };
    const CalculationProfile physical{"vertex-ansi-partition-geometry",1,AreaUnit::square_metre,2,
        {{"physical",{false,false}}}};
    std::set<std::string> active,complete;
    // Explicit stack bounds native stack usage even for deep retained graphs.
    for(const auto& assignment:assignments) {
        std::vector<std::pair<std::string,bool>> pending{{assignment.parent_id,false}};
        while(!pending.empty()) {
            auto [id,finish]=std::move(pending.back());pending.pop_back();
            if(finish) {
                const auto& node=node_for(id);
                std::vector<AreaDeduction> tools;
                for(const auto& child_id:node.children) {
                    const auto& child=node_for(child_id);
                    if(child.owner.property_id!=node.owner.property_id || child.owner.building_id!=node.owner.building_id ||
                        child.owner.floor_id!=node.owner.floor_id)invalid("deductions must share their parent property, building and floor");
                    tools.push_back({child_id,child.boundary});
                }
                (void)calculate_area({id,node.owner.building_id,node.owner.floor_id,"physical",node.boundary,
                    std::move(tools),{1,1}},physical);
                active.erase(id);complete.insert(id);
                continue;
            }
            if(active.contains(id))invalid("cyclic deduction dependency: "+id);
            if(complete.contains(id))continue;
            const auto& node=node_for(id);
            active.insert(id);pending.push_back({id,true});
            for(auto child=node.children.rbegin();child!=node.children.rend();++child)pending.push_back({*child,false});
        }
    }
    std::vector<Entity> result;result.reserve(assignments.size());
    for(const auto& assignment:assignments)result.push_back(replacements.at(assignment.parent_id));
    return result;
}
} // namespace sketch
