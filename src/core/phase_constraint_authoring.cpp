#include "sketch/phase_constraint_authoring.hpp"

#include "sketch/boundary_transform.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/joint_translation_replay.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_coordinated_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_stair_demolition_retirement.hpp"
#include "sketch/phase_stair_replacement.hpp"
#include "sketch/phase_structural_replacement.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/phase_wall_demolition_authoring.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/stair_property_review.hpp"
#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"
#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string,Entity,std::less<>>;
constexpr std::size_t proof_budget = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void resource_shape(const Json& root) {
    std::set<std::string,std::less<>> owners;
    std::size_t nodes=0;
    const auto walk=[&](const auto& self,const Json& value,std::size_t depth) -> void {
        if (depth>64 || ++nodes>4*1024*1024) invalid("Phase constraint proof node/nesting budget exceeded");
        if (value.is_array()) {
            if (value.size()>4096) invalid("Phase constraint proof collection budget exceeded");
            for (const auto& row : value) self(self,row,depth+1);
        } else if (value.is_object()) {
            for (const auto& [key,child] : value.items()) {
                if ((key=="owner_id" || key=="boundary_id" || key=="wall_id" || key=="stroke_id" || key=="reference_id") && child.is_string())
                    owners.insert(child.get<std::string>());
                if ((key=="rigid_boundary_ids" || key=="rigid_stroke_ids" || key=="partial_wall_ids" || key=="dimension_ids" ||
                     key=="entity_ids" || key=="wall_ids") && child.is_array())
                    for (const auto& owner : child) if (owner.is_string()) owners.insert(owner.get<std::string>());
                if (owners.size()>4096) invalid("Phase constraint proof owner budget exceeded");
                self(self,child,depth+1);
            }
        } else if (value.is_number_float() && !std::isfinite(value.get<double>()))
            invalid("Phase constraint proof number must be finite");
    };
    walk(walk,root,0);
}
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size()!=expected.size()) invalid("Phase constraint proof fields are invalid");
    for (const auto* key : expected) if (!value.contains(key)) invalid("Phase constraint proof field is missing");
}
std::string id(const Json& value) {
    if (!value.is_string()) invalid("Phase constraint identity must be a string");
    const auto result=value.get<std::string>();
    if (result.empty() || result.size()>128 || !std::all_of(result.begin(),result.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
    })) invalid("Phase constraint identity is invalid");
    return result;
}
bool flag(const Json& value) {
    if (!value.is_boolean()) invalid("Phase constraint movement flag must be boolean");
    return value.get<bool>();
}
Vec2 point(const Json& value) {
    if (!value.is_array() || value.size()!=2 || !value[0].is_number() || !value[1].is_number())
        invalid("Phase constraint point is invalid");
    Vec2 result{value[0].get<double>(),value[1].get<double>()};
    if (!std::isfinite(result.x) || !std::isfinite(result.y)) invalid("Phase constraint point must be finite");
    return result;
}
Json point(Vec2 value) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y)) invalid("Phase constraint point must be finite");
    return Json::array({value.x,value.y});
}
WallEndpointRole role(const Json& value) {
    if (value=="start") return WallEndpointRole::start;
    if (value=="end") return WallEndpointRole::end;
    invalid("Phase constraint endpoint role is invalid");
}
Json binding(const WallEndpointBinding& value) {
    (void)id(value.owner_id);
    if (value.role!=WallEndpointRole::start && value.role!=WallEndpointRole::end) invalid("Phase constraint endpoint role is invalid");
    if (!value.segment_id.empty()) (void)id(value.segment_id);
    if (!value.vertex_id.empty()) (void)id(value.vertex_id);
    return {{"owner_id",value.owner_id},{"role",wall_endpoint_role_name(value.role)},
        {"segment_id",value.segment_id},{"vertex_id",value.vertex_id}};
}
WallEndpointBinding binding(const Json& value) {
    keys(value,{"owner_id","role","segment_id","vertex_id"});
    WallEndpointBinding result{id(value.at("owner_id")),role(value.at("role")),
        value.at("segment_id").get<std::string>(),value.at("vertex_id").get<std::string>()};
    (void)binding(result);
    return result;
}
Json transform(const std::string& owner, const PlanarTransform& value) {
    return encode_boundary_transform({owner,value});
}
PlanarTransform transform(const std::string& owner, const Json& value) {
    const auto decoded=decode_boundary_transform(value);
    if (decoded.boundary_id!=owner) invalid("Phase constraint transform has a different owner");
    return decoded.transform;
}
void bounded_array(const Json& value) {
    if (!value.is_array() || value.size()>4096) invalid("Phase constraint target budget exceeded");
}
bool has_rotated_roof_edits(const Json& value) {
    return value.is_array() && std::any_of(value.begin(), value.end(), [](const auto& row) {
        return row.is_object() && row.contains("version") && row.at("version") == 9;
    });
}
std::vector<RoofEditIntent> ordinary_roof_edits(const Json& value, bool allow_rotation = false) {
    bounded_array(value);
    if (value.empty()) invalid("Ordinary roof authoring requires a nonempty typed edit list");
    std::vector<RoofEditIntent> result;
    std::set<std::string,std::less<>> targets;
    for (const auto& row:value) {
        if (!allow_rotation && row.is_object() && row.contains("version") && row.at("version") == 9)
            invalid("Historical ordinary roof authoring cannot borrow rotation authority");
        auto edit=decode_roof_edit_intent(row);
        if (encode_roof_edit_intent(edit).dump()!=row.dump() || !targets.insert(edit.roof_id).second)
            invalid("Ordinary roof authoring requires unique canonical typed edits");
        result.push_back(std::move(edit));
    }
    return result;
}
Json joint(const JointTranslationIntent& intent) {
    return encode_joint_translation_intent(intent);
}
JointTranslationIntent joint(const Json& value) {
    return decode_joint_translation_intent(value);
}

Json encode_intent(const ConstraintAuthoringIntent& value) {
    Json result={{"wall_resize",nullptr},{"wall_geometry_move",nullptr},{"wall_curve_construction",nullptr},
        {"boundary_resize",nullptr},{"boundary_vertex_move",nullptr},{"exterior_corner_move",nullptr},
        {"exterior_segment_resize",nullptr},{"exterior_segment_arc",nullptr},{"measured_stroke_resize",nullptr},
        {"measured_stroke_vertex_move",nullptr},{"measured_stroke_transform",nullptr},{"joint_translation",nullptr},
        {"relation_mutations",Json::array()},{"relation_anchor",value.relation_anchor ? binding(*value.relation_anchor) : Json(nullptr)},
        {"relation_move_connected_walls",value.relation_move_connected_walls},{"message",value.message}};
    if (value.wall_resize) {
        const auto& resize=*value.wall_resize;
        if (resize.anchored_endpoint!=WallResizeAnchor::start && resize.anchored_endpoint!=WallResizeAnchor::end)
            invalid("Phase constraint resize anchor is invalid");
        result["wall_resize"]={{"wall_id",id(resize.wall_id)},
            {"exact_length",encode_constraint_quantity_receipt(resize.exact_length)},
            {"anchored_endpoint",resize.anchored_endpoint==WallResizeAnchor::start ? "start" : "end"},
            {"move_connected_walls",resize.move_connected_walls},
            {"proposed_endpoint",resize.proposed_endpoint ? point(*resize.proposed_endpoint) : Json(nullptr)}};
    }
    if (value.wall_geometry_move) {
        const auto& move=*value.wall_geometry_move;
        Json targets=Json::array();
        for (const auto& target : move.targets) targets.push_back({{"wall_id",id(target.wall_id)},
            {"proposed_start",point(target.proposed_start)},{"proposed_end",point(target.proposed_end)},
            {"rigid_transform",target.rigid_transform ? transform(target.wall_id,*target.rigid_transform) : Json(nullptr)}});
        bounded_array(targets);
        result["wall_geometry_move"]={{"targets",targets},{"move_connected_walls",move.move_connected_walls},
            {"complete_saved_dimensions",move.complete_saved_dimensions}};
    }
    if (value.wall_curve_construction) result["wall_curve_construction"]={
        {"edit",encode_constraint_wall_edit(value.wall_curve_construction->edit)},
        {"move_connected_walls",value.wall_curve_construction->move_connected_walls}};
    if (value.boundary_resize) result["boundary_resize"]={
        {"edit",encode_boundary_geometry_edit(value.boundary_resize->edit)},
        {"move_related_objects",value.boundary_resize->move_related_objects},
        {"exact_length",value.boundary_resize->exact_length ? encode_constraint_quantity_receipt(*value.boundary_resize->exact_length) : Json(nullptr)}};
    if (value.boundary_vertex_move) result["boundary_vertex_move"]={
        {"edit",encode_boundary_geometry_edit(value.boundary_vertex_move->edit)},
        {"move_related_objects",value.boundary_vertex_move->move_related_objects}};
    if (value.exterior_corner_move) result["exterior_corner_move"]=encode_exterior_corner_move(*value.exterior_corner_move);
    if (value.exterior_segment_resize) result["exterior_segment_resize"]=encode_exterior_segment_resize(*value.exterior_segment_resize);
    if (value.exterior_segment_arc) result["exterior_segment_arc"]=encode_exterior_segment_arc(*value.exterior_segment_arc);
    if (value.measured_stroke_resize) result["measured_stroke_resize"]={
        {"edit",encode_boundary_geometry_edit(value.measured_stroke_resize->edit)},
        {"exact_length",encode_constraint_quantity_receipt(value.measured_stroke_resize->exact_length)},
        {"move_related_objects",value.measured_stroke_resize->move_related_objects}};
    if (value.measured_stroke_vertex_move) result["measured_stroke_vertex_move"]={
        {"edit",encode_boundary_geometry_edit(value.measured_stroke_vertex_move->edit)},
        {"move_related_objects",value.measured_stroke_vertex_move->move_related_objects}};
    if (value.measured_stroke_transform) {
        Json targets=Json::array();
        for (const auto& target : value.measured_stroke_transform->targets)
            targets.push_back(transform(id(target.stroke_id),target.transform));
        bounded_array(targets);
        result["measured_stroke_transform"]={{"targets",targets},{"move_related_objects",value.measured_stroke_transform->move_related_objects}};
    }
    if (value.joint_translation) result["joint_translation"]=joint(*value.joint_translation);
    // Historical ordinary intents retain their exact sixteen-field shape.
    // Only the dedicated scale wrapper admits this additional semantic field.
    if (value.wall_group_scale)
        result["wall_group_scale"]=encode_wall_group_scale_intent(*value.wall_group_scale);
    for (const auto& mutation : value.relation_mutations) {
        if (mutation.kind==ConstraintRelationMutationKind::remove)
            result["relation_mutations"].push_back({{"kind","remove"},{"constraint_id",id(mutation.constraint_id)}});
        else if (mutation.kind==ConstraintRelationMutationKind::upsert) {
            const auto entity=encode_constraint_entity(mutation.constraint);
            result["relation_mutations"].push_back({{"kind","upsert"},{"constraint_id",id(entity.id)},{"relation",entity.properties}});
        } else invalid("Phase constraint mutation kind is invalid");
    }
    bounded_array(result.at("relation_mutations"));
    if (value.message.size()>4096 || value.message.find('\0')!=std::string::npos) invalid("Phase constraint message budget exceeded");
    return result;
}

ConstraintAuthoringIntent decode_intent(const Json& value,bool wall_group_scale=false) {
    if (wall_group_scale) keys(value,{"wall_resize","wall_geometry_move","wall_curve_construction","boundary_resize","boundary_vertex_move",
        "exterior_corner_move","exterior_segment_resize","exterior_segment_arc","measured_stroke_resize",
        "measured_stroke_vertex_move","measured_stroke_transform","joint_translation","relation_mutations",
        "relation_anchor","relation_move_connected_walls","message","wall_group_scale"});
    else keys(value,{"wall_resize","wall_geometry_move","wall_curve_construction","boundary_resize","boundary_vertex_move",
        "exterior_corner_move","exterior_segment_resize","exterior_segment_arc","measured_stroke_resize",
        "measured_stroke_vertex_move","measured_stroke_transform","joint_translation","relation_mutations",
        "relation_anchor","relation_move_connected_walls","message"});
    ConstraintAuthoringIntent result;
    if (wall_group_scale) {
        if (value.at("wall_group_scale").is_null())
            invalid("Wall group scale dialect requires wall group scale intent");
        result.wall_group_scale=decode_wall_group_scale_intent(value.at("wall_group_scale"));
    }
    result.message=value.at("message").get<std::string>();
    result.relation_move_connected_walls=flag(value.at("relation_move_connected_walls"));
    if (!value.at("relation_anchor").is_null()) result.relation_anchor=binding(value.at("relation_anchor"));
    if (const auto& v=value.at("wall_resize"); !v.is_null()) {
        keys(v,{"wall_id","exact_length","anchored_endpoint","move_connected_walls","proposed_endpoint"});
        result.wall_resize=WallResizeIntent{id(v.at("wall_id")),decode_constraint_quantity_receipt(v.at("exact_length")),
            role(v.at("anchored_endpoint"))==WallEndpointRole::start ? WallResizeAnchor::start : WallResizeAnchor::end,
            flag(v.at("move_connected_walls")),v.at("proposed_endpoint").is_null() ? std::nullopt : std::optional<Vec2>{point(v.at("proposed_endpoint"))}};
    }
    if (const auto& v=value.at("wall_geometry_move"); !v.is_null()) {
        keys(v,{"targets","move_connected_walls","complete_saved_dimensions"}); bounded_array(v.at("targets"));
        WallGeometryMoveIntent move; move.move_connected_walls=flag(v.at("move_connected_walls"));
        move.complete_saved_dimensions=flag(v.at("complete_saved_dimensions"));
        for (const auto& target : v.at("targets")) {
            keys(target,{"wall_id","proposed_start","proposed_end","rigid_transform"});
            const auto owner=id(target.at("wall_id"));
            move.targets.push_back({owner,point(target.at("proposed_start")),point(target.at("proposed_end")),
                target.at("rigid_transform").is_null() ? std::nullopt : std::optional<PlanarTransform>{transform(owner,target.at("rigid_transform"))}});
        }
        result.wall_geometry_move=std::move(move);
    }
    if (const auto& v=value.at("wall_curve_construction"); !v.is_null()) {
        keys(v,{"edit","move_connected_walls"});
        result.wall_curve_construction=WallCurveConstructionIntent{decode_constraint_wall_edit(v.at("edit")),flag(v.at("move_connected_walls"))};
    }
    if (const auto& v=value.at("boundary_resize"); !v.is_null()) {
        keys(v,{"edit","move_related_objects","exact_length"});
        result.boundary_resize=BoundaryResizeIntent{decode_boundary_geometry_edit(v.at("edit")),flag(v.at("move_related_objects")),
            v.at("exact_length").is_null() ? std::nullopt : std::optional<Quantity>{decode_constraint_quantity_receipt(v.at("exact_length"))}};
    }
    if (const auto& v=value.at("boundary_vertex_move"); !v.is_null()) {
        keys(v,{"edit","move_related_objects"});
        result.boundary_vertex_move=BoundaryVertexMoveIntent{decode_boundary_geometry_edit(v.at("edit")),flag(v.at("move_related_objects"))};
    }
    if (!value.at("exterior_corner_move").is_null()) result.exterior_corner_move=decode_exterior_corner_move(value.at("exterior_corner_move"));
    if (!value.at("exterior_segment_resize").is_null()) result.exterior_segment_resize=decode_exterior_segment_resize(value.at("exterior_segment_resize"));
    if (!value.at("exterior_segment_arc").is_null()) result.exterior_segment_arc=decode_exterior_segment_arc(value.at("exterior_segment_arc"));
    if (const auto& v=value.at("measured_stroke_resize"); !v.is_null()) {
        keys(v,{"edit","exact_length","move_related_objects"});
        result.measured_stroke_resize=MeasuredStrokeResizeIntent{decode_boundary_geometry_edit(v.at("edit")),
            decode_constraint_quantity_receipt(v.at("exact_length")),flag(v.at("move_related_objects"))};
    }
    if (const auto& v=value.at("measured_stroke_vertex_move"); !v.is_null()) {
        keys(v,{"edit","move_related_objects"});
        result.measured_stroke_vertex_move=MeasuredStrokeVertexMoveIntent{decode_boundary_geometry_edit(v.at("edit")),flag(v.at("move_related_objects"))};
    }
    if (const auto& v=value.at("measured_stroke_transform"); !v.is_null()) {
        keys(v,{"targets","move_related_objects"}); bounded_array(v.at("targets"));
        MeasuredStrokeTransformIntent move; move.move_related_objects=flag(v.at("move_related_objects"));
        for (const auto& target : v.at("targets")) {
            const auto decoded=decode_boundary_transform(target); move.targets.push_back({decoded.boundary_id,decoded.transform});
        }
        result.measured_stroke_transform=std::move(move);
    }
    if (!value.at("joint_translation").is_null()) result.joint_translation=joint(value.at("joint_translation"));
    bounded_array(value.at("relation_mutations"));
    for (const auto& mutation : value.at("relation_mutations")) {
        if (mutation.at("kind")=="remove") {
            keys(mutation,{"kind","constraint_id"});
            result.relation_mutations.push_back(ConstraintRelationMutation::remove(id(mutation.at("constraint_id"))));
        } else if (mutation.at("kind")=="upsert") {
            keys(mutation,{"kind","constraint_id","relation"});
            Entity entity{id(mutation.at("constraint_id")),"constraint",mutation.at("relation")};
            const auto decoded=decode_constraint_entity(entity);
            if (!decoded.supported()) invalid("Unsupported phase constraint relation is read-only");
            if (encode_constraint_entity(*decoded.constraint).properties!=entity.properties)
                invalid("Phase constraint relation contains nonsemantic payload fields");
            result.relation_mutations.push_back(ConstraintRelationMutation::upsert(*decoded.constraint));
        } else invalid("Phase constraint mutation kind is invalid");
    }
    if (encode_intent(result)!=value) invalid("Phase constraint intent is not canonical");
    return result;
}

void digest(const std::string& value) {
    if (value.size()!=64 || !std::all_of(value.begin(),value.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    })) invalid("Phase constraint source digest is invalid");
}
void selections(const Json& value) {
    bounded_array(value); std::set<std::string,std::less<>> registries;
    std::string previous;
    for (const auto& row : value) {
        keys(row,{"registry_id","alternative_id"});
        const auto registry=id(row.at("registry_id"));
        if (!registries.insert(registry).second || (!previous.empty() && registry<=previous))
            invalid("Phase constraint saved selections must be unique and sorted");
        previous=registry;
        if (!row.at("alternative_id").is_null()) (void)id(row.at("alternative_id"));
    }
}

