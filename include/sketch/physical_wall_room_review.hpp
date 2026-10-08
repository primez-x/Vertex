#pragma once
#include "sketch/physical_wall_room.hpp"
#include "sketch/room_relationships.hpp"

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
// A direct ordinary wall-edit, profile change or curve command is admitted on a
// detached copy. Retained rooms remain available for correspondence review.
// Only unwrapped ordinary wall-bearing v2/v3/v4/v5/v6/v7/v11 and direct
// curve v23 proofs qualify. Profile changes require one existing-wall raw v1
// upsert or a completed v6/v7 physical upsert with exterior redraws. Ordinary
// connected/source consequences retain their existing child command authority.
[[nodiscard]] bool is_physical_wall_room_profile_review_command(const Command& command);
// Source-dependent profile admission preserves exact identity, extensions and
// every nonprofile property, including opaque geometry and drawing context.
// The declared profile includes height, thickness, layers and sloped top fields.
// Raw profile upserts refuse when linked exterior sources need completion.
void validate_physical_wall_room_profile_review_source(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::map<std::string,Entity,std::less<>>& candidate,const Command& command);
[[nodiscard]] DocumentSnapshot preview_physical_wall_room_review_geometry(
    const DocumentSnapshot& source,const Command& geometry_command);
// The report and intent belong to the detached geometry snapshot above. The
// returned single command is independently previewed against the original
// source; neither preparation publishes the intermediate geometry state.
// Full-snapshot/history/save hashes bind the original source, while the room
// entity-map hash retains the derived geometry that was actually reviewed.
[[nodiscard]] PreparedPhysicalWallRoomReviewAfterGeometry prepare_physical_wall_room_review_after_geometry(
    const DocumentSnapshot& source,const Command& geometry_command,
    const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent);
// Each plain envelope-eighteen command belongs to the preceding detached
// snapshot, starting with the geometry preview. Two to thirty-two disjoint
// context/plane reviews become one original-source-bound atomic command.
[[nodiscard]] PreparedPhysicalWallRoomReviewAfterGeometry prepare_physical_wall_room_review_batch_after_geometry(
    const DocumentSnapshot& source,const Command& geometry_command,
    const std::vector<ApplyBoundaryConstraintChanges>& staged_room_commands);
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
    const std::map<std::string,Entity,std::less<>>& source,const nlohmann::json& intent);
// Typed transitions are reconstructed from decisions, never accepted as proof.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_physical_wall_room_review(
    const std::map<std::string,Entity,std::less<>>& source,const nlohmann::json& intent);
// Sequential pure replay keeps every preceding entity-map digest exact and
// prevents fresh identity reuse across the entire batch, including retired
// owners, boundary children and replaced dimensions. No intermediate state is
// published and no classification or correspondence is inferred.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_physical_wall_room_review_batch(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::vector<nlohmann::json>& intents);
} // namespace sketch
