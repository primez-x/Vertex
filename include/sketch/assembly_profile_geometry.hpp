#pragma once

#include "sketch/geometry.hpp"

#include <cstddef>

namespace sketch {

// The source lines/circular arcs remain authoritative. Their affine images are
// evaluated analytically (in general an ellipse), never encoded as new circular
// Segments. Reflections belong to the assembly node pose, not this map.
// q = target_origin + M * (p - source_origin).
struct AssemblyProfilePlanarAffine {
    Vec2 source_origin{};
    Vec2 target_origin{};
    double xx{1.0}, xy{}, yx{}, yy{1.0};
    bool operator==(const AssemblyProfilePlanarAffine& other) const noexcept;
};

// All APIs throw std::invalid_argument on invalid/non-finite or unrepresentable
// geometry. Validation requires a positive, representable determinant and
// finite inverse coefficients; there is no arbitrary minimum determinant.
void validate_assembly_profile_affine(const AssemblyProfilePlanarAffine& affine);
[[nodiscard]] double assembly_profile_affine_determinant(const AssemblyProfilePlanarAffine& affine);
[[nodiscard]] Vec2 assembly_profile_affine_point(Vec2 point, const AssemblyProfilePlanarAffine& affine);
[[nodiscard]] Vec2 assembly_profile_affine_direction(Vec2 direction, const AssemblyProfilePlanarAffine& affine);
[[nodiscard]] Vec2 assembly_profile_affine_inverse_point(Vec2 point, const AssemblyProfilePlanarAffine& affine);
// Apply inner first, then outer; retain inner's source origin as the anchor.
[[nodiscard]] AssemblyProfilePlanarAffine compose_assembly_profile_affine(
    const AssemblyProfilePlanarAffine& outer, const AssemblyProfilePlanarAffine& inner);

// Parameter is finite and in [0,1]. Endpoints are mapped directly and returned
// exactly as by affine_point. Tangent is dp/du, not a unit direction. Arc
// evaluation uses length and sinc half-angle displacement, never a huge center.
// Source admission matches segment_length: even a zero-sweep line must have
// distinct finite endpoints. An identity anchored map returns points unchanged.
[[nodiscard]] Vec2 assembly_profile_segment_point(
    const Segment& source, const AssemblyProfilePlanarAffine& affine, double parameter);
[[nodiscard]] Vec2 assembly_profile_segment_tangent(
    const Segment& source, const AssemblyProfilePlanarAffine& affine, double parameter);
[[nodiscard]] Bounds2 assembly_profile_segment_bounds(
    const Segment& source, const AssemblyProfilePlanarAffine& affine);

// Whole-operation hard cap, including the O(n^2) analytical admission used by
// closed area and all segment validation/bounds work. Empty boundaries and
// counts above this cap are rejected. Bounds/perimeter do not establish topology.
inline constexpr std::size_t assembly_profile_geometry_max_segments = 1024;
[[nodiscard]] Bounds2 assembly_profile_boundary_bounds(
    const Boundary& source, const AssemblyProfilePlanarAffine& affine);

// Requires exact endpoint connectivity/closure AND validate_boundary admission
// at the supplied positive metre tolerance. Tolerance-gapped rings must be
// explicitly repaired by the owner before using the closed-area identity.
// Translation never contributes area: result = det(M) * signed_area(source).
[[nodiscard]] double assembly_profile_closed_boundary_signed_area(
    const Boundary& source, const AssemblyProfilePlanarAffine& affine,
    double tolerance_metres = default_geometry_tolerance_metres);

struct AssemblyProfileLengthOptions {
    double absolute_tolerance_metres{1e-8};
    double relative_tolerance{1e-10};
    // Shared across the entire perimeter, not reset for each segment.
    std::size_t maximum_evaluations{262144};
    std::size_t maximum_subdivision_depth{24};
};
struct AssemblyProfileLengthResult {
    double length_metres{};
    // Adaptive Simpson/Richardson estimate plus a floating-point roundoff
    // allowance; an estimate, not a certified interval error bound.
    double estimated_error_metres{};
    std::size_t evaluations{};
};

// Lines and exact similarities use direct analytical length (zero quadrature
// evaluations/error estimate). Other arcs integrate L*||M R(sweep*u)t0||.
// Eight initial panels avoid whole-turn periodic aliasing, followed by
// deterministic left-before-right adaptive Simpson with Richardson correction.
// Success requires summed estimated error <= absolute + relative*length;
// perimeter distributes its absolute budget across all source segments.
// These budgets cover one boundary only; callers measuring profiles with holes
// or collections must also enforce their own shared collection work budget.
// Positive finite tolerances are mandatory. Hard limits are 1,048,576 speed
// evaluations and depth 32; exhausting any budget, nonconvergence, or numeric
// range failure throws std::invalid_argument and returns no partial result.
// Bounds/area are analytical and do not depend on this quadrature/tessellation.
[[nodiscard]] AssemblyProfileLengthResult assembly_profile_segment_length(
    const Segment& source, const AssemblyProfilePlanarAffine& affine,
    const AssemblyProfileLengthOptions& options = {});
[[nodiscard]] AssemblyProfileLengthResult assembly_profile_boundary_perimeter(
    const Boundary& source, const AssemblyProfilePlanarAffine& affine,
    const AssemblyProfileLengthOptions& options = {});

} // namespace sketch
