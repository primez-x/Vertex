#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

inline constexpr double default_geometry_tolerance_metres = 1e-7;

struct Vec2 {
    double x{};
    double y{};
};

struct Segment {
    Vec2 start{};
    Vec2 end{};
    double sweep_radians{};
};

using Boundary = std::vector<Segment>;

struct Bounds2 {
    Vec2 minimum;
    Vec2 maximum;
};

// Axis-aligned geometric bounds, including circular-arc extrema analytically.
// Invalid/unrepresentable segments and empty boundaries throw. Computing bounds
// does not establish closure or absence of intersections.
[[nodiscard]] Bounds2 segment_bounds(const Segment& segment);
[[nodiscard]] Bounds2 boundary_bounds(const Boundary& boundary);

enum class BoundaryIssue {
    empty_boundary,
    non_finite,
    numeric_overflow,
    invalid_sweep,
    degenerate_segment,
    disconnected,
    open_boundary,
    self_intersection,
    overlapping_segments,
    indeterminate_intersection,
};

struct BoundaryDiagnostic {
    BoundaryIssue issue{};
    std::size_t segment_index{};
    std::optional<std::size_t> other_segment_index{};
    std::string message;
};

// Positive sweep is counter-clockwise. For a directed chord, its arc bulges
// toward the chord's right-hand side when the sweep is positive.
[[nodiscard]] Segment arc_from_chord_angle(Vec2 start, Vec2 end, double sweep_radians);

// signed_height is the arc midpoint offset along the directed chord's
// right-hand normal. Its sign therefore matches the returned sweep.
[[nodiscard]] Segment arc_from_chord_height(Vec2 start, Vec2 end, double signed_height);

// The arc length must be greater than the chord. Its unique sweep is below a
// full turn; clockwise selects the negative sweep. Unrepresentable cases fail.
[[nodiscard]] Segment arc_from_chord_arc_length(Vec2 start, Vec2 end, double arc_length_metres,
                                                bool clockwise = false);
// Construct a curve from its starting tangent and measured arc length. Angles
// are radians, lengths are metres, and positive sweep turns counter-clockwise.
[[nodiscard]] Segment arc_from_start_tangent(Vec2 start, double tangent_radians,
                                            double arc_length_metres, double sweep_radians);

[[nodiscard]] double segment_length(const Segment& segment);
[[nodiscard]] double perimeter(const Boundary& boundary);
// For a boundary connected and closed within the default local tolerance, a
// local origin avoids translation cancellation. Open or disconnected inputs
// retain the line integral over the supplied segments; validate_boundary
// determines whether an area is enclosed.
[[nodiscard]] double signed_area(const Boundary& boundary);

// Validation is screen independent and uses a local metre tolerance. It checks
// all line/line, line/arc, and circular-arc/arc pairs analytically. Numerically
// unresolved intersection cases are reported as indeterminate rather than
// accepted as valid.
[[nodiscard]] std::vector<BoundaryDiagnostic> validate_boundary(
    const Boundary& boundary,
    double tolerance_metres = default_geometry_tolerance_metres);

}  // namespace sketch
