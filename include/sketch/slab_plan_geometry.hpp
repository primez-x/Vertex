#pragma once

// Pure slab plan geometry: no native kernel dependency. Configurations built
// without OCCT link this through sketch_slab_plan_geometry.
#include "sketch/assembly_profile_geometry.hpp"
#include <nlohmann/json.hpp>
#include <concepts>
#include <cstddef>
#include <vector>

namespace sketch {

// Closed version-1 wire: version, source_origin_m, target_origin_m, matrix.
// Reject unknown fields, wrong counts/types, nonfinite values and invalid maps.
[[nodiscard]] nlohmann::json encode_slab_planar_affine(const AssemblyProfilePlanarAffine& affine);
[[nodiscard]] AssemblyProfilePlanarAffine decode_slab_planar_affine(const nlohmann::json& value);

// Borrowed view of authoritative slab source rings and their optional map.
// A null map retains historical slab geometry and circular quantities.
struct SlabPlanSource {
    const Boundary& boundary;
    const std::vector<Boundary>& holes;
    const AssemblyProfilePlanarAffine* planar_affine{};
};

// Any slab-shaped aggregate (Slab, decoded document slab, frame structs) whose
// planar_affine is std::optional<AssemblyProfilePlanarAffine>.
template <class S>
concept SlabPlanShaped = requires(const S& s) {
    { s.boundary } -> std::convertible_to<const Boundary&>;
    { s.holes } -> std::convertible_to<const std::vector<Boundary>&>;
    { s.planar_affine.has_value() } -> std::convertible_to<bool>;
};

template <SlabPlanShaped S>
[[nodiscard]] SlabPlanSource slab_plan_source(const S& slab) {
    return {slab.boundary, slab.holes, slab.planar_affine ? &*slab.planar_affine : nullptr};
}

// Effective plan position of a source vertex. Identity when unmapped.
[[nodiscard]] inline Vec2 slab_plan_effective_point(const SlabPlanSource& slab, Vec2 source) {
    return slab.planar_affine ? assembly_profile_affine_point(source, *slab.planar_affine) : source;
}

// Admit source topology and analytical mapped representability first. All
// rings share the 1024-source-segment cap. Area is the nonnegative net outer
// area minus holes, scaled analytically by the determinant; display chords
// never supply measurement truth. Unmapped quantities retain circular truth.
[[nodiscard]] double slab_plan_signed_area(const SlabPlanSource& slab);
// Actual outer plus hole lengths; mapped quadrature shares one evaluation and
// absolute-error budget across all rings and rejects any partial result.
[[nodiscard]] double slab_plan_perimeter(const SlabPlanSource& slab);
[[nodiscard]] Bounds2 slab_plan_bounds(const SlabPlanSource& slab);

// Outer first, then authored holes. Unmapped rings retain their exact source
// primitives; mapped paths are derived display chords only. Every ring shares
// one aggregate output-segment cap (1..65536).
[[nodiscard]] std::vector<Boundary> slab_plan_display_boundaries(
    const SlabPlanSource& slab, double maximum_deviation_metres, std::size_t maximum_segments = 8192);

template <SlabPlanShaped S>
[[nodiscard]] double slab_plan_signed_area(const S& slab) { return slab_plan_signed_area(slab_plan_source(slab)); }
template <SlabPlanShaped S>
[[nodiscard]] double slab_plan_perimeter(const S& slab) { return slab_plan_perimeter(slab_plan_source(slab)); }
template <SlabPlanShaped S>
[[nodiscard]] Bounds2 slab_plan_bounds(const S& slab) { return slab_plan_bounds(slab_plan_source(slab)); }
template <SlabPlanShaped S>
[[nodiscard]] std::vector<Boundary> slab_plan_display_boundaries(
    const S& slab, double maximum_deviation_metres, std::size_t maximum_segments = 8192) {
    return slab_plan_display_boundaries(slab_plan_source(slab), maximum_deviation_metres, maximum_segments);
}
template <SlabPlanShaped S>
[[nodiscard]] Vec2 slab_plan_effective_point(const S& slab, Vec2 source) {
    return slab_plan_effective_point(slab_plan_source(slab), source);
}

} // namespace sketch
