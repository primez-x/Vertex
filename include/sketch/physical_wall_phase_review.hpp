#pragma once

#include "sketch/physical_wall_room_review.hpp"

namespace sketch {

enum class PhysicalWallRoomPhaseSourceDisposition { share_unchanged, supersede_in_target, redefine_proposed, retire_proposed };
enum class PhysicalWallRoomPhaseFreshDisposition { share_unchanged, create_proposed, leave_unclassified, redefine_proposed };

struct PhysicalWallRoomPhaseSourceDecision {
    std::string room_id;
    std::string expected_descriptor_digest;
    PhysicalWallRoomPhaseSourceDisposition disposition{PhysicalWallRoomPhaseSourceDisposition::share_unchanged};
    // Redefinition only: explicit retained-reference topology and fresh full
    // automatic edge-dimension identities. Empty decisions grant no mapping.
    nlohmann::json child_mapping=nlohmann::json::object();
    std::vector<std::string> replacement_dimension_ids;
};
struct PhysicalWallRoomPhaseFreshDecision {
    std::size_t candidate_index{};
    nlohmann::json reviewed_source_lineage;
    PhysicalWallRoomPhaseFreshDisposition disposition{PhysicalWallRoomPhaseFreshDisposition::share_unchanged};
    std::string room_id;
    Vec2 interior_witness;
    LegacyBoundaryIdentityOptions fresh_ids;
    // Creation requires all three facts. Sharing and redefinition cannot supply
    // replacement facts; redefinition retains the original owner's metadata.
    std::string name;
    std::string classification;
    std::optional<double> factor;
};
struct PhysicalWallRoomPhasePlaneReview {
    DrawingContext context;
    double effective_elevation_m{};
    std::vector<PhysicalWallRoomPhaseSourceDecision> source_rooms;
    std::vector<PhysicalWallRoomPhaseFreshDecision> fresh;
};
struct PhysicalWallRoomPhaseBaselineAcknowledgement {
    std::string entity_id;
    // Singleton entity-map digest, including every opaque field.
    std::string expected_entity_digest;
    // Exact affected room/child tokens mentioned by this retained entity.
    std::vector<std::string> referenced_ids;
};
struct PhysicalWallRoomPhasePresentationRemoval {
    std::string entity_id;
    // Exact complete original and independently pruned replacement entities.
    std::string expected_entity_digest;
    std::string expected_replacement_entity_digest;
    // Canonically sorted actual tokens removed from known presentation rows.
    std::vector<std::string> removed_entity_ids;
};
struct PhysicalWallRoomPhaseReviewIntent {
    std::string source_snapshot_digest;
    std::string source_authoring_digest;
    std::optional<Revision> source_saved_revision;
    std::string source_entities_digest;
    Revision expected_revision{};
    std::string registry_id;
    std::optional<std::string> alternative_id;
    // Null explicitly denotes registry creation. An existing registry binds
    // its exact original complete entity, independently of the child proof.
    std::optional<std::string> source_registry_entity_digest;
    nlohmann::json registry_command_proof;
    std::vector<PhysicalWallRoomPhasePlaneReview> planes;
    // Every saved dependent touching superseded owners/children remains in
    // the baseline only. No companion copy or identity transfer is implied.
    std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement> baseline_only_acknowledgements;
    // Version two alone admits existing target proposal redefinition/retirement.
    // Supported references touching those changed owners need complete explicit
    // Keep/Remove decisions; retiring relationship rows need exact acknowledgement.
    bool proposed_room_completion{};
    std::vector<std::string> removed_reference_ids;
    std::vector<std::string> kept_reference_ids;
    std::vector<PhysicalWallRoomRelationshipRemoval> relationship_removals;
    // Every affected saved view/annotation pruning requires explicit acceptance
    // of exact source/replacement evidence, independently rederived on replay.
    std::vector<PhysicalWallRoomPhasePresentationRemoval> presentation_removals;
};
struct PhysicalWallRoomPhaseProposedDependents {
    std::vector<std::string> reference_ids;
    // Exact proposals for removing the requested owners' graph memberships and
    // incident relations. Callers accept only rows for actual retire decisions.
    std::vector<PhysicalWallRoomRelationshipRemoval> relationship_removals;
};
struct PhysicalWallRoomPhaseReviewInventory {
    // Complete original-source and canonical-child binding. Plane rows contain
    // only context/elevation; the caller must supply every explicit decision.
    PhysicalWallRoomPhaseReviewIntent intent;
    // Actual entity-map phase reports, in the same order as intent.planes.
    std::vector<PhasePhysicalWallRoomCorrespondenceReport> reports;
};
struct ReplayedPhysicalWallPhaseRoomReview {
    std::map<std::string,Entity,std::less<>> entities;
    std::vector<std::string> created_room_ids;
    std::vector<std::string> preserved_room_ids;
    std::vector<std::string> redefined_room_ids;
    std::vector<std::string> retired_room_ids;
    // Includes a newly created registry, new room owners, every fresh child and
    // replacement dimensions. Retained redefined owner IDs are not fresh;
    // enclosing command authority must additionally reserve retained history.
    std::vector<std::string> fresh_identity_ids;
};
struct PreparedPhysicalWallPhaseRoomReview : ReplayedPhysicalWallPhaseRoomReview {
    nlohmann::json intent;
};
[[nodiscard]] nlohmann::json encode_physical_wall_phase_room_review_intent(
    const PhysicalWallRoomPhaseReviewIntent& intent);
[[nodiscard]] PhysicalWallRoomPhaseReviewIntent decode_physical_wall_phase_room_review_intent(
    const nlohmann::json& value);
// Pure preparation evidence using the replay's mandatory affected inventory and
// complete eligible original roster. Does not apply/preview a Document command
// or choose source dispositions, fresh facts/identities or acknowledgements.
[[nodiscard]] PhysicalWallRoomPhaseReviewInventory inspect_physical_wall_phase_room_review(
    const DocumentSnapshot& source,const ApplyEntityChanges& registry_command,
    const PhysicalWallPhaseSelection& destination);
// Proposed exact baseline-only acknowledgements for GUI review, never implicit
// acceptance. The named registry may be absent when its separate child creates
// it; an existing registry admits only its actual shared baseline room owners.
[[nodiscard]] std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement> physical_wall_phase_room_baseline_dependents(
    const DocumentSnapshot& source,const std::string& registry_id,
    const std::vector<std::string>& superseded_room_ids);
// Pure captured-source evidence. Every requested owner must be an actual
// original proposal of this exact registry/alternative; no decisions are made.
[[nodiscard]] PhysicalWallRoomPhaseProposedDependents physical_wall_phase_room_proposed_dependents(
    const DocumentSnapshot& source,const PhysicalWallPhaseSelection& destination,
    const std::vector<std::string>& changed_proposed_room_ids);
// Pure evidence for known sheet restrictions/overlays/appearance and annotation
// overrides. Returns sorted exact before/after entity proofs, never acceptance.
[[nodiscard]] std::vector<PhysicalWallRoomPhasePresentationRemoval> physical_wall_phase_room_presentation_removals(
    const DocumentSnapshot& source,const std::vector<std::string>& removed_entity_ids);
// Pure source-map replay. Registry proof is a canonical single raw upsert;
// all room geometry is rederived from explicit phase detection.
[[nodiscard]] ReplayedPhysicalWallPhaseRoomReview replay_physical_wall_phase_room_review(
    const std::map<std::string,Entity,std::less<>>& source,const nlohmann::json& intent);
// Binds complete original source/history/save state without Document mutation,
// preview, application or command-authority admission.
[[nodiscard]] PreparedPhysicalWallPhaseRoomReview prepare_physical_wall_phase_room_review(
    const DocumentSnapshot& source,const PhysicalWallRoomPhaseReviewIntent& intent);
} // namespace sketch
