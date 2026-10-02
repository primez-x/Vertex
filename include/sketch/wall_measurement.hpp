#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <string>
#include <vector>

namespace sketch {

struct WallMeasurementResult {
    Boundary boundary;
    nlohmann::json source;
};

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

} // namespace sketch
