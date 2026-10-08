#include "sketch/phase_wall_replacement_request.hpp"

#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase wall replacement request: " + reason);
}
} // namespace

std::vector<PhaseWallReplacementRequest> phase_wall_replacement_requests(
    const Entities& entities, const ConstraintAuthoringIntent& intent) {
    const auto scope = constraint_phase_scope(entities);
    Ids owners;
    const auto actual = [&](const std::string& id) -> const Entity& {
        const auto found = entities.find(id);
        if (id.empty() || found == entities.end() || found->second.id != id)
            reject("semantic owner does not resolve: " + id);
        return found->second;
    };
    const auto owner = [&](const std::string& id, const std::string& expected = {}) {
        const auto& entity = actual(id);
        if (!expected.empty() && entity.type != expected)
            reject("semantic owner has wrong type: " + id);
        if (entity.type == "wall") {
            Wall wall; std::string error;
            if (!read_document_wall(entity, {}, wall, error)) reject(id + ": " + error);
        } else if (can_recognize_boundary_entity_type(entity.type) || entity.type == "measurement_linework") {
            (void)resolve_constraint_segment_owner(entity);
        } else reject("semantic endpoint owner has unsupported type: " + id);
        if (!scope.inactive_owner_ids.contains(id)) owners.insert(id);
    };
    const auto binding = [&](const WallEndpointBinding& value) {
        owner(value.owner_id);
        const auto& entity = actual(value.owner_id);
        if (entity.type == "wall") {
            if (!value.segment_id.empty() || !value.vertex_id.empty())
                reject("wall binding contains boundary child identities: " + value.owner_id);
        } else {
            const auto boundary = resolve_constraint_segment_owner(entity);
            const auto found = std::find_if(boundary.segments.begin(), boundary.segments.end(), [&](const auto& edge) {
                return edge.segment_id == value.segment_id &&
                    (value.role == WallEndpointRole::start ? edge.start_vertex_id : edge.end_vertex_id) == value.vertex_id;
            });
            if (found == boundary.segments.end()) reject("binding endpoint does not resolve: " + value.owner_id);
        }
        if (value.role != WallEndpointRole::start && value.role != WallEndpointRole::end)
            reject("invalid endpoint role: " + value.owner_id);
    };
    const auto edit = [&](const BoundaryGeometryEdit& value, bool stroke) {
        owner(value.boundary_id, stroke ? "measurement_linework" : "");
        if (!stroke && !can_recognize_boundary_entity_type(actual(value.boundary_id).type))
            reject("boundary edit requires a boundary: " + value.boundary_id);
        if (scope.inactive_owner_ids.contains(value.boundary_id)) return;
        for (const auto& id : value.replacement_wall_source_ids) owner(id, "wall");
        if (value.physical_wall_room_repair) owner(value.physical_wall_room_repair->selected_wall_id, "wall");
    };
    const auto exterior = [&](const std::string& id) {
        owner(id);
        const auto& entity = actual(id);
        if (!can_recognize_boundary_entity_type(entity.type)) reject("exterior owner is not a boundary: " + id);
        if (scope.inactive_owner_ids.contains(id)) return;
        for (const auto& wall : exterior_wall_measurement_source_ids(entity)) owner(wall, "wall");
    };
    if (intent.wall_resize) owner(intent.wall_resize->wall_id, "wall");
    if (intent.wall_geometry_move) for (const auto& target : intent.wall_geometry_move->targets) owner(target.wall_id, "wall");
    if (intent.wall_curve_construction) owner(intent.wall_curve_construction->edit.wall_id, "wall");
    if (intent.boundary_resize) edit(intent.boundary_resize->edit, false);
    if (intent.boundary_vertex_move) edit(intent.boundary_vertex_move->edit, false);
    if (intent.measured_stroke_resize) edit(intent.measured_stroke_resize->edit, true);
    if (intent.measured_stroke_vertex_move) edit(intent.measured_stroke_vertex_move->edit, true);
    if (intent.measured_stroke_transform) for (const auto& target : intent.measured_stroke_transform->targets) owner(target.stroke_id, "measurement_linework");
    if (intent.exterior_corner_move) exterior(intent.exterior_corner_move->boundary_id);
    if (intent.exterior_segment_resize) exterior(intent.exterior_segment_resize->boundary_id);
    if (intent.exterior_segment_arc) exterior(intent.exterior_segment_arc->boundary_id);
    if (intent.joint_translation) {
        const auto& joint = *intent.joint_translation;
        // The typed resolver validates exact target coverage without replaying geometry.
        (void)resolve_joint_translation_offsets(entities, joint);
        for (const auto& id : joint.rigid_boundary_ids) owner(id);
        for (const auto& id : joint.rigid_stroke_ids) owner(id, "measurement_linework");
        for (const auto& id : joint.partial_wall_ids) owner(id, "wall");
        for (const auto& target : joint.owner_translations) owner(target.owner_id);
        for (const auto& target : joint.owner_transformations) owner(target.owner_id);
        for (const auto& id : joint.dimension_ids) (void)actual(id);
        for (const auto& target : joint.dimension_translations) (void)actual(target.owner_id);
        for (const auto& target : joint.reference_translations) (void)actual(target.reference_id);
        for (const auto& target : joint.annotation_translations) (void)actual(target.owner_id);
    }
    if (intent.relation_anchor) binding(*intent.relation_anchor);
    for (const auto& mutation : intent.relation_mutations) {
        PersistentConstraint relation;
        if (mutation.kind == ConstraintRelationMutationKind::upsert) {
            if (!mutation.constraint_id.empty() && mutation.constraint_id != mutation.constraint.id)
                reject("relation mutation identity differs from its constraint");
            const auto decoded = decode_constraint_entity(encode_constraint_entity(mutation.constraint));
            if (!decoded.supported()) reject("relation mutation is unsupported");
            relation = *decoded.constraint;
        } else if (mutation.kind == ConstraintRelationMutationKind::remove) {
            const auto& entity = actual(mutation.constraint_id);
            if (entity.type != "constraint") reject("removed relation is not a constraint: " + entity.id);
            const auto decoded = decode_constraint_entity(entity);
            if (!decoded.supported()) reject("removed relation is unsupported: " + entity.id);
            relation = *decoded.constraint;
        } else reject("unknown relation mutation kind");
        if (constraint_participates(relation, scope)) for (const auto& endpoint : relation.bindings) binding(endpoint);
    }
    if (owners.empty()) return {};
    std::vector<PersistentConstraint> relations;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "constraint") continue;
        if (entity.id != id) reject("constraint map identity differs: " + id);
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.supported()) reject("persistent relation has uninterpretable owner scope: " + id);
        if (constraint_participates(*decoded.constraint, scope)) relations.push_back(*decoded.constraint);
    }
    bool expanded = true;
    while (expanded) {
        expanded = false;
        // A selected or hard-connected exterior owner is authored through its
        // typed physical producers, even when no wall endpoint relation exists.
        const auto current = owners;
        for (const auto& id : current) {
            const auto& entity = actual(id);
            if (!can_recognize_boundary_entity_type(entity.type) ||
                !entity.properties.contains("wall_measurement_source")) continue;
            const auto before = owners.size();
            for (const auto& wall : exterior_wall_measurement_source_ids(entity)) owner(wall, "wall");
            expanded = expanded || owners.size() != before;
        }
        for (const auto& relation : relations) {
            if (!std::any_of(relation.bindings.begin(), relation.bindings.end(), [&](const auto& endpoint) { return owners.contains(endpoint.owner_id); })) continue;
            const auto before = owners.size();
            for (const auto& endpoint : relation.bindings) binding(endpoint);
            expanded = expanded || owners.size() != before;
        }
    }
    std::vector<PhaseWallReplacementRequest> result;
    for (const auto& registry : scope.registries) {
        if (!registry.alternative_id) continue;
        const auto model = ModelPhases::from_json(actual(registry.registry_id).properties.at("model"));
        PhaseWallReplacementRequest request{registry.registry_id, *registry.alternative_id, {}};
        for (const auto& id : model.baseline_ids())
            if (owners.contains(id) && actual(id).type == "wall" && !scope.inactive_owner_ids.contains(id))
                request.seed_wall_ids.push_back(id);
        std::sort(request.seed_wall_ids.begin(), request.seed_wall_ids.end());
        if (!request.seed_wall_ids.empty()) result.push_back(std::move(request));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.registry_id < b.registry_id; });
    return result;
}

} // namespace sketch
