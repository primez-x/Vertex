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

// Snap a signed station to the nearest lattice from host station zero, with
// half increments rounded away from zero. A zero increment returns the exact
// input unchanged. Finite stations and finite nonnegative increments are
// required; unrepresentable arithmetic throws std::invalid_argument. No host
// bounds are applied, preserving the existing admission checks after snapping.
[[nodiscard]] double quantize_host_station(double station_metres, double increment_metres);

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

struct WallPlanJunction {
    bool at_start{};
    Segment neighbor;
    double neighbor_thickness{};
};

struct WallPlanGeometry {
    // Complete closed wall-interval polygons for fill and hit testing.
    Boundary footprint;
    // Visible plan edges. Endpoint miters omit shared caps; the document
    // projection can also subtract internal material strokes at T/X junctions.
    Boundary strokes;
    // True only when the corresponding host endpoint cap was successfully joined.
    bool joined_start{};
    bool joined_end{};
};

// Straight wall endpoints can be joined to one unambiguous straight neighbor
// each. Successful unequal-angle joins miter the actual face endpoints and
// preserve closed footprint polygons while omitting their shared cap strokes.
// Curves, openings at the joined endpoint, ambiguous neighbors, and unsafe
// miters retain the established butt-capped wall geometry.
[[nodiscard]] WallPlanGeometry joined_wall_plan_geometry(const Segment& host,
    const std::vector<HostedOpening>& openings, double wall_thickness_metres,
    const std::vector<WallPlanJunction>& junctions);

// The canonical wall overload also removes admitted straight pocket cavities.
// Closed, nonoverlapping convex quadrilaterals cover the remaining material;
// visible strokes exclude the subdivision seams and retain joined-end omission.
// Walls without pockets use the preceding overload without any changes.
[[nodiscard]] WallPlanGeometry joined_wall_plan_geometry(const Wall& wall,
    const std::vector<WallPlanJunction>& junctions);

} // namespace sketch
