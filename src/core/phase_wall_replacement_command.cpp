#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_wall_replacement_authoring.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/corner_window_edit.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/opening_host_geometry.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <Standard_Failure.hxx>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=PhaseWallReplacementEntities;
[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Proposed wall edit: "+reason);
}
void keys(const Json& value,std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size()!=expected.size()) invalid("unsupported decision fields");
    for (const auto* key:expected) if (!value.contains(key)) invalid("missing decision field");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("identity must be a string");
    auto result=value.get<std::string>();
    if (result.empty() || result.size()>128 || !std::all_of(result.begin(),result.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
    })) invalid("invalid identity");
    return result;
}
std::string presentation_source_identity(const std::string& value) {
    if (value.empty() || value.size()>128 || std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return std::isspace(c);
    })) invalid("presentation source identity must be nonblank and at most 128 bytes");
    return value;
}
std::string hosted_source_identity(const Json& value) {
    if (!value.is_string()) invalid("qualified hosted source identity must be a string");
    const auto result=value.get<std::string>();
    // AssemblyModel accepts nonblank local names, not document-ID grammar.
    // The enclosing one-MiB authoring budget bounds their retained bytes.
    if (result.empty() || result.size()>1024*1024 || std::all_of(result.begin(),result.end(),[](unsigned char c) {
        return std::isspace(c);
    })) invalid("qualified hosted source identity must be bounded and nonblank");
    return result;
}
PhaseWallReplacementAuthoring decode_flat_phase_wall_replacement_authoring(
    const Json& value, bool allow_presentation_source_ids);
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
bool strict_profile_admission(const WallProfileEditIntent& profile) {
    return profile.elevation.has_value() || profile.top_rise.has_value() || (profile.layer_thicknesses &&
        std::any_of(profile.layer_thicknesses->begin(), profile.layer_thicknesses->end(),
            [](const auto& row) { return row.retain_source_thickness; }));
}
const char* disposition(PhaseWallRoomConstraintDisposition value) {
    switch (value) {
    case PhaseWallRoomConstraintDisposition::keep:return "keep";
    case PhaseWallRoomConstraintDisposition::remap:return "remap";
    case PhaseWallRoomConstraintDisposition::omit:return "omit";
    }
    invalid("unknown room relationship decision");
}
void preserve_baseline(const Entities& source,const Entities& candidate,
    const PhaseWallReplacementPlan& plan) {
    std::set<std::string,std::less<>> preserved(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
    for (const auto& [id,entity]:source) if (entity.type=="model_phases") {
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        preserved.insert(model.baseline_ids().begin(),model.baseline_ids().end());
        if (id!=plan.registry_id) preserved.insert(id);
    }
    preserved.erase(plan.registry_id);
    for (const auto& id:preserved) {
        const auto found=candidate.find(id);
        if (found==candidate.end() || !exact(source.at(id),found->second))
            invalid("replacement changed an original baseline object or dependent: "+id);
    }
}
struct Stage {
    PhaseWallReplacementPlan plan;
    PhaseWallReplacementResult replacement;
    Entities entities;
    std::set<std::string,std::less<>> deferred;
    std::set<std::string,std::less<>> deferred_removals;
};
// The complete copied source supplies active cuts, floor placement and native
// bodies. Neither a render alias nor a sliced synthetic snapshot is a host.
struct HostedWallAdmission {
    const Entities& entities;
    ConstraintPhaseScope scope;
    AssemblyExpansionBudget expansion;
    std::size_t native_work{};
    std::map<std::string,Wall,std::less<>> walls;
    std::map<std::string,TopoDS_Shape,std::less<>> bodies;
    explicit HostedWallAdmission(const Entities& actual):entities(actual),scope(constraint_phase_scope(actual)) {}
    const Wall& wall(const std::string& id) {
        if (const auto found=walls.find(id);found!=walls.end()) return found->second;
        const auto found=entities.find(id);
        if (found==entities.end() || found->second.type!="wall" || scope.inactive_owner_ids.contains(id))
            invalid("hosted row requires an actual active wall");
        std::vector<const Entity*> cuts;
        for (const auto& [cut_id,entity]:entities)
            if (entity.type=="opening" && !scope.inactive_owner_ids.contains(cut_id) &&
                entity.properties.value("wall_id",std::string{})==id) {
                if (cuts.size()>=128) invalid("hosted wall cut budget exceeded");
                cuts.push_back(&entity);
            }
        Wall decoded;std::string error;
        if (!read_document_wall(resolve_vertical_placement(entities,found->second),cuts,decoded,error)) invalid(error);
        validate_wall_semantics(decoded);
        return walls.emplace(id,std::move(decoded)).first->second;
    }
    const TopoDS_Shape& body(const std::string& id) {
        if (const auto found=bodies.find(id);found!=bodies.end()) return found->second;
        TopoDS_Shape result;
        if (entities.at(id).type=="opening") result=make_document_opening_host_shape(entities,id,&native_work);
        else {
            const auto& decoded=wall(id);
            const auto layers=std::max<std::size_t>(1,decoded.layers.size());
            const auto work=layers*(decoded.openings.size()+1);
            if (layers>32 || work>262144-native_work) invalid("hosted native geometry budget exceeded");
            native_work+=work;result=make_wall(decoded);
        }
        if (result.IsNull()) invalid("hosted source has no native body");
        return bodies.emplace(id,std::move(result)).first->second;
    }
    void admit(const std::string& catalog_id,const AssemblyModel& model,const AssemblyInstance& instance) try {
        if (!instance.placement) invalid("copied hosted row lost its actual placement");
        const auto& p=*instance.placement;
        const auto host=entities.find(p.host_entity_id);
        if (host==entities.end() || (host->second.type!="wall" && host->second.type!="opening") ||
            assembly_host_inactive_in_phase(entities,scope.inactive_owner_ids,host->first) ||
            scope.assembly_memberships.inactive_roots.contains({catalog_id,instance.id}))
            invalid("copied hosted row is inactive or has an unsupported host");
        (void)resolve_site_presentation(entities,host->first);
        const auto expanded=model.expand(instance,expansion);
        if (!expanded.profiles.empty()) {
            // Type-owned geometry still requires its independently admitted host.
            const auto wall_id=host->second.type=="wall" ? host->first : host->second.properties.at("wall_id").get<std::string>();
            (void)body(wall_id);
            if (make_assembly_geometry(expanded).shape.IsNull()) invalid("copied hosted profiles have no native body");
        } else (void)transform_assembly_shape(body(host->first),
            {{p.translation_m.x,p.translation_m.y,p.translation_z_m},p.rotation_radians,p.scale,p.mirrored_y,p.vertical_scale});
    } catch (const Standard_Failure& error) { invalid(std::string("copied hosted native admission failed: ")+error.what()); }
};
AssemblyTransform hosted_planar_transform(const PlanarTransform& t) {
    // Determine the actual orthogonal basis using the geometry producer's
    // rotation-then-reflection ordering, including exact half turns.
    auto basis=t;basis.pivot={};basis.offset={};
    const auto x=transform_point({1,0},basis);
    AssemblyTransform result;
    result.rotation_radians=std::atan2(x.y,x.x);
    result.mirrored_y=t.flip_horizontal!=t.flip_vertical;
    const auto translated=transform_point({},t);
    result.translation_m={translated.x,translated.y,0};
    return result;
}
AssemblyTransform hosted_pose_delta(AssemblyPoint3 before,double old_yaw,
    AssemblyPoint3 after,double new_yaw) {
    AssemblyTransform result;
    result.rotation_radians=std::remainder(new_yaw-old_yaw,2*std::acos(-1.0));
    const auto moved=transform_assembly_point(before,result);
    result.translation_m={after.x-moved.x,after.y-moved.y,after.z-before.z};
    return result;
}
AssemblyPoint3 hosted_station_point(const Wall& wall,double station,double sill,double& yaw) {
    const auto& line=wall.baseline;
    const auto dx=line.end.x-line.start.x,dy=line.end.y-line.start.y;
    const auto chord=std::hypot(dx,dy),length=segment_length(line);
    if (!(chord>0) || !(length>0) || !std::isfinite(station) || station<0 || station>length)
        invalid("hosted opening has no actual wall station frame");
    const auto ux=dx/chord,uy=dy/chord;
    double along=station,across=0;
    yaw=std::atan2(dy,dx);
    if (line.sweep_radians!=0) {
        const auto angle=line.sweep_radians*(station/length);
        const auto center_along=chord/2,center_across=chord/(2*std::tan(line.sweep_radians/2));
        const auto half=std::sin(angle/2),one_minus_cosine=2*half*half;
        along=center_along*one_minus_cosine+center_across*std::sin(angle);
        across=center_across*one_minus_cosine-center_along*std::sin(angle);
        yaw+=angle-line.sweep_radians/2;
    }
    AssemblyPoint3 point{line.start.x+ux*along-uy*across,line.start.y+uy*along+ux*across,wall.elevation+sill};
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) || !std::isfinite(yaw))
        invalid("hosted station carry exceeds supported numeric range");
    return point;
}
void complete_hosted_wall_carry(Stage& stage,const ConstraintAuthoringIntent& mapped) {
    if (!stage.plan.complete_hosted_instances) return;
    HostedWallAdmission before(stage.replacement.entities),after(stage.entities);
    AssemblyExpansionBudget source_expansion;
    std::map<std::string,AssemblyModel,std::less<>> source_models;
    std::optional<JointTranslationOffsets> joint;
    if (mapped.joint_translation) joint=resolve_joint_translation_offsets(before.entities,*mapped.joint_translation);
    std::map<std::string,std::map<std::string,AssemblyTransform,std::less<>>,std::less<>> operations;
    // Every operation is derived from the copied actual host and the final
    // independently replayed intent; the qualified key supplies only identity.
    for (const auto& [original,destination]:stage.replacement.hosted_instance_original_to_proposed) {
        const auto& catalog=stage.replacement.entities.at(destination.first);
        auto decoded=source_models.find(destination.first);
        if (decoded==source_models.end()) decoded=source_models.emplace(destination.first,
            AssemblyModel::from_json(catalog.properties.at("model"))).first;
        const auto& model=decoded->second;
        const auto row=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& instance) {
            return instance.id==destination.second;
        });
        if (row==model.instances().end() || !row->placement) invalid("qualified copied hosted row disappeared");
        const auto& host=row->placement->host_entity_id;
        const auto& old_host=before.entities.at(host);
        const auto& new_host=after.entities.at(host);
        const auto host_wall=[](const Entity& entity) {
            return entity.type=="wall" ? entity.id : entity.properties.at("wall_id").get<std::string>();
        };
        const auto old_wall_id=host_wall(old_host),new_wall_id=host_wall(new_host);
        const auto& old_wall=before.wall(old_wall_id);const auto& new_wall=after.wall(new_wall_id);
        AssemblyTransform operation;
        bool explicit_operation=false;
        if (mapped.wall_group_scale && std::binary_search(mapped.wall_group_scale->wall_ids.begin(),
            mapped.wall_group_scale->wall_ids.end(),old_wall_id)) {
            const auto& t=*mapped.wall_group_scale;
            operation.translation_m={(1-t.scale)*t.pivot.x,(1-t.scale)*t.pivot.y,(1-t.scale)*t.pivot.z};
            operation.scale=t.scale;explicit_operation=true;
        } else if (mapped.wall_geometry_move) {
            for (const auto& target:mapped.wall_geometry_move->targets)
                if (target.wall_id==old_wall_id && target.rigid_transform) {
                    operation=hosted_planar_transform(*target.rigid_transform);explicit_operation=true;break;
                }
        }
        if (!explicit_operation && joint) {
            if (const auto found=joint->owner_similarities.find(old_wall_id);found!=joint->owner_similarities.end()) {
                const auto& similarity=found->second;
                AssemblyTransform scale;
                scale.scale=similarity.scale;
                scale.translation_m={(1-scale.scale)*similarity.transform.pivot.x,
                    (1-scale.scale)*similarity.transform.pivot.y,(1-scale.scale)*old_wall.elevation};
                // The physical producer uses the captured effective base plane
                // as the Z pivot while scaling height and hosted sections.
                operation=compose_assembly_transform(hosted_planar_transform(similarity.transform),scale);
                explicit_operation=true;
            } else if (const auto found=joint->owner_transforms.find(old_wall_id);found!=joint->owner_transforms.end()) {
                operation=hosted_planar_transform(found->second);explicit_operation=true;
            } else if (const auto found=joint->owner_offsets.find(old_wall_id);found!=joint->owner_offsets.end()) {
                operation.translation_m={found->second.x,found->second.y,0};explicit_operation=true;
            }
        }
        const auto expansion=model.expand(*row,source_expansion);
        if (!explicit_operation) {
            const auto& a=old_wall.baseline;const auto& b=new_wall.baseline;
            const bool same_baseline=a.start.x==b.start.x && a.start.y==b.start.y &&
                a.end.x==b.end.x && a.end.y==b.end.y && a.sweep_radians==b.sweep_radians;
            if (old_host.type=="opening") {
                const auto old_cut=std::find_if(old_wall.openings.begin(),old_wall.openings.end(),[&](const auto& cut){return cut.id==host;});
                const auto new_cut=std::find_if(new_wall.openings.begin(),new_wall.openings.end(),[&](const auto& cut){return cut.id==host;});
                if (old_cut==old_wall.openings.end() || new_cut==new_wall.openings.end()) invalid("copied opening lost its actual host cut");
                double old_yaw{},new_yaw{};
                const auto old_point=hosted_station_point(old_wall,old_cut->offset+old_cut->width/2,old_cut->sill,old_yaw);
                const auto new_point=hosted_station_point(new_wall,new_cut->offset+new_cut->width/2,new_cut->sill,new_yaw);
                operation=hosted_pose_delta(old_point,old_yaw,new_point,new_yaw);
            } else if (same_baseline) operation.translation_m.z=new_wall.elevation-old_wall.elevation;
            else {
                // Arbitrary host deformation changes native host-derived bodies
                // directly. A world profile has no source-owned station/warp
                // contract, so only a proved directed rigid baseline may carry it.
                const auto old_length=segment_length(a),new_length=segment_length(b);
                const bool rigid=a.sweep_radians==b.sweep_radians &&
                    std::abs(old_length-new_length)<=default_geometry_tolerance_metres;
                if (!rigid) {
                    if (!expansion.profiles.empty()) invalid("deformed wall has no source-bound carry authority for type-owned hosted profiles");
                    continue;
                }
                operation=hosted_pose_delta({a.start.x,a.start.y,old_wall.elevation},std::atan2(a.end.y-a.start.y,a.end.x-a.start.x),
                    {b.start.x,b.start.y,new_wall.elevation},std::atan2(b.end.y-b.start.y,b.end.x-b.start.x));
            }
        }
        if (!expansion.profiles.empty()) {
            const auto& frame=resolve_site_presentation(before.entities,host).forward;
            const auto& final_frame=resolve_site_presentation(after.entities,host).forward;
            if (frame.translation_m.x!=final_frame.translation_m.x || frame.translation_m.y!=final_frame.translation_m.y ||
                frame.translation_m.z!=final_frame.translation_m.z || frame.rotation_radians!=final_frame.rotation_radians)
                invalid("hosted carry between different Site frames requires explicit world-frame authority");
            operation=conjugate_assembly_transform_through_rigid_frame(operation,
                {{frame.translation_m.x,frame.translation_m.y,frame.translation_m.z},frame.rotation_radians,1,false});
        }
        if (operation!=AssemblyTransform{}) operations[destination.first].emplace(destination.second,operation);
    }
    for (const auto& [catalog,rows]:operations)
        stage.entities.at(catalog).properties.at("model")=transform_hosted_assembly_model(
            stage.replacement.entities.at(catalog).properties.at("model"),rows,true);
    // Re-admit the exact whole candidate after changed-host carry. Original
    // catalogs and rows are protected separately by preserve_baseline.
    std::map<std::string,AssemblyModel,std::less<>> final_models;
    for (const auto& [original,destination]:stage.replacement.hosted_instance_original_to_proposed) {
        auto decoded=final_models.find(destination.first);
        if (decoded==final_models.end()) decoded=final_models.emplace(destination.first,
            AssemblyModel::from_json(stage.entities.at(destination.first).properties.at("model"))).first;
        const auto& model=decoded->second;
        const auto row=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& instance){return instance.id==destination.second;});
        if (row==model.instances().end()) invalid("carried hosted row disappeared");
        after.admit(destination.first,model,*row);
    }
    const auto original_aliases=embedded_assembly_presentation_ids(stage.replacement.entities);
    const auto final_aliases=embedded_assembly_presentation_ids(stage.entities);
    if (original_aliases!=final_aliases) invalid("hosted carry changed qualified presentation identities");
}
bool has_geometry_or_relation_intent(const ConstraintAuthoringIntent& intent) {
    return intent.wall_resize || intent.wall_geometry_move || intent.wall_group_scale || intent.wall_curve_construction ||
        intent.boundary_resize || intent.boundary_vertex_move || intent.exterior_corner_move ||
        intent.exterior_segment_resize || intent.exterior_segment_arc || intent.measured_stroke_resize ||
        intent.measured_stroke_vertex_move || intent.measured_stroke_transform || intent.joint_translation ||
        intent.relation_anchor || !intent.relation_mutations.empty();
}
Stage physical_stage(const Entities& source,const PhaseConstraintAuthoringIntent& intent,
    const PhaseWallReplacementAuthoring& edit) {
    if (intent.source_entities_digest!=entity_map_digest(source) ||
        intent.phase_selections!=phase_constraint_authoring_selections(source)) invalid("actual source or saved choice changed");
    if (edit.complete_vertical_profiles || edit.complete_hosted_instances) {
        // Detached inspection shares the enclosing dialect guard with retained
        // publication. Historical outer records cannot borrow leaf-ten authority.
        (void)encode_phase_constraint_authoring_intent(intent);
    }
    if (edit.complete_scalar_profiles) {
        if (!edit.complete_presentations || !edit.complete_corner_windows ||
            !edit.opening_rehosts.empty() || !edit.opening_families.empty() ||
            !edit.wall_stacks.empty() || !edit.corner_profiles.empty() ||
            has_geometry_or_relation_intent(intent.intent))
            invalid("scalar cohort requires complete scalar authority without sibling edit operations");
        const auto request=phase_wall_scalar_profile_replacement_request(source,edit.wall_profiles,edit.opening_profiles);
        if (!request || request->registry_id!=edit.registry_id || request->alternative_id!=edit.alternative_id ||
            request->seed_wall_ids!=edit.seed_wall_ids)
            invalid("scalar cohort seeds differ from actually changed saved baseline hosts");
    }
    if (intent.intent.wall_group_scale) {
        // Detached inspection must obey the same additive outer dialect as
        // retained publication, including exclusive scale/replacement keys.
        (void)encode_phase_constraint_authoring_intent(intent);
        auto siblings=intent.intent;
        siblings.wall_group_scale.reset();
        if (!edit.complete_presentations || !edit.complete_corner_windows ||
            !edit.wall_profiles.empty() || !edit.opening_profiles.empty() || !edit.opening_rehosts.empty() ||
            !edit.opening_families.empty() || !edit.wall_stacks.empty() || !edit.corner_profiles.empty() ||
            has_geometry_or_relation_intent(siblings) || !intent.intent.relation_move_connected_walls)
            invalid("wall group scale requires exclusive leaf-eight replacement and complete presentation/corner authority");
        // Independently rediscover the original targets and participating
        // persisted relation closure before accepting any supplied plan/map.
        // Physical contact, host and presentation closure is then rederived
        // by inspect_phase_wall_replacement_plan below, from this same source.
        const auto requests=phase_wall_replacement_requests(source,intent.intent);
        if (requests.size()!=1 || requests.front().registry_id!=edit.registry_id ||
            requests.front().alternative_id!=edit.alternative_id || requests.front().seed_wall_ids!=edit.seed_wall_ids)
            invalid("wall group scale replacement roots do not match actual original owners and saved baseline membership");
    }
    if (!edit.corner_profiles.empty()) {
        if (!edit.complete_corner_windows || !edit.wall_profiles.empty() || !edit.opening_profiles.empty() ||
            !edit.opening_rehosts.empty() || !edit.opening_families.empty() || !edit.wall_stacks.empty() ||
            has_geometry_or_relation_intent(intent.intent))
            invalid("corner profiles require leaf eight and a separate reviewed operation from all other edit authority");
        const auto request=phase_corner_window_profile_replacement_request(source,edit.corner_profiles);
        if (!request || request->registry_id!=edit.registry_id || request->alternative_id!=edit.alternative_id ||
            request->seed_wall_ids!=edit.seed_wall_ids)
            invalid("corner profile replacement roots do not match actual changes and saved baseline membership");
    }
    if (!edit.wall_stacks.empty()) {
        if (!edit.wall_profiles.empty() || !edit.opening_profiles.empty() || !edit.opening_rehosts.empty() ||
            !edit.opening_families.empty() || has_geometry_or_relation_intent(intent.intent))
            invalid("wall stacks require a separate reviewed operation from other edit dialects, geometry and relationships");
        // Only actual changes on active shared-baseline owners establish roots.
        // Neither a caller's seed list nor another stack's closure lends authority.
        const auto request=phase_wall_layer_stack_replacement_request(source,edit.wall_stacks);
        if (!request || request->registry_id!=edit.registry_id || request->alternative_id!=edit.alternative_id ||
            request->seed_wall_ids!=edit.seed_wall_ids)
            invalid("wall stack replacement roots do not match actual changes and saved membership");
    }
    if (!edit.opening_rehosts.empty()) {
        if (!edit.wall_profiles.empty() || !edit.opening_profiles.empty() || has_geometry_or_relation_intent(intent.intent))
            invalid("opening rehosts require a separate reviewed operation from geometry, relationships and profiles");
        // Derive authority anew from all three actual original identities. A
        // caller cannot borrow another rehost's seed or an unrelated wall plan.
        const auto request=phase_hosted_opening_rehost_replacement_request(source,edit.opening_rehosts);
        if (!request || request->registry_id!=edit.registry_id || request->alternative_id!=edit.alternative_id ||
            request->seed_wall_ids!=edit.seed_wall_ids)
            invalid("opening rehost replacement roots do not match the actual original hosts and saved choice");
    }
    if (!edit.opening_families.empty()) {
        if (!edit.wall_profiles.empty() || !edit.opening_profiles.empty() || !edit.opening_rehosts.empty() ||
            has_geometry_or_relation_intent(intent.intent))
            invalid("opening families require a separate reviewed operation from geometry, relationships, profiles and rehosts");
        // Re-derive each row's actual shared host. Neither supplied seeds nor
        // a different edit's closure can lend family-conversion authority.
        const auto request = phase_hosted_opening_family_replacement_request(source, edit.opening_families);
        if (!request || request->registry_id != edit.registry_id || request->alternative_id != edit.alternative_id ||
            request->seed_wall_ids != edit.seed_wall_ids)
            invalid("opening family replacement roots do not match the actual original hosts and saved choice");
    }
    Stage stage;
    stage.plan=inspect_phase_wall_replacement_plan(source,edit.seed_wall_ids,edit.registry_id,
        edit.alternative_id,edit.complete_presentations,edit.complete_corner_windows,edit.complete_hosted_instances);
    stage.replacement=replay_phase_wall_replacement(source,stage.plan,edit.identities,edit.hosted_instance_identities);
    auto mapped=remap_phase_wall_replacement_authoring_intent(intent.intent,stage.replacement.original_to_proposed);
    std::erase_if(mapped.relation_mutations,[&](const auto& mutation) {
        const auto found=stage.replacement.deferred_room_constraints.find(mutation.constraint_id);
        if (found==stage.replacement.deferred_room_constraints.end()) return false;
        if (mutation.kind==ConstraintRelationMutationKind::remove)
            stage.deferred_removals.insert(mutation.constraint_id);
        else found->second=encode_constraint_entity(mutation.constraint,&found->second);
        return true;
    });
    stage.entities=stage.replacement.entities;
    if (!edit.corner_profiles.empty()) {
        // Independently derive the original edits before touching the copied
        // stage. Same-value rows need no mapping and retain their exact source.
        const auto originals=replay_corner_window_profile_entities(source,edit.corner_profiles,false);
        auto profiles=edit.corner_profiles;
        std::erase_if(profiles,[&](const auto& profile) {
            return exact(source.at(profile.owner_id),originals.at(profile.owner_id));
        });
        for (auto& profile:profiles) {
            const auto mapped_owner=stage.replacement.original_to_proposed.find(profile.owner_id);
            if (mapped_owner==stage.replacement.original_to_proposed.end())
                invalid("corner profile requires its independently derived proposed owner");
            const auto corner=parse_corner_window(source.at(profile.owner_id));
            for (const auto& id:{corner.wall_ids[0],corner.wall_ids[1],corner.opening_ids[0],corner.opening_ids[1]})
                if (!stage.replacement.original_to_proposed.contains(id))
                    invalid("corner profile requires complete independently derived host and cut copies");
            profile.owner_id=mapped_owner->second;
        }
        stage.entities=replay_corner_window_profile_entities(stage.entities,profiles,false);
    }
    if (!edit.wall_stacks.empty()) {
        std::set<std::string,std::less<>> original_walls,copied_walls,reserved;
        for (const auto& [original,copy]:stage.replacement.original_to_proposed) {
            reserved.insert(copy);
            const auto found=source.find(original);
            if (found!=source.end() && found->second.type=="wall") {
                original_walls.insert(original);copied_walls.insert(copy);
            }
        }
        // Strictly admit the complete independently discovered cohort, including
        // copied hosts and joins outside the authored stack target roster.
        validate_active_wall_physical_dependencies(source,original_walls,true);
        validate_active_wall_physical_dependencies(stage.entities,copied_walls,true);
        auto stacks=edit.wall_stacks;
        std::set<std::string,std::less<>> new_layers;
        for (auto& stack:stacks) {
            const auto& original=source.at(stack.wall_id);
            std::set<std::string,std::less<>> existing;
            if (original.properties.contains("layers"))
                for (const auto& layer:original.properties.at("layers")) existing.insert(layer.at("id").get<std::string>());
            stack.wall_id=stage.replacement.original_to_proposed.at(stack.wall_id);
            for (auto& row:stack.layers) {
                if (existing.contains(row.layer_id)) row.layer_id=stage.replacement.original_to_proposed.at(row.layer_id);
                else {
                    // Actual-source map replay already proves source freshness.
                    // Also reserve all copies, including withheld relationships
                    // and removed original children; never map authored rows.
                    if (reserved.contains(row.layer_id) || !new_layers.insert(row.layer_id).second)
                        invalid("new stack layer collides with a reserved proposed identity");
                }
            }
        }
        stage.entities=replay_wall_layer_stack_entities(stage.entities,stacks,false);
        validate_active_wall_physical_dependencies(stage.entities,copied_walls,true);
        // These exact IDs are additional publication reservations. Removed
        // child copies remain in the original mapping as provenance only.
        stage.replacement.fresh_identity_ids.insert(stage.replacement.fresh_identity_ids.end(),new_layers.begin(),new_layers.end());
        std::sort(stage.replacement.fresh_identity_ids.begin(),stage.replacement.fresh_identity_ids.end());
    }
    if (edit.complete_scalar_profiles) {
        std::set<std::string,std::less<>> original_walls,final_walls;
        const auto map_id=[&](std::string& id) {
            if (const auto found=stage.replacement.original_to_proposed.find(id);
                found!=stage.replacement.original_to_proposed.end()) id=found->second;
        };
        for (const auto& [original,proposed]:stage.replacement.original_to_proposed) {
            const auto found=source.find(original);
            if (found!=source.end() && found->second.type=="wall") {
                original_walls.insert(original);final_walls.insert(proposed);
            }
        }
        for (const auto& profile:edit.wall_profiles) original_walls.insert(profile.wall_id);
        for (const auto& profile:edit.opening_profiles) original_walls.insert(profile.wall_id);
        validate_active_wall_physical_dependencies(source,original_walls,true,edit.complete_vertical_profiles);
        // Each intent is admitted against its actual original owner first.
        // The copied envelope comes solely from the reconstructed replacement;
        // remapping typed owned IDs is the only change to its scalar authority.
        // Stage both families before fit checks: a shorter wall and resized cut
        // must not be rejected because an intermediate half-edit cannot fit.
        for (auto profile:edit.wall_profiles) {
            (void)replay_wall_profile_entity(source.at(profile.wall_id),profile);
            map_id(profile.wall_id);
            if (profile.layer_thicknesses) for (auto& layer:*profile.layer_thicknesses) map_id(layer.layer_id);
            stage.entities.at(profile.wall_id)=replay_wall_profile_entity(
                stage.replacement.entities.at(profile.wall_id),profile);
            final_walls.insert(profile.wall_id);
        }
        for (auto profile:edit.opening_profiles) {
            (void)replay_hosted_opening_profile_entity(source.at(profile.opening_id),profile);
            map_id(profile.opening_id);map_id(profile.wall_id);
            stage.entities.at(profile.opening_id)=replay_hosted_opening_profile_entity(
                stage.replacement.entities.at(profile.opening_id),profile);
            final_walls.insert(profile.wall_id);
        }
        validate_active_wall_physical_dependencies(stage.entities,final_walls,true,edit.complete_vertical_profiles);
        const auto measurements=exterior_wall_measurement_source_updates_active_phase(
            stage.replacement.entities,stage.entities,false);
        stage.entities=edited_boundary_entities_batch(stage.entities,measurements);
    }
    if (!edit.complete_scalar_profiles && !edit.wall_profiles.empty()) {
        const bool strict_profiles=std::any_of(edit.wall_profiles.begin(),edit.wall_profiles.end(),
            strict_profile_admission);
        std::set<std::string,std::less<>> original_walls,copied_walls;
        if (strict_profiles) {
            for (const auto& [original,copy]:stage.replacement.original_to_proposed) {
                const auto found=source.find(original);
                if (found!=source.end() && found->second.type=="wall") {
                    original_walls.insert(original);copied_walls.insert(copy);
                }
            }
            validate_active_wall_physical_dependencies(source,original_walls,true);
            validate_active_wall_physical_dependencies(stage.entities,copied_walls,true);
        }
        auto profiles=edit.wall_profiles;
        for (auto& profile:profiles) {
            if (!std::binary_search(edit.seed_wall_ids.begin(),edit.seed_wall_ids.end(),profile.wall_id))
                invalid("profile edit must name an explicitly seeded original wall");
            profile.wall_id=stage.replacement.original_to_proposed.at(profile.wall_id);
            // Complete layer rosters are typed child identity references, never
            // caller supplied replacement wall/layer payloads.
            if (profile.layer_thicknesses) for (auto& layer:*profile.layer_thicknesses)
                layer.layer_id=stage.replacement.original_to_proposed.at(layer.layer_id);
        }
        stage.entities=replay_wall_profile_entities(stage.entities,profiles,false);
        if (strict_profiles) validate_active_wall_physical_dependencies(stage.entities,copied_walls,true);
    }
    if (!edit.complete_scalar_profiles && !edit.opening_profiles.empty()) {
        if (!edit.wall_profiles.empty() || has_geometry_or_relation_intent(mapped))
            invalid("opening profiles require a separate reviewed operation from wall geometry or relationships");
        auto profiles = edit.opening_profiles;
        for (auto& profile : profiles) {
            if (!std::binary_search(edit.seed_wall_ids.begin(), edit.seed_wall_ids.end(), profile.wall_id))
                invalid("opening profile must name an explicitly seeded original host");
            const auto original = source.find(profile.opening_id);
            if (original == source.end() || original->second.type != "opening" ||
                original->second.properties.at("wall_id") != profile.wall_id)
                invalid("opening profile must retain its actual original host");
            profile.opening_id = stage.replacement.original_to_proposed.at(profile.opening_id);
            profile.wall_id = stage.replacement.original_to_proposed.at(profile.wall_id);
        }
        stage.entities = replay_hosted_opening_profile_entities(stage.entities, profiles, false);
        std::set<std::string, std::less<>> copied_walls;
        for (const auto& [original, proposed] : stage.replacement.original_to_proposed) {
            const auto owner = source.find(original);
            if (owner != source.end() && owner->second.type == "wall") copied_walls.insert(proposed);
        }
        // Document replay derives the same complete physical admission as the
        // desktop preview, including copied hosts outside the edited roster.
        validate_active_wall_physical_dependencies(stage.entities, copied_walls);
    }
    if (!edit.opening_rehosts.empty()) {
        auto rehosts=edit.opening_rehosts;
        const auto original_model=ModelPhases::from_json(source.at(edit.registry_id).properties.at("model"));
        const std::set<std::string,std::less<>> baseline(original_model.baseline_ids().begin(),original_model.baseline_ids().end());
        const auto map_identity=[&](std::string& id) {
            if (const auto copied=stage.replacement.original_to_proposed.find(id);
                copied!=stage.replacement.original_to_proposed.end()) id=copied->second;
        };
        for (auto& rehost:rehosts) {
            if (baseline.contains(rehost.opening_id) &&
                !stage.replacement.original_to_proposed.contains(rehost.opening_id))
                invalid("a retained baseline opening must have its own independently derived proposed copy");
            // Mapping is independent for opening, old host and target host.
            // An existing proposed/nonshared owner retains its actual identity.
            map_identity(rehost.opening_id);
            map_identity(rehost.original_wall_id);
            map_identity(rehost.target_wall_id);
        }
        stage.entities=replay_hosted_opening_rehost_entities(stage.entities,rehosts,false);
        std::set<std::string,std::less<>> copied_walls;
        for (const auto& [original,proposed]:stage.replacement.original_to_proposed) {
            const auto owner=source.find(original);
            if (owner!=source.end() && owner->second.type=="wall") copied_walls.insert(proposed);
        }
        validate_active_wall_physical_dependencies(stage.entities,copied_walls);
    }
    if (!edit.opening_families.empty()) {
        auto families = edit.opening_families;
        std::set<std::string, std::less<>> original_walls, copied_walls;
        for (const auto& [original, proposed] : stage.replacement.original_to_proposed) {
            const auto owner = source.find(original);
            if (owner != source.end() && owner->second.type == "wall") {
                original_walls.insert(original);
                copied_walls.insert(proposed);
            }
        }
        // Record five independently admits complete original/copied profiles;
        // earlier recorded dialects retain their original replay policy.
        validate_active_wall_physical_dependencies(source, original_walls,true);
        validate_active_wall_physical_dependencies(stage.entities, copied_walls,true);
        for (auto& family : families) {
            const auto opening = stage.replacement.original_to_proposed.find(family.opening_id);
            const auto host = stage.replacement.original_to_proposed.find(family.wall_id);
            if (opening == stage.replacement.original_to_proposed.end() ||
                host == stage.replacement.original_to_proposed.end())
                invalid("opening family requires its own independently derived proposed opening and host");
            family.opening_id = opening->second;
            family.wall_id = host->second;
        }
        stage.entities = replay_hosted_opening_family_entities(stage.entities, families, false);
        validate_active_wall_physical_dependencies(stage.entities, copied_walls,true);
    }
    if (has_geometry_or_relation_intent(mapped))
        stage.entities=reconstruct_active_phase_constraint_authoring(stage.entities,mapped);
    else if (edit.wall_profiles.empty() && edit.opening_profiles.empty() && edit.opening_rehosts.empty() &&
        edit.opening_families.empty() && edit.wall_stacks.empty() && edit.corner_profiles.empty()) invalid("replacement has no semantic edit");
    if (edit.complete_corner_windows) {
        // Identity remapping has already produced a complete valid physical
        // source. Geometry consequences complete against that copied source,
        // retaining the originals and every other saved alternative.
        complete_corner_window_geometry(stage.replacement.entities,stage.entities);
        validate_corner_window_state(stage.entities);
    }
    complete_hosted_wall_carry(stage,mapped);
    preserve_baseline(source,stage.entities,stage.plan);
    // Incoming room evidence sees every independently reconstructed new
    // relationship. These copies were withheld from the physical solve; only
    // the final explicit disposition can establish their new room endpoints.
    for (const auto& [id,entity]:stage.replacement.deferred_room_constraints) {
        if (!stage.entities.emplace(id,entity).second) invalid("deferred relationship identity is occupied");
        stage.deferred.insert(id);
    }
    return stage;
}
PhysicalWallRoomPhaseReviewIntent room_binding(const PhaseConstraintAuthoringIntent& source,
    const PhaseWallReplacementAuthoring& edit,const Entities& stage) {
    PhysicalWallRoomPhaseReviewIntent result;
    result.source_snapshot_digest=source.source_snapshot_digest;
    result.source_authoring_digest=source.source_authoring_digest;
    result.source_saved_revision=source.source_saved_revision;
    result.expected_revision=source.expected_revision;
    result.source_entities_digest=entity_map_digest(stage);
    result.registry_id=edit.registry_id;result.alternative_id=edit.alternative_id;
    const auto& registry=stage.at(edit.registry_id);
    result.source_registry_entity_digest=entity_map_digest(Entities{{registry.id,registry}});
    result.registry_command_proof=command_to_json(Command{ApplyEntityChanges{source.expected_revision,
        {EntityChange::upsert(registry)},{} ,"Review proposed rooms"}});
    return result;
}
bool requires_rooms(const Stage& stage,const PhysicalWallRoomPhaseReviewInventory& inventory) {
    return !stage.replacement.affected_original_room_ids.empty() || !stage.deferred.empty() ||
        std::any_of(inventory.reports.begin(),inventory.reports.end(),[](const auto& report) {
            return !report.correspondence.retained.empty() || !report.correspondence.fresh.empty();
        });
}
void validate_room_binding(const PhaseConstraintAuthoringIntent& original,const PhaseWallReplacementAuthoring& edit,
    const Entities& stage,const PhysicalWallRoomPhaseReviewIntent& room) {
    const auto expected=room_binding(original,edit,stage);
    if (room.expected_revision!=expected.expected_revision || room.source_snapshot_digest!=expected.source_snapshot_digest ||
        room.source_authoring_digest!=expected.source_authoring_digest || room.source_saved_revision!=expected.source_saved_revision ||
        room.source_entities_digest!=expected.source_entities_digest || room.registry_id!=expected.registry_id ||
        room.alternative_id!=expected.alternative_id || room.source_registry_entity_digest!=expected.source_registry_entity_digest)
        invalid("room decisions do not bind the original capture and reconstructed physical stage");
    const auto proof=command_from_json(room.registry_command_proof);
    const auto* raw=std::get_if<ApplyEntityChanges>(&proof);
    if (!raw || raw->expected_revision!=original.expected_revision || !raw->asset_changes.empty() ||
        raw->entity_changes.size()!=1 || raw->entity_changes.front().kind!=EntityChangeKind::upsert ||
        !exact(raw->entity_changes.front().entity,stage.at(edit.registry_id)))
        invalid("room review cannot replace the independently reconstructed wall registry");
}
void complete_room_constraints(Stage& stage,const PhaseWallReplacementAuthoring& edit) {
    std::map<std::string,const PhaseWallRoomConstraintDecision*,std::less<>> decisions;
    for (const auto& choice:edit.room_constraint_decisions)
        if (!decisions.emplace(choice.constraint_id,&choice).second) invalid("duplicate room relationship decision");
    if (decisions.size()!=stage.replacement.room_constraint_ids_requiring_review.size())
        invalid("review every copied room relationship individually");
    const auto scope=constraint_phase_scope(stage.entities);
    const auto original_organization=organize_project(stage.replacement.entities);
    const auto reviewed_organization=organize_project(stage.entities);
    for (const auto& original_id:stage.replacement.room_constraint_ids_requiring_review) {
        if (!decisions.contains(original_id)) invalid("copied room relationship has no explicit disposition: "+original_id);
        const auto& choice=*decisions.at(original_id);
        const auto fresh_id=stage.replacement.original_to_proposed.at(original_id);
        const auto& reconstructed=stage.replacement.deferred_room_constraints.at(fresh_id);
        if (stage.deferred_removals.contains(fresh_id) && choice.disposition!=PhaseWallRoomConstraintDisposition::omit)
            invalid("the requested relationship removal must omit its proposed copy");
        if (choice.disposition==PhaseWallRoomConstraintDisposition::omit) {
            if (!choice.endpoints.empty()) invalid("omitted relationship cannot carry endpoint mappings");
            stage.entities.erase(fresh_id);continue;
        }
        auto decoded=decode_constraint_entity(reconstructed);
        if (!decoded.supported()) invalid("copied room relationship is unsupported");
        auto relation=*decoded.constraint;
        std::set<std::size_t> room_indices,mapped;
        for (std::size_t index=0;index<relation.bindings.size();++index) {
            const auto found=stage.replacement.entities.find(relation.bindings[index].owner_id);
            if (found!=stage.replacement.entities.end() && is_physical_wall_room(found->second)) room_indices.insert(index);
        }
        if (choice.disposition==PhaseWallRoomConstraintDisposition::keep && !choice.endpoints.empty())
            invalid("unchanged relationship cannot carry endpoint mappings");
        for (const auto& endpoint:choice.endpoints) {
            if (!room_indices.contains(endpoint.binding_index) || !mapped.insert(endpoint.binding_index).second)
                invalid("mapping must name each original physical-room binding once");
            const auto found=stage.entities.find(endpoint.target.owner_id);
            if (found==stage.entities.end() || !is_physical_wall_room(found->second) ||
                scope.inactive_owner_ids.contains(endpoint.target.owner_id)) invalid("mapped room must be active in the selected design");
            const auto original_context=original_organization.drawing_context(relation.bindings.at(endpoint.binding_index).owner_id);
            const auto target_context=reviewed_organization.drawing_context(endpoint.target.owner_id);
            if (!original_context || !target_context || *original_context!=*target_context)
                invalid("mapped room endpoint must remain in its original drawing context");
            const auto original_plane=validate_retained_physical_wall_room_lineage(
                stage.replacement.entities.at(relation.bindings.at(endpoint.binding_index).owner_id),*original_context);
            const auto target_plane=validate_retained_physical_wall_room_lineage(found->second,*target_context);
            if (std::abs(original_plane.effective_elevation_m-target_plane.effective_elevation_m)>default_geometry_tolerance_metres)
                invalid("mapped room endpoint must remain in its original physical plane");
            relation.bindings.at(endpoint.binding_index)=endpoint.target;
        }
        if (choice.disposition==PhaseWallRoomConstraintDisposition::remap && mapped!=room_indices)
            invalid("map every physical-room endpoint explicitly");
        if (!constraint_participates(relation,scope)) invalid("copied relationship still targets an inactive design; map it or omit the copy");
        auto replacement=encode_constraint_entity(relation,&reconstructed);
        if (choice.disposition==PhaseWallRoomConstraintDisposition::keep && !exact(replacement,reconstructed))
            invalid("unchanged relationship altered its reconstructed payload");
        stage.entities.insert_or_assign(fresh_id,std::move(replacement));
    }
}
void complete_surviving_presentations(Stage& stage,const Entities& source,
    const PhaseWallReplacementAuthoring& edit) {
    if (!edit.complete_presentations) return;
    std::set<std::string,std::less<>> absent;
    // Entity copies can disappear through semantic removal or an explicit room
    // disposition. Owned overlay, segment and layer IDs are not entity copies.
    for (const auto& original_id:stage.plan.required_entity_ids) {
        const auto& fresh_id=stage.replacement.original_to_proposed.at(original_id);
        if (!stage.entities.contains(fresh_id)) absent.insert(fresh_id);
    }
    if (absent.empty()) return;
    const auto absent_reference=[&](const Json& row,const char* key) {
        return row.contains(key) && absent.contains(row.at(key).get<std::string>());
    };
    const auto prune=[](Json& rows,const auto& predicate) {
        return std::erase_if(rows.get_ref<Json::array_t&>(),predicate)!=0;
    };
    // Replay reserves fresh IDs against every source string. Thus only newly
    // appended source-derived rows can match; erase in place to retain all raw
    // original rows, envelope fields and the order of surviving additions.
    for (const auto& [id,original]:source) {
        if (original.type!=kSheetViewEntityType && original.type!=kAnnotationEntityType) continue;
        auto& candidate=stage.entities.at(id);
        if (candidate.properties==original.properties) continue;
        bool changed=false;
        if (original.type==kSheetViewEntityType) {
            for (auto& view:candidate.properties.at("model").at("views")) {
                if (view.contains("object_ids"))
                    changed=prune(view.at("object_ids"),[&](const Json& row) {
                        return absent.contains(row.get<std::string>());
                    }) || changed;
                auto& presentation=view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    changed=prune(presentation.at("appearance").at("objects"),[&](const Json& row) {
                        return absent_reference(row,"object_id");
                    }) || changed;
                if (view.contains("overlays"))
                    changed=prune(view.at("overlays"),[&](const Json& row) {
                        return absent_reference(row,"object_id") ||
                            (row.contains("dimension_binding") && !row.at("dimension_binding").is_null() &&
                             absent_reference(row.at("dimension_binding"),"object_id"));
                    }) || changed;
            }
            if (changed) validate_sheet_view_entity(candidate);
        } else {
            changed=prune(candidate.properties.at("state").at("overrides"),[&](const Json& row) {
                return absent_reference(row,"target_id");
            });
            if (changed) validate_annotation_entity(candidate);
        }
    }
    // Keep absent copies and all child IDs declared in the identity mapping and
    // fresh-ID inventory: omitting a row does not release its reservation.
}
} // namespace

