#include "sketch/phase_constraint_authoring.hpp"

#include "sketch/boundary_transform.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/joint_translation_replay.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string,Entity,std::less<>>;
constexpr std::size_t proof_budget = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void resource_shape(const Json& root) {
    std::set<std::string,std::less<>> owners;
    const auto walk=[&](const auto& self,const Json& value,std::size_t depth) -> void {
        if (depth>64) invalid("Phase constraint proof nesting budget exceeded");
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
    if (!value.wall_replacement.is_null() && !value.opening_demolition.is_null())
        invalid("Opening demolition cannot borrow wall replacement authority");
    Json result={{"version",!value.opening_demolition.is_null()?3:value.wall_replacement.is_null()?1:2},{"expected_revision",value.expected_revision},
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
    if (replacement) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","wall_replacement"});
    else if (demolition) keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent","opening_demolition"});
    else keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent"});
    if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3) ||
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
    if (encode_phase_constraint_authoring_intent(result)!=value) invalid("Phase constraint proof is not canonical");
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
    if (!decoded.opening_demolition.is_null())
        return replay_phase_opening_demolition_entities(source,
            decode_phase_opening_demolition_intent(decoded.opening_demolition));
    return decoded.wall_replacement.is_null() ? reconstruct_active_phase_constraint_authoring(source,decoded.intent)
        : replay_phase_wall_replacement_authoring(source,decoded);
}
} // namespace sketch
