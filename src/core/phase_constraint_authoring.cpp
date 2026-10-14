#include "sketch/phase_constraint_authoring.hpp"

#include "sketch/boundary_transform.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_dimension_transform.hpp"
#include "sketch/assembly_component_phase_demolition.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/assembly_phase_membership_transport.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/drawing_selection_removal.hpp"
#include "sketch/joint_translation_replay.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_coordinated_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_semantics.hpp"
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
#include "sketch/retained_authoring_identities.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/stair_property_review.hpp"
#include "sketch/stair_object_edit.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"
#include "sketch/phase_selection_property_edit.hpp"
#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/wall_measurement.hpp"
#include "assembly_instance_property_carry.hpp"

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
[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument(reason); }
void admit_component_demolition_source_catalogs(const Entities& source) {
    // Charge every catalog, including unused carriers, before phase scope can
    // decode a registry or resolve any qualified component membership.
    AssemblyCatalogTransferBudget budget;
    for (const auto& [key,entity]:source) {
        (void)key;
        if (entity.type!="assembly_model") continue;
        if (!entity.properties.is_object() || !entity.properties.contains("model") ||
            !entity.properties.at("model").is_object())
            invalid("Component demolition requires an actual typed catalog carrier");
        admit_existing_assembly_catalog_work(entity,budget);
    }
}
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
    // Direct inner10 is separate from the historical family-body dialect.
    bool new_slab_geometry_authority{false};
    bool complete_hosted_component_dimensions{false};
    bool complete_wall_component_dimensions{false};
    bool complete_stair_component_dimensions{false};
    bool complete_roof_component_dimensions{false};
    bool complete_slab_component_dimensions{false};
    bool complete_structural_component_dimensions{false};
    bool complete_family_component_dimensions{false};
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
CoordinatedReplacements coordinated(const Json& value,const PhaseConstraintAuthoringIntent& enclosing,
    bool allow_dimension_wrapper=true,bool allow_stair_dimension_copies=false,bool allow_wall_dimension_copies=false,
    bool allow_roof_dimension_copies=false,bool allow_slab_dimension_copies=false,bool allow_structural_dimension_copies=false,
    bool new_slab_geometry_authority=false) {
    resource_shape(value);
    if (value.dump().size()>proof_budget) invalid("Coordinated replacement proof byte budget exceeded");
    if (value.is_object() && value.contains("version") && value.at("version")==10) {
        if (!allow_dimension_wrapper) invalid("Direct coordinated slab authority cannot nest inside a historical wrapper");
        (void)phase_constraint_coordinated_authoring_body(value);
        const auto declared=[&](const char* field) { return value.contains(field); };
        auto body=value;
        for (const auto* field:{"complete_hosted_component_dimensions","complete_wall_component_dimensions",
            "complete_stair_component_dimensions","complete_roof_component_dimensions",
            "complete_slab_component_dimensions","complete_structural_component_dimensions"}) body.erase(field);
        const bool wall=body.at("wall_authoring").is_object() &&
            body.at("wall_authoring").contains("wall_replacement") &&
            !body.at("wall_authoring").at("wall_replacement").is_null();
        body["version"]=wall ? 5 : 4;
        auto result=coordinated(body,enclosing,false,declared("complete_stair_component_dimensions"),
            declared("complete_wall_component_dimensions"),declared("complete_roof_component_dimensions"),
            declared("complete_slab_component_dimensions"),declared("complete_structural_component_dimensions"),true);
        result.complete_hosted_component_dimensions=true;
        result.complete_family_component_dimensions=true;
        result.complete_wall_component_dimensions=declared("complete_wall_component_dimensions");
        result.complete_stair_component_dimensions=declared("complete_stair_component_dimensions");
        result.complete_roof_component_dimensions=declared("complete_roof_component_dimensions");
        result.complete_slab_component_dimensions=declared("complete_slab_component_dimensions");
        result.complete_structural_component_dimensions=declared("complete_structural_component_dimensions");
        return result;
    }
    if (value.is_object() && value.contains("version") &&
        (value.at("version")==6 || value.at("version")==7 || value.at("version")==8 || value.at("version")==9)) {
        if (!allow_dimension_wrapper) invalid("Coordinated dimension authoring cannot nest wrappers");
        const bool family_copies=value.at("version")==9;
        const bool wall_copies=value.at("version")==8 || (family_copies && value.contains("complete_wall_component_dimensions"));
        const bool stair_copies=value.at("version")==7 ||
            ((wall_copies || family_copies) && value.contains("complete_stair_component_dimensions"));
        const bool roof_copies=family_copies && value.contains("complete_roof_component_dimensions");
        const bool slab_copies=family_copies && value.contains("complete_slab_component_dimensions");
        const bool structural_copies=family_copies && value.contains("complete_structural_component_dimensions");
        // Explicit wrappers admit only their declared family completion.
        // Historical wrappers retain their original child dialects and carry.
        auto result=coordinated(phase_constraint_coordinated_authoring_body(value),enclosing,false,stair_copies,wall_copies,
            roof_copies,slab_copies,structural_copies);
        result.complete_hosted_component_dimensions=true;
        result.complete_wall_component_dimensions=wall_copies;
        result.complete_stair_component_dimensions=stair_copies;
        result.complete_roof_component_dimensions=roof_copies;
        result.complete_slab_component_dimensions=slab_copies;
        result.complete_structural_component_dimensions=structural_copies;
        result.complete_family_component_dimensions=family_copies;
        if (stair_copies && (!result.stair || !result.stair->complete_component_dimensions ||
            !result.stair->complete_ordinary_component_dimensions))
            invalid("Coordinated stair dimensions require an actual explicit replacement leaf");
        if ((roof_copies && (!result.roof || !result.roof->complete_component_dimensions)) ||
            (slab_copies && (!result.slab || !result.slab->complete_component_dimensions)) ||
            (structural_copies && (!result.structural || !result.structural->complete_component_dimensions)))
            invalid("Coordinated family dimensions require each declared actual replacement leaf");
        return result;
    }
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
    result.new_slab_geometry_authority=new_slab_geometry_authority;
    if (hosted_walls && value.at("wall_authoring").is_null())
        invalid("Coordinated qualified wall authoring requires an actual hosted wall replacement");
    if (walls && !value.at("wall_authoring").is_null()) {
        const auto& child=value.at("wall_authoring");
        // Refuse nested coordination before decoding. The additive dialect
        // requires a qualified copy while retaining only historical move authority.
        if (!child.is_object() || !child.contains("version") ||
            (hosted_walls ? (child.at("version")!=(allow_wall_dimension_copies ? 36 : 28) || !child.contains("wall_edit_version") ||
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
            if (hosted_walls && (result.wall->wall_replacement.at("version")!=(allow_wall_dimension_copies ? 12 : 11) ||
                !leaf.complete_hosted_instances || leaf.complete_component_dimensions!=allow_wall_dimension_copies))
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
        if (result.roof->complete_component_dimensions!=allow_roof_dimension_copies)
            invalid("Coordinated roof component dimension copies require matching explicit authority");
        if (result.roof->demolition || encode_phase_roof_replacement_authoring(*result.roof).dump()!=leaf.dump())
            invalid("Coordinated roof replacement requires canonical replacement authority without demolition");
    }
    if (!value.at("slab_replacement").is_null()) {
        const auto& leaf=value.at("slab_replacement");
        result.slab=decode_phase_slab_replacement_authoring(leaf);
        if (result.slab->complete_component_dimensions!=allow_slab_dimension_copies)
            invalid("Coordinated slab component dimension copies require matching explicit authority");
        if (result.slab->coordinate_hosted_profiles)
            invalid("Coordinated replacement dialects cannot borrow slab elevation carry authority");
        if (!new_slab_geometry_authority && result.slab->coordinate_hosted_directional_geometry)
            invalid("Historical coordinated replacement dialects cannot borrow directional slab geometry authority");
        if (!new_slab_geometry_authority && result.slab->preserve_slab_planar_source_geometry)
            invalid("Historical coordinated replacement dialects cannot borrow mapped slab geometry authority");
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
        if (result.structural->complete_component_dimensions!=allow_structural_dimension_copies)
            invalid("Coordinated structural component dimension copies require matching explicit authority");
        if (result.structural->demolition || result.structural->edits.empty() ||
            encode_phase_structural_replacement_authoring(*result.structural).dump()!=leaf.dump())
            invalid("Coordinated structural replacement requires canonical non-demolition authority");
        for (const auto& edit:result.structural->edits) structural_transform_only(edit);
    }
    if (stairs && !value.at("stair_replacement").is_null()) {
        const auto& leaf=value.at("stair_replacement");
        result.stair=decode_phase_stair_replacement_authoring(leaf);
        if (new_slab_geometry_authority && (!result.stair->dependency_rail_ids.empty() ||
            !result.stair->dependency_dispositions.empty()))
            invalid("Direct coordinated slab authority cannot borrow stair dependency review");
        if (result.stair->complete_scalar_dependencies ||
            result.stair->complete_component_dimensions!=allow_stair_dimension_copies ||
            result.stair->complete_ordinary_component_dimensions!=allow_stair_dimension_copies ||
            encode_phase_stair_replacement_authoring(*result.stair).dump()!=leaf.dump())
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
        if (!new_slab_geometry_authority && edit.kind == SlabGeometryEditKind::resize_plan && edit.coordinate_world_hosted_geometry)
            invalid("Historical coordinated ordinary slabs cannot borrow directional geometry authority");
        if (!new_slab_geometry_authority && edit.preserve_planar_source_geometry)
            invalid("Historical coordinated ordinary slabs cannot borrow mapped source geometry authority");
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
    if (new_slab_geometry_authority) {
        const auto directional=[](const auto& edits) {
            return std::any_of(edits.begin(),edits.end(),[](const auto& edit) {
                return edit.preserve_planar_source_geometry ||
                    (edit.kind==SlabGeometryEditKind::resize_plan && edit.coordinate_world_hosted_geometry);
            });
        };
        if (!directional(result.ordinary_slabs) && !(result.slab &&
            (directional(result.slab->slab_geometry) || directional(result.slab->ordinary_geometry))))
            invalid("Direct coordinated slab authority requires an actual directional or mapped slab intent");
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
    bool coalesce_exact_consequences=false,bool defer_registry_result_decode=false) {
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
    if (source.type=="model_phases") {
        // The membership removal union validates all final registries only
        // after its complete final catalog admission, in the typed pruner.
        if (!defer_registry_result_decode) (void)ModelPhases::from_json(merged.properties.at("model"));
    }
    else if (source.type==kAnnotationEntityType) validate_annotation_entity(merged);
    else validate_sheet_view_entity(merged);
    return merged;
}

// Hosted transformation codecs may promote a legacy whole-catalog dialect.
// Omit only the defaults that promotion adds, then retain actual raw rows under
// v7. Definitions, identity namespaces and every other envelope field are exact.
// Current v8/v9 carriers keep their actual dialect and profile rows verbatim;
// historical promotion grants no authority to create mapped geometry.
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
        Json surviving=Json::array();
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
            surviving.push_back(original_row);
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
        // Derive organization solely from actual source records and the row
        // membership admitted above. Placement authority cannot edit context.
        auto expected=source;
        expected.properties.at("model").at("instances")=std::move(surviving);
        expected=prune_embedded_assembly_drawing_contexts(expected);
        normalized.at("instances")=expected.properties.at("model").at("instances");
        if (!exact(expected,envelope)) invalid("Coordinated hosted catalog changed definitions or its retained envelope");
    }
    if (!retired.empty()) {
        Json survivors=Json::array();
        for (const auto& row:rows) if (!retired.contains(row.at("id").get<std::string>())) survivors.push_back(row);
        rows=std::move(survivors);
    }
    merged.properties.at("model").at("instances")=std::move(rows);
    if (promoted && !changed.empty()) merged.properties.at("model").at("schema")="sketch.assemblies.v7";
    merged=prune_embedded_assembly_drawing_contexts(merged);
    (void)AssemblyModel::from_json(merged.properties.at("model"));
    return merged;
}

Entity merge_coordinated_directional_catalog(const Entity& source,const std::vector<const Entity*>& candidates) {
    auto merged=source;
    for (const auto* candidate:candidates) {
        auto envelope=*candidate;
        envelope.properties.at("model")=source.properties.at("model");
        if (!exact(source,envelope))
            invalid("Coordinated directional catalog changed fields outside its actual model");
        merged.properties.at("model")=merge_disjoint_directionally_resized_hosted_assembly_models(
            source.properties.at("model"),merged.properties.at("model"),candidate->properties.at("model"));
    }
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
// The exact source envelope is pruned only for admitted instance membership;
// this is no authority to normalize a dialect or merge arbitrary catalog data.
Entity merge_demolition_catalog(const Entity& source,const std::vector<const Entity*>& candidates,
    bool allow_shared_reference_retirement=false,bool complete_hosted_catalog_consequences=false,
    bool defer_result_decode=false) {
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
        // Fresh suffixes have typed leaf authority for model rows only. They
        // cannot invent organization records; retain and prune actual source
        // context rows using the independently admitted local membership.
        auto expected=source;
        expected.properties.at("model").at("instances")=rows;
        expected=prune_embedded_assembly_drawing_contexts(expected);
        expected.properties.at("model").at("instances")=retained;
        envelope.properties.at("model").at("instances")=retained;
        if (!exact(expected,envelope)) invalid("Coordinated demolition changed a retained catalog envelope");
    }
    auto result=source;
    auto rows=Json::array();
    for (const auto& row:retained)
        if (!removed.contains(row.at("id").get<std::string>())) rows.push_back(row);
    for (const auto& row:fresh_rows) rows.push_back(row);
    result.properties.at("model").at("instances")=std::move(rows);
    result=prune_embedded_assembly_drawing_contexts(result);
    // Membership completion admits every final catalog together before its
    // pruner and complete-map validation decode the composed result.
    if (!defer_result_decode) (void)AssemblyModel::from_json(result.properties.at("model"));
    return result;
}

Entity merge_demolition_row_container(const Entity& source,const std::vector<const Entity*>& candidates,
    bool ordinary_removal_rosters=false,bool allow_shared_reference_retirement=false,
    bool coalesce_exact_consequences=false,bool defer_registry_result_decode=false) {
    if (source.type!=kSheetViewEntityType)
        return merge_source_row_container(source,candidates,ordinary_removal_rosters,allow_shared_reference_retirement,
            coalesce_exact_consequences,defer_registry_result_decode);
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
    for (const auto& [original,fresh]:replacement.dimension_identities) { (void)original; result.push_back(fresh); }
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
    if (lanes.new_slab_geometry_authority && source.size()>100000)
        invalid("Direct coordinated slab source entity budget exceeded");
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
    std::set<std::string,std::less<>> ordinary_dimension_hosts;
    std::optional<ConstraintPhaseScope> ordinary_dimension_scope;
    std::set<std::string,std::less<>> ordinary_dimension_protected_baselines;
    std::vector<std::pair<std::string,std::string>> copied_dimension_roots;
    std::map<std::pair<std::string,std::string>,std::pair<std::string,std::string>> copied_dimension_root_destinations;
    std::map<std::string,std::string,std::less<>> copied_dimension_identities;
    const auto register_dimension_copies=[&](const auto& roots,const auto& identities,
        const auto& hosted_identities,const auto& dimension_identities) {
        if (!lanes.complete_family_component_dimensions) return;
        for (const auto& root:roots) {
            if (!copied_dimension_root_destinations.emplace(root,
                std::pair{identities.at(root.first),hosted_identities.at(root)}).second)
                invalid("Coordinated component dimension source roots overlap");
            copied_dimension_roots.push_back(root);
            if (copied_dimension_roots.size()>4096) invalid("Coordinated component dimension root budget exceeded");
        }
        for (const auto& [original,proposed]:dimension_identities) {
            if (!copied_dimension_identities.emplace(original,proposed).second)
                invalid("Coordinated component dimension source rosters overlap");
            if (copied_dimension_identities.size()>1000)
                invalid("Coordinated shared component dimension association budget exceeded");
        }
    };
    if (lanes.complete_hosted_component_dimensions) {
        ordinary_dimension_scope=constraint_phase_scope(source);
        for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
            (void)key;
            const auto model=ModelPhases::from_json(entity.properties.at("model"));
            if (model.active_alternative())
                ordinary_dimension_protected_baselines.insert(model.baseline_ids().begin(),model.baseline_ids().end());
        }
    }
    std::map<std::pair<std::string,std::string>,Json> authenticated_ordinary_rows;
    const auto collect_ordinary_dimension_hosts=[&](Entities& candidate,
        std::initializer_list<std::string_view> physical_types,
        const std::vector<SlabGeometryEditIntent>& slab_intents=std::vector<SlabGeometryEditIntent>{},
        bool wall_dependencies=false) {
        if (!lanes.complete_hosted_component_dimensions) return;
        coordinated_map_budget(candidate);
        // This map has just been derived by the family's actual-source typed
        // producer. No replacement or composed map can supply host authority.
        std::vector<std::string> candidate_hosts;
        const bool directional=lanes.new_slab_geometry_authority &&
            std::any_of(slab_intents.begin(),slab_intents.end(),[](const auto& edit) {
                return edit.kind==SlabGeometryEditKind::resize_plan && edit.coordinate_world_hosted_geometry;
            });
        std::set<std::string,std::less<>> unit_authority;
        if (directional) for (const auto& edit:slab_intents)
            if (edit.kind==SlabGeometryEditKind::resize_plan && edit.coordinate_world_hosted_geometry)
                unit_authority.insert(edit.slab_id);
        // Match the complete wall leaf's dependency discovery: an opening's
        // local fields can stay exact while its changed parent moves its frame.
        std::set<std::string,std::less<>> changed_walls;
        if (wall_dependencies) for (const auto& [key,original]:source) if (original.type=="wall") {
            const auto found=candidate.find(key);
            if (found==candidate.end() || found->second.id!=key || found->second.type!=original.type)
                invalid("Coordinated ordinary wall hosts must retain actual owner identity and type");
            if (!exact(original,found->second)) changed_walls.insert(key);
        }
        for (const auto& [key,original]:source) {
            const auto found=candidate.find(key);
            if (can_recognize_boundary_dimension_entity_type(original.type) &&
                is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(original).format) &&
                (found==candidate.end() || (!lanes.complete_family_component_dimensions && !exact(original,found->second))))
                invalid("Coordinated ordinary host candidates must retain original component dimension rows");
            if (std::find(physical_types.begin(),physical_types.end(),original.type)==physical_types.end()) continue;
            if (lanes.complete_family_component_dimensions && found==candidate.end()) continue;
            if (found==candidate.end() || found->second.id!=key || found->second.type!=original.type)
                invalid("Coordinated ordinary dimension hosts must retain actual owner identity and type");
            const bool parent_changed=wall_dependencies && original.type=="opening" &&
                changed_walls.contains(original.properties.at("wall_id").get<std::string>());
            if (exact_json(original.properties,found->second.properties) &&
                exact_json(original.extensions,found->second.extensions) && !unit_authority.contains(key) &&
                !parent_changed) continue;
            if (ordinary_dimension_scope->inactive_owner_ids.contains(key) ||
                ordinary_dimension_protected_baselines.contains(key))
                invalid("Coordinated ordinary dimension hosts require active nonbaseline authority");
            ordinary_dimension_hosts.insert(key);
            candidate_hosts.push_back(key);
            if (ordinary_dimension_hosts.size()>100000)
                invalid("Coordinated ordinary dimension host roster budget exceeded");
        }
        if (lanes.complete_family_component_dimensions) {
            // The actual leaf may already carry ordinary dimensions. Verify
            // precisely that contribution using original-source authority,
            // then release it for one carry against the final combined body.
            std::vector<std::string> changed;
            for (const auto& [key,original]:source)
                if (can_recognize_boundary_dimension_entity_type(original.type) &&
                    is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(original).format) &&
                    !exact(original,candidate.at(key))) {
                    if (copied_dimension_identities.contains(key))
                        invalid("Coordinated leaf changed a protected original copied dimension");
                    changed.push_back(key);
                }
            if (!changed.empty() || lanes.new_slab_geometry_authority) {
                auto normalized=candidate;
                for (const auto& key:changed) normalized.at(key)=source.at(key);
                const auto proven=wall_dependencies ?
                    carry_wall_hosted_assembly_dimension_transform_entities(source,normalized,candidate_hosts) : directional ?
                    carry_hosted_assembly_directional_resize_dimension_entities(source,normalized,slab_intents) :
                    carry_hosted_assembly_dimension_transform_entities(source,normalized,candidate_hosts);
                std::map<std::string,const Entity*,std::less<>> contributions;
                for (const auto& dimension:proven) contributions.emplace(dimension.id,&dimension);
                validate_ordinary_assembly_dimension_changes(source,changed);
                for (const auto& key:changed) {
                    const auto contribution=contributions.find(key);
                    if (contribution==contributions.end() || !exact(candidate.at(key),*contribution->second))
                        invalid("Coordinated leaf dimension change lacks its proven actual ordinary host contribution");
                    candidate.at(key)=source.at(key);
                }
            }
        }
        if (lanes.new_slab_geometry_authority) {
            const std::set<std::string,std::less<>> hosts(candidate_hosts.begin(),candidate_hosts.end());
            for (const auto& [catalog,entity]:source) if (entity.type=="assembly_model") {
                const auto& before=entity.properties.at("model").at("instances");
                const auto& after=candidate.at(catalog).properties.at("model").at("instances");
                if (before.size()!=after.size()) invalid("Coordinated ordinary roots changed source row inventory");
                for (std::size_t i=0;i<before.size();++i) if (before[i].contains("placement") &&
                    hosts.contains(before[i].at("placement").at("host_entity_id").get<std::string>())) {
                    const std::pair root{catalog,before[i].at("id").get<std::string>()};
                    if (!authenticated_ordinary_rows.emplace(root,after[i]).second)
                        invalid("Coordinated independent ordinary root contributions overlap");
                    if (authenticated_ordinary_rows.size()>4096)
                        invalid("Coordinated ordinary component root budget exceeded");
                }
            }
        }
    };
    if (lanes.wall) {
        const auto& child=*lanes.wall;
        const auto requests=phase_wall_replacement_requests(source,child.intent);
        if (!child.wall_replacement.is_null()) {
            const auto leaf=decode_phase_wall_replacement_authoring(child.wall_replacement);
            if (requests.size()!=1 || requests.front().registry_id!=leaf.registry_id ||
                requests.front().alternative_id!=leaf.alternative_id || requests.front().seed_wall_ids!=leaf.seed_wall_ids)
                invalid("Coordinated wall replacement roots differ from the actual typed geometry authority");
            const auto plan=inspect_phase_wall_replacement_plan(source,leaf.seed_wall_ids,leaf.registry_id,
                leaf.alternative_id,leaf.complete_presentations,leaf.complete_corner_windows,leaf.complete_hosted_instances,
                leaf.complete_component_dimensions);
            retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
            retained_baselines.insert(plan.required_dimension_ids.begin(),plan.required_dimension_ids.end());
            if (leaf.complete_component_dimensions)
                register_dimension_copies(plan.required_hosted_instance_ids,leaf.identities,
                    leaf.hosted_instance_identities,leaf.dimension_identities);
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
        if (lanes.new_slab_geometry_authority)
            collect_ordinary_dimension_hosts(candidates.back(),{"wall","opening"},{},true);
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
            leaf.alternative_id,leaf.phase_qualified_joins,leaf.include_hosted_instances,leaf.complete_component_dimensions);
        auto replay=replay_phase_roof_replacement(source,plan,leaf.identities,leaf.roof_profiles,
            leaf.roof_opening_edits,leaf.roof_edits,leaf.ordinary_roof_edits,leaf.phase_qualified_joins,
            leaf.hosted_instance_identities,leaf.include_hosted_instances,leaf.dimension_identities);
        candidates.push_back(std::move(replay.entities)); family_fresh.push_back(std::move(replay.fresh_identity_ids));
        retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
        retained_baselines.insert(plan.required_dimension_ids.begin(),plan.required_dimension_ids.end());
        if (leaf.complete_component_dimensions)
            register_dimension_copies(plan.required_hosted_instance_ids,leaf.identities,leaf.hosted_instance_identities,leaf.dimension_identities);
        if (lanes.complete_family_component_dimensions) collect_ordinary_dimension_hosts(candidates.back(),{"roof"});
    } else if (!lanes.ordinary_roofs.empty()) {
        const auto partition=partition_phase_roof_geometry_edits(source,lanes.ordinary_roofs);
        if (partition.replacement || !partition.baseline_roof_edits.empty())
            invalid("Coordinated ordinary roof lane requires actual ordinary/proposed membership");
        candidates.push_back(replay_roof_edit_entities(source,lanes.ordinary_roofs));
        collect_ordinary_dimension_hosts(candidates.back(),{"roof"});
        family_fresh.push_back(new_roof_opening_identity_ids(source,roof_edit_opening_intents(lanes.ordinary_roofs)));
    }
    if (lanes.slab) {
        const auto& leaf=*lanes.slab;
        const auto plan=inspect_phase_slab_replacement_plan(source,leaf.seed_slab_ids,leaf.registry_id,leaf.alternative_id,
            leaf.complete_component_dimensions);
        auto replay=replay_phase_slab_replacement(source,plan,leaf.identities,leaf.slab_profiles,
            leaf.slab_stacks,leaf.slab_geometry,leaf.ordinary_geometry,leaf.hosted_instance_identities,
            leaf.coordinate_ordinary_hosted_geometry,leaf.coordinate_hosted_profiles,leaf.dimension_identities,
            leaf.coordinate_hosted_directional_geometry,leaf.preserve_slab_planar_source_geometry);
        candidates.push_back(std::move(replay.entities)); family_fresh.push_back(std::move(replay.fresh_identity_ids));
        retained_baselines.insert(plan.seed_slab_ids.begin(),plan.seed_slab_ids.end());
        retained_baselines.insert(plan.required_dimension_ids.begin(),plan.required_dimension_ids.end());
        if (leaf.complete_component_dimensions)
            register_dimension_copies(plan.required_hosted_instance_ids,leaf.identities,leaf.hosted_instance_identities,leaf.dimension_identities);
        if (lanes.complete_family_component_dimensions)
            collect_ordinary_dimension_hosts(candidates.back(),{"slab"},leaf.ordinary_geometry);
    } else if (!lanes.ordinary_slabs.empty()) {
        const auto partition=partition_phase_slab_geometry_edits(source,lanes.ordinary_slabs);
        if (partition.replacement || !partition.baseline_geometry.empty())
            invalid("Coordinated ordinary slab lane requires actual ordinary/proposed membership");
        candidates.push_back(replay_slab_geometry_with_hosted_entities(source,lanes.ordinary_slabs));
        collect_ordinary_dimension_hosts(candidates.back(),{"slab"},lanes.ordinary_slabs);
        family_fresh.emplace_back();
    }
    if (lanes.structural) {
        const auto& leaf=*lanes.structural;
        const auto plan=inspect_phase_structural_replacement_plan(source,leaf.seed_object_ids,
            leaf.registry_id,leaf.alternative_id,leaf.complete_hosted,leaf.complete_component_dimensions);
        auto candidate=replay_phase_structural_replacement_authoring(source,leaf);
        std::vector<std::string> destinations;
        for (const auto& [original,proposed]:leaf.identities) { (void)original; destinations.push_back(proposed); }
        for (const auto& [key,proposed]:leaf.hosted_instance_identities) { (void)key; destinations.push_back(proposed); }
        for (const auto& [original,proposed]:leaf.dimension_identities) {
            retained_baselines.insert(original); destinations.push_back(proposed);
        }
        if (leaf.complete_hosted) {
            // Aliases are derived after the complete leaf's physical and
            // presentation replay; local instance IDs never stand in for them.
            const auto aliases=embedded_assembly_presentation_ids(candidate);
            for (const auto& [key,proposed]:leaf.hosted_instance_identities)
                destinations.push_back(aliases.at({leaf.identities.at(key.first),proposed}));
        }
        candidates.push_back(std::move(candidate)); family_fresh.push_back(std::move(destinations));
        retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
        retained_baselines.insert(plan.required_dimension_ids.begin(),plan.required_dimension_ids.end());
        if (leaf.complete_component_dimensions)
            register_dimension_copies(plan.required_hosted_instance_ids,leaf.identities,leaf.hosted_instance_identities,leaf.dimension_identities);
        if (lanes.complete_family_component_dimensions) collect_ordinary_dimension_hosts(candidates.back(),{"column","beam"});
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
        collect_ordinary_dimension_hosts(candidates.back(),{"column","beam"});
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
        for (const auto& [original,proposed]:leaf.dimension_identities) {
            retained_baselines.insert(original);destinations.push_back(proposed);
        }
        if (!leaf.hosted_instance_identities.empty()) {
            const auto aliases=embedded_assembly_presentation_ids(candidate);
            for (const auto& [key,proposed]:leaf.hosted_instance_identities)
                destinations.push_back(aliases.at({leaf.identities.at(key.first),proposed}));
        }
        candidates.push_back(std::move(candidate)); family_fresh.push_back(std::move(destinations));
        if (lanes.complete_family_component_dimensions) {
            if (leaf.complete_component_dimensions) {
                const auto plan=inspect_phase_stair_replacement_plan(source,leaf);
                register_dimension_copies(plan.required_hosted_instance_ids,leaf.identities,
                    leaf.hosted_instance_identities,leaf.dimension_identities);
            }
            collect_ordinary_dimension_hosts(candidates.back(),{"stair","railing"});
        }
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
        // The stair producer discovers actual attached rails from source, so
        // their changed physical descriptors participate without selection IDs.
        collect_ordinary_dimension_hosts(candidates.back(),{"stair","railing"});
        family_fresh.emplace_back();
    }
    if (lanes.new_slab_geometry_authority) {
        std::size_t changed_families=0;
        for (const auto& candidate:candidates) {
            const auto physical=[](std::string_view type) {
                return type=="wall" || type=="roof" || type=="slab" || type=="column" ||
                    type=="beam" || type=="stair" || type=="railing";
            };
            bool changed=false;
            for (const auto& [key,entity]:source) if (physical(entity.type)) {
                const auto found=candidate.find(key);
                if (found==candidate.end() || !exact(entity,found->second)) { changed=true; break; }
            }
            if (!changed) for (const auto& [key,entity]:candidate)
                if (!source.contains(key) && physical(entity.type)) { changed=true; break; }
            if (changed) ++changed_families;
        }
        if (changed_families<2 || changed_families>5)
            invalid("Direct coordinated slab authority requires two to five changed physical families");
        if (!permitted_catalog_retirements.empty())
            invalid("Direct coordinated slab authority cannot retire hosted rows");
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
        else if (!changes.empty() && lanes.new_slab_geometry_authority && original.type=="assembly_model")
            result.at(key)=merge_coordinated_directional_catalog(original,changes);
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
    if (lanes.new_slab_geometry_authority && result.size()>100000)
        invalid("Direct coordinated slab result entity budget exceeded");
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
    std::size_t copied_dimension_count=0;
    if (lanes.new_slab_geometry_authority) {
        std::vector<std::pair<std::string,std::string>> roots=copied_dimension_roots;
        for (const auto& [root,row]:authenticated_ordinary_rows) {
            if (copied_dimension_root_destinations.contains(root))
                invalid("Coordinated ordinary and cloned component roots overlap");
            roots.push_back(root);
            if (roots.size()>4096) invalid("Coordinated combined component root budget exceeded");
            const auto& rows=result.at(root.first).properties.at("model").at("instances");
            const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& value) { return value.at("id")==root.second; });
            if (found==rows.end() || !exact_json(*found,row))
                invalid("Coordinated final ordinary row differs from its authenticated family consequence");
        }
        // Counts associations, not changed outputs: no-upsert units consume the
        // same 1000-target budget as baseline clones and changed sibling roots.
        const auto roster=inspect_embedded_assembly_dimension_clone_roster(source,roots);
        auto framed=result;
        std::vector<AssemblyDimensionTransformRequest> requests;
        std::map<std::pair<std::string,std::string>,std::size_t> requests_by_root;
        std::set<std::string,std::less<>> discovered_copies;
        for (const auto& [original,target]:roster) {
            if (!exact(source.at(original),framed.at(original)))
                invalid("Coordinated final batch requires exact original dimension envelopes");
            const std::pair root{target.catalog_id,target.instance_id};
            const auto destination=copied_dimension_root_destinations.find(root);
            const bool clone=destination!=copied_dimension_root_destinations.end();
            const auto [request,inserted]=requests_by_root.emplace(root,requests.size());
            if (inserted) {
                auto qualified=target; qualified.part_path.clear();
                auto proposed=qualified;
                if (clone) { proposed.catalog_id=destination->second.first; proposed.instance_id=destination->second.second; }
                requests.push_back({std::move(qualified),std::move(proposed),clone ?
                    AssemblyDimensionTransformMode::clone : AssemblyDimensionTransformMode::move,{}});
            }
            if (clone) {
                const auto identity=copied_dimension_identities.find(original);
                if (identity==copied_dimension_identities.end() || !framed.erase(identity->second))
                    invalid("Coordinated final clone inventory is incomplete");
                discovered_copies.insert(original);
                requests.at(request->second).fresh_dimension_ids.emplace(original,identity->second);
            } else if (copied_dimension_identities.contains(original))
                invalid("Coordinated ordinary root borrowed clone authority");
        }
        if (discovered_copies.size()!=copied_dimension_identities.size())
            invalid("Coordinated dimension identities differ from the actual combined clone roster");
        const auto dimensions=requests.empty() ? std::vector<Entity>{} :
            carry_assembly_dimension_transform_entities(source,framed,requests);
        std::vector<std::string> changed;
        std::size_t copies=0;
        for (const auto& dimension:dimensions) {
            if (source.contains(dimension.id)) changed.push_back(dimension.id);
            else {
                ++copies;
                if (!result.contains(dimension.id) || !exact(dimension,result.at(dimension.id)))
                    invalid("Coordinated final clone differs from the independent leaf contribution");
            }
        }
        if (copies!=discovered_copies.size()) invalid("Coordinated final clone output inventory differs");
        validate_ordinary_assembly_dimension_changes(source,changed);
        for (const auto& dimension:dimensions) if (source.contains(dimension.id)) result.at(dimension.id)=dimension;
    } else if (lanes.complete_family_component_dimensions) {
        // Independently discover the union from actual roots, rather than
        // treating declared mappings as association or clone authority.
        const auto roster=inspect_embedded_assembly_dimension_clone_roster(source,copied_dimension_roots);
        copied_dimension_count=roster.size();
        if (roster.size()!=copied_dimension_identities.size())
            invalid("Coordinated dimension identities differ from the actual shared clone roster");
        auto framed=result;
        std::map<std::pair<std::string,std::string>,std::size_t> requests_by_root;
        std::vector<AssemblyDimensionTransformRequest> requests;
        for (const auto& [original,target]:roster) {
            if (!copied_dimension_identities.contains(original) || !exact(source.at(original),result.at(original)))
                invalid("Coordinated dimension copies must retain each actual original dimension exactly");
            const auto& proposed=copied_dimension_identities.at(original);
            if (!framed.erase(proposed)) invalid("Coordinated dimension copy is absent from the final combined candidate");
            const std::pair root{target.catalog_id,target.instance_id};
            const auto [request,inserted]=requests_by_root.emplace(root,requests.size());
            if (inserted) {
                auto source_root=target; source_root.part_path.clear();
                const auto& destination=copied_dimension_root_destinations.at(root);
                requests.push_back({std::move(source_root),
                    {destination.first,destination.second,std::nullopt,{}},AssemblyDimensionTransformMode::clone,{}});
            }
            requests.at(request->second).fresh_dimension_ids.emplace(original,proposed);
        }
        const auto copies=requests.empty()?std::vector<Entity>{}:
            carry_assembly_dimension_transform_entities(source,framed,requests);
        if (copies.size()!=roster.size()) invalid("Coordinated final dimension copy contribution differs from source roster");
        for (const auto& copy:copies)
            if (!exact(copy,result.at(copy.id)))
                invalid("Coordinated dimension copy differs from its actual source-to-final component frame");
    }
    if (lanes.complete_hosted_component_dimensions && !lanes.new_slab_geometry_authority) {
        const std::vector<std::string> host_roster(ordinary_dimension_hosts.begin(),ordinary_dimension_hosts.end());
        auto dimensions=carry_hosted_assembly_dimension_transform_entities(source,result,host_roster,1000-copied_dimension_count);
        std::vector<std::string> changed_dimension_ids;
        changed_dimension_ids.reserve(dimensions.size());
        for (const auto& dimension:dimensions) changed_dimension_ids.push_back(dimension.id);
        validate_ordinary_assembly_dimension_changes(source,changed_dimension_ids);
        for (auto& dimension:dimensions) result.at(dimension.id)=std::move(dimension);
    }
    if (lanes.complete_family_component_dimensions)
        for (const auto& [original,proposed]:copied_dimension_identities) {
            (void)proposed;
            if (!exact(source.at(original),result.at(original)))
                invalid("Coordinated ordinary contribution changed a protected original copied dimension");
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

nlohmann::json encode_phase_constraint_authoring_child_intent(const ConstraintAuthoringIntent& value) {
    return encode_intent(value);
}

const nlohmann::json& phase_constraint_coordinated_authoring_body(const Json& value) {
    if (value.is_object() && value.contains("version") && value.at("version")==10) {
        resource_shape(value);
        if (value.dump().size()>proof_budget) invalid("Coordinated replacement proof byte budget exceeded");
        for (const auto* key:{"version","wall_authoring","roof_replacement","slab_replacement",
            "structural_replacement","stair_replacement","ordinary_roof_edits","ordinary_slab_geometry",
            "ordinary_structural_edits","ordinary_stair_transforms","complete_hosted_component_dimensions"})
            if (!value.contains(key)) invalid("Direct coordinated slab authoring requires its closed fields");
        const std::set<std::string,std::less<>> fields{"version","wall_authoring","roof_replacement","slab_replacement",
            "structural_replacement","stair_replacement","ordinary_roof_edits","ordinary_slab_geometry",
            "ordinary_structural_edits","ordinary_stair_transforms"};
        const std::set<std::string,std::less<>> flags{"complete_hosted_component_dimensions","complete_wall_component_dimensions",
            "complete_stair_component_dimensions","complete_roof_component_dimensions",
            "complete_slab_component_dimensions","complete_structural_component_dimensions"};
        for (const auto& [key,field]:value.items()) if (!fields.contains(key) &&
            (!flags.contains(key) || !field.is_boolean() || field!=true))
            invalid("Direct coordinated slab authoring requires known literal true flags and no wrapper");
        if (!value.at("version").is_number_integer()) invalid("Direct coordinated slab version must be integral");
        for (const auto* key:{"wall_authoring","roof_replacement","slab_replacement",
            "structural_replacement","stair_replacement"})
            if (!value.at(key).is_null() && !value.at(key).is_object())
                invalid("Direct coordinated slab children must be nullable objects");
        for (const auto* key:{"ordinary_roof_edits","ordinary_slab_geometry",
            "ordinary_structural_edits","ordinary_stair_transforms"}) bounded_array(value.at(key));
        const auto child=[&](const char* field,std::initializer_list<int> versions) {
            const auto& leaf=value.at(field);
            return leaf.is_object() && leaf.contains("version") && leaf.at("version").is_number_integer() &&
                std::find(versions.begin(),versions.end(),leaf.at("version").get<int>())!=versions.end();
        };
        const auto& wall=value.at("wall_authoring");
        const bool wall_copies=wall.is_object() && wall.contains("version") && wall.at("version")==36 &&
            wall.contains("wall_edit_version") && wall.at("wall_edit_version")==2 &&
            wall.contains("wall_replacement") && wall.at("wall_replacement").is_object() &&
            wall.at("wall_replacement").contains("version") && wall.at("wall_replacement").at("version")==12;
        if (value.contains("complete_wall_component_dimensions")!=wall_copies ||
            value.contains("complete_stair_component_dimensions")!=child("stair_replacement",{8}) ||
            value.contains("complete_roof_component_dimensions")!=child("roof_replacement",{11}) ||
            value.contains("complete_slab_component_dimensions")!=child("slab_replacement",{8,10,12}) ||
            value.contains("complete_structural_component_dimensions")!=child("structural_replacement",{5}))
            invalid("Direct coordinated slab flags must match actual complete children exactly");
        return value;
    }
    if (!value.is_object() || !value.contains("version") ||
        (value.at("version")!=6 && value.at("version")!=7 && value.at("version")!=8 && value.at("version")!=9)) return value;
    resource_shape(value);
    if (value.dump().size()>proof_budget) invalid("Coordinated replacement proof byte budget exceeded");
    if (value.at("version")==9) {
        if (!value.at("version").is_number_integer() || !value.contains("authoring") ||
            !value.contains("complete_hosted_component_dimensions"))
            invalid("Coordinated family dimension authoring requires its closed wrapper");
        for (const auto& [key,field]:value.items()) {
            if (key=="version" || key=="authoring") continue;
            if ((key!="complete_hosted_component_dimensions" && key!="complete_wall_component_dimensions" &&
                 key!="complete_stair_component_dimensions" && key!="complete_roof_component_dimensions" &&
                 key!="complete_slab_component_dimensions" && key!="complete_structural_component_dimensions") ||
                !field.is_boolean() || field!=true)
                invalid("Coordinated family dimension wrapper requires known literal true authority");
        }
        const auto& body=value.at("authoring");
        if (!body.is_object() || !body.contains("version") || !body.at("version").is_number_integer() ||
            (body.at("version")!=4 && body.at("version")!=5))
            invalid("Coordinated family dimension authoring requires nonnested body four or five");
        const auto actual_child=[&](const char* field,int version) {
            return body.contains(field) && body.at(field).is_object() && body.at(field).contains("version") &&
                body.at(field).at("version").is_number_integer() && body.at(field).at("version")==version;
        };
        const bool roof=actual_child("roof_replacement",11);
        const bool slab=actual_child("slab_replacement",8);
        const bool structural=actual_child("structural_replacement",5);
        if (!roof && !slab && !structural)
            invalid("Coordinated family dimension authoring requires an actual new roof, slab or structural child");
        if (value.contains("complete_roof_component_dimensions")!=roof ||
            value.contains("complete_slab_component_dimensions")!=slab ||
            value.contains("complete_structural_component_dimensions")!=structural ||
            value.contains("complete_stair_component_dimensions")!=actual_child("stair_replacement",8))
            invalid("Coordinated family dimension flags must match actual child authority exactly");
        const bool wall=body.contains("wall_authoring") && body.at("wall_authoring").is_object() &&
            body.at("wall_authoring").contains("wall_replacement") && !body.at("wall_authoring").at("wall_replacement").is_null();
        if (value.contains("complete_wall_component_dimensions")!=wall || (body.at("version")==5)!=wall)
            invalid("Coordinated family dimension body requires qualified wall authority exactly for an actual replacement");
        return body;
    }
    const bool wall_copies=value.at("version")==8;
    const bool stair_copies=value.at("version")==7 ||
        (wall_copies && value.contains("complete_stair_component_dimensions"));
    if (wall_copies && stair_copies) keys(value,{"version","authoring","complete_hosted_component_dimensions",
        "complete_wall_component_dimensions","complete_stair_component_dimensions"});
    else if (wall_copies) keys(value,{"version","authoring","complete_hosted_component_dimensions","complete_wall_component_dimensions"});
    else if (stair_copies) keys(value,{"version","authoring","complete_hosted_component_dimensions","complete_stair_component_dimensions"});
    else keys(value,{"version","authoring","complete_hosted_component_dimensions"});
    if (!value.at("version").is_number_integer() ||
        !value.at("complete_hosted_component_dimensions").is_boolean() ||
        value.at("complete_hosted_component_dimensions")!=true)
        invalid("Coordinated dimension authoring requires literal complete hosted dimension authority");
    if (wall_copies && (!value.at("complete_wall_component_dimensions").is_boolean() ||
        value.at("complete_wall_component_dimensions")!=true))
        invalid("Coordinated wall dimension copying requires literal explicit authority");
    if (stair_copies && (!value.at("complete_stair_component_dimensions").is_boolean() ||
        value.at("complete_stair_component_dimensions")!=true))
        invalid("Coordinated stair dimension copying requires literal explicit authority");
    const auto& body=value.at("authoring");
    if (!body.is_object() || !body.contains("version") || !body.at("version").is_number_integer() ||
        (body.at("version")!=1 && body.at("version")!=2 && body.at("version")!=3 &&
         body.at("version")!=4 && body.at("version")!=5))
        invalid("Coordinated dimension authoring requires one historical inner dialect without nesting");
    if (stair_copies && body.at("version")!=4 && body.at("version")!=5)
        invalid("Coordinated stair dimension copying requires a body with actual stair authority");
    if (wall_copies) {
        if (body.at("version")!=5 || !body.contains("stair_replacement"))
            invalid("Coordinated wall dimension copying requires the qualified wall body dialect");
        const auto& stair=body.at("stair_replacement");
        const bool complete_stair=stair.is_object() && stair.contains("version") &&
            stair.at("version").is_number_integer() && stair.at("version")==8;
        if (stair_copies!=complete_stair)
            invalid("Coordinated wall dimensions require stair completion exactly for an actual child8");
    }
    return body;
}

nlohmann::json& phase_constraint_coordinated_authoring_body(Json& value) {
    const auto& body=phase_constraint_coordinated_authoring_body(static_cast<const Json&>(value));
    return &body==&value ? value : value.at("authoring");
}

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
        static_cast<int>(!value.component_demolition.is_null()) +
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
    if (value.complete_ordinary_roof_component_dimensions && value.ordinary_roof_edits.is_null())
        invalid("Ordinary roof component dimension completion requires standalone typed roof edits");
    if (value.intent.wall_group_scale) {
        ConstraintAuthoringIntent only_scale;
        only_scale.wall_group_scale=value.intent.wall_group_scale;
        only_scale.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(only_scale))
            invalid("Wall group scale cannot borrow sibling geometry or relationship authority");
    }
    const auto coordinated_lanes=value.coordinated_replacements.is_null()?std::optional<CoordinatedReplacements>{}:
        std::optional<CoordinatedReplacements>{coordinated(value.coordinated_replacements,value)};
    const auto coordinated_version=coordinated_lanes?coordinated_lanes->version:0;
    const bool coordinated_dimensions=coordinated_lanes && coordinated_lanes->complete_hosted_component_dimensions;
    const bool coordinated_wall_dimensions=coordinated_lanes && coordinated_lanes->complete_wall_component_dimensions;
    const bool coordinated_stair_dimensions=coordinated_lanes && coordinated_lanes->complete_stair_component_dimensions;
    Json result={{"version",!value.component_demolition.is_null()?30:baseline_wall_group_scale?19:value.intent.wall_group_scale?18:!value.ordinary_roof_edits.is_null()?17:!value.wall_demolition.is_null()?16:!value.coordinated_demolition.is_null()?15:!value.stair_demolition_retirement.is_null()?13:!value.stair_replacement.is_null()?12:!value.stair_demolition.is_null()?11:!value.structural_replacement.is_null()?9:coordinated_dimensions?31:coordinated_version==5?29:coordinated_version==4?14:coordinated_version==3?10:coordinated_version==2?8:coordinated_version==1?7:!value.slab_demolition.is_null()?6:!value.slab_replacement.is_null()?5:!value.roof_replacement.is_null()?4:!value.opening_demolition.is_null()?3:value.wall_replacement.is_null()?1:2},{"expected_revision",value.expected_revision},
        {"source_snapshot_digest",value.source_snapshot_digest},{"source_authoring_digest",value.source_authoring_digest},
        {"source_entities_digest",value.source_entities_digest},
        {"source_saved_revision",value.source_saved_revision ? Json(*value.source_saved_revision) : Json(nullptr)},
        {"phase_selections",value.phase_selections},{"intent",encode_intent(value.intent)}};
    if (joint_similarity) result["version"]=22;
    if (coordinated_lanes && coordinated_lanes->new_slab_geometry_authority) result["version"]=46;
    else if (coordinated_lanes && coordinated_lanes->complete_family_component_dimensions) result["version"]=41;
    else if (coordinated_wall_dimensions) result["version"]=37;
    else if (coordinated_stair_dimensions) result["version"]=34;
    if (!value.component_demolition.is_null()) {
        const auto demolition=decode_assembly_component_phase_demolition_intent(value.component_demolition);
        const auto canonical=encode_assembly_component_phase_demolition_intent(demolition);
        if (canonical.dump()!=value.component_demolition.dump())
            invalid("Component demolition decisions are not canonical");
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Component demolition cannot borrow geometry or relationship authority");
        result["component_demolition"]=canonical;
    }
    if (!value.wall_replacement.is_null()) {
        const auto replacement=decode_phase_wall_replacement_authoring(value.wall_replacement);
        if (encode_phase_wall_replacement_authoring(replacement).dump()!=value.wall_replacement.dump())
            invalid("Wall replacement decisions are not canonical");
        if (replacement.complete_scalar_profiles) result["version"]=replacement.complete_vertical_profiles ? 26 : 23;
        const auto& hosted_body=replacement.complete_component_dimensions ? value.wall_replacement.at("authoring") : value.wall_replacement;
        const auto& wall_body=replacement.complete_hosted_instances ? hosted_body.at("authoring") : hosted_body;
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
            result["version"]=replacement.complete_component_dimensions ? 36 : 28;
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
        const auto& roof_body=replacement.complete_component_dimensions ? value.roof_replacement.at("authoring") : value.roof_replacement;
        if (roof_body.at("version") == 10) result["version"] = 21;
        if (replacement.complete_component_dimensions) {
            result["roof_edit_version"]=result.at("version");
            result["version"]=38;
        }
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
        if (replacement.coordinate_hosted_directional_geometry) result["version"] = 42;
        // Source-preserving mapped geometry owns its own lane regardless of
        // whether a directional resize leaf is present.
        if (replacement.preserve_slab_planar_source_geometry) result["version"] = 44;
        if (replacement.complete_component_dimensions) {
            result["slab_edit_version"]=result.at("version");
            result["version"]=replacement.preserve_slab_planar_source_geometry ? 45 :
                replacement.coordinate_hosted_directional_geometry ? 43 : 39;
        }
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
        const auto& structural_body=replacement.complete_component_dimensions ? value.structural_replacement.at("authoring") : value.structural_replacement;
        if (structural_body.at("version")==4) result["version"]=24;
        if (replacement.complete_component_dimensions) {
            result["structural_edit_version"]=result.at("version");
            result["version"]=40;
        }
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
        if (replacement.complete_ordinary_component_dimensions) result["version"]=35;
        else if (replacement.complete_component_dimensions) result["version"]=33;
        else if (replacement.complete_scalar_dependencies) result["version"]=25;
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
        if (value.complete_ordinary_roof_component_dimensions) {
            result["version"]=32;
            result["complete_ordinary_roof_component_dimensions"]=true;
        } else if (has_rotated_roof_edits(value.ordinary_roof_edits)) result["version"] = 20;
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
    const bool wall_component_dimensions=value.is_object() && value.contains("version") && value.at("version")==36;
    const bool hosted_walls=wall_component_dimensions ||
        (value.is_object() && value.contains("version") && value.at("version")==28);
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
    const bool roof_dimensions=value.is_object() && value.contains("version") && value.at("version")==38;
    if (roof_dimensions && (!value.contains("roof_edit_version") || !value.at("roof_edit_version").is_number_integer() ||
        (value.at("roof_edit_version")!=4 && value.at("roof_edit_version")!=21)))
        invalid("Roof dimension copies require an explicit historical roof edit lane");
    const bool roof_rotation=roof_dimensions ? value.at("roof_edit_version")==21 :
        value.is_object() && value.contains("version") && value.at("version")==21;
    const bool roof=roof_dimensions || roof_rotation || (value.is_object() && value.contains("version") && value.at("version")==4);
    const bool directional_slab_dimensions=value.is_object() && value.contains("version") && value.at("version")==43;
    const bool mapped_slab_dimensions=value.is_object() && value.contains("version") && value.at("version")==45;
    const bool slab_dimensions=directional_slab_dimensions || mapped_slab_dimensions ||
        (value.is_object() && value.contains("version") && value.at("version")==39);
    if (slab_dimensions && (!value.contains("slab_edit_version") || !value.at("slab_edit_version").is_number_integer() ||
        (mapped_slab_dimensions ? value.at("slab_edit_version")!=44 :
            directional_slab_dimensions ? value.at("slab_edit_version")!=42 :
            (value.at("slab_edit_version")!=5 && value.at("slab_edit_version")!=27))))
        invalid("Slab dimension copies require their exact historical, directional or mapped slab edit lane");
    const bool vertical_slabs=slab_dimensions ? value.at("slab_edit_version")==27 :
        value.is_object() && value.contains("version") && value.at("version")==27;
    const bool directional_slabs=directional_slab_dimensions ||
        (value.is_object() && value.contains("version") && value.at("version")==42);
    const bool mapped_slabs=mapped_slab_dimensions ||
        (value.is_object() && value.contains("version") && value.at("version")==44);
    const bool slab=slab_dimensions || vertical_slabs || directional_slabs || mapped_slabs ||
        (value.is_object() && value.contains("version") && value.at("version")==5);
    const bool slab_demolition=value.is_object() && value.contains("version") && value.at("version")==6;
    const bool coordinated_replacements=value.is_object() && value.contains("version") &&
        (value.at("version")==7 || value.at("version")==8 || value.at("version")==10 || value.at("version")==14 || value.at("version")==29 || value.at("version")==31 || value.at("version")==34 || value.at("version")==37 || value.at("version")==41 || value.at("version")==46);
    const bool structural_dimensions=value.is_object() && value.contains("version") && value.at("version")==40;
    if (structural_dimensions && (!value.contains("structural_edit_version") || !value.at("structural_edit_version").is_number_integer() ||
        (value.at("structural_edit_version")!=9 && value.at("structural_edit_version")!=24)))
        invalid("Structural dimension copies require an explicit historical structural edit lane");
    const bool scalar_structural=structural_dimensions ? value.at("structural_edit_version")==24 :
        value.is_object() && value.contains("version") && value.at("version")==24;
    const bool structural=structural_dimensions || scalar_structural || (value.is_object() && value.contains("version") && value.at("version")==9);
    const bool stair_demolition=value.is_object() && value.contains("version") && value.at("version")==11;
    const bool scalar_stairs=value.is_object() && value.contains("version") && value.at("version")==25;
    const bool ordinary_stair_dimensions=value.is_object() && value.contains("version") && value.at("version")==35;
    const bool stair_dimensions=ordinary_stair_dimensions ||
        (value.is_object() && value.contains("version") && value.at("version")==33);
    const bool stair_replacement=stair_dimensions || scalar_stairs || (value.is_object() && value.contains("version") && value.at("version")==12);
    const bool stair_retirement=value.is_object() && value.contains("version") && value.at("version")==13;
    const bool coordinated_demolition=value.is_object() && value.contains("version") && value.at("version")==15;
    const bool wall_demolition=value.is_object() && value.contains("version") && value.at("version")==16;
    const bool ordinary_roof_rotations=value.is_object() && value.contains("version") && value.at("version")==20;
    const bool ordinary_roof_dimensions=value.is_object() && value.contains("version") && value.at("version")==32;
    const bool ordinary_roofs=ordinary_roof_dimensions || ordinary_roof_rotations ||
        (value.is_object() && value.contains("version") && value.at("version")==17);
    const bool wall_group_scale=value.is_object() && value.contains("version") && value.at("version")==18;
    const bool baseline_wall_group_scale=wall_edit_version==19;
    const bool component_demolition=value.is_object() && value.contains("version") && value.at("version")==30;
    if (component_demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","component_demolition"});
    else if (hosted_walls) keys(value,{"version","wall_edit_version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","wall_replacement"});
    else if (replacement || baseline_wall_group_scale) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","wall_replacement"});
    else if (demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","opening_demolition"});
    else if (roof_dimensions) keys(value,{"version","roof_edit_version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","roof_replacement"});
    else if (roof) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","roof_replacement"});
    else if (slab_dimensions) keys(value,{"version","slab_edit_version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","slab_replacement"});
    else if (slab) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","slab_replacement"});
    else if (slab_demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","slab_demolition"});
    else if (coordinated_replacements) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","coordinated_replacements"});
    else if (structural_dimensions) keys(value,{"version","structural_edit_version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","structural_replacement"});
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
    else if (ordinary_roof_dimensions) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","ordinary_roof_edits",
        "complete_ordinary_roof_component_dimensions"});
    else if (ordinary_roofs) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","ordinary_roof_edits"});
    else keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent"});
    if (!value.at("version").is_number_integer() || value.at("version")<1 || value.at("version")>46 ||
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
            leaf.complete_hosted_instances!=hosted_walls || leaf.complete_component_dimensions!=wall_component_dimensions)
            invalid("Wall scalar cohort outer and leaf versions do not match");
    }
    if (demolition) {
        result.opening_demolition = value.at("opening_demolition");
        if (result.opening_demolition.is_null()) invalid("Version three requires opening demolition decisions");
        (void)decode_phase_opening_demolition_intent(result.opening_demolition);
    }
    if (component_demolition) {
        result.component_demolition=value.at("component_demolition");
        if (result.component_demolition.is_null()) invalid("Version thirty requires component demolition decisions");
        (void)decode_assembly_component_phase_demolition_intent(result.component_demolition);
    }
    if (roof) {
        result.roof_replacement = value.at("roof_replacement");
        if (result.roof_replacement.is_null()) invalid("Roof replacement dialect requires replacement decisions");
        const auto leaf=decode_phase_roof_replacement_authoring(result.roof_replacement);
        const auto& body=leaf.complete_component_dimensions ? result.roof_replacement.at("authoring") : result.roof_replacement;
        if (leaf.complete_component_dimensions!=roof_dimensions || (body.at("version") == 10) != roof_rotation)
            invalid("Roof replacement outer and rotation leaf versions do not match");
    }
    if (slab) {
        result.slab_replacement = value.at("slab_replacement");
        if (result.slab_replacement.is_null()) invalid("Version five requires slab replacement decisions");
        const auto leaf=decode_phase_slab_replacement_authoring(result.slab_replacement);
        // Mapped lanes 44/45 own their child's directional flag; historical
        // 5/27/39/42/43 lanes can never contain source-preserving leaves.
        if (leaf.complete_component_dimensions!=slab_dimensions || leaf.coordinate_hosted_profiles!=vertical_slabs ||
            leaf.preserve_slab_planar_source_geometry!=mapped_slabs ||
            (!mapped_slabs && leaf.coordinate_hosted_directional_geometry!=directional_slabs))
            invalid("Slab outer and geometry/profile/dimension leaf versions do not match");
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
        const bool direct_slab=value.at("version")==46;
        const bool family_dimensions=value.at("version")==41 || direct_slab;
        const bool wall_dimensions=value.at("version")==37;
        if (direct_slab!=inner.new_slab_geometry_authority ||
            family_dimensions!=inner.complete_family_component_dimensions ||
            (value.at("version")==31 || value.at("version")==34 || wall_dimensions || family_dimensions)!=inner.complete_hosted_component_dimensions ||
            (!family_dimensions && wall_dimensions!=inner.complete_wall_component_dimensions) ||
            (!family_dimensions && !wall_dimensions && (value.at("version")==34)!=inner.complete_stair_component_dimensions) ||
            (value.at("version")==7 && inner.version!=1) || (value.at("version")==8 && inner.version!=2) ||
            (value.at("version")==10 && inner.version!=3) || (value.at("version")==14 && inner.version!=4) ||
            (value.at("version")==29 && inner.version!=5))
            invalid("Coordinated outer and inner authoring versions do not match");
    }
    if (structural) {
        result.structural_replacement=value.at("structural_replacement");
        if (result.structural_replacement.is_null()) invalid("Version nine requires structural replacement decisions");
        const auto leaf=decode_phase_structural_replacement_authoring(result.structural_replacement);
        const auto& body=leaf.complete_component_dimensions ? result.structural_replacement.at("authoring") : result.structural_replacement;
        if (leaf.complete_component_dimensions!=structural_dimensions || (body.at("version")==4)!=scalar_structural)
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
        if (leaf.complete_component_dimensions!=stair_dimensions ||
            leaf.complete_ordinary_component_dimensions!=ordinary_stair_dimensions ||
            (!stair_dimensions && leaf.complete_scalar_dependencies!=scalar_stairs))
            invalid("Stair dimension/scalar authority requires its matching outer version");
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
        if (ordinary_roof_dimensions) {
            if (!value.at("complete_ordinary_roof_component_dimensions").is_boolean() ||
                value.at("complete_ordinary_roof_component_dimensions")!=true)
                invalid("Ordinary roof component dimension completion requires literal true authority");
            result.complete_ordinary_roof_component_dimensions=true;
        }
        (void)ordinary_roof_edits(result.ordinary_roof_edits, ordinary_roof_rotations || ordinary_roof_dimensions);
        if (!ordinary_roof_dimensions && has_rotated_roof_edits(result.ordinary_roof_edits) != ordinary_roof_rotations)
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
        std::vector<PhaseConstraintAuthoringIntent> result;
        if (!demolition.other_authoring.is_null())
            result=phase_constraint_replacement_components(
                decode_phase_constraint_authoring_intent(demolition.other_authoring));
        if (demolition.complete_component_phase_memberships && !demolition.component_authoring.is_null())
            result.push_back(decode_phase_constraint_authoring_intent(demolition.component_authoring));
        return result;
    }
    if (!intent.coordinated_demolition.is_null())
        return phase_coordinated_demolition_components(intent.coordinated_demolition,intent);
    // Standalone phase nineteen returns its complete scale/replacement pair,
    // so original-preservation guards inspect its actual replacement plan.
    // Component demolition also returns itself: it reserves no fresh roots
    // and provides no copied stage or physical replacement authority.
    if (intent.coordinated_replacements.is_null()) return {intent};
    std::vector<PhaseConstraintAuthoringIntent> result;
    const auto lanes=coordinated(intent.coordinated_replacements,intent);
    const auto& body=phase_constraint_coordinated_authoring_body(intent.coordinated_replacements);
    if (lanes.wall && !lanes.wall->wall_replacement.is_null()) result.push_back(*lanes.wall);
    for (const auto* family:{"roof_replacement","slab_replacement","structural_replacement","stair_replacement"}) {
        if (!body.contains(family)) continue;
        const auto& leaf=body.at(family);
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
    const DocumentSnapshot& source,const ConstraintAuthoringIntent& intent,const Json& component_demolition,
    bool complete_component_membership_work) {
    if (!component_demolition.is_null() || complete_component_membership_work)
        admit_component_demolition_source_catalogs(source.entities());
    PhaseConstraintAuthoringIntent result{source.revision(),document_snapshot_digest(source),
        document_authoring_source_digest_v2(source),entity_map_digest(source.entities()),
        source.saved_revision_optional(),phase_constraint_authoring_selections(source.entities()),intent};
    result.component_demolition=component_demolition;
    (void)encode_phase_constraint_authoring_intent(result); return result;
}

Entities replay_phase_constraint_authoring(const Entities& source,const Json& proof) {
    const auto decoded=decode_phase_constraint_authoring_intent(proof);
    if (decoded.complete_ordinary_roof_component_dimensions) {
        coordinated_map_budget(source);
        // Saved phase scope and the typed roof producer both resolve actual
        // component memberships, so admit all catalogs before either discovery.
        admit_component_demolition_source_catalogs(source);
    }
    if (!decoded.component_demolition.is_null()) admit_component_demolition_source_catalogs(source);
    if (!decoded.wall_demolition.is_null() &&
        phase_wall_demolition_complete_component_phase_memberships(decoded.wall_demolition))
        preflight_phase_wall_demolition_component_catalogs(source,decoded.wall_demolition,decoded);
    if (!decoded.coordinated_demolition.is_null() &&
        phase_coordinated_demolition_complete_component_phase_memberships(decoded.coordinated_demolition,decoded))
        preflight_phase_coordinated_demolition_component_catalogs(source,decoded.coordinated_demolition,decoded);
    if (decoded.source_entities_digest!=entity_map_digest(source) ||
        decoded.phase_selections!=phase_constraint_authoring_selections(source))
        invalid("Phase constraint proof does not describe the actual source entities and saved choices");
    if (!decoded.component_demolition.is_null())
        return replay_assembly_component_phase_demolition(source,
            decode_assembly_component_phase_demolition_intent(decoded.component_demolition));
    if (!decoded.ordinary_roof_edits.is_null()) {
        const auto edits=ordinary_roof_edits(decoded.ordinary_roof_edits, true);
        const auto partition=partition_phase_roof_geometry_edits(source,edits);
        if (partition.replacement || !partition.baseline_roof_edits.empty())
            invalid("Ordinary roof authoring requires actual ordinary/proposed membership");
        auto candidate=replay_roof_edit_entities(source,edits);
        if (!decoded.complete_ordinary_roof_component_dimensions) return candidate;
        coordinated_map_budget(candidate);
        const auto scope=constraint_phase_scope(source);
        std::set<std::string,std::less<>> protected_baselines;
        for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
            (void)key;
            const auto model=ModelPhases::from_json(entity.properties.at("model"));
            if (model.active_alternative())
                protected_baselines.insert(model.baseline_ids().begin(),model.baseline_ids().end());
        }
        std::vector<std::string> hosts;
        for (const auto& [key,original]:source) {
            const auto found=candidate.find(key);
            if (can_recognize_boundary_dimension_entity_type(original.type) &&
                is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(original).format) &&
                (found==candidate.end() || !exact(original,found->second)))
                invalid("Ordinary roof candidates must retain original component dimension rows");
            if (original.type!="roof") continue;
            if (found==candidate.end() || found->second.id!=key || found->second.type!=original.type)
                invalid("Ordinary roof dimension hosts must retain actual owner identity and type");
            if (exact(original,found->second)) continue;
            if (scope.inactive_owner_ids.contains(key) || protected_baselines.contains(key))
                invalid("Ordinary roof dimension hosts require active nonbaseline authority");
            if (hosts.size()>=100000) invalid("Ordinary roof dimension host roster budget exceeded");
            hosts.push_back(key);
        }
        auto dimensions=carry_hosted_assembly_dimension_transform_entities(source,candidate,hosts);
        std::vector<std::string> changed_dimension_ids;
        changed_dimension_ids.reserve(dimensions.size());
        for (const auto& dimension:dimensions) changed_dimension_ids.push_back(dimension.id);
        validate_ordinary_assembly_dimension_changes(source,changed_dimension_ids);
        for (auto& dimension:dimensions) candidate.at(dimension.id)=std::move(dimension);
        coordinated_map_budget(candidate);
        return candidate;
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
    bool profile_dialect=false;
    bool mapped_dialect=false;
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
                !((before=="sketch.assemblies.v3" || before=="sketch.assemblies.v4" ||
                    before=="sketch.assemblies.v5" || before=="sketch.assemblies.v6" ||
                    before=="sketch.assemblies.v7") && after=="sketch.assemblies.v8") &&
                !((before=="sketch.assemblies.v3" || before=="sketch.assemblies.v4" ||
                    before=="sketch.assemblies.v5" || before=="sketch.assemblies.v6" ||
                    before=="sketch.assemblies.v7" || before=="sketch.assemblies.v8") && after=="sketch.assemblies.v9") &&
                !(allow_legacy_assembly_promotion && (before=="sketch.assemblies.v1" || before=="sketch.assemblies.v2") &&
                    (after=="sketch.assemblies.v7" || after=="sketch.assemblies.v8" || after=="sketch.assemblies.v9")))
                invalid("Mixed edit catalog has an unsupported dialect change");
            promoted=true;
            profile_dialect=profile_dialect || after=="sketch.assemblies.v8";
            mapped_dialect=mapped_dialect || after=="sketch.assemblies.v9";
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
    if (promoted && (!changed.empty() || !suffix.empty())) result.properties.at("model").at("schema")=
        mapped_dialect ? "sketch.assemblies.v9" : profile_dialect ? "sketch.assemblies.v8" : "sketch.assemblies.v7";
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
    deletion=prune_embedded_assembly_drawing_contexts(deletion);
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
        normalized.back()=prune_embedded_assembly_drawing_contexts(normalized.back());
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

