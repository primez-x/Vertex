#include "sketch/geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {

constexpr double full_turn = 2.0 * std::numbers::pi;

Vec2 operator+(Vec2 left, Vec2 right) {
    return {left.x + right.x, left.y + right.y};
}

Vec2 operator-(Vec2 left, Vec2 right) {
    return {left.x - right.x, left.y - right.y};
}

Vec2 operator*(Vec2 point, double scale) {
    return {point.x * scale, point.y * scale};
}

double dot(Vec2 left, Vec2 right) {
    return left.x * right.x + left.y * right.y;
}

double cross(Vec2 left, Vec2 right) {
    return left.x * right.y - left.y * right.x;
}

double length(Vec2 vector) {
    return std::hypot(vector.x, vector.y);
}

double distance(Vec2 left, Vec2 right) {
    return length(left - right);
}

bool finite(Vec2 point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

struct ArcGeometry {
    Vec2 center;
    double radius;
    double start_angle;
    double sweep;
};

void require_finite_segment(const Segment& segment) {
    if (!finite(segment.start) || !finite(segment.end) ||
        !std::isfinite(segment.sweep_radians)) {
        throw std::invalid_argument("segment contains a non-finite value");
    }
    const auto chord = segment.end - segment.start;
    if (!finite(chord)) {
        throw std::invalid_argument("segment chord exceeds numeric range");
    }
    const auto chord_length = length(chord);
    if (!std::isfinite(chord_length)) {
        throw std::invalid_argument("segment length exceeds numeric range");
    }
    if (!(chord_length > 0.0)) {
        throw std::invalid_argument("segment endpoints must be distinct");
    }
    if (segment.sweep_radians != 0.0 &&
        !(std::abs(segment.sweep_radians) < full_turn)) {
        throw std::invalid_argument("arc sweep magnitude must be less than two pi");
    }
}

ArcGeometry arc_geometry(const Segment& segment) {
    require_finite_segment(segment);
    if (segment.sweep_radians == 0.0) {
        throw std::invalid_argument("line segment has no arc geometry");
    }

    const auto chord = segment.end - segment.start;
    const auto chord_length = length(chord);
    const auto half_sweep = segment.sweep_radians / 2.0;
    const auto sine = std::sin(std::abs(half_sweep));
    const auto tangent = std::tan(half_sweep);
    if (!(sine > 0.0) || tangent == 0.0 || !std::isfinite(tangent)) {
        throw std::invalid_argument("arc sweep cannot be represented");
    }

    const auto midpoint = (segment.start + segment.end) * 0.5;
    const Vec2 left_normal{-chord.y / chord_length, chord.x / chord_length};
    const auto center = midpoint + left_normal * (chord_length / (2.0 * tangent));
    const auto radius = chord_length / (2.0 * sine);
    if (!finite(center) || !std::isfinite(radius)) {
        throw std::invalid_argument("arc geometry exceeds numeric range");
    }
    return {center, radius,
            std::atan2(segment.start.y - center.y, segment.start.x - center.x),
            segment.sweep_radians};
}

double positive_angle(double angle) {
    auto result = std::fmod(angle, full_turn);
    if (result < 0.0) {
        result += full_turn;
    }
    return result;
}

double arc_parameter(const ArcGeometry& arc, Vec2 point) {
    const auto angle = std::atan2(point.y - arc.center.y, point.x - arc.center.x);
    const auto travel = arc.sweep > 0.0 ? positive_angle(angle - arc.start_angle)
                                        : positive_angle(arc.start_angle - angle);
    return travel / std::abs(arc.sweep);
}

bool point_on_arc(const ArcGeometry& arc, Vec2 point, double tolerance) {
    const auto radial_error = std::abs(distance(point, arc.center) - arc.radius);
    if (radial_error > tolerance) {
        return false;
    }
    const auto parameter_tolerance = tolerance / std::max(arc.radius * std::abs(arc.sweep), tolerance);
    const auto parameter = arc_parameter(arc, point);
    return parameter <= 1.0 + parameter_tolerance;
}

bool point_strictly_inside_arc(const ArcGeometry& arc, Vec2 point, double tolerance) {
    if (!point_on_arc(arc, point, tolerance)) {
        return false;
    }
    const auto parameter_tolerance = tolerance / std::max(arc.radius * std::abs(arc.sweep), tolerance);
    const auto parameter = arc_parameter(arc, point);
    return parameter > parameter_tolerance && parameter < 1.0 - parameter_tolerance;
}

Vec2 point_at(const ArcGeometry& arc, double parameter) {
    const auto angle = arc.start_angle + arc.sweep * parameter;
    return arc.center + Vec2{std::cos(angle), std::sin(angle)} * arc.radius;
}

enum class IntersectionKind { none, points, overlap, indeterminate };

struct IntersectionResult {
    IntersectionKind kind{IntersectionKind::none};
    std::array<Vec2, 2> points{};
    std::size_t point_count{};
};

void add_point(IntersectionResult& result, Vec2 point, double tolerance) {
    for (std::size_t index = 0; index < result.point_count; ++index) {
        if (distance(result.points[index], point) <= tolerance) {
            return;
        }
    }
    if (result.point_count < result.points.size()) {
        result.points[result.point_count++] = point;
        result.kind = IntersectionKind::points;
    } else {
        result.kind = IntersectionKind::indeterminate;
    }
}

bool bounding_boxes_overlap(const Segment& left, const Segment& right, double tolerance) {
    const auto left_min_x = std::min(left.start.x, left.end.x) - tolerance;
    const auto left_max_x = std::max(left.start.x, left.end.x) + tolerance;
    const auto left_min_y = std::min(left.start.y, left.end.y) - tolerance;
    const auto left_max_y = std::max(left.start.y, left.end.y) + tolerance;
    const auto right_min_x = std::min(right.start.x, right.end.x);
    const auto right_max_x = std::max(right.start.x, right.end.x);
    const auto right_min_y = std::min(right.start.y, right.end.y);
    const auto right_max_y = std::max(right.start.y, right.end.y);
    return left_min_x <= right_max_x && right_min_x <= left_max_x &&
           left_min_y <= right_max_y && right_min_y <= left_max_y;
}

IntersectionResult intersect_lines(const Segment& left, const Segment& right, double tolerance) {
    IntersectionResult result;
    const auto p = left.start;
    const auto q = right.start;
    const auto r = left.end - left.start;
    const auto s = right.end - right.start;
    const auto r_length = length(r);
    const auto s_length = length(s);
    const auto denominator = cross(r, s);
    const auto parallel_threshold = tolerance * (r_length + s_length);
    const auto q_minus_p = q - p;

    if (std::abs(denominator) <= parallel_threshold) {
        if (std::abs(cross(q_minus_p, r)) > tolerance * r_length) {
            // Overlapping axis-aligned boxes do not imply uncertainty for rotated
            // parallel edges. Both endpoints strictly on one side of the line
            // prove separation, even when the directions are only near parallel.
            // Keep the conservative fallback when roundoff or tolerance could
            // change either endpoint's side.
            const auto side = [&](Vec2 point) {
                const auto offset = point - p;
                const auto first_product = offset.x * r.y;
                const auto second_product = offset.y * r.x;
                const auto determinant = first_product - second_product;
                const auto rounding_margin = 16.0 * std::numeric_limits<double>::epsilon() *
                    (std::abs(first_product) + std::abs(second_product)) +
                    16.0 * std::numeric_limits<double>::denorm_min();
                const auto margin = tolerance * r_length + rounding_margin;
                if (!std::isfinite(determinant) || !std::isfinite(margin)) return 0;
                if (determinant > margin) return 1;
                if (determinant < -margin) return -1;
                return 0;
            };
            const auto start_side = side(right.start);
            if (start_side != 0 && start_side == side(right.end)) {
                return result;
            }
            if (bounding_boxes_overlap(left, right, tolerance)) {
                result.kind = IntersectionKind::indeterminate;
            }
            return result;
        }

        const auto r_squared = dot(r, r);
        const auto first = dot(q_minus_p, r) / r_squared;
        const auto second = first + dot(s, r) / r_squared;
        const auto low = std::max(0.0, std::min(first, second));
        const auto high = std::min(1.0, std::max(first, second));
        const auto parameter_tolerance = tolerance / r_length;
        if (high < low - parameter_tolerance) {
            return result;
        }
        if (high > low + parameter_tolerance) {
            result.kind = IntersectionKind::overlap;
            return result;
        }
        add_point(result, p + r * std::clamp((low + high) * 0.5, 0.0, 1.0), tolerance);
        return result;
    }

    const auto left_parameter = cross(q_minus_p, s) / denominator;
    const auto right_parameter = cross(q_minus_p, r) / denominator;
    const auto left_tolerance = tolerance / r_length;
    const auto right_tolerance = tolerance / s_length;
    if (left_parameter >= -left_tolerance && left_parameter <= 1.0 + left_tolerance &&
        right_parameter >= -right_tolerance && right_parameter <= 1.0 + right_tolerance) {
        add_point(result, p + r * std::clamp(left_parameter, 0.0, 1.0), tolerance);
    }
    return result;
}

IntersectionResult intersect_line_arc(const Segment& line, const Segment& arc_segment,
                                      double tolerance) {
    IntersectionResult result;
    const auto arc = arc_geometry(arc_segment);
    const auto direction = line.end - line.start;
    const auto line_length = length(direction);
    const auto direction_squared = dot(direction, direction);
    const auto center_offset = arc.center - line.start;
    const auto projection = dot(center_offset, direction) / direction_squared;
    const auto nearest = line.start + direction * projection;
    const auto nearest_distance = distance(nearest, arc.center);
    const auto square_limit = std::sqrt(std::numeric_limits<double>::max());
    if (!std::isfinite(direction_squared) || !std::isfinite(nearest_distance) ||
        arc.radius > square_limit || nearest_distance > square_limit) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }
    const auto radial_delta = arc.radius * arc.radius - nearest_distance * nearest_distance;
    const auto delta_tolerance = tolerance * (2.0 * arc.radius + tolerance);
    if (radial_delta < -delta_tolerance) {
        return result;
    }

    const auto parameter_tolerance = tolerance / line_length;
    const auto add_parameter = [&](double parameter) {
        if (parameter < -parameter_tolerance || parameter > 1.0 + parameter_tolerance) {
            return;
        }
        const auto point = line.start + direction * std::clamp(parameter, 0.0, 1.0);
        if (point_on_arc(arc, point, tolerance)) {
            add_point(result, point, tolerance);
        }
    };

    if (radial_delta <= delta_tolerance) {
        add_parameter(projection);
        return result;
    }

    const auto offset = std::sqrt(radial_delta / direction_squared);
    add_parameter(projection - offset);
    add_parameter(projection + offset);
    return result;
}