struct CoordinatedReplacements {
    int version{1};
    std::optional<PhaseConstraintAuthoringIntent> wall;
    std::optional<PhaseRoofReplacementAuthoring> roof;
    std::optional<PhaseSlabReplacementAuthoring> slab;
    std::optional<PhaseStructuralReplacementAuthoring> structural;
    std::optional<PhaseStairReplacementAuthoring> stair;
    std::vector<RoofEditIntent> ordinary_roofs;
    std::vector<SlabGeometryEditIntent> ordinary_slabs;
    std::vector<StructuralObjectEditIntent> ordinary_structural;
    std::vector<StairTransformIntent> ordinary_stairs;
};
CoordinatedReplacements coordinated(const Json& value,const PhaseConstraintAuthoringIntent& enclosing) {
    resource_shape(value);
    if (value.dump().size()>proof_budget) invalid("Coordinated replacement proof byte budget exceeded");
    const bool hosted_walls=value.is_object() && value.contains("version") && value.at("version")==5;
    const bool stairs=hosted_walls || (value.is_object() && value.contains("version") && value.at("version")==4);
    const bool structural=stairs || (value.is_object() && value.contains("version") && value.at("version")==3);
    const bool walls=structural || (value.is_object() && value.contains("version") && value.at("version")==2);
    if (stairs) keys(value,{"version","wall_authoring","roof_replacement","slab_replacement","structural_replacement",
        "stair_replacement","ordinary_roof_edits","ordinary_slab_geometry","ordinary_structural_edits","ordinary_stair_transforms"});
    else if (structural) keys(value,{"version","wall_authoring","roof_replacement","slab_replacement","structural_replacement",
        "ordinary_roof_edits","ordinary_slab_geometry","ordinary_structural_edits"});
    else if (walls) keys(value,{"version","wall_authoring","roof_replacement","slab_replacement","ordinary_roof_edits","ordinary_slab_geometry"});
    else keys(value,{"version","roof_replacement","slab_replacement","ordinary_roof_edits","ordinary_slab_geometry"});
    if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5))
        invalid("Coordinated replacements require a supported version");
    bounded_array(value.at("ordinary_roof_edits"));
    bounded_array(value.at("ordinary_slab_geometry"));
    CoordinatedReplacements result;
    result.version=hosted_walls?5:stairs?4:structural?3:walls?2:1;
    if (hosted_walls && value.at("wall_authoring").is_null())
        invalid("Coordinated qualified wall authoring requires an actual hosted wall replacement");
    if (walls && !value.at("wall_authoring").is_null()) {
        const auto& child=value.at("wall_authoring");
        // Refuse nested coordination before decoding. The additive dialect
        // requires a qualified copy while retaining only historical move authority.
        if (!child.is_object() || !child.contains("version") ||
            (hosted_walls ? (child.at("version")!=28 || !child.contains("wall_edit_version") ||
                child.at("wall_edit_version")!=2) : (child.at("version")!=1 && child.at("version")!=2)))
            invalid("Coordinated wall child requires its exact historical or qualified move dialect");
        result.wall=decode_phase_constraint_authoring_intent(child);
        if (encode_phase_constraint_authoring_intent(*result.wall).dump()!=child.dump())
            invalid("Coordinated wall authoring is not canonical");
        ConstraintAuthoringIntent only_move;
        only_move.message=result.wall->intent.message;
        only_move.wall_geometry_move=result.wall->intent.wall_geometry_move;
        if (!only_move.wall_geometry_move || only_move.wall_geometry_move->targets.empty() ||
            encode_intent(only_move)!=encode_intent(result.wall->intent))
            invalid("Coordinated wall authoring requires only actual typed wall geometry movement");
        if (!result.wall->wall_replacement.is_null()) {
            const auto leaf=decode_phase_wall_replacement_authoring(result.wall->wall_replacement);
            if (hosted_walls && (result.wall->wall_replacement.at("version")!=11 || !leaf.complete_hosted_instances))
                invalid("Coordinated qualified wall replacement requires explicit complete hosted authority");
            if (!leaf.wall_profiles.empty() || !leaf.opening_profiles.empty() || !leaf.opening_rehosts.empty() ||
                !leaf.opening_families.empty() || !leaf.wall_stacks.empty() || !leaf.corner_profiles.empty())
                invalid("Coordinated wall replacement cannot borrow another wall edit family");
        }
        if (result.wall->expected_revision!=enclosing.expected_revision ||
            result.wall->source_snapshot_digest!=enclosing.source_snapshot_digest ||
            result.wall->source_authoring_digest!=enclosing.source_authoring_digest ||
            result.wall->source_entities_digest!=enclosing.source_entities_digest ||
            result.wall->source_saved_revision!=enclosing.source_saved_revision ||
            result.wall->phase_selections.dump()!=enclosing.phase_selections.dump())
            invalid("Coordinated wall child differs from the enclosing actual source binding");
    }
    if (!value.at("roof_replacement").is_null()) {
        const auto& leaf=value.at("roof_replacement");
        if (leaf.is_object() && leaf.contains("version") && leaf.at("version") == 10)
            invalid("Historical coordinated authoring cannot borrow roof rotation replacement authority");
        result.roof=decode_phase_roof_replacement_authoring(leaf);
        if (result.roof->demolition || encode_phase_roof_replacement_authoring(*result.roof).dump()!=leaf.dump())
            invalid("Coordinated roof replacement requires canonical replacement authority without demolition");
    }
    if (!value.at("slab_replacement").is_null()) {
        const auto& leaf=value.at("slab_replacement");
        result.slab=decode_phase_slab_replacement_authoring(leaf);
        if (result.slab->coordinate_hosted_profiles)
            invalid("Coordinated replacement dialects cannot borrow slab elevation carry authority");
        if (result.slab->slab_geometry.empty() || !result.slab->slab_profiles.empty() || !result.slab->slab_stacks.empty() ||
            encode_phase_slab_replacement_authoring(*result.slab).dump()!=leaf.dump())
            invalid("Coordinated horizontal replacement requires canonical geometry authority");
    }
    const auto structural_transform_only=[](const StructuralObjectEditIntent& edit) {
        if (!edit.transform || !edit.profile_fields.is_null() || !edit.quantity_entries.is_null())
            invalid("Coordinated structural authoring requires only typed transforms");
    };
    if (structural && !value.at("structural_replacement").is_null()) {
        const auto& leaf=value.at("structural_replacement");
        result.structural=decode_phase_structural_replacement_authoring(leaf);
        if (result.structural->demolition || result.structural->edits.empty() ||
            encode_phase_structural_replacement_authoring(*result.structural).dump()!=leaf.dump())
            invalid("Coordinated structural replacement requires canonical non-demolition authority");
        for (const auto& edit:result.structural->edits) structural_transform_only(edit);
    }
    if (stairs && !value.at("stair_replacement").is_null()) {
        const auto& leaf=value.at("stair_replacement");
        result.stair=decode_phase_stair_replacement_authoring(leaf);
        if (result.stair->complete_scalar_dependencies || encode_phase_stair_replacement_authoring(*result.stair).dump()!=leaf.dump())
            invalid("Coordinated stair replacement requires canonical typed authority");
    }
    std::set<std::string,std::less<>> roof_targets, slab_targets;
    for (const auto& row:value.at("ordinary_roof_edits")) {
        if (row.is_object() && row.contains("version") && row.at("version") == 9)
            invalid("Historical coordinated authoring cannot borrow roof rotation authority");
        auto edit=decode_roof_edit_intent(row);
        if (encode_roof_edit_intent(edit).dump()!=row.dump() || !roof_targets.insert(edit.roof_id).second)
            invalid("Coordinated ordinary roofs require unique canonical typed edits");
        result.ordinary_roofs.push_back(std::move(edit));
    }
    for (const auto& row:value.at("ordinary_slab_geometry")) {
        auto edit=decode_slab_geometry_edit_intent(row);
        if (encode_slab_geometry_edit_intent(edit).dump()!=row.dump() || !slab_targets.insert(edit.slab_id).second)
            invalid("Coordinated ordinary slabs require unique canonical typed geometry");
        result.ordinary_slabs.push_back(std::move(edit));
    }
    if (structural) {
        bounded_array(value.at("ordinary_structural_edits"));
        std::set<std::string,std::less<>> targets;
        for (const auto& row:value.at("ordinary_structural_edits")) {
            auto edit=decode_structural_object_edit_intent(row);
            structural_transform_only(edit);
            if (encode_structural_object_edit_intent(edit).dump()!=row.dump() || !targets.insert(edit.object_id).second)
                invalid("Coordinated ordinary structural edits require unique canonical typed transforms");
            result.ordinary_structural.push_back(std::move(edit));
        }
    }
    if (stairs) {
        bounded_array(value.at("ordinary_stair_transforms"));
        std::set<std::string,std::less<>> targets;
        for (const auto& row:value.at("ordinary_stair_transforms")) {
            auto edit=decode_stair_transform_intent(row);
            if (encode_stair_transform_intent(edit).dump()!=row.dump() || !targets.insert(edit.object_id).second)
                invalid("Coordinated ordinary stair transforms require unique canonical typed operators");
            result.ordinary_stairs.push_back(std::move(edit));
        }
    }
    const auto roof_lanes=static_cast<int>(result.roof.has_value())+static_cast<int>(!result.ordinary_roofs.empty());
    const auto slab_lanes=static_cast<int>(result.slab.has_value())+static_cast<int>(!result.ordinary_slabs.empty());
    const auto structural_lanes=static_cast<int>(result.structural.has_value())+static_cast<int>(!result.ordinary_structural.empty());
    const auto stair_lanes=static_cast<int>(result.stair.has_value())+static_cast<int>(!result.ordinary_stairs.empty());
    const bool wall_replacement=result.wall && !result.wall->wall_replacement.is_null();
    if ((!walls && (roof_lanes!=1 || slab_lanes!=1)) || roof_lanes>1 || slab_lanes>1 || structural_lanes>1 || stair_lanes>1 ||
        (walls && static_cast<int>(result.wall.has_value())+roof_lanes+slab_lanes+structural_lanes+stair_lanes<2) ||
        (!structural && !wall_replacement && !result.roof && !result.slab))
        invalid("Coordinated authoring requires one lane per present family and two families; historical versions also require a replacement");
    if (result.roof && result.slab && (result.roof->registry_id!=result.slab->registry_id ||
        result.roof->alternative_id!=result.slab->alternative_id))
        invalid("Coordinated replacement leaves must name the same actual registry and alternative");
    if (wall_replacement) {
        const auto leaf=decode_phase_wall_replacement_authoring(result.wall->wall_replacement);
        if ((result.roof && (leaf.registry_id!=result.roof->registry_id || leaf.alternative_id!=result.roof->alternative_id)) ||
            (result.slab && (leaf.registry_id!=result.slab->registry_id || leaf.alternative_id!=result.slab->alternative_id)) ||
            (result.structural && (leaf.registry_id!=result.structural->registry_id || leaf.alternative_id!=result.structural->alternative_id)))
            invalid("Coordinated wall replacement must name the same actual registry and alternative");
    }
    if (result.structural && ((result.roof && (result.structural->registry_id!=result.roof->registry_id ||
        result.structural->alternative_id!=result.roof->alternative_id)) ||
        (result.slab && (result.structural->registry_id!=result.slab->registry_id ||
        result.structural->alternative_id!=result.slab->alternative_id))))
        invalid("Coordinated structural replacement must name the same actual registry and alternative");
    if (result.stair) {
        const auto same_choice=[&](const auto& leaf) {
            return result.stair->registry_id==leaf.registry_id && result.stair->alternative_id==leaf.alternative_id;
        };
        if ((result.roof && !same_choice(*result.roof)) || (result.slab && !same_choice(*result.slab)) ||
            (result.structural && !same_choice(*result.structural)) ||
            (wall_replacement && !same_choice(decode_phase_wall_replacement_authoring(result.wall->wall_replacement))))
            invalid("Coordinated stair replacement must name the same actual registry and alternative");
    }
    return result;
}

bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
bool exact_json(const Json& a,const Json& b) { return a==b && a.dump()==b.dump(); }

// Explicit paths only. Removing these arrays from each independently replayed
// envelope must leave the exact source envelope. This is not JSON merge authority.
struct AppendPath { Json::json_pointer path; const char* identity_key; };
std::vector<AppendPath> append_paths(const Entity& source, bool ordinary_removal_rosters=false) {
    std::vector<AppendPath> result;
    if (source.type=="model_phases") {
        const auto model=ModelPhases::from_json(source.properties.at("model"));
        result.push_back({Json::json_pointer("/model/entity_ids"),nullptr});
        if (ordinary_removal_rosters)
            result.push_back({Json::json_pointer("/model/baseline_ids"),nullptr});
        if (!model.active_alternative()) {
            if (ordinary_removal_rosters) return result;
            invalid("Shared phase append requires an actual saved active alternative");
        }
        const auto& alternatives=source.properties.at("model").at("alternatives");
        for (std::size_t i=0;i<alternatives.size();++i) if (alternatives[i].at("id")==*model.active_alternative()) {
            const auto prefix="/model/alternatives/"+std::to_string(i);
            result.push_back({Json::json_pointer(prefix+"/proposed_ids"),nullptr});
            result.push_back({Json::json_pointer(prefix+"/demolished_ids"),nullptr});
        }
    } else if (source.type==kAnnotationEntityType) {
        validate_annotation_entity(source);
        result.push_back({Json::json_pointer("/state/overrides"),"target_id"});
    } else if (source.type==kSheetViewEntityType) {
        validate_sheet_view_entity(source);
        const auto& views=source.properties.at("model").at("views");
        for (std::size_t i=0;i<views.size();++i) {
            const auto prefix="/model/views/"+std::to_string(i);
            const auto& view=views[i];
            if (view.contains("object_ids")) result.push_back({Json::json_pointer(prefix+"/object_ids"),nullptr});
            const auto& presentation=view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                result.push_back({Json::json_pointer(prefix+"/presentation/appearance/objects"),"object_id"});
            if (view.contains("overlays")) result.push_back({Json::json_pointer(prefix+"/overlays"),"id"});
        }
    } else if (source.type=="assembly_model")
        invalid("Coordinated replacements have overlapping hosted catalog consequences; catalog rows cannot be merged");
    else invalid("Coordinated replacements have conflicting physical changes to the same actual entity");
    return result;
}
Entity merge_append_container(const Entity& source,const Entity& roof,const Entity& slab) {
    const auto paths=append_paths(source);
    auto roof_envelope=roof, slab_envelope=slab, merged=source;
    for (const auto& allowed:paths) {
        const auto& retained=source.properties.at(allowed.path);
        const auto& roof_rows=roof.properties.at(allowed.path);
        const auto& slab_rows=slab.properties.at(allowed.path);
        if (!retained.is_array() || !roof_rows.is_array() || !slab_rows.is_array() ||
            roof_rows.size()<retained.size() || slab_rows.size()<retained.size())
            invalid("Coordinated shared container must retain source array types and prefixes");
        for (std::size_t i=0;i<retained.size();++i)
            if (!exact_json(retained[i],roof_rows[i]) || !exact_json(retained[i],slab_rows[i]))
                invalid("Coordinated shared container changed a retained source row or its order");
        auto& rows=merged.properties.at(allowed.path);
        std::set<std::string,std::less<>> destinations;
        const auto destination=[&](const Json& row) {
            const auto& value=allowed.identity_key ? row.at(allowed.identity_key) : row;
            // The admitted source codecs own these reference alphabets. View
            // aliases and annotation targets need not use entity-ID spelling.
            if (!value.is_string()) invalid("Coordinated append destination must be an admitted string reference");
            return value.get<std::string>();
        };
        for (const auto& row:retained) destinations.insert(destination(row));
        for (const auto* additions:{&roof_rows,&slab_rows})
            for (std::size_t i=retained.size();i<additions->size();++i) {
                const auto& row=(*additions)[i];
                if (!destinations.insert(destination(row)).second)
                    invalid("Coordinated shared container append destinations overlap");
                rows.push_back(row);
            }
        roof_envelope.properties.at(allowed.path)=retained;
        slab_envelope.properties.at(allowed.path)=retained;
    }
    if (!exact(source,roof_envelope) || !exact(source,slab_envelope))
        invalid("Coordinated shared container changed fields outside its explicit append authority");
    if (source.type=="model_phases") (void)ModelPhases::from_json(merged.properties.at("model"));
    else if (source.type==kAnnotationEntityType) validate_annotation_entity(merged);
    else validate_sheet_view_entity(merged);
    return merged;
}

// Inner version two composes codec-known source rows, never arbitrary JSON
// fields. Each retained row can have only one family consequence (including
// removal); all other source rows keep their exact order and bytes. Fresh rows
// remain disjoint suffixes in wall/roof/horizontal order.
Entity merge_source_row_container(const Entity& source,const std::vector<const Entity*>& candidates,
    bool ordinary_removal_rosters=false,bool allow_shared_reference_retirement=false,
    bool coalesce_exact_consequences=false) {
    auto paths=append_paths(source,ordinary_removal_rosters);
    if (source.type==kAnnotationEntityType) {
        paths.push_back({Json::json_pointer("/state/labels"),"id"});
        paths.push_back({Json::json_pointer("/state/symbols"),"id"});
    }
    auto merged=source;
    std::vector<Entity> envelopes;
    for (const auto* candidate:candidates) {
        if (source.type==kAnnotationEntityType) validate_annotation_entity(*candidate);
        else if (source.type==kSheetViewEntityType) validate_sheet_view_entity(*candidate);
        else (void)ModelPhases::from_json(candidate->properties.at("model"));
        envelopes.push_back(*candidate);
    }
    for (const auto& allowed:paths) {
        const auto& retained=source.properties.at(allowed.path);
        if (!retained.is_array()) invalid("Coordinated source row collection must be an array");
        const auto destination=[&](const Json& row) -> std::pair<std::string,std::string> {
            const auto& token=allowed.identity_key ? row.at(allowed.identity_key) : row;
            if (!token.is_string()) invalid("Coordinated source row requires an admitted string identity");
            // Annotation override identity is its typed target pair. Existing
            // codecs permit separate name/calculation callouts on one owner.
            std::string kind;
            if (source.type==kAnnotationEntityType && allowed.identity_key && std::string_view(allowed.identity_key)=="target_id") {
                if (!row.at("target_kind").is_string()) invalid("Coordinated annotation target kind is invalid");
                kind=row.at("target_kind").get<std::string>();
            }
            return {token.get<std::string>(),kind};
        };
        using RowIdentity=std::pair<std::string,std::string>;
        std::map<RowIdentity,std::size_t> source_rows;
        for (std::size_t i=0;i<retained.size();++i)
            if (!source_rows.emplace(destination(retained[i]),i).second)
                invalid("Coordinated source row identity is ambiguous");
        std::vector<std::map<RowIdentity,const Json*>> lane_rows;
        std::vector<std::vector<const Json*>> additions;
        for (auto& envelope:envelopes) {
            const auto& rows=envelope.properties.at(allowed.path);
            if (!rows.is_array()) invalid("Coordinated candidate row collection must retain its source type");
            std::map<RowIdentity,const Json*> indexed;
            std::vector<const Json*> suffix;
            std::optional<std::size_t> previous;
            bool appended=false;
            for (const auto& row:rows) {
                const auto identity=destination(row);
                if (!indexed.emplace(identity,&row).second) invalid("Coordinated candidate row identity is ambiguous");
                const auto found=source_rows.find(identity);
                if (found==source_rows.end()) { appended=true; suffix.push_back(&row); }
                else {
                    if (appended || (previous && found->second<=*previous))
                        invalid("Coordinated candidate changed retained source row order");
                    previous=found->second;
                }
            }
            lane_rows.push_back(std::move(indexed));
            additions.push_back(std::move(suffix));
        }
        Json rows=Json::array();
        for (const auto& original:retained) {
            const auto identity=destination(original);
            const Json* consequence=&original;
            bool changed=false;
            for (const auto& lane:lane_rows) {
                const auto found=lane.find(identity);
                const Json* next=found==lane.end()?nullptr:found->second;
                if (next && exact_json(original,*next)) continue;
                if (changed) {
                    if (coalesce_exact_consequences && ((!consequence && !next) ||
                        (consequence && next && exact_json(*consequence,*next)))) continue;
                    if (allow_shared_reference_retirement && !consequence && !next &&
                        (source.type==kAnnotationEntityType || source.type==kSheetViewEntityType)) continue;
                    invalid("Coordinated families change the same retained source row");
                }
                changed=true; consequence=next;
            }
            if (consequence) rows.push_back(*consequence);
        }
        std::map<RowIdentity,const Json*> destinations;
        for (const auto& [identity,index]:source_rows) destinations.emplace(identity,&retained[index]);
        for (const auto& suffix:additions) for (const auto* row:suffix) {
            const auto [found,inserted]=destinations.emplace(destination(*row),row);
            if (!inserted) {
                // Membership/target references can have one exact shared
                // consequence. Newly owned child IDs never gain that exception.
                if (coalesce_exact_consequences && (!allowed.identity_key ||
                    std::string_view(allowed.identity_key)!="id") && exact_json(*found->second,*row)) continue;
                invalid("Coordinated fresh presentation or registry rows overlap");
            }
            rows.push_back(*row);
        }
        merged.properties.at(allowed.path)=std::move(rows);
        // Only after consuming row pointers restore the complete source table.
        for (auto& envelope:envelopes) envelope.properties.at(allowed.path)=retained;
    }
    for (const auto& envelope:envelopes) if (!exact(source,envelope))
        invalid("Coordinated shared container changed fields outside its explicit source row authority");
    if (source.type=="model_phases") (void)ModelPhases::from_json(merged.properties.at("model"));
    else if (source.type==kAnnotationEntityType) validate_annotation_entity(merged);
    else validate_sheet_view_entity(merged);
    return merged;
}