void validate_mixed_edit_fresh_destinations(const Entities& actual,
    const std::vector<Entities>& candidates,bool first_lane_cannot_allocate=false) {
    const auto original_owners=mixed_edit_identity_owners(actual);
    const auto original_aliases=embedded_assembly_presentation_ids(actual);
    std::set<std::string,std::less<>> source_aliases;
    for (const auto& [key,alias]:original_aliases) { (void)key; source_aliases.insert(alias); }
    std::set<std::string,std::less<>> fresh;
    std::set<std::pair<std::string,std::string>> fresh_instances;
    for (const auto& candidate:candidates) {
        coordinated_map_budget(candidate);
        std::set<std::string,std::less<>> lane_fresh;
        for (const auto& [token,owners]:mixed_edit_identity_owners(candidate)) {
            const auto found=original_owners.find(token);
            if (found!=original_owners.end()) {
                for (const auto& owner:owners) if (!found->second.contains(owner))
                    invalid("Mixed edit reuses an actual owned identity for a different owner");
                continue;
            }
            if (first_lane_cannot_allocate && &candidate==&candidates.front())
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
}

Entities compose_mixed_selection_edit_candidates_impl(const Entities& actual,
    const std::vector<Entities>& independently_admitted_candidates,
    const std::set<std::string,std::less<>>* retired_rooms=nullptr,
    const std::set<std::string,std::less<>>* removed_annotation_targets=nullptr,
    const StairPropertyReviewPreparation* stair_review=nullptr,
    const std::set<std::string,std::less<>>* assembly_promoted_catalogs=nullptr,
    const PhasePropertyCompositionRetirements* phase_properties=nullptr,
    const AssemblyPropertyRowAdmission* assembly_row_edits=nullptr,
    bool physical_only=false) {
    coordinated_map_budget(actual);
    if (physical_only && (retired_rooms || removed_annotation_targets || stair_review || phase_properties || assembly_row_edits))
        invalid("Physical-only resize union cannot receive reviewed retirement authority");
    if (independently_admitted_candidates.empty() || independently_admitted_candidates.size()>
        (physical_only ? 3u : phase_properties ? 2u : stair_review ? 6u : 5u))
        invalid("Mixed edit composition requires one to five independently admitted actual-source candidates");
    for (const auto& candidate:independently_admitted_candidates) coordinated_map_budget(candidate);
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
    validate_mixed_edit_fresh_destinations(actual,independently_admitted_candidates,stair_review!=nullptr);
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
            if (physical_only) invalid("Physical-only resize union cannot retire or erase source owners");
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
                deletion=prune_embedded_assembly_drawing_contexts(deletion);
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
        else if (original.type==assembly_phase_membership_entity_type && (physical_only || (phase_properties && assembly_row_edits)))
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
        if ((before=="sketch.assemblies.v1" || before=="sketch.assemblies.v2") &&
            (after=="sketch.assemblies.v7" || after=="sketch.assemblies.v8" || after=="sketch.assemblies.v9"))
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
            (after.at("schema")=="sketch.assemblies.v7" || after.at("schema")=="sketch.assemblies.v8" ||
                after.at("schema")=="sketch.assemblies.v9")) promoted.insert(id);
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
    if (!((version>=5 && version<=13) || version==15 || version==21 || version==24 || version==27) || !wire.at("room_reviews").empty() ||
        (wire.contains("phase_room_reviews") && !wire.at("phase_room_reviews").empty()) ||
        (wire.contains("assembly_request") && !wire.at("assembly_request").is_null()))
        invalid("Component Properties base must be an unreviewed admitted noncomponent contribution");
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

using ComponentTarget=AssemblyInstancePropertyEditTarget;
ComponentTarget component_root(ComponentTarget target) { target.part_path.clear(); return target; }

AssemblyPropertyRowAdmission component_property_row_admission(const Entities& original,const Entities& admitted,
    std::set<std::string,std::less<>>& promoted) {
    AssemblyPropertyRowAdmission result;
    for (const auto& [id,entity]:original) if (entity.type=="assembly_model") {
        const auto& before=entity.properties.at("model");
        const auto& after=admitted.at(id).properties.at("model");
        if ((before.at("schema")=="sketch.assemblies.v1" || before.at("schema")=="sketch.assemblies.v2") &&
            (after.at("schema")=="sketch.assemblies.v7" || after.at("schema")=="sketch.assemblies.v8" ||
                after.at("schema")=="sketch.assemblies.v9")) promoted.insert(id);
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

namespace {
// Only source-reconstructed physical lanes enter this shared finalization.
// Provisional carry is discarded before the one original-to-final batch.
CoordinatedComponentPropertyComposition finalize_component_directional_resize(
    const DocumentSnapshot& source, const PhaseAssemblyInstancePropertyEditPreparation& replayed,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring, const Entities& physical,
    const std::map<ComponentTarget,ComponentTarget>& destinations,
    const PhaseSlabReplacementIdentityMap& physical_dimension_ids) {
    const auto& actual=source.entities();
    CoordinatedComponentPropertyComposition result;
    validate_mixed_edit_fresh_destinations(actual,{physical,replayed.preview_entities()});
    std::set<std::string,std::less<>> promoted;
    auto rows=component_property_row_admission(actual,replayed.preview_entities(),promoted);
    const auto physical_rows=component_property_row_admission(actual,physical,promoted);
    for (const auto& [binding,row]:physical_rows.changed_rows) {
        const auto [entry,inserted]=rows.changed_rows.emplace(binding,row);
        if (!inserted && !exact_json(entry->second,row)) invalid("Mixed resize has conflicting independently replayed catalog rows");
    }
    rows.catalogs.insert(physical_rows.catalogs.begin(),physical_rows.catalogs.end());
    PhasePropertyCompositionRetirements retirements;
    retirements.erased.insert(replayed.result().retired_dimension_ids.begin(),replayed.result().retired_dimension_ids.end());
    result.entities=compose_mixed_selection_edit_candidates_impl(actual,{physical,replayed.preview_entities()},
        nullptr,nullptr,nullptr,&promoted,&retirements,&rows);
    result.original_to_proposed=replayed.result().original_to_proposed;
    result.reserved_component_identity_ids=replayed.result().fresh_identity_ids;
    auto framed=result.entities;
    // Remove only independently inventoried provisional copies. Registries and
    // presentation references keep their admitted final destination identities.
    std::map<std::string,std::string,std::less<>> copies=authoring.dimension_identities;
    for (const auto& [original,proposed]:physical_dimension_ids)
        if (!copies.emplace(original,proposed).second) invalid("Mixed resize repeats a source dimension allocation");
    for (const auto& [original,proposed]:copies) {
        if (!exact(actual.at(original),framed.at(original)) || actual.contains(proposed) || !framed.erase(proposed) ||
            !result.entities.erase(proposed)) invalid("Mixed resize lost a protected original or provisional dimension copy");
    }
    std::map<ComponentTarget,std::size_t> requests_by_root;
    std::vector<AssemblyDimensionReviewedTransformRequest> requests;
    std::size_t associations{};
    for (const auto& [id,entity]:actual) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type) ||
            !is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(entity).format)) continue;
        const auto dimension=decode_boundary_dimension_entity(entity);
        if (!dimension.dimension || !dimension.dimension->assembly_target) invalid("Mixed resize has an unsupported dimension carrier");
        const auto root=component_root(dimension.dimension->assembly_target->component);
        const auto destination=destinations.find(root);
        if (destination==destinations.end()) continue;
        if (++associations>1000) invalid("Mixed resize combined dimension association bound exceeded");
        const auto [request,inserted]=requests_by_root.emplace(root,requests.size());
        if (inserted) requests.push_back({{root,destination->second,root==destination->second ?
            AssemblyDimensionTransformMode::move : AssemblyDimensionTransformMode::clone,{}},{},{}});
        const bool retired=authoring.profile_dimension_decisions.contains(id) && !authoring.profile_dimension_decisions.at(id);
        if (root!=destination->second && !retired)
            requests.at(request->second).transform.fresh_dimension_ids.emplace(id,copies.at(id));
        framed.insert_or_assign(id,entity);
        if (!retired) result.entities.insert_or_assign(id,entity);
    }
    for (const auto& intent:authoring.intents) if (intent.review_profile_topology ||
        intent.initialize_deformed_profile_topology || intent.review_initial_profile_geometry) {
        const auto request=requests_by_root.find(component_root(intent.target));
        if (request!=requests_by_root.end()) requests.at(request->second).reviewed_profile_nodes.push_back(intent.target);
    }
    const auto reviews=inspect_phase_assembly_instance_profile_dimension_review_roster(actual,authoring.intents);
    for (const auto& [id,decision]:authoring.profile_dimension_decisions) {
        const auto root=component_root(reviews.at(id).component);
        requests.at(requests_by_root.at(root)).profile_dimension_decisions.emplace(id,decision);
    }
    AssemblyDimensionReviewedCarryResult dimensions;
    if (!requests.empty()) {
        if (std::none_of(requests.begin(),requests.end(),[](const auto& request) {
                return !request.reviewed_profile_nodes.empty();
            })) {
            std::vector<AssemblyDimensionTransformRequest> ordinary;
            for (const auto& request:requests) ordinary.push_back(request.transform);
            dimensions.upserts=carry_assembly_dimension_transform_entities(actual,framed,ordinary);
        } else dimensions=carry_reviewed_assembly_dimension_transform_entities(actual,framed,requests);
    }
    if (dimensions.retired_source_dimension_ids!=replayed.result().retired_dimension_ids)
        invalid("Mixed resize changed the independently reviewed retirement closure");
    std::vector<std::string> changed=dimensions.retired_source_dimension_ids;
    for (const auto& dimension:dimensions.upserts) {
        if (actual.contains(dimension.id)) changed.push_back(dimension.id);
        result.entities.insert_or_assign(dimension.id,dimension);
    }
    std::sort(changed.begin(),changed.end());
    validate_ordinary_assembly_dimension_changes(actual,changed);
    for (const auto& id:dimensions.retired_source_dimension_ids) result.entities.erase(id);
    std::vector<AssemblyInstancePropertyEditIntent> retained;
    for (const auto& [original,destination]:result.original_to_proposed) {
        (void)original;
        AssemblyInstancePropertyEditIntent intent;
        intent.target=destination;
        retained.push_back(std::move(intent));
    }
    std::sort(retained.begin(),retained.end(),[](const auto& left,const auto& right) { return left.target<right.target; });
    (void)replay_phase_assembly_instance_property_edits(result.entities,retained,{});
    return result;
}
} // namespace