IntersectionResult intersect_coincident_arcs(const Segment& left_segment,
                                             const ArcGeometry& left,
                                             const Segment& right_segment,
                                             const ArcGeometry& right,
                                             double tolerance) {
    IntersectionResult result;
    const std::array<Vec2, 6> candidates{
        left_segment.start, left_segment.end, point_at(left, 0.5),
        right_segment.start, right_segment.end, point_at(right, 0.5),
    };
    for (const auto point : candidates) {
        if (point_strictly_inside_arc(left, point, tolerance) &&
            point_strictly_inside_arc(right, point, tolerance)) {
            result.kind = IntersectionKind::overlap;
            return result;
        }
    }

    for (const auto point : std::array<Vec2, 4>{left_segment.start, left_segment.end,
                                                right_segment.start, right_segment.end}) {
        if (point_on_arc(left, point, tolerance) && point_on_arc(right, point, tolerance)) {
            add_point(result, point, tolerance);
        }
    }
    return result;
}

IntersectionResult intersect_arcs(const Segment& left_segment, const Segment& right_segment,
                                  double tolerance) {
    IntersectionResult result;
    const auto left = arc_geometry(left_segment);
    const auto right = arc_geometry(right_segment);
    const auto centers = right.center - left.center;
    const auto center_distance = length(centers);

    if (center_distance <= tolerance && std::abs(left.radius - right.radius) <= tolerance) {
        return intersect_coincident_arcs(left_segment, left, right_segment, right, tolerance);
    }
    if (center_distance <= tolerance) {
        return result;
    }

    const auto square_limit = std::sqrt(std::numeric_limits<double>::max());
    if (!std::isfinite(center_distance) || left.radius > square_limit ||
        right.radius > square_limit || center_distance > square_limit) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }

    if (center_distance > left.radius + right.radius + tolerance ||
        center_distance < std::abs(left.radius - right.radius) - tolerance) {
        return result;
    }

    const auto along = (left.radius * left.radius - right.radius * right.radius +
                        center_distance * center_distance) /
                       (2.0 * center_distance);
    const auto height_squared = left.radius * left.radius - along * along;
    const auto height_tolerance = tolerance * (2.0 * left.radius + tolerance);
    if (height_squared < -height_tolerance) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }

    const auto base = left.center + centers * (along / center_distance);
    const auto add_if_on_both = [&](Vec2 point) {
        if (point_on_arc(left, point, tolerance) && point_on_arc(right, point, tolerance)) {
            add_point(result, point, tolerance);
        }
    };
    if (height_squared <= height_tolerance) {
        add_if_on_both(base);
        return result;
    }

    const auto height = std::sqrt(height_squared);
    const Vec2 perpendicular{-centers.y / center_distance, centers.x / center_distance};
    add_if_on_both(base + perpendicular * height);
    add_if_on_both(base - perpendicular * height);
    return result;
}

