#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sketch {

// Dedicated split correspondence preserves every outer vertex, surviving edge
// and placed dimension identity while deriving the analytical seam from sources.
[[nodiscard]] std::map<std::string, Entity, std::less<>> complete_wall_split_measurement_sources(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& physical, const WallSplitIntent& intent);

struct WallMeasurementResult {
    Boundary boundary;
    nlohmann::json source;
    // Physical source identity for each analytical exterior edge in boundary
    // order. Fresh v1 results have canonical winding/seed; materialized v2
    // results retain the captured owner's winding and cyclic start.
    std::vector<std::string> ordered_wall_ids;
};

// Reconstructs physical sources and existing corner/T endpoint contacts. The
// measured owners remain unchanged until source completion derives their exact
// geometry. Rejects stale input, ambiguous contacts and unrepresentable inverses.
[[nodiscard]] std::map<std::string, Entity, std::less<>> exterior_corner_physical_entities(
    const std::map<std::string, Entity, std::less<>>& original,
    const ExteriorCornerMoveIntent& intent);
[[nodiscard]] nlohmann::json encode_exterior_corner_move(const ExteriorCornerMoveIntent& intent);
[[nodiscard]] ExteriorCornerMoveIntent decode_exterior_corner_move(const nlohmann::json& value);
// Resizes the measured outline analytically, reconstructs physical sources,
// then checks the final source-derived selected length, anchor and sweep after
// solving and source completion. The two movement flags remain independent.
[[nodiscard]] std::map<std::string, Entity, std::less<>> exterior_segment_resize_physical_entities(
    const std::map<std::string, Entity, std::less<>>& original,
    const ExteriorSegmentResizeIntent& intent);
[[nodiscard]] nlohmann::json encode_exterior_segment_resize(const ExteriorSegmentResizeIntent& intent);
[[nodiscard]] ExteriorSegmentResizeIntent decode_exterior_segment_resize(const nlohmann::json& value);
void validate_exterior_segment_resize_result(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& final,
    const ExteriorSegmentResizeIntent& intent);
// Reconstructs the selected measured chord arc through analytical physical
// sources. The measured receipt is command authority, never a physical input.
[[nodiscard]] std::map<std::string, Entity, std::less<>> exterior_segment_arc_physical_entities(
    const std::map<std::string, Entity, std::less<>>& original,
    const ExteriorSegmentArcIntent& intent);
[[nodiscard]] nlohmann::json encode_exterior_segment_arc(const ExteriorSegmentArcIntent& intent);
[[nodiscard]] ExteriorSegmentArcIntent decode_exterior_segment_arc(const nlohmann::json& value);
void validate_exterior_segment_arc_result(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& final,
    const ExteriorSegmentArcIntent& intent);
void validate_exterior_corner_physical_contacts(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& proposed);
struct ExteriorCornerPhysicalContact { std::string owner; bool start; std::string host; double station; };
[[nodiscard]] std::vector<ExteriorCornerPhysicalContact> exterior_corner_physical_contact_graph(
    const std::map<std::string, Entity, std::less<>>& original);
[[nodiscard]] std::vector<std::string> exterior_corner_perimeter_ids(
    const std::map<std::string, Entity, std::less<>>& original, const Entity& owner);

// Proves old edge/corner correspondence through physical wall identity, then
// derives stable-coordinate redraws for every affected existing source owner.
// Initially stale and anonymous sources retain explicit repair behavior.
// Initially current owners require unchanged cyclic adjacency and valid new
// context, joins, deductions and constraints; no identities are minted.
[[nodiscard]] std::vector<BoundaryGeometryEdit> exterior_wall_measurement_source_updates(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& proposed,
    bool validate_final_constraints = true,
    const std::map<std::string, Vec2, std::less<>>& rigid_offsets = {});

// Intrinsic v2 lineage validation, independent of today's source-wall presence.
// Retains every sequential add; bounds wall-pairs * (moves + 2) to 8,388,608
// before materialization, alongside the independent 4096-offset/2048-wall caps.
[[nodiscard]] WallMeasurementResult materialize_exterior_wall_measurement(const Entity& owner);
// Validates one typed translation against retained source and final physical walls.
[[nodiscard]] WallMeasurementResult derive_translated_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const std::vector<std::string>& wall_ids, const nlohmann::json& proof);

// Authored edits opt in explicitly. Completes ordinary wall changes or typed
// constraint changes through one v6 command; other command types and commands
// without affected source consumers are returned unchanged. Affected ordinary
// commands with asset changes or unsupported supplemental edits are rejected.
// Already completed commands are validated and returned unchanged.
[[nodiscard]] Command complete_exterior_wall_measurement_command(
    const DocumentSnapshot& source, const Command& command);

// Strict structural decoding only; historical wall IDs may no longer exist.
[[nodiscard]] std::vector<std::string> exterior_wall_measurement_source_ids(const Entity& owner);

// Recognizes the unique simple exterior in an analytical line/arc wall network.
// Interior partitions/loops and connected dangling branches are excluded.
// Disconnected geometry must be strictly inside that exterior. Returns sorted
// complete wall IDs; ambiguous topology, partial source walls, and unsupported
// geometry throw std::invalid_argument. Recognition never edits authoritative
// walls.
[[nodiscard]] std::vector<std::string> exterior_wall_measurement_sources(
    const DocumentSnapshot& document, const std::vector<std::string>& candidate_wall_ids);

// Derives the exterior outline for one closed loop of analytical line/arc
// source walls, retaining concentric curves and per-wall thickness. Throws
// std::invalid_argument when the selected walls do not form a supported simple
// outline or an offset join is ambiguous or exceeds the geometry envelope.
[[nodiscard]] WallMeasurementResult derive_exterior_wall_measurement(
    const DocumentSnapshot& document, const std::vector<std::string>& wall_ids);
[[nodiscard]] WallMeasurementResult derive_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& wall_ids);

// Rebinding additionally validates the measured owner's historical source,
// fully resolved hierarchy, phase and effective source-wall elevation plane.
[[nodiscard]] WallMeasurementResult derive_replacement_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const std::vector<std::string>& wall_ids);

// Exact retained-record compatibility only. Reproduces the original v1
// line/circle squared-distance arithmetic and geometry-gap allowance. Live
// creation and replacement must use the stable derivation APIs above.
[[nodiscard]] WallMeasurementResult derive_legacy_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& wall_ids);
[[nodiscard]] WallMeasurementResult derive_legacy_replacement_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const std::vector<std::string>& wall_ids);

// Boundaries without a wall measurement source remain current for compatibility.
// A malformed source, changed source context, missing wall, or edited outline
// returns false. Exact original-v1 outlines remain current without being
// rewritten. Openings do not change the measured exterior outline.
[[nodiscard]] bool wall_measurement_source_current(
    const DocumentSnapshot& document, const Entity& boundary);
[[nodiscard]] bool wall_measurement_source_current(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& boundary);

} // namespace sketch