namespace {
// A builder carries only original-source physical consequences. Component
// replay and reviewed dimension retirement are owned by the finalizer.
template<class Composition> struct PhysicalDirectionalResizeContribution {
    Composition result;
    Entities entities;
    std::map<ComponentTarget,ComponentTarget> destinations;
    PhaseSlabReplacementIdentityMap dimension_ids;
    std::vector<std::string> structural_hosts;
};
PhaseAssemblyInstancePropertyEditAuthoring directional_component_authoring(
    const PhaseAssemblyInstancePropertyEditPreparation& replayed,std::size_t physical_members) {
    const auto version=replayed.request().at("version").get<int>();
    if (version<7 || version>9) invalid("Mixed resize requires component geometry request seven, eight or nine");
    auto authoring=decode_phase_assembly_instance_property_edit_authoring(replayed.request().at("authoring"));
    if (authoring.intents.empty() || physical_members>1000 || authoring.intents.size()>1000-physical_members)
        invalid("Mixed resize selection exceeds its complete aggregate roster bound");
    return authoring;
}
template<class Map> void merge_directional_destinations(Map& combined,const Map& lane) {
    for (const auto& [original,destination]:lane) {
        const auto [entry,inserted]=combined.emplace(original,destination);
        if (!inserted && entry->second!=destination)
            invalid("Mixed physical resize has conflicting original-source destinations");
    }
}
// Inventory every requested hosted root, including units, before a producer
// can discard a no-op. Unknown affected reference codecs grant no authority.
void admit_directional_host_dimensions(const Entities& actual,
    const std::set<std::string,std::less<>>& hosts) {
    std::vector<std::pair<std::string,std::string>> roots;
    for (const auto& [id,entity]:actual) if (entity.type=="assembly_model")
        for (const auto& row:entity.properties.at("model").at("instances")) {
            if (!row.contains("placement") || row.at("placement").is_null() ||
                !hosts.contains(row.at("placement").at("host_entity_id").get<std::string>())) continue;
            roots.emplace_back(id,row.at("id").get<std::string>());
        }
    if (!roots.empty()) (void)inspect_embedded_assembly_dimension_clone_roster(actual,roots);
}
void verify_directional_structural_result(const Entities& final_entities,const Entities& physical,
    const std::vector<std::string>& final_hosts) {
    const auto final_inventory=inspect_structural_hosted_components(final_entities,final_hosts);
    if (!final_inventory.ready()) invalid("Mixed structural resize final native/hosted geometry is blocked");
    for (const auto& host:final_hosts) {
        if (!exact(final_entities.at(host),physical.at(host)))
            invalid("Mixed structural resize final composition changed the native owner");
        (void)stage_structural_plan_axis_resize_entity(final_entities.at(host),1,1,{0,0},0);
    }
}

PhysicalDirectionalResizeContribution<ComponentSlabDirectionalResizeComposition> build_slab_directional_physical_contribution(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& replayed,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    std::span<const SlabGeometryEditIntent> slab_geometry,
    const std::optional<ComponentSlabDirectionalResizeAllocation>& allocation,std::string_view message) {
    if (slab_geometry.empty() || slab_geometry.size()>1000 || message.size()>1024 || message.find('\0')!=std::string_view::npos)
        invalid("Mixed component/slab resize roster or message exceeds its bound");
    const auto& actual=source.entities();
    std::map<ComponentTarget,ComponentTarget> destinations;
    for (const auto& intent:authoring.intents) {
        if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
            !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
            intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
            intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
            intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
            invalid("Mixed component/slab resize admits independent document component geometry only");
        const auto root=component_root(intent.target);
        const auto destination=component_root(replayed.result().original_to_proposed.at(intent.target));
        const auto [entry,inserted]=destinations.emplace(root,destination);
        if (!inserted && entry->second!=destination) invalid("Mixed resize has conflicting component destinations");
    }
    if (slab_geometry.empty() || slab_geometry.size()>1000 || authoring.intents.size()+slab_geometry.size()>1000)
        invalid("Mixed component/slab resize selection exceeds its complete roster bound");
    std::vector<SlabGeometryEditIntent> slabs(slab_geometry.begin(),slab_geometry.end());
    std::set<std::string,std::less<>> slab_hosts;
    std::string previous;
    for (const auto& intent:slabs) {
        const auto found=actual.find(intent.slab_id);
        if (found==actual.end() || found->second.id!=intent.slab_id || found->second.type!="slab" ||
            (!previous.empty() && previous>=intent.slab_id) || intent.kind!=SlabGeometryEditKind::resize_plan ||
            !intent.coordinate_world_hosted_geometry ||
            intent.preserve_planar_source_geometry!=slab_geometry_edit_requires_planar_source(found->second,intent))
            invalid("Mixed resize requires sorted actual-source directional slab leaves four or five");
        (void)decode_slab_geometry_edit_intent(encode_slab_geometry_edit_intent(intent));
        previous=intent.slab_id;
        slab_hosts.insert(intent.slab_id);
    }
    // Authenticate the complete hosted lane before partitioning drops exact
    // no-ops. In particular, baseline replacement must not skip unit hosts
    // whose catalogs or nested geometry need actual-source admission.
    auto authenticated_slabs=replay_slab_geometry_with_hosted_entities(actual,slabs);
    admit_directional_host_dimensions(actual,slab_hosts);
    const auto partition=partition_phase_slab_geometry_edits(actual,slabs);
    if (partition.replacement.has_value()!=allocation.has_value())
        invalid("Mixed resize slab allocation must exactly match actual changed baseline targets");
    ComponentSlabDirectionalResizeComposition result;
    Entities physical;
    PhaseSlabReplacementIdentityMap slab_dimension_ids;
    if (partition.replacement) {
        const auto& request=*partition.replacement;
        const auto plan=inspect_phase_slab_replacement_plan(actual,request.seed_slab_ids,
            request.registry_id,request.alternative_id,true);
        if (!plan.ready()) invalid("Mixed resize slab replacement source inventory is blocked");
        PhaseSlabReplacementAuthoring leaf;
        leaf.registry_id=request.registry_id;
        leaf.alternative_id=request.alternative_id;
        leaf.seed_slab_ids=request.seed_slab_ids;
        leaf.identities=allocation->identities;
        leaf.hosted_instance_identities=allocation->hosted_instance_identities;
        leaf.dimension_identities=allocation->dimension_identities;
        leaf.complete_component_dimensions=true;
        leaf.slab_geometry=partition.baseline_geometry;
        leaf.ordinary_geometry=partition.ordinary_geometry;
        leaf.coordinate_ordinary_hosted_geometry=!leaf.ordinary_geometry.empty();
        leaf.coordinate_hosted_directional_geometry=true;
        const auto mapped=[](const auto& intents) { return std::any_of(intents.begin(),intents.end(),
            [](const auto& intent) { return intent.preserve_planar_source_geometry; }); };
        leaf.preserve_slab_planar_source_geometry=mapped(leaf.slab_geometry) || mapped(leaf.ordinary_geometry);
        const auto canonical=encode_phase_slab_replacement_authoring(leaf);
        const auto replacement=replay_phase_slab_replacement(actual,plan,leaf.identities,{}, {},
            leaf.slab_geometry,leaf.ordinary_geometry,leaf.hosted_instance_identities,
            leaf.coordinate_ordinary_hosted_geometry,false,leaf.dimension_identities,true,
            leaf.preserve_slab_planar_source_geometry);
        physical=replay_phase_slab_replacement_authoring(actual,decode_phase_slab_replacement_authoring(canonical));
        if (physical!=replacement.entities) invalid("Mixed resize slab canonical replay differs from source inspection");
        std::set<std::string,std::less<>> fresh(replacement.fresh_identity_ids.begin(),replacement.fresh_identity_ids.end());
        for (const auto& revision:source.history()) validate_selection_edit_fresh_identity_tokens(revision.entities,fresh);
        validate_retained_authoring_fresh_identities(source.history(),source.history().size(),fresh);
        result.slab_original_to_proposed=replacement.original_to_proposed;
        slab_dimension_ids=leaf.dimension_identities;
        for (const auto& [binding,instance]:replacement.original_to_hosted_instance_proposed) {
            const auto destination=std::pair{leaf.identities.at(binding.first),instance};
            result.slab_hosted_bindings.emplace(binding,destination);
            destinations.emplace(ComponentTarget{binding.first,binding.second,std::nullopt,{}},
                ComponentTarget{destination.first,destination.second,std::nullopt,{}});
        }
        ConstraintAuthoringIntent semantic;
        semantic.message=std::string(message);
        auto proof=make_phase_constraint_authoring_intent(source,semantic,nullptr,true);
        proof.slab_replacement=canonical;
        result.slab_phase_authorings.push_back(encode_phase_constraint_authoring_intent(proof));
    } else physical=std::move(authenticated_slabs);
    for (const auto& intent:slabs) {
        const auto mapped=result.slab_original_to_proposed.find(intent.slab_id);
        const auto destination=mapped==result.slab_original_to_proposed.end() ? intent.slab_id : mapped->second;
        result.slab_selected_original_to_proposed.emplace(intent.slab_id,destination);
        result.slab_original_to_proposed.emplace(intent.slab_id,destination);
    }
    // Unit members remain in this source-derived roster even when partitioning
    // found no physical delta. A no-upsert association still consumes the bound.
    for (const auto& [id,entity]:actual) if (entity.type=="assembly_model")
        for (const auto& row:entity.properties.at("model").at("instances")) {
            if (!row.contains("placement") || row.at("placement").is_null() ||
                !slab_hosts.contains(row.at("placement").at("host_entity_id").get<std::string>())) continue;
            const ComponentTarget root{id,row.at("id").get<std::string>(),std::nullopt,{}};
            destinations.emplace(root,root);
        }
    return {std::move(result),std::move(physical),std::move(destinations),std::move(slab_dimension_ids),{}};
}

PhysicalDirectionalResizeContribution<ComponentStructuralDirectionalResizeComposition> build_structural_directional_physical_contribution(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& replayed,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    std::span<const StructuralDirectionalResizeRequest> requests,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>& allocation,std::string_view message) {
    if (requests.empty() || requests.size()>1000 || message.size()>1024 || message.find('\0')!=std::string_view::npos)
        invalid("Mixed component/structural resize roster or message exceeds its bound");
    const auto& actual=source.entities();
    std::map<ComponentTarget,ComponentTarget> destinations;
    for (const auto& intent:authoring.intents) {
        if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
            !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
            intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
            intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
            intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
            invalid("Mixed structural resize admits independent document component geometry only");
        const auto root=component_root(intent.target);
        const auto destination=component_root(replayed.result().original_to_proposed.at(intent.target));
        const auto [entry,inserted]=destinations.emplace(root,destination);
        if (!inserted && entry->second!=destination) invalid("Mixed resize has conflicting component destinations");
    }
    std::vector<std::string> hosts;
    std::vector<StructuralObjectEditIntent> edits;
    Entities staged=actual;
    std::string previous;
    for (const auto& request:requests) {
        const auto found=actual.find(request.object_id);
        if (found==actual.end() || found->second.id!=request.object_id ||
            (!previous.empty() && previous>=request.object_id))
            invalid("Mixed structural resize requires sorted unique actual owners");
        previous=request.object_id;
        const auto& entity=found->second;
        const auto& p=entity.properties;
        if (!p.is_object() || !p.contains("version") || !p.at("version").is_number_integer() || p.at("version")!=1 ||
            !p.contains("form") || !((entity.type=="column" && p.at("form")=="rectangular_column") ||
                (entity.type=="beam" && p.at("form")=="straight_beam")))
            invalid("Mixed structural resize admits canonical v1 rectangular columns and horizontal straight beams only");
        validate_structural_object_source_entity(entity);
        if (entity.type=="beam") {
            const auto& start=p.at("start_m");const auto& end=p.at("end_m");const auto& up=p.at("up_dir");
            const auto dx=end.at(0).get<double>()-start.at(0).get<double>();
            const auto dy=end.at(1).get<double>()-start.at(1).get<double>();
            const auto length=std::hypot(dx,dy);
            // Match the native producer's projected-section test even when its
            // exact-unit fast path would otherwise return before that test.
            if (std::abs(start.at(2).get<double>()-end.at(2).get<double>())>default_geometry_tolerance_metres ||
                !(length>default_geometry_tolerance_metres) ||
                std::abs(up.at(0).get<double>()*dy-up.at(1).get<double>()*dx)>
                    1e-10*length*std::max(1.0,std::abs(up.at(2).get<double>())))
                invalid("Mixed structural resize requires a horizontal beam with a vertical section frame, including units");
        }
        auto candidate=stage_structural_plan_axis_resize_entity(entity,request.sx,request.sy,request.anchor_m,
            request.frame_rotation_radians);
        if (!exact(entity,candidate)) {
            try {
                const auto captured=capture_structural_object_edit(entity,candidate);
                if (!captured) invalid("Mixed structural resize exact typed full capture is unavailable");
                edits.push_back(*captured);
            } catch (const std::invalid_argument& error) {
                invalid(std::string("Mixed structural resize exact typed full capture cannot preserve the source: ")+error.what());
            }
        }
        staged.at(request.object_id)=std::move(candidate);
        hosts.push_back(request.object_id);
    }
    // Units enter complete actual-map context, phase and hosted admission before
    // changed-owner classification. They never acquire allocation authority.
    const auto inventory=inspect_structural_hosted_components(actual,hosts);
    if (!inventory.ready()) invalid("Mixed structural resize complete source hosted inventory is blocked");
    // The clone inventory is source inspection only here. It rejects unknown
    // affected dimension codecs even on ordinary/unit roots; no IDs are created.
    (void)inspect_embedded_assembly_dimension_clone_roster(actual,inventory.instance_ids);
    const auto typed=replay_structural_object_edit_entities(actual,edits);
    for (const auto& host:hosts) if (!exact(typed.at(host),staged.at(host)))
        invalid("Mixed structural resize exact typed replay differs from the native candidate");
    const auto replacement=phase_structural_edit_replacement_request(actual,edits);
    if (replacement.has_value()!=allocation.has_value())
        invalid("Mixed structural resize allocation must exactly match changed baseline seeds");
    ComponentStructuralDirectionalResizeComposition result;
    Entities physical;
    PhaseStructuralReplacementIdentityMap dimension_ids;
    if (replacement) {
        const auto plan=inspect_phase_structural_replacement_plan(actual,replacement->seed_object_ids,
            replacement->registry_id,replacement->alternative_id,true,true);
        if (!plan.ready()) invalid("Mixed structural resize complete replacement inventory is blocked");
        PhaseStructuralReplacementAuthoring leaf;
        leaf.registry_id=replacement->registry_id;leaf.alternative_id=replacement->alternative_id;
        leaf.seed_object_ids=replacement->seed_object_ids;leaf.edits=edits;
        leaf.identities=allocation->identities;leaf.hosted_instance_identities=allocation->hosted_instance_identities;
        leaf.dimension_identities=allocation->dimension_identities;
        leaf.complete_hosted=true;leaf.complete_component_dimensions=true;
        const auto canonical=encode_phase_structural_replacement_authoring(leaf);
        if (canonical.at("version")!=5 || canonical.at("authoring").at("version")!=3)
            invalid("Mixed structural resize requires internally captured leaf five with full body three");
        physical=replay_phase_structural_replacement_authoring(actual,decode_phase_structural_replacement_authoring(canonical));
        result.structural_original_to_proposed=leaf.identities;
        result.structural_original_to_proposed.insert(leaf.dimension_identities.begin(),leaf.dimension_identities.end());
        dimension_ids=leaf.dimension_identities;
        std::set<std::string,std::less<>> fresh;
        for (const auto& [original,proposed]:leaf.identities) { (void)original;fresh.insert(proposed); }
        for (const auto& [binding,proposed]:leaf.hosted_instance_identities) {
            fresh.insert(proposed);
            const std::pair destination{leaf.identities.at(binding.first),proposed};
            result.structural_hosted_bindings.emplace(binding,destination);
            destinations.emplace(ComponentTarget{binding.first,binding.second,std::nullopt,{}},
                ComponentTarget{destination.first,destination.second,std::nullopt,{}});
        }
        for (const auto& [original,proposed]:leaf.dimension_identities) { (void)original;fresh.insert(proposed); }
        const auto original_aliases=embedded_assembly_presentation_ids(actual);
        const auto final_aliases=embedded_assembly_presentation_ids(physical);
        for (const auto& [binding,destination]:result.structural_hosted_bindings)
            result.structural_original_to_proposed.emplace(original_aliases.at(binding),final_aliases.at(destination));
        for (const auto& [binding,alias]:final_aliases)
            if (!original_aliases.contains(binding)) fresh.insert(alias);
        for (const auto& token:replayed.result().fresh_identity_ids)
            if (fresh.contains(token)) invalid("Mixed structural resize collides with a component identity reservation");
        for (const auto& revision:source.history()) validate_selection_edit_fresh_identity_tokens(revision.entities,fresh);
        validate_retained_authoring_fresh_identities(source.history(),source.history().size(),fresh);
        ConstraintAuthoringIntent semantic;semantic.message=std::string(message);
        auto proof=make_phase_constraint_authoring_intent(source,semantic,nullptr,true);
        proof.structural_replacement=canonical;
        result.structural_phase_authorings.push_back(encode_phase_constraint_authoring_intent(proof));
    } else physical=replay_structural_hosted_component_geometry(actual,edits);
    std::vector<std::string> final_hosts;
    for (const auto& request:requests) {
        const auto mapped=result.structural_original_to_proposed.find(request.object_id);
        const auto destination=mapped==result.structural_original_to_proposed.end() ? request.object_id : mapped->second;
        result.structural_selected_original_to_proposed.emplace(request.object_id,destination);
        result.structural_original_to_proposed.emplace(request.object_id,destination);
        auto expected=staged.at(request.object_id);expected.id=destination;
        if (!exact(expected,physical.at(destination))) invalid("Mixed structural resize lost the exact native owner");
        final_hosts.push_back(destination);
    }
    for (const auto& binding:inventory.instance_ids) {
        const ComponentTarget root{binding.first,binding.second,std::nullopt,{}};
        destinations.emplace(root,root);
        result.structural_hosted_bindings.emplace(binding,binding);
    }
    return {std::move(result),std::move(physical),std::move(destinations),std::move(dimension_ids),std::move(final_hosts)};
}

PhysicalDirectionalResizeContribution<ComponentRoofOpeningDirectionalResizeComposition> build_roof_opening_directional_physical_contribution(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& replayed,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    std::span<const RoofOpeningDirectionalResizeRequest> requests,std::string_view message) {
    if (requests.empty() || requests.size()>1000 || message.size()>1024 || message.find('\0')!=std::string_view::npos)
        invalid("Mixed component/skylight resize roster or message exceeds its bound");
    const auto& actual=source.entities();
    std::map<ComponentTarget,ComponentTarget> destinations;
    for (const auto& intent:authoring.intents) {
        if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
            !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
            intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
            intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
            intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
            invalid("Mixed component/skylight resize admits independent document component geometry only");
        const auto root=component_root(intent.target);
        const auto destination=component_root(replayed.result().original_to_proposed.at(intent.target));
        const auto [entry,inserted]=destinations.emplace(root,destination);
        if (!inserted && entry->second!=destination) invalid("Mixed resize has conflicting component destinations");
    }
    ComponentRoofOpeningDirectionalResizeComposition result;
    std::set<std::string,std::less<>> hosts;
    std::optional<std::pair<std::string,std::string>> previous;
    for (const auto& request:requests) {
        const auto& target=request.target;
        const std::pair key{target.roof_id,target.opening_id};
        if (previous && *previous>=key) invalid("Mixed skylight resize requires sorted unique qualified children");
        previous=key;
        hosts.insert(target.roof_id);
        result.roof_children.emplace(key,target);
    }
    // Membership is checked before derivation or unit/no-op elimination. Even
    // baseline-only and inactive/proposed registry members are outside lane40.
    for (const auto& [id,entity]:actual) if (entity.type=="model_phases") {
        (void)id;
        const auto registry=ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& host:registry.entity_ids())
            if (hosts.contains(host)) invalid("Mixed skylight resize requires hosts absent from every phase registry");
    }
    auto physical=prepare_roof_opening_directional_resizes(actual,requests);
    admit_directional_host_dimensions(actual,hosts);
    ConstraintAuthoringIntent semantic;
    semantic.message=std::string(message);
    auto proof=make_phase_constraint_authoring_intent(source,semantic,nullptr,true);
    proof.complete_ordinary_roof_component_dimensions=true;
    proof.ordinary_roof_edits=Json::array();
    for (const auto& leaf:physical.intents) proof.ordinary_roof_edits.push_back(encode_roof_edit_intent(leaf));
    result.phase_authorings.push_back(encode_phase_constraint_authoring_intent(proof));
    // Include embedded roots on every requested host, even when all selected
    // children are units and the roof and its dimensions receive no upsert.
    for (const auto& [id,entity]:actual) if (entity.type=="assembly_model")
        for (const auto& row:entity.properties.at("model").at("instances")) {
            if (!row.contains("placement") || row.at("placement").is_null() ||
                !hosts.contains(row.at("placement").at("host_entity_id").get<std::string>())) continue;
            const ComponentTarget root{id,row.at("id").get<std::string>(),std::nullopt,{}};
            destinations.emplace(root,root);
        }
    return {std::move(result),std::move(physical.candidate_entities),std::move(destinations),{}, {}};
}