IntersectionResult intersect_segments(const Segment& left, const Segment& right,
                                      double tolerance) {
    const auto left_is_arc = left.sweep_radians != 0.0;
    const auto right_is_arc = right.sweep_radians != 0.0;
    if (!left_is_arc && !right_is_arc) {
        return intersect_lines(left, right, tolerance);
    }
    if (!left_is_arc) {
        return intersect_line_arc(left, right, tolerance);
    }
    if (!right_is_arc) {
        return intersect_line_arc(right, left, tolerance);
    }
    return intersect_arcs(left, right, tolerance);
}

void add_diagnostic(std::vector<BoundaryDiagnostic>& diagnostics, BoundaryIssue issue,
                    std::size_t segment_index, std::optional<std::size_t> other_segment_index,
                    const char* message) {
    diagnostics.push_back({issue, segment_index, other_segment_index, message});
}

bool is_expected_adjacent_intersection(const Boundary& boundary, std::size_t left_index,
                                       std::size_t right_index, Vec2 point, double tolerance) {
    if (right_index == left_index + 1 &&
        distance(boundary[left_index].end, boundary[right_index].start) <= tolerance &&
        distance(point, boundary[left_index].end) <= tolerance) {
        return true;
    }
    if (left_index == 0 && right_index + 1 == boundary.size() &&
        distance(boundary[right_index].end, boundary[left_index].start) <= tolerance &&
        distance(point, boundary[left_index].start) <= tolerance) {
        return true;
    }
    return false;
}

