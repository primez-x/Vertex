#include "sketch/boundary_construction.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

constexpr std::uint32_t automatic_placement_version = 1;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return left.x == right.x && left.y == right.y;
}

bool same_segment(const Segment& left, const Segment& right) noexcept {
    return same_point(left.start, right.start) && same_point(left.end, right.end) &&
           left.sweep_radians == right.sweep_radians;
}

bool finite_point(Vec2 point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void require_point(Vec2 point, std::string_view label) {
    if (!finite_point(point)) invalid(std::string(label) + " must be finite");
}

void require_identifier(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must contain 1..128 supported ASCII characters");
    }
}

void require_options(const BoundaryAuthoringOptions& options) {
    if (!can_recognize_boundary_entity_type(options.default_boundary_type)) {
        invalid("default boundary entity type is unsupported");
    }
    if (!std::isfinite(options.geometry_tolerance_metres) ||
        !(options.geometry_tolerance_metres > 0.0)) {
        invalid("geometry tolerance must be finite and positive");
    }
    if (options.automatic_placement_version != automatic_placement_version) {
        invalid("only automatic dimension placement version one is supported");
    }
}

struct ArcGeometry {
    Vec2 center;
    double radius{};
    double start_angle{};
};

ArcGeometry arc_geometry(const Segment& segment) {
    if (segment.sweep_radians == 0.0) invalid("line segment has no arc geometry");
    const auto chord_x = segment.end.x - segment.start.x;
    const auto chord_y = segment.end.y - segment.start.y;
    const auto chord = std::hypot(chord_x, chord_y);
    if (!std::isfinite(chord) || !(chord > 0.0)) invalid("arc chord is degenerate");
    const auto half_sweep = segment.sweep_radians / 2.0;
    const auto sine = std::sin(std::abs(half_sweep));
    const auto tangent = std::tan(half_sweep);
    if (!(sine > 0.0) || tangent == 0.0 || !std::isfinite(tangent)) {
        invalid("arc sweep cannot be represented");
    }
    const Vec2 midpoint{(segment.start.x + segment.end.x) / 2.0,
                        (segment.start.y + segment.end.y) / 2.0};
    const Vec2 left_normal{-chord_y / chord, chord_x / chord};
    const auto offset = chord / (2.0 * tangent);
    const Vec2 center{midpoint.x + left_normal.x * offset,
                      midpoint.y + left_normal.y * offset};
    const auto radius = chord / (2.0 * sine);
    if (!finite_point(center) || !std::isfinite(radius)) invalid("arc geometry is not finite");
    return {center, radius,
            std::atan2(segment.start.y - center.y, segment.start.x - center.x)};
}

Vec2 arc_point_at(const Segment& segment, double parameter) {
    const auto arc = arc_geometry(segment);
    const auto angle = arc.start_angle + segment.sweep_radians * parameter;
    return {arc.center.x + arc.radius * std::cos(angle),
            arc.center.y + arc.radius * std::sin(angle)};
}

Vec2 automatic_dimension_position(const Segment& segment) {
    const auto length = segment_length(segment);
    if (!std::isfinite(length) || !(length > 0.0)) invalid("cannot place a degenerate dimension");
    double tangent = 0.0;
    Vec2 midpoint{};
    if (segment.sweep_radians == 0.0) {
        midpoint = {(segment.start.x + segment.end.x) / 2.0,
                    (segment.start.y + segment.end.y) / 2.0};
        tangent = std::atan2(segment.end.y - segment.start.y,
                             segment.end.x - segment.start.x);
    } else {
        midpoint = arc_point_at(segment, 0.5);
        const auto arc = arc_geometry(segment);
        const auto middle_angle = arc.start_angle + segment.sweep_radians * 0.5;
        tangent = middle_angle + (segment.sweep_radians > 0.0 ? std::numbers::pi / 2.0
                                                               : -std::numbers::pi / 2.0);
    }
    const auto offset = std::max(0.25, length * 0.1);
    const Vec2 result{midpoint.x - std::sin(tangent) * offset,
                      midpoint.y + std::cos(tangent) * offset};
    require_point(result, "automatic dimension position");
    return result;
}

bool valid_classification(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(), [](unsigned char character) {
               return std::iscntrl(character) != 0;
           });
}

