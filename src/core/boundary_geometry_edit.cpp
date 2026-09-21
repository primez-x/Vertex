#include "sketch/geometry_operations.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sketch {
namespace {

void validate_editable_boundary(const IdentifiedBoundary& boundary) {
    (void)encode_identified_boundary_entity(boundary);
}

void validate_finite(Vec2 point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        throw std::invalid_argument("Point must be finite");
    }
}

} // namespace

IdentifiedBoundary move_boundary_vertex(const IdentifiedBoundary& source,
                                        std::string_view id,
                                        Vec2 position) {
    validate_finite(position);
    validate_editable_boundary(source);
    auto result = source;
    bool found = false;
    for (auto& edge : result.segments) {
        if (edge.start_vertex_id == id) {
            edge.segment.start = position;
            found = true;
        }
        if (edge.end_vertex_id == id) edge.segment.end = position;
    }
    if (!found) throw std::invalid_argument("Unknown vertex ID");
    validate_editable_boundary(result);
    return result;
}

IdentifiedBoundary set_boundary_segment_length(const IdentifiedBoundary& source,
                                                std::string_view id,
                                                double target_length,
                                                BoundaryFixedEndpoint fixed_endpoint,
                                                bool move_connected) {
    if (!std::isfinite(target_length) || target_length <= 0.0) {
        throw std::invalid_argument("Target segment length must be positive and finite");
    }
    if (fixed_endpoint != BoundaryFixedEndpoint::start &&
        fixed_endpoint != BoundaryFixedEndpoint::end) {
        throw std::invalid_argument("Unsupported fixed endpoint");
    }
    validate_editable_boundary(source);
    const auto found = std::find_if(source.segments.begin(), source.segments.end(),
        [&](const auto& edge) { return edge.segment_id == id; });
    if (found == source.segments.end()) throw std::invalid_argument("Unknown segment ID");
    const auto& segment = found->segment;
    const auto length = segment_length(segment);
    if (!std::isfinite(length) || length <= 0.0) {
        throw std::invalid_argument("Source segment length must be positive and finite");
    }
    if (target_length == length) return source;
    const bool fix_start = fixed_endpoint == BoundaryFixedEndpoint::start;
    const auto anchor = fix_start ? segment.start : segment.end;
    const auto moving = fix_start ? segment.end : segment.start;
    const auto& fixed_id = fix_start ? found->start_vertex_id : found->end_vertex_id;
    const auto& moving_id = fix_start ? found->end_vertex_id : found->start_vertex_id;
    // With a fixed sweep, analytical arc length scales directly with chord length.
    const auto scale = target_length / length;
    const Vec2 position{anchor.x + (moving.x - anchor.x) * scale,
                        anchor.y + (moving.y - anchor.y) * scale};
    validate_finite(position);
    if (!move_connected) return move_boundary_vertex(source, moving_id, position);

    const Vec2 delta{position.x - moving.x, position.y - moving.y};
    validate_finite(delta);
    auto result = source;
    const auto translate = [&](Vec2 point, const std::string& vertex_id) {
        if (vertex_id == fixed_id) return point;
        if (vertex_id == moving_id) return position;
        const Vec2 translated{point.x + delta.x, point.y + delta.y};
        validate_finite(translated);
        return translated;
    };
    for (auto& edge : result.segments) {
        edge.segment.start = translate(edge.segment.start, edge.start_vertex_id);
        edge.segment.end = translate(edge.segment.end, edge.end_vertex_id);
    }
    validate_editable_boundary(result);
    return result;
}

} // namespace sketch
