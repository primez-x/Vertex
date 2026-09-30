#pragma once
#include "sketch/document.hpp"

namespace sketch {
// Reconstructs the baseline and its exact entry/derivation receipt. All
// unrelated wall and receipt metadata remains owned by the original entity.
[[nodiscard]] Entity replay_constraint_wall_edit(
    const Entity& source, const ConstraintWallGeometryEdit& edit);
[[nodiscard]] nlohmann::json encode_constraint_wall_edit(const ConstraintWallGeometryEdit& edit);
[[nodiscard]] ConstraintWallGeometryEdit decode_constraint_wall_edit(const nlohmann::json& value);
void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline);
// Checks active curve construction and any archived fixed-sweep derivation.
void validate_wall_curve_input(const Entity& wall);
// Preserve exact source input during endpoint deformation; rigid transforms
// preserve the active construction and append to an existing derivation.
void rebase_wall_curve_input(Entity& wall, const Segment& transformed_baseline);
// Explicit curve construction after an endpoint derivation keeps its full
// archived source and records the independently validated new construction.
void preserve_wall_curve_construction(Entity& candidate,const Entity& source);
// Endpoint-only deformations require typed replay. Full independently
// validated constructions may append without changing prior provenance.
void validate_constraint_wall_geometry_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    bool qualified_curve_edits = false);
void validate_constraint_wall_host(const std::string& wall_id,
    const std::map<std::string, Entity, std::less<>>& entities);
}