std::optional<PhaseWallReplacementRequest> phase_wall_layer_stack_replacement_request(
    const Entities& source,const std::vector<WallLayerStackEditIntent>& stacks) {
    if (stacks.size()>2048) invalid("invalid wall stack inventory");
    if (stacks.empty()) return std::nullopt;
    // Replay complete real source entities, including global row-ID freshness,
    // context, material catalogs, hosted cuts and joined native profiles. Only
    // final relationship residuals wait for enclosing physical-room review.
    const auto physical=replay_wall_layer_stack_entities(source,stacks,false);
    const auto scope=constraint_phase_scope(source);
    std::optional<PhaseWallReplacementRequest> request;
    std::size_t ordinary=0;
    for (const auto& stack:stacks) {
        if (exact(source.at(stack.wall_id),physical.at(stack.wall_id))) continue;
        const PhysicalWallPhaseState* membership=nullptr;
        for (const auto& registry:scope.registries)
            if (std::find(registry.registered_entity_ids.begin(),registry.registered_entity_ids.end(),stack.wall_id)!=
                registry.registered_entity_ids.end()) {
                if (membership) invalid("wall stack target has overlapping registry membership");
                membership=&registry;
            }
        if (!membership) { ++ordinary;continue; }
        const auto model=ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        if (!model.active_alternative() ||
            std::find(model.baseline_ids().begin(),model.baseline_ids().end(),stack.wall_id)==model.baseline_ids().end()) {
            ++ordinary;continue;
        }
        if (request && (request->registry_id!=membership->registry_id ||
            request->alternative_id!=*model.active_alternative()))
            invalid("wall stacks span different saved-active design registries");
        if (!request) request=PhaseWallReplacementRequest{membership->registry_id,*model.active_alternative(),{}};
        request->seed_wall_ids.push_back(stack.wall_id);
    }
    if (request && ordinary) invalid("wall stacks mix shared-baseline and ordinary/proposed owners");
    if (request) std::sort(request->seed_wall_ids.begin(),request->seed_wall_ids.end());
    return request;
}