// Hosted transformation codecs may promote a legacy whole-catalog dialect.
// Omit only the defaults that promotion adds, then retain actual raw rows under
// v7. Definitions, identity namespaces and every other envelope field are exact.
Entity merge_hosted_instance_placements(const Entity& source,const std::vector<const Entity*>& candidates,
    const std::set<std::string,std::less<>>& permitted_retirements={}) {
    const auto& original=source.properties.at("model");
    (void)AssemblyModel::from_json(original);
    const auto& retained=original.at("instances");
    auto merged=source;
    auto rows=retained;
    std::set<std::string,std::less<>> changed;
    std::set<std::string,std::less<>> retired;
    bool promoted=false;
    for (const auto* candidate:candidates) {
        const auto& model=candidate->properties.at("model");
        (void)AssemblyModel::from_json(model);
        auto envelope=*candidate;
        auto& normalized=envelope.properties.at("model");
        const auto& schema=model.at("schema");
        const bool promotion=!exact_json(schema,original.at("schema"));
        if (promotion) {
            const auto before=original.at("schema").get<std::string>();
            const auto after=schema.get<std::string>();
            if (!((before=="sketch.assemblies.v3" || before=="sketch.assemblies.v4") &&
                    (after=="sketch.assemblies.v5" || after=="sketch.assemblies.v6" || after=="sketch.assemblies.v7")) &&
                !(before=="sketch.assemblies.v5" && (after=="sketch.assemblies.v6" || after=="sketch.assemblies.v7")) &&
                !(before=="sketch.assemblies.v6" && after=="sketch.assemblies.v7"))
                invalid("Coordinated hosted catalog has an unsupported dialect change");
            promoted=true;
            normalized.at("schema")=original.at("schema");
            auto& types=normalized.at("types");
            const auto& source_types=original.at("types");
            if (types.size()!=source_types.size()) invalid("Coordinated hosted catalog changed type definitions");
            for (std::size_t i=0;i<types.size();++i) for (const auto* field:{"profiles","parts"})
                if (!source_types[i].contains(field) && types[i].contains(field) &&
                    types[i].at(field).is_array() && types[i].at(field).empty()) types[i].erase(field);
        }
        auto& instances=normalized.at("instances");
        if (permitted_retirements.empty() && instances.size()!=retained.size())
            invalid("Coordinated hosted catalogs require exact source instance membership");
        std::size_t next=0;
        for (std::size_t i=0;i<retained.size();++i) {
            const auto& original_row=retained[i];
            const auto local_id=original_row.at("id").get<std::string>();
            if (next==instances.size() || instances[next].at("id")!=original_row.at("id")) {
                if (!permitted_retirements.contains(local_id))
                    invalid("Coordinated hosted catalog changed source instance order or identity");
                if (!changed.insert(local_id).second)
                    invalid("Coordinated families overlap hosted instance retirement or placement");
                retired.insert(local_id);
                continue;
            }
            auto row=instances[next++];
            if (row.at("id")!=original_row.at("id")) invalid("Coordinated hosted catalog changed source instance order or identity");
            if (promotion && !original_row.contains("root_transform") && row.contains("root_transform") &&
                row.at("root_transform").is_null() && row.contains("nested_overrides") &&
                row.at("nested_overrides").is_array() && row.at("nested_overrides").empty()) {
                row.erase("root_transform"); row.erase("nested_overrides");
            }
            if (original_row.contains("placement")) {
                auto& placed=row.at("placement");
                const auto& before=original_row.at("placement");
                if (promotion && !before.contains("vertical_scale") && placed.contains("vertical_scale") &&
                    placed.at("vertical_scale")==1.0) placed.erase("vertical_scale");
                if (promotion && before.at("translation_m").size()==2 && placed.at("translation_m").size()==3 &&
                    placed.at("translation_m")[2]==0.0) placed.at("translation_m").erase(placed.at("translation_m").begin()+2);
                if (!exact_json(placed.at("host_entity_id"),before.at("host_entity_id")))
                    invalid("Coordinated hosted catalog cannot change placement hosts");
                auto row_envelope=row;
                row_envelope.at("placement")=before;
                if (!exact_json(row_envelope,original_row))
                    invalid("Coordinated hosted catalog changed fields outside placement authority");
                if (!exact_json(placed,before)) {
                    if (!changed.insert(local_id).second) invalid("Coordinated families change the same hosted instance placement");
                    rows[i]=std::move(row);
                }
            } else if (!exact_json(row,original_row))
                invalid("Coordinated hosted catalog changed an unhosted instance");
        }
        if (next!=instances.size()) invalid("Coordinated hosted catalog added or reordered source instances");
        normalized.at("instances")=retained;
        if (!exact(source,envelope)) invalid("Coordinated hosted catalog changed definitions or its retained envelope");
    }
    if (!retired.empty()) {
        Json survivors=Json::array();
        for (const auto& row:rows) if (!retired.contains(row.at("id").get<std::string>())) survivors.push_back(row);
        rows=std::move(survivors);
    }
    merged.properties.at("model").at("instances")=std::move(rows);
    if (promoted && !changed.empty()) merged.properties.at("model").at("schema")="sketch.assemblies.v7";
    (void)AssemblyModel::from_json(merged.properties.at("model"));
    return merged;
}

Entity compose_source_container(const Entity& source,const std::vector<const Entity*>& candidates,
    const std::set<std::string,std::less<>>& permitted_retirements={}) {
    return source.type=="assembly_model" ? merge_hosted_instance_placements(source,candidates,permitted_retirements) :
        merge_source_row_container(source,candidates);
}

// Retirement can remove disjoint hosted rows from a shared catalog. It cannot
// change retained definitions, material assignments, row bytes or order.
// Only the dialect-sixteen opt-in also consumes fresh raw instance suffixes
// from complete independently admitted same-source primitive replay maps.
// Restoring the source instances must still recover the exact source envelope;
// this is no authority to normalize a dialect or merge arbitrary catalog data.
Entity merge_demolition_catalog(const Entity& source,const std::vector<const Entity*>& candidates,
    bool allow_shared_reference_retirement=false,bool complete_hosted_catalog_consequences=false) {
    const auto& retained=source.properties.at("model").at("instances");
    std::map<std::string,std::size_t,std::less<>> positions;
    for (std::size_t i=0;i<retained.size();++i)
        if (!positions.emplace(retained.at(i).at("id").get<std::string>(),i).second)
            invalid("Coordinated demolition has ambiguous actual hosted rows");
    std::set<std::string,std::less<>> removed;
    std::set<std::string,std::less<>> fresh_ids;
    auto fresh_rows=Json::array();
    for (const auto* candidate:candidates) {
        (void)AssemblyModel::from_json(candidate->properties.at("model"));
        auto envelope=*candidate;
        const auto& rows=envelope.properties.at("model").at("instances");
        std::set<std::string,std::less<>> surviving;
        std::optional<std::size_t> previous;
        bool appended=false;
        for (const auto& row:rows) {
            const auto name=row.at("id").get<std::string>();
            const auto actual=positions.find(name);
            if (actual==positions.end() && complete_hosted_catalog_consequences) {
                if (!fresh_ids.insert(name).second)
                    invalid("Coordinated demolition fresh hosted instance destinations overlap");
                appended=true;
                fresh_rows.push_back(row);
                continue;
            }
            if (actual==positions.end() || !surviving.insert(name).second ||
                appended || (previous && actual->second<=*previous) || !exact_json(row,retained.at(actual->second)))
                invalid("Coordinated demolition catalog authority is limited to actual row removal");
            previous=actual->second;
        }
        for (const auto& [name,index]:positions) {
            (void)index;
            if (!surviving.contains(name) && !removed.insert(name).second && !allow_shared_reference_retirement)
                invalid("Coordinated demolition repeats the same hosted row retirement");
        }
        envelope.properties.at("model").at("instances")=retained;
        if (!exact(source,envelope)) invalid("Coordinated demolition changed a retained catalog envelope");
    }
    auto result=source;
    auto rows=Json::array();
    for (const auto& row:retained)
        if (!removed.contains(row.at("id").get<std::string>())) rows.push_back(row);
    for (const auto& row:fresh_rows) rows.push_back(row);
    result.properties.at("model").at("instances")=std::move(rows);
    (void)AssemblyModel::from_json(result.properties.at("model"));
    return result;
}

Entity merge_demolition_row_container(const Entity& source,const std::vector<const Entity*>& candidates,
    bool ordinary_removal_rosters=false,bool allow_shared_reference_retirement=false,
    bool coalesce_exact_consequences=false) {
    if (source.type!=kSheetViewEntityType)
        return merge_source_row_container(source,candidates,ordinary_removal_rosters,allow_shared_reference_retirement,
            coalesce_exact_consequences);
    std::vector<Entity> normalized;
    normalized.reserve(candidates.size());
    const auto& source_views=source.properties.at("model").at("views");
    for (const auto* candidate:candidates) {
        normalized.push_back(*candidate);
        auto& views=normalized.back().properties.at("model").at("views");
        if (views.size()!=source_views.size()) invalid("Coordinated demolition cannot change the saved view inventory");
        for (std::size_t i=0;i<views.size();++i) {
            auto& view=views.at(i);
            const auto& actual=source_views.at(i);
            const bool same_presence=view.contains("restrict_to_objects")==actual.contains("restrict_to_objects");
            if (same_presence && (!view.contains("restrict_to_objects") ||
                exact_json(view.at("restrict_to_objects"),actual.at("restrict_to_objects")))) continue;
            // Preserve an emptied restricted view as restricted; an empty list
            // must never broaden a view to every surviving building object.
            if (actual.at("object_ids").empty() || !view.at("object_ids").empty() ||
                !view.contains("restrict_to_objects") || view.at("restrict_to_objects")!=true ||
                actual.value("restrict_to_objects",false))
                invalid("Coordinated demolition changed saved view restriction authority");
            if (actual.contains("restrict_to_objects")) view["restrict_to_objects"]=actual.at("restrict_to_objects");
            else view.erase("restrict_to_objects");
        }
    }
    std::vector<const Entity*> rows;
    for (const auto& candidate:normalized) rows.push_back(&candidate);
    auto result=merge_source_row_container(source,rows,false,allow_shared_reference_retirement,coalesce_exact_consequences);
    auto& views=result.properties.at("model").at("views");
    for (std::size_t i=0;i<views.size();++i)
        if (source_views.at(i).contains("object_ids") && views.at(i).contains("object_ids") &&
            (!source_views.at(i).at("object_ids").empty() || source_views.at(i).value("restrict_to_objects",false)) &&
            views.at(i).at("object_ids").empty()) views.at(i)["restrict_to_objects"]=true;
    validate_sheet_view_entity(result);
    return result;
}

std::vector<std::string> coordinated_wall_fresh(const PhaseConstraintAuthoringIntent& child) {
    std::vector<std::string> result;
    if (child.wall_replacement.is_null()) return result;
    const auto replacement=decode_phase_wall_replacement_authoring(child.wall_replacement);
    for (const auto& [original,fresh]:replacement.identities) { (void)original; result.push_back(fresh); }
    for (const auto& [original,fresh]:replacement.hosted_instance_identities) { (void)original; result.push_back(fresh); }
    if (!replacement.room_review_intent.is_null()) {
        const auto room=decode_physical_wall_phase_room_review_intent(replacement.room_review_intent);
        for (const auto& plane:room.planes) {
            for (const auto& decision:plane.fresh) {
                if (decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::create_proposed &&
                    decision.disposition!=PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) continue;
                if (decision.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) result.push_back(decision.room_id);
                result.insert(result.end(),decision.fresh_ids.segment_ids.begin(),decision.fresh_ids.segment_ids.end());
                result.insert(result.end(),decision.fresh_ids.vertex_ids.begin(),decision.fresh_ids.vertex_ids.end());
            }
            for (const auto& decision:plane.source_rooms)
                result.insert(result.end(),decision.replacement_dimension_ids.begin(),decision.replacement_dimension_ids.end());
        }
    }
    return result;
}

// Bound the whole composed map, including opaque keys/strings which reserve
// identity destinations in the existing leaf admission policy.
void coordinated_map_budget(const Entities& entities) {
    if (entities.size()>250000) invalid("Coordinated replacement entity budget exceeded");
    std::size_t nodes=0, bytes=0, serialized_bytes=0;
    const auto add_bytes=[&](std::size_t count) {
        if (count>64*1024*1024-bytes) invalid("Coordinated replacement JSON byte budget exceeded");
        bytes+=count;
    };
    const auto walk=[&](const auto& self,const Json& value,std::size_t depth)->void {
        if (depth>64 || ++nodes>4*1024*1024) invalid("Coordinated replacement JSON node/nesting budget exceeded");
        if (value.is_string()) add_bytes(value.get_ref<const std::string&>().size());
        else if (value.is_object()) for (const auto& [key,child]:value.items()) {
            add_bytes(key.size()); self(self,child,depth+1);
        } else if (value.is_array()) for (const auto& child:value) self(self,child,depth+1);
        else if (value.is_number_float() && !std::isfinite(value.get<double>()))
            invalid("Coordinated replacement source number must be finite");
    };
    for (const auto& [key,entity]:entities) {
        if (key!=entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("Coordinated replacement requires actual identified entity envelopes");
        (void)id(key); add_bytes(key.size()); add_bytes(entity.type.size());
        walk(walk,entity.properties,0); walk(walk,entity.extensions,0);
        // Reserve the enclosing map key and complete entity envelope as well
        // as its JSON payloads; opaque numeric arrays consume the same budget.
        const auto envelope_bytes=2*key.size()+entity.type.size()+128;
        if (envelope_bytes>64*1024*1024-serialized_bytes)
            invalid("Coordinated replacement serialized map byte budget exceeded");
        serialized_bytes+=envelope_bytes;
        for (const auto* payload:{&entity.properties,&entity.extensions}) {
            const auto count=payload->dump().size();
            if (count>64*1024*1024-serialized_bytes)
                invalid("Coordinated replacement serialized map byte budget exceeded");
            serialized_bytes+=count;
        }
    }
}

Entity merge_component_membership_rows(const Entity& original,const std::vector<const Entity*>& changes);

Entities replay_coordinated(const Entities& source,const PhaseConstraintAuthoringIntent& enclosing,
    const Entities* inspected_wall=nullptr,const std::vector<std::string>* inspected_wall_fresh=nullptr) {
    const auto lanes=coordinated(enclosing.coordinated_replacements,enclosing);
    const bool coordinated_hosted=lanes.version==5 || (lanes.roof && lanes.roof->include_hosted_instances) ||
        std::any_of(lanes.ordinary_roofs.begin(),lanes.ordinary_roofs.end(),
            [](const auto& edit) { return edit.coordinate_world_hosted_geometry; }) ||
        (lanes.stair && std::any_of(lanes.stair->compound_edits.begin(),lanes.stair->compound_edits.end(),
            [](const auto& edit) { return edit.coordinate_profile_hosted_geometry; }));
    coordinated_map_budget(source);
    if (lanes.version==5) (void)resolve_assembly_phase_memberships(source);
    std::vector<Entities> candidates;
    std::vector<std::vector<std::string>> family_fresh;
    std::set<std::string,std::less<>> retained_baselines;
    std::map<std::string,std::set<std::string,std::less<>>,std::less<>> permitted_catalog_retirements;
    if (lanes.wall) {
        const auto& child=*lanes.wall;
        const auto requests=phase_wall_replacement_requests(source,child.intent);
        if (!child.wall_replacement.is_null()) {
            const auto leaf=decode_phase_wall_replacement_authoring(child.wall_replacement);
            if (requests.size()!=1 || requests.front().registry_id!=leaf.registry_id ||
                requests.front().alternative_id!=leaf.alternative_id || requests.front().seed_wall_ids!=leaf.seed_wall_ids)
                invalid("Coordinated wall replacement roots differ from the actual typed geometry authority");
            const auto plan=inspect_phase_wall_replacement_plan(source,leaf.seed_wall_ids,leaf.registry_id,
                leaf.alternative_id,leaf.complete_presentations,leaf.complete_corner_windows,leaf.complete_hosted_instances);
            retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
        } else if (!requests.empty())
            invalid("Coordinated ordinary wall lane cannot borrow replacement authority for actual baseline owners");
        if (inspected_wall) {
            if (child.wall_replacement.is_null() || !inspected_wall_fresh)
                invalid("Coordinated detached preview requires an independently inspected wall replacement");
            candidates.push_back(*inspected_wall);
            family_fresh.push_back(*inspected_wall_fresh);
        } else {
            candidates.push_back(replay_phase_constraint_authoring(source,encode_phase_constraint_authoring_intent(child)));
            family_fresh.push_back(coordinated_wall_fresh(child));
        }
        if (lanes.version==5) {
            // Reserve aliases only after actual leaf replay, from exact copied
            // catalog/local destinations. Detached fresh IDs alone omit them.
            const auto leaf=decode_phase_wall_replacement_authoring(child.wall_replacement);
            const auto aliases=embedded_assembly_presentation_ids(candidates.back());
            for (const auto& [key,proposed]:leaf.hosted_instance_identities)
                family_fresh.back().push_back(aliases.at({leaf.identities.at(key.first),proposed}));
        }
    }
    if (lanes.roof) {
        const auto& leaf=*lanes.roof;
        const auto plan=inspect_phase_roof_replacement_plan(source,leaf.seed_roof_ids,leaf.registry_id,
            leaf.alternative_id,leaf.phase_qualified_joins,leaf.include_hosted_instances);
        auto replay=replay_phase_roof_replacement(source,plan,leaf.identities,leaf.roof_profiles,
            leaf.roof_opening_edits,leaf.roof_edits,leaf.ordinary_roof_edits,leaf.phase_qualified_joins,
            leaf.hosted_instance_identities,leaf.include_hosted_instances);
        candidates.push_back(std::move(replay.entities)); family_fresh.push_back(std::move(replay.fresh_identity_ids));
        retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
    } else if (!lanes.ordinary_roofs.empty()) {
        const auto partition=partition_phase_roof_geometry_edits(source,lanes.ordinary_roofs);
        if (partition.replacement || !partition.baseline_roof_edits.empty())
            invalid("Coordinated ordinary roof lane requires actual ordinary/proposed membership");
        candidates.push_back(replay_roof_edit_entities(source,lanes.ordinary_roofs));
        family_fresh.push_back(new_roof_opening_identity_ids(source,roof_edit_opening_intents(lanes.ordinary_roofs)));
    }
    if (lanes.slab) {
        const auto& leaf=*lanes.slab;
        const auto plan=inspect_phase_slab_replacement_plan(source,leaf.seed_slab_ids,leaf.registry_id,leaf.alternative_id);
        auto replay=replay_phase_slab_replacement(source,plan,leaf.identities,leaf.slab_profiles,
            leaf.slab_stacks,leaf.slab_geometry,leaf.ordinary_geometry,leaf.hosted_instance_identities,
            leaf.coordinate_ordinary_hosted_geometry);
        candidates.push_back(std::move(replay.entities)); family_fresh.push_back(std::move(replay.fresh_identity_ids));
        retained_baselines.insert(plan.seed_slab_ids.begin(),plan.seed_slab_ids.end());
    } else if (!lanes.ordinary_slabs.empty()) {
        const auto partition=partition_phase_slab_geometry_edits(source,lanes.ordinary_slabs);
        if (partition.replacement || !partition.baseline_geometry.empty())
            invalid("Coordinated ordinary slab lane requires actual ordinary/proposed membership");
        candidates.push_back(replay_slab_geometry_with_hosted_entities(source,lanes.ordinary_slabs));
        family_fresh.emplace_back();
    }
    if (lanes.structural) {
        const auto& leaf=*lanes.structural;
        const auto plan=inspect_phase_structural_replacement_plan(source,leaf.seed_object_ids,
            leaf.registry_id,leaf.alternative_id,leaf.complete_hosted);
        auto candidate=replay_phase_structural_replacement_authoring(source,leaf);
        std::vector<std::string> destinations;
        for (const auto& [original,proposed]:leaf.identities) { (void)original; destinations.push_back(proposed); }
        for (const auto& [key,proposed]:leaf.hosted_instance_identities) { (void)key; destinations.push_back(proposed); }
        if (leaf.complete_hosted) {
            // Aliases are derived after the complete leaf's physical and
            // presentation replay; local instance IDs never stand in for them.
            const auto aliases=embedded_assembly_presentation_ids(candidate);
            for (const auto& [key,proposed]:leaf.hosted_instance_identities)
                destinations.push_back(aliases.at({leaf.identities.at(key.first),proposed}));
        }
        candidates.push_back(std::move(candidate)); family_fresh.push_back(std::move(destinations));
        retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
    } else if (!lanes.ordinary_structural.empty()) {
        const auto scope=constraint_phase_scope(source);
        std::set<std::string,std::less<>> shared_baselines;
        for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
            (void)key;
            const auto model=ModelPhases::from_json(entity.properties.at("model"));
            if (model.active_alternative()) shared_baselines.insert(model.baseline_ids().begin(),model.baseline_ids().end());
        }
        for (const auto& edit:lanes.ordinary_structural) {
            if (scope.inactive_owner_ids.contains(edit.object_id))
                invalid("Coordinated ordinary structural lane requires actual active membership");
            if (shared_baselines.contains(edit.object_id))
                invalid("Coordinated ordinary structural lane cannot borrow actual shared-baseline authority");
        }
        if (phase_structural_edit_replacement_request(source,lanes.ordinary_structural))
            invalid("Coordinated ordinary structural lane cannot borrow replacement authority");
        candidates.push_back(replay_structural_hosted_component_geometry(source,lanes.ordinary_structural));
        family_fresh.emplace_back();
    }
    if (lanes.stair) {
        const auto& leaf=*lanes.stair;
        // The leaf codec and actual-source replay own complete closure,
        // quantity receipts, hosting, private catalogs and saved presentation.
        // Successful replay verifies that identities are exactly its discovered
        // required owner inventory; a second native plan replay is unnecessary.
        auto candidate=replay_phase_stair_replacement_authoring(source,leaf);
        // Only a successfully replayed closed dependency leaf grants omission
        // authority, qualified by the actual catalog and actual placement host.
        std::set<std::string,std::less<>> retired_rails;
        for (const auto& disposition:leaf.dependency_dispositions)
            if (disposition.action==PhaseStairReplacementDependencyAction::retire)
                retired_rails.insert(disposition.rail_id);
        for (const auto& [catalog,entity]:source) if (entity.type=="assembly_model")
            for (const auto& row:entity.properties.at("model").at("instances"))
                if (row.contains("placement") && retired_rails.contains(
                    row.at("placement").at("host_entity_id").get<std::string>()))
                    permitted_catalog_retirements[catalog].insert(row.at("id").get<std::string>());
        std::vector<std::string> destinations;
        for (const auto& [original,proposed]:leaf.identities) {
            retained_baselines.insert(original); destinations.push_back(proposed);
        }
        for (const auto& [key,proposed]:leaf.child_identities) { (void)key; destinations.push_back(proposed); }
        for (const auto& [key,proposed]:leaf.hosted_instance_identities) { (void)key; destinations.push_back(proposed); }
        for (const auto& [key,proposed]:leaf.overlay_identities) { (void)key; destinations.push_back(proposed); }
        if (!leaf.hosted_instance_identities.empty()) {
            const auto aliases=embedded_assembly_presentation_ids(candidate);
            for (const auto& [key,proposed]:leaf.hosted_instance_identities)
                destinations.push_back(aliases.at({leaf.identities.at(key.first),proposed}));
        }
        candidates.push_back(std::move(candidate)); family_fresh.push_back(std::move(destinations));
    } else if (!lanes.ordinary_stairs.empty()) {
        const auto scope=constraint_phase_scope(source);
        std::set<std::string,std::less<>> shared_baselines;
        for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
            (void)key;
            const auto model=ModelPhases::from_json(entity.properties.at("model"));
            if (model.active_alternative()) shared_baselines.insert(model.baseline_ids().begin(),model.baseline_ids().end());
        }
        for (const auto& edit:lanes.ordinary_stairs) {
            if (scope.inactive_owner_ids.contains(edit.object_id))
                invalid("Coordinated ordinary stair lane requires actual active membership");
            if (shared_baselines.contains(edit.object_id))
                invalid("Coordinated ordinary stair lane cannot borrow actual shared-baseline authority");
        }
        // This discovers baseline consequences propagated to actual attached
        // rails as well as explicit roots; no scratch candidate becomes source.
        if (phase_stair_replacement_request(source,lanes.ordinary_stairs))
            invalid("Coordinated ordinary stair lane cannot borrow replacement authority for attached baseline owners");
        candidates.push_back(replay_stair_transform_entities(source,lanes.ordinary_stairs));
        family_fresh.emplace_back();
    }
    if (lanes.version>=3) for (const auto& candidate:candidates) coordinated_map_budget(candidate);
    std::set<std::string,std::less<>> fresh;
    for (const auto& destinations:family_fresh) for (const auto& destination:destinations) {
        // Leaf admission owns entity/child syntax. Its derived presentation
        // aliases may combine two valid IDs and exceed an entity ID's length.
        if (destination.empty()) invalid("Coordinated replacement has an empty derived identity destination");
        if (!fresh.insert(destination).second) invalid("Coordinated replacement fresh identity destinations overlap");
        if (fresh.size()>4096) invalid("Coordinated replacement identity budget exceeded");
    }
    auto result=source;
    for (const auto& [key,original]:source) {
        const std::set<std::string,std::less<>> no_retirements;
        const auto permission=permitted_catalog_retirements.find(key);
        const auto& permitted=permission==permitted_catalog_retirements.end() ? no_retirements : permission->second;
        std::vector<const Entity*> changes;
        bool removed=false;
        for (const auto& candidate:candidates) {
            const auto found=candidate.find(key);
            if (found==candidate.end()) {
                if (lanes.version==1 || removed || (lanes.version>=3 && original.type=="assembly_model") ||
                    (lanes.version==5 && original.type==assembly_phase_membership_entity_type))
                    invalid("Coordinated replacement cannot overlap removal of an actual source owner or hosted catalog");
                removed=true;
            } else if (!exact(original,found->second)) changes.push_back(&found->second);
        }
        if (removed) {
            if (!changes.empty()) invalid("Coordinated family removal overlaps another physical consequence");
            result.erase(key);
        } else if (!changes.empty() && lanes.version==5 && original.type==assembly_phase_membership_entity_type)
            // Only independently replayed same-source leaves reach this typed
            // partition merge; retain the original companion carrier and order.
            result.at(key)=merge_component_membership_rows(original,changes);
        else if (changes.size()==1) result.at(key)=lanes.version>=3 && original.type=="assembly_model" ?
            merge_hosted_instance_placements(original,changes,permitted) : *changes.front();
        else if (changes.size()>1)
            result.at(key)=lanes.version==1 && !(coordinated_hosted && original.type=="assembly_model")
                ? merge_append_container(original,*changes[0],*changes[1]) :
                (lanes.version>=3 || (coordinated_hosted && original.type=="assembly_model"))
                    ? compose_source_container(original,changes,permitted) : merge_source_row_container(original,changes);
    }
    for (std::size_t lane=0;lane<candidates.size();++lane) {
        const std::set<std::string,std::less<>> destinations(family_fresh[lane].begin(),family_fresh[lane].end());
        for (const auto& [key,entity]:candidates[lane]) if (!source.contains(key)) {
            if (!destinations.contains(key) || !result.emplace(key,entity).second)
                invalid("Coordinated replacement new entity destination is unreserved or overlapping");
        }
    }
    if (lanes.version>=2) for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
        (void)key;
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        if (lanes.version==2 || model.active_alternative())
            retained_baselines.insert(model.baseline_ids().begin(),model.baseline_ids().end());
    }
    for (const auto& key:retained_baselines) {
        if (!result.contains(key)) invalid("Coordinated replacement removed a retained baseline owner");
        if ((lanes.version>=3 || coordinated_hosted) && source.at(key).type=="assembly_model") continue;
        if (!exact(source.at(key),result.at(key))) invalid("Coordinated replacement changed a retained baseline envelope");
    }
    if (lanes.version>=3 || coordinated_hosted) {
        for (const auto& [key,entity]:source) if (entity.type=="assembly_model") {
            const auto& before=entity.properties.at("model").at("instances");
            const auto& after=result.at(key).properties.at("model").at("instances");
            std::map<std::string,const Json*,std::less<>> survivors;
            for (const auto& row:after)
                if (!survivors.emplace(row.at("id").get<std::string>(),&row).second)
                    invalid("Coordinated replacement has ambiguous surviving hosted rows");
            for (const auto& row:before) if (row.contains("placement") &&
                retained_baselines.contains(row.at("placement").at("host_entity_id").get<std::string>())) {
                const auto found=survivors.find(row.at("id").get<std::string>());
                if (found==survivors.end() || !exact_json(row,*found->second))
                    invalid("Coordinated replacement changed a retained baseline hosted row");
            }
        }
        // A second family can force a different computed fallback alias. That
        // must refuse rather than silently retarget a leaf's saved references.
        const auto original_aliases=embedded_assembly_presentation_ids(source);
        const auto final_aliases=embedded_assembly_presentation_ids(result);
        const auto permitted_missing_alias=[&](const auto& key) {
            const auto catalog=permitted_catalog_retirements.find(key.first);
            return catalog!=permitted_catalog_retirements.end() && catalog->second.contains(key.second);
        };
        for (const auto& [key,alias]:original_aliases) {
            const auto found=final_aliases.find(key);
            if (fresh.contains(alias) || (found==final_aliases.end() ? !permitted_missing_alias(key) : found->second!=alias))
                invalid("Coordinated replacement changed or reused an actual source render alias");
        }
        for (const auto& candidate:candidates) for (const auto& [key,alias]:embedded_assembly_presentation_ids(candidate)) {
            const auto found=final_aliases.find(key);
            if (found==final_aliases.end() ? !permitted_missing_alias(key) : found->second!=alias)
                invalid("Coordinated complete candidate changed a leaf's computed render alias");
        }
        validate_document_assembly_instances(result);
    }
    coordinated_map_budget(result);
    std::set<std::string,std::less<>> members;
    for (const auto& [key,entity]:result) if (entity.type=="model_phases") {
        (void)key;
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& member:model.entity_ids()) {
            if (!result.contains(member) || !is_model_phase_entity_type(result.at(member).type) || !members.insert(member).second)
                invalid("Coordinated merged phase membership is missing, unsupported or overlapping");
        }
    }
    // Also resolves the complete companion graph against final actual catalogs
    // and saved registries; typed partition merging never bypasses admission.
    (void)constraint_phase_scope(result);
    if (lanes.version>=2 && !inspected_wall) if (const auto error=validate_active_phase_constraint_integrity(result))
        throw std::invalid_argument(*error);
    validate_roof_join_ownership(result);
    return result;
}
} // namespace

