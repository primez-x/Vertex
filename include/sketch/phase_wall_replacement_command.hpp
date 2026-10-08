#pragma once

#include "sketch/phase_wall_replacement.hpp"
#include "sketch/phase_wall_profile_edit.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_hosted_opening_rehost.hpp"
#include "sketch/phase_hosted_opening_family_edit.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/physical_wall_phase_review.hpp"

namespace sketch {

struct PhaseConstraintAuthoringIntent;

enum class PhaseWallRoomConstraintDisposition { keep, remap, omit };
struct PhaseWallRoomConstraintEndpoint {
    std::size_t binding_index{};
    WallEndpointBinding target;
};
struct PhaseWallRoomConstraintDecision {
    std::string constraint_id; // Original, never a caller-supplied clone payload.
    PhaseWallRoomConstraintDisposition disposition{PhaseWallRoomConstraintDisposition::keep};
    std::vector<PhaseWallRoomConstraintEndpoint> endpoints;
};
struct PhaseWallReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_wall_ids;
    PhaseWallReplacementIdentityMap identities;
    nlohmann::json room_review_intent=nullptr;
    std::vector<PhaseWallRoomConstraintDecision> room_constraint_decisions;
    // Record dialect two: exact profile intent for seeded original walls.
    // Layer identities are mapped with the qualified owned-child inventory.
    std::vector<WallProfileEditIntent> wall_profiles;
    // Record dialect three: existing copied openings retain their host/family.
    // Wall-profile and geometry edits use separate reviewed operations.
    std::vector<HostedOpeningProfileEditIntent> opening_profiles;
    // Record dialect four: actual old and target hosts independently qualify
    // replacement authority. All three identities are mapped only if copied.
    // Exclusive from geometry, relationships and either profile dialect.
    std::vector<HostedOpeningRehostIntent> opening_rehosts;
    // Record dialect five: same-host conversion with explicit final family,
    // assembly and optional operation. Exclusive from all other edit authority.
    std::vector<HostedOpeningFamilyEditIntent> opening_families;
};
struct PhaseWallReplacementAuthoringPreview {
    PhaseWallReplacementResult replacement;
    // Complete detached physical stage. Source/history/save authority remains
    // the original captured document; this map never masquerades as a Snapshot.
    PhaseWallReplacementEntities edited_entities;
    PhysicalWallRoomPhaseReviewInventory room_inventory;
    bool needs_room_review{};
};

[[nodiscard]] nlohmann::json encode_phase_wall_replacement_authoring(const PhaseWallReplacementAuthoring& value);
[[nodiscard]] PhaseWallReplacementAuthoring decode_phase_wall_replacement_authoring(const nlohmann::json& value);
[[nodiscard]] PhaseWallReplacementAuthoringPreview inspect_phase_wall_replacement_authoring(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent);
[[nodiscard]] PhaseWallReplacementEntities replay_phase_wall_replacement_authoring(
    const PhaseWallReplacementEntities& source,const PhaseConstraintAuthoringIntent& intent);
void validate_phase_wall_replacement_originals(const PhaseWallReplacementEntities& source,
    const PhaseWallReplacementEntities& candidate,const PhaseConstraintAuthoringIntent& intent);
// Caller supplies the source-bound original intent and complete independently
// derived replacement map. The resulting exclusive command still needs room
// decisions where its derived inventory requires them.
[[nodiscard]] ApplyBoundaryConstraintChanges phase_wall_replacement_authoring_command(
    const PhaseConstraintAuthoringIntent& intent);

} // namespace sketch
