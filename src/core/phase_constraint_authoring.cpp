#include "sketch/phase_constraint_authoring.hpp"

#include "sketch/boundary_transform.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/joint_translation_replay.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
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
    std::optional<PhaseRoofReplacementAuthoring> roof;
    std::optional<PhaseSlabReplacementAuthoring> slab;
    std::vector<RoofEditIntent> ordinary_roofs;
    std::vector<SlabGeometryEditIntent> ordinary_slabs;
};
CoordinatedReplacements coordinated(const Json& value) {
    resource_shape(value);
    if (value.dump().size()>proof_budget) invalid("Coordinated replacement proof byte budget exceeded");
    keys(value,{"version","roof_replacement","slab_replacement","ordinary_roof_edits","ordinary_slab_geometry"});
    if (!value.at("version").is_number_integer() || value.at("version")!=1)
        invalid("Coordinated replacements require version one");
    bounded_array(value.at("ordinary_roof_edits"));
    bounded_array(value.at("ordinary_slab_geometry"));
    CoordinatedReplacements result;
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
    if (static_cast<int>(result.roof.has_value())+static_cast<int>(!result.ordinary_roofs.empty())!=1 ||
        static_cast<int>(result.slab.has_value())+static_cast<int>(!result.ordinary_slabs.empty())!=1 ||
        (!result.roof && !result.slab))
        invalid("Coordinated replacements require exactly one lane per family and at least one replacement");
    if (result.roof && result.slab && (result.roof->registry_id!=result.slab->registry_id ||
        result.roof->alternative_id!=result.slab->alternative_id))
        invalid("Coordinated replacement leaves must name the same actual registry and alternative");
    return result;
}

bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
bool exact_json(const Json& a,const Json& b) { return a==b && a.dump()==b.dump(); }

