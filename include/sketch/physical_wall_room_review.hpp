#pragma once
#include "sketch/physical_wall_room.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/wall_join_removal.hpp"

namespace sketch {
enum class PhysicalWallRoomRetainedDisposition { retain, retire };
enum class PhysicalWallRoomFreshDisposition { retained, create, unclassified };
struct PhysicalWallRoomRetainedDecision {
    std::string room_id;
    std::string expected_descriptor_digest;
    PhysicalWallRoomRetainedDisposition disposition{PhysicalWallRoomRetainedDisposition::retain};
    // Used only when retaining. No geometric correspondence is inferred.
    nlohmann::json child_mapping=nlohmann::json::object();
    std::vector<std::string> replacement_dimension_ids;
};
struct PhysicalWallRoomFreshDecision {
    std::size_t candidate_index{};
    nlohmann::json reviewed_source_lineage;
    PhysicalWallRoomFreshDisposition disposition{PhysicalWallRoomFreshDisposition::unclassified};
    // Retained owner or explicitly chosen new identity; empty if unclassified.
    std::string room_id;
    Vec2 interior_witness;
    LegacyBoundaryIdentityOptions fresh_ids;
    // Creation only: all metadata/context is explicitly reviewed.
    std::string name;
    std::string classification;
    DrawingContext context;
};
struct PhysicalWallRoomRelationshipRemoval {
    std::string entity_id;
    std::vector<std::string> removed_room_ids;
    std::vector<RoomRelation> acknowledged_relations;
};
struct PhysicalWallRoomReviewIntent {
    std::string selected_wall_id;
    std::string source_snapshot_digest;
    std::string source_authoring_digest;
    std::optional<Revision> source_saved_revision;
    std::string source_entities_digest;
    DrawingContext context;
    double effective_elevation_m{};
    std::vector<PhysicalWallRoomRetainedDecision> retained;
    std::vector<PhysicalWallRoomFreshDecision> fresh;
    // Only supported, affected dimensions/endpoint constraints may be removed.
    std::vector<std::string> removed_reference_ids;
    std::vector<std::string> kept_reference_ids;
    std::vector<PhysicalWallRoomRelationshipRemoval> relationship_removals;
    // Version two discovers this exact context/plane without a selected wall.
    // Its selected_wall_id must be empty; version one remains seed-based.
    bool context_plane_selection{};
    // Version three admits ordinary retained coverage and new-room membership
    // through actual saved phase choices. One/two retain collect-all replay.
    bool active_phase_room_scope{};
};
struct ReplayedPhysicalWallRoomReview {
    std::map<std::string,Entity,std::less<>> entities;
    std::vector<BoundaryGeometryEdit> retained_edits;
    std::vector<std::string> created_room_ids;
    std::vector<std::string> retired_room_ids;
};
struct PreparedPhysicalWallRoomReview : ReplayedPhysicalWallRoomReview {
    nlohmann::json intent;
};
struct PreparedPhysicalWallRoomReviewAfterGeometry {
    ApplyBoundaryConstraintChanges command;
    DocumentSnapshot snapshot;
};
using PreparedPhysicalWallRoomReviewAfterCurve=PreparedPhysicalWallRoomReviewAfterGeometry;
[[nodiscard]] nlohmann::json encode_physical_wall_room_review_intent(const PhysicalWallRoomReviewIntent& intent);
[[nodiscard]] PhysicalWallRoomReviewIntent decode_physical_wall_room_review_intent(const nlohmann::json& value);
// Detached preparation binds the complete captured state, including history,
// assets and saved/navigation state. Cancellation leaves the source untouched.
[[nodiscard]] PreparedPhysicalWallRoomReview prepare_physical_wall_room_review(
    const DocumentSnapshot& source,const PhysicalWallRoomCorrespondenceReport& report,
    const PhysicalWallRoomReviewIntent& intent);
// A direct ordinary/rigid wall-edit, intact joint wall command, profile change
// or curve command is admitted on a detached copy. Retained rooms remain
// available for correspondence review.
// Ordinary wall-bearing proofs require unwrapped v2/v3/v4/v5/v6/v7/v11;
// curve proofs require direct v23. Profile changes require one existing-wall raw v1
// upsert or a completed v6/v7 physical upsert with exterior redraws. Ordinary
// connected/source consequences retain their existing child command authority.
[[nodiscard]] bool is_physical_wall_room_profile_review_command(const Command& command);
// Direct rigid v10 or mixed measured-source v11 requires at least one curved
// v4/straight v5 wall proof. Connected neighbors keep ordinary v1/v2/v3
// authority. A v21 wall-callout completion may wrap only that same v10/v11
// child and retains the core's shared-transform validation. No other wrapper,
// room intent, asset/reference lane or curve-construction proof qualifies.
// This source-independent predicate checks the bounded canonical child shape;
// Document preview still independently admits all geometry and consequences.
[[nodiscard]] bool is_physical_wall_room_rigid_review_command(const Command& command);
// An intact bounded v17 requires actual wall edits and selected physical wall
// IDs. Shared translation and per-owner v3/v4 operators retain the original
// joint codec and source replay. Lower physical/exterior/measured/dimension
// receipts and position-only presentation proofs keep that child authority;
// competing intents and asset mutation/reference lanes do not qualify.
[[nodiscard]] bool is_physical_wall_room_joint_review_command(const Command& command);
// One existing wall and its supported hosted openings, saved dimensions and
// attached constraints are removed together. Known phase/view/presentation
// memberships are reconstructed from the exact original source; room owners
// remain unchanged until explicit context/plane review.
// Opt-in complete removal includes actual semantic openings and qualified
// hosted catalog rows. A closed v35 proof grants that original replay
// authority; historical raw singleton/v31 consequences retain their meaning.
// Join completion additionally requires hosted completion and exact explicit
// fresh split-join slots. Closed v36 grants the original join authority.
// Opening-hosted completion is a separate opt-in v38 authority. It includes
// qualified rows on removed semantic openings, with actual wall/sibling cuts;
// hosted and join completion are both required. Historical proofs stay exact.
[[nodiscard]] ApplyEntityChanges prepare_physical_wall_deletion(
    const DocumentSnapshot& source,std::string_view wall_id,bool complete_hosted_removal=false,
    bool complete_join_removal=false,const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities={});
// Wall-only selections are sorted; duplicates and more than 128 roots refuse.
// Legacy singleton commands retain their raw-v1 proof and message. Legacy
// groups require explicit v31 qualification; complete consequences use v35.
[[nodiscard]] ApplyEntityChanges prepare_physical_walls_deletion(
    const DocumentSnapshot& source,const std::vector<std::string>& wall_ids,bool complete_hosted_removal=false,
    bool complete_join_removal=false,const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities={},
    bool complete_opening_hosted_removal=false);
// Complete immutable actual-map replay for a larger source-derived removal
// composition. Includes hosted and join consequences. The enclosing authoring
// command owns snapshot/history/assets/fresh-ID reservation and room review;
// this function grants neither supplied-candidate nor arbitrary erase authority.
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_complete_physical_walls_deletion(
    const std::map<std::string,Entity,std::less<>>& actual,const std::vector<std::string>& wall_ids,
    const PhysicalWallJoinRemovalAdditionalIdentities& additional_join_identities={},
    bool complete_opening_hosted_removal=false);
// Analytical aggregate admission before native join inspection. The caller
// must first reserve fresh identities against the complete original snapshot.
void preflight_physical_walls_deletion_join_inference(
    const std::map<std::string,Entity,std::less<>>& actual,const std::vector<std::string>& wall_ids,
    bool complete_opening_hosted_removal=false);
[[nodiscard]] nlohmann::json encode_physical_wall_deletion_review_proof(
    const DocumentSnapshot& source,const Command& command);
// Accepts only bounded canonical grouped v31 or complete v35/v36/v38 envelopes and
// returns their raw child. Actual source must validate all declared roots.
[[nodiscard]] Command decode_physical_wall_deletion_review_proof(const nlohmann::json& proof);
[[nodiscard]] bool is_physical_wall_room_deletion_review_command(const Command& command);
// Exclusive semantic active-design authoring, independently replayed from the
// actual source before it can lend geometry to reviewed room consequences.
[[nodiscard]] bool is_physical_wall_room_active_constraint_review_command(const Command& command);
void validate_physical_wall_room_deletion_review_source(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate,const Command& command,
    const nlohmann::json& retained_proof=nlohmann::json(nullptr));
// Source-dependent profile admission preserves exact identity, extensions and
// every nonprofile property, including opaque geometry and drawing context.
// The declared profile includes height, thickness, layers and sloped top fields.
// Raw profile upserts refuse when linked exterior sources need completion.
void validate_physical_wall_room_profile_review_source(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate,const Command& command);
[[nodiscard]] DocumentSnapshot preview_physical_wall_room_review_geometry(
    const DocumentSnapshot& source,const Command& geometry_command,
    const nlohmann::json& retained_geometry_proof=nlohmann::json(nullptr));
// The report and intent belong to the detached geometry snapshot above. The
// returned single command is independently previewed against the original
// source; neither preparation publishes the intermediate geometry state.
// Full-snapshot/history/save hashes bind the original source, while the room
// entity-map hash retains the derived geometry that was actually reviewed.
[[nodiscard]] PreparedPhysicalWallRoomReviewAfterGeometry prepare_physical_wall_room_review_after_geometry(
    const DocumentSnapshot& source,const Command& geometry_command,
    const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent,
    const nlohmann::json& retained_geometry_proof=nlohmann::json(nullptr));
// Each plain envelope-eighteen/version-one or envelope-twenty-nine/version-two
// command belongs to the preceding detached
// snapshot, starting with the geometry preview. Two to thirty-two disjoint
// context/plane reviews become one original-source-bound atomic command.
[[nodiscard]] PreparedPhysicalWallRoomReviewAfterGeometry prepare_physical_wall_room_review_batch_after_geometry(
    const DocumentSnapshot& source,const Command& geometry_command,
    const std::vector<ApplyBoundaryConstraintChanges>& staged_room_commands,
    const nlohmann::json& retained_geometry_proof=nlohmann::json(nullptr));
// Narrow compatibility helpers retain the direct curve-construction contract.
[[nodiscard]] DocumentSnapshot preview_physical_wall_room_review_curve(
    const DocumentSnapshot& source,const Command& curve_command);
[[nodiscard]] PreparedPhysicalWallRoomReviewAfterCurve prepare_physical_wall_room_review_after_curve(
    const DocumentSnapshot& source,const Command& curve_command,
    const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent);
// Pure rederivation for a dedicated typed atomic command. This checks the exact
// preceding entity map, not its enclosing history/assets; command authority must
// separately bind those and enforce identity lifetimes. No Document or preview.
// Reference topology is reviewed explicitly; numeric dimensions are resolved
// against the complete rederived physical-room candidate, including holes.
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_physical_wall_room_review_entities(
    const std::map<std::string,Entity,std::less<>>& source,const nlohmann::json& intent,
    bool active_phase_constraints=false);
// Typed transitions are reconstructed from decisions, never accepted as proof.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_physical_wall_room_review(
    const std::map<std::string,Entity,std::less<>>& source,const nlohmann::json& intent,
    bool active_phase_constraints=false);
// Sequential pure replay keeps every preceding entity-map digest exact and
// prevents fresh identity reuse across the entire batch, including retired
// owners, boundary children and replaced dimensions. No intermediate state is
// published and no classification or correspondence is inferred.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_physical_wall_room_review_batch(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::vector<nlohmann::json>& intents,bool active_phase_constraints=false);
} // namespace sketch
