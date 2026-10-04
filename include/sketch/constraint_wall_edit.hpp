#pragma once
#include "sketch/document.hpp"

namespace sketch {
[[nodiscard]] Entity reconstruct_split_wall(const Entity& source, const Segment& baseline,
    double fraction, bool second_piece);
void validate_wall_split_archive(const Entity& wall);
// Reconstructs the baseline and its exact entry/derivation receipt. All
// unrelated wall and receipt metadata remains owned by the original entity.
// Version four first verifies the exact selected curve transform, then rebases
// its retained construction archive and physical receipt independently.
[[nodiscard]] Entity replay_constraint_wall_edit(
    const Entity& source, const ConstraintWallGeometryEdit& edit);
[[nodiscard]] nlohmann::json encode_constraint_wall_edit(const ConstraintWallGeometryEdit& edit);
[[nodiscard]] ConstraintWallGeometryEdit decode_constraint_wall_edit(const nlohmann::json& value);
void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline);
// Independently checks understood section v1 receipts: straight v1 and
// physical curved v2. Missing/future optional versions remain opaque on open;
// editing or rebasing unsupported metadata still refuses without loss.
void validate_wall_length_input(const Entity& wall);
// Invalidate a known measurement receipt before explicit reconstruction.
// Validate against the original baseline and refuse opaque metadata loss.
void clear_wall_length_input(Entity& wall);
// Checks active curve construction and any archived fixed-sweep derivation.
void validate_wall_curve_input(const Entity& wall);
// Preserve exact source input during fixed-signed-sweep endpoint deformation.
// Reflections use the separate rigid-transform helper below.
void rebase_wall_curve_input(Entity& wall, const Segment& transformed_baseline);
// Metadata-only rigid transform while the wall still has its original baseline.
// Archives the exact original input and independently replays rotation,
// reflection and translation; the caller then updates the baseline geometry.
void transform_wall_curve_input(Entity& wall, const PlanarTransform& transform);
// Explicit curve construction after an endpoint derivation keeps its full
// archived source and records the independently validated new construction.
void preserve_wall_curve_construction(Entity& candidate,const Entity& source);
// Qualified exterior-corner reconstruction may change circular signed sweep.
// Retains the original input and every prior operation as an exact archive.
[[nodiscard]] Entity reconstruct_exterior_corner_wall(const Entity& source, const Segment& baseline);
// Endpoint-only deformations require typed replay. Full independently
// validated constructions may append without changing prior provenance.
void validate_constraint_wall_geometry_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    bool qualified_curve_edits = false);
void validate_constraint_wall_host(const std::string& wall_id,
    const std::map<std::string, Entity, std::less<>>& entities);
}