Entities union_directional_physical_candidates(const Entities& actual,const std::vector<Entities>& lanes) {
    if (lanes.size()<2 || lanes.size()>3)
        invalid("Combined physical resize requires two or three independently reconstructed physical families");
    std::set<std::string,std::less<>> promoted;
    for (const auto& lane:lanes) (void)component_property_row_admission(actual,lane,promoted);
    // No retirement or erased-owner authority crosses this boundary.
    return compose_mixed_selection_edit_candidates_impl(actual,lanes,nullptr,nullptr,nullptr,
        &promoted,nullptr,nullptr,true);
}
void merge_physical_owner_correspondence(const Entities& actual,
    std::map<std::string,std::string,std::less<>>& combined,
    const std::map<std::string,std::string,std::less<>>& lane) {
    for (const auto& [original,destination]:lane) {
        const auto source=actual.find(original);
        // One source catalog may have several independently allocated private
        // copies. Its qualified roots and family proofs carry those identities.
        if (source!=actual.end() && source->second.type=="assembly_model") continue;
        const auto [entry,inserted]=combined.emplace(original,destination);
        if (!inserted && entry->second!=destination)
            invalid("Mixed physical resize has conflicting owner or alias destinations");
    }
}
void validate_directional_physical_freshness(const DocumentSnapshot& source,
    const std::vector<Entities>& lanes,const PhaseAssemblyInstancePropertyEditPreparation& component,
    const std::optional<ComponentSlabDirectionalResizeAllocation>& slabs,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>& structures) {
    auto all=lanes;all.push_back(component.preview_entities());
    validate_mixed_edit_fresh_destinations(source.entities(),all);
    const auto originals=mixed_edit_identity_owners(source.entities());
    const auto original_aliases=embedded_assembly_presentation_ids(source.entities());
    std::set<std::string,std::less<>> physical_fresh;
    for (const auto& lane:lanes) {
        for (const auto& [token,owners]:mixed_edit_identity_owners(lane)) {
            (void)owners;if (!originals.contains(token)) physical_fresh.insert(token);
        }
        for (const auto& [binding,alias]:embedded_assembly_presentation_ids(lane))
            if (!original_aliases.contains(binding)) physical_fresh.insert(alias);
    }
    std::set<std::string,std::less<>> allocated;
    const auto allocations=[&](const auto& allocation) {
        if (!allocation) return;
        const auto reserve=[&](const std::string& token) {
            if (!allocated.insert(token).second)
                invalid("Mixed physical resize fresh allocation destinations overlap between families");
            physical_fresh.insert(token);
        };
        for (const auto& [original,proposed]:allocation->identities) { (void)original;reserve(proposed); }
        for (const auto& [binding,proposed]:allocation->hosted_instance_identities) { (void)binding;reserve(proposed); }
        for (const auto& [original,proposed]:allocation->dimension_identities) { (void)original;reserve(proposed); }
    };
    allocations(slabs);allocations(structures);
    for (const auto& lane:lanes) for (const auto& [binding,alias]:embedded_assembly_presentation_ids(lane))
        if (!original_aliases.contains(binding) && allocated.contains(alias))
            invalid("Mixed physical resize fresh allocation collides with a computed presentation alias");
    for (const auto& token:component.result().fresh_identity_ids)
        if (physical_fresh.contains(token)) invalid("Mixed physical resize collides with a component reservation");
    auto fresh=physical_fresh;
    fresh.insert(component.result().fresh_identity_ids.begin(),component.result().fresh_identity_ids.end());
    if (fresh.size()>4096) invalid("Mixed physical resize aggregate fresh identity budget exceeded");
    for (const auto& [id,asset]:source.assets()) {
        (void)asset;
        if (fresh.contains(id)) invalid("Mixed physical resize fresh identity collides with a current asset");
    }
    for (const auto& revision:source.history()) {
        validate_selection_edit_fresh_identity_tokens(revision.entities,fresh);
        for (const auto& [id,asset]:revision.assets) {
            (void)asset;
            if (fresh.contains(id)) invalid("Mixed physical resize fresh identity collides with a retained asset");
        }
    }
    validate_retained_authoring_fresh_identities(source.history(),source.history().size(),fresh);
}
} // namespace