// Explicit paths only. Removing these arrays from each independently replayed
// envelope must leave the exact source envelope. This is not JSON merge authority.
struct AppendPath { Json::json_pointer path; const char* identity_key; };
std::vector<AppendPath> append_paths(const Entity& source) {
    std::vector<AppendPath> result;
    if (source.type=="model_phases") {
        const auto model=ModelPhases::from_json(source.properties.at("model"));
        result.push_back({Json::json_pointer("/model/entity_ids"),nullptr});
        if (!model.active_alternative()) invalid("Shared phase append requires an actual saved active alternative");
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

Entities replay_coordinated(const Entities& source,const Json& value) {
    const auto lanes=coordinated(value);
    coordinated_map_budget(source);
    auto roof_candidate=source, slab_candidate=source;
    std::vector<std::string> roof_fresh, slab_fresh;
    std::set<std::string,std::less<>> retained_baselines;
    if (lanes.roof) {
        const auto& leaf=*lanes.roof;
        const auto plan=inspect_phase_roof_replacement_plan(source,leaf.seed_roof_ids,leaf.registry_id,
            leaf.alternative_id,leaf.phase_qualified_joins);
        auto replay=replay_phase_roof_replacement(source,plan,leaf.identities,leaf.roof_profiles,
            leaf.roof_opening_edits,leaf.roof_edits,leaf.ordinary_roof_edits,leaf.phase_qualified_joins);
        roof_candidate=std::move(replay.entities); roof_fresh=std::move(replay.fresh_identity_ids);
        retained_baselines.insert(plan.required_entity_ids.begin(),plan.required_entity_ids.end());
    } else {
        const auto partition=partition_phase_roof_geometry_edits(source,lanes.ordinary_roofs);
        if (partition.replacement || !partition.baseline_roof_edits.empty())
            invalid("Coordinated ordinary roof lane requires actual ordinary/proposed membership");
        roof_candidate=replay_roof_edit_entities(source,lanes.ordinary_roofs);
        roof_fresh=new_roof_opening_identity_ids(source,roof_edit_opening_intents(lanes.ordinary_roofs));
    }
    if (lanes.slab) {
        const auto& leaf=*lanes.slab;
        const auto plan=inspect_phase_slab_replacement_plan(source,leaf.seed_slab_ids,leaf.registry_id,leaf.alternative_id);
        auto replay=replay_phase_slab_replacement(source,plan,leaf.identities,leaf.slab_profiles,
            leaf.slab_stacks,leaf.slab_geometry,leaf.ordinary_geometry,leaf.hosted_instance_identities,
            leaf.coordinate_ordinary_hosted_geometry);
        slab_candidate=std::move(replay.entities); slab_fresh=std::move(replay.fresh_identity_ids);
        retained_baselines.insert(plan.seed_slab_ids.begin(),plan.seed_slab_ids.end());
    } else {
        const auto partition=partition_phase_slab_geometry_edits(source,lanes.ordinary_slabs);
        if (partition.replacement || !partition.baseline_geometry.empty())
            invalid("Coordinated ordinary slab lane requires actual ordinary/proposed membership");
        slab_candidate=replay_slab_geometry_with_hosted_entities(source,lanes.ordinary_slabs);
    }
    std::set<std::string,std::less<>> fresh;
    for (const auto* destinations:{&roof_fresh,&slab_fresh}) for (const auto& destination:*destinations) {
        // Leaf admission owns entity/child syntax. Its derived presentation
        // aliases may combine two valid IDs and exceed an entity ID's length.
        if (destination.empty()) invalid("Coordinated replacement has an empty derived identity destination");
        if (!fresh.insert(destination).second) invalid("Coordinated replacement fresh identity destinations overlap");
        if (fresh.size()>4096) invalid("Coordinated replacement identity budget exceeded");
    }
    auto result=source;
    for (const auto& [key,original]:source) {
        if (!roof_candidate.contains(key) || !slab_candidate.contains(key))
            invalid("Coordinated replacement cannot remove an actual source owner");
        const auto& roof=roof_candidate.at(key); const auto& slab=slab_candidate.at(key);
        const bool roof_changed=!exact(original,roof), slab_changed=!exact(original,slab);
        if (roof_changed && slab_changed) result.at(key)=merge_append_container(original,roof,slab);
        else if (roof_changed) result.at(key)=roof;
        else if (slab_changed) result.at(key)=slab;
    }
    for (const auto* candidate:{&roof_candidate,&slab_candidate})
        for (const auto& [key,entity]:*candidate) if (!source.contains(key)) {
            if (!fresh.contains(key) || !result.emplace(key,entity).second)
                invalid("Coordinated replacement new entity destination is unreserved or overlapping");
        }
    for (const auto& key:retained_baselines) if (!exact(source.at(key),result.at(key)))
        invalid("Coordinated replacement changed a retained baseline envelope");
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
        static_cast<int>(!value.coordinated_replacements.is_null());
    if (exclusive_operations > 1)
        invalid("Active design operations cannot borrow another replacement or demolition authority");
    Json result={{"version",!value.coordinated_replacements.is_null()?7:!value.slab_demolition.is_null()?6:!value.slab_replacement.is_null()?5:!value.roof_replacement.is_null()?4:!value.opening_demolition.is_null()?3:value.wall_replacement.is_null()?1:2},{"expected_revision",value.expected_revision},
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
        (void)coordinated(value.coordinated_replacements);
        ConstraintAuthoringIntent empty;
        empty.message=value.intent.message;
        if (encode_intent(value.intent)!=encode_intent(empty))
            invalid("Coordinated replacements cannot borrow other geometry or relationship authority");
        result["coordinated_replacements"]=value.coordinated_replacements;
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
    const bool coordinated_replacements=value.is_object() && value.contains("version") && value.at("version")==7;
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
    else keys(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
        "source_entities_digest","source_saved_revision","phase_selections","intent"});
    if (!value.at("version").is_number_integer() || (value.at("version")!=1 && value.at("version")!=2 && value.at("version")!=3 && value.at("version")!=4 && value.at("version")!=5 && value.at("version")!=6 && value.at("version")!=7) ||
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
        if (result.coordinated_replacements.is_null()) invalid("Version seven requires coordinated replacement decisions");
        (void)coordinated(result.coordinated_replacements);
    }
    if (encode_phase_constraint_authoring_intent(result)!=value) invalid("Phase constraint proof is not canonical");
    return result;
}

std::vector<PhaseConstraintAuthoringIntent> phase_constraint_replacement_components(
    const PhaseConstraintAuthoringIntent& intent) {
    (void)encode_phase_constraint_authoring_intent(intent);
    if (intent.coordinated_replacements.is_null()) return {intent};
    std::vector<PhaseConstraintAuthoringIntent> result;
    for (const auto* family:{"roof_replacement","slab_replacement"}) {
        const auto& leaf=intent.coordinated_replacements.at(family);
        if (leaf.is_null()) continue;
        auto component=intent;
        component.coordinated_replacements=nullptr;
        if (std::string_view(family)=="roof_replacement") component.roof_replacement=leaf;
        else component.slab_replacement=leaf;
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
    if (!decoded.coordinated_replacements.is_null())
        return replay_coordinated(source,decoded.coordinated_replacements);
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
    return decoded.wall_replacement.is_null() ? reconstruct_active_phase_constraint_authoring(source,decoded.intent)
        : replay_phase_wall_replacement_authoring(source,decoded);
}
} // namespace sketch
