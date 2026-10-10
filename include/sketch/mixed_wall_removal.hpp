#pragma once

#include "sketch/architectural_selection_removal.hpp"
#include "sketch/physical_wall_room_review.hpp"

namespace sketch {

struct MixedWallRemovalIntent {
    // Canonical sorted actual owners; aliases never carry selection authority.
    std::vector<std::string> wall_ids;
    PhysicalWallJoinRemovalAdditionalIdentities wall_additional_identities;
    ArchitecturalSelectionRemovalIntent other;
};

using MixedWallRemovalEntities = std::map<std::string, Entity, std::less<>>;

// Source-only analytical admission before native join/roof inspectors. This
// neither invokes native factories nor grants any removal authority; replay
// independently repeats admission against its full immutable source.
// Explicit corner completion admits actual owner-hosted catalog copies while
// managed corner cuts remain bare voids. Historical callers keep their limits.
void validate_mixed_wall_removal_source_admission(const MixedWallRemovalEntities& actual,
    bool include_manufactured_opening_hosts=false, bool complete_corner_catalog_hosts=false);

// Both sides must be selected. Every leaf sees the same complete actual map.
// Attached openings follow their wall; independent opening/room/drawing roots
// have no authority here. Rooms and source lineage remain for detached review.
// Shared analytical admission precedes all native leaf work.
// Explicit completion also retires qualified opening-hosted rows with their
// selected wall, and admits selected components on retained semantic openings.
// Corner completion additionally admits selected actual corner owners/managed
// cuts and complete catalog consequences; every cut selection retires its full
// owner aggregate or refuses. Wall-covered aggregates collapse only after replay.
[[nodiscard]] MixedWallRemovalEntities replay_mixed_wall_removal(
    const MixedWallRemovalEntities& actual, const MixedWallRemovalIntent& intent,
    bool complete_opening_hosted_removal=false, bool complete_corner_window_consequences=false);

// Asset-free raw command, bound to the captured revision. Reserves wall and
// roof destinations together against actual source, all history and assets.
// Live publication must still retain the complete captured snapshot fence.
[[nodiscard]] ApplyEntityChanges prepare_mixed_wall_removal(
    const DocumentSnapshot& source, const MixedWallRemovalIntent& intent,
    const std::string& message, bool complete_opening_hosted_removal=false,
    bool complete_corner_window_consequences=false);

struct DecodedMixedWallDeletionReviewProof {
    ApplyEntityChanges command;
    MixedWallRemovalIntent intent;
    bool complete_opening_hosted_removal{false};
    bool complete_corner_window_consequences{false};
};

// Explicit closed v37 authority by default; v39 alone enables opening-hosted
// completion. Existing wall and architectural-selection proofs keep their meaning.
// Closed v49 explicitly completes corner owners, both managed cuts and catalog
// consequences. Selecting a managed cut grants only its complete owner removal.
[[nodiscard]] nlohmann::json encode_mixed_wall_deletion_review_proof(
    const DocumentSnapshot& source, const MixedWallRemovalIntent& intent,
    const Command& command, bool complete_opening_hosted_removal=false,
    bool complete_corner_window_consequences=false);
[[nodiscard]] DecodedMixedWallDeletionReviewProof decode_mixed_wall_deletion_review_proof(
    const nlohmann::json& proof);

// Exact whole-command and whole-candidate replay. The enclosing Document owns
// revision/history/asset admission and preceding-history destination lifetime.
void validate_mixed_wall_deletion_review_source(
    const MixedWallRemovalEntities& actual, const MixedWallRemovalEntities& candidate,
    const Command& command, const nlohmann::json& retained_proof);

} // namespace sketch
