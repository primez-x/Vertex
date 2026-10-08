#pragma once
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/physical_wall_phase.hpp"

#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace sketch {
struct PhysicalWallSpace;
struct PhysicalWallRoomCheck {
    bool current{};
    std::string diagnostic;
    Boundary boundary;
    std::vector<Boundary> holes;
    double area_square_metres{};
};
struct PhysicalWallRoomLineageCheck {
    double effective_elevation_m{};
    std::vector<std::string> source_owner_ids;
};
// The same strict captured-v1 lineage/region admission as correspondence,
// without a document or fresh-source comparison. Malformed evidence rejects.
[[nodiscard]] PhysicalWallRoomLineageCheck validate_retained_physical_wall_room_lineage(
    const Entity& room,const DrawingContext& context);
// Predicate over caller-admitted fresh phase detection. Both captured-v1
// lineages and analytical regions are validated; only semantic_phases may
// differ. Exact physical inventory, selected source, context/plane and clear
// geometry must survive. Malformed evidence throws; this grants no command
// authority and neither updates nor transfers the retained owner's facts.
[[nodiscard]] bool physical_wall_room_lineage_matches_current_inventory(
    const Entity& room,const DrawingContext& context,const PhysicalWallSpace& fresh);
// Current values are rederived from physical walls, never claimed persisted
// areas. Malformed/future/stale owners receive diagnostics and empty geometry.
// Detection is cached within this call; no caller-owned document is modified.
[[nodiscard]] std::map<std::string,PhysicalWallRoomCheck,std::less<>>
physical_wall_room_checks(const DocumentSnapshot& source);
// Uses the same bounded analytical line/circle region representation as room
// correspondence. Both directed differences must have zero represented area;
// numerically uncertain or invalid comparisons refuse. Includes inline holes.
[[nodiscard]] bool physical_wall_room_regions_equal(
    const Boundary& first, const std::vector<Boundary>& first_holes,
    const Boundary& second, const std::vector<Boundary>& second_holes);

enum class PhysicalWallRoomCorrespondenceKind {
    unique_continuation, split, merge, new_space, retired, ambiguous
};
struct RetainedPhysicalWallRoomCorrespondence {
    // Complete retained facts/identities, never assigned to a fresh candidate.
    Entity room;
    std::string descriptor_digest;
    Boundary boundary;
    std::vector<Boundary> holes;
    PhysicalWallRoomCorrespondenceKind kind{PhysicalWallRoomCorrespondenceKind::ambiguous};
    std::vector<std::size_t> candidate_indices;
    std::string diagnostic;
};
struct FreshPhysicalWallRoomCorrespondence {
    // Indices are meaningful only within this source-bound report.
    std::size_t index{};
    std::size_t baseline_face_index{};
    Boundary boundary;
    std::vector<Boundary> holes;
    nlohmann::json source_lineage;
    // Freshly detected diagnostic quantity, not a second stored area authority.
    double area_square_metres{};
    PhysicalWallRoomCorrespondenceKind kind{PhysicalWallRoomCorrespondenceKind::ambiguous};
    std::vector<std::string> retained_room_ids;
    std::string diagnostic;
};
struct PhysicalWallRoomOverlap {
    std::string room_id;
    std::size_t candidate_index{};
    // Absent for an unresolved comparison. Zero with surviving lineage remains
    // an ambiguous proposal; a positive value alone never transfers identity.
    std::optional<double> area_square_metres;
    std::vector<MeasurementSourceUse> surviving_sources;
    bool exact_lineage_match{};
    bool reliable{};
    std::string diagnostic;
};
struct PhysicalWallRoomCorrespondenceReport {
    std::string document_id;
    Revision revision{};
    std::string source_snapshot_digest;
    std::string selected_wall_id;
    // Context/plane discovery has no selected physical owner. It can describe
    // the real empty result after deletion without inventing a source wall.
    bool context_plane_selection{};
    DrawingContext context;
    double effective_elevation_m{};
    std::vector<RetainedPhysicalWallRoomCorrespondence> retained;
    std::vector<FreshPhysicalWallRoomCorrespondence> fresh;
    // Deterministic room-ID/candidate-index order. Includes uncertain proposals
    // so they cannot silently become new/retired claims.
    std::vector<PhysicalWallRoomOverlap> overlaps;
    // Transient analytical phase reports must never borrow the ordinary
    // destructive retain/retire command's acceptance authority.
    bool explicit_phase_evaluation{false};
};
// A distinct analytical report for an explicitly selected destination phase
// and exact old owner roster. The ordinary payload remains detached evidence;
// it must not be admitted as an ordinary room command or identity transfer.
struct PhasePhysicalWallRoomCorrespondenceReport {
    PhysicalWallPhaseSelection destination_selection;
    std::vector<std::string> retained_room_ids;
    std::string source_entities_digest;
    PhysicalWallRoomCorrespondenceReport correspondence;
};
// Detached review evidence for one physical context/plane. Exact planar region
// intersections include holes and circular arcs; surviving boundary-source
// intervals qualify relationships. No bounds/centroid identity heuristic,
// classification transfer, commands, cross-snapshot cache or Document mutation.
// Invalid selection/detection/resource budgets throw; uncertain retained owners
// and comparisons are reported conservatively as ambiguous. Unique/split/merge
// describe review candidates, never permission for automatic reassignment.
[[nodiscard]] PhysicalWallRoomCorrespondenceReport physical_wall_room_correspondence(
    const DocumentSnapshot& source,std::string_view selected_wall_id);
