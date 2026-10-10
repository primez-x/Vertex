#include "sketch/phase_constraint_authoring.hpp"

#include "sketch/boundary_transform.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
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
#include "sketch/model_phases.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>

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

ConstraintAuthoringIntent decode_intent(const Json& value) {
    keys(value,{"wall_resize","wall_geometry_move","wall_curve_construction","boundary_resize","boundary_vertex_move",
        "exterior_corner_move","exterior_segment_resize","exterior_segment_arc","measured_stroke_resize",
        "measured_stroke_vertex_move","measured_stroke_transform","joint_translation","relation_mutations",
        "relation_anchor","relation_move_connected_walls","message"});
    ConstraintAuthoringIntent result;
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
    const bool stairs=value.is_object() && value.contains("version") && value.at("version")==4;
    const bool structural=stairs || (value.is_object() && value.contains("version") && value.at("version")==3);
    const bool walls=structural || (value.is_object() && value.contains("version") && value.at("version")==2);
    if (stairs) keys(value,{"version","wall_authoring","roof_replacement","slab_replacement","structural_replacement",
        "stair_replacement","ordinary_roof_edits","ordinary_slab_geometry","ordinary_structural_edits","ordinary_stair_transforms"});
    else if (structural) keys(value,{"version","wall_authoring","roof_replacement","slab_replacement","structural_replacement",
        "ordinary_roof_edits","ordinary_slab_geometry","ordinary_structural_edits"});
    else if (walls) keys(value,{"version","wall_authoring","roof_replacement","slab_replacement","ordinary_roof_edits","ordinary_slab_geometry"});
    else keys(value,{"version","roof_replacement","slab_replacement","ordinary_roof_edits","ordinary_slab_geometry"});
    if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4))
        invalid("Coordinated replacements require a supported version");
    bounded_array(value.at("ordinary_roof_edits"));
    bounded_array(value.at("ordinary_slab_geometry"));
    CoordinatedReplacements result;
    result.version=stairs?4:structural?3:walls?2:1;
    if (walls && !value.at("wall_authoring").is_null()) {
        const auto& child=value.at("wall_authoring");
        // Refuse nested coordination before decoding; historical wall children
        // alone carry this complete captured-source binding.
        if (!child.is_object() || !child.contains("version") ||
            (child.at("version")!=1 && child.at("version")!=2))
            invalid("Coordinated wall child requires historical wall authoring version one or two");
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
            if (!leaf.wall_profiles.empty() || !leaf.opening_profiles.empty() || !leaf.opening_rehosts.empty() ||
                !leaf.opening_families.empty() || !leaf.wall_stacks.empty())
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
        result.roof=decode_phase_roof_replacement_authoring(leaf);
        if (result.roof->demolition || encode_phase_roof_replacement_authoring(*result.roof).dump()!=leaf.dump())
            invalid("Coordinated roof replacement requires canonical replacement authority without demolition");
    }
    if (!value.at("slab_replacement").is_null()) {
        const auto& leaf=value.at("slab_replacement");
        result.slab=decode_phase_slab_replacement_authoring(leaf);
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
        if (encode_phase_stair_replacement_authoring(*result.stair).dump()!=leaf.dump())
            invalid("Coordinated stair replacement requires canonical typed authority");
    }
    std::set<std::string,std::less<>> roof_targets, slab_targets;
    for (const auto& row:value.at("ordinary_roof_edits")) {
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
    bool ordinary_removal_rosters=false,bool allow_shared_reference_retirement=false) {
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
                    if (allow_shared_reference_retirement && !consequence && !next &&
                        (source.type==kAnnotationEntityType || source.type==kSheetViewEntityType)) continue;
                    invalid("Coordinated families change the same retained source row");
                }
                changed=true; consequence=next;
            }
            if (consequence) rows.push_back(*consequence);
        }
        std::set<RowIdentity> destinations;
        for (const auto& [identity,index]:source_rows) { (void)index; destinations.insert(identity); }
        for (const auto& suffix:additions) for (const auto* row:suffix) {
            if (!destinations.insert(destination(*row)).second)
                invalid("Coordinated fresh presentation or registry rows overlap");
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
    bool ordinary_removal_rosters=false,bool allow_shared_reference_retirement=false) {
    if (source.type!=kSheetViewEntityType)
        return merge_source_row_container(source,candidates,ordinary_removal_rosters,allow_shared_reference_retirement);
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
    auto result=merge_source_row_container(source,rows,false,allow_shared_reference_retirement);
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

Entities replay_coordinated(const Entities& source,const PhaseConstraintAuthoringIntent& enclosing,
    const Entities* inspected_wall=nullptr,const std::vector<std::string>* inspected_wall_fresh=nullptr) {
    const auto lanes=coordinated(enclosing.coordinated_replacements,enclosing);
    const bool coordinated_hosted=(lanes.roof && lanes.roof->include_hosted_instances) ||
        std::any_of(lanes.ordinary_roofs.begin(),lanes.ordinary_roofs.end(),
            [](const auto& edit) { return edit.coordinate_world_hosted_geometry; }) ||
        (lanes.stair && std::any_of(lanes.stair->compound_edits.begin(),lanes.stair->compound_edits.end(),
            [](const auto& edit) { return edit.coordinate_profile_hosted_geometry; }));
    coordinated_map_budget(source);
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
                leaf.alternative_id,leaf.complete_presentations);
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
                if (lanes.version==1 || removed || (lanes.version>=3 && original.type=="assembly_model"))
                    invalid("Coordinated replacement cannot overlap removal of an actual source owner or hosted catalog");
                removed=true;
            } else if (!exact(original,found->second)) changes.push_back(&found->second);
        }
        if (removed) {
            if (!changes.empty()) invalid("Coordinated family removal overlaps another physical consequence");
            result.erase(key);
        } else if (changes.size()==1) result.at(key)=lanes.version>=3 && original.type=="assembly_model" ?
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
        static_cast<int>(!value.wall_demolition.is_null());
    if (exclusive_operations > 1)
        invalid("Active design operations cannot borrow another replacement or demolition authority");
    const auto coordinated_version=value.coordinated_replacements.is_null()?0:coordinated(value.coordinated_replacements,value).version;
    Json result={{"version",!value.wall_demolition.is_null()?16:!value.coordinated_demolition.is_null()?15:!value.stair_demolition_retirement.is_null()?13:!value.stair_replacement.is_null()?12:!value.stair_demolition.is_null()?11:!value.structural_replacement.is_null()?9:coordinated_version==4?14:coordinated_version==3?10:coordinated_version==2?8:coordinated_version==1?7:!value.slab_demolition.is_null()?6:!value.slab_replacement.is_null()?5:!value.roof_replacement.is_null()?4:!value.opening_demolition.is_null()?3:value.wall_replacement.is_null()?1:2},{"expected_revision",value.expected_revision},
        {"source_snapshot_digest",value.source_snapshot_digest},{"source_authoring_digest",value.source_authoring_digest},
        {"source_entities_digest",value.source_entities_digest},
        {"source_saved_revision",value.source_saved_revision ? Json(*value.source_saved_revision) : Json(nullptr)},
        {"phase_selections",value.phase_selections},{"intent",encode_intent(value.intent)}};
    if (!value.wall_replacement.is_null()) {
        const auto replacement=decode_phase_wall_replacement_authoring(value.wall_replacement);
        if (encode_phase_wall_replacement_authoring(replacement).dump()!=value.wall_replacement.dump())
            invalid("Wall replacement decisions are not canonical");
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
    // dump validates UTF-8 as well as the complete byte resource bound.
    resource_shape(result);
    if (result.dump().size()>proof_budget) invalid("Phase constraint proof exceeds its byte budget");
    return result;
}

