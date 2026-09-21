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

struct PlanarTransform {
    Vec2 pivot{};
    double rotation_radians{};
    bool flip_horizontal{};
    bool flip_vertical{};
    Vec2 offset{};

    bool operator==(const PlanarTransform& other) const noexcept {
        return pivot.x == other.pivot.x && pivot.y == other.pivot.y &&
            rotation_radians == other.rotation_radians && flip_horizontal == other.flip_horizontal &&
            flip_vertical == other.flip_vertical && offset.x == other.offset.x && offset.y == other.offset.y;
    }
};

// Rotate around pivot, reflect X/Y around pivot, then translate. Parameters and
// output must be finite. A reflected circular arc reverses its signed sweep.
[[nodiscard]] Vec2 transform_point(Vec2 point, const PlanarTransform& transform);
[[nodiscard]] Segment transform_segment(const Segment& segment, const PlanarTransform& transform);

struct Bounds2 {
    Vec2 minimum;
    Vec2 maximum;
};

// Axis-aligned geometric bounds, including circular-arc extrema analytically.
// Invalid/unrepresentable segments and empty boundaries throw. Computing bounds
// does not establish closure or absence of intersections.
[[nodiscard]] Bounds2 segment_bounds(const Segment& segment);
[[nodiscard]] Bounds2 boundary_bounds(const Boundary& boundary);

// Clip line and circular-arc segments to a closed axis-aligned rectangle.
// Surviving arc pieces retain their exact circular representation; the helper
// never tessellates or invents edges along the crop rectangle. Segment order
// is preserved and an empty result means no nonzero-length geometry survives.
// Invalid bounds or source segments throw std::invalid_argument.
[[nodiscard]] Boundary clip_boundary_to_bounds(
    const Boundary& boundary, const Bounds2& bounds,
    double tolerance_metres = default_geometry_tolerance_metres);

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

// Checks individually valid closed boundaries, strict hole containment, and
// pairwise disjoint hole interiors and boundaries using analytical lines/arcs.
// Boundary contact within tolerance, nesting, and indeterminate geometry fail
// closed. Returns the first deterministic error, or no error for valid topology.
[[nodiscard]] std::optional<std::string> validate_boundary_holes(
    const Boundary& outer, const std::vector<Boundary>& holes,
    double tolerance_metres = default_geometry_tolerance_metres);

}  // namespace sketch