[[nodiscard]] PhysicalWallRoomCorrespondenceReport physical_wall_room_correspondence(
    const DocumentSnapshot& source,const DrawingContext& context,double effective_elevation_m);
// Does not expand the supplied roster with other or inactive alternative
// owners. Original owner evidence/facts are retained even when the destination
// phase differs; duplicate, missing, nonphysical, foreign or unresolved roster
// selections reject. Empty destination detection and an empty roster are valid.
[[nodiscard]] PhasePhysicalWallRoomCorrespondenceReport phase_physical_wall_room_correspondence(
    const DocumentSnapshot& source,const DrawingContext& context,double effective_elevation_m,
    const PhysicalWallPhaseSelection& destination_selection,const std::vector<std::string>& retained_room_ids);
// Pure command replay uses the actual entity map and its digest, without
// constructing a synthetic document/history. Snapshot provenance is absent;
// the enclosing typed command must bind its original full snapshot separately.
[[nodiscard]] PhasePhysicalWallRoomCorrespondenceReport phase_physical_wall_room_correspondence(
    const std::map<std::string,Entity,std::less<>>& entities,const DrawingContext& context,
    double effective_elevation_m,const PhysicalWallPhaseSelection& destination_selection,
    const std::vector<std::string>& retained_room_ids);
[[nodiscard]] bool physical_wall_room_correspondence_is_current(
    const PhysicalWallRoomCorrespondenceReport& report,const DocumentSnapshot& source);
// Captured-snapshot preparation. Indices belong only to freshly detected
// clear components; repeated definitions reuse current owners without changing
// their classification. Inline holes do not create separate deduction tools.
[[nodiscard]] ApplyEntityChanges prepare_physical_wall_rooms(const DocumentSnapshot& source,
    std::string_view selected_wall_id,const std::vector<std::size_t>& indices,std::string classification);

struct PhysicalWallRoomRepairReferences {
    nlohmann::json child_mapping=nlohmann::json::object();
    std::vector<std::string> removed_reference_ids;
    std::vector<std::string> replacement_dimension_ids;
    bool allow_automatic_angle_removal{};
};
// Fresh child identities and explicit reference decisions belong to the
// reviewed reassignment. Preparation never chooses geometric correspondence.
[[nodiscard]] EditBoundaryGeometry prepare_physical_wall_room_repair(
    const DocumentSnapshot& source,std::string_view room_id,std::string_view selected_wall_id,
    Vec2 interior_witness,const nlohmann::json& reviewed_source_lineage,
    std::string expected_descriptor_digest,const LegacyBoundaryIdentityOptions& fresh_ids,
    const PhysicalWallRoomRepairReferences& references);
} // namespace sketch