nlohmann::json phase_constraint_authoring_selections(const Entities& source) {
    Json result=Json::array();
    for (const auto& state : constraint_phase_scope(source).registries)
        result.push_back({{"registry_id",state.registry_id},
            {"alternative_id",state.alternative_id ? Json(*state.alternative_id) : Json(nullptr)}});
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b) {
        return a.at("registry_id").get<std::string>()<b.at("registry_id").get<std::string>();
    });
    selections(result); return result;
}

nlohmann::json encode_phase_constraint_authoring_intent(const PhaseConstraintAuthoringIntent& value) {
    digest(value.source_snapshot_digest); digest(value.source_authoring_digest); digest(value.source_entities_digest);
    if (value.source_saved_revision && *value.source_saved_revision>value.expected_revision)
        invalid("Phase constraint saved revision exceeds its source revision");
    selections(value.phase_selections);
    const auto exclusive_operations = static_cast<int>(!value.wall_replacement.is_null()) +
        static_cast<int>(!value.opening_demolition.is_null()) + static_cast<int>(!value.roof_replacement.is_null()) +
        static_cast<int>(!value.slab_replacement.is_null()) + static_cast<int>(!value.slab_demolition.is_null()) +
        static_cast<int>(!value.coordinated_replacements.is_null()) +
        static_cast<int>(!value.structural_replacement.is_null()) +
        static_cast<int>(!value.stair_demolition.is_null()) +
        static_cast<int>(!value.stair_replacement.is_null()) +
        static_cast<int>(!value.stair_demolition_retirement.is_null()) +
        static_cast<int>(!value.coordinated_demolition.is_null()) +
        static_cast<int>(!value.wall_demolition.is_null()) +
        static_cast<int>(!value.ordinary_roof_edits.is_null()) +
        static_cast<int>(value.intent.wall_group_scale.has_value());
    // The standalone baseline scale lane pairs only these two authorities.
    // Every other operation remains exclusive, including ordinary scale.
    const bool baseline_wall_group_scale=value.intent.wall_group_scale && !value.wall_replacement.is_null();
    const bool joint_similarity=value.intent.joint_translation &&
        (value.intent.joint_translation->per_owner_similarity_completion || !value.intent.joint_translation->owner_similarities.empty());
    if (joint_similarity) {
        ConstraintAuthoringIntent only_similarity;
        only_similarity.joint_translation=value.intent.joint_translation;
        only_similarity.message=value.intent.message;
        if (exclusive_operations || encode_intent(value.intent)!=encode_intent(only_similarity))
            invalid("Joint similarity cannot borrow replacement, sibling geometry or relationship authority");
    }
    if (exclusive_operations > (baseline_wall_group_scale ? 2 : 1))
        invalid("Active design operations cannot borrow another replacement or demolition authority");
    if (value.intent.wall_group_scale) {
        ConstraintAuthoringIntent only_scale;
        only_scale.wall_group_scale=value.intent.wall_group_scale;
        only_scale.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(only_scale))
            invalid("Wall group scale cannot borrow sibling geometry or relationship authority");
    }
    const auto coordinated_version=value.coordinated_replacements.is_null()?0:coordinated(value.coordinated_replacements,value).version;
    Json result={{"version",baseline_wall_group_scale?19:value.intent.wall_group_scale?18:!value.ordinary_roof_edits.is_null()?17:!value.wall_demolition.is_null()?16:!value.coordinated_demolition.is_null()?15:!value.stair_demolition_retirement.is_null()?13:!value.stair_replacement.is_null()?12:!value.stair_demolition.is_null()?11:!value.structural_replacement.is_null()?9:coordinated_version==5?29:coordinated_version==4?14:coordinated_version==3?10:coordinated_version==2?8:coordinated_version==1?7:!value.slab_demolition.is_null()?6:!value.slab_replacement.is_null()?5:!value.roof_replacement.is_null()?4:!value.opening_demolition.is_null()?3:value.wall_replacement.is_null()?1:2},{"expected_revision",value.expected_revision},
        {"source_snapshot_digest",value.source_snapshot_digest},{"source_authoring_digest",value.source_authoring_digest},
        {"source_entities_digest",value.source_entities_digest},
        {"source_saved_revision",value.source_saved_revision ? Json(*value.source_saved_revision) : Json(nullptr)},
        {"phase_selections",value.phase_selections},{"intent",encode_intent(value.intent)}};
    if (joint_similarity) result["version"]=22;
    if (!value.wall_replacement.is_null()) {
        const auto replacement=decode_phase_wall_replacement_authoring(value.wall_replacement);
        if (encode_phase_wall_replacement_authoring(replacement).dump()!=value.wall_replacement.dump())
            invalid("Wall replacement decisions are not canonical");
        if (replacement.complete_scalar_profiles) result["version"]=replacement.complete_vertical_profiles ? 26 : 23;
        const auto& wall_body=replacement.complete_hosted_instances ? value.wall_replacement.at("authoring") : value.wall_replacement;
        if (baseline_wall_group_scale && (wall_body.at("version")!=8 ||
            !replacement.complete_presentations || !replacement.complete_corner_windows ||
            !replacement.wall_profiles.empty() || !replacement.opening_profiles.empty() ||
            !replacement.opening_rehosts.empty() || !replacement.opening_families.empty() ||
            !replacement.wall_stacks.empty() || !replacement.corner_profiles.empty()))
            invalid("Baseline wall group scale requires complete leaf eight without other wall edit authority");
        if (replacement.complete_hosted_instances) {
            // Qualified copying is additive authority. Preserve the exact
            // historical edit lane explicitly; old outers cannot borrow it.
            result["wall_edit_version"]=result.at("version");
            result["version"]=28;
        }
        result["wall_replacement"]=value.wall_replacement;
    }
    if (!value.opening_demolition.is_null()) {
        const auto demolition = decode_phase_opening_demolition_intent(value.opening_demolition);
        if (encode_phase_opening_demolition_intent(demolition).dump() != value.opening_demolition.dump())
            invalid("Opening demolition decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message = value.intent.message;
        if (encode_intent(value.intent) != encode_intent(empty))
            invalid("Opening demolition cannot borrow geometry or relationship authority");
        result["opening_demolition"] = value.opening_demolition;
    }
    if (!value.roof_replacement.is_null()) {
        const auto replacement = decode_phase_roof_replacement_authoring(value.roof_replacement);
        if (encode_phase_roof_replacement_authoring(replacement).dump() != value.roof_replacement.dump())
            invalid("Roof replacement decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message = value.intent.message;
        if (encode_intent(value.intent) != encode_intent(empty))
            invalid("Roof replacement cannot borrow geometry or relationship authority");
        result["roof_replacement"] = value.roof_replacement;
        if (value.roof_replacement.at("version") == 10) result["version"] = 21;
    }
    if (!value.slab_replacement.is_null()) {
        const auto replacement = decode_phase_slab_replacement_authoring(value.slab_replacement);
        if (encode_phase_slab_replacement_authoring(replacement).dump() != value.slab_replacement.dump())
            invalid("Slab replacement decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message = value.intent.message;
        if (encode_intent(value.intent) != encode_intent(empty))
            invalid("Slab replacement cannot borrow geometry or relationship authority");
        result["slab_replacement"] = value.slab_replacement;
        if (replacement.coordinate_hosted_profiles) result["version"] = 27;
    }
    if (!value.slab_demolition.is_null()) {
        const auto demolition = decode_slab_demolition_intent(value.slab_demolition);
        if (encode_slab_demolition_intent(demolition).dump() != value.slab_demolition.dump())
            invalid("Slab demolition decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message = value.intent.message;
        if (encode_intent(value.intent) != encode_intent(empty))
            invalid("Slab demolition cannot borrow geometry or relationship authority");
        result["slab_demolition"] = value.slab_demolition;
    }
    if (!value.coordinated_replacements.is_null()) {
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Coordinated replacements cannot borrow other geometry or relationship authority");
        result["coordinated_replacements"]=value.coordinated_replacements;
    }
    if (!value.structural_replacement.is_null()) {
        const auto replacement=decode_phase_structural_replacement_authoring(value.structural_replacement);
        if (encode_phase_structural_replacement_authoring(replacement).dump()!=value.structural_replacement.dump())
            invalid("Structural replacement decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Structural replacement cannot borrow geometry or relationship authority");
        result["structural_replacement"]=value.structural_replacement;
        if (value.structural_replacement.at("version")==4) result["version"]=24;
    }
    if (!value.stair_demolition.is_null()) {
        const auto demolition=decode_stair_demolition_intent(value.stair_demolition);
        if (encode_stair_demolition_intent(demolition).dump()!=value.stair_demolition.dump())
            invalid("Stair demolition decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Stair demolition cannot borrow geometry or relationship authority");
        result["stair_demolition"]=value.stair_demolition;
    }
    if (!value.stair_replacement.is_null()) {
        const auto replacement=decode_phase_stair_replacement_authoring(value.stair_replacement);
        if (replacement.complete_scalar_dependencies) result["version"]=25;
        if (encode_phase_stair_replacement_authoring(replacement).dump()!=value.stair_replacement.dump())
            invalid("Stair replacement decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Stair replacement cannot borrow geometry or relationship authority");
        result["stair_replacement"]=value.stair_replacement;
    }
    if (!value.stair_demolition_retirement.is_null()) {
        const auto retirement=decode_stair_demolition_retirement_intent(value.stair_demolition_retirement);
        if (encode_stair_demolition_retirement_intent(retirement).dump()!=value.stair_demolition_retirement.dump())
            invalid("Stair dependent retirement decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Stair dependent retirement cannot borrow geometry or relationship authority");
        result["stair_demolition_retirement"]=value.stair_demolition_retirement;
    }
    if (!value.coordinated_demolition.is_null()) {
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Coordinated demolition cannot borrow geometry or relationship authority");
        result["coordinated_demolition"]=encode_phase_coordinated_demolition(value.coordinated_demolition,value);
    }
    if (!value.wall_demolition.is_null()) {
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Wall demolition cannot borrow geometry or relationship authority");
        const auto demolition=decode_phase_wall_demolition_authoring(value.wall_demolition);
        result["wall_demolition"]=encode_phase_wall_demolition_authoring(demolition);
        if (result.at("wall_demolition").dump()!=value.wall_demolition.dump())
            invalid("Wall demolition decisions are not canonical");
    }
    if (!value.ordinary_roof_edits.is_null()) {
        (void)ordinary_roof_edits(value.ordinary_roof_edits, true);
        if (has_rotated_roof_edits(value.ordinary_roof_edits)) result["version"] = 20;
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Ordinary roof authoring cannot borrow geometry or relationship authority");
        result["ordinary_roof_edits"]=value.ordinary_roof_edits;
    }
    // dump validates UTF-8 as well as the complete byte resource bound.
    resource_shape(result);
    if (result.dump().size()>proof_budget) invalid("Phase constraint proof exceeds its byte budget");
    return result;
}

PhaseConstraintAuthoringIntent decode_phase_constraint_authoring_intent(const Json& value) {
    resource_shape(value);
    if (value.dump().size()>proof_budget) invalid("Phase constraint proof exceeds its byte budget");
    const bool hosted_walls=value.is_object() && value.contains("version") && value.at("version")==28;
    if (hosted_walls && (!value.contains("wall_edit_version") || !value.at("wall_edit_version").is_number_integer() ||
        (value.at("wall_edit_version")!=2 && value.at("wall_edit_version")!=19 &&
         value.at("wall_edit_version")!=23 && value.at("wall_edit_version")!=26)))
        invalid("Qualified wall copies require an explicit historical wall edit lane");
    const auto wall_edit_version=hosted_walls ? value.at("wall_edit_version") :
        (value.is_object() ? value.value("version",Json()) : Json());
    const bool vertical_walls=wall_edit_version==26;
    const bool scalar_walls=vertical_walls || wall_edit_version==23;
    const bool replacement=hosted_walls || scalar_walls || (value.is_object() && value.contains("version") && value.at("version")==2);
    const bool demolition=value.is_object() && value.contains("version") && value.at("version")==3;
    const bool roof_rotation=value.is_object() && value.contains("version") && value.at("version")==21;
    const bool roof=roof_rotation || (value.is_object() && value.contains("version") && value.at("version")==4);
    const bool vertical_slabs=value.is_object() && value.contains("version") && value.at("version")==27;
    const bool slab=vertical_slabs || (value.is_object() && value.contains("version") && value.at("version")==5);
    const bool slab_demolition=value.is_object() && value.contains("version") && value.at("version")==6;
    const bool coordinated_replacements=value.is_object() && value.contains("version") &&
        (value.at("version")==7 || value.at("version")==8 || value.at("version")==10 || value.at("version")==14 || value.at("version")==29);
    const bool scalar_structural=value.is_object() && value.contains("version") && value.at("version")==24;
    const bool structural=scalar_structural || (value.is_object() && value.contains("version") && value.at("version")==9);
    const bool stair_demolition=value.is_object() && value.contains("version") && value.at("version")==11;
    const bool scalar_stairs=value.is_object() && value.contains("version") && value.at("version")==25;
    const bool stair_replacement=scalar_stairs || (value.is_object() && value.contains("version") && value.at("version")==12);
    const bool stair_retirement=value.is_object() && value.contains("version") && value.at("version")==13;
    const bool coordinated_demolition=value.is_object() && value.contains("version") && value.at("version")==15;
    const bool wall_demolition=value.is_object() && value.contains("version") && value.at("version")==16;
    const bool ordinary_roof_rotations=value.is_object() && value.contains("version") && value.at("version")==20;
    const bool ordinary_roofs=ordinary_roof_rotations || (value.is_object() && value.contains("version") && value.at("version")==17);
    const bool wall_group_scale=value.is_object() && value.contains("version") && value.at("version")==18;
    const bool baseline_wall_group_scale=wall_edit_version==19;
    if (hosted_walls) keys(value,{"version","wall_edit_version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","wall_replacement"});
    else if (replacement || baseline_wall_group_scale) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","wall_replacement"});
    else if (demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","opening_demolition"});
    else if (roof) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","roof_replacement"});
    else if (slab) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","slab_replacement"});
    else if (slab_demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","slab_demolition"});
    else if (coordinated_replacements) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","coordinated_replacements"});
    else if (structural) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","structural_replacement"});
    else if (stair_demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","stair_demolition"});
    else if (stair_replacement) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","stair_replacement"});
    else if (stair_retirement) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","stair_demolition_retirement"});
    else if (coordinated_demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","coordinated_demolition"});
    else if (wall_demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","wall_demolition"});
    else if (ordinary_roofs) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","ordinary_roof_edits"});
    else keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent"});
    if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5 && value.at("version")!=6 && value.at("version")!=7 && value.at("version")!=8 && value.at("version")!=9 && value.at("version")!=10 && value.at("version")!=11 && value.at("version")!=12 && value.at("version")!=13 && value.at("version")!=14 && value.at("version")!=15 && value.at("version")!=16 && value.at("version")!=17 && value.at("version")!=18 && value.at("version")!=19 && value.at("version")!=20 && value.at("version")!=21 && value.at("version")!=22 && value.at("version")!=23 && value.at("version")!=24 && value.at("version")!=25 && value.at("version")!=26 && value.at("version")!=27 && value.at("version")!=28 && value.at("version")!=29) ||
        !value.at("expected_revision").is_number_unsigned()) invalid("Phase constraint proof version or revision is invalid");
    if (!value.at("source_saved_revision").is_null() && !value.at("source_saved_revision").is_number_unsigned())
        invalid("Phase constraint saved revision is invalid");
    PhaseConstraintAuthoringIntent result{value.at("expected_revision").get<Revision>(),
        value.at("source_snapshot_digest").get<std::string>(),value.at("source_authoring_digest").get<std::string>(),
        value.at("source_entities_digest").get<std::string>(),
        value.at("source_saved_revision").is_null() ? std::nullopt : std::optional<Revision>{value.at("source_saved_revision").get<Revision>()},
        value.at("phase_selections"),decode_intent(value.at("intent"),wall_group_scale || baseline_wall_group_scale)};
    if (replacement || baseline_wall_group_scale) {
        result.wall_replacement=value.at("wall_replacement");
        if (result.wall_replacement.is_null()) invalid("Wall replacement dialect requires wall replacement decisions");
        const auto leaf=decode_phase_wall_replacement_authoring(result.wall_replacement);
        if (leaf.complete_scalar_profiles!=scalar_walls || leaf.complete_vertical_profiles!=vertical_walls ||
            leaf.complete_hosted_instances!=hosted_walls)
            invalid("Wall scalar cohort outer and leaf versions do not match");
    }
    if (demolition) {
        result.opening_demolition = value.at("opening_demolition");
        if (result.opening_demolition.is_null()) invalid("Version three requires opening demolition decisions");
        (void)decode_phase_opening_demolition_intent(result.opening_demolition);
    }
    if (roof) {
        result.roof_replacement = value.at("roof_replacement");
        if (result.roof_replacement.is_null()) invalid("Roof replacement dialect requires replacement decisions");
        (void)decode_phase_roof_replacement_authoring(result.roof_replacement);
        if ((result.roof_replacement.at("version") == 10) != roof_rotation)
            invalid("Roof replacement outer and rotation leaf versions do not match");
    }
    if (slab) {
        result.slab_replacement = value.at("slab_replacement");
        if (result.slab_replacement.is_null()) invalid("Version five requires slab replacement decisions");
        const auto leaf=decode_phase_slab_replacement_authoring(result.slab_replacement);
        if (leaf.coordinate_hosted_profiles!=vertical_slabs)
            invalid("Slab elevation carry outer and leaf versions do not match");
    }
    if (slab_demolition) {
        result.slab_demolition = value.at("slab_demolition");
        if (result.slab_demolition.is_null()) invalid("Version six requires slab demolition decisions");
        (void)decode_slab_demolition_intent(result.slab_demolition);
    }
    if (coordinated_replacements) {
        result.coordinated_replacements=value.at("coordinated_replacements");
        if (result.coordinated_replacements.is_null()) invalid("Coordinated authoring requires replacement decisions");
        const auto inner=coordinated(result.coordinated_replacements,result);
        if ((value.at("version")==7 && inner.version!=1) || (value.at("version")==8 && inner.version!=2) ||
            (value.at("version")==10 && inner.version!=3) || (value.at("version")==14 && inner.version!=4) ||
            (value.at("version")==29 && inner.version!=5))
            invalid("Coordinated outer and inner authoring versions do not match");
    }
    if (structural) {
        result.structural_replacement=value.at("structural_replacement");
        if (result.structural_replacement.is_null()) invalid("Version nine requires structural replacement decisions");
        (void)decode_phase_structural_replacement_authoring(result.structural_replacement);
        if ((result.structural_replacement.at("version")==4)!=scalar_structural)
            invalid("Structural scalar cohort outer and leaf versions do not match");
    }
    if (stair_demolition) {
        result.stair_demolition=value.at("stair_demolition");
        if (result.stair_demolition.is_null()) invalid("Version eleven requires stair demolition decisions");
        (void)decode_stair_demolition_intent(result.stair_demolition);
    }
    if (stair_replacement) {
        result.stair_replacement=value.at("stair_replacement");
        if (result.stair_replacement.is_null()) invalid("Version twelve requires stair replacement decisions");
        const auto leaf=decode_phase_stair_replacement_authoring(result.stair_replacement);
        if (leaf.complete_scalar_dependencies!=scalar_stairs)
            invalid("Stair scalar dependency authority requires its matching outer version");
    }
    if (stair_retirement) {
        result.stair_demolition_retirement=value.at("stair_demolition_retirement");
        if (result.stair_demolition_retirement.is_null()) invalid("Version thirteen requires stair dependent retirement decisions");
        (void)decode_stair_demolition_retirement_intent(result.stair_demolition_retirement);
    }
    if (coordinated_demolition) {
        result.coordinated_demolition=value.at("coordinated_demolition");
        if (result.coordinated_demolition.is_null()) invalid("Version fifteen requires coordinated demolition decisions");
        (void)encode_phase_coordinated_demolition(result.coordinated_demolition,result);
    }
    if (wall_demolition) {
        result.wall_demolition=value.at("wall_demolition");
        if (result.wall_demolition.is_null()) invalid("Version sixteen requires wall demolition decisions");
        (void)decode_phase_wall_demolition_authoring(result.wall_demolition);
    }
    if (ordinary_roofs) {
        result.ordinary_roof_edits=value.at("ordinary_roof_edits");
        (void)ordinary_roof_edits(result.ordinary_roof_edits, ordinary_roof_rotations);
        if (has_rotated_roof_edits(result.ordinary_roof_edits) != ordinary_roof_rotations)
            invalid("Ordinary roof outer and rotation versions do not match");
    }
    if (encode_phase_constraint_authoring_intent(result)!=value) invalid("Phase constraint proof is not canonical");
    return result;
}

std::vector<PhaseConstraintAuthoringIntent> phase_constraint_replacement_components(
    const PhaseConstraintAuthoringIntent& intent) {
    (void)encode_phase_constraint_authoring_intent(intent);
    if (!intent.wall_demolition.is_null()) {
        const auto demolition=decode_phase_wall_demolition_authoring(intent.wall_demolition);
        if (demolition.other_authoring.is_null()) return {};
        return phase_constraint_replacement_components(
            decode_phase_constraint_authoring_intent(demolition.other_authoring));
    }
    if (!intent.coordinated_demolition.is_null())
        return phase_coordinated_demolition_components(intent.coordinated_demolition,intent);
    // Standalone phase nineteen returns its complete scale/replacement pair,
    // so original-preservation guards inspect its actual replacement plan.
    if (intent.coordinated_replacements.is_null()) return {intent};
    std::vector<PhaseConstraintAuthoringIntent> result;
    const auto lanes=coordinated(intent.coordinated_replacements,intent);
    if (lanes.wall && !lanes.wall->wall_replacement.is_null()) result.push_back(*lanes.wall);
    for (const auto* family:{"roof_replacement","slab_replacement","structural_replacement","stair_replacement"}) {
        if (!intent.coordinated_replacements.contains(family)) continue;
        const auto& leaf=intent.coordinated_replacements.at(family);
        if (leaf.is_null()) continue;
        auto component=intent;
        component.coordinated_replacements=nullptr;
        if (std::string_view(family)=="roof_replacement") component.roof_replacement=leaf;
        else if (std::string_view(family)=="slab_replacement") component.slab_replacement=leaf;
        else if (std::string_view(family)=="structural_replacement") component.structural_replacement=leaf;
        else component.stair_replacement=leaf;
        (void)encode_phase_constraint_authoring_intent(component);
        result.push_back(std::move(component));
    }
    return result;
}

PhaseConstraintAuthoringIntent make_phase_constraint_authoring_intent(
    const DocumentSnapshot& source,const ConstraintAuthoringIntent& intent) {
    PhaseConstraintAuthoringIntent result{source.revision(),document_snapshot_digest(source),
        document_authoring_source_digest_v2(source),entity_map_digest(source.entities()),
        source.saved_revision_optional(),phase_constraint_authoring_selections(source.entities()),intent};
    (void)encode_phase_constraint_authoring_intent(result); return result;
}

Entities replay_phase_constraint_authoring(const Entities& source,const Json& proof) {
    const auto decoded=decode_phase_constraint_authoring_intent(proof);
    if (decoded.source_entities_digest!=entity_map_digest(source) ||
        decoded.phase_selections!=phase_constraint_authoring_selections(source))
        invalid("Phase constraint proof does not describe the actual source entities and saved choices");
    if (!decoded.ordinary_roof_edits.is_null()) {
        const auto edits=ordinary_roof_edits(decoded.ordinary_roof_edits, true);
        const auto partition=partition_phase_roof_geometry_edits(source,edits);
        if (partition.replacement || !partition.baseline_roof_edits.empty())
            invalid("Ordinary roof authoring requires actual ordinary/proposed membership");
        return replay_roof_edit_entities(source,edits);
    }
    if (!decoded.wall_demolition.is_null())
        return replay_phase_wall_demolition_authoring(source,decoded);
    if (!decoded.coordinated_replacements.is_null())
        return replay_coordinated(source,decoded);
    if (!decoded.coordinated_demolition.is_null())
        return replay_phase_coordinated_demolition(source,decoded);
    if (!decoded.opening_demolition.is_null())
        return replay_phase_opening_demolition_entities(source,
            decode_phase_opening_demolition_intent(decoded.opening_demolition));
    if (!decoded.roof_replacement.is_null())
        return replay_phase_roof_replacement_authoring(source,
            decode_phase_roof_replacement_authoring(decoded.roof_replacement));
    if (!decoded.slab_replacement.is_null())
        return replay_phase_slab_replacement_authoring(source,
            decode_phase_slab_replacement_authoring(decoded.slab_replacement));
    if (!decoded.slab_demolition.is_null())
        return replay_phase_slab_demolition_entities(source,
            decode_slab_demolition_intent(decoded.slab_demolition));
    if (!decoded.structural_replacement.is_null())
        return replay_phase_structural_replacement_authoring(source,
            decode_phase_structural_replacement_authoring(decoded.structural_replacement));
    if (!decoded.stair_demolition.is_null())
        return replay_phase_stair_demolition_entities(source,
            decode_stair_demolition_intent(decoded.stair_demolition));
    if (!decoded.stair_replacement.is_null())
        return replay_phase_stair_replacement_authoring(source,
            decode_phase_stair_replacement_authoring(decoded.stair_replacement));
    if (!decoded.stair_demolition_retirement.is_null())
        return replay_phase_stair_demolition_retirement_entities(source,
            decode_stair_demolition_retirement_intent(decoded.stair_demolition_retirement));
    // Phase nineteen follows complete replacement replay; ordinary phase
    // eighteen never gains a copied stage or baseline edit authority.
    return decoded.wall_replacement.is_null() ? reconstruct_active_phase_constraint_authoring(source,decoded.intent)
        : replay_phase_wall_replacement_authoring(source,decoded);
}

PhaseWallReplacementAuthoringPreview inspect_phase_coordinated_authoring(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent) {
    (void)encode_phase_constraint_authoring_intent(intent);
    if (!source.is_editable() || intent.expected_revision!=source.revision() ||
        intent.source_snapshot_digest!=document_snapshot_digest(source) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
        intent.source_entities_digest!=entity_map_digest(source.entities()) ||
        intent.source_saved_revision!=source.saved_revision_optional() ||
        intent.phase_selections!=phase_constraint_authoring_selections(source.entities()))
        invalid("Coordinated preview differs from the actual captured source");
    const auto lanes=coordinated(intent.coordinated_replacements,intent);
    if ((lanes.version!=2 && lanes.version!=3 && lanes.version!=4 && lanes.version!=5) || !lanes.wall || lanes.wall->wall_replacement.is_null())
        invalid("Coordinated preview requires inner version two through five with an actual wall replacement");
    auto preview=inspect_phase_wall_replacement_authoring(source,*lanes.wall);
    preview.edited_entities=replay_coordinated(source.entities(),intent,&preview.edited_entities,
        &preview.replacement.fresh_identity_ids);
    return preview;
}

Entities compose_architectural_family_candidates(const Entities& source,const std::vector<Entities>& candidates) {
    coordinated_map_budget(source);
    if (candidates.empty() || candidates.size()>5)
        invalid("Ordinary family composition requires one to five complete actual-source candidates");
    for (const auto& candidate:candidates) {
        coordinated_map_budget(candidate);
        if (candidate.size()!=source.size()) invalid("Ordinary family composition cannot create or remove actual owners");
        for (const auto& [key,entity]:source) {
            (void)entity;
            if (!candidate.contains(key)) invalid("Ordinary family composition requires exact actual source owner identities");
        }
    }
    auto result=source;
    for (const auto& [key,entity]:source) {
        std::vector<const Entity*> changes;
        for (const auto& candidate:candidates) if (!exact(entity,candidate.at(key))) changes.push_back(&candidate.at(key));
        if (changes.empty()) continue;
        if (entity.type=="assembly_model") result.at(key)=merge_hosted_instance_placements(entity,changes);
        else if (changes.size()==1) result.at(key)=*changes.front();
        else result.at(key)=compose_source_container(entity,changes);
    }
    coordinated_map_budget(result);
    const auto final_aliases=embedded_assembly_presentation_ids(result);
    for (const auto& [key,alias]:embedded_assembly_presentation_ids(source))
        if (final_aliases.at(key)!=alias) invalid("Ordinary family composition changed an actual source render alias");
    for (const auto& candidate:candidates) for (const auto& [key,alias]:embedded_assembly_presentation_ids(candidate))
        if (final_aliases.at(key)!=alias) invalid("Ordinary family composition changed a candidate render alias");
    validate_document_assembly_instances(result);
    return result;
}

namespace {
// Reflection promotes legacy annotation axes for the whole state. Compare
// independently admitted rows against the same codec-owned promoted baseline,
// so required sibling defaults are not mistaken for competing row edits.
// Only the state version and six absent symbol fields owned by upgrade_axes in
// presentation_transform change; encoding never replaces a row or envelope.
Entity promote_mixed_annotation_axes(Entity entity) {
    const auto state=decode_annotation_entity(entity);
    auto& raw=entity.properties.at("state");
    if (raw.at("version").get<int>()>=3) return entity;
    const auto encoded=encode_annotation_state(state,default_symbol_catalog());
    auto& symbols=raw.at("symbols");
    for (std::size_t i=0;i<symbols.size();++i)
        for (const auto* field:{"definition","pinned_svg","width_scale","depth_scale",
                               "flip_horizontal","flip_vertical"})
            if (!symbols[i].contains(field)) symbols[i][field]=encoded.at("symbols")[i].at(field);
    raw.at("version")=3;
    validate_annotation_entity(entity);
    return entity;
}

Entity merge_mixed_edit_annotation_rows(const Entity& source,const std::vector<const Entity*>& candidates) {
    validate_annotation_entity(source);
    const auto& version=source.properties.at("state").at("version");
    bool promoted=false;
    for (const auto* candidate:candidates) {
        validate_annotation_entity(*candidate);
        const auto& next=candidate->properties.at("state").at("version");
        if (exact_json(version,next)) continue;
        if ((version!=1 && version!=2) || next!=3)
            invalid("Mixed edit annotation has an unsupported state promotion");
        promoted=true;
    }
    if (!promoted) return merge_source_row_container(source,candidates,false,false,true);

    const auto baseline=promote_mixed_annotation_axes(source);
    std::vector<Entity> normalized;
    normalized.reserve(candidates.size());
    for (const auto* candidate:candidates) normalized.push_back(promote_mixed_annotation_axes(*candidate));
    std::vector<const Entity*> rows;
    rows.reserve(normalized.size());
    for (const auto& candidate:normalized) rows.push_back(&candidate);
    return merge_source_row_container(baseline,rows,false,false,true);
}

// Mixed edit maps have already passed their own typed row admission. Unlike
// historical hosted placement composition, their authority can include an
// independent root pose, parameters and complete raw instance suffixes.
Entity merge_mixed_edit_instance_rows(const Entity& source,const std::vector<const Entity*>& candidates,
    bool allow_legacy_assembly_promotion=false) {
    const auto& original=source.properties.at("model");
    (void)AssemblyModel::from_json(original);
    const auto& retained=original.at("instances");
    auto rows=retained;
    std::set<std::string,std::less<>> retained_ids,changed,suffix_ids;
    for (const auto& row:retained) retained_ids.insert(row.at("id").get<std::string>());
    Json suffix=Json::array();
    bool promoted=false;
    for (const auto* candidate:candidates) {
        const auto& model=candidate->properties.at("model");
        (void)AssemblyModel::from_json(model);
        auto envelope=*candidate;
        auto& normalized=envelope.properties.at("model");
        const bool promotion=!exact_json(model.at("schema"),original.at("schema"));
        if (promotion) {
            const auto before=original.at("schema").get<std::string>();
            const auto after=model.at("schema").get<std::string>();
            if (!((before=="sketch.assemblies.v3" || before=="sketch.assemblies.v4") &&
                    (after=="sketch.assemblies.v5" || after=="sketch.assemblies.v6" || after=="sketch.assemblies.v7")) &&
                !(before=="sketch.assemblies.v5" && (after=="sketch.assemblies.v6" || after=="sketch.assemblies.v7")) &&
                !(before=="sketch.assemblies.v6" && after=="sketch.assemblies.v7") &&
                !(allow_legacy_assembly_promotion && (before=="sketch.assemblies.v1" || before=="sketch.assemblies.v2") &&
                    after=="sketch.assemblies.v7"))
                invalid("Mixed edit catalog has an unsupported dialect change");
            promoted=true;
            normalized.at("schema")=original.at("schema");
            auto& types=normalized.at("types");
            const auto& source_types=original.at("types");
            if (types.size()!=source_types.size()) invalid("Mixed edit changed catalog type definitions");
            for (std::size_t i=0;i<types.size();++i) for (const auto* field:{"profiles","parts"})
                if (!source_types[i].contains(field) && types[i].contains(field) &&
                    types[i].at(field).is_array() && types[i].at(field).empty()) types[i].erase(field);
        }
        const auto& instances=model.at("instances");
        if (instances.size()<retained.size()) invalid("Mixed edit cannot erase actual catalog instance rows");
        for (std::size_t i=0;i<retained.size();++i) {
            const auto& before=retained[i];
            auto row=instances[i];
            const auto local_id=before.at("id").get<std::string>();
            if (row.at("id")!=before.at("id")) invalid("Mixed edit changed retained catalog instance identities or order");
            // Exactly the defaults recognized by historical catalog promotion.
            // Genuine independent root transforms and row parameters remain
            // complete typed consequences of the independently admitted leaf.
            if (promotion && !before.contains("root_transform") && row.contains("root_transform") &&
                row.at("root_transform").is_null() && row.contains("nested_overrides") &&
                row.at("nested_overrides").is_array() && row.at("nested_overrides").empty()) {
                row.erase("root_transform"); row.erase("nested_overrides");
            }
            if (promotion && before.contains("placement") && row.contains("placement")) {
                auto& placed=row.at("placement");
                const auto& placement=before.at("placement");
                if (!placement.contains("vertical_scale") && placed.contains("vertical_scale") &&
                    placed.at("vertical_scale")==1.0) placed.erase("vertical_scale");
                if (placement.at("translation_m").size()==2 && placed.at("translation_m").size()==3 &&
                    placed.at("translation_m")[2]==0.0) placed.at("translation_m").erase(placed.at("translation_m").begin()+2);
            }
            if (exact_json(before,row)) continue;
            if (!changed.insert(local_id).second && !exact_json(rows[i],row))
                invalid("Mixed edit leaves have conflicting consequences for one catalog instance row");
            rows[i]=std::move(row);
        }
        for (std::size_t i=retained.size();i<instances.size();++i) {
            const auto& row=instances[i];
            const auto local_id=row.at("id").get<std::string>();
            if (retained_ids.contains(local_id) || !suffix_ids.insert(local_id).second)
                invalid("Mixed edit catalog suffix repeats or reorders an instance identity");
            suffix.push_back(row);
        }
        normalized.at("instances")=retained;
        if (!exact(source,envelope)) invalid("Mixed edit changed catalog definitions or its retained envelope");
    }
    for (const auto& row:suffix) rows.push_back(row);
    auto result=source;
    result.properties.at("model").at("instances")=std::move(rows);
    if (promoted && (!changed.empty() || !suffix.empty())) result.properties.at("model").at("schema")="sketch.assemblies.v7";
    (void)AssemblyModel::from_json(result.properties.at("model"));
    return result;
}

using MixedEditIdentityOwners=std::map<std::string,std::set<std::string,std::less<>>,std::less<>>;

// Inventory only owned slots, never registry membership or presentation target
// references. Structural owner keys avoid delimiter aliases between views and
// children. Catalog instance bindings use their separate qualified namespace.
// This is a collision fence, not identity creation authority.
MixedEditIdentityOwners mixed_edit_identity_owners(const Entities& entities) {
    MixedEditIdentityOwners owners;
    const auto reserve=[&](const Json& token,const Json& owner) {
        if (!token.is_string() || token.get_ref<const std::string&>().empty())
            invalid("Mixed edit has an invalid owned identity");
        owners[token.get<std::string>()].insert(owner.dump());
    };
    for (const auto& [key,entity]:entities) {
        reserve(key,Json::array({key,"entity"}));
        const auto rows=[&](const Json& values,const Json& owner) {
            if (!values.is_array()) invalid("Mixed edit owned child collection must be an array");
            for (const auto& row:values) reserve(row.at("id"),owner);
        };
        const auto& p=entity.properties;
        // Catalog instance IDs have their own qualified (catalog, instance)
        // namespace, inventoried separately through the presentation bindings.
        if (entity.type==kAnnotationEntityType) {
            for (const auto* slot:{"labels","symbols"}) rows(p.at("state").at(slot),Json::array({key,slot}));
        } else if (entity.type==kSheetViewEntityType) {
            for (const auto& view:p.at("model").at("views")) if (view.contains("overlays"))
                rows(view.at("overlays"),Json::array({key,"overlays",view.at("id")}));
        } else if (entity.type=="roof" && p.contains("roof_openings"))
            rows(p.at("roof_openings"),Json::array({key,"roof_openings"}));
        else if ((entity.type=="wall" || entity.type=="slab") && p.contains("layers"))
            rows(p.at("layers"),Json::array({key,"layers"}));
        else if (entity.type=="stair") {
            for (const auto* slot:{"flights","landings"}) if (p.contains(slot)) rows(p.at(slot),Json::array({key,slot}));
        }
        if (can_recognize_boundary_entity_type(entity.type) || entity.type=="measurement_linework") {
            const auto topology_owner=Json::array({key,"topology"});
            const auto topology=[&](const Json& segments) {
                if (!segments.is_array()) invalid("Mixed edit retained topology must be an array");
                for (const auto& edge:segments) for (const auto* slot:{"segment_id","start_vertex_id","end_vertex_id"})
                    reserve(edge.at(slot),topology_owner);
            };
            const auto authoring=[&](const Json& value) {
                const auto decoded=decode_boundary_receipt_envelope(value);
                if (!decoded.supported()) invalid("Mixed edit retained construction topology is unsupported");
                for (const auto& edge:decoded.record->edges) {
                    reserve(edge.segment_id,topology_owner); reserve(edge.start_vertex_id,topology_owner);
                    reserve(edge.end_vertex_id,topology_owner);
                }
            };
            const auto edit=[&](const Json& value) {
                const auto decoded=decode_boundary_geometry_edit(value);
                if (decoded.kind!=BoundaryGeometryEditKind::redefine_boundary) reserve(decoded.target_id,topology_owner);
                if (!decoded.new_vertex_id.empty()) reserve(decoded.new_vertex_id,topology_owner);
                if (!decoded.new_segment_id.empty()) reserve(decoded.new_segment_id,topology_owner);
                if (decoded.kind==BoundaryGeometryEditKind::redefine_boundary) {
                    topology(decoded.replacement_segments);
                    if (!decoded.replacement_authoring.is_null()) authoring(decoded.replacement_authoring);
                    for (const auto& [group,mapping]:decoded.replacement_child_mapping.items()) {
                        (void)group;
                        for (const auto& [before,after]:mapping.items()) {
                            reserve(before,topology_owner); reserve(after,topology_owner);
                        }
                    }
                }
                // replacement_linework_sources, physical room descriptors,
                // dimensions and retired references refer to other owners.
            };
            if (entity.type=="measurement_linework") {
                const auto& model=p.at("model");
                const auto inspected=inspect_measurement_linework_model(model);
                if (inspected.format==MeasurementLineworkFormat::supported_v1 ||
                    inspected.format==MeasurementLineworkFormat::supported_v2 ||
                    inspected.format==MeasurementLineworkFormat::supported_v3 ||
                    inspected.format==MeasurementLineworkFormat::supported_v4 ||
                    inspected.format==MeasurementLineworkFormat::supported_v5 ||
                    inspected.format==MeasurementLineworkFormat::supported_v6) {
                    topology(model.at("segments"));
                    if (model.contains("operations")) for (const auto& operation:model.at("operations")) {
                        if (operation.at("type")=="edit") edit(operation.at("edit"));
                        else if (operation.at("type")=="vertex_batch")
                            for (const auto& value:operation.at("edits")) edit(value);
                    }
                }
            } else {
                if (inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1)
                    topology(p.at("segments"));
                if (p.contains("boundary_authoring")) authoring(p.at("boundary_authoring"));
                const auto found=entity.extensions.find("boundary_geometry_derivation");
                if (found!=entity.extensions.end() && found->is_object() && found->contains("version") &&
                    (found->at("version")==1 || found->at("version")==2)) {
                    const auto& derivation=*found;
                    if (derivation.at("version")==1) authoring(derivation.at("source_boundary_authoring"));
                    else topology(derivation.at("source_boundary").at("segments"));
                    for (const auto& operation:derivation.at("operations")) {
                        const auto& kind=operation.at("kind");
                        const auto& value=operation.at("value");
                        if (kind=="geometry_edit") edit(value);
                        else if (kind=="vertex_batch") for (const auto& row:value) edit(row);
                        else if (kind=="wall_merge") {
                            topology(value.at("segments")); reserve(value.at("vertex_id"),topology_owner);
                        } else if (kind=="physical_room_wall_merge") {
                            topology(value.at("segments"));
                            for (const auto& vertex:value.at("seam_vertex_ids")) reserve(vertex,topology_owner);
                        } else if (kind=="physical_room_wall_split") {
                            topology(value.at("segments"));
                            for (const auto& insertion:value.at("insertions"))
                                for (const auto* slot:{"segment_id","new_vertex_id","new_segment_id"})
                                    reserve(insertion.at(slot),topology_owner);
                        }
                    }
                }
            }
            // No recursive scan of properties or extensions: measured source
            // lineage/group segment IDs and opaque metadata are references,
            // not children owned by this boundary or stroke.
        }
    }
    return owners;
}

// Only the typed replay's actual removals qualify this normalization. Restore
// no removed rows when joining selected annotation children with area callouts.
Entity merge_reviewed_geometry_annotation_rows(const Entity& original,const Entity& geometry,
    const std::vector<const Entity*>& ordinary,const std::set<std::string,std::less<>>& removed_targets) {
    validate_annotation_entity(geometry);
    using Target=std::pair<std::string,std::string>;
    std::set<Target> surviving;
    for (const auto& row:geometry.properties.at("state").at("overrides"))
        surviving.emplace(row.at("target_id").get<std::string>(),row.at("target_kind").get<std::string>());
    const auto clean=[&](Entity entity) {
        validate_annotation_entity(entity);
        auto& rows=entity.properties.at("state").at("overrides");
        rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
            const auto target=row.at("target_id").template get<std::string>();
            return removed_targets.contains(target) &&
                !surviving.contains({target,row.at("target_kind").template get<std::string>()});
        }),rows.end());
        return entity;
    };
    auto baseline=clean(original);
    auto result=geometry;
    bool promoted=geometry.properties.at("state").at("version").get<int>()>=3;
    for (const auto* lane:ordinary)
        promoted=promoted || lane->properties.at("state").at("version").get<int>()>=3;
    if (promoted) {
        baseline=promote_mixed_annotation_axes(std::move(baseline));
        result=promote_mixed_annotation_axes(std::move(result));
    }
    const auto retain_envelope=[&](const Entity& lane) {
        auto envelope=lane;
        for (const auto* table:{"labels","symbols","overrides"})
            envelope.properties.at("state").at(table)=baseline.properties.at("state").at(table);
        envelope.properties.at("state").at("version")=baseline.properties.at("state").at("version");
        if (!exact(baseline,envelope))
            invalid("Reviewed geometry annotation changed fields outside its source row authority");
    };
    retain_envelope(result);
    // Verify geometry's only added rows through the source-callout codec before
    // any ordinary replacement suffix is considered. Its proof owns neither
    // copied labels/symbols nor arbitrary added override targets.
    result=merge_selection_annotation_entities(baseline,result,baseline,
        AnnotationMergeMode::source_callout_geometry);
    std::map<std::string,Json,std::less<>> suffixes;
    for (const auto* lane:ordinary) {
        auto normalized=clean(*lane);
        if (promoted) normalized=promote_mixed_annotation_axes(std::move(normalized));
        retain_envelope(normalized);
        for (const auto* table:{"labels","symbols","overrides"}) {
            const auto identity=[&](const Json& row) -> Target {
                if (std::string_view(table)=="overrides")
                    return {row.at("target_id").get<std::string>(),row.at("target_kind").get<std::string>()};
                return {row.at("id").get<std::string>(),{}};
            };
            const auto& retained=baseline.properties.at("state").at(table);
            std::map<Target,std::size_t> source_rows;
            for (std::size_t index=0;index<retained.size();++index)
                if (!source_rows.emplace(identity(retained[index]),index).second)
                    invalid("Reviewed geometry annotation source row identity is ambiguous");
            auto prefix=Json::array();
            auto& suffix=suffixes.try_emplace(table,Json::array()).first->second;
            bool appended=false;
            std::size_t next{};
            for (const auto& row:normalized.properties.at("state").at(table)) {
                const auto found=source_rows.find(identity(row));
                if (found==source_rows.end()) { appended=true; suffix.push_back(row); }
                else {
                    if (appended || found->second!=next++)
                        invalid("Reviewed ordinary annotation changed source row identities or order");
                    prefix.push_back(row);
                }
            }
            if (next!=retained.size())
                invalid("Reviewed ordinary annotation cannot remove a surviving source row");
            normalized.properties.at("state").at(table)=std::move(prefix);
        }
        result=merge_selection_annotation_entities(baseline,result,normalized,
            AnnotationMergeMode::source_callout_geometry);
    }
    // Complete ordinary leaf replay alone admits these exact copied rows.
    // Coalesce identical references only; incompatible same-identity rows refuse.
    // Fresh child ownership across lanes was already checked by the composer.
    for (const auto& [table,suffix]:suffixes) {
        auto& rows=result.properties.at("state").at(table);
        const auto identity=[&](const Json& row) -> Target {
            if (table=="overrides")
                return {row.at("target_id").get<std::string>(),row.at("target_kind").get<std::string>()};
            return {row.at("id").get<std::string>(),{}};
        };
        std::map<Target,std::size_t> destinations;
        for (std::size_t index=0;index<rows.size();++index)
            if (!destinations.emplace(identity(rows[index]),index).second)
                invalid("Reviewed geometry annotation destination identity is ambiguous");
        for (const auto& row:suffix) {
            const auto [found,inserted]=destinations.emplace(identity(row),rows.size());
            if (inserted) rows.push_back(row);
            else if (!exact_json(rows[found->second],row))
                invalid("Reviewed ordinary annotation suffix conflicts with an admitted destination");
        }
    }
    validate_annotation_entity(result);
    return result;
}

// Validate the review's exact deletion-only catalog consequence, then retain
// the historical edit merger's typed row/promotion/suffix rules on survivors.
// Ordinary leaves cannot edit a retired row or remove any additional row.
Entity merge_stair_review_instance_rows(const Entity& source,const Entity& review,
    const std::vector<const Entity*>& ordinary,
    const std::set<std::pair<std::string,std::string>>& retired_bindings,
    bool allow_legacy_assembly_promotion=false) {
    const auto& retained=source.properties.at("model").at("instances");
    std::map<std::string,const Json*,std::less<>> retained_rows;
    for (const auto& row:retained)
        if (!retained_rows.emplace(row.at("id").get<std::string>(),&row).second)
            invalid("Reviewed stair catalog has ambiguous source instance identities");
    auto deletion=source;
    auto& surviving=deletion.properties.at("model").at("instances");
    surviving.erase(std::remove_if(surviving.begin(),surviving.end(),[&](const auto& row) {
        return retired_bindings.contains({source.id,row.at("id").template get<std::string>()});
    }),surviving.end());
    const auto baseline=merge_demolition_catalog(source,{&deletion},true,true);
    std::vector<Entity> normalized;
    normalized.reserve(ordinary.size());
    for (const auto* lane:ordinary) {
        normalized.push_back(*lane);
        auto& rows=normalized.back().properties.at("model").at("instances");
        rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
            const auto id=row.at("id").template get<std::string>();
            if (!retired_bindings.contains({source.id,id})) return false;
            const auto original=retained_rows.find(id);
            if (original==retained_rows.end() || !exact_json(*original->second,row))
                invalid("Reviewed stair retirement overlaps an ordinary catalog row edit");
            return true;
        }),rows.end());
    }
    std::vector<const Entity*> changes{&review};
    for (const auto& lane:normalized) changes.push_back(&lane);
    return merge_mixed_edit_instance_rows(baseline,changes,allow_legacy_assembly_promotion);
}

struct PhasePropertyCompositionRetirements {
    std::set<std::string,std::less<>> erased;
    std::set<std::pair<std::string,std::string>> bindings;
};

struct AssemblyPropertyRowAdmission {
    std::map<AssemblyPhaseRoot, Json> changed_rows;
    std::set<std::string, std::less<>> catalogs;
};

Entity merge_component_membership_rows(const Entity& original, const std::vector<const Entity*>& changes) {
    const auto saved=AssemblyPhaseMembership::from_json(original.properties.at("model"));
    std::vector<AssemblyPhaseMembership> lanes;
    for (const auto* change:changes) {
        auto envelope=*change;
        envelope.properties.at("model")=original.properties.at("model");
        if (!exact(original,envelope)) invalid("Component membership composition changed its retained carrier");
        auto lane=AssemblyPhaseMembership::from_json(change->properties.at("model"));
        if (lane.registry_id()!=saved.registry_id()) invalid("Component membership composition changed its actual registry");
        lanes.push_back(std::move(lane));
    }
    // A source row omitted by an independently admitted retirement stays
    // omitted; fresh rows contributed by disjoint replays append once. The
    // immutable codec rejects conflicting baseline/proposal partitions.
    const auto combine=[](const std::vector<AssemblyPhaseRoot>& before,
        const std::vector<std::vector<AssemblyPhaseRoot>>& contributions) {
        std::set<AssemblyPhaseRoot> result(before.begin(),before.end());
        const std::set<AssemblyPhaseRoot> source(before.begin(),before.end());
        for (const auto& rows:contributions) {
            const std::set<AssemblyPhaseRoot> present(rows.begin(),rows.end());
            for (const auto& row:before) if (!present.contains(row)) result.erase(row);
            for (const auto& row:rows) if (!source.contains(row)) result.insert(row);
        }
        return std::vector<AssemblyPhaseRoot>(result.begin(),result.end());
    };
    std::vector<std::vector<AssemblyPhaseRoot>> roots,baselines;
    std::set<std::string,std::less<>> alternative_ids;
    for (const auto& alternative:saved.alternatives()) alternative_ids.insert(alternative.alternative_id);
    for (const auto& lane:lanes) {
        roots.push_back(lane.roots()); baselines.push_back(lane.baseline_roots());
        for (const auto& alternative:lane.alternatives()) alternative_ids.insert(alternative.alternative_id);
    }
    std::vector<AssemblyPhaseAlternative> alternatives;
    for (const auto& id:alternative_ids) {
        const auto find=[&](const AssemblyPhaseMembership& membership) -> const AssemblyPhaseAlternative* {
            const auto row=std::find_if(membership.alternatives().begin(),membership.alternatives().end(),
                [&](const auto& value) { return value.alternative_id==id; });
            return row==membership.alternatives().end() ? nullptr : &*row;
        };
        const auto* before=find(saved);
        std::vector<std::vector<AssemblyPhaseRoot>> demolished,proposed;
        for (const auto& lane:lanes) {
            const auto* row=find(lane);
            if (before && !row) invalid("Component membership composition removed a source alternative");
            demolished.push_back(row ? row->demolished_roots : std::vector<AssemblyPhaseRoot>{});
            proposed.push_back(row ? row->proposed_roots : std::vector<AssemblyPhaseRoot>{});
        }
        alternatives.push_back({id,combine(before ? before->demolished_roots : std::vector<AssemblyPhaseRoot>{},demolished),
            combine(before ? before->proposed_roots : std::vector<AssemblyPhaseRoot>{},proposed)});
    }
    const auto merged=AssemblyPhaseMembership::create(saved.registry_id(),combine(saved.roots(),roots),
        combine(saved.baseline_roots(),baselines),std::move(alternatives));
    auto result=original;
    result.properties.at("model")=retain_assembly_phase_membership_source(original.properties.at("model"),merged);
    return result;
}

Entities compose_mixed_selection_edit_candidates_impl(const Entities& actual,
    const std::vector<Entities>& independently_admitted_candidates,
    const std::set<std::string,std::less<>>* retired_rooms=nullptr,
    const std::set<std::string,std::less<>>* removed_annotation_targets=nullptr,
    const StairPropertyReviewPreparation* stair_review=nullptr,
    const std::set<std::string,std::less<>>* assembly_promoted_catalogs=nullptr,
    const PhasePropertyCompositionRetirements* phase_properties=nullptr,
    const AssemblyPropertyRowAdmission* assembly_row_edits=nullptr) {
    coordinated_map_budget(actual);
    if (independently_admitted_candidates.empty() || independently_admitted_candidates.size()>
        (phase_properties ? 2u : stair_review ? 6u : 5u))
        invalid("Mixed edit composition requires one to five independently admitted actual-source candidates");
    for (const auto& candidate:independently_admitted_candidates) coordinated_map_budget(candidate);
    const auto original_owners=mixed_edit_identity_owners(actual);
    const auto original_aliases=embedded_assembly_presentation_ids(actual);
    std::set<std::string,std::less<>> reviewed_erased;
    std::set<std::pair<std::string,std::string>> reviewed_retired_bindings;
    if (phase_properties) {
        reviewed_erased=phase_properties->erased;
        reviewed_retired_bindings=phase_properties->bindings;
    }
    if (stair_review) {
        reviewed_erased.insert(stair_review->erased_entity_ids().begin(),stair_review->erased_entity_ids().end());
        const auto review_aliases=embedded_assembly_presentation_ids(stair_review->entities());
        const std::set<std::string,std::less<>> rails(stair_review->retired_rail_ids().begin(),stair_review->retired_rail_ids().end());
        for (const auto& [catalog,entity]:actual) if (entity.type=="assembly_model")
            for (const auto& row:entity.properties.at("model").at("instances")) if (row.contains("placement") &&
                rails.contains(row.at("placement").at("host_entity_id").get<std::string>()))
                reviewed_retired_bindings.emplace(catalog,row.at("id").get<std::string>());
        for (const auto& [binding,alias]:original_aliases) {
            const auto next=review_aliases.find(binding);
            if (reviewed_retired_bindings.contains(binding)) {
                if (next!=review_aliases.end()) invalid("Reviewed stair replay retained an explicitly retired hosted row");
                continue;
            }
            if (next==review_aliases.end())
                invalid("Reviewed stair retired a catalog row outside explicit rail authority");
            if (next->second!=alias) invalid("Reviewed stair changed a surviving qualified render alias");
        }
        for (const auto& [binding,alias]:review_aliases) {
            (void)alias;
            if (!original_aliases.contains(binding)) invalid("Reviewed stair introduced a fresh qualified catalog row");
        }
    }
    std::set<std::string,std::less<>> source_aliases;
    for (const auto& [key,alias]:original_aliases) { (void)key; source_aliases.insert(alias); }
    std::set<std::string,std::less<>> fresh;
    std::set<std::pair<std::string,std::string>> fresh_instances;
    for (const auto& candidate:independently_admitted_candidates) {
        std::set<std::string,std::less<>> lane_fresh;
        for (const auto& [token,owners]:mixed_edit_identity_owners(candidate)) {
            const auto found=original_owners.find(token);
            if (found!=original_owners.end()) {
                for (const auto& owner:owners) if (!found->second.contains(owner))
                    invalid("Mixed edit reuses an actual owned identity for a different owner");
                continue;
            }
            if (stair_review && &candidate==&independently_admitted_candidates.front())
                invalid("Reviewed stair replay introduced a fresh owned identity");
            if (owners.size()!=1 || source_aliases.contains(token))
                invalid("Mixed edit fresh identity is ambiguous or reuses an actual render alias");
            lane_fresh.insert(token);
        }
        for (const auto& [binding,alias]:embedded_assembly_presentation_ids(candidate)) {
            if (original_aliases.contains(binding)) continue;
            if (!fresh_instances.insert(binding).second)
                invalid("Mixed edit fresh qualified catalog instance destinations overlap between leaves");
            if (fresh_instances.size()+fresh.size()>4096) invalid("Mixed edit fresh identity budget exceeded");
            if (source_aliases.contains(alias) || original_owners.contains(alias) || lane_fresh.contains(alias))
                invalid("Mixed edit fresh render alias collides with an owned identity");
            lane_fresh.insert(alias);
        }
        for (const auto& token:lane_fresh) {
            if (!fresh.insert(token).second) invalid("Mixed edit fresh destinations overlap between leaves");
            if (fresh.size()+fresh_instances.size()>4096) invalid("Mixed edit fresh identity budget exceeded");
        }
    }
    std::set<std::string,std::less<>> retained_baselines;
    for (const auto& [key,entity]:actual) if (entity.type=="model_phases") {
        (void)key;
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        if (model.active_alternative() || ((stair_review || (phase_properties &&
            (!phase_properties->erased.empty() || !phase_properties->bindings.empty()))) && !model.alternatives().empty()))
            retained_baselines.insert(model.baseline_ids().begin(),model.baseline_ids().end());
    }
    const auto inactive=(stair_review || phase_properties) ? constraint_phase_scope(actual).inactive_owner_ids :
        std::set<std::string,std::less<>>{};
    auto result=actual;
    for (const auto& [key,original]:actual) {
        std::vector<const Entity*> changes;
        std::size_t erased{};
        for (const auto& candidate:independently_admitted_candidates) {
            const auto found=candidate.find(key);
            if (found==candidate.end()) ++erased;
            else if (!exact(original,found->second) && std::none_of(changes.begin(),changes.end(),
                [&](const auto* previous) { return exact(*previous,found->second); })) changes.push_back(&found->second);
        }
        if (erased) {
            if (original.required || retained_baselines.contains(key) || inactive.contains(key) || !changes.empty())
                invalid("Mixed edit erased a required or baseline owner, or overlaps an upsert");
            const bool dimension=can_recognize_boundary_dimension_entity_type(original.type) &&
                decode_boundary_dimension_entity(original).supported();
            const bool constraint=original.type=="constraint" && decode_constraint_entity(original).supported();
            const bool annotation=original.type==kAnnotationEntityType;
            if (annotation) validate_annotation_entity(original);
            const bool reviewed_room=retired_rooms && retired_rooms->contains(key);
            const bool reviewed_retirement=(stair_review || phase_properties) && reviewed_erased.contains(key);
            if ((stair_review || phase_properties) && !reviewed_retirement)
                invalid("Reviewed stair composition erased an owner outside its authenticated removal closure");
            if (!dimension && !constraint && !annotation && !reviewed_room && !reviewed_retirement)
                invalid("Mixed edit erasure lacks a supported reference-cleanup owner");
            result.erase(key);
            continue;
        }
        if (changes.empty()) continue;
        if (((stair_review || phase_properties) && inactive.contains(key) &&
             !(assembly_row_edits && original.type=="assembly_model" && assembly_row_edits->catalogs.contains(key))) ||
            (stair_review && original.required &&
            original.type!="assembly_model" && original.type!=kAnnotationEntityType &&
            original.type!="model_phases" && original.type!=kSheetViewEntityType))
            invalid("Reviewed stair composition changed a required or inactive physical owner");
        if (retained_baselines.contains(key) && original.type!="assembly_model")
            invalid("Mixed edit changed an actual retained baseline owner");
        if (original.type=="assembly_model") {
            if (phase_properties && !reviewed_retired_bindings.empty()) {
                auto deletion=original;
                auto& rows=deletion.properties.at("model").at("instances");
                rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
                    return reviewed_retired_bindings.contains({key,row.at("id").template get<std::string>()});
                }),rows.end());
                result.at(key)=merge_stair_review_instance_rows(original,deletion,changes,reviewed_retired_bindings,
                    assembly_promoted_catalogs && assembly_promoted_catalogs->contains(key));
            } else if (stair_review) {
                std::vector<const Entity*> ordinary;
                for (std::size_t index=1;index<independently_admitted_candidates.size();++index)
                    ordinary.push_back(&independently_admitted_candidates[index].at(key));
                result.at(key)=merge_stair_review_instance_rows(original,stair_review->entities().at(key),
                    ordinary,reviewed_retired_bindings,assembly_promoted_catalogs && assembly_promoted_catalogs->contains(key));
            } else result.at(key)=merge_mixed_edit_instance_rows(original,changes,
                assembly_promoted_catalogs && assembly_promoted_catalogs->contains(key));
        }
        else if (original.type==kAnnotationEntityType) {
            if (removed_annotation_targets) {
                std::vector<const Entity*> ordinary;
                for (std::size_t index=1;index<independently_admitted_candidates.size();++index)
                    ordinary.push_back(&independently_admitted_candidates[index].at(key));
                result.at(key)=merge_reviewed_geometry_annotation_rows(original,
                    independently_admitted_candidates.front().at(key),ordinary,*removed_annotation_targets);
            } else result.at(key)=merge_mixed_edit_annotation_rows(original,changes);
        }
        else if (original.type=="model_phases")
            result.at(key)=merge_source_row_container(original,changes,removed_annotation_targets!=nullptr,false,true);
        else if (original.type==assembly_phase_membership_entity_type && phase_properties && assembly_row_edits)
            result.at(key)=merge_component_membership_rows(original,changes);
        else if (original.type==kSheetViewEntityType)
            result.at(key)=removed_annotation_targets
                ? merge_demolition_row_container(original,changes,false,true,true)
                : merge_source_row_container(original,changes,false,false,true);
        else if (changes.size()==1) result.at(key)=*changes.front();
        else invalid("Mixed edit leaves have conflicting consequences for one physical owner");
    }
    for (const auto& candidate:independently_admitted_candidates) for (const auto& [key,entity]:candidate)
        if (!actual.contains(key) && !result.emplace(key,entity).second)
            invalid("Mixed edit fresh physical owners overlap");
    for (const auto& [key,entity]:actual) if (entity.type=="assembly_model") {
        const auto& before=entity.properties.at("model").at("instances");
        const auto& after=result.at(key).properties.at("model").at("instances");
        std::map<std::string,const Json*,std::less<>> retained_rows;
        if (stair_review || phase_properties) for (const auto& row:after) retained_rows.emplace(row.at("id").get<std::string>(),&row);
        for (std::size_t i=0;i<before.size();++i) if (before[i].contains("placement") &&
            (retained_baselines.contains(before[i].at("placement").at("host_entity_id").get<std::string>()) ||
             ((stair_review || phase_properties) && inactive.contains(before[i].at("placement").at("host_entity_id").get<std::string>())))) {
            // Only the independently reconstructed component lane may edit a
            // retained-host row. Its complete exact result must survive every
            // other lane; no host visibility or supplied raw map grants this.
            const AssemblyPhaseRoot binding{key,before[i].at("id").get<std::string>()};
            if (assembly_row_edits && assembly_row_edits->changed_rows.contains(binding)) {
                const auto expected=assembly_row_edits->changed_rows.find(binding);
                const auto retained=std::find_if(after.begin(),after.end(),[&](const auto& row) {
                    return row.at("id")==binding.second;
                });
                if (retained==after.end() || !exact_json(*retained,expected->second))
                    invalid("Mixed edit changed an independently admitted component row");
                continue;
            }
            if (stair_review || phase_properties) {
                const auto retained=retained_rows.find(before[i].at("id").get<std::string>());
                if (retained==retained_rows.end() || !exact_json(before[i],*retained->second))
                    invalid("Mixed edit changed an actual retained baseline hosted row");
            } else if (i>=after.size() || !exact_json(before[i],after[i]))
                invalid("Mixed edit changed an actual retained baseline hosted row");
        }
    }
    if (assembly_row_edits) for (const auto& [binding,expected]:assembly_row_edits->changed_rows) {
        const auto& rows=result.at(binding.first).properties.at("model").at("instances");
        const auto retained=std::find_if(rows.begin(),rows.end(),[&](const auto& row) { return row.at("id")==binding.second; });
        if (retained==rows.end() || !exact_json(*retained,expected))
            invalid("Mixed edit lost an independently admitted component row");
    }
    coordinated_map_budget(result);
    const auto final_aliases=embedded_assembly_presentation_ids(result);
    const auto preserve_aliases=[&](const auto& aliases) {
        for (const auto& [binding,alias]:aliases) {
            const auto found=final_aliases.find(binding);
            if ((stair_review || phase_properties) && reviewed_retired_bindings.contains(binding)) {
                if (found!=final_aliases.end()) invalid("Reviewed stair composition restored a retired catalog row");
                continue;
            }
            if (found==final_aliases.end() || found->second!=alias)
                invalid("Mixed edit changed a source or independently admitted leaf render alias");
        }
    };
    preserve_aliases(original_aliases);
    for (const auto& candidate:independently_admitted_candidates)
        preserve_aliases(embedded_assembly_presentation_ids(candidate));
    std::set<std::string,std::less<>> members;
    for (const auto& [key,entity]:result) if (entity.type=="model_phases") {
        (void)key;
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& member:model.entity_ids())
            if (!result.contains(member) || !is_model_phase_entity_type(result.at(member).type) || !members.insert(member).second)
                invalid("Mixed edit phase membership is missing, unsupported or overlapping");
    }
    (void)constraint_phase_scope(result);
    validate_document_assembly_instances(result);
    if (stair_review || phase_properties) validate_stair_attachment_state(result);
    return result;
}

