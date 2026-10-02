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

IdentifiedBoundary insert_boundary_vertex(const IdentifiedBoundary& source, std::string_view id,
                                          double fraction, std::string vertex_id,
                                          std::string second_id) {
    validate_editable_boundary(source);
    if (!std::isfinite(fraction) || fraction <= 0 || fraction >= 1)
        throw std::invalid_argument("Insertion fraction must be inside (0,1)");
    auto result = source;
    auto found = std::find_if(result.segments.begin(), result.segments.end(),
        [&](const auto& edge) { return edge.segment_id == id; });
    if (found == result.segments.end()) throw std::invalid_argument("Unknown segment ID");
    const auto original = *found;
    const auto& segment = original.segment;
    Vec2 point{std::lerp(segment.start.x, segment.end.x, fraction),
               std::lerp(segment.start.y, segment.end.y, fraction)};
    if (segment.sweep_radians != 0) {
        const auto dx = segment.end.x - segment.start.x;
        const auto dy = segment.end.y - segment.start.y;
        const auto k = 0.5 / std::tan(segment.sweep_radians / 2);
        const Vec2 center{segment.start.x + dx / 2 - dy * k,
                          segment.start.y + dy / 2 + dx * k};
        const auto angle = segment.sweep_radians * fraction;
        const auto x = segment.start.x - center.x;
        const auto y = segment.start.y - center.y;
        point = {center.x + x * std::cos(angle) - y * std::sin(angle),
                 center.y + x * std::sin(angle) + y * std::cos(angle)};
    }
    validate_finite(point);
    found->end_vertex_id = vertex_id;
    found->segment.end = point;
    found->segment.sweep_radians = segment.sweep_radians * fraction;
    result.segments.insert(found + 1, {std::move(second_id), std::move(vertex_id), original.end_vertex_id,
        {point, segment.end, segment.sweep_radians * (1 - fraction)}});
    validate_editable_boundary(result);
    return result;
}

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

IdentifiedBoundary reconstruct_boundary_arc(const IdentifiedBoundary& source,
                                             std::string_view id,
                                             const ConstructionReceipt& receipt) {
    validate_editable_boundary(source);
    const auto found = std::find_if(source.segments.begin(), source.segments.end(),
        [&](const auto& edge) { return edge.segment_id == id; });
    if (found == source.segments.end()) throw std::invalid_argument("Unknown segment ID");
    if (receipt.segment_id != id || receipt.start.x != found->segment.start.x ||
        receipt.start.y != found->segment.start.y || !receipt.chord_end ||
        receipt.chord_end->x != found->segment.end.x ||
        receipt.chord_end->y != found->segment.end.y) {
        throw std::invalid_argument("Arc construction receipt must retain the selected segment chord");
    }
    if (receipt.kind != BoundaryConstructionKind::arc_chord_angle &&
        receipt.kind != BoundaryConstructionKind::arc_chord_height &&
        receipt.kind != BoundaryConstructionKind::arc_chord_length) {
        throw std::invalid_argument("Arc reconstruction requires a chord arc receipt");
    }

    const auto replayed = replay_construction_receipt(
        receipt, ConstructionReplayContext{found->segment.start, std::nullopt, std::nullopt,
                                           default_geometry_tolerance_metres});
    if (replayed.segment.start.x != found->segment.start.x ||
        replayed.segment.start.y != found->segment.start.y ||
        replayed.segment.end.x != found->segment.end.x ||
        replayed.segment.end.y != found->segment.end.y) {
        throw std::invalid_argument("Reconstructed arc did not retain the exact segment chord");
    }
    auto result = source;
    const auto target = std::find_if(result.segments.begin(), result.segments.end(),
        [&](const auto& edge) { return edge.segment_id == id; });
    target->segment = replayed.segment;
    validate_editable_boundary(result);
    return result;
}

} // namespace sketch