double stable_theta_minus_sine(double theta) {
    if (std::abs(theta) >= 1e-3) {
        return theta - std::sin(theta);
    }
    const auto theta_squared = theta * theta;
    return theta * theta_squared *
           (1.0 / 6.0 - theta_squared / 120.0 + theta_squared * theta_squared / 5040.0);
}

double checked_arc_length(const Segment& segment) {
    const auto chord_length = length(segment.end - segment.start);
    const auto half_sweep = std::abs(segment.sweep_radians) / 2.0;
    const auto sine = std::sin(half_sweep);
    if (!(sine > 0.0)) {
        throw std::invalid_argument("arc sweep cannot be represented");
    }
    const auto scale = std::abs(segment.sweep_radians) / (2.0 * sine);
    const auto result = chord_length * scale;
    if (!std::isfinite(scale) || !std::isfinite(result)) {
        throw std::invalid_argument("arc length exceeds numeric range");
    }
    return result;
}

double checked_arc_twice_area_correction(const Segment& segment) {
    const auto theta = segment.sweep_radians;
    const auto chord_length = length(segment.end - segment.start);
    double scale = 0.0;
    if (std::abs(theta) < 1e-3) {
        const auto theta_squared = theta * theta;
        scale = theta / 6.0 *
                (1.0 + theta_squared / 30.0 + theta_squared * theta_squared / 840.0);
    } else {
        const auto sine = std::sin(std::abs(theta) / 2.0);
        scale = stable_theta_minus_sine(theta) / (4.0 * sine * sine);
    }
    const auto first_product = chord_length * scale;
    const auto result = chord_length * first_product;
    if (!std::isfinite(scale) || !std::isfinite(first_product) || !std::isfinite(result)) {
        throw std::invalid_argument("arc area exceeds numeric range");
    }
    return result;
}

bool is_closed_within(const Boundary& boundary, double tolerance) {
    if (boundary.empty()) {
        return false;
    }
    for (std::size_t index = 0; index + 1 < boundary.size(); ++index) {
        if (distance(boundary[index].end, boundary[index + 1].start) > tolerance) {
            return false;
        }
    }
    return distance(boundary.back().end, boundary.front().start) <= tolerance;
}

}  // namespace

Segment arc_from_chord_angle(Vec2 start, Vec2 end, double sweep_radians) {
    if (!finite(start) || !finite(end) || !std::isfinite(sweep_radians)) {
        throw std::invalid_argument("arc contains a non-finite value");
    }
    if (!(length(end - start) > 0.0)) {
        throw std::invalid_argument("arc endpoints must be distinct");
    }
    if (sweep_radians == 0.0 || !(std::abs(sweep_radians) < full_turn)) {
        throw std::invalid_argument("arc sweep must be nonzero with magnitude less than two pi");
    }
    Segment result{start, end, sweep_radians};
    (void)arc_geometry(result);
    return result;
}

Segment arc_from_chord_height(Vec2 start, Vec2 end, double signed_height) {
    if (!finite(start) || !finite(end) || !std::isfinite(signed_height)) {
        throw std::invalid_argument("arc contains a non-finite value");
    }
    const auto chord_length = length(end - start);
    if (!(chord_length > 0.0)) {
        throw std::invalid_argument("arc endpoints must be distinct");
    }
    if (signed_height == 0.0) {
        throw std::invalid_argument("arc chord height must be nonzero");
    }
    const auto sweep = 4.0 * std::atan2(signed_height, chord_length / 2.0);
    return arc_from_chord_angle(start, end, sweep);
}

