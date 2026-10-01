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

// Derives the exterior outline for one closed loop of straight source walls.
// Throws std::invalid_argument when the selected walls do not form a supported
// simple outline or exceed the supported geometry envelope.
[[nodiscard]] WallMeasurementResult derive_exterior_wall_measurement(
    const DocumentSnapshot& document, const std::vector<std::string>& wall_ids);

// Boundaries without a wall measurement source remain current for compatibility.
// A malformed source, changed source context, missing wall, or edited outline
// returns false. Openings do not change the measured exterior outline.
[[nodiscard]] bool wall_measurement_source_current(
    const DocumentSnapshot& document, const Entity& boundary);

} // namespace sketch
