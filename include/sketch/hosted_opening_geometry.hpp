#pragma once

#include "sketch/geometry.hpp"
#include "sketch/wall_semantics.hpp"

namespace sketch {

// Distances are model metres along the host's directed line or circular arc.
// The span must fit the host (allowing the standard endpoint tolerance).
// Curved spans retain their signed sweep and measured arc length.
[[nodiscard]] Segment hosted_opening_span(const Segment& host,
    double offset_metres, double width_metres);

// Signed, finite stations may extrapolate beyond either host endpoint so a
// drag can display a candidate which admission will subsequently reject.
[[nodiscard]] Vec2 point_at_host_station(const Segment& host, double station_metres);

// Line projection is unclamped. Arc projection ignores radial displacement,
// choosing the unwrapped station nearest reference_station_metres; the centre
// has no unique projection and is rejected. Invalid/unrepresentable inputs to
// these helpers throw std::invalid_argument.
[[nodiscard]] double project_host_station(const Segment& host, Vec2 point,
    double reference_station_metres);

// Two glazing outlines and two connecting jambs. Curved glazing uses exact
// concentric arcs. Wall thickness must be finite, positive and fit the radius.
// Straight hosts retain the established tangent-left inset clamp.
[[nodiscard]] Boundary window_plan_symbol(const Segment& host,
    double offset_metres, double width_metres, double wall_thickness_metres);

// Plan faces and jamb ends of the remaining wall intervals. Offsets follow the
// directed host's left normal; circular faces retain their exact sweep. An
// opening spanning the complete host leaves no wall linework.
[[nodiscard]] Boundary wall_plan_footprint(const Segment& host,
    const std::vector<HostedOpening>& openings, double wall_thickness_metres);

} // namespace sketch
