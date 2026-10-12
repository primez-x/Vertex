#pragma once

#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_profile_geometry.hpp"

#include <cmath>
#include <stdexcept>

namespace sketch::desktop {

// Derived canvas geometry only. Authoritative quantities stay in the model
// and dimension resolver. One physical 0.25 mm deviation and a shared per-owner
// segment budget apply to mapped outer/cutout paths, independent of zoom.
inline Boundary mapped_assembly_profile_canvas_path(const AssemblyExpandedProfile& profile,
                                                    const Boundary& boundary,
                                                    std::size_t& remaining_segments) {
    if (!profile.profile.planar_affine)
        throw std::invalid_argument("Mapped component presentation requires its actual profile map.");
    const auto deviation = 0.00025 / profile.transform.scale;
    auto result = assembly_profile_boundary_display_path(boundary, *profile.profile.planar_affine,
                                                         deviation, remaining_segments);
    remaining_segments -= result.size();
    for (auto& edge : result) {
        const auto place = [&](Vec2 point) {
            const auto placed = transform_assembly_point({point.x, point.y, 0.0}, profile.transform);
            if (!std::isfinite(placed.x) || !std::isfinite(placed.y))
                throw std::invalid_argument("Mapped component canvas placement is unrepresentable.");
            return Vec2{placed.x, placed.y};
        };
        edge.start = place(edge.start);
        edge.end = place(edge.end);
        if (edge.start.x == edge.end.x && edge.start.y == edge.end.y)
            throw std::invalid_argument("Mapped component canvas chord is unrepresentable.");
    }
    return result;
}

// Elliptical edge dimensions show the actual derived edge and a midpoint
// leader. A circular offset arc would falsely depict the affine image.
// The text uses the resolver's analytical length, never this display path.
inline Boundary mapped_assembly_dimension_canvas_overlay(const AssemblyDimensionResolution& geometry,
                                                         Vec2 text_position) {
    if (!geometry.profile_geometry || !geometry.profile_geometry->affine_edge)
        throw std::invalid_argument("Mapped component dimension has no exact edge descriptor.");
    const auto& edge = *geometry.profile_geometry->affine_edge;
    auto result = assembly_profile_boundary_display_path({edge.source}, edge.source_to_anchor_m, 0.00025, 8191);
    const auto [c, s] = assembly_rotation_components(geometry.selected_node.transform.rotation_radians);
    const auto sign = geometry.selected_node.transform.mirrored_y ? -1.0 : 1.0;
    const auto authored = [&](Vec2 relative) {
        const Vec2 point{geometry.anchor_authored_m.x + c * relative.x - s * sign * relative.y,
                         geometry.anchor_authored_m.y + s * relative.x + c * sign * relative.y};
        if (!std::isfinite(point.x) || !std::isfinite(point.y))
            throw std::invalid_argument("Mapped component dimension display is unrepresentable.");
        return point;
    };
    for (auto& line : result) {
        line.start = authored(line.start);
        line.end = authored(line.end);
    }
    const auto midpoint = authored(assembly_profile_segment_point(edge.source, edge.source_to_anchor_m, 0.5));
    if (!std::isfinite(text_position.x) || !std::isfinite(text_position.y))
        throw std::invalid_argument("Mapped component dimension text position is unrepresentable.");
    if (std::hypot(text_position.x - midpoint.x, text_position.y - midpoint.y) > 1e-7)
        result.push_back({midpoint, text_position, 0.0});
    return result;
}

inline bool has_mapped_assembly_dimension_curve(const AssemblyDimensionResolution& geometry) {
    return geometry.profile_geometry && geometry.profile_geometry->affine_edge &&
        geometry.profile_geometry->affine_edge->source.sweep_radians != 0.0;
}

} // namespace sketch::desktop
