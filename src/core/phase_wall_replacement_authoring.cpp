#include "sketch/phase_wall_replacement_authoring.hpp"

#include <set>
#include <stdexcept>

namespace sketch {
namespace {

using IdentityMap = std::map<std::string, std::string, std::less<>>;

void remap_id(std::string& id, const IdentityMap& mapping) {
    if (const auto found = mapping.find(id); found != mapping.end())
        id = found->second;
}

void remap_ids(std::vector<std::string>& ids, const IdentityMap& mapping) {
    for (auto& id : ids) remap_id(id, mapping);
}

void require_unique(const std::vector<std::string>& ids) {
    std::set<std::string, std::less<>> seen;
    for (const auto& id : ids)
        if (!seen.insert(id).second)
            throw std::invalid_argument("Wall replacement remapping collides at a semantic target");
}

template<class Targets, class Key>
void remap_targets(Targets& targets, const IdentityMap& mapping, Key key) {
    std::set<std::string, std::less<>> seen;
    for (auto& target : targets) {
        auto& id = target.*key;
        remap_id(id, mapping);
        if (!seen.insert(id).second)
            throw std::invalid_argument("Wall replacement remapping collides at an operator target");
    }
}

void remap_binding(WallEndpointBinding& binding, const IdentityMap& mapping) {
    remap_id(binding.owner_id, mapping);
    remap_id(binding.segment_id, mapping);
    remap_id(binding.vertex_id, mapping);
}

void remap_edit(BoundaryGeometryEdit& edit, const IdentityMap& mapping) {
    remap_id(edit.boundary_id, mapping);
    remap_id(edit.target_id, mapping);
    remap_id(edit.new_vertex_id, mapping);
    remap_id(edit.new_segment_id, mapping);
    remap_id(edit.new_dimension_id, mapping);
    remap_ids(edit.replacement_dimension_ids, mapping);
    remap_ids(edit.replacement_removed_reference_ids, mapping);
    remap_ids(edit.replacement_wall_source_ids, mapping);
    if (edit.arc_construction) remap_id(edit.arc_construction->segment_id, mapping);
    if (edit.physical_wall_room_repair)
        remap_id(edit.physical_wall_room_repair->selected_wall_id, mapping);
}

void remap_joint(JointTranslationIntent& joint, const IdentityMap& mapping) {
    remap_ids(joint.rigid_boundary_ids, mapping);
    remap_ids(joint.rigid_stroke_ids, mapping);
    remap_ids(joint.partial_wall_ids, mapping);
    remap_ids(joint.dimension_ids, mapping);
    // Owner selections share one geometry namespace, even across owner types.
    auto owners = joint.rigid_boundary_ids;
    owners.insert(owners.end(), joint.rigid_stroke_ids.begin(), joint.rigid_stroke_ids.end());
    owners.insert(owners.end(), joint.partial_wall_ids.begin(), joint.partial_wall_ids.end());
    require_unique(owners);
    require_unique(joint.dimension_ids);
    remap_targets(joint.owner_translations, mapping, &JointOwnerTranslationIntent::owner_id);
    remap_targets(joint.dimension_translations, mapping, &JointOwnerTranslationIntent::owner_id);
    remap_targets(joint.owner_transformations, mapping, &RigidOwnerTransformation::owner_id);
    remap_targets(joint.reference_translations, mapping, &JointReferenceTranslationIntent::reference_id);
    std::set<std::pair<std::string, std::string>> annotations;
    for (auto& annotation : joint.annotation_translations) {
        remap_id(annotation.owner_id, mapping);
        remap_id(annotation.child_id, mapping);
        if (!annotations.emplace(annotation.owner_id, annotation.child_id).second)
            throw std::invalid_argument("Wall replacement remapping collides at an annotation target");
    }
}

} // namespace

ConstraintAuthoringIntent remap_phase_wall_replacement_authoring_intent(
    const ConstraintAuthoringIntent& intent, const IdentityMap& original_to_proposed) {
    std::set<std::string, std::less<>> destinations;
    for (const auto& [original, proposed] : original_to_proposed) {
        if (original.empty() || proposed.empty() || !destinations.insert(proposed).second)
            throw std::invalid_argument("Wall replacement requires unambiguous nonempty identity mappings");
    }
    auto result = intent;
    if (result.wall_resize) remap_id(result.wall_resize->wall_id, original_to_proposed);
    if (result.wall_geometry_move)
        remap_targets(result.wall_geometry_move->targets, original_to_proposed,
                      &WallGeometryMoveTarget::wall_id);
    if (result.wall_group_scale) {
        auto& scale = *result.wall_group_scale;
        scale = decode_wall_group_scale_intent(encode_wall_group_scale_intent(scale));
        remap_ids(scale.wall_ids, original_to_proposed);
        require_unique(scale.wall_ids);
        // Fresh IDs may reverse source order. Canonical target order is part
        // of retained replay; XYZ pivot and physical factor stay world-owned.
        scale = decode_wall_group_scale_intent(encode_wall_group_scale_intent(scale));
    }
    std::set<std::string, std::less<>> relations;
    for (auto& mutation : result.relation_mutations) {
        if (mutation.kind == ConstraintRelationMutationKind::upsert) {
            if (!mutation.constraint_id.empty() && mutation.constraint_id != mutation.constraint.id)
                throw std::invalid_argument("Wall replacement relation mutation has inconsistent identity");
            remap_id(mutation.constraint.id, original_to_proposed);
            mutation.constraint_id = mutation.constraint.id;
        } else {
            remap_id(mutation.constraint_id, original_to_proposed);
            remap_id(mutation.constraint.id, original_to_proposed);
        }
        for (auto& binding : mutation.constraint.bindings)
            remap_binding(binding, original_to_proposed);
        if (!relations.insert(mutation.constraint_id).second)
            throw std::invalid_argument("Wall replacement remapping collides at a relation mutation");
    }
    if (result.relation_anchor) remap_binding(*result.relation_anchor, original_to_proposed);
    if (result.boundary_resize) remap_edit(result.boundary_resize->edit, original_to_proposed);
    if (result.boundary_vertex_move) remap_edit(result.boundary_vertex_move->edit, original_to_proposed);
    if (result.measured_stroke_resize) remap_edit(result.measured_stroke_resize->edit, original_to_proposed);
    if (result.measured_stroke_vertex_move) remap_edit(result.measured_stroke_vertex_move->edit, original_to_proposed);
    if (result.measured_stroke_transform)
        remap_targets(result.measured_stroke_transform->targets, original_to_proposed,
                      &MeasuredStrokeTransformTarget::stroke_id);
    if (result.exterior_corner_move) {
        remap_id(result.exterior_corner_move->boundary_id, original_to_proposed);
        remap_id(result.exterior_corner_move->vertex_id, original_to_proposed);
    }
    if (result.exterior_segment_resize) {
        remap_id(result.exterior_segment_resize->boundary_id, original_to_proposed);
        remap_id(result.exterior_segment_resize->segment_id, original_to_proposed);
    }
    if (result.exterior_segment_arc) {
        remap_id(result.exterior_segment_arc->boundary_id, original_to_proposed);
        remap_id(result.exterior_segment_arc->segment_id, original_to_proposed);
        remap_id(result.exterior_segment_arc->arc_construction.segment_id, original_to_proposed);
    }
    if (result.joint_translation) remap_joint(*result.joint_translation, original_to_proposed);
    if (result.wall_curve_construction)
        remap_id(result.wall_curve_construction->edit.wall_id, original_to_proposed);
    return result;
}

} // namespace sketch
