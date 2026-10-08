#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

struct RoofPoseEditIntent {
    std::string roof_id;
    // Absolute authored coordinates in base_position_m, before level placement.
    // Signed and zero quantities retain the exact entered length expressions.
    std::optional<Quantity> x;
    std::optional<Quantity> y;
    std::optional<Quantity> z;
    // Angles have their own finite scalar authority, never a length receipt.
    std::optional<double> orientation_radians;
};

// Strict version 1: exactly version, roof_id, x, y, z, orientation_radians.
// Retained fields are explicit null; an entirely unauthored intent is refused.
[[nodiscard]] nlohmann::json encode_roof_pose_edit_intent(const RoofPoseEditIntent& intent);
[[nodiscard]] RoofPoseEditIntent decode_roof_pose_edit_intent(const nlohmann::json& value);

// Actual native roof/source receipt admission. This does not establish document
// ownership, phase, level placement, or transaction authority.
void validate_roof_pose_source_entity(const Entity& source);

// Source-admitted typed staging for an atomic composite roof edit. Admission
// of the actual original and typed input is mandatory; final result geometry is
// deferred. The caller must independently combine typed deltas and admit the
// complete result before publication. This supplies no candidate/transaction
// authority and must never validate a fabricated intermediate as its source.
[[nodiscard]] Entity stage_roof_pose_entity(const Entity& actual_source, const RoofPoseEditIntent& intent);

// Change only supplied unequal pose scalars and their understood coordinate
// receipt cores. Preserve every other owner field and opaque receipt sibling.
// Affected opaque receipts refuse; equal input returns the exact source.
[[nodiscard]] Entity replay_roof_pose_entity(const Entity& source, const RoofPoseEditIntent& intent);

// Replay against the complete actual source map, with unique active targets,
// actual level/context resolution, and source/final affected fused joins.
// Join records/membership remain exact; edits breaking connectivity refuse.
// No baseline replacement, phase remapping, or document transaction is implied.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_pose_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofPoseEditIntent>& intents);

// Narrow input inference for composite capture: admit actual source/final
// candidate and require exact changed coordinate inputs. Companion changes are
// not authorized; a complete independent typed replay must account for them.
[[nodiscard]] std::optional<RoofPoseEditIntent> infer_roof_pose_edit(
    const Entity& original, const Entity& candidate);

// Extend shared roof equivalence normalization with equal coordinate receipts.
// Only understood, source-derived cores may be normalized; opaque siblings and
// actual changes remain exact for the caller's complete replay comparison.
[[nodiscard]] Entity normalize_equivalent_roof_pose_inputs(
    const Entity& original, const Entity& candidate);

// Changed coordinates require exact entered candidate receipts. Capture admits
// both actual entities and compares the full candidate to independent replay,
// including property/extension dumps. Equivalent known source-derived re-entry
// retains original representations and receipts without hiding companion edits.
// Unsupported changes throw; nullopt denotes an exact/equivalent source no-op.
[[nodiscard]] std::optional<RoofPoseEditIntent> capture_roof_pose_edit(
    const Entity& original, const Entity& candidate);

} // namespace sketch