Segment arc_from_chord_arc_length(Vec2 start, Vec2 end, double arc_length_metres, bool clockwise) {
    if (!finite(start) || !finite(end) || !std::isfinite(arc_length_metres)) {
        throw std::invalid_argument("measured arc contains a non-finite value");
    }
    const double chord = length(end - start);
    if (!(chord > 0) || !std::isfinite(chord) || !(arc_length_metres > chord)) {
        throw std::invalid_argument("arc length must exceed its nonzero chord length");
    }
    // L/c = theta/(2*sin(theta/2)) is strictly increasing on (0,2*pi).
    // Compare its excess over 1 with a series near zero to avoid cancellation
    // for shallow curves whose measured arc is barely longer than the chord.
    const auto excess = [](double theta) {
        if (theta < 0.01) {
            const double square = theta * theta;
            return square * (1.0 / 24.0 + square * (7.0 / 5760.0
                + square * (31.0 / 967680.0 + square * (127.0 / 154828800.0))));
        }
        return theta / (2.0 * std::sin(theta * 0.5)) - 1.0;
    };
    const double target = (arc_length_metres - chord) / chord;
    double low = 0.0;
    double high = std::nextafter(full_turn, 0.0);
    if (!std::isfinite(target) || target > excess(high)) {
        throw std::invalid_argument("measured arc sweep is outside representable precision");
    }
    for (int iteration = 0; iteration < 160; ++iteration) {
        const double middle = low + (high - low) * 0.5;
        if (middle == low || middle == high) break;
        if (excess(middle) < target) low = middle;
        else high = middle;
    }
    const double sweep = low + (high - low) * 0.5;
    auto result = arc_from_chord_angle(start, end, clockwise ? -sweep : sweep);
    if (std::abs(segment_length(result) - arc_length_metres) > arc_length_metres * 1e-12) {
        throw std::invalid_argument("measured arc cannot preserve the entered length at this precision");
    }
    return result;
}

Segment arc_from_start_tangent(Vec2 start, double tangent_radians, double arc_length_metres,
                               double sweep_radians) {
    if (!finite(start) || !std::isfinite(tangent_radians) || !std::isfinite(arc_length_metres)
        || !(arc_length_metres > 0) || !std::isfinite(sweep_radians)
        || sweep_radians == 0 || !(std::abs(sweep_radians) < full_turn)) {
        throw std::invalid_argument("tangent arc needs finite positive length and a nonzero sweep below a full turn");
    }
    const double half = sweep_radians * 0.5;
    const double chord = arc_length_metres * (std::sin(half) / half);
    const double direction = std::remainder(tangent_radians, full_turn) + half;
    const Vec2 end{start.x + chord * std::cos(direction), start.y + chord * std::sin(direction)};
    auto result = arc_from_chord_angle(start, end, sweep_radians);
    if (std::abs(segment_length(result) - arc_length_metres) > std::max(1e-7, arc_length_metres * 1e-12)) {
        throw std::invalid_argument("tangent arc loses the entered length at these coordinate magnitudes");
    }
    return result;
}

Vec2 transform_point(Vec2 point, const PlanarTransform& transform) {
    if (!finite(point) || !finite(transform.pivot) || !finite(transform.offset) ||
        !std::isfinite(transform.rotation_radians))
        throw std::invalid_argument("planar transform requires finite points and parameters");
    if (transform.rotation_radians != 0.0) {
        const auto x = point.x - transform.pivot.x;
        const auto y = point.y - transform.pivot.y;
        const auto cosine = std::cos(transform.rotation_radians);
        const auto sine = std::sin(transform.rotation_radians);
        point = {transform.pivot.x + x * cosine - y * sine,
                 transform.pivot.y + x * sine + y * cosine};
    }
    if (transform.flip_horizontal) point.x = transform.pivot.x - (point.x - transform.pivot.x);
    if (transform.flip_vertical) point.y = transform.pivot.y - (point.y - transform.pivot.y);
    if (transform.offset.x != 0.0) point.x += transform.offset.x;
    if (transform.offset.y != 0.0) point.y += transform.offset.y;
    if (!finite(point)) throw std::invalid_argument("planar transform exceeds numeric range");
    return point;
}

Segment transform_segment(const Segment& segment, const PlanarTransform& transform) {
    require_finite_segment(segment);
    return {transform_point(segment.start, transform), transform_point(segment.end, transform),
            transform.flip_horizontal != transform.flip_vertical && segment.sweep_radians != 0.0
                ? -segment.sweep_radians : segment.sweep_radians};
}