namespace {
// Bounded raw inspection precedes catalog/phase/typed projection. Metadata is
// charged as source data, never interpreted as a typed host relationship.
struct DirectionalResizeSourceBudget {
    std::size_t nodes{},bytes{};
    bool portable_text{};
    const char* diagnostic{"Mixed stair resize"};
    void text(std::string_view value) {
        if (value.size()>64*1024*1024-bytes) invalid(std::string(diagnostic)+" source byte budget exceeded");
        bytes+=value.size();
        if (portable_text) try { (void)Json(value).dump(); }
        catch (const Json::exception&) { invalid("Mixed roof resize source text is nonportable UTF-8"); }
    }
    void visit(const Json& value,std::size_t depth=0) {
        if (depth>64 || ++nodes>4*1024*1024) invalid(std::string(diagnostic)+" source node/nesting budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>())))
            invalid(std::string(diagnostic)+" source JSON is nonportable");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key,child]:value.items()) { text(key);visit(child,depth+1); }
        else if (value.is_array()) for (const auto& child:value) visit(child,depth+1);
    }
};
bool stair_resize_reference(const Json& value,const std::set<std::string,std::less<>>& owners) {
    if (value.is_string()) return owners.contains(value.get_ref<const std::string&>());
    if (value.is_array()) for (const auto& child:value) if (stair_resize_reference(child,owners)) return true;
    if (value.is_object()) for (const auto& [key,child]:value.items()) {
        (void)key;
        if (stair_resize_reference(child,owners)) return true;
    }
    return false;
}
// The existing typed object's closed profile includes all known fields and
// explicit absence. This source-only projection budgets capture including units;
// actual edit authority is still obtained solely by capture/replay below.
Json stair_resize_capture_projection(const Entity& entity) {
    Json profile=Json::object();
    for (const auto* key:{"version","form","base_position_m","orientation_rad","riser_count","total_rise_m",
        "going_m","width_m","top_landing","level_connection","flights","landings","vertical_placement"})
        profile[key]=entity.properties.contains(key) ? entity.properties.at(key) : Json(nullptr);
    const auto& landing=entity.properties.at("top_landing");
    profile["top_landing"]={{"depth_m",landing.at("depth_m")},{"thickness_m",landing.at("thickness_m")}};
    StairObjectEditIntent intent{entity.id,std::move(profile),entity.properties.value("quantity_entries",Json(nullptr))};
    return encode_stair_object_edit_intent(intent);
}
} // namespace

namespace {
void preflight_stair_directional_resize_impl(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring* authoring,std::span<const StairDirectionalResizeRequest> requests) {
    if (requests.empty() || requests.size()>1000 || (authoring && (authoring->intents.empty() ||
        authoring->intents.size()>1000-requests.size()))) invalid("Mixed stair resize aggregate selection bound exceeded");
    const auto& actual=source.entities();
    if (actual.size()>100000) invalid("Mixed stair resize source entity bound exceeded");
    DirectionalResizeSourceBudget raw;
    for (const auto& [id,entity]:actual) {
        if (id!=entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("Mixed stair resize requires actual identified source envelopes");
        raw.text(id);raw.text(entity.type);raw.visit(entity.properties);raw.visit(entity.extensions);
    }
    const auto component_wire=authoring ? encode_phase_assembly_instance_property_edit_authoring(*authoring) : Json(nullptr);
    if (authoring) raw.visit(component_wire);
    if (authoring && component_wire.at("version")!=7 && component_wire.at("version")!=8 && component_wire.at("version")!=9)
        invalid("Mixed stair resize requires component geometry request seven, eight or nine");
    std::set<ComponentTarget> roots;
    std::set<std::string,std::less<>> component_owners;
    if (authoring) for (const auto& intent:authoring->intents) {
        if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
            !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
            intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
            intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
            intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
            invalid("Mixed stair resize admits independent document component geometry only");
        roots.insert(component_root(intent.target));
        component_owners.insert(*intent.target.document_entity_id);
    }
    // Existing catalog transfer bounds are shared across the complete source.
    // Component replay retains its separate established expansion bounds.
    AssemblyCatalogTransferBudget catalog_budget;
    for (const auto& [id,entity]:actual) {
        (void)id;if (entity.type=="assembly_model") admit_existing_assembly_catalog_work(entity,catalog_budget);
    }
    std::set<std::string,std::less<>> stairs;
    std::size_t work{};
    for (const auto& request:requests) {
        if (request.stair_id.empty() || !stairs.insert(request.stair_id).second)
            invalid("Mixed stair resize requires unique actual owners");
        if (!std::isfinite(request.sx) || !std::isfinite(request.sy) || request.sx<=0 || request.sy<=0 ||
            !std::isfinite(request.anchor_m.x) || !std::isfinite(request.anchor_m.y) || !std::isfinite(request.frame_rotation_radians))
            invalid("Mixed stair resize requires positive factors and a finite frame");
        const auto found=actual.find(request.stair_id);
        if (found==actual.end() || found->second.type!="stair") invalid("Mixed stair resize actual owner is missing");
        const auto& p=found->second.properties;
        if (!p.contains("version") || !p.at("version").is_number_integer() || p.at("version")!=1 ||
            !p.contains("form") || p.at("form")!="straight_stair_flight" ||
            !p.contains("top_landing") || !p.at("top_landing").is_object())
            invalid("Mixed stair resize requires v1 straight stairs with existing valid top landings");
        for (const auto* key:{"transform","level_connection","vertical_placement","phase_id"})
            if (p.contains(key)) invalid("Mixed stair resize refuses transforms, levels and explicit phase authority, including units");
        for (const auto* key:{"flights","landings"}) if (p.contains(key))
            invalid("Mixed stair resize cannot preserve dormant topology outside v1 typed authority, including units");
        if (!p.contains("riser_count") || !p.at("riser_count").is_number_integer() ||
            (p.at("riser_count").is_number_integer() && !p.at("riser_count").is_number_unsigned() && p.at("riser_count").get<std::int64_t>()<1))
            invalid("Mixed stair resize riser count must be positive");
        const auto count=p.at("riser_count").get<std::uint64_t>();
        if (count>=100000-work) invalid("Mixed stair resize aggregate riser plus landing work budget exceeded");
        work+=static_cast<std::size_t>(count)+1;
    }
    std::size_t associations{};
    for (const auto& [id,entity]:actual) {
        const auto& p=entity.properties;
        if (entity.type=="model_phases") {
            const auto registry=ModelPhases::from_json(p.at("model"));
            for (const auto& member:registry.entity_ids()) if (stairs.contains(member))
                invalid("Mixed stair resize requires absence from every phase registry, including baseline-only");
        }
        if (entity.type==kSheetViewEntityType) {
            const auto model=decode_sheet_view_entity(entity);
            for (const auto& view:model.views()) for (const auto& overlay:view.overlays)
                if (overlay.dimension_binding && stairs.contains(overlay.dimension_binding->object_id))
                    invalid("Mixed stair resize excludes stair-owned saved-view dimensions, including units");
        }
        const bool typed_rail=entity.type=="railing" && p.contains("version") && p.at("version").is_number_integer() &&
            p.contains("form") && ((p.at("version")==2 && p.at("form")=="stair_flight_railing") ||
                (p.at("version")==3 && p.at("form")=="stair_landing_railing"));
        if (typed_rail) {
            const auto rail=decode_railing_properties(id,p);
            if ((rail.host && stairs.contains(rail.host->stair_id)) ||
                (rail.landing_host && stairs.contains(rail.landing_host->stair_id)))
                invalid("Mixed stair resize excludes actual attached rails, including units");
        }
        if (entity.type=="railing" && !typed_rail && p.contains("host") && p.at("host").is_object() &&
            p.at("host").contains("stair_id") && p.at("host").at("stair_id").is_string() &&
            stairs.contains(p.at("host").at("stair_id").get<std::string>()))
            invalid("Mixed stair resize cannot preserve an affected opaque railing binding through full typed replay");
        if (entity.type=="assembly_model") {
            const auto model=AssemblyModel::from_json(p.at("model"));
            for (const auto& row:model.instances()) if (row.placement && stairs.contains(row.placement->host_entity_id))
                invalid("Mixed stair resize excludes actual hosted component roots, including units");
        }
        // Only authoritative reference slots establish affected dependencies.
        // Opaque vendor host lookalikes do not create stair or assembly hosts.
        for (const auto* key:{"entity_ids","wall_ids","owner_id","boundary_id","dependency_ids","dependencies"})
            if (p.contains(key) && stair_resize_reference(p.at(key),stairs))
                invalid("Mixed stair resize has an unsupported affected dependency or constraint, including units");
        if (entity.type=="constraint" && p.contains("bindings") && stair_resize_reference(p.at("bindings"),stairs))
            invalid("Mixed stair resize excludes affected persistent constraints, including units");
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        if (p.contains("target") && stair_resize_reference(p.at("target"),stairs))
            invalid("Mixed stair resize excludes stair-owned dimensions, including units");
        // Unknown affected dimension codecs must refuse before component native
        // replay as well. Read only their established qualified carrier slots.
        bool component_association=false;
        if (p.contains("target") && p.at("target").is_object() && p.at("target").contains("entity_id") &&
            p.at("target").at("entity_id").is_string()) {
            const auto& carrier=p.at("target").at("entity_id").get_ref<const std::string&>();
            component_association=component_owners.contains(carrier);
        }
        if (p.contains("target") && p.at("target").is_object() && p.at("target").contains("assembly")) {
            const auto& target=p.at("target").at("assembly");
            if (target.is_object() && target.contains("catalog_id") && target.at("catalog_id").is_string() &&
                target.contains("instance_id") && target.at("instance_id").is_string() &&
                target.contains("document_entity_id") && target.at("document_entity_id").is_string()) {
                component_association=component_association || roots.contains(ComponentTarget{target.at("catalog_id").get<std::string>(),
                    target.at("instance_id").get<std::string>(),target.at("document_entity_id").get<std::string>(),{}});
            }
        }
        if (!is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(entity).format)) {
            if (component_association) invalid("Mixed stair resize has an unsupported affected component dimension");
            continue;
        }
        const auto dimension=decode_boundary_dimension_entity(entity);
        if (!dimension.supported() || !dimension.dimension->assembly_target)
            invalid("Mixed stair resize has an unsupported component dimension carrier");
        if (roots.contains(component_root(dimension.dimension->assembly_target->component)) && ++associations>1000)
            invalid("Mixed stair resize component dimension association budget exceeded");
    }
    // All work and dependencies are inventoried before typed decoding allocates
    // layout treads and before any per-owner native manufacture.
    std::size_t bytes=authoring ? component_wire.dump().size() : 0,nodes{};
    if (bytes>proof_budget) invalid("Mixed stair resize aggregate typed capture byte budget exceeded");
    const auto charge_nodes=[&](const auto& self,const Json& value,std::size_t depth) -> void {
        if (depth>32 || ++nodes>65536) invalid("Mixed stair resize aggregate typed capture node/nesting budget exceeded");
        if (value.is_object()) for (const auto& [key,child]:value.items()) { (void)key;self(self,child,depth+1); }
        else if (value.is_array()) for (const auto& child:value) self(self,child,depth+1);
    };
    if (authoring) charge_nodes(charge_nodes,component_wire,0);
    for (const auto& request:requests) {
        const auto wire=stair_resize_capture_projection(actual.at(request.stair_id));
        charge_nodes(charge_nodes,wire,0);
        const auto size=wire.dump().size();
        // Raw source integer scalars can become longer floating encodings. This
        // bounded reserve covers the five owned values before native staging.
        constexpr std::size_t scalar_encoding_reserve=5*32;
        if (size>proof_budget-bytes || scalar_encoding_reserve>proof_budget-bytes-size)
            invalid("Mixed stair resize aggregate typed capture byte budget exceeded");
        bytes+=size+scalar_encoding_reserve;
    }
}

} // namespace

void require_stair_directional_resize_source_admission(const DocumentSnapshot& source,
    std::span<const StairDirectionalResizeRequest> requests) {
    preflight_stair_directional_resize_impl(source,nullptr,requests);
}

void preflight_component_stair_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const StairDirectionalResizeRequest> requests) {
    preflight_stair_directional_resize_impl(source,&authoring,requests);
}

ComponentStairDirectionalResizeComposition compose_component_stair_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& component,
    std::span<const StairDirectionalResizeRequest> requests,std::string_view message) {
    if (message.size()>1024 || message.find('\0')!=std::string_view::npos)
        invalid("Mixed stair resize message exceeds its bound");
    // A borrowed preparation is never authority for a different source.
    const auto wire=validate_phase_assembly_instance_property_edit_request(component.request());
    const auto authoring=decode_phase_assembly_instance_property_edit_authoring(wire.at("authoring"));
    preflight_component_stair_directional_resize(source,authoring,requests);
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,wire);
    (void)directional_component_authoring(replayed,requests.size());
    const auto& actual=source.entities();
    Entities staged=actual;
    std::vector<StairObjectEditIntent> edits;
    std::size_t bytes{};
    ComponentStairDirectionalResizeComposition result;
    std::string previous;
    for (const auto& request:requests) {
        if (!previous.empty() && previous>=request.stair_id) invalid("Mixed stair resize requires sorted unique operators");
        previous=request.stair_id;
        const auto& original=actual.at(request.stair_id);
        auto edited=stage_stair_plan_axis_resize_entity(original,request.sx,request.sy,request.anchor_m,request.frame_rotation_radians);
        if (!exact(original,edited)) {
            try {
                const auto capture=capture_stair_object_edit(original,edited);
                if (!capture) invalid("Mixed stair resize full pose/profile capture is unavailable");
                const auto size=encode_stair_object_edit_intent(*capture).dump().size();
                if (size>proof_budget-bytes) invalid("Mixed stair resize aggregate typed capture byte budget exceeded");
                bytes+=size;edits.push_back(*capture);
            } catch (const std::invalid_argument& error) {
                invalid(std::string("Mixed stair resize cannot preserve the source through full typed capture: ")+error.what());
            }
        }
        staged.at(request.stair_id)=std::move(edited);
        result.stair_selected_original_to_proposed.emplace(request.stair_id,request.stair_id);
    }
    // Validate every unit through the same full typed native admission as a
    // changed owner, while refusing any normalized dormant representation.
    std::vector<StairObjectEditIntent> all_edits=edits;
    for (const auto& request:requests) if (exact(actual.at(request.stair_id),staged.at(request.stair_id)))
        all_edits.push_back(decode_stair_object_edit_intent(stair_resize_capture_projection(actual.at(request.stair_id))));
    const auto physical=replay_stair_object_edit_entities(actual,all_edits);
    for (const auto& request:requests) if (!exact(physical.at(request.stair_id),staged.at(request.stair_id)))
        invalid("Mixed stair resize full original-source typed replay differs from raw staging");
    if (mixed_edit_identity_owners(actual)!=mixed_edit_identity_owners(physical) ||
        embedded_assembly_presentation_ids(actual)!=embedded_assembly_presentation_ids(physical))
        invalid("Mixed stair resize introduced or changed physical identity inventory");
    validate_directional_physical_freshness(source,{physical},replayed,std::nullopt,std::nullopt);
    std::map<ComponentTarget,ComponentTarget> destinations;
    for (const auto& intent:authoring.intents) {
        const auto root=component_root(intent.target);
        const auto destination=component_root(replayed.result().original_to_proposed.at(intent.target));
        const auto [entry,inserted]=destinations.emplace(root,destination);
        if (!inserted && entry->second!=destination) invalid("Mixed stair resize has conflicting component destinations");
    }
    result.components=finalize_component_directional_resize(source,replayed,authoring,physical,destinations,{});
    for (const auto& request:requests) if (!exact(result.components.entities.at(request.stair_id),staged.at(request.stair_id)))
        invalid("Mixed stair resize final component composition changed the exact native stair envelope");
    return result;
}


