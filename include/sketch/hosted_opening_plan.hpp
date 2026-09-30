#pragma once

#include "sketch/door_operation.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/wall_semantics.hpp"

#include <cmath>
#include <optional>
#include <stdexcept>

namespace sketch {

// Bound source work before entering the solid kernel. The plan adapter accepts
// at most 128 hosted cuts and 64 layers, in a world envelope of +/-1e6 metres.
// This guard is also available to core-only exchange builds.
inline void validate_hosted_opening_plan_source(const Wall& wall) {
    constexpr double envelope = 1e6;
    const auto bounded = [envelope](double value) {
        return std::isfinite(value) && std::abs(value) <= envelope;
    };
    if (wall.openings.size() > 128 || wall.layers.size() > 64 ||
        !bounded(wall.thickness) || !bounded(wall.height) || !bounded(wall.elevation) ||
        !bounded(wall.elevation + wall.height) ||
        (wall.slope_rise && (!bounded(*wall.slope_rise) ||
                            !bounded(wall.elevation + wall.height + *wall.slope_rise))))
        throw std::invalid_argument("Hosted plan source exceeds its bounded work envelope");
    validate_wall_semantics(wall);
    const auto length = segment_length(wall.baseline);
    if (wall.baseline.sweep_radians != 0.0 &&
        !bounded(length / std::abs(wall.baseline.sweep_radians)))
        throw std::invalid_argument("Hosted plan arc radius exceeds its bounded kernel envelope");
    const auto bounds = segment_bounds(wall.baseline);
    if (!bounded(bounds.minimum.x - wall.thickness) || !bounded(bounds.minimum.y - wall.thickness) ||
        !bounded(bounds.maximum.x + wall.thickness) || !bounded(bounds.maximum.y + wall.thickness) ||
        !bounded(length))
        throw std::invalid_argument("Hosted plan source exceeds its bounded coordinate envelope");
}

// Physical horizontal cut through the middle of the hosted manufactured solid,
// in world XY metres. Uses the shared make_wall/make_opening_assembly admission
// and exact section projector. Door operation adds the physical clear-leaf swing arc;
// a missing operation preserves the authoritative closed leaf and adds no swing.
// This is presentation geometry, never an authoring or measurement boundary.
[[nodiscard]] Boundary project_hosted_opening_plan(
    const Wall& wall, const HostedOpening& opening, const OpeningAssembly& assembly,
    const std::optional<DoorOperation>& operation = std::nullopt);

} // namespace sketch