bool reviewed_geometry_owner(const Entity& entity,const Entities& actual,const Entities& geometry) {
    if (entity.type=="wall" || is_physical_wall_room(entity) || entity.type=="constraint") return true;
    if (!can_recognize_boundary_dimension_entity_type(entity.type)) return false;
    const auto dimension=decode_boundary_dimension_entity(entity);
    // Unknown dimensions stay opaque; composition cannot supply new authority.
    if (!dimension.supported()) return true;
    if (dimension.dimension->kind==BoundaryDimensionKind::wall_axis_length ||
        dimension.dimension->kind==BoundaryDimensionKind::corner_window_leg_length) return true;
    for (const auto* map:{&actual,&geometry}) {
        const auto owner=map->find(dimension.dimension->boundary_id);
        if (owner!=map->end() && (owner->second.type=="wall" || is_physical_wall_room(owner->second))) return true;
    }
    return false;
}
} // namespace

Entities compose_mixed_selection_edit_candidates(const Entities& actual,
    const std::vector<Entities>& independently_admitted_candidates) {
    return compose_mixed_selection_edit_candidates_impl(actual,independently_admitted_candidates);
}

void validate_selection_edit_fresh_identity_tokens(const Entities& retained,
    const std::set<std::string,std::less<>>& fresh) {
    if (fresh.empty()) return;
    coordinated_map_budget(retained);
    const auto owned=mixed_edit_identity_owners(retained);
    for (const auto& token:fresh) if (owned.contains(token))
        invalid("Reviewed room identity was already owned in retained history: "+token);
    for (const auto& [binding,alias]:embedded_assembly_presentation_ids(retained)) {
        (void)binding;
        if (fresh.contains(alias))
            invalid("Reviewed room identity reuses a retained component render alias: "+alias);
    }
}