namespace {
// One checked batch budget covers component authoring and roof source/operator
// captures before any source/archive codec can manufacture native geometry.
struct RoofDirectionalCaptureBudget {
    std::size_t bytes{},nodes{};
    const char* diagnostic{"Mixed roof resize"};
    void reserve(std::size_t size) {
        if (size>proof_budget-bytes) invalid(std::string(diagnostic)+" aggregate capture byte budget exceeded");
        bytes+=size;
    }
    void shape(const Json& value,std::size_t depth=0) {
        if (depth>32 || ++nodes>65536) invalid(std::string(diagnostic)+" aggregate capture node/nesting budget exceeded");
        if ((value.is_array() || value.is_object()) && value.size()>4096)
            invalid(std::string(diagnostic)+" capture collection bound exceeded");
        if (value.is_string() && value.get_ref<const std::string&>().size()>16384)
            invalid(std::string(diagnostic)+" capture string bound exceeded");
        if (value.is_object()) for (const auto& [key,child]:value.items()) { (void)key;shape(child,depth+1); }
        else if (value.is_array()) for (const auto& child:value) shape(child,depth+1);
    }
    void charge(const Json& value) { shape(value);reserve(value.dump().size()); }
};
bool roof_resize_identity_field(const Json& object,const char* key,const std::set<std::string,std::less<>>& owners) {
    const auto found=object.find(key);
    return found!=object.end() && found->is_string() && owners.contains(found->get_ref<const std::string&>());
}
void preflight_roof_directional_resize_impl(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring* authoring,std::span<const RoofPlanResizeIntent> requests,
    bool validate_native_source=true) {
    if (requests.empty() || requests.size()>1000 || (authoring &&
        (authoring->intents.empty() || authoring->intents.size()>1000-requests.size())))
        invalid("Mixed roof resize aggregate selection bound exceeded");
    const auto& actual=source.entities();
    if (actual.size()>100000) invalid("Mixed roof resize source entity bound exceeded");
    DirectionalResizeSourceBudget raw;raw.portable_text=true;raw.diagnostic="Mixed roof resize";
    for (const auto& [id,entity]:actual) {
        if (id!=entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("Mixed roof resize requires actual identified source envelopes");
        raw.text(id);raw.text(entity.type);raw.visit(entity.properties);raw.visit(entity.extensions);
    }
    RoofDirectionalCaptureBudget capture;
    std::set<ComponentTarget> roots;
    std::set<std::string,std::less<>> component_owners,roofs;
    if (authoring) {
        const auto wire=encode_phase_assembly_instance_property_edit_authoring(*authoring);
        raw.visit(wire);capture.charge(wire);
        if (wire.at("version")!=7 && wire.at("version")!=8 && wire.at("version")!=9)
            invalid("Mixed roof resize requires component geometry request seven, eight or nine");
        for (const auto& intent:authoring->intents) {
            if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
                !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
                intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
                intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
                intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
                invalid("Mixed roof resize admits independent document component geometry only");
            const auto root=component_root(intent.target);
            const auto found=actual.find(*root.document_entity_id);
            if (found==actual.end() || found->second.type!="assembly_instance")
                invalid("Mixed roof resize independent component source owner is missing");
            const auto instance=decode_document_assembly_instance(found->second);
            if (instance.assembly_catalog_id!=root.catalog_id || instance.instance.id!=root.instance_id)
                invalid("Mixed roof resize component target differs from the actual source codec");
            roots.insert(root);component_owners.insert(*root.document_entity_id);
        }
    }
    for (const auto& request:requests) {
        if (request.anchor_mode!=PlanAxisResizeAnchorMode::shared_frame)
            invalid("Mixed roof resize requires shared-frame operators, including units");
        const auto operation=encode_roof_plan_resize_intent(request);
        capture.charge(operation);
        if (!roofs.insert(request.roof_id).second) invalid("Mixed roof resize requires unique actual owners");
        const auto found=actual.find(request.roof_id);
        if (found==actual.end() || found->second.type!="roof") invalid("Mixed roof resize actual source roof is missing");
        const auto& entity=found->second;
        const auto& p=entity.properties;
        if (!p.contains("version") || !p.at("version").is_number_integer() || p.at("version")!=1 ||
            !p.contains("form") || (p.at("form")!="sloped_roof_panel" && p.at("form")!="gable_roof" && p.at("form")!="hip_roof"))
            invalid("Mixed roof resize requires ordinary schema1 panel, gable or hip sources");
        for (const auto* key:{"roof_openings","transform","vertical_placement","phase_id"})
            if (p.contains(key)) invalid("Mixed roof resize refuses openings, transforms and explicit phase authority, including units");
        // The full selected envelope includes retained archives and opaque
        // receipts. Each pending archive has two closed physical frames plus
        // receipts bounded by the original properties, and the actual v2 operator.
        // Charging three additional property envelopes is conservative; reserve
        // numeric growth and archive4/record wrappers separately, with checked
        // addition. Charging the encoded operator also includes anchor_mode.
        capture.charge(p);capture.charge(entity.extensions);
        capture.reserve(entity.id.size());capture.reserve(entity.type.size());capture.reserve(128);
        if (request.scale_x!=1.0 || request.scale_y!=1.0) {
            capture.charge(p);capture.charge(p);capture.charge(p);capture.charge(operation);capture.reserve(2048);
            const auto archive=entity.extensions.find(std::string(roof_plan_resize_derivations_key));
            if (archive!=entity.extensions.end() && archive->is_object() && archive->contains("operations") &&
                archive->at("operations").is_array() && archive->at("operations").size()>=256)
                invalid("Mixed roof resize pending archive operation budget exceeded");
        }
    }
    AssemblyCatalogTransferBudget catalogs;
    for (const auto& [id,entity]:actual) {
        (void)id;if (entity.type=="assembly_model") admit_existing_assembly_catalog_work(entity,catalogs);
    }
    std::size_t associations{};
    for (const auto& [id,entity]:actual) {
        (void)id;
        const auto& p=entity.properties;
        if (entity.type=="model_phases") {
            const auto registry=ModelPhases::from_json(p.at("model"));
            for (const auto& member:registry.entity_ids()) if (roofs.contains(member))
                invalid("Mixed roof resize requires absence from every phase registry, including baseline-only");
        }
        if (entity.type=="roof_join") {
            const auto join=parse_roof_join(p,entity.id);
            for (const auto& member:join.roof_ids) if (roofs.contains(member))
                invalid("Mixed roof resize excludes affected actual roof joins, including units");
        }
        if (entity.type=="assembly_model") {
            const auto model=AssemblyModel::from_json(p.at("model"));
            for (const auto& row:model.instances()) if (row.placement && roofs.contains(row.placement->host_entity_id))
                invalid("Mixed roof resize excludes actual hosted embedded component roots, including units");
        }
        if (entity.type==kSheetViewEntityType) {
            const auto model=decode_sheet_view_entity(entity);
            for (const auto& view:model.views()) for (const auto& overlay:view.overlays)
                if (overlay.dimension_binding && roofs.contains(overlay.dimension_binding->object_id))
                    invalid("Mixed roof resize excludes roof-owned saved-view dimensions, including units");
        }
        if (entity.type=="constraint") {
            const auto decoded=decode_constraint_entity(entity);
            if (decoded.supported()) {
                for (const auto& binding:decoded.constraint->bindings) if (roofs.contains(binding.owner_id))
                    invalid("Mixed roof resize excludes affected persistent constraints, including units");
            } else if (p.contains("bindings") && p.at("bindings").is_array()) {
                for (const auto& binding:p.at("bindings"))
                    if (binding.is_object() && (roof_resize_identity_field(binding,"owner_id",roofs) ||
                        roof_resize_identity_field(binding,"wall_id",roofs)))
                        invalid("Mixed roof resize excludes unsupported affected persistent constraints");
            }
        }
        for (const auto* key:{"entity_ids","wall_ids","owner_id","boundary_id","dependency_ids","dependencies"})
            if (p.contains(key) && stair_resize_reference(p.at(key),roofs))
                invalid("Mixed roof resize excludes unsupported declared dependencies, including units");
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto target=p.find("target");
        if (target!=p.end() && target->is_object() && roof_resize_identity_field(*target,"entity_id",roofs))
            invalid("Mixed roof resize excludes roof-owned dimensions, including units");
        bool component_association=target!=p.end() && target->is_object() &&
            roof_resize_identity_field(*target,"entity_id",component_owners);
        if (target!=p.end() && target->is_object() && target->contains("assembly")) {
            const auto& assembly=target->at("assembly");
            if (assembly.is_object() && assembly.contains("catalog_id") && assembly.at("catalog_id").is_string() &&
                assembly.contains("instance_id") && assembly.at("instance_id").is_string() &&
                assembly.contains("document_entity_id") && assembly.at("document_entity_id").is_string())
                component_association=component_association || roots.contains(ComponentTarget{assembly.at("catalog_id").get<std::string>(),
                    assembly.at("instance_id").get<std::string>(),assembly.at("document_entity_id").get<std::string>(),{}});
        }
        if (!is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(entity).format)) {
            if (component_association) invalid("Mixed roof resize has an unsupported affected component dimension");
            continue;
        }
        const auto dimension=decode_boundary_dimension_entity(entity);
        if (!dimension.supported() || !dimension.dimension->assembly_target)
            invalid("Mixed roof resize has an unsupported component dimension carrier");
        if (roots.contains(component_root(dimension.dimension->assembly_target->component)) && ++associations>1000)
            invalid("Mixed roof resize component dimension association budget exceeded");
    }
    // Only after the complete batch's resource and relationship inventory do
    // the authoritative source codecs validate geometry and retained archives.
    if (validate_native_source)
        for (const auto& request:requests) validate_roof_plan_resize_source_entity(actual.at(request.roof_id));
}

// Properties45 reserves all three captures together. This pass is deliberately
// analytical: no preparation, resize producer, roof archive replay or native
// inspector runs before the aggregate inventory has admitted the complete batch.
void preflight_roof_skylight_directional_resize_impl(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring* authoring,
    std::span<const AssemblyDirectionalResizeTarget> targets,
    std::span<const RoofPlanResizeIntent> roofs,
    std::span<const RoofOpeningDirectionalResizeRequest> children) {
    const auto component_count=authoring ? authoring->intents.size() : targets.size();
    if (!component_count || roofs.empty() || children.empty() || component_count>1000 ||
        roofs.size()>1000-component_count || children.size()>1000-component_count-roofs.size())
        invalid("Mixed roof/skylight resize aggregate selection bound exceeded");
    const auto& actual=source.entities();
    if (actual.size()>100000) invalid("Mixed roof/skylight resize source entity bound exceeded");
    DirectionalResizeSourceBudget raw;raw.portable_text=true;raw.diagnostic="Mixed roof/skylight resize";
    const auto raw_entities=[&](const Entities& entities) {
        for (const auto& [id,entity]:entities) {
            if (id!=entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
                invalid("Mixed roof/skylight resize requires actual identified source envelopes");
            raw.text(id);raw.text(entity.type);raw.visit(entity.properties);raw.visit(entity.extensions);
        }
    };
    raw_entities(actual);
    for (const auto& revision:source.history()) {
        raw_entities(revision.entities);
        if (revision.boundary_constraint_changes) raw.visit(command_to_json(*revision.boundary_constraint_changes));
    }
    RoofDirectionalCaptureBudget capture;capture.diagnostic="Mixed roof/skylight resize";
    std::set<ComponentTarget> roots;
    std::set<std::string,std::less<>> component_owners,whole_ids,hosts,catalog_ids;
    const auto independent=[&](const std::string& id,const ComponentTarget* qualified) {
        if (!component_owners.insert(id).second && !authoring)
            invalid("Mixed roof/skylight resize repeats an actual component root");
        const auto found=actual.find(id);
        if (found==actual.end() || found->second.type!="assembly_instance")
            invalid("Mixed roof/skylight resize actual independent component is missing");
        const auto instance=decode_document_assembly_instance(found->second);
        const ComponentTarget root{instance.assembly_catalog_id,instance.instance.id,id,{}};
        if (qualified && component_root(*qualified)!=root)
            invalid("Mixed roof/skylight resize component target differs from the actual source codec");
        roots.insert(root);catalog_ids.insert(root.catalog_id);
    };
    if (authoring) {
        const auto wire=encode_phase_assembly_instance_property_edit_authoring(*authoring);
        raw.visit(wire);capture.charge(wire);
        if (wire.at("version")!=7 && wire.at("version")!=8 && wire.at("version")!=9)
            invalid("Mixed roof/skylight resize requires component geometry request seven, eight or nine");
        for (const auto& intent:authoring->intents) {
            if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
                !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
                intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
                intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
                intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
                invalid("Mixed roof/skylight resize admits independent document component geometry only");
            independent(*intent.target.document_entity_id,&intent.target);
        }
    } else for (const auto& target:targets) {
        const auto& operation=target.operation;
        if (target.independent_entity_id.empty() || target.independent_entity_id.size()>128 ||
            !std::isfinite(operation.world_anchor_m.x) || !std::isfinite(operation.world_anchor_m.y) ||
            !std::isfinite(operation.x_factor) || !std::isfinite(operation.y_factor) ||
            operation.x_factor<=0 || operation.y_factor<=0 || !operation.world_axis_rotation_radians ||
            !std::isfinite(*operation.world_axis_rotation_radians))
            invalid("Mixed roof/skylight resize requires actual finite component operators");
        if (operation.x_factor!=targets.front().operation.x_factor || operation.y_factor!=targets.front().operation.y_factor)
            invalid("Mixed roof/skylight resize component operators have inconsistent factors");
        if (operation.operation_frame) {
            const auto& frame=*operation.operation_frame;
            if (!std::isfinite(frame.translation_m.x) || !std::isfinite(frame.translation_m.y) ||
                !std::isfinite(frame.translation_m.z) || !std::isfinite(frame.rotation_radians) ||
                frame.scale!=1 || frame.vertical_scale!=1 || frame.mirrored_y)
                invalid("Mixed roof/skylight resize component operation frame must be proper and rigid");
        }
        independent(target.independent_entity_id,nullptr);
        capture.reserve(512);
    }
    for (const auto& roof:roofs) {
        if (!whole_ids.insert(roof.roof_id).second || roof.anchor_mode!=PlanAxisResizeAnchorMode::shared_frame)
            invalid("Mixed roof/skylight resize requires unique shared-frame whole roofs, including units");
        capture.charge(encode_roof_plan_resize_intent(roof));
    }
    std::set<std::pair<std::string,std::string>> child_ids;
    for (const auto& child:children) {
        if (whole_ids.contains(child.target.roof_id))
            invalid("Mixed roof/skylight resize whole owners and child hosts must be disjoint, including units");
        if (!child_ids.emplace(child.target.roof_id,child.target.opening_id).second ||
            child.target.roof_id.empty() || child.target.roof_id.size()>128 || child.target.opening_id.empty() || child.target.opening_id.size()>128 ||
            !std::isfinite(child.sx) || !std::isfinite(child.sy) || child.sx<=0 || child.sy<=0 ||
            !std::isfinite(child.anchor_m.x) || !std::isfinite(child.anchor_m.y) || !std::isfinite(child.frame_rotation_radians))
            invalid("Mixed roof/skylight resize requires unique finite qualified child operators");
        hosts.insert(child.target.roof_id);
        capture.charge(Json{{"roof_id",child.target.roof_id},{"opening_id",child.target.opening_id},
            {"sx",child.sx},{"sy",child.sy},{"anchor_m",{child.anchor_m.x,child.anchor_m.y}},
            {"frame_rotation_radians",child.frame_rotation_radians}});
    }
    for (const auto& host:hosts) {
        const auto found=actual.find(host);
        if (found==actual.end() || found->second.type!="roof") invalid("Mixed roof/skylight resize actual child host is missing");
        if (found->second.properties.contains("phase_id"))
            invalid("Mixed roof/skylight resize child hosts require literal phase_id absence, including null and units");
    }
    AssemblyCatalogTransferBudget catalogs;
    for (const auto& [id,entity]:actual) {
        if (entity.type=="model_phases") {
            const auto registry=ModelPhases::from_json(entity.properties.at("model"));
            for (const auto& member:registry.entity_ids()) if (hosts.contains(member))
                invalid("Mixed roof/skylight resize child hosts must be absent from every phase registry, including units");
        }
        if (entity.type!="assembly_model") continue;
        admit_existing_assembly_catalog_work(entity,catalogs);
        for (const auto& row:entity.properties.at("model").at("instances")) {
            if (!row.contains("placement") || row.at("placement").is_null() ||
                !hosts.contains(row.at("placement").at("host_entity_id").get<std::string>())) continue;
            roots.insert(ComponentTarget{id,row.at("id").get<std::string>(),std::nullopt,{}});catalog_ids.insert(id);
        }
    }
    // Count each actual dimension once across independent and every requested
    // hosted root before any pending capture growth or native consequence work.
    std::vector<const Entity*> dimensions;
    for (const auto& [id,entity]:actual) {
        (void)id;
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto target=entity.properties.find("target");
        bool affected=target!=entity.properties.end() && target->is_object() &&
            roof_resize_identity_field(*target,"entity_id",component_owners);
        if (target!=entity.properties.end() && target->is_object() && target->contains("assembly")) {
            const auto& a=target->at("assembly");
            if (a.is_object() && a.contains("catalog_id") && a.at("catalog_id").is_string() &&
                a.contains("instance_id") && a.at("instance_id").is_string()) {
                std::optional<std::string> document;
                if (a.contains("document_entity_id") && !a.at("document_entity_id").is_null()) {
                    if (!a.at("document_entity_id").is_string()) invalid("Mixed roof/skylight resize component dimension identity is invalid");
                    document=a.at("document_entity_id").get<std::string>();
                }
                affected=affected || roots.contains(ComponentTarget{a.at("catalog_id").get<std::string>(),a.at("instance_id").get<std::string>(),document,{}});
            }
        }
        if (!is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(entity).format)) {
            if (affected) invalid("Mixed roof/skylight resize has an unsupported affected component dimension");
            continue;
        }
        const auto decoded=decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || !decoded.dimension->assembly_target)
            invalid("Mixed roof/skylight resize has an unsupported component dimension carrier");
        if (!roots.contains(component_root(decoded.dimension->assembly_target->component))) continue;
        dimensions.push_back(&entity);
        if (dimensions.size()>1000) invalid("Mixed roof/skylight resize combined dimension association budget exceeded");
    }
    const auto envelope=[&](const Entity& entity,unsigned copies) {
        for (unsigned i=0;i<copies;++i) {capture.charge(entity.properties);capture.charge(entity.extensions);}
        capture.reserve(entity.id.size());capture.reserve(entity.type.size());capture.reserve(256);
    };
    for (const auto& roof:roofs) {
        const auto found=actual.find(roof.roof_id);
        if (found==actual.end() || found->second.type!="roof") invalid("Mixed roof/skylight resize actual whole roof is missing");
        envelope(found->second,1);
        if (roof.scale_x!=1 || roof.scale_y!=1) {
            for (int i=0;i<3;++i) capture.charge(found->second.properties);
            capture.charge(encode_roof_plan_resize_intent(roof));capture.reserve(2048);
        }
    }
    // One host capture/proof reservation per distinct host, even with several
    // selected children or an all-unit roster. Raw archives and receipts are
    // retained; pending typed leaf/phase32 numeric and record growth is extra.
    for (const auto& host:hosts) {envelope(actual.at(host),4);capture.reserve(4096);}
    for (const auto& child:children) {(void)child;capture.reserve(2048);}
    for (const auto& id:catalog_ids) {
        const auto found=actual.find(id);
        if (found==actual.end() || found->second.type!="assembly_model") invalid("Mixed roof/skylight resize actual component catalog is missing");
        // Reserve shared rows, a possible private baseline catalog and the
        // before/after qualified contribution proof without assuming units.
        envelope(found->second,4);
    }
    // Walk selected source types analytically, charging repeated DAG paths and
    // actual own overrides. No manufactured candidate or fake authoring exists.
    std::size_t selected_nodes{};
    for (const auto& owner:component_owners) {
        const auto& entity=actual.at(owner);envelope(entity,3);capture.reserve(4096);
        const auto instance=decode_document_assembly_instance(entity);
        // Several independent roots in one catalog may reserve separate
        // baseline catalog copies. A once-per-catalog source charge alone
        // cannot bound those allocations, including an initial unit gesture.
        envelope(actual.at(instance.assembly_catalog_id),3);
        const auto model=AssemblyModel::from_json(actual.at(instance.assembly_catalog_id).properties.at("model"));
        const auto visit=[&](const auto& self,const std::string& type_id,std::vector<std::string> path)->void {
            if (path.size()>=32 || ++selected_nodes>1000) invalid("Mixed roof/skylight resize pending component node budget exceeded");
            const auto type=std::find_if(model.types().begin(),model.types().end(),[&](const auto& row) {return row.id==type_id;});
            if (type==model.types().end()) invalid("Mixed roof/skylight resize actual source type is missing");
            const auto* profiles=&type->profiles;
            if (path.empty() && instance.instance.profile_overrides) profiles=&*instance.instance.profile_overrides;
            for (const auto& override:instance.instance.nested_overrides)
                if (override.part_path==path && override.profile_overrides) profiles=&*override.profile_overrides;
            const auto wire=encode_assembly_profiles(*profiles);
            for (int i=0;i<3;++i) capture.charge(wire);
            capture.charge(Json(path));capture.reserve(2048);
            for (const auto& profile:*profiles) {
                // Numeric map, fresh profile/ring/edge/vertex identities and
                // reviewed selectors may grow even on the initial unit frame.
                capture.reserve(1024);
                const auto ring=[&](const auto& segments) {for (const auto& segment:segments) {(void)segment;capture.reserve(512);}};
                ring(profile.outer);for (const auto& hole:profile.holes) ring(hole);
            }
            for (const auto& part:type->parts) {auto child_path=path;child_path.push_back(part.id);self(self,part.type_id,std::move(child_path));}
        };
        visit(visit,instance.instance.type_id,{});
    }
    for (const auto* dimension:dimensions) {envelope(*dimension,3);capture.reserve(2048);}
    // Membership/registry revisions, source fences, allocations, review rows,
    // and proof wrappers reserve space separately from their source envelopes.
    for (const auto& [id,entity]:actual) if (entity.type=="model_phases" || entity.type=="assembly_phase_membership") {
        (void)id;envelope(entity,3);
    }
    capture.reserve(8192);
    for (const auto& host:hosts) {
        const auto object=decode_roof_entity(actual.at(host));
        std::visit([&](const auto& roof) {
            std::set<std::string,std::less<>> identities;
            for (const auto& opening:roof.openings)
                if (!identities.insert(opening.id).second) invalid("Mixed roof/skylight resize actual child identities are duplicated");
            for (const auto& child:children) if (child.target.roof_id==host) {
                const auto opening=std::find_if(roof.openings.begin(),roof.openings.end(),[&](const auto& row) {return row.id==child.target.opening_id;});
                if (opening==roof.openings.end() || !opening->skylight)
                    invalid("Mixed roof/skylight resize requires actual typed skylight children");
            }
        },object);
    }
    // Preserve all Properties44 relationship restrictions, without invoking
    // its final native source/archive validator in this pure early gate.
    preflight_roof_directional_resize_impl(source,authoring,roofs,false);
}
} // namespace

void require_roof_skylight_directional_resize_source_admission(const DocumentSnapshot& source,
    std::span<const AssemblyDirectionalResizeTarget> components,std::span<const RoofPlanResizeIntent> roofs,
    std::span<const RoofOpeningDirectionalResizeRequest> children) {
    preflight_roof_skylight_directional_resize_impl(source,nullptr,components,roofs,children);
}
void preflight_component_roof_skylight_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const RoofPlanResizeIntent> roofs,
    std::span<const RoofOpeningDirectionalResizeRequest> children) {
    preflight_roof_skylight_directional_resize_impl(source,&authoring,{},roofs,children);
}

void require_roof_directional_resize_source_admission(const DocumentSnapshot& source,
    std::span<const RoofPlanResizeIntent> requests) {
    preflight_roof_directional_resize_impl(source,nullptr,requests);
}
void preflight_component_roof_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const RoofPlanResizeIntent> requests) {
    preflight_roof_directional_resize_impl(source,&authoring,requests);
}
namespace {
PhysicalDirectionalResizeContribution<ComponentRoofDirectionalResizeComposition> build_roof_directional_physical_contribution(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& replayed,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const RoofPlanResizeIntent> requests) {
    std::string previous;
    for (const auto& request:requests) {
        if (!previous.empty() && previous>=request.roof_id) invalid("Mixed roof resize requires sorted unique operators");
        previous=request.roof_id;
    }
    const auto& actual=source.entities();
    // This private contribution is one complete native replay of the original
    // source. Capture binds each full raw envelope to its actual operator;
    // neither candidate geometry nor its archive is a producer operation.
    auto physical=replay_roof_plan_resize_entities(actual,
        std::vector<RoofPlanResizeIntent>(requests.begin(),requests.end()));
    ComponentRoofDirectionalResizeComposition result;
    for (const auto& request:requests) {
        const auto captured=capture_roof_plan_resize(actual.at(request.roof_id),physical.at(request.roof_id),request);
        if (captured && encode_roof_plan_resize_intent(*captured).dump()!=encode_roof_plan_resize_intent(request).dump())
            invalid("Mixed roof resize actual operator capture changed the requested operator");
        result.roof_selected_original_to_proposed.emplace(request.roof_id,request.roof_id);
    }
    if (mixed_edit_identity_owners(actual)!=mixed_edit_identity_owners(physical) ||
        embedded_assembly_presentation_ids(actual)!=embedded_assembly_presentation_ids(physical))
        invalid("Mixed roof resize changed the physical owner or child identity inventory");
    validate_directional_physical_freshness(source,{physical},replayed,std::nullopt,std::nullopt);
    std::map<ComponentTarget,ComponentTarget> destinations;
    for (const auto& intent:authoring.intents) {
        const auto root=component_root(intent.target);
        const auto destination=component_root(replayed.result().original_to_proposed.at(intent.target));
        const auto [entry,inserted]=destinations.emplace(root,destination);
        if (!inserted && entry->second!=destination) invalid("Mixed roof resize has conflicting component destinations");
    }
    return {std::move(result),std::move(physical),std::move(destinations),{}, {}};
}
} // namespace

