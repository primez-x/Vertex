#pragma once

#include "sketch/architectural_selection_removal.hpp"
#include "sketch/physical_wall_room_review.hpp"

namespace sketch {

struct MixedWallOpeningRemovalIntent {
    // Sorted actual owners and qualified catalog rows; aliases grant no authority.
    std::vector<std::string> wall_ids;
    std::vector<std::string> opening_ids;
    PhysicalWallJoinRemovalAdditionalIdentities wall_additional_identities;
    ArchitecturalSelectionRemovalIntent other;
};

using MixedWallOpeningRemovalEntities = std::map<std::string, Entity, std::less<>>;

// Complete immutable-source leaves. Openings on selected walls collapse into
// wall consequences; independent openings retain their own complete leaf.
// Other architectural/catalog roots are optional, retaining the architectural
// leaf's narrow qualified roof phase-retention semantics. No independent
// room/drawing or general shared-baseline demolition authority is granted. The explicit constraint
// policy belongs to the actual captured history, never inferred from registries.
[[nodiscard]] MixedWallOpeningRemovalEntities replay_mixed_wall_opening_removal(
    const MixedWallOpeningRemovalEntities& actual, const MixedWallOpeningRemovalIntent& intent,
    bool active_phase_constraints);

// Reserves all wall/roof slots against real source/history/assets before native
// work, and validates deterministic asset-free raw changes through exact real
// snapshot preview and geometry admission. Publication retains the source fence.
[[nodiscard]] ApplyEntityChanges prepare_mixed_wall_opening_removal(
    const DocumentSnapshot& source, const MixedWallOpeningRemovalIntent& intent,
    const std::string& message);

struct DecodedMixedWallOpeningDeletionReviewProof {
    ApplyEntityChanges command;
    MixedWallOpeningRemovalIntent intent;
};

// Closed version40 mixed_wall_opening_deletion envelope. Historical wall/mixed
// proofs keep their existing meanings. The raw version-one child is asset-free.
[[nodiscard]] nlohmann::json encode_mixed_wall_opening_deletion_review_proof(
    const DocumentSnapshot& source, const MixedWallOpeningRemovalIntent& intent,
    const Command& command);
[[nodiscard]] DecodedMixedWallOpeningDeletionReviewProof decode_mixed_wall_opening_deletion_review_proof(
    const nlohmann::json& proof);

// Exact whole-command and whole-candidate actual-source replay. The enclosing
// Document owns revision/history/assets and preceding-history lifetime admission.
void validate_mixed_wall_opening_deletion_review_source(
    const MixedWallOpeningRemovalEntities& actual, const MixedWallOpeningRemovalEntities& candidate,
    const Command& command, const nlohmann::json& retained_proof, bool active_phase_constraints);

} // namespace sketch
