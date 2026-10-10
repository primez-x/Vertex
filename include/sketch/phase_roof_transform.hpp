#pragma once

#include "sketch/architectural_document_adapter.hpp"

#include <string_view>

namespace sketch {

struct RoofRigidTransformIntent {
    std::string roof_id;
    ArchitecturalGroupTransform transform;
};

inline constexpr std::string_view roof_rigid_transform_derivations_key =
    "roof_rigid_transform_derivations";

// Closed v1: version, roof_id, transform. Transform has exactly pivot_m,
// offset_m, rotation_z_radians, scale, flip_horizontal and flip_vertical.
// Scale must equal one. No entered final coordinate, entity or candidate wire.
[[nodiscard]] nlohmann::json encode_roof_rigid_transform_intent(const RoofRigidTransformIntent& intent);
[[nodiscard]] RoofRigidTransformIntent decode_roof_rigid_transform_intent(const nlohmann::json& value);

// Checks the owned closed derivation envelope, if present. Unknown collisions
// refuse. Historical owner/cut IDs remain historical through proposed copies;
// callers must retain these records exactly, never remap archived receipts.
// This checks retained evidence only, not native geometry or live ownership.
// Archive v1 retains its original closed frame. V2 additionally admits
// roof_schema:4 frames with optional canonical rotation_rad on each cut.
void validate_roof_rigid_transform_derivations(const Entity& source);
// Read-only reference qualification after full envelope validation. Omit only
// understood historical identifiers/frame fields and archived receipt cores.
// Unknown receipt/rational siblings remain exact, recursively; historical
// child-map keys become ordered array slots. Never replace the live archive
// with this residual. An absent archive returns an empty object.
[[nodiscard]] nlohmann::json roof_rigid_transform_opaque_remainder(const Entity& source);
void validate_roof_rigid_transform_source_entity(const Entity& source);

// Reads the actual original only. Rigid reflection follows the shared adapter's
// panel corner / gable-and-hip centered semantics. Only pose and reflected cut
// Y and schema-4 surface-angle sign change. Affected understood receipts move verbatim to the owned archive;
// all other metadata and receipt bindings remain exact. No Quantity is made
// from a computed coordinate. Opaque affected bindings refuse.
[[nodiscard]] Entity stage_roof_rigid_transform_entity(
    const Entity& actual_source, const RoofRigidTransformIntent& intent);
[[nodiscard]] Entity replay_roof_rigid_transform_entity(
    const Entity& actual_source, const RoofRigidTransformIntent& intent);

// Active unique actual-map targets, resolved native source/final roofs and
// affected fused joins. Join membership is retained; broken joins refuse.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_rigid_transform_entities(
    const std::map<std::string, Entity, std::less<>>& actual_source,
    const std::vector<RoofRigidTransformIntent>& intents);

// The producer must supply its actual mathematical operation. An arbitrary
// changed candidate cannot establish transform authority. Complete exact
// source-derived replay comparison includes opaque data and archive evidence.
// nullopt is an exact no-op; unsupported differences throw.
[[nodiscard]] std::optional<RoofRigidTransformIntent> capture_roof_rigid_transform(
    const Entity& original, const Entity& candidate,
    const ArchitecturalGroupTransform& actual_transform);

} // namespace sketch