ComponentRoofDirectionalResizeComposition compose_component_roof_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& component,
    std::span<const RoofPlanResizeIntent> requests,std::string_view message) {
    if (message.size()>1024 || message.find('\0')!=std::string_view::npos)
        invalid("Mixed roof resize message exceeds its bound");
    const auto wire=validate_phase_assembly_instance_property_edit_request(component.request());
    const auto authoring=decode_phase_assembly_instance_property_edit_authoring(wire.at("authoring"));
    preflight_component_roof_directional_resize(source,authoring,requests);
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,wire);
    (void)directional_component_authoring(replayed,requests.size());
    auto lane=build_roof_directional_physical_contribution(source,replayed,authoring,requests);
    lane.result.components=finalize_component_directional_resize(source,replayed,authoring,lane.entities,lane.destinations,{});
    for (const auto& request:requests)
        if (!exact(lane.result.components.entities.at(request.roof_id),lane.entities.at(request.roof_id)))
            invalid("Mixed roof resize final composition changed the exact original-source native roof envelope");
    return std::move(lane.result);
}

ComponentRoofSkylightDirectionalResizeComposition compose_component_roof_skylight_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& component,
    std::span<const RoofPlanResizeIntent> roofs,std::span<const RoofOpeningDirectionalResizeRequest> children,
    std::string_view message) {
    if (message.size()>1024 || message.find('\0')!=std::string_view::npos)
        invalid("Mixed roof/skylight resize message exceeds its bound");
    const auto wire=validate_phase_assembly_instance_property_edit_request(component.request());
    const auto authoring=decode_phase_assembly_instance_property_edit_authoring(wire.at("authoring"));
    preflight_component_roof_skylight_directional_resize(source,authoring,roofs,children);
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,wire);
    (void)directional_component_authoring(replayed,roofs.size()+children.size());
    auto whole=build_roof_directional_physical_contribution(source,replayed,authoring,roofs);
    auto openings=build_roof_opening_directional_physical_contribution(source,replayed,authoring,children,message);
    const std::vector<Entities> lanes{whole.entities,openings.entities};
    validate_directional_physical_freshness(source,lanes,replayed,std::nullopt,std::nullopt);
    const auto physical=union_directional_physical_candidates(source.entities(),lanes);
    merge_directional_destinations(whole.destinations,openings.destinations);
    ComponentRoofSkylightDirectionalResizeComposition result;
    result.components=finalize_component_directional_resize(source,replayed,authoring,physical,whole.destinations,{});
    for (const auto& roof:roofs)
        if (!exact(result.components.entities.at(roof.roof_id),whole.entities.at(roof.roof_id)))
            invalid("Mixed roof/skylight resize final composition changed the exact original-source native roof envelope");
    for (const auto& child:children)
        if (!exact(result.components.entities.at(child.target.roof_id),openings.entities.at(child.target.roof_id)))
            invalid("Mixed roof/skylight resize final composition changed the exact original-source child-host envelope");
    result.roof_selected_original_to_proposed=std::move(whole.result.roof_selected_original_to_proposed);
    result.roof_children=std::move(openings.result.roof_children);
    result.phase_authorings=std::move(openings.result.phase_authorings);
    return result;
}
ComponentSlabDirectionalResizeComposition compose_component_slab_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& components,
    std::span<const SlabGeometryEditIntent> slabs,
    const std::optional<ComponentSlabDirectionalResizeAllocation>& allocation,std::string_view message) {
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,components.request());
    const auto authoring=directional_component_authoring(replayed,slabs.size());
    auto lane=build_slab_directional_physical_contribution(source,replayed,authoring,slabs,allocation,message);
    lane.result.components=finalize_component_directional_resize(source,replayed,authoring,lane.entities,lane.destinations,lane.dimension_ids);
    return std::move(lane.result);
}
ComponentStructuralDirectionalResizeComposition compose_component_structural_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& components,
    std::span<const StructuralDirectionalResizeRequest> requests,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>& allocation,std::string_view message) {
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,components.request());
    const auto authoring=directional_component_authoring(replayed,requests.size());
    auto lane=build_structural_directional_physical_contribution(source,replayed,authoring,requests,allocation,message);
    lane.result.components=finalize_component_directional_resize(source,replayed,authoring,lane.entities,lane.destinations,lane.dimension_ids);
    verify_directional_structural_result(lane.result.components.entities,lane.entities,lane.structural_hosts);
    return std::move(lane.result);
}
ComponentRoofOpeningDirectionalResizeComposition compose_component_roof_opening_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& components,
    std::span<const RoofOpeningDirectionalResizeRequest> requests,std::string_view message) {
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,components.request());
    const auto authoring=directional_component_authoring(replayed,requests.size());
    auto lane=build_roof_opening_directional_physical_contribution(source,replayed,authoring,requests,message);
    lane.result.components=finalize_component_directional_resize(source,replayed,authoring,lane.entities,lane.destinations,lane.dimension_ids);
    return std::move(lane.result);
}
ComponentPhysicalDirectionalResizeComposition compose_component_physical_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& components,
    std::span<const SlabGeometryEditIntent> slabs,
    const std::optional<ComponentSlabDirectionalResizeAllocation>& slab_allocation,
    std::span<const StructuralDirectionalResizeRequest> structures,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>& structural_allocation,
    std::span<const RoofOpeningDirectionalResizeRequest> openings,std::string_view message) {
    const auto families=static_cast<unsigned>(!slabs.empty())+static_cast<unsigned>(!structures.empty())+
        static_cast<unsigned>(!openings.empty());
    if (families<2 || message.size()>1024 || message.find('\0')!=std::string_view::npos ||
        slabs.size()>1000 || structures.size()>1000 || openings.size()>1000 ||
        (slabs.empty() && slab_allocation) || (structures.empty() && structural_allocation))
        invalid("Combined physical resize requires a bounded complete roster and exact family allocation presence");
    // The sealed component contribution has already replayed from its captured
    // source. Authenticate it without deriving it again or feeding a physical
    // candidate back into the component producer.
    require_phase_assembly_instance_property_edit_source_current(source,components);
    const auto& replayed=components;
    const auto authoring=directional_component_authoring(replayed,slabs.size()+structures.size()+openings.size());
    ComponentPhysicalDirectionalResizeComposition result;
    std::vector<Entities> lanes;
    std::map<ComponentTarget,ComponentTarget> destinations;
    PhaseSlabReplacementIdentityMap dimensions;
    std::vector<std::string> structural_hosts;
    const auto append=[&](auto&& lane,const auto& owners,const auto& selected,const auto& bindings,const auto& proofs) {
        merge_directional_destinations(destinations,lane.destinations);
        for (const auto& [original,proposed]:lane.dimension_ids)
            if (!dimensions.emplace(original,proposed).second)
                invalid("Combined physical resize repeats a provisional source dimension allocation");
        merge_physical_owner_correspondence(source.entities(),result.original_to_proposed,owners);
        merge_directional_destinations(result.selected_original_to_proposed,selected);
        merge_directional_destinations(result.hosted_bindings,bindings);
        result.phase_authorings.insert(result.phase_authorings.end(),proofs.begin(),proofs.end());
        lanes.push_back(std::move(lane.entities));
    };
    if (!slabs.empty()) {
        auto lane=build_slab_directional_physical_contribution(source,replayed,authoring,slabs,slab_allocation,message);
        append(lane,lane.result.slab_original_to_proposed,lane.result.slab_selected_original_to_proposed,
            lane.result.slab_hosted_bindings,lane.result.slab_phase_authorings);
    }
    if (!structures.empty()) {
        auto lane=build_structural_directional_physical_contribution(source,replayed,authoring,structures,structural_allocation,message);
        structural_hosts=lane.structural_hosts;
        append(lane,lane.result.structural_original_to_proposed,lane.result.structural_selected_original_to_proposed,
            lane.result.structural_hosted_bindings,lane.result.structural_phase_authorings);
    }
    if (!openings.empty()) {
        auto lane=build_roof_opening_directional_physical_contribution(source,replayed,authoring,openings,message);
        merge_directional_destinations(destinations,lane.destinations);
        result.roof_children=std::move(lane.result.roof_children);
        result.phase_authorings.insert(result.phase_authorings.end(),lane.result.phase_authorings.begin(),lane.result.phase_authorings.end());
        lanes.push_back(std::move(lane.entities));
    }
    validate_directional_physical_freshness(source,lanes,replayed,slab_allocation,structural_allocation);
    const auto physical=union_directional_physical_candidates(source.entities(),lanes);
    const auto source_aliases=embedded_assembly_presentation_ids(source.entities());
    const auto final_aliases=embedded_assembly_presentation_ids(physical);
    for (const auto& [binding,destination]:result.hosted_bindings) {
        const auto& original=source_aliases.at(binding);
        const auto& proposed=final_aliases.at(destination);
        const auto [entry,inserted]=result.original_to_proposed.emplace(original,proposed);
        if (!inserted && entry->second!=proposed)
            invalid("Combined physical resize has conflicting qualified presentation alias destinations");
    }
    result.components=finalize_component_directional_resize(source,replayed,authoring,physical,destinations,dimensions);
    if (!structural_hosts.empty()) verify_directional_structural_result(result.components.entities,physical,structural_hosts);
    for (const auto& [binding,target]:result.roof_children) {
        (void)binding;(void)roof_opening_group_member_center_world(result.components.entities,target);
    }
    return result;
}

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

namespace {
Json& mutable_component_row(Entities& entities,const ComponentTarget& target) {
    if (target.document_entity_id) return entities.at(*target.document_entity_id).properties.at("instance");
    auto& rows=entities.at(target.catalog_id).properties.at("model").at("instances");
    const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& value) { return value.at("id")==target.instance_id; });
    if (row==rows.end()) invalid("Coordinated component Properties lost its exact carried row");
    return *row;
}

template<class T> std::vector<T> substitute_component_proposals(const std::vector<T>& rows,
    const std::map<T,T>& substitutions) {
    std::vector<T> result;
    for (const auto& row:rows) {
        const auto found=substitutions.find(row);
        const auto& destination=found==substitutions.end() ? row : found->second;
        if (std::find(result.begin(),result.end(),destination)==result.end()) result.push_back(destination);
    }
    return result;
}

void substitute_component_memberships(Entities& components,
    const std::map<AssemblyPhaseRoot,AssemblyPhaseRoot>& embedded,
    const std::map<std::string,std::string>& independent) {
    for (auto& [id,entity]:components) {
        (void)id;
        if (entity.type==assembly_phase_membership_entity_type) {
            const auto saved=AssemblyPhaseMembership::from_json(entity.properties.at("model"));
            for (const auto& root:saved.baseline_roots()) if (embedded.contains(root))
                invalid("Coordinated component substitution cannot change a baseline root");
            auto alternatives=saved.alternatives();
            for (auto& alternative:alternatives) {
                for (const auto& root:alternative.demolished_roots) if (embedded.contains(root))
                    invalid("Coordinated component substitution cannot change a demolished source root");
                alternative.proposed_roots=substitute_component_proposals(alternative.proposed_roots,embedded);
            }
            const auto replacement=AssemblyPhaseMembership::create(saved.registry_id(),
                substitute_component_proposals(saved.roots(),embedded),saved.baseline_roots(),std::move(alternatives));
            entity.properties.at("model")=retain_assembly_phase_membership_source(entity.properties.at("model"),replacement);
        } else if (entity.type=="model_phases") {
            const auto saved=ModelPhases::from_json(entity.properties.at("model"));
            for (const auto& owner:saved.baseline_ids()) if (independent.contains(owner))
                invalid("Coordinated component substitution cannot change a baseline owner");
            auto alternatives=saved.alternatives();
            for (auto& alternative:alternatives) {
                for (const auto& owner:alternative.demolished_ids) if (independent.contains(owner))
                    invalid("Coordinated component substitution cannot change a demolished source owner");
                alternative.proposed_ids=substitute_component_proposals(alternative.proposed_ids,independent);
            }
            const auto replacement=ModelPhases::create(substitute_component_proposals(saved.entity_ids(),independent),
                saved.baseline_ids(),std::move(alternatives),saved.active_alternative());
            entity.properties.at("model")=retain_model_phase_source(entity.properties.at("model"),replacement);
        }
    }
}

// Only fresh, independently replayed component proposals are substituted.
// Source presentation records remain byte-for-byte retained; matching base
// copies survive when both lanes carried the same bound callout.
void substitute_component_presentation_proposals(Entities& components,const Entities& carried,
    const std::map<std::string,std::string,std::less<>>& substitutions,
    const std::map<std::string,std::string,std::less<>>& overlay_allocations) {
    if (substitutions.empty()) return;
    const auto remap=[&](Json& row,const char* field) {
        if (!row.contains(field) || !row.at(field).is_string()) return;
        if (const auto found=substitutions.find(row.at(field).get<std::string>());found!=substitutions.end())
            row.at(field)=found->second;
    };
    const auto deduplicate=[&](Json& rows,const char* field,bool paired_kind=false) {
        Json retained=Json::array();
        std::map<std::pair<std::string,std::string>,std::size_t> destinations;
        for (const auto& row:rows) {
            const auto id=field ? row.at(field).get<std::string>() : row.get<std::string>();
            const auto kind=paired_kind ? row.at("target_kind").get<std::string>() : std::string{};
            const auto [found,inserted]=destinations.emplace(std::pair{id,kind},retained.size());
            if (inserted) retained.push_back(row);
            else if (!exact_json(retained.at(found->second),row))
                invalid("Coordinated component presentation proposals conflict at their actual survivor");
        }
        rows=std::move(retained);
    };
    std::set<std::string,std::less<>> fresh_overlays;
    for (const auto& [original,proposed]:overlay_allocations) { (void)original; fresh_overlays.insert(proposed); }
    for (auto& [id,entity]:components) {
        if (entity.type==kSheetViewEntityType) {
            (void)decode_sheet_view_entity(entity);
            const auto& base=carried.at(id);
            (void)decode_sheet_view_entity(base);
            for (auto& view:entity.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    for (auto& object:view.at("object_ids"))
                        if (const auto found=substitutions.find(object.get<std::string>());found!=substitutions.end()) object=found->second;
                    deduplicate(view.at("object_ids"),nullptr);
                }
                auto& presentation=view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null()) {
                    auto& rows=presentation.at("appearance").at("objects");
                    for (auto& row:rows) remap(row,"object_id");
                    deduplicate(rows,"object_id");
                }
                if (!view.contains("overlays")) continue;
                const auto& base_views=base.properties.at("model").at("views");
                const auto base_view=std::find_if(base_views.begin(),base_views.end(),[&](const auto& row) {
                    return row.at("id")==view.at("id");
                });
                if (base_view==base_views.end()) invalid("Coordinated component presentation lost its actual source view");
                auto& rows=view.at("overlays");
                for (auto& row:rows) {
                    remap(row,"object_id");
                    if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                        remap(row.at("dimension_binding"),"object_id");
                }
                if (base_view->contains("overlays")) rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
                    if (!fresh_overlays.contains(row.at("id").template get<std::string>())) return false;
                    auto content=row; content.erase("id");
                    return std::any_of(base_view->at("overlays").begin(),base_view->at("overlays").end(),[&](auto candidate) {
                        candidate.erase("id"); return exact_json(content,candidate);
                    });
                }),rows.end());
            }
            validate_sheet_view_entity(entity);
        } else if (entity.type==kAnnotationEntityType) {
            validate_annotation_entity(entity);
            auto& rows=entity.properties.at("state").at("overrides");
            for (auto& row:rows) remap(row,"target_id");
            deduplicate(rows,"target_id",true);
            validate_annotation_entity(entity);
        }
    }
}
} // namespace