std::optional<PhaseWallReplacementRequest> phase_wall_scalar_profile_replacement_request(
    const Entities& source,const std::vector<WallProfileEditIntent>& walls,
    const std::vector<HostedOpeningProfileEditIntent>& openings) {
    if (walls.size()>2048 || openings.size()>2048-walls.size()) invalid("scalar cohort target budget exceeded");
    const auto scope=constraint_phase_scope(source);
    std::set<std::string,std::less<>> targets,seeds;
    std::optional<PhaseWallReplacementRequest> request;
    std::size_t bytes{};
    const auto record=[&](const std::string& owner,const std::string& host,const Entity& candidate,
                          const Json& encoded) {
        const auto count=encoded.dump().size();
        if (count>1024*1024-bytes) invalid("scalar cohort proof byte budget exceeded");
        bytes+=count;
        if (!targets.insert(owner).second) invalid("duplicate scalar cohort owner");
        const auto original=source.find(owner),wall=source.find(host);
        if (original==source.end() || original->first!=original->second.id ||
            wall==source.end() || wall->first!=wall->second.id || wall->second.type!="wall")
            invalid("scalar cohort owner or host is missing or inconsistent");
        if (scope.inactive_owner_ids.contains(owner) || scope.inactive_owner_ids.contains(host))
            invalid("scalar cohort owner or host is inactive in the saved design");
        // Even a retained member is admitted, but an exact no-op cannot grant
        // fresh identity or baseline demolition authority to another operation.
        if (exact(original->second,candidate)) return;
        const PhysicalWallPhaseState* membership=nullptr;
        for (const auto& registry:scope.registries)
            if (std::find(registry.registered_entity_ids.begin(),registry.registered_entity_ids.end(),host)!=
                registry.registered_entity_ids.end()) {
                if (membership) invalid("scalar host has overlapping registry membership");
                membership=&registry;
            }
        if (!membership) return;
        const auto model=ModelPhases::from_json(source.at(membership->registry_id).properties.at("model"));
        if (!model.active_alternative() ||
            std::find(model.baseline_ids().begin(),model.baseline_ids().end(),host)==model.baseline_ids().end()) return;
        if (request && (request->registry_id!=membership->registry_id || request->alternative_id!=*model.active_alternative()))
            invalid("changed scalar baseline hosts span saved-active registries");
        if (!request) request=PhaseWallReplacementRequest{membership->registry_id,*model.active_alternative(),{}};
        seeds.insert(host);
    };
    for (const auto& profile:walls) {
        const auto encoded=encode_wall_profile_edit_intent(profile);
        const auto original=source.find(profile.wall_id);
        if (original==source.end()) invalid("scalar wall target is missing");
        record(profile.wall_id,profile.wall_id,replay_wall_profile_entity(original->second,profile),encoded);
    }
    for (const auto& profile:openings) {
        const auto encoded=encode_hosted_opening_profile_edit_intent(profile);
        const auto original=source.find(profile.opening_id);
        if (original==source.end()) invalid("scalar opening target is missing");
        record(profile.opening_id,profile.wall_id,replay_hosted_opening_profile_entity(original->second,profile),encoded);
    }
    if (request) request->seed_wall_ids.assign(seeds.begin(),seeds.end());
    return request;
}