Bounds2 segment_bounds(const Segment& segment) {
    require_finite_segment(segment);
    Bounds2 result{{std::min(segment.start.x, segment.end.x), std::min(segment.start.y, segment.end.y)},
                   {std::max(segment.start.x, segment.end.x), std::max(segment.start.y, segment.end.y)}};
    if (segment.sweep_radians == 0.0) return result;
    const auto arc = arc_geometry(segment);
    const auto chord = segment.end - segment.start;
    const auto chord_length = length(chord);
    const Vec2 normal{-chord.y / chord_length, chord.x / chord_length};
    const Vec2 midpoint{std::midpoint(segment.start.x, segment.end.x),
                        std::midpoint(segment.start.y, segment.end.y)};
    const auto center_distance = chord_length / (2.0 * std::tan(segment.sweep_radians / 2.0));
    const auto half = std::abs(segment.sweep_radians) / 2.0;
    const auto small_sine = half <= std::numbers::pi / 2.0 ?
        std::sin(half / 2.0) : std::cos(half / 2.0);
    const auto extremum = [&](double middle, double component, double other, double direction) {
        const auto offset = component * center_distance;
        if (offset * direction < 0.0) {
            // Avoid subtracting nearly equal radius/center terms for shallow
            // arcs. 1-|normal| = other^2/(1+|normal|), and
            // 1-|cos(half)| uses the smaller sine/cosine half-angle square.
            const auto deficit = arc.radius * other * other / (1.0 + std::abs(component)) +
                arc.radius * std::abs(component) * small_sine * (2.0 * small_sine);
            return middle + direction * deficit;
        }
        return (middle + offset) + direction * arc.radius;
    };
    constexpr std::array<Vec2, 4> directions{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
    for (std::size_t index = 0; index < directions.size(); ++index) {
        const auto angle = static_cast<double>(index) * std::numbers::pi / 2.0;
        const auto travel = arc.sweep > 0.0 ? positive_angle(angle - arc.start_angle) :
                                             positive_angle(arc.start_angle - angle);
        if (travel > std::abs(arc.sweep)) continue;
        const auto direction = directions[index];
        const Vec2 point{direction.x == 0.0 ? arc.center.x :
                            extremum(midpoint.x, normal.x, normal.y, direction.x),
                         direction.y == 0.0 ? arc.center.y :
                            extremum(midpoint.y, normal.y, normal.x, direction.y)};
        if (!finite(point)) throw std::invalid_argument("arc bounds exceed numeric range");
        result.minimum.x = std::min(result.minimum.x, point.x);
        result.minimum.y = std::min(result.minimum.y, point.y);
        result.maximum.x = std::max(result.maximum.x, point.x);
        result.maximum.y = std::max(result.maximum.y, point.y);
    }
    return result;
}

Bounds2 boundary_bounds(const Boundary& boundary) {
    if (boundary.empty()) throw std::invalid_argument("empty boundary has no bounds");
    auto result = segment_bounds(boundary.front());
    for (std::size_t index = 1; index < boundary.size(); ++index) {
        const auto bounds = segment_bounds(boundary[index]);
        result.minimum.x = std::min(result.minimum.x, bounds.minimum.x);
        result.minimum.y = std::min(result.minimum.y, bounds.minimum.y);
        result.maximum.x = std::max(result.maximum.x, bounds.maximum.x);
        result.maximum.y = std::max(result.maximum.y, bounds.maximum.y);
    }
    return result;
}

double segment_length(const Segment& segment) {
    require_finite_segment(segment);
    if (segment.sweep_radians == 0.0) {
        return length(segment.end - segment.start);
    }
    return checked_arc_length(segment);
}

double perimeter(const Boundary& boundary) {
    double sum = 0.0;
    double compensation = 0.0;
    for (const auto& segment : boundary) {
        const auto value = segment_length(segment);
        const auto adjusted = value - compensation;
        const auto next = sum + adjusted;
        if (!std::isfinite(adjusted) || !std::isfinite(next)) {
            throw std::invalid_argument("perimeter exceeds numeric range");
        }
        compensation = (next - sum) - adjusted;
        if (!std::isfinite(compensation)) {
            throw std::invalid_argument("perimeter compensation exceeds numeric range");
        }
        sum = next;
    }
    return sum;
}

double signed_area(const Boundary& boundary) {
    double twice_area = 0.0;
    double compensation = 0.0;
    const auto origin = is_closed_within(boundary, default_geometry_tolerance_metres)
                            ? boundary.front().start
                            : Vec2{};
    for (const auto& segment : boundary) {
        require_finite_segment(segment);
        const auto local_start = segment.start - origin;
        const auto local_end = segment.end - origin;
        if (!finite(local_start) || !finite(local_end)) {
            throw std::invalid_argument("area coordinates exceed numeric range");
        }
        auto contribution = cross(local_start, local_end);
        if (!std::isfinite(contribution)) {
            throw std::invalid_argument("area exceeds numeric range");
        }
        if (segment.sweep_radians != 0.0) {
            contribution += checked_arc_twice_area_correction(segment);
            if (!std::isfinite(contribution)) {
                throw std::invalid_argument("area exceeds numeric range");
            }
        }
        const auto adjusted = contribution - compensation;
        const auto next = twice_area + adjusted;
        if (!std::isfinite(adjusted) || !std::isfinite(next)) {
            throw std::invalid_argument("area sum exceeds numeric range");
        }
        compensation = (next - twice_area) - adjusted;
        if (!std::isfinite(compensation)) {
            throw std::invalid_argument("area compensation exceeds numeric range");
        }
        twice_area = next;
    }
    return twice_area / 2.0;
}

std::vector<BoundaryDiagnostic> validate_boundary(const Boundary& boundary,
                                                   double tolerance_metres) {
    if (!std::isfinite(tolerance_metres) || !(tolerance_metres > 0.0)) {
        throw std::invalid_argument("geometry tolerance must be finite and positive");
    }

    std::vector<BoundaryDiagnostic> diagnostics;
    if (boundary.empty()) {
        add_diagnostic(diagnostics, BoundaryIssue::empty_boundary, 0, std::nullopt,
                       "boundary has no segments");
        return diagnostics;
    }

    std::vector<bool> valid(boundary.size(), true);
    std::vector<bool> numeric_overflow_reported(boundary.size(), false);
    const auto report_numeric_overflow = [&](std::size_t index, const char* message) {
        if (!numeric_overflow_reported[index]) {
            add_diagnostic(diagnostics, BoundaryIssue::numeric_overflow, index, std::nullopt,
                           message);
            numeric_overflow_reported[index] = true;
        }
        valid[index] = false;
    };
    for (std::size_t index = 0; index < boundary.size(); ++index) {
        const auto& segment = boundary[index];
        if (!finite(segment.start) || !finite(segment.end) ||
            !std::isfinite(segment.sweep_radians)) {
            add_diagnostic(diagnostics, BoundaryIssue::non_finite, index, std::nullopt,
                           "segment contains a non-finite value");
            valid[index] = false;
            continue;
        }
        const auto chord = segment.end - segment.start;
        if (!finite(chord)) {
            report_numeric_overflow(index, "segment chord exceeds numeric range");
            continue;
        }
        const auto chord_length = length(chord);
        if (!std::isfinite(chord_length)) {
            report_numeric_overflow(index, "segment length exceeds numeric range");
            continue;
        }
        if (chord_length <= tolerance_metres) {
            add_diagnostic(diagnostics, BoundaryIssue::degenerate_segment, index, std::nullopt,
                           "segment chord is at or below the geometry tolerance");
            valid[index] = false;
            continue;
        }
        if (segment.sweep_radians != 0.0 &&
            !(std::abs(segment.sweep_radians) < full_turn)) {
            add_diagnostic(diagnostics, BoundaryIssue::invalid_sweep, index, std::nullopt,
                           "arc sweep magnitude must be less than two pi");
            valid[index] = false;
            continue;
        }
        if (segment.sweep_radians != 0.0) {
            try {
                (void)arc_geometry(segment);
                (void)checked_arc_length(segment);
                (void)checked_arc_twice_area_correction(segment);
            } catch (const std::invalid_argument&) {
                report_numeric_overflow(index, "derived arc geometry exceeds numeric range");
            }
        }
    }

    double perimeter_sum = 0.0;
    double area_sum = 0.0;
    const auto area_origin = is_closed_within(boundary, tolerance_metres)
                                 ? boundary.front().start
                                 : Vec2{};
    for (std::size_t index = 0; index < boundary.size(); ++index) {
        if (!valid[index]) {
            continue;
        }
        const auto& segment = boundary[index];
        const auto next_perimeter = perimeter_sum + segment_length(segment);
        if (!std::isfinite(next_perimeter)) {
            report_numeric_overflow(index, "boundary perimeter exceeds numeric range");
            continue;
        }
        perimeter_sum = next_perimeter;

        const auto local_start = segment.start - area_origin;
        const auto local_end = segment.end - area_origin;
        if (!finite(local_start) || !finite(local_end)) {
            report_numeric_overflow(index, "boundary area coordinates exceed numeric range");
            continue;
        }
        auto contribution = cross(local_start, local_end);
        if (segment.sweep_radians != 0.0) {
            contribution += checked_arc_twice_area_correction(segment);
        }
        const auto next_area = area_sum + contribution;
        if (!std::isfinite(contribution) || !std::isfinite(next_area)) {
            report_numeric_overflow(index, "boundary area exceeds numeric range");
            continue;
        }
        area_sum = next_area;
    }

    for (std::size_t index = 0; index + 1 < boundary.size(); ++index) {
        if (finite(boundary[index].end) && finite(boundary[index + 1].start) &&
            distance(boundary[index].end, boundary[index + 1].start) > tolerance_metres) {
            add_diagnostic(diagnostics, BoundaryIssue::disconnected, index, index + 1,
                           "consecutive segments do not share an endpoint");
        }
    }
    if (finite(boundary.back().end) && finite(boundary.front().start) &&
        distance(boundary.back().end, boundary.front().start) > tolerance_metres) {
        add_diagnostic(diagnostics, BoundaryIssue::open_boundary, boundary.size() - 1, 0,
                       "last segment does not close to the first segment");
    }

    for (std::size_t left = 0; left < boundary.size(); ++left) {
        if (!valid[left]) {
            continue;
        }
        for (std::size_t right = left + 1; right < boundary.size(); ++right) {
            if (!valid[right]) {
                continue;
            }
            IntersectionResult intersections;
            try {
                intersections = intersect_segments(boundary[left], boundary[right],
                                                   tolerance_metres);
            } catch (const std::invalid_argument&) {
                intersections.kind = IntersectionKind::indeterminate;
            }
            if (intersections.kind == IntersectionKind::overlap) {
                add_diagnostic(diagnostics, BoundaryIssue::overlapping_segments, left, right,
                               "segments overlap over a nonzero length");
                continue;
            }
            if (intersections.kind == IntersectionKind::indeterminate) {
                add_diagnostic(diagnostics, BoundaryIssue::indeterminate_intersection, left, right,
                               "segment intersection is numerically indeterminate");
                continue;
            }
            for (std::size_t point_index = 0; point_index < intersections.point_count;
                 ++point_index) {
                if (!is_expected_adjacent_intersection(boundary, left, right,
                                                       intersections.points[point_index],
                                                       tolerance_metres)) {
                    add_diagnostic(diagnostics, BoundaryIssue::self_intersection, left, right,
                                   "segments meet away from their expected adjacent endpoint");
                    break;
                }
            }
        }
    }
    return diagnostics;
}

std::optional<std::string> validate_boundary_holes(
    const Boundary& outer, const std::vector<Boundary>& holes, double tolerance_metres) {
    // Reuse the same analytical intersection predicates as boundary validation;
    // no tessellation is involved in either topology or the eventual quantities.
    try {
        if (const auto issues = validate_boundary(outer, tolerance_metres); !issues.empty())
            return "outer boundary is invalid: " + issues.front().message;
        for (std::size_t index = 0; index < holes.size(); ++index) {
            if (const auto issues = validate_boundary(holes[index], tolerance_metres); !issues.empty())
                return "hole " + std::to_string(index + 1) + " is invalid: " + issues.front().message;
        }
        const auto boundaries_meet = [&](const Boundary& first, const Boundary& second) {
            for (const auto& left : first) {
                for (const auto& right : second) {
                    if (intersect_segments(left, right, tolerance_metres).kind != IntersectionKind::none)
                        return true;
                }
            }
            return false;
        };
        // After excluding boundary intersections, one point locates the complete
        // connected hole. Split arcs at their Y extrema to count horizontal-ray
        // crossings on monotonic pieces, with half-open endpoint ownership.
        const auto contains = [](const Boundary& boundary, Vec2 point) {
            int winding = 0;
            const auto crossing = [&](Vec2 start, Vec2 end, const auto& crossing_x) {
                const bool upward = start.y <= point.y && end.y > point.y;
                const bool downward = end.y <= point.y && start.y > point.y;
                if ((upward || downward) && crossing_x() > point.x)
                    winding += upward ? 1 : -1;
            };
            for (const auto& segment : boundary) {
                if (segment.sweep_radians == 0.0) {
                    crossing(segment.start, segment.end, [&] {
                        const auto fraction = (point.y - segment.start.y) / (segment.end.y - segment.start.y);
                        const auto x = segment.start.x + fraction * (segment.end.x - segment.start.x);
                        if (!std::isfinite(x)) throw std::invalid_argument("unrepresentable ray crossing");
                        return x;
                    });
                    continue;
                }
                const auto arc = arc_geometry(segment);
                std::vector<double> cuts{0.0, 1.0};
                for (const double angle : {std::numbers::pi / 2.0, 3.0 * std::numbers::pi / 2.0}) {
                    const auto travel = arc.sweep > 0.0 ? positive_angle(angle - arc.start_angle)
                                                        : positive_angle(arc.start_angle - angle);
                    const auto parameter = travel / std::abs(arc.sweep);
                    if (parameter > 0.0 && parameter < 1.0) cuts.push_back(parameter);
                }
                std::sort(cuts.begin(), cuts.end());
                for (std::size_t index = 1; index < cuts.size(); ++index) {
                    const auto start = cuts[index - 1] == 0.0 ? segment.start : point_at(arc, cuts[index - 1]);
                    const auto end = cuts[index] == 1.0 ? segment.end : point_at(arc, cuts[index]);
                    crossing(start, end, [&] {
                        const auto dy = point.y - arc.center.y;
                        // Factored form avoids cancellation close to an extremum.
                        const auto dx = std::sqrt(std::max(0.0, (arc.radius - std::abs(dy)) *
                                                                       (arc.radius + std::abs(dy))));
                        const auto middle_angle = arc.start_angle + arc.sweep *
                            std::midpoint(cuts[index - 1], cuts[index]);
                        const auto x = arc.center.x + std::copysign(dx, std::cos(middle_angle));
                        if (!std::isfinite(x)) throw std::invalid_argument("unrepresentable arc ray crossing");
                        return x;
                    });
                }
            }
            return winding != 0;
        };
        for (std::size_t index = 0; index < holes.size(); ++index) {
            const auto label = "hole " + std::to_string(index + 1);
            if (boundaries_meet(outer, holes[index]) || !contains(outer, holes[index].front().start))
                return label + " must be strictly inside the outer boundary without contact";
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (boundaries_meet(holes[previous], holes[index]) ||
                    contains(holes[previous], holes[index].front().start) ||
                    contains(holes[index], holes[previous].front().start))
                    return "holes " + std::to_string(previous + 1) + " and " +
                        std::to_string(index + 1) + " must be disjoint without contact or nesting";
            }
        }
    } catch (const std::exception&) {
        return "hole topology is numerically indeterminate";
    }
    return std::nullopt;
}

}  // namespace sketch
