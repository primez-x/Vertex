#include "site_canvas_presentation.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sketch::desktop {
namespace {

void validate_transform(const SiteRigidTransform& transform) {
    // The core helper performs the canonical rigidity and finite-value check.
    (void)site_transform_delta({}, transform);
}

void add_bounded_entries(std::size_t& count, std::size_t amount,
                         std::size_t maximum, const char* message) {
    if (amount > maximum || count > maximum - amount)
        throw std::invalid_argument(message);
    count += amount;
}

std::size_t entity_geometry_entries(const CanvasEntity& entity,
                                    const SiteCanvasPresentationLimits& limits) {
    std::size_t count = 0;
    const auto add = [&](std::size_t amount) {
        add_bounded_entries(count, amount, limits.maximum_geometry_entries,
                           "site canvas entity geometry budget exceeded");
    };
    add(entity.segments.size());
    add(entity.holes.size());
    for (const auto& hole : entity.holes) add(hole.size());
    add(entity.vertex_handles.size());
    add(entity.snap_points.size());
    add(entity.snap_segments.size());
    if (entity.stroke_segments) add(entity.stroke_segments->size());
    add(entity.hit_segments.size());
    add(entity.drawing_alignment_segments.size());
    if (entity.opening_width_controls && entity.opening_width_controls->host_baseline) add(1);
    return count;
}

double presented_angle(double radians, const SiteRigidTransform& transform) {
    if (!std::isfinite(radians))
        throw std::invalid_argument("site canvas angle must be finite");
    const auto result = radians + transform.rotation_radians;
    if (!std::isfinite(result))
        throw std::invalid_argument("site canvas angle result must be finite");
    return result;
}

Segment presented_segment(const Segment& segment, const SiteRigidTransform& transform) {
    return site_transform_boundary(Boundary{segment}, transform).front();
}

Boundary presented_boundary(const Boundary& boundary, const SiteRigidTransform& transform) {
    return site_transform_boundary(boundary, transform);
}

double radians_to_degrees(double radians) {
    const auto degrees = radians * 180.0 / std::numbers::pi;
    if (!std::isfinite(degrees))
        throw std::invalid_argument("site canvas yaw is outside the finite degree range");
    return degrees;
}

} // namespace

Vec2 site_presented_plan_point(Vec2 point, const SitePresentationPlacement& placement) {
    const auto result = site_transform_point({point.x, point.y, 0.0}, placement.forward);
    return {result.x, result.y};
}

Vec2 site_presented_plan_delta(Vec2 delta, const SitePresentationPlacement& placement) {
    const auto result = site_transform_delta({delta.x, delta.y, 0.0}, placement.forward);
    return {result.x, result.y};
}

Vec2 site_source_plan_point(Vec2 point, const SitePresentationPlacement& placement) {
    const auto result = site_transform_point({point.x, point.y, 0.0}, placement.inverse);
    return {result.x, result.y};
}

Vec2 site_source_plan_delta(Vec2 delta, const SitePresentationPlacement& placement) {
    const auto result = site_transform_delta({delta.x, delta.y, 0.0}, placement.inverse);
    return {result.x, result.y};
}

CanvasEntity site_presented_canvas_entity(
    const CanvasEntity& source, const SitePresentationPlacement& placement,
    const SiteCanvasPresentationLimits& limits) {
    validate_transform(placement.forward);
    (void)entity_geometry_entries(source, limits);

    auto result = source;
    result.segments = presented_boundary(source.segments, placement.forward);
    result.holes.clear();
    result.holes.reserve(source.holes.size());
    for (const auto& hole : source.holes)
        result.holes.push_back(presented_boundary(hole, placement.forward));
    for (std::size_t i = 0; i < source.vertex_handles.size(); ++i)
        result.vertex_handles[i].position = site_presented_plan_point(
            source.vertex_handles[i].position, placement);
    for (std::size_t i = 0; i < source.snap_points.size(); ++i)
        result.snap_points[i] = site_presented_plan_point(source.snap_points[i], placement);
    result.snap_segments = presented_boundary(source.snap_segments, placement.forward);
    if (source.stroke_segments)
        result.stroke_segments = presented_boundary(*source.stroke_segments, placement.forward);
    result.hit_segments = presented_boundary(source.hit_segments, placement.forward);
    result.drawing_alignment_segments = presented_boundary(
        source.drawing_alignment_segments, placement.forward);

    if (source.resize_frame) {
        result.resize_frame->center = site_presented_plan_point(
            source.resize_frame->center, placement);
        result.resize_frame->rotation_radians = presented_angle(
            source.resize_frame->rotation_radians, placement.forward);
    }
    if (source.opening_width_controls) {
        result.opening_width_controls->start_jamb = site_presented_plan_point(
            source.opening_width_controls->start_jamb, placement);
        result.opening_width_controls->end_jamb = site_presented_plan_point(
            source.opening_width_controls->end_jamb, placement);
        if (source.opening_width_controls->host_baseline)
            result.opening_width_controls->host_baseline = presented_segment(
                *source.opening_width_controls->host_baseline, placement.forward);
    }
    if (source.svg_symbol) {
        result.svg_symbol->position = site_presented_plan_point(
            source.svg_symbol->position, placement);
        result.svg_symbol->rotation_radians = presented_angle(
            source.svg_symbol->rotation_radians, placement.forward);
    }
    return result;
}

CanvasLabel site_presented_canvas_label(
    const CanvasLabel& source, const SitePresentationPlacement& placement) {
    validate_transform(placement.forward);
    auto result = source;
    result.position = site_presented_plan_point(source.position, placement);
    result.rotation_radians = presented_angle(source.rotation_radians, placement.forward);
    if (source.automatic_linear_placement) {
        result.automatic_linear_placement->anchor = presented_segment(
            source.automatic_linear_placement->anchor, placement.forward);
        result.automatic_linear_placement->outward_normal = site_presented_plan_delta(
            source.automatic_linear_placement->outward_normal, placement);
    }
    if (source.leader_start)
        result.leader_start = site_presented_plan_point(*source.leader_start, placement);
    if (source.plan_label_offset)
        result.plan_label_offset = site_presented_plan_delta(*source.plan_label_offset, placement);
    return result;
}

CanvasReference site_presented_canvas_reference(
    const CanvasReference& source, const SitePresentationPlacement& placement) {
    validate_transform(placement.forward);
    auto result = source;
    result.position = site_presented_plan_point(source.position, placement);
    if (!std::isfinite(source.rotation_degrees))
        throw std::invalid_argument("site canvas reference angle must be finite");
    const auto rotation = source.rotation_degrees + radians_to_degrees(
        placement.forward.rotation_radians);
    if (!std::isfinite(rotation))
        throw std::invalid_argument("site canvas reference angle result must be finite");
    result.rotation_degrees = rotation;
    return result;
}

CanvasReferenceGrid site_presented_canvas_reference_grid(
    const CanvasReferenceGrid& source, const SitePresentationPlacement& placement,
    const SiteCanvasPresentationLimits& limits) {
    validate_transform(placement.forward);
    if (source.lines.size() > limits.maximum_grid_lines)
        throw std::invalid_argument("site canvas reference-grid line budget exceeded");
    auto result = source;
    for (std::size_t i = 0; i < source.lines.size(); ++i) {
        result.lines[i].start = site_presented_plan_point(source.lines[i].start, placement);
        result.lines[i].end = site_presented_plan_point(source.lines[i].end, placement);
    }
    return result;
}

} // namespace sketch::desktop