Json encode_phase_wall_replacement_authoring(const PhaseWallReplacementAuthoring& value) {
    if (!value.complete_hosted_instances && !value.hosted_instance_identities.empty())
        invalid("qualified hosted identities require explicit leaf-eleven authority");
    if (value.complete_hosted_instances) {
        if (value.identities.size()>4096 || value.hosted_instance_identities.size()>4096-value.identities.size())
            invalid("replacement entity/child/hosted identity budget exceeded");
        auto historical=value;
        historical.complete_hosted_instances=false;
        historical.hosted_instance_identities.clear();
        Json result{{"version",11},{"complete_hosted_instances",true},
            {"authoring",encode_phase_wall_replacement_authoring(historical)},
            {"hosted_instance_identities",Json::array()}};
        std::set<std::string,std::less<>> destinations;
        for (const auto& [old_id,new_id]:value.identities) {
            destinations.insert(old_id);destinations.insert(new_id);
        }
        for (const auto& [key,new_id]:value.hosted_instance_identities) {
            (void)identity(Json(key.first));(void)hosted_source_identity(Json(key.second));(void)identity(Json(new_id));
            if (new_id==key.second || !destinations.insert(new_id).second)
                invalid("qualified hosted destinations must be fresh, injective and disjoint from flat identities");
            result.at("hosted_instance_identities").push_back(
                {{"catalog_id",key.first},{"instance_id",key.second},{"proposed_id",new_id}});
        }
        if (result.dump().size()>1024*1024) invalid("replacement decisions exceed one MiB");
        return result;
    }
    const bool elevation=std::any_of(value.wall_profiles.begin(),value.wall_profiles.end(),[](const auto& profile) {
        return profile.elevation.has_value();
    });
    if (value.complete_vertical_profiles!=elevation || (elevation && !value.complete_scalar_profiles))
        invalid("elevation edits require explicit complete leaf-ten scalar authority");
    if (value.complete_scalar_profiles) {
        if (!value.complete_presentations || !value.complete_corner_windows ||
            (value.wall_profiles.empty() && value.opening_profiles.empty()) ||
            value.wall_profiles.size()>2048 || value.opening_profiles.size()>2048-value.wall_profiles.size() ||
            !value.opening_rehosts.empty() || !value.opening_families.empty() ||
            !value.wall_stacks.empty() || !value.corner_profiles.empty())
            invalid("scalar cohort requires bounded wall/opening authority and complete presentation/corner admission");
        auto base=value;
        base.complete_vertical_profiles=false;base.complete_scalar_profiles=false;
        base.complete_presentations=false;base.complete_corner_windows=false;
        base.wall_profiles.clear();base.opening_profiles.clear();
        // The old flat record's envelope/room decisions are reused verbatim;
        // Version nine admits historical scalar arrays; ten explicitly adds
        // elevation. Both retain the same closed identity/room envelope.
        Json result{{"version",value.complete_vertical_profiles ? 10 : 9},{"complete_presentations",true},
            {"authoring",encode_phase_wall_replacement_authoring(base)},
            {"wall_profiles",Json::array()},{"opening_profiles",Json::array()}};
        std::set<std::string,std::less<>> targets;
        for (const auto& profile:value.wall_profiles) {
            if (!targets.insert(profile.wall_id).second) invalid("duplicate scalar wall target");
            result["wall_profiles"].push_back(encode_wall_profile_edit_intent(profile));
        }
        for (const auto& profile:value.opening_profiles) {
            if (!targets.insert(profile.opening_id).second) invalid("duplicate scalar opening target");
            result["opening_profiles"].push_back(encode_hosted_opening_profile_edit_intent(profile));
        }
        if (result.dump().size()>1024*1024) invalid("replacement decisions exceed one MiB");
        return result;
    }
    if (value.opening_families.size() > 2048) invalid("invalid opening family inventory");
    if (value.wall_stacks.size()>2048) invalid("invalid wall stack inventory");
    if (value.corner_profiles.size()>2048) invalid("invalid corner profile inventory");
    if (value.complete_corner_windows && !value.complete_presentations)
        invalid("corner completion requires complete presentation admission");
    if (!value.corner_profiles.empty() && (!value.complete_corner_windows || !value.wall_profiles.empty() ||
        !value.opening_profiles.empty() || !value.opening_rehosts.empty() || !value.opening_families.empty() ||
        !value.wall_stacks.empty())) invalid("corner profiles require exclusive leaf-eight edit authority");
    Json decisions=Json::array();
    for (const auto& choice:value.room_constraint_decisions) {
        Json endpoints=Json::array();
        for (const auto& endpoint:choice.endpoints) endpoints.push_back({{"binding_index",endpoint.binding_index},
            {"owner_id",endpoint.target.owner_id},{"segment_id",endpoint.target.segment_id},
            {"vertex_id",endpoint.target.vertex_id},{"role",wall_endpoint_role_name(endpoint.target.role)}});
        decisions.push_back({{"constraint_id",choice.constraint_id},{"disposition",disposition(choice.disposition)},
            {"endpoints",std::move(endpoints)}});
    }
    Json result={{"version",1},{"registry_id",value.registry_id},{"alternative_id",value.alternative_id},
        {"seed_wall_ids",value.seed_wall_ids},{"identities",value.identities},{"room_review_intent",value.room_review_intent},
        {"room_constraint_decisions",std::move(decisions)}};
    if (!value.wall_profiles.empty()) {
        result["version"]=2;result["wall_profiles"]=Json::array();
        for (const auto& profile:value.wall_profiles)
            result["wall_profiles"].push_back(encode_wall_profile_edit_intent(profile));
    }
    if (!value.opening_profiles.empty()) {
        if (!value.wall_profiles.empty()) invalid("wall and opening profiles require separate reviewed operations");
        result["version"] = 3; result["opening_profiles"] = Json::array();
        for (const auto& profile : value.opening_profiles)
            result["opening_profiles"].push_back(encode_hosted_opening_profile_edit_intent(profile));
    }
    if (!value.opening_rehosts.empty()) {
        if (!value.wall_profiles.empty() || !value.opening_profiles.empty())
            invalid("opening rehosts and profiles require separate reviewed operations");
        result["version"]=4;result["opening_rehosts"]=Json::array();
        for (const auto& rehost:value.opening_rehosts)
            result["opening_rehosts"].push_back(encode_hosted_opening_rehost_intent(rehost));
    }
    if (!value.opening_families.empty()) {
        if (!value.wall_profiles.empty() || !value.opening_profiles.empty() || !value.opening_rehosts.empty())
            invalid("opening families and other edit dialects require separate reviewed operations");
        result["version"] = 5; result["opening_families"] = Json::array();
        std::set<std::string, std::less<>> targets;
        for (const auto& family : value.opening_families) {
            if (!targets.insert(family.opening_id).second ||
                !std::binary_search(value.seed_wall_ids.begin(), value.seed_wall_ids.end(), family.wall_id))
                invalid("opening family must name a unique original opening and its own seeded host");
            result["opening_families"].push_back(encode_hosted_opening_family_edit_intent(family));
        }
    }
    if (!value.wall_stacks.empty()) {
        if (!value.wall_profiles.empty() || !value.opening_profiles.empty() || !value.opening_rehosts.empty() ||
            !value.opening_families.empty()) invalid("wall stacks and other edit dialects require separate reviewed operations");
        result["version"]=6;result["wall_stacks"]=Json::array();
        std::set<std::string,std::less<>> targets;
        for (const auto& stack:value.wall_stacks) {
            if (!targets.insert(stack.wall_id).second) invalid("duplicate wall stack target");
            result["wall_stacks"].push_back(encode_wall_layer_stack_edit_intent(stack));
        }
        if (std::vector<std::string>(targets.begin(),targets.end())!=value.seed_wall_ids)
            invalid("wall stack seeds must exactly match the authored original targets");
    }
    // The decoder below is the single strict semantic admission path. Encoding
    // never supplies geometry, inferred room mappings or arbitrary clone data.
    if (value.complete_corner_windows) {
        Json profiles=Json::array();
        std::set<std::string,std::less<>> targets;
        for (const auto& profile:value.corner_profiles) {
            if (!targets.insert(profile.owner_id).second) invalid("duplicate corner profile target");
            profiles.push_back(encode_corner_window_profile_edit_intent(profile));
        }
        result=Json{{"version",8},{"complete_presentations",true},{"authoring",std::move(result)},
            {"corner_profiles",std::move(profiles)}};
    } else if (value.complete_presentations)
        result = Json{{"version",7},{"complete_presentations",true},{"authoring",std::move(result)}};
    if (result.dump().size()>1024*1024) invalid("replacement decisions exceed one MiB");
    return result;
}
PhaseWallReplacementAuthoring decode_phase_wall_replacement_authoring(const Json& value) {
    if (value.dump().size()>1024*1024) invalid("replacement decisions exceed one MiB");
    if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
        value.at("version")==11) {
        keys(value,{"version","complete_hosted_instances","authoring","hosted_instance_identities"});
        if (!value.at("complete_hosted_instances").is_boolean() || value.at("complete_hosted_instances")!=true)
            invalid("hosted completion requires explicit true authority");
        const auto& body=value.at("authoring");
        if (!body.is_object() || !body.contains("version") || !body.at("version").is_number_integer() ||
            body.at("version")<1 || body.at("version")>10)
            invalid("hosted completion requires one canonical historical authoring body");
        auto result=decode_phase_wall_replacement_authoring(body);
        const auto& rows=value.at("hosted_instance_identities");
        if (!rows.is_array() || rows.size()>4096-result.identities.size())
            invalid("replacement entity/child/hosted identity budget exceeded");
        result.complete_hosted_instances=true;
        for (const auto& row:rows) {
            keys(row,{"catalog_id","instance_id","proposed_id"});
            PhaseWallReplacementHostedInstanceKey key{identity(row.at("catalog_id")),hosted_source_identity(row.at("instance_id"))};
            if (!result.hosted_instance_identities.emplace(std::move(key),identity(row.at("proposed_id"))).second)
                invalid("duplicate qualified hosted source identity");
        }
        if (encode_phase_wall_replacement_authoring(result).dump()!=value.dump())
            invalid("hosted completion wrapper is not canonical");
        return result;
    }
    if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
        (value.at("version")==9 || value.at("version")==10)) {
        keys(value,{"version","complete_presentations","authoring","wall_profiles","opening_profiles"});
        if (!value.at("complete_presentations").is_boolean() || value.at("complete_presentations")!=true)
            invalid("scalar cohort requires complete presentations");
        const auto& base=value.at("authoring");
        if (!base.is_object() || !base.contains("version") || !base.at("version").is_number_integer() || base.at("version")!=1)
            invalid("scalar cohort requires one flat identity/room envelope");
        auto result=decode_flat_phase_wall_replacement_authoring(base,true);
        result.complete_presentations=true;result.complete_corner_windows=true;result.complete_scalar_profiles=true;
        result.complete_vertical_profiles=value.at("version")==10;
        const auto& walls=value.at("wall_profiles");const auto& openings=value.at("opening_profiles");
        if (!walls.is_array() || !openings.is_array() || walls.size()>2048 || openings.size()>2048-walls.size())
            invalid("scalar cohort target budget exceeded");
        for (const auto& row:walls) result.wall_profiles.push_back(decode_wall_profile_edit_intent(row));
        for (const auto& row:openings) result.opening_profiles.push_back(decode_hosted_opening_profile_edit_intent(row));
        if (encode_phase_wall_replacement_authoring(result).dump()!=value.dump()) invalid("scalar cohort is not canonical");
        return result;
    }
    if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
        value.at("version")==8) {
        keys(value,{"version","complete_presentations","authoring","corner_profiles"});
        if (!value.at("complete_presentations").is_boolean() || value.at("complete_presentations")!=true)
            invalid("corner completion wrapper requires complete presentations");
        const auto& body=value.at("authoring");
        if (!body.is_object() || !body.contains("version") || !body.at("version").is_number_integer() ||
            body.at("version")<1 || body.at("version")>6)
            invalid("corner completion requires a flat existing authoring dialect");
        auto result=decode_flat_phase_wall_replacement_authoring(body,true);
        result.complete_presentations=true;
        result.complete_corner_windows=true;
        const auto& rows=value.at("corner_profiles");
        if (!rows.is_array() || rows.size()>2048) invalid("invalid corner profile inventory");
        for (const auto& row:rows) result.corner_profiles.push_back(decode_corner_window_profile_edit_intent(row));
        if (encode_phase_wall_replacement_authoring(result).dump()!=value.dump())
            invalid("corner completion wrapper is not canonical");
        return result;
    }
    if (value.is_object() && value.contains("version") && value.at("version").is_number_integer() &&
        value.at("version") == 7) {
        keys(value,{"version","complete_presentations","authoring"});
        if (!value.at("complete_presentations").is_boolean() || value.at("complete_presentations") != true)
            invalid("presentation completion wrapper requires an explicit true flag");
        const auto& body = value.at("authoring");
        // One closed wrapper only. Its flat body admits the existing overlay
        // source vocabulary solely in old mapping keys, never destinations.
        if (!body.is_object() || !body.contains("version") || !body.at("version").is_number_integer() ||
            body.at("version") < 1 || body.at("version") > 6)
            invalid("presentation completion requires a flat existing authoring dialect");
        auto result = decode_flat_phase_wall_replacement_authoring(body, true);
        result.complete_presentations = true;
        if (encode_phase_wall_replacement_authoring(result).dump() != value.dump())
            invalid("presentation completion wrapper is not canonical");
        return result;
    }
    return decode_flat_phase_wall_replacement_authoring(value, false);
}
namespace {
PhaseWallReplacementAuthoring decode_flat_phase_wall_replacement_authoring(
    const Json& value, bool allow_presentation_source_ids) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 &&
         value.at("version")!=5 && value.at("version")!=6)) invalid("unsupported replacement version");
    const bool profiles=value.at("version")==2;
    if (profiles) keys(value,{"version","registry_id","alternative_id","seed_wall_ids","identities",
        "room_review_intent","room_constraint_decisions","wall_profiles"});
    else if (value.at("version") == 3) keys(value,{"version","registry_id","alternative_id","seed_wall_ids","identities",
        "room_review_intent","room_constraint_decisions","opening_profiles"});
    else if (value.at("version") == 4) keys(value,{"version","registry_id","alternative_id","seed_wall_ids","identities",
        "room_review_intent","room_constraint_decisions","opening_rehosts"});
    else if (value.at("version") == 5) keys(value,{"version","registry_id","alternative_id","seed_wall_ids","identities",
        "room_review_intent","room_constraint_decisions","opening_families"});
    else if (value.at("version") == 6) keys(value,{"version","registry_id","alternative_id","seed_wall_ids","identities",
        "room_review_intent","room_constraint_decisions","wall_stacks"});
    else keys(value,{"version","registry_id","alternative_id","seed_wall_ids","identities","room_review_intent","room_constraint_decisions"});
    PhaseWallReplacementAuthoring result;
    result.registry_id=identity(value.at("registry_id"));result.alternative_id=identity(value.at("alternative_id"));
    const auto& seeds=value.at("seed_wall_ids");
    if (!seeds.is_array() || seeds.empty() || seeds.size()>2048) invalid("invalid wall seed inventory");
    for (const auto& seed:seeds) result.seed_wall_ids.push_back(identity(seed));
    if (!std::is_sorted(result.seed_wall_ids.begin(),result.seed_wall_ids.end()) ||
        std::adjacent_find(result.seed_wall_ids.begin(),result.seed_wall_ids.end())!=result.seed_wall_ids.end()) invalid("wall seeds must be unique and sorted");
    if (profiles) {
        const auto& edits=value.at("wall_profiles");
        if (!edits.is_array() || edits.empty() || edits.size()>2048) invalid("invalid wall profile inventory");
        std::set<std::string,std::less<>> targets;
        for (const auto& edit:edits) {
            auto profile=decode_wall_profile_edit_intent(edit);
            if (profile.elevation) invalid("historical wall profile leaf cannot admit elevation authority");
            if (!targets.insert(profile.wall_id).second ||
                !std::binary_search(result.seed_wall_ids.begin(),result.seed_wall_ids.end(),profile.wall_id))
                invalid("profile target must be a unique original wall seed");
            result.wall_profiles.push_back(std::move(profile));
        }
    }
    if (value.at("version") == 3) {
        const auto& edits = value.at("opening_profiles");
        if (!edits.is_array() || edits.empty() || edits.size() > 2048) invalid("invalid opening profile inventory");
        std::set<std::string, std::less<>> targets;
        for (const auto& edit : edits) {
            auto profile = decode_hosted_opening_profile_edit_intent(edit);
            if (!targets.insert(profile.opening_id).second ||
                !std::binary_search(result.seed_wall_ids.begin(), result.seed_wall_ids.end(), profile.wall_id))
                invalid("opening profile must name a unique original opening and a seeded host");
            result.opening_profiles.push_back(std::move(profile));
        }
    }
    if (value.at("version") == 4) {
        const auto& edits=value.at("opening_rehosts");
        if (!edits.is_array() || edits.empty() || edits.size()>2048) invalid("invalid opening rehost inventory");
        std::set<std::string,std::less<>> targets;
        for (const auto& edit:edits) {
            auto rehost=decode_hosted_opening_rehost_intent(edit);
            if (!targets.insert(rehost.opening_id).second ||
                (!std::binary_search(result.seed_wall_ids.begin(),result.seed_wall_ids.end(),rehost.original_wall_id) &&
                 !std::binary_search(result.seed_wall_ids.begin(),result.seed_wall_ids.end(),rehost.target_wall_id)))
                invalid("opening rehost must name a unique original opening and its own seeded old or target host");
            result.opening_rehosts.push_back(std::move(rehost));
        }
    }
    if (value.at("version") == 5) {
        const auto& edits = value.at("opening_families");
        if (!edits.is_array() || edits.empty() || edits.size() > 2048) invalid("invalid opening family inventory");
        std::set<std::string, std::less<>> targets;
        for (const auto& edit : edits) {
            auto family = decode_hosted_opening_family_edit_intent(edit);
            if (!targets.insert(family.opening_id).second ||
                !std::binary_search(result.seed_wall_ids.begin(), result.seed_wall_ids.end(), family.wall_id))
                invalid("opening family must name a unique original opening and its own seeded host");
            result.opening_families.push_back(std::move(family));
        }
    }
    if (value.at("version") == 6) {
        const auto& edits=value.at("wall_stacks");
        if (!edits.is_array() || edits.empty() || edits.size()>2048) invalid("invalid wall stack inventory");
        std::set<std::string,std::less<>> targets;
        for (const auto& edit:edits) {
            auto stack=decode_wall_layer_stack_edit_intent(edit);
            if (!targets.insert(stack.wall_id).second) invalid("duplicate wall stack target");
            result.wall_stacks.push_back(std::move(stack));
        }
        if (std::vector<std::string>(targets.begin(),targets.end())!=result.seed_wall_ids)
            invalid("wall stack seeds must exactly match the authored original targets");
    }
    const auto& identities=value.at("identities");
    if (!identities.is_object() || identities.empty() || identities.size()>4096) invalid("invalid replacement identity inventory");
    std::set<std::string,std::less<>> destinations;
    for (const auto& [old_id,new_id]:identities.items()) {
        const auto original=allow_presentation_source_ids ? presentation_source_identity(old_id) : identity(Json(old_id));
        const auto fresh=identity(new_id);
        if (original==fresh || !destinations.insert(fresh).second) invalid("replacement identities must be fresh and injective");
        result.identities.emplace(original,fresh);
    }
    result.room_review_intent=value.at("room_review_intent");
    if (!result.room_review_intent.is_null()) {
        const auto room=decode_physical_wall_phase_room_review_intent(result.room_review_intent);
        if (encode_physical_wall_phase_room_review_intent(room).dump()!=result.room_review_intent.dump()) invalid("room review is not canonical");
    }
    const auto& rows=value.at("room_constraint_decisions");
    if (!rows.is_array() || rows.size()>4096) invalid("invalid room relationship inventory");
    std::set<std::string,std::less<>> constraints;
    for (const auto& row:rows) {
        keys(row,{"constraint_id","disposition","endpoints"});
        PhaseWallRoomConstraintDecision choice;choice.constraint_id=identity(row.at("constraint_id"));
        if (!constraints.insert(choice.constraint_id).second) invalid("duplicate room relationship decision");
        if (row.at("disposition")=="keep") choice.disposition=PhaseWallRoomConstraintDisposition::keep;
        else if (row.at("disposition")=="remap") choice.disposition=PhaseWallRoomConstraintDisposition::remap;
        else if (row.at("disposition")=="omit") choice.disposition=PhaseWallRoomConstraintDisposition::omit;
        else invalid("unknown room relationship disposition");
        const auto& endpoints=row.at("endpoints");
        if (!endpoints.is_array() || endpoints.size()>4096) invalid("invalid room endpoint inventory");
        std::set<std::size_t> bindings;
        for (const auto& endpoint:endpoints) {
            keys(endpoint,{"binding_index","owner_id","segment_id","vertex_id","role"});
            if (!endpoint.at("binding_index").is_number_unsigned()) invalid("binding index must be unsigned");
            PhaseWallRoomConstraintEndpoint target;target.binding_index=endpoint.at("binding_index").get<std::size_t>();
            if (!bindings.insert(target.binding_index).second) invalid("duplicate room endpoint mapping");
            target.target.owner_id=identity(endpoint.at("owner_id"));target.target.segment_id=identity(endpoint.at("segment_id"));
            target.target.vertex_id=identity(endpoint.at("vertex_id"));
            if (endpoint.at("role")=="start") target.target.role=WallEndpointRole::start;
            else if (endpoint.at("role")=="end") target.target.role=WallEndpointRole::end;
            else invalid("invalid mapped endpoint role");
            choice.endpoints.push_back(std::move(target));
        }
        if (choice.disposition!=PhaseWallRoomConstraintDisposition::remap && !choice.endpoints.empty()) invalid("only remapping carries endpoint targets");
        result.room_constraint_decisions.push_back(std::move(choice));
    }
    if (encode_phase_wall_replacement_authoring(result).dump()!=value.dump()) invalid("replacement decisions are not canonical");
    return result;
}
} // namespace
PhaseWallReplacementAuthoringPreview inspect_phase_wall_replacement_authoring(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent) {
    if (!source.is_editable() || intent.expected_revision!=source.revision() ||
        intent.source_snapshot_digest!=document_snapshot_digest(source) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
        intent.source_saved_revision!=source.saved_revision_optional()) invalid("original captured document changed");
    const auto edit=decode_phase_wall_replacement_authoring(intent.wall_replacement);
    auto stage=physical_stage(source.entities(),intent,edit);
    auto inventory=inspect_physical_wall_phase_room_review_entities(stage.entities,room_binding(intent,edit,stage.entities));
    const bool needed=requires_rooms(stage,inventory);
    return {std::move(stage.replacement),std::move(stage.entities),std::move(inventory),needed};
}
Entities replay_phase_wall_replacement_authoring(const Entities& source,const PhaseConstraintAuthoringIntent& intent) {
    const auto edit=decode_phase_wall_replacement_authoring(intent.wall_replacement);
    auto stage=physical_stage(source,intent,edit);
    const auto inventory=inspect_physical_wall_phase_room_review_entities(stage.entities,room_binding(intent,edit,stage.entities));
    if (edit.room_review_intent.is_null()) {
        if (requires_rooms(stage,inventory)) invalid("complete the proposed room and relationship review before saving this wall edit");
    } else {
        const auto room=decode_physical_wall_phase_room_review_intent(edit.room_review_intent);
        validate_room_binding(intent,edit,stage.entities,room);
        const auto replay=replay_physical_wall_phase_room_review_with_deferred_constraints(stage.entities,edit.room_review_intent,stage.deferred);
        stage.entities=replay.entities;
    }
    complete_room_constraints(stage,edit);
    complete_surviving_presentations(stage,source,edit);
    if (edit.complete_corner_windows || !edit.opening_families.empty() || !edit.wall_stacks.empty() || std::any_of(edit.wall_profiles.begin(),edit.wall_profiles.end(),
            strict_profile_admission)) {
        std::set<std::string,std::less<>> copied_walls;
        for (const auto& [original,copy]:stage.replacement.original_to_proposed) {
            const auto found=source.find(original);
            if (found!=source.end() && found->second.type=="wall") copied_walls.insert(copy);
        }
        if (intent.intent.wall_group_scale) {
            const auto scope=constraint_phase_scope(stage.entities);
            for (const auto& [id,entity]:stage.entities) {
                const auto original=source.find(id);
                if (entity.type=="wall" && !scope.inactive_owner_ids.contains(id) &&
                    original!=source.end() && entity!=original->second) copied_walls.insert(id);
            }
        }
        validate_active_wall_physical_dependencies(stage.entities,copied_walls,true,edit.complete_vertical_profiles);
    }
    preserve_baseline(source,stage.entities,stage.plan);
    if (const auto error=validate_active_phase_constraint_integrity(stage.entities)) invalid(*error);
    return std::move(stage.entities);
}
void validate_phase_wall_replacement_originals(const Entities& source,const Entities& candidate,
    const PhaseConstraintAuthoringIntent& intent) {
    const auto edit=decode_phase_wall_replacement_authoring(intent.wall_replacement);
    const auto plan=inspect_phase_wall_replacement_plan(source,edit.seed_wall_ids,edit.registry_id,
        edit.alternative_id,edit.complete_presentations,edit.complete_corner_windows,edit.complete_hosted_instances);
    preserve_baseline(source,candidate,plan);
}
namespace {
Stage composed_room_stage(const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent,
    const Entities& analytical_entities) {
    if (!source.is_editable() || intent.expected_revision!=source.revision() ||
        intent.source_snapshot_digest!=document_snapshot_digest(source) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
        intent.source_saved_revision!=source.saved_revision_optional()) invalid("original captured document changed");
    const auto edit=decode_phase_wall_replacement_authoring(intent.wall_replacement);
    if (!edit.complete_scalar_profiles) invalid("composed room completion requires explicit scalar wall authority");
    auto stage=physical_stage(source.entities(),intent,edit);
    // A complete enclosing producer may also update catalogs, presentation and
    // foreign registries. It may not substitute different physical copies or
    // relationship bodies for this leaf's independently derived closure.
    for (const auto& [original,copy]:stage.replacement.original_to_proposed) {
        const auto before=source.entities().find(original);
        if (before==source.entities().end() || before->second.type=="assembly_model") continue;
        const auto expected=stage.entities.find(copy);
        if (expected==stage.entities.end()) continue; // Reserved owned-child slot.
        const auto actual=analytical_entities.find(copy);
        if (actual==analytical_entities.end() || !exact(expected->second,actual->second))
            invalid("composed stage changed an independently derived wall closure: "+copy);
    }
    for (const auto& id:stage.deferred) {
        const auto expected=stage.entities.find(id),actual=analytical_entities.find(id);
        if (expected==stage.entities.end() || actual==analytical_entities.end() || !exact(expected->second,actual->second))
            invalid("composed stage changed a deferred room relationship: "+id);
    }
    stage.entities=analytical_entities;
    return stage;
}
}
PhaseWallReplacementAuthoringPreview inspect_phase_wall_replacement_rooms_on_composed_stage(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent,const Entities& analytical_entities) {
    const auto edit=decode_phase_wall_replacement_authoring(intent.wall_replacement);
    auto stage=composed_room_stage(source,intent,analytical_entities);
    auto inventory=inspect_physical_wall_phase_room_review_entities(stage.entities,room_binding(intent,edit,stage.entities));
    const bool needed=requires_rooms(stage,inventory);
    return {std::move(stage.replacement),std::move(stage.entities),std::move(inventory),needed};
}
Entities complete_phase_wall_replacement_rooms_on_composed_stage(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent,const Entities& analytical_entities,
    const std::set<std::string,std::less<>>& deferred_constraint_ids) {
    const auto edit=decode_phase_wall_replacement_authoring(intent.wall_replacement);
    auto stage=composed_room_stage(source,intent,analytical_entities);
    const auto inventory=inspect_physical_wall_phase_room_review_entities(stage.entities,room_binding(intent,edit,stage.entities));
    if (edit.room_review_intent.is_null()) {
        if (requires_rooms(stage,inventory)) invalid("complete every proposed room and relationship decision");
    } else {
        const auto room=decode_physical_wall_phase_room_review_intent(edit.room_review_intent);
        validate_room_binding(intent,edit,stage.entities,room);
        auto deferred=stage.deferred;
        deferred.insert(deferred_constraint_ids.begin(),deferred_constraint_ids.end());
        stage.entities=replay_physical_wall_phase_room_review_with_deferred_constraints(
            stage.entities,edit.room_review_intent,deferred).entities;
    }
    complete_room_constraints(stage,edit);
    complete_surviving_presentations(stage,source.entities(),edit);
    // Other independently derived leaves can still await their review. Final
    // composed physical/constraint admission belongs to the enclosing command.
    return std::move(stage.entities);
}
ApplyBoundaryConstraintChanges phase_wall_replacement_authoring_command(const PhaseConstraintAuthoringIntent& intent) {
    if (intent.wall_replacement.is_null()) invalid("replacement command needs its typed identity decisions");
    ApplyBoundaryConstraintChanges command;command.expected_revision=intent.expected_revision;
    command.message=intent.intent.message;command.phase_constraint_authoring_completion=true;
    command.phase_constraint_authoring_intent=encode_phase_constraint_authoring_intent(intent);
    return command;
}
} // namespace sketch
