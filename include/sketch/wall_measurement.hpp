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

// Boundaries without a wall measurement source remain current for compatibility.
// A malformed source, changed source context, missing wall, or edited outline
// returns false. Openings do not change the measured exterior outline.
[[nodiscard]] bool wall_measurement_source_current(
    const DocumentSnapshot& document, const Entity& boundary);

} // namespace sketch