void validate_dimension(const BoundaryDimension& dimension,
                        const AcceptedBoundaryChain& source,
                        const std::set<std::string, std::less<>>& segment_ids,
                        std::set<std::string, std::less<>>& dimension_ids,
                        std::set<std::string, std::less<>>& dimension_targets,
                        const std::vector<IdentifiedSegment>& replayed_segments) {
    require_identifier(dimension.id, "dimension identity");
    if (!dimension_ids.insert(dimension.id).second) invalid("duplicate dimension identity");
    if (dimension.boundary_id != source.boundary.id) {
        invalid("dimension refers to a different boundary identity");
    }
    if (segment_ids.find(dimension.segment_id) == segment_ids.end()) {
        invalid("dimension refers to an unknown segment identity");
    }
    if (!dimension_targets.insert(dimension.segment_id).second) {
        invalid("multiple dimensions refer to the same segment identity");
    }
    require_point(dimension.text_position, "dimension position");

    const auto found = std::find_if(
        replayed_segments.begin(), replayed_segments.end(), [&](const IdentifiedSegment& edge) {
            return edge.segment_id == dimension.segment_id;
        });
    if (found == replayed_segments.end()) invalid("dimension target disappeared during replay");

    switch (dimension.placement) {
        case BoundaryDimensionPlacement::manual:
            if (dimension.automatic_placement_version.has_value()) {
                invalid("manual dimension cannot carry an automatic placement version");
            }
            break;
        case BoundaryDimensionPlacement::automatic:
            if (!dimension.automatic_placement_version.has_value() ||
                *dimension.automatic_placement_version != automatic_placement_version) {
                invalid("automatic dimension requires placement version one");
            }
            if (!same_point(dimension.text_position,
                            automatic_dimension_position(found->segment))) {
                invalid("automatic dimension position does not match version one placement");
            }
            break;
        default:
            invalid("dimension placement kind is unsupported");
    }
}

BoundaryConstructionRecord make_record(const AcceptedBoundaryChain& source) {
    if (source.receipts.size() != source.boundary.segments.size()) {
        invalid("boundary receipt count does not match segment count");
    }
    BoundaryConstructionRecord result;
    result.schema_version = boundary_receipt_latest_schema_version;
    result.anchor = source.anchor;
    result.boundary_id = source.boundary.id;
    result.edges.reserve(source.boundary.segments.size());
    for (std::size_t index = 0; index < source.boundary.segments.size(); ++index) {
        const auto& edge = source.boundary.segments[index];
        result.edges.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id,
                                source.receipts[index]});
    }
    return result;
}

}  // namespace

bool BoundaryConstructionReplay::operator==(
    const BoundaryConstructionReplay& other) const noexcept {
    if (!same_point(anchor, other.anchor) || boundary.id != other.boundary.id ||
        boundary.type != other.boundary.type || classification != other.classification ||
        dimensions != other.dimensions || receipts != other.receipts ||
        classified != other.classified || boundary.segments.size() != other.boundary.segments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < boundary.segments.size(); ++index) {
        const auto& left = boundary.segments[index];
        const auto& right = other.boundary.segments[index];
        if (left.segment_id != right.segment_id || left.start_vertex_id != right.start_vertex_id ||
            left.end_vertex_id != right.end_vertex_id || !same_segment(left.segment, right.segment)) {
            return false;
        }
    }
    return true;
}

BoundaryConstructionReplay replay_accepted_chain(const AcceptedBoundaryChain& source,
                                                  const BoundaryAuthoringOptions& options) {
    require_options(options);
    require_identifier(source.boundary.id, "boundary identity");
    if (!can_recognize_boundary_entity_type(source.boundary.type)) {
        invalid("boundary entity type is unsupported");
    }
    if (source.boundary.type != options.default_boundary_type) {
        invalid("replay boundary type does not match the captured construction context");
    }
    require_point(source.anchor, "boundary anchor");
    if (source.classified) {
        if (!valid_classification(source.classification)) {
            invalid("classified boundary has no valid area classification");
        }
    } else if (!source.classification.empty()) {
        invalid("unclassified boundary cannot carry an area classification");
    }

    const auto record = make_record(source);
    const auto lower_replay = replay_boundary_construction(
        record, options.geometry_tolerance_metres);
    std::set<std::string, std::less<>> segment_ids;
    for (const auto& edge : lower_replay.edges) segment_ids.insert(edge.segment_id);

    std::vector<IdentifiedSegment> replayed_segments;
    replayed_segments.reserve(lower_replay.edges.size());
    for (const auto& edge : lower_replay.edges) {
        replayed_segments.push_back(
            {edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, edge.segment});
    }

    std::set<std::string, std::less<>> dimension_ids;
    std::set<std::string, std::less<>> dimension_targets;
    for (const auto& dimension : source.dimensions) {
        validate_dimension(dimension, source, segment_ids, dimension_ids, dimension_targets,
                           replayed_segments);
    }

    BoundaryConstructionReplay result;
    result.anchor = lower_replay.anchor;
    result.boundary = {source.boundary.id, source.boundary.type, std::move(replayed_segments)};
    result.classification = source.classification;
    result.dimensions = source.dimensions;
    result.receipts = lower_replay.receipts;
    result.classified = source.classified;
    return result;
}

void verify_accepted_chain(const AcceptedBoundaryChain& source,
                           const BoundaryAuthoringOptions& options) {
    const auto replay = replay_accepted_chain(source, options);
    if (!(replay == BoundaryConstructionReplay{source.anchor, source.boundary,
                                                source.classification, source.dimensions,
                                                source.receipts, source.classified})) {
        invalid("accepted boundary differs from authoritative receipt replay");
    }
}

nlohmann::json boundary_construction_envelope(const AcceptedBoundaryChain& source,
                                              const BoundaryAuthoringOptions& options) {
    verify_accepted_chain(source, options);
    return encode_boundary_receipt_envelope(make_record(source));
}

}  // namespace sketch