Entities compose_reviewed_geometry_selection_edit_candidates(const Entities& actual,
    const ReplayedPhysicalWallRoomReview& independently_replayed_geometry,
    const std::vector<Entities>& independently_admitted_ordinary_candidates) {
    coordinated_map_budget(actual);
    const auto& geometry=independently_replayed_geometry.entities;
    coordinated_map_budget(geometry);
    if (independently_admitted_ordinary_candidates.size()>4 || independently_replayed_geometry.retired_room_ids.size()>4096)
        invalid("Reviewed geometry composition exceeds its actual-source lane or retirement budget");
    std::set<std::string,std::less<>> retired,removed_targets;
    for (const auto& room_id:independently_replayed_geometry.retired_room_ids) {
        const auto room=actual.find(room_id);
        if (!retired.insert(room_id).second || room==actual.end() || room->second.id!=room_id ||
            !is_physical_wall_room(room->second) || geometry.contains(room_id) ||
            validate_physical_wall_room_descriptor(room->second))
            invalid("Reviewed geometry retirement requires a unique actual supported physical room absent from replay");
        (void)decode_identified_boundary_entity(room->second);
        removed_targets.insert(room_id);
    }
    for (const auto& [key,entity]:actual) if (!geometry.contains(key) &&
        ((entity.type=="constraint" && decode_constraint_entity(entity).supported()) ||
         (can_recognize_boundary_dimension_entity_type(entity.type) && decode_boundary_dimension_entity(entity).supported())))
        removed_targets.insert(key);
    for (const auto& lane:independently_admitted_ordinary_candidates) {
        coordinated_map_budget(lane);
        for (const auto& [key,entity]:actual) {
            const auto next=lane.find(key);
            if ((reviewed_geometry_owner(entity,actual,geometry) ||
                (next!=lane.end() && reviewed_geometry_owner(next->second,actual,geometry))) &&
                (next==lane.end() || !exact(entity,next->second)))
                invalid("Ordinary reviewed-selection lane changed a protected geometry owner");
        }
        for (const auto& [key,entity]:lane) if (!actual.contains(key) && reviewed_geometry_owner(entity,actual,geometry))
            invalid("Ordinary reviewed-selection lane introduced a protected geometry owner");
    }
    std::vector<Entities> lanes;
    lanes.reserve(1+independently_admitted_ordinary_candidates.size());
    lanes.push_back(geometry);
    lanes.insert(lanes.end(),independently_admitted_ordinary_candidates.begin(),independently_admitted_ordinary_candidates.end());
    auto result=compose_mixed_selection_edit_candidates_impl(actual,lanes,&retired,&removed_targets);
    const auto retain_geometry=[&](const Entities& owners) {
        for (const auto& [key,entity]:owners) if (reviewed_geometry_owner(entity,actual,geometry)) {
            const auto expected=geometry.find(key);
            const auto final=result.find(key);
            if ((expected==geometry.end())!=(final==result.end()) ||
                (expected!=geometry.end() && !exact(expected->second,final->second)))
                invalid("Reviewed selection composition changed an exact typed geometry consequence");
        }
    };
    retain_geometry(actual);
    retain_geometry(geometry);
    retain_geometry(result);
    return result;
}