PhaseConstraintAuthoringIntent decode_phase_constraint_authoring_intent(const Json& value) {
    resource_shape(value);
    if (value.dump().size()>proof_budget) invalid("Phase constraint proof exceeds its byte budget");
    const bool replacement=value.is_object() && value.contains("version") && value.at("version")==2;
    const bool demolition=value.is_object() && value.contains("version") && value.at("version")==3;
    const bool roof=value.is_object() && value.contains("version") && value.at("version")==4;
    const bool slab=value.is_object() && value.contains("version") && value.at("version")==5;
    const bool slab_demolition=value.is_object() && value.contains("version") && value.at("version")==6;
    const bool coordinated_replacements=value.is_object() && value.contains("version") &&
        (value.at("version")==7 || value.at("version")==8 || value.at("version")==10 || value.at("version")==14);
    const bool structural=value.is_object() && value.contains("version") && value.at("version")==9;
    const bool stair_demolition=value.is_object() && value.contains("version") && value.at("version")==11;
    const bool stair_replacement=value.is_object() && value.contains("version") && value.at("version")==12;
    const bool stair_retirement=value.is_object() && value.contains("version") && value.at("version")==13;
    const bool coordinated_demolition=value.is_object() && value.contains("version") && value.at("version")==15;
    const bool wall_demolition=value.is_object() && value.contains("version") && value.at("version")==16;
    if (replacement) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
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
    else keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent"});
    if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5 && value.at("version")!=6 && value.at("version")!=7 && value.at("version")!=8 && value.at("version")!=9 && value.at("version")!=10 && value.at("version")!=11 && value.at("version")!=12 && value.at("version")!=13 && value.at("version")!=14 && value.at("version")!=15 && value.at("version")!=16) ||
        !value.at("expected_revision").is_number_unsigned()) invalid("Phase constraint proof version or revision is invalid");
    if (!value.at("source_saved_revision").is_null() && !value.at("source_saved_revision").is_number_unsigned())
        invalid("Phase constraint saved revision is invalid");
    PhaseConstraintAuthoringIntent result{value.at("expected_revision").get<Revision>(),
        value.at("source_snapshot_digest").get<std::string>(),value.at("source_authoring_digest").get<std::string>(),
        value.at("source_entities_digest").get<std::string>(),
        value.at("source_saved_revision").is_null() ? std::nullopt : std::optional<Revision>{value.at("source_saved_revision").get<Revision>()},
        value.at("phase_selections"),decode_intent(value.at("intent"))};
    if (replacement) {
        result.wall_replacement=value.at("wall_replacement");
        if (result.wall_replacement.is_null()) invalid("Version two requires wall replacement decisions");
        (void)decode_phase_wall_replacement_authoring(result.wall_replacement);
    }
    if (demolition) {
        result.opening_demolition = value.at("opening_demolition");
        if (result.opening_demolition.is_null()) invalid("Version three requires opening demolition decisions");
        (void)decode_phase_opening_demolition_intent(result.opening_demolition);
    }
    if (roof) {
        result.roof_replacement = value.at("roof_replacement");
        if (result.roof_replacement.is_null()) invalid("Version four requires roof replacement decisions");
        (void)decode_phase_roof_replacement_authoring(result.roof_replacement);
    }
    if (slab) {
        result.slab_replacement = value.at("slab_replacement");
        if (result.slab_replacement.is_null()) invalid("Version five requires slab replacement decisions");
        (void)decode_phase_slab_replacement_authoring(result.slab_replacement);
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
            (value.at("version")==10 && inner.version!=3) || (value.at("version")==14 && inner.version!=4))
            invalid("Coordinated outer and inner authoring versions do not match");
    }
    if (structural) {
        result.structural_replacement=value.at("structural_replacement");
        if (result.structural_replacement.is_null()) invalid("Version nine requires structural replacement decisions");
        (void)decode_phase_structural_replacement_authoring(result.structural_replacement);
    }
    if (stair_demolition) {
        result.stair_demolition=value.at("stair_demolition");
        if (result.stair_demolition.is_null()) invalid("Version eleven requires stair demolition decisions");
        (void)decode_stair_demolition_intent(result.stair_demolition);
    }
    if (stair_replacement) {
        result.stair_replacement=value.at("stair_replacement");
        if (result.stair_replacement.is_null()) invalid("Version twelve requires stair replacement decisions");
        (void)decode_phase_stair_replacement_authoring(result.stair_replacement);
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
    if ((lanes.version!=2 && lanes.version!=3 && lanes.version!=4) || !lanes.wall || lanes.wall->wall_replacement.is_null())
        invalid("Coordinated preview requires inner version two, three or four with an actual wall replacement");
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