CoordinatedComponentPropertyComposition compose_coordinated_component_property_dependencies(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditPreparation& components,
    const MixedSelectionPropertyEditPreparation& base) {
    const auto replayed=replay_phase_assembly_instance_property_edit_preparation(source,components.request());
    const auto& wire=base.intent();
    const auto version=wire.at("version").get<int>();
    if (!((version>=5 && version<=13) || version==15 || version==21 || version==24 || version==27) || !wire.at("room_reviews").empty() ||
        (wire.contains("phase_room_reviews") && !wire.at("phase_room_reviews").empty()) ||
        (wire.contains("assembly_request") && !wire.at("assembly_request").is_null()))
        invalid("Coordinated component Properties requires an unreviewed noncomponent base");
    const auto historical=replay_mixed_selection_property_edit_preparation(source,wire);
    const auto& actual=source.entities();
    const auto& physical_base=historical.preview_snapshot().entities();
    auto carried=physical_base;
    coordinated_map_budget(actual);
    // Check complete independently replayed allocations BEFORE eliding a copy.
    // An elided destination cannot hide an allocation/alias collision.
    validate_phase_assembly_instance_property_edit_fresh_identities(carried,replayed.result().fresh_identity_ids);
    validate_mixed_edit_fresh_destinations(actual,{carried,replayed.preview_entities()});
    auto normalized=replayed.preview_entities();
    const auto allocated_aliases=embedded_assembly_presentation_ids(normalized);
    const auto authoring=decode_phase_assembly_instance_property_edit_authoring(replayed.request().at("authoring"));
    // Geometry and explicit complete Properties use the same independently
    // inventoried dimensions. Historical pose requests keep their meaning.
    const auto component_version=replayed.request().at("version").get<int>();
    const bool geometry_completion=component_version>=3 && component_version<=7;
    const bool reviewed_topology=component_version==7;
    std::set<ComponentTarget> reviewed_nodes;
    if (reviewed_topology) for (const auto& intent:authoring.intents)
        if (intent.review_profile_topology) reviewed_nodes.insert(intent.target);
    std::map<std::string,std::string,std::less<>> base_dimension_ids;
    if (geometry_completion && (version==21 || version==24 || version==27))
        for (const auto& allocation:wire.at("phase_request").at("allocations"))
            for (const auto& [original,proposed]:allocation.at("dimension_identities").items())
                if (!base_dimension_ids.emplace(original,proposed.get<std::string>()).second)
                    invalid("Coordinated component Properties repeats a base dimension allocation");
    // The physical lane was independently replayed and its complete fresh
    // inventory checked above. Elide only its exact allocated copies for actual
    // reviewed retirements. Typed removal retains raw surviving rows and refuses
    // opaque references; this does not retire their protected source originals.
    std::vector<std::string> retired_base_copies;
    for (const auto& [original,selector]:authoring.profile_dimension_decisions) if (!selector) {
        const auto copied=base_dimension_ids.find(original);
        if (copied==base_dimension_ids.end()) continue;
        if (actual.contains(copied->second) || !carried.contains(copied->second) ||
            !exact(actual.at(original),carried.at(original)))
            invalid("Coordinated reviewed retirement lacks an exact fresh base dimension copy");
        retired_base_copies.push_back(copied->second);
    }
    if (!retired_base_copies.empty()) {
        std::sort(retired_base_copies.begin(),retired_base_copies.end());
        // Separate actual registry cohorts retain their existing removal
        // admission. No cross-registry selection authority is inferred here.
        std::map<std::string,std::string,std::less<>> copy_registries;
        for (const auto& [key,entity]:carried) if (entity.type=="model_phases") {
            const auto phases=ModelPhases::from_json(entity.properties.at("model"));
            for (const auto& owner:phases.entity_ids())
                if (std::binary_search(retired_base_copies.begin(),retired_base_copies.end(),owner) &&
                    !copy_registries.emplace(owner,key).second)
                    invalid("Coordinated reviewed base copy belongs to overlapping registries");
        }
        std::map<std::string,std::vector<std::string>,std::less<>> retirement_cohorts;
        for (const auto& owner:retired_base_copies) {
            const auto registry=copy_registries.find(owner);
            retirement_cohorts[registry==copy_registries.end() ? std::string{} : registry->second].push_back(owner);
        }
        auto retained=carried;
        for (const auto& [registry,owners]:retirement_cohorts) {
            (void)registry;
            std::map<std::string,std::string,std::less<>> fresh_copies;
            for (const auto& [original,proposed]:base_dimension_ids)
                if (std::binary_search(owners.begin(),owners.end(),proposed))
                    fresh_copies.emplace(original,proposed);
            if (fresh_copies.size()!=owners.size())
                invalid("Coordinated reviewed elision lost its exact base allocation roster");
            retained=replay_unpublished_assembly_dimension_copy_elision(
                actual,retained,fresh_copies,true);
        }
        for (const auto& [key,entity]:carried) {
            (void)entity;
            if (!retained.contains(key) && !std::binary_search(retired_base_copies.begin(),retired_base_copies.end(),key))
                invalid("Coordinated reviewed base elision removed an unallocated owner");
        }
        for (const auto& [key,entity]:actual) {
            const auto before=carried.find(key);
            if (before==carried.end()) continue;
            const auto after=retained.find(key);
            if (after==retained.end() || ((exact(entity,before->second) ||
                can_recognize_boundary_dimension_entity_type(entity.type)) && !exact(before->second,after->second)))
                invalid("Coordinated reviewed base elision changed an unaffected actual owner");
        }
        for (const auto& [original,selector]:authoring.profile_dimension_decisions)
            if (!selector && base_dimension_ids.contains(original) && !exact(actual.at(original),retained.at(original)))
                invalid("Coordinated reviewed base elision changed a protected source dimension");
        carried=retained;
    }
    CoordinatedComponentPropertyComposition result;
    result.original_to_proposed=replayed.result().original_to_proposed;
    result.reserved_component_identity_ids=replayed.result().fresh_identity_ids;
    std::map<ComponentTarget,ComponentTarget> carried_roots;
    std::map<AssemblyPhaseRoot,AssemblyPhaseRoot> embedded_substitutions;
    std::map<std::string,std::string> independent_substitutions;
    for (const auto& [original,component_destination]:replayed.result().original_to_proposed) {
        const auto root=component_root(original);
        if (carried_roots.contains(root)) continue;
        auto base_destination=root;
        if (root.document_entity_id) {
            const auto copied=historical.original_to_proposed().find(*root.document_entity_id);
            if (copied!=historical.original_to_proposed().end() && copied->second!=*root.document_entity_id) {
                const auto owner=decode_document_assembly_instance(carried.at(copied->second));
                base_destination={owner.assembly_catalog_id,owner.instance.id,copied->second,{}};
            }
        } else {
            const auto copied=historical.hosted_binding_original_to_proposed().find({root.catalog_id,root.instance_id});
            if (copied!=historical.hosted_binding_original_to_proposed().end()) {
                base_destination.catalog_id=copied->second.first;
                base_destination.instance_id=copied->second.second;
            }
        }
        const auto& original_row=qualified_component_row(actual,root);
        const auto& base_row=qualified_component_row(carried,base_destination);
        bool coedit=base_destination!=root || !exact_json(original_row,base_row);
        if (original_row.contains("placement") && original_row.at("placement").is_object()) {
            const auto host=original_row.at("placement").at("host_entity_id").get<std::string>();
            const auto final_host=carried.find(host);
            coedit=coedit || final_host==carried.end() || !exact(actual.at(host),final_host->second);
        }
        if (!coedit) continue;
        const auto component_root_destination=component_root(component_destination);
        const auto survivor=base_destination!=root ? base_destination : component_root_destination;
        carried_roots.emplace(root,survivor);
        // Ordinary/proposed edits on a source row move to the actual carried
        // destination too. They have no allocation to substitute.
        if (component_root_destination==root)
            mutable_component_row(normalized,root)=original_row;
        else if (component_root_destination==survivor) {
            // A same-ID host edit leaves a component-only private proposal as
            // the survivor. Begin with the base's actual unpatched carried row.
            auto row=base_row; row["id"]=survivor.instance_id;
            mutable_component_row(normalized,survivor)=std::move(row);
        } else {
            const auto allocation=authoring.allocation.roots.find(root);
            if (allocation==authoring.allocation.roots.end())
                invalid("Coordinated component elision lacks its exact original allocation");
            if (component_root_destination.document_entity_id) {
                const auto& owner=*component_root_destination.document_entity_id;
                if (actual.contains(owner) || !allocation->second.proposed_entity_id ||
                    *allocation->second.proposed_entity_id!=owner || !survivor.document_entity_id)
                    invalid("Coordinated component owner elision is not a proved fresh proposal");
                independent_substitutions.emplace(owner,*survivor.document_entity_id);
                normalized.erase(owner);
            } else {
                const auto& catalog=component_root_destination.catalog_id;
                if (actual.contains(catalog) || !allocation->second.proposed_catalog_id ||
                    *allocation->second.proposed_catalog_id!=catalog ||
                    normalized.at(catalog).properties.at("model").at("instances").size()!=1)
                    invalid("Coordinated component catalog elision is not an exact private proposal");
                embedded_substitutions.emplace(AssemblyPhaseRoot{catalog,component_root_destination.instance_id},
                    AssemblyPhaseRoot{survivor.catalog_id,survivor.instance_id});
                normalized.erase(catalog);
            }
        }
    }
    if (geometry_completion) for (const auto& [original,proposed]:authoring.dimension_identities) {
        const auto base_dimension=base_dimension_ids.find(original);
        if (base_dimension==base_dimension_ids.end()) continue;
        if (!normalized.erase(proposed) || !carried.contains(base_dimension->second))
            invalid("Coordinated component dimension elision lost its exact allocated carriers");
        if (!independent_substitutions.emplace(proposed,base_dimension->second).second)
            invalid("Coordinated component dimension substitution overlaps another allocation");
    }
    substitute_component_memberships(normalized,embedded_substitutions,independent_substitutions);
    if (authoring.carry_presentation_references) {
        std::map<std::string,std::string,std::less<>> presentation_substitutions;
        for (const auto& [from,to]:independent_substitutions) presentation_substitutions.emplace(from,to);
        const auto aliases=embedded_assembly_presentation_ids(carried);
        for (const auto& [from,to]:embedded_substitutions)
            presentation_substitutions.emplace(allocated_aliases.at(from),aliases.at(to));
        substitute_component_presentation_proposals(normalized,carried,presentation_substitutions,
            authoring.presentation_overlay_identities);
    }
    std::vector<AssemblyInstancePropertyEditIntent> carried_intents;
    std::map<ComponentTarget,ComponentTarget> selected_carried;
    for (const auto& intent:authoring.intents) {
        const auto found=carried_roots.find(component_root(intent.target));
        if (found==carried_roots.end()) continue;
        auto destination=found->second; destination.part_path=intent.target.part_path;
        result.original_to_proposed.at(intent.target)=destination;
        selected_carried.emplace(intent.target,destination);
        carried_intents.push_back(intent);
    }
    std::set<ComponentTarget> geometry_move_roots;
    if (geometry_completion) {
        const auto plan=inspect_phase_assembly_instance_property_edit_plan(actual,authoring.intents);
        for (const auto& original:plan.ordinary_roots) {
            const auto destination=carried_roots.find(original);
            if (destination==carried_roots.end() || destination->second==original)
                geometry_move_roots.insert(original);
        }
        for (const auto& [original,destination]:selected_carried)
            if (component_root(original)==component_root(destination)) geometry_move_roots.insert(component_root(original));
        // Both contributions were independently sealed against the original.
        // Keep the physical base carrier until the one final-frame calculation.
        for (const auto& [id,entity]:actual) {
            if (!can_recognize_boundary_dimension_entity_type(entity.type) ||
                !is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(entity).format)) continue;
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (decoded.dimension && decoded.dimension->assembly_target &&
                geometry_move_roots.contains(component_root(decoded.dimension->assembly_target->component))) {
                // A reviewed ordinary retirement stays absent. Neutralize only
                // the base's intermediate move so it cannot overlap that removal.
                if (normalized.contains(id)) normalized.at(id)=entity;
                else if (std::binary_search(replayed.result().retired_dimension_ids.begin(),
                    replayed.result().retired_dimension_ids.end(),id)) carried.insert_or_assign(id,entity);
                else invalid("Coordinated geometry lost an unretired ordinary dimension");
            }
        }
    }
    PhasePropertyCompositionRetirements retirements;
    for (const auto& [id,entity]:actual) { (void)entity; if (!carried.contains(id)) retirements.erased.insert(id); }
    retirements.erased.insert(replayed.result().retired_dimension_ids.begin(),replayed.result().retired_dimension_ids.end());
    const auto carried_aliases=embedded_assembly_presentation_ids(carried);
    for (const auto& [binding,alias]:embedded_assembly_presentation_ids(actual)) {
        const auto retained=carried_aliases.find(binding);
        if (retained==carried_aliases.end()) retirements.bindings.insert(binding);
        else if (retained->second!=alias) invalid("Coordinated component base changed a surviving source alias");
    }
    std::set<std::string,std::less<>> promoted;
    auto rows=component_property_row_admission(actual,normalized,promoted);
    for (const auto& [root,survivor]:carried_roots) if (!root.document_entity_id && root==survivor &&
        !exact_json(qualified_component_row(actual,root),qualified_component_row(carried,root))) {
        rows.changed_rows.insert_or_assign(AssemblyPhaseRoot{root.catalog_id,root.instance_id},
            qualified_component_row(carried,root));
        rows.catalogs.insert(root.catalog_id);
    }
    result.entities=compose_mixed_selection_edit_candidates_impl(actual,{carried,normalized},
        nullptr,nullptr,nullptr,&promoted,&retirements,&rows);
    if (!carried_intents.empty()) result.entities=detail::calculate_carried_assembly_instance_property_entities(
        actual,carried_intents,result.entities,selected_carried);
    if (version==27 || geometry_completion) {
        // A component SET can change the frame after the physical base copied
        // dimensions. Reconstruct the actual base inventory and carry from the
        // original source to the complete final component graph only once.
        std::optional<PhaseSelectionPropertyEditPreparation> phase;
        if (version==21 || version==24 || version==27)
            phase=replay_phase_selection_property_edit_preparation(source,wire.at("phase_request"));
        std::vector<std::pair<std::string,std::string>> roots;
        std::map<ComponentTarget,ComponentTarget> clone_destinations;
        std::map<ComponentTarget,std::size_t> requests_by_root;
        std::vector<AssemblyDimensionTransformRequest> requests;
        std::map<std::string,std::string,std::less<>> dimension_ids;
        std::map<std::string,AssemblyDimensionTarget,std::less<>> profile_review_roster;
        if (reviewed_topology) profile_review_roster=
            inspect_phase_assembly_instance_profile_dimension_review_roster(actual,authoring.intents);
        for (std::size_t index=0;phase && index<phase->inspection().replacements().size();++index) {
            const auto& inventory=phase->inspection().replacements().at(index);
            const auto& allocation=phase->validated_identity_allocations().at(index);
            const char* permission=inventory.lane==PhaseSelectionPropertyLane::walls ? "complete_wall_component_dimensions" :
                inventory.lane==PhaseSelectionPropertyLane::stairs ? "complete_stair_component_dimensions" :
                inventory.lane==PhaseSelectionPropertyLane::roofs ? "complete_roof_component_dimensions" :
                inventory.lane==PhaseSelectionPropertyLane::slabs ? "complete_slab_component_dimensions" :
                "complete_structural_component_dimensions";
            if (!phase->request().contains(permission) || !phase->request().at(permission).get<bool>()) continue;
            std::visit([&](const auto& plan) {
                for (const auto& root:plan.required_hosted_instance_ids) {
                    const ComponentTarget original{root.first,root.second,std::nullopt,{}};
                    const auto destination=historical.hosted_binding_original_to_proposed().find(root);
                    if (destination==historical.hosted_binding_original_to_proposed().end() ||
                        destination->second!=std::pair{allocation.identities.at(root.first),allocation.hosted_instance_identities.at(root)})
                        invalid("Coordinated component dimension destination differs from its actual base inventory");
                    if (!clone_destinations.emplace(original,ComponentTarget{
                            destination->second.first,destination->second.second,std::nullopt,{}}).second)
                        invalid("Coordinated component Properties repeats a copied source root");
                    roots.push_back(root);
                }
            },inventory.plan);
            for (const auto& [original,proposed]:allocation.dimension_identities)
                if (!dimension_ids.emplace(original,proposed).second)
                    invalid("Coordinated component Properties repeats a copied source dimension");
        }
        auto roster=inspect_embedded_assembly_dimension_clone_roster(actual,roots);
        if (roster.size()!=dimension_ids.size())
            invalid("Coordinated component Properties dimension map differs from its actual source roster");
        for (const auto& [original,selector]:authoring.profile_dimension_decisions)
            if (!selector) dimension_ids.erase(original);
        if (geometry_completion) {
            const auto component_roster=inspect_phase_assembly_instance_property_dimension_clone_roster(
                actual,authoring.intents,authoring.profile_dimension_decisions);
            if (component_roster.size()!=authoring.dimension_identities.size())
                invalid("Coordinated geometry dimension allocations differ from their actual source roster");
            for (const auto& [original,target]:component_roster) {
                const auto root=component_root(target);
                const auto selected=std::find_if(result.original_to_proposed.begin(),result.original_to_proposed.end(),
                    [&](const auto& pair) { return component_root(pair.first)==root; });
                if (selected==result.original_to_proposed.end())
                    invalid("Coordinated geometry dimension has no actual selected source root");
                const auto destination=component_root(selected->second);
                const auto [existing,inserted]=clone_destinations.emplace(root,destination);
                if (!inserted && existing->second!=destination)
                    invalid("Coordinated geometry dimension copies have different final survivors");
                if (roster.contains(original)) {
                    if (component_root(roster.at(original))!=root)
                        invalid("Coordinated geometry dimension copies disagree about their source root");
                    continue;
                }
                roster.emplace(original,target);
                if (!dimension_ids.emplace(original,authoring.dimension_identities.at(original)).second)
                    invalid("Coordinated geometry dimension allocation overlaps another source");
            }
            // Clone inspection deliberately excludes reviewed retirements from
            // allocation. They still belong to the complete carry roster, with
            // their actual selected node and clone/move destination authority.
            if (reviewed_topology) for (const auto& [original,target]:profile_review_roster) {
                const auto root=component_root(target.component);
                const auto selected=std::find_if(result.original_to_proposed.begin(),result.original_to_proposed.end(),
                    [&](const auto& pair) { return component_root(pair.first)==root; });
                if (selected==result.original_to_proposed.end())
                    invalid("Coordinated reviewed dimension has no actual selected source root");
                const auto destination=component_root(selected->second);
                if (destination==root) continue;
                const auto [existing,inserted]=clone_destinations.emplace(root,destination);
                if (!inserted && existing->second!=destination)
                    invalid("Coordinated reviewed dimension copies have different final survivors");
                const auto [entry,added]=roster.emplace(original,target.component);
                if (!added && entry->second!=target.component)
                    invalid("Coordinated reviewed dimension differs from its actual source association");
            }
        }
        auto framed=result.entities;
        for (const auto& [original,target]:roster) {
            const bool retired=authoring.profile_dimension_decisions.contains(original) &&
                !authoring.profile_dimension_decisions.at(original);
            const auto retained=result.entities.find(original);
            if ((!retired && !dimension_ids.contains(original)) || retained==result.entities.end() ||
                !exact(actual.at(original),retained->second))
                invalid("Coordinated component Properties must preserve each original copied dimension");
            if (!retired) {
                const auto& proposed=dimension_ids.at(original);
                if (!framed.erase(proposed)) invalid("Coordinated component Properties lost its actual copied dimension");
            }
            const auto root=component_root(target);
            const auto [request,inserted]=requests_by_root.emplace(root,requests.size());
            if (inserted) requests.push_back({root,clone_destinations.at(root),AssemblyDimensionTransformMode::clone,{}});
            if (!retired) requests.at(request->second).fresh_dimension_ids.emplace(original,dimension_ids.at(original));
        }
        std::set<ComponentTarget> move_roots;
        if (geometry_completion) {
            for (const auto& root:geometry_move_roots)
                if (!requests_by_root.contains(root)) move_roots.insert(root);
        } else for (const auto& [original,destination]:selected_carried) {
                const auto root=component_root(original);
                if (root!=component_root(destination) || requests_by_root.contains(root)) continue;
                move_roots.insert(root);
            }
        // Existing ordinary dimensions may already have followed the physical
        // base. Release only carriers on the actual selected move roots, retaining
        // their independent base proof, for one source-to-final frame calculation.
        for (const auto& [key,entity]:actual) {
            if (!can_recognize_boundary_dimension_entity_type(entity.type) ||
                !is_assembly_boundary_dimension_format(inspect_boundary_dimension_version(entity).format)) continue;
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (!decoded.dimension || !decoded.dimension->assembly_target) continue;
            const auto root=component_root(decoded.dimension->assembly_target->component);
            if (!move_roots.contains(root)) continue;
            if (requests_by_root.emplace(root,requests.size()).second)
                requests.push_back({root,root,AssemblyDimensionTransformMode::move,{}});
            const bool retired=authoring.profile_dimension_decisions.contains(key) &&
                !authoring.profile_dimension_decisions.at(key);
            const auto retained=result.entities.find(key);
            if (retired ? retained!=result.entities.end() :
                retained==result.entities.end() || !exact(physical_base.at(key),retained->second))
                invalid("Coordinated component Properties has an unproved ordinary dimension contribution");
            framed.insert_or_assign(key,entity);
            // Move carry emits only changes from source. A final frame equal
            // to source must also discard the base's intermediate label move.
            if (!retired) result.entities.at(key)=entity;
        }
        AssemblyDimensionReviewedCarryResult dimensions;
        if (!requests.empty()) {
            if (reviewed_topology) {
                std::vector<AssemblyDimensionReviewedTransformRequest> reviewed_requests;
                for (const auto& request:requests) {
                    AssemblyDimensionReviewedTransformRequest reviewed;
                    reviewed.transform=request;
                    reviewed_requests.push_back(std::move(reviewed));
                }
                bool has_reviewed_request=false;
                for (const auto& node:reviewed_nodes) {
                    const auto request=requests_by_root.find(component_root(node));
                    if (request==requests_by_root.end()) continue;
                    reviewed_requests.at(request->second).reviewed_profile_nodes.push_back(node);
                    has_reviewed_request=true;
                }
                for (const auto& [key,decision]:authoring.profile_dimension_decisions) {
                    const auto association=profile_review_roster.find(key);
                    if (association==profile_review_roster.end())
                        invalid("Coordinated reviewed decision lost its actual source association");
                    const auto request=requests_by_root.find(component_root(association->second.component));
                    if (request==requests_by_root.end())
                        invalid("Coordinated reviewed decision lost its final carry request");
                    reviewed_requests.at(request->second).profile_dimension_decisions.emplace(key,decision);
                }
                if (has_reviewed_request)
                    dimensions=carry_reviewed_assembly_dimension_transform_entities(actual,framed,reviewed_requests);
                else dimensions.upserts=carry_assembly_dimension_transform_entities(actual,framed,requests);
            } else dimensions.upserts=carry_assembly_dimension_transform_entities(actual,framed,requests);
        }
        if (dimensions.retired_source_dimension_ids!=replayed.result().retired_dimension_ids)
            invalid("Coordinated reviewed carry changed the exact ordinary retirement roster");
        std::vector<std::string> changed;
        for (const auto& dimension:dimensions.upserts)
            if (actual.contains(dimension.id)) changed.push_back(dimension.id);
        changed.insert(changed.end(),dimensions.retired_source_dimension_ids.begin(),dimensions.retired_source_dimension_ids.end());
        std::sort(changed.begin(),changed.end());
        validate_ordinary_assembly_dimension_changes(actual,changed);
        for (const auto& dimension:dimensions.upserts) result.entities.at(dimension.id)=dimension;
        for (const auto& original:dimensions.retired_source_dimension_ids)
            if (result.entities.contains(original))
                invalid("Coordinated reviewed carry restored a retired ordinary dimension");
        for (const auto& [original,target]:roster) {
            (void)target;
            if (!exact(actual.at(original),result.entities.at(original)))
                invalid("Coordinated component Properties changed a protected copied source dimension");
        }
    }
    std::vector<AssemblyInstancePropertyEditIntent> final_targets;
    for (const auto& [original,destination]:result.original_to_proposed) {
        (void)original;
        AssemblyInstancePropertyEditIntent intent; intent.target=destination;
        final_targets.push_back(std::move(intent));
    }
    std::sort(final_targets.begin(),final_targets.end(),[](const auto& left,const auto& right) { return left.target<right.target; });
    (void)replay_phase_assembly_instance_property_edits(result.entities,final_targets,{});
    const auto final_owners=mixed_edit_identity_owners(result.entities);
    std::set<std::string,std::less<>> final_aliases;
    for (const auto& [binding,alias]:embedded_assembly_presentation_ids(result.entities)) { (void)binding; final_aliases.insert(alias); }
    std::set<std::string,std::less<>> final_instance_ids;
    for (const auto& [id,entity]:result.entities) {
        (void)id;
        if (entity.type=="assembly_model") for (const auto& row:entity.properties.at("model").at("instances"))
            final_instance_ids.insert(row.at("id").get<std::string>());
        else if (entity.type=="assembly_instance") final_instance_ids.insert(entity.properties.at("instance").at("id").get<std::string>());
    }
    for (const auto& id:result.reserved_component_identity_ids)
        if (!final_owners.contains(id) && !final_aliases.contains(id) && !final_instance_ids.contains(id))
            result.elided_component_identity_ids.push_back(id);
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
            if ((before=="sketch.assemblies.v1" || before=="sketch.assemblies.v2") &&
                (after=="sketch.assemblies.v7" || after=="sketch.assemblies.v8" || after=="sketch.assemblies.v9"))
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
        const bool qualified_scalar_wall=(version==28 || version==36) &&
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
        } else if (version==24 || (version==40 && wire.at("structural_edit_version")==24)) {
            const auto leaf=decode_phase_structural_replacement_authoring(intent.structural_replacement);
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            replay=replay_phase_constraint_authoring(actual,wire);
        } else if (version==5 || version==27 || version==39) {
            const auto leaf=decode_phase_slab_replacement_authoring(intent.slab_replacement);
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            replay=replay_phase_constraint_authoring(actual,wire);
        } else if (version==4 || (version==38 && wire.at("roof_edit_version")==4)) {
            const auto leaf=decode_phase_roof_replacement_authoring(intent.roof_replacement);
            if (leaf.demolition) invalid("Phase Properties cannot borrow roof demolition authority");
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            replay=replay_phase_constraint_authoring(actual,wire);
        } else if (version==25 || version==33) {
            const auto leaf=decode_phase_stair_replacement_authoring(intent.stair_replacement);
            if (!leaf.complete_scalar_dependencies)
                invalid("Phase Properties requires complete scalar stair dependency authority");
            registry=leaf.registry_id;alternative=leaf.alternative_id;
            for (const auto& decision:leaf.dependency_dispositions)
                if (decision.action==PhaseStairReplacementDependencyAction::retire && !retired_rails.insert(decision.rail_id).second)
                    invalid("Phase Properties repeats an explicit rail retirement");
            replay=replay_phase_constraint_authoring(actual,wire);
        } else invalid("Phase Properties contribution has unsupported replacement authority");
        // Historical scalar and vertical scalar dialects are the same family.
        // Different wire versions cannot split overlapping wall authority.
        const auto family=version==26 || qualified_scalar_wall ? 23 :
            version==27 || version==39 ? 5 : version==33 ? 25 :
            version==40 ? 24 : version==38 ? 4 : version;
        if (!groups.emplace(family,registry,alternative).second)
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
void admit_removal_component_catalogs(const Entities& entities,AssemblyCatalogTransferBudget& budget) {
    // Match the membership transport's complete-map inventory bound before
    // any projected map copy or semantic registry/catalog decode.
    if (entities.size()>100000) invalid("Component phase removal composition entity budget exceeded");
    for (const auto& [key,entity]:entities) {
        (void)key;
        if (entity.type=="assembly_model") admit_existing_assembly_catalog_work(entity,budget);
    }
}

void prove_removal_component_memberships(const Entities& source,const Entities& candidate) {
    for (const auto& [key,entity]:candidate) if (entity.type==assembly_phase_membership_entity_type) {
        const auto original=source.find(key);
        if (original==source.end() || original->second.type!=assembly_phase_membership_entity_type)
            invalid("Component phase removal composition cannot enroll a fresh companion");
    }
    auto projected_size=candidate.size();
    for (const auto& [key,entity]:source)
        if (entity.type==assembly_phase_membership_entity_type && !candidate.contains(key) && ++projected_size>100000)
            invalid("Component phase removal projection entity budget exceeded");
    auto projection=candidate;
    // Restore every actual source companion, even when the leaf retired its
    // registry. Only the typed pruner can prove that carrier's omission.
    for (const auto& [key,entity]:source)
        if (entity.type==assembly_phase_membership_entity_type) projection.insert_or_assign(key,entity);
    const auto pruned=prune_removed_assembly_phase_memberships(source,projection);
    if (pruned.size()!=candidate.size()) invalid("Component phase removal leaf differs from exact source pruning");
    for (const auto& [key,entity]:pruned) {
        const auto supplied=candidate.find(key);
        if (supplied==candidate.end() || !exact(entity,supplied->second))
            invalid("Component phase removal leaf differs from exact source pruning");
    }
}

Entities compose_removal_candidates(const Entities& source,const std::vector<Entities>& candidates,
    bool include_ordinary_removal, bool complete_roof_removal, bool preserve_all_baselines,
    bool allow_shared_reference_retirement=false,bool complete_hosted_catalog_consequences=false,
    bool complete_component_phase_memberships=false) {
    if (complete_component_phase_memberships && !complete_hosted_catalog_consequences)
        invalid("Component phase removal composition requires complete hosted catalog consequences");
    coordinated_map_budget(source);
    if (candidates.size()<2 || candidates.size()>(include_ordinary_removal ?
            (complete_hosted_catalog_consequences ? 7u : 6u) : 5u))
        invalid("Coordinated demolition exceeds its complete family candidate bounds");
    for (const auto& candidate:candidates) coordinated_map_budget(candidate);
    AssemblyCatalogTransferBudget component_catalog_budget;
    if (complete_component_phase_memberships) {
        // One cumulative ledger reserves ALL catalogs, including unreferenced
        // ones, in the source and every leaf before the first semantic decode.
        admit_removal_component_catalogs(source,component_catalog_budget);
        for (const auto& candidate:candidates) admit_removal_component_catalogs(candidate,component_catalog_budget);
        for (const auto& candidate:candidates) prove_removal_component_memberships(source,candidate);
    }
    std::set<std::string,std::less<>> baseline;
    for (const auto& [key,entity]:source) if (entity.type=="model_phases") {
        (void)key;
        const auto model=ModelPhases::from_json(entity.properties.at("model"));
        if (preserve_all_baselines || model.active_alternative() || !model.alternatives().empty())
            baseline.insert(model.baseline_ids().begin(),model.baseline_ids().end());
    }
    auto result=source;
    for (const auto& [key,entity]:source) {
        // Each complete leaf's carrier changes/omissions already have exact
        // typed proof. Retain source carriers for one prune of the actual union.
        if (complete_component_phase_memberships && entity.type==assembly_phase_membership_entity_type) continue;
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
                complete_hosted_catalog_consequences,complete_component_phase_memberships);
        else if (changed.size()==1) result.at(key)=*changed.front();
        else if (changed.size()>1) result.at(key)=entity.type=="assembly_model"
            ? merge_demolition_catalog(entity,changed,allow_shared_reference_retirement) :
                merge_demolition_row_container(entity,changed,complete_roof_removal,allow_shared_reference_retirement,
                    false,complete_component_phase_memberships);
    }
    for (const auto& candidate:candidates) for (const auto& [key,entity]:candidate) {
        if (source.contains(key)) continue;
        if (!result.emplace(key,entity).second)
            invalid("Coordinated demolition family destinations overlap");
    }
    coordinated_map_budget(result);
    if (complete_component_phase_memberships) {
        admit_removal_component_catalogs(result,component_catalog_budget);
        result=prune_removed_assembly_phase_memberships(source,result);
    }
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
    bool allow_shared_reference_retirement,bool complete_hosted_catalog_consequences,
    bool complete_component_phase_memberships) {
    return compose_removal_candidates(source,candidates,true,true,false,allow_shared_reference_retirement,
        complete_hosted_catalog_consequences,complete_component_phase_memberships);
}
} // namespace sketch