namespace {
Entities compose_stair_property_review_selection_edit_candidates_impl(const DocumentSnapshot& source,
    const StairPropertyReviewPreparation& reviewed,
    const std::vector<Entities>& independently_admitted_ordinary_candidates,
    const std::set<std::string,std::less<>>* assembly_promoted_catalogs=nullptr,
    const AssemblyPropertyRowAdmission* assembly_row_edits=nullptr) {
    const auto& actual=source.entities();
    coordinated_map_budget(actual);
    if (independently_admitted_ordinary_candidates.size()>5)
        invalid("Reviewed stair composition exceeds its actual-source lane budget");
    // A sealed preparation is evidence, not authority from another snapshot.
    // Reconstruct its closed request against the full original source/history.
    const auto replayed=replay_stair_property_review_preparation(source,reviewed.request());
    const auto& review=replayed.entities();
    coordinated_map_budget(review);
    std::set<std::string,std::less<>> erased(replayed.erased_entity_ids().begin(),replayed.erased_entity_ids().end());
    std::set<std::string,std::less<>> removed_targets=erased;
    for (const auto& [key,entity]:actual) {
        (void)entity;
        if (erased.contains(key)!=!review.contains(key))
            invalid("Reviewed stair replay removal closure does not match its complete result");
    }
    for (const auto& [key,entity]:review) {
        (void)entity;
        if (!actual.contains(key)) invalid("Reviewed stair replay introduced a fresh physical owner");
    }
    const auto review_aliases=embedded_assembly_presentation_ids(review);
    for (const auto& [binding,alias]:embedded_assembly_presentation_ids(actual))
        if (!review_aliases.contains(binding)) removed_targets.insert(alias);
    for (const auto& lane:independently_admitted_ordinary_candidates) {
        coordinated_map_budget(lane);
        for (const auto& [key,entity]:actual) if (entity.type=="stair" || entity.type=="railing") {
            const auto next=lane.find(key);
            // Matching explicit retirement is the only ordinary omission of a
            // physical stair-family member that can coalesce with the review.
            if (next==lane.end()) {
                if (!erased.contains(key)) invalid("Ordinary reviewed-stair lane erased an unreviewed physical owner");
            } else if (!exact(entity,next->second))
                invalid("Ordinary reviewed-stair lane changed a protected stair or railing");
        }
        for (const auto& [key,entity]:lane) if (!actual.contains(key) &&
            (entity.type=="stair" || entity.type=="railing"))
            invalid("Ordinary reviewed-stair lane introduced a stair or railing");
    }
    std::vector<Entities> lanes;
    lanes.reserve(1+independently_admitted_ordinary_candidates.size());
    lanes.push_back(review);
    lanes.insert(lanes.end(),independently_admitted_ordinary_candidates.begin(),independently_admitted_ordinary_candidates.end());
    auto result=compose_mixed_selection_edit_candidates_impl(actual,lanes,nullptr,&removed_targets,&replayed,
        assembly_promoted_catalogs,nullptr,assembly_row_edits);
    for (const auto& [key,entity]:actual) {
        const auto expected=review.find(key);
        if (erased.contains(key)!=!result.contains(key))
            invalid("Reviewed stair composition changed its exact authenticated removal closure");
        if (expected==review.end()) {
            if (result.contains(key)) invalid("Reviewed stair composition restored a retired source owner");
        } else if (!exact(entity,expected->second) && entity.type!="assembly_model" &&
            entity.type!=kAnnotationEntityType && entity.type!="model_phases" && entity.type!=kSheetViewEntityType) {
            const auto final=result.find(key);
            if (final==result.end() || !exact(expected->second,final->second))
                invalid("Reviewed stair composition changed an exact replayed physical consequence");
        }
    }
    return result;
}
} // namespace

Entities compose_stair_property_review_selection_edit_candidates(const DocumentSnapshot& source,
    const StairPropertyReviewPreparation& reviewed,const std::vector<Entities>& ordinary) {
    return compose_stair_property_review_selection_edit_candidates_impl(source,reviewed,ordinary);
}

Entities compose_assembly_property_selection_edit_candidates(const DocumentSnapshot& source,
    const AssemblyInstancePropertyEditPreparation& assemblies,const std::vector<Entities>& ordinary,
    const StairPropertyReviewPreparation* stair_review) {
    if (ordinary.size()>4) invalid("Assembly Properties composition exceeds its source lane budget");
    const auto replayed=replay_assembly_instance_property_edit_preparation(source,assemblies.request());
    std::set<std::string,std::less<>> promoted;
    for (const auto& [id,entity]:source.entities()) if (entity.type=="assembly_model") {
        const auto& before=entity.properties.at("model").at("schema");
        const auto& after=replayed.preview_entities().at(id).properties.at("model").at("schema");
        if ((before=="sketch.assemblies.v1" || before=="sketch.assemblies.v2") && after=="sketch.assemblies.v7")
            promoted.insert(id);
    }
    auto leaves=ordinary;
    leaves.push_back(replayed.preview_entities());
    if (stair_review)
        return compose_stair_property_review_selection_edit_candidates_impl(source,*stair_review,leaves,&promoted);
    return compose_mixed_selection_edit_candidates_impl(source.entities(),leaves,nullptr,nullptr,nullptr,&promoted);
}

Entities compose_phase_assembly_property_selection_edit_candidates(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditPreparation& assemblies,const std::vector<Entities>& ordinary,
    const StairPropertyReviewPreparation* stair_review) {
    if (ordinary.size()>4) invalid("Component alternative Properties composition exceeds its source lane budget");
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,assemblies.request());
    const auto& original=source.entities();
    const auto& admitted=replayed.preview_entities();
    AssemblyPropertyRowAdmission rows;
    std::set<std::string,std::less<>> promoted;
    for (const auto& [id,entity]:original) if (entity.type=="assembly_model") {
        const auto& before=entity.properties.at("model");
        const auto& after=admitted.at(id).properties.at("model");
        if ((before.at("schema")=="sketch.assemblies.v1" || before.at("schema")=="sketch.assemblies.v2") &&
            after.at("schema")=="sketch.assemblies.v7") promoted.insert(id);
        std::map<std::string,const Json*,std::less<>> final_rows;
        for (const auto& row:after.at("instances"))
            if (!final_rows.emplace(row.at("id").get<std::string>(),&row).second)
                invalid("Component alternative replay has duplicate catalog rows");
        for (const auto& before_row:before.at("instances")) {
            const auto instance=before_row.at("id").get<std::string>();
            const auto found=final_rows.find(instance);
            if (found==final_rows.end()) invalid("Component alternative replay removed an original catalog row");
            if (!exact_json(before_row,*found->second)) {
                rows.changed_rows.emplace(AssemblyPhaseRoot{id,instance},*found->second);
                rows.catalogs.insert(id);
            }
        }
    }
    auto leaves=ordinary;
    leaves.push_back(admitted);
    auto result=stair_review
        ? compose_stair_property_review_selection_edit_candidates_impl(source,*stair_review,leaves,&promoted,&rows)
        : compose_mixed_selection_edit_candidates_impl(original,leaves,nullptr,nullptr,nullptr,&promoted,nullptr,&rows);
    // Geometry and actual host dependencies must still hold after other lanes
    // compose. Empty typed patches retain the exact proposed path roster while
    // admitting the complete final map; they carry no replacement allocation.
    std::vector<AssemblyInstancePropertyEditIntent> retained;
    retained.reserve(replayed.result().original_to_proposed.size());
    for (const auto& [before,after]:replayed.result().original_to_proposed) {
        (void)before;
        AssemblyInstancePropertyEditIntent intent;
        intent.target=after;
        retained.push_back(std::move(intent));
    }
    std::sort(retained.begin(),retained.end(),[](const auto& left,const auto& right) { return left.target<right.target; });
    (void)replay_phase_assembly_instance_property_edits(result,retained,{});
    return result;
}

namespace {
std::optional<MixedSelectionPropertyEditPreparation> reconstruct_component_properties_base(
    const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation* base) {
    if (!base) return std::nullopt;
    const auto& wire=base->intent();
    const auto version=wire.at("version").get<int>();
    if (version<5 || version>13 || !wire.at("room_reviews").empty() ||
        (wire.contains("phase_room_reviews") && !wire.at("phase_room_reviews").empty()) ||
        (wire.contains("assembly_request") && !wire.at("assembly_request").is_null()))
        invalid("Component Properties base must be an unreviewed historical noncomponent contribution");
    return replay_mixed_selection_property_edit_preparation(source,wire);
}

const Json& qualified_component_row(const Entities& entities,const AssemblyInstancePropertyEditTarget& target) {
    if (target.document_entity_id) return entities.at(*target.document_entity_id).properties.at("instance");
    const auto& rows=entities.at(target.catalog_id).properties.at("model").at("instances");
    const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& value) { return value.at("id")==target.instance_id; });
    if (row==rows.end()) invalid("Component Properties lost an actual qualified row");
    return *row;
}

std::map<AssemblyInstancePropertyEditTarget,AssemblyInstancePropertyEditTarget> component_properties_correspondence(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& components,
    const MixedSelectionPropertyEditPreparation* base) {
    auto result=components.result().original_to_proposed;
    if (!base) return result;
    for (auto& [original,destination]:result) {
        if (original.document_entity_id) {
            const auto copied=base->original_to_proposed().find(*original.document_entity_id);
            if (copied==base->original_to_proposed().end() || copied->second==*original.document_entity_id) continue;
            if (destination!=original || !exact_json(qualified_component_row(source.entities(),original),
                    qualified_component_row(components.preview_entities(),destination)))
                invalid("Editing a component with its replaced owner requires coordinated component dependency carry");
            const auto actual=decode_document_assembly_instance(base->preview_snapshot().entities().at(copied->second));
            destination.catalog_id=actual.assembly_catalog_id;
            destination.instance_id=actual.instance.id;
            destination.document_entity_id=copied->second;
        } else {
            const auto copied=base->hosted_binding_original_to_proposed().find({original.catalog_id,original.instance_id});
            if (copied==base->hosted_binding_original_to_proposed().end() ||
                copied->second==AssemblyPhaseRoot{original.catalog_id,original.instance_id}) continue;
            if (destination!=original || !exact_json(qualified_component_row(source.entities(),original),
                    qualified_component_row(components.preview_entities(),destination)))
                invalid("Editing a component with its replaced host requires coordinated component dependency carry");
            destination.catalog_id=copied->second.first;
            destination.instance_id=copied->second.second;
        }
    }
    return result;
}

AssemblyPropertyRowAdmission component_property_row_admission(const Entities& original,const Entities& admitted,
    std::set<std::string,std::less<>>& promoted) {
    AssemblyPropertyRowAdmission result;
    for (const auto& [id,entity]:original) if (entity.type=="assembly_model") {
        const auto& before=entity.properties.at("model");
        const auto& after=admitted.at(id).properties.at("model");
        if ((before.at("schema")=="sketch.assemblies.v1" || before.at("schema")=="sketch.assemblies.v2") &&
            after.at("schema")=="sketch.assemblies.v7") promoted.insert(id);
        std::map<std::string,const Json*,std::less<>> rows;
        for (const auto& row:after.at("instances"))
            if (!rows.emplace(row.at("id").get<std::string>(),&row).second)
                invalid("Component Properties replay has duplicate qualified rows");
        for (const auto& before_row:before.at("instances")) {
            const auto instance=before_row.at("id").get<std::string>();
            const auto found=rows.find(instance);
            if (found==rows.end()) invalid("Component Properties replay removed an original catalog row");
            if (!exact_json(before_row,*found->second)) {
                result.changed_rows.emplace(AssemblyPhaseRoot{id,instance},*found->second);
                result.catalogs.insert(id);
            }
        }
    }
    return result;
}
} // namespace

std::map<AssemblyInstancePropertyEditTarget,AssemblyInstancePropertyEditTarget>
resolve_phase_assembly_with_mixed_property_correspondence(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditPreparation& components,const MixedSelectionPropertyEditPreparation* base) {
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,components.request());
    const auto reconstructed=reconstruct_component_properties_base(source,base);
    return component_properties_correspondence(source,replayed,reconstructed ? &*reconstructed : nullptr);
}

Entities compose_phase_assembly_with_mixed_property_selection_edit_candidates(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditPreparation& components,const MixedSelectionPropertyEditPreparation* base) {
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,components.request());
    const auto reconstructed=reconstruct_component_properties_base(source,base);
    if (!reconstructed) return compose_phase_assembly_property_selection_edit_candidates(source,replayed,{});
    const auto correspondence=component_properties_correspondence(source,replayed,&*reconstructed);
    const auto& actual=source.entities();
    const auto& historical=reconstructed->preview_snapshot().entities();
    PhasePropertyCompositionRetirements retirements;
    for (const auto& [id,entity]:actual) {
        (void)entity;
        if (!historical.contains(id)) retirements.erased.insert(id);
    }
    const auto aliases=embedded_assembly_presentation_ids(historical);
    for (const auto& [binding,alias]:embedded_assembly_presentation_ids(actual)) {
        const auto retained=aliases.find(binding);
        if (retained==aliases.end()) retirements.bindings.insert(binding);
        else if (retained->second!=alias) invalid("Component Properties base changed a surviving source alias");
    }
    std::set<std::string,std::less<>> promoted;
    const auto rows=component_property_row_admission(actual,replayed.preview_entities(),promoted);
    auto result=compose_mixed_selection_edit_candidates_impl(actual,{historical,replayed.preview_entities()},
        nullptr,nullptr,nullptr,&promoted,&retirements,&rows);
    // Neither captured preparation can certify geometry altered by the other.
    // Invariant selected paths re-admit actual destinations against the final
    // complete physical graph without allocating or publishing any identity.
    std::vector<AssemblyInstancePropertyEditIntent> retained;
    for (const auto& [original,destination]:correspondence) {
        (void)original;
        AssemblyInstancePropertyEditIntent intent; intent.target=destination;
        retained.push_back(std::move(intent));
    }
    std::sort(retained.begin(),retained.end(),[](const auto& left,const auto& right) { return left.target<right.target; });
    (void)replay_phase_assembly_instance_property_edits(result,retained,{});
    return result;
}

Entities compose_phase_property_selection_edit_candidates(const DocumentSnapshot& source,
    const std::vector<Json>& authorings,const std::vector<Entities>& ordinary,
    const StairPropertyReviewPreparation* ordinary_stair_review,
    const AssemblyInstancePropertyEditPreparation* assemblies) {
    if (!source.is_editable() || authorings.size()>1000 || ordinary.size()>4 ||
        (authorings.empty() && ordinary.empty() && !ordinary_stair_review && !assemblies))
        invalid("Phase Properties composition requires bounded actual-source contributions");
    const auto& actual=source.entities();
    const auto snapshot_digest=document_snapshot_digest(source);
    const auto authoring_digest=document_authoring_source_digest_v2(source);
    const auto entities_digest=entity_map_digest(actual);
    const auto choices=phase_constraint_authoring_selections(actual);
    std::optional<AssemblyInstancePropertyEditPreparation> assembly_replay;
    std::set<std::string,std::less<>> promoted;
    if (assemblies) {
        assembly_replay=replay_assembly_instance_property_edit_preparation(source,assemblies->request());
        for (const auto& [id,entity]:actual) if (entity.type=="assembly_model") {
            const auto& before=entity.properties.at("model").at("schema");
            const auto& after=assembly_replay->preview_entities().at(id).properties.at("model").at("schema");
            if ((before=="sketch.assemblies.v1" || before=="sketch.assemblies.v2") && after=="sketch.assemblies.v7")
                promoted.insert(id);
        }
    }
    Entities result=actual;
    PhasePropertyCompositionRetirements retirements;
    std::set<std::string,std::less<>> retired_rails;
    std::set<std::tuple<int,std::string,std::string>> groups;
    std::size_t bytes{};
    const auto collect=[&](const Entities& replay) {
        for (const auto& [id,entity]:actual) if (!replay.contains(id)) {
            if (entity.type=="railing" && !retired_rails.contains(id))
                invalid("Phase Properties physical retirement lacks an explicit actual rail decision");
            if (entity.type!="railing" && entity.type!="constraint" && entity.type!=kAnnotationEntityType &&
                !can_recognize_boundary_dimension_entity_type(entity.type))
                invalid("Phase Properties erased an unsupported physical owner");
            retirements.erased.insert(id);
        }
        const auto aliases=embedded_assembly_presentation_ids(replay);
        for (const auto& [binding,alias]:embedded_assembly_presentation_ids(actual)) {
            const auto next=aliases.find(binding);
            if (next!=aliases.end()) {
                if (next->second!=alias) invalid("Phase Properties changed an actual surviving render alias");
                continue;
            }
            const auto& catalog=actual.at(binding.first).properties.at("model").at("instances");
            const auto row=std::find_if(catalog.begin(),catalog.end(),[&](const auto& value) { return value.at("id")==binding.second; });
            if (row==catalog.end() || !row->contains("placement") ||
                !retired_rails.contains(row->at("placement").at("host_entity_id").template get<std::string>()))
                invalid("Phase Properties hosted retirement lacks an explicit actual rail decision");
            retirements.bindings.insert(binding);
        }
    };
    // Fold original-source contributions in pairs, keeping the exact original
    // map as every merge's authority. The accumulated map is a derived result,
    // never a projected snapshot. This retains the 1000-target request budget
    // without holding one whole-document candidate for every phase cohort.
    const auto merge=[&](const Entities& replay) {
        result=compose_mixed_selection_edit_candidates_impl(actual,{result,replay},nullptr,nullptr,nullptr,
            assemblies ? &promoted : nullptr,&retirements);
    };
    for (const auto& wire:authorings) {
        const auto count=wire.dump().size();
        if (count>4*1024*1024-bytes) invalid("Phase Properties combined proof byte budget exceeded");
        bytes+=count;
        const auto intent=decode_phase_constraint_authoring_intent(wire);
        if (intent.expected_revision!=source.revision() || intent.source_saved_revision!=source.saved_revision_optional() ||
            intent.source_snapshot_digest!=snapshot_digest || intent.source_authoring_digest!=authoring_digest ||
            intent.source_entities_digest!=entities_digest || intent.phase_selections.dump()!=choices.dump())
            invalid("Phase Properties contribution differs from its complete original source");
        const auto version=wire.at("version").get<int>();
        const bool qualified_scalar_wall=version==28 &&
            (wire.at("wall_edit_version")==23 || wire.at("wall_edit_version")==26);
        std::string registry,alternative;
        Entities replay;
        if (version==23 || version==26 || qualified_scalar_wall) {
            const auto leaf=decode_phase_wall_replacement_authoring(intent.wall_replacement);
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            // Explicit room decisions, when present, complete their own leaf.
            // Otherwise only detached physical inspection is reconstructed.
            replay=leaf.room_review_intent.is_null() ? inspect_phase_wall_replacement_authoring(source,intent).edited_entities :
                replay_phase_constraint_authoring(actual,wire);
        } else if (version==24) {
            const auto leaf=decode_phase_structural_replacement_authoring(intent.structural_replacement);
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            replay=replay_phase_constraint_authoring(actual,wire);
        } else if (version==5 || version==27) {
            const auto leaf=decode_phase_slab_replacement_authoring(intent.slab_replacement);
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            replay=replay_phase_constraint_authoring(actual,wire);
        } else if (version==4) {
            const auto leaf=decode_phase_roof_replacement_authoring(intent.roof_replacement);
            if (leaf.demolition) invalid("Phase Properties cannot borrow roof demolition authority");
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            replay=replay_phase_constraint_authoring(actual,wire);
        } else if (version==25) {
            const auto leaf=decode_phase_stair_replacement_authoring(intent.stair_replacement);
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            for (const auto& decision:leaf.dependency_dispositions)
                if (decision.action==PhaseStairReplacementDependencyAction::retire && !retired_rails.insert(decision.rail_id).second)
                    invalid("Phase Properties repeats an explicit rail retirement");
            replay=replay_phase_constraint_authoring(actual,wire);
        } else invalid("Phase Properties contribution has unsupported replacement authority");
        // Historical scalar and vertical scalar dialects are the same family.
        // Different wire versions cannot split overlapping wall authority.
        if (!groups.emplace(version==26 || qualified_scalar_wall ? 23 : version==27 ? 5 : version,registry,alternative).second)
            invalid("Phase Properties repeats a family/registry/alternative contribution");
        collect(replay);merge(replay);
    }
    if (ordinary_stair_review) {
        const auto reviewed=replay_stair_property_review_preparation(source,ordinary_stair_review->request());
        for (const auto& id:reviewed.retired_rail_ids()) if (!retired_rails.insert(id).second)
            invalid("Ordinary and replacement Properties overlap explicit rail retirement");
        collect(reviewed.entities());merge(reviewed.entities());
    }
    for (const auto& replay:ordinary) merge(replay);
    if (assembly_replay) merge(assembly_replay->preview_entities());
    for (const auto& id:retirements.erased) if (result.contains(id))
        invalid("Phase Properties restored a source-derived retired owner");
    return result;
}

namespace {
Entities compose_removal_candidates(const Entities& source,const std::vector<Entities>& candidates,
    bool include_ordinary_removal, bool complete_roof_removal, bool preserve_all_baselines,
    bool allow_shared_reference_retirement=false,bool complete_hosted_catalog_consequences=false) {
    coordinated_map_budget(source);
    if (candidates.size()<2 || candidates.size()>(include_ordinary_removal ?
            (complete_hosted_catalog_consequences ? 7u : 6u) : 5u))
        invalid("Coordinated demolition exceeds its complete family candidate bounds");
    for (const auto& candidate:candidates) coordinated_map_budget(candidate);
    std::set<std::string,std::less<>> baseline;
    for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
        (void)key;
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        if (preserve_all_baselines || model.active_alternative() || !model.alternatives().empty())
            baseline.insert(model.baseline_ids().begin(),model.baseline_ids().end());
    }
    auto result=source;
    for (const auto& [key,entity]:source) {
        std::vector<const Entity*> changed;
        std::size_t erased{};
        for (const auto& candidate:candidates) {
            const auto found=candidate.find(key);
            if (found==candidate.end()) ++erased;
            else if (!exact(entity,found->second)) changed.push_back(&found->second);
        }
        if (baseline.contains(key) && (erased || (!changed.empty() &&
            (!include_ordinary_removal || entity.type!="assembly_model"))))
            invalid("Coordinated demolition cannot change retained baseline physical owners");
        if (erased) {
            if (complete_roof_removal && entity.required)
                invalid("Complete architectural removal cannot erase a required source entity");
            const bool shared_reference=allow_shared_reference_retirement &&
                ((entity.type=="constraint" && decode_constraint_entity(entity).supported()) ||
                 (can_recognize_boundary_dimension_entity_type(entity.type) && decode_boundary_dimension_entity(entity).supported()));
            if ((erased!=1 && !shared_reference) || !changed.empty())
                invalid("Coordinated demolition has overlapping retirement consequences");
            result.erase(key);
        } else if (include_ordinary_removal && entity.type=="assembly_model" && !changed.empty())
            result.at(key)=merge_demolition_catalog(entity,changed,allow_shared_reference_retirement,
                complete_hosted_catalog_consequences);
        else if (changed.size()==1) result.at(key)=*changed.front();
        else if (changed.size()>1) result.at(key)=entity.type=="assembly_model"
            ? merge_demolition_catalog(entity,changed,allow_shared_reference_retirement) :
                merge_demolition_row_container(entity,changed,complete_roof_removal,allow_shared_reference_retirement);
    }
    for (const auto& candidate:candidates) for (const auto& [key,entity]:candidate) {
        if (source.contains(key)) continue;
        if (!result.emplace(key,entity).second)
            invalid("Coordinated demolition family destinations overlap");
    }
    coordinated_map_budget(result);
    const auto final_aliases=embedded_assembly_presentation_ids(result);
    const auto preserve_surviving_aliases=[&](const Entities& original) {
        for (const auto& [key,alias]:embedded_assembly_presentation_ids(original))
            if (const auto final=final_aliases.find(key);final!=final_aliases.end() && final->second!=alias)
                invalid("Coordinated demolition changed a surviving component's render alias");
    };
    preserve_surviving_aliases(source);
    for (const auto& candidate:candidates) preserve_surviving_aliases(candidate);
    (void)constraint_phase_scope(result);
    validate_document_assembly_instances(result);
    validate_stair_attachment_state(result);
    return result;
}
} // namespace

Entities compose_phase_demolition_candidates(const Entities& source,const std::vector<Entities>& candidates,
    bool include_ordinary_removal, bool complete_roof_removal) {
    if (complete_roof_removal && !include_ordinary_removal)
        invalid("Complete roof removal requires its ordinary removal lane");
    return compose_removal_candidates(source,candidates,include_ordinary_removal,complete_roof_removal,true);
}

Entities compose_ordinary_architectural_removal_candidates(const Entities& source,const std::vector<Entities>& candidates,
    bool allow_shared_reference_retirement,bool complete_hosted_catalog_consequences) {
    return compose_removal_candidates(source,candidates,true,true,false,allow_shared_reference_retirement,
        complete_hosted_catalog_consequences);
}
} // namespace sketch
