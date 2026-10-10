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
    const bool half_turn = std::abs(segment.sweep_radians) == std::numbers::pi;
    const auto sine = half_turn ? 1.0 : std::sin(std::abs(half_sweep));
    const auto tangent = half_turn ? 1.0 : std::tan(half_sweep);
    if (!(sine > 0.0) || tangent == 0.0 || !std::isfinite(tangent)) {
        throw std::invalid_argument("arc sweep cannot be represented");
    }

    const Vec2 midpoint{std::midpoint(segment.start.x, segment.end.x),
                        std::midpoint(segment.start.y, segment.end.y)};
    const Vec2 left_normal{-chord.y / chord_length, chord.x / chord_length};
    const auto center = half_turn ? midpoint :
        midpoint + left_normal * (chord_length / (2.0 * tangent));
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

struct CompensatedValue {
    double value;
    double tail;
};

CompensatedValue compensated_sum(double a, double b) {
    const auto sum = a + b;
    const auto restored_b = sum - a;
    return {sum, (a - (sum - restored_b)) + (b - restored_b)};
}

CompensatedValue compensated_add(CompensatedValue a, CompensatedValue b) {
    const auto sum = compensated_sum(a.value, b.value);
    return compensated_sum(sum.value, sum.tail + a.tail + b.tail);
}

CompensatedValue compensated_product(CompensatedValue a, CompensatedValue b) {
    const auto product = a.value * b.value;
    const auto tail = std::fma(a.value, b.value, -product) +
        a.value * b.tail + a.tail * b.value + a.tail * b.tail;
    return compensated_sum(product, tail);
}

bool exact_point_difference(Vec2 point, Vec2 origin) {
    return compensated_sum(point.x, -origin.x).tail == 0.0 &&
        compensated_sum(point.y, -origin.y).tail == 0.0;
}

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
    // Internal validators also call this kernel directly. Normalize them at
    // the arc, just as the public contact API does, so a rounded world midpoint
    // cannot change branch membership or erase a second endpoint-root contact.
    if (arc_segment.start.x != 0.0 || arc_segment.start.y != 0.0) {
        const auto origin = arc_segment.start;
        const Segment local_line{line.start - origin, line.end - origin, 0.0};
        const Segment local_arc{{0, 0}, arc_segment.end - origin, arc_segment.sweep_radians};
        if (!finite(local_line.start) || !finite(local_line.end) || !finite(local_arc.end)) {
            result.kind = IntersectionKind::indeterminate;
            return result;
        }
        auto local = intersect_line_arc(local_line, local_arc, tolerance);
        for (std::size_t index = 0; index < local.point_count; ++index) {
            const auto point = local.points[index];
            const auto restored = point + origin;
            if (!finite(restored) || distance(restored - origin, point) > tolerance) {
                result.kind = IntersectionKind::indeterminate;
                return result;
            }
            local.points[index] = restored;
        }
        return local;
    }
    const auto bounds = segment_bounds(arc_segment);
    if (std::max(line.start.x, line.end.x) < bounds.minimum.x - tolerance ||
        std::min(line.start.x, line.end.x) > bounds.maximum.x + tolerance ||
        std::max(line.start.y, line.end.y) < bounds.minimum.y - tolerance ||
        std::min(line.start.y, line.end.y) > bounds.maximum.y + tolerance) return result;

    const auto direction = line.end - line.start;
    const auto line_length = length(direction);
    const auto chord = arc_segment.end - arc_segment.start;
    const auto chord_length = length(chord);
    const Vec2 midpoint{std::midpoint(arc_segment.start.x, arc_segment.end.x),
                        std::midpoint(arc_segment.start.y, arc_segment.end.y)};
    const Vec2 normal{-chord.y / chord_length, chord.x / chord_length};
    // The chosen arc is confined to one chord half-plane. Prove separation
    // of the whole finite line before solving its supporting circle: a tangent
    // on the excluded circle portion is not an unresolved arc contact.
    const auto excluded_side = [&](Vec2 point) {
        const auto offset = point - midpoint;
        const auto side = std::copysign(1.0, arc_segment.sweep_radians) *
            std::fma(offset.x, normal.x, offset.y * normal.y);
        const auto error = 64.0 * std::numeric_limits<double>::epsilon() *
            ((std::abs(point.x) + std::abs(midpoint.x)) * std::abs(normal.x) +
             (std::abs(point.y) + std::abs(midpoint.y)) * std::abs(normal.y));
        return std::isfinite(side) && std::isfinite(error) && side > tolerance + error;
    };
    if (excluded_side(line.start) && excluded_side(line.end)) return result;
    const Vec2 unit{direction.x / line_length, direction.y / line_length};
    const Vec2 line_normal{-unit.y, unit.x};
    const auto accurate_cross = [](Vec2 a, Vec2 b) {
        const auto product = a.y * b.x;
        return std::fma(a.x, b.y, -product) + std::fma(-a.y, b.x, product);
    };
    // Anchor the line near the chord, independently of its finite endpoints.
    // Subtracting a distant line parameter would erase nearby distinct roots.
    double offset;
    if (direction.y == 0.0) offset = (line.start.y - midpoint.y) * unit.x;
    else if (direction.x == 0.0) offset = -(line.start.x - midpoint.x) * unit.y;
    else offset = (accurate_cross(direction, line.start) -
                   accurate_cross(direction, midpoint)) / line_length;
    const auto scale = std::max(chord_length, std::abs(offset));
    if (!std::isfinite(scale) || !(scale > 0.0) || !finite(unit)) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }
    const Vec2 p = line_normal * (offset / scale);
    const auto half_chord = (chord_length / scale) * 0.5;
    const bool half_turn = std::abs(arc_segment.sweep_radians) == std::numbers::pi;
    const auto sine = half_turn ? std::copysign(1.0, arc_segment.sweep_radians) :
        std::sin(arc_segment.sweep_radians * 0.5);
    const auto half_chord_cosine = half_turn ? 0.0 :
        half_chord * std::cos(arc_segment.sweep_radians * 0.5);
    const auto precise_dot = [](Vec2 a, Vec2 b) { return std::fma(a.x, b.x, a.y * b.y); };
    // S*(x*x+y*y-h*h)-2*h*C*dot((x,y),normal)=0 is the
    // circle equation in the chord frame; no distant center or radius square.
    double a = sine * precise_dot(unit, unit);
    double b = 2.0 * std::fma(sine, precise_dot(p, unit),
                              -half_chord_cosine * precise_dot(unit, normal));
    double k = std::fma(sine, std::fma(p.x, p.x, std::fma(p.y, p.y, -half_chord * half_chord)),
                        -2.0 * half_chord_cosine * precise_dot(p, normal));
    constexpr auto epsilon = std::numeric_limits<double>::epsilon();
    double error_a = 32.0 * epsilon * std::abs(sine) * (unit.x * unit.x + unit.y * unit.y);
    double error_b = 64.0 * epsilon * (std::abs(sine) *
        (std::abs(p.x * unit.x) + std::abs(p.y * unit.y)) + std::abs(half_chord_cosine) *
        (std::abs(unit.x * normal.x) + std::abs(unit.y * normal.y)));
    double error_k = 64.0 * epsilon * (std::abs(sine) *
        (p.x * p.x + p.y * p.y + half_chord * half_chord) +
        2.0 * std::abs(half_chord_cosine) * (std::abs(p.x * normal.x) + std::abs(p.y * normal.y)));
    const auto coefficient_scale = std::max({std::abs(a), std::abs(b), std::abs(k)});
    if (!std::isfinite(coefficient_scale) || !(coefficient_scale > 0.0)) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }
    a /= coefficient_scale; b /= coefficient_scale; k /= coefficient_scale;
    error_a /= coefficient_scale; error_b /= coefficient_scale; error_k /= coefficient_scale;
    const auto along = [&](Vec2 point) {
        return precise_dot((point - midpoint) * (1.0 / scale), unit);
    };
    const auto low = std::min(along(line.start), along(line.end));
    const auto high = std::max(along(line.start), along(line.end));
    if (!std::isfinite(low) || !std::isfinite(high) ||
        !std::isfinite(error_a) || !std::isfinite(error_b) || !std::isfinite(error_k)) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }
    bool unresolved = false;
    const auto admit_verified_point = [&](Vec2 point) {
        if (!finite(point)) { unresolved = true; return; }
        const auto root = along(point);
        const auto range_tolerance = tolerance / scale + 64.0 * epsilon * std::max(1.0, std::abs(root));
        if (!std::isfinite(root)) { unresolved = true; return; }
        if (root < low - range_tolerance || root > high + range_tolerance) return;
        const auto local = (point - midpoint) * (1.0 / scale);
        const auto side = precise_dot(local, normal);
        const auto side_error = 64.0 * epsilon *
            (std::abs(local.x * normal.x) + std::abs(local.y * normal.y));
        if (!std::isfinite(side) || !std::isfinite(side_error)) { unresolved = true; return; }
        // A world-distance tolerance may admit an endpoint contact, but must
        // not enlarge the chosen sweep by the entire band around its chord.
        if (std::copysign(1.0, arc_segment.sweep_radians) * side > side_error &&
            distance(point, arc_segment.start) > tolerance &&
            distance(point, arc_segment.end) > tolerance) return;
        if (distance(point, line.start) <= tolerance) add_point(result, line.start, tolerance);
        else if (distance(point, line.end) <= tolerance) add_point(result, line.end, tolerance);
        else if (distance(point, arc_segment.start) <= tolerance) add_point(result, arc_segment.start, tolerance);
        else if (distance(point, arc_segment.end) <= tolerance) add_point(result, arc_segment.end, tolerance);
        else add_point(result, point, tolerance);
    };
    const auto admit_root = [&](double root) {
        if (!std::isfinite(root)) { unresolved = true; return; }
        const auto range_tolerance = tolerance / scale + 64.0 * epsilon * std::max(1.0, std::abs(root));
        if (root < low - range_tolerance || root > high + range_tolerance) return;
        const Vec2 local{std::fma(root, unit.x, p.x), std::fma(root, unit.y, p.y)};
        const auto residual = std::fma(std::fma(a, root, b), root, k);
        const auto residual_error = error_a * root * root + error_b * std::abs(root) + error_k +
            64.0 * epsilon * (std::abs(a) * root * root + std::abs(b * root) + std::abs(k));
        if (!std::isfinite(residual) || !std::isfinite(residual_error) ||
            std::abs(residual) > residual_error) { unresolved = true; return; }
        const Vec2 point{std::fma(local.x, scale, midpoint.x), std::fma(local.y, scale, midpoint.y)};
        admit_verified_point(point);
    };
    // An exact shared station is a known circle root. Anchor the polynomial
    // there instead of evaluating a nearly cancelling constant coefficient.
    // Factoring t*(A*t+B) retains the possible second contact; only a proved
    // tolerance-sized second-root band may merge with the known endpoint.
    for (const auto endpoint : std::array{arc_segment.start, arc_segment.end}) {
        if (!((endpoint.x == line.start.x && endpoint.y == line.start.y) ||
              (endpoint.x == line.end.x && endpoint.y == line.end.y))) continue;
        admit_verified_point(endpoint);
        // An endpoint is exactly +/- half the chord from its geometric
        // midpoint, even when that midpoint cannot be represented in world
        // coordinates. Do not let rounded translation erase the derivative.
        const auto half_sign = endpoint.x == arc_segment.start.x && endpoint.y == arc_segment.start.y
            ? -0.5 : 0.5;
        const Vec2 local{(chord.x / scale) * half_sign, (chord.y / scale) * half_sign};
        const auto endpoint_a = sine * precise_dot(unit, unit);
        const auto endpoint_b = 2.0 * std::fma(sine, precise_dot(local, unit),
            -half_chord_cosine * precise_dot(unit, normal));
        const auto endpoint_b_error = 64.0 * epsilon * (std::abs(sine) *
            (std::abs(local.x * unit.x) + std::abs(local.y * unit.y)) +
            std::abs(half_chord_cosine) *
            (std::abs(unit.x * normal.x) + std::abs(unit.y * normal.y)));
        const auto delta = -endpoint_b / endpoint_a;
        const auto delta_error = endpoint_b_error / std::abs(endpoint_a) +
            64.0 * epsilon * std::abs(delta);
        if (!std::isfinite(delta) || !std::isfinite(delta_error) || endpoint_a == 0.0) {
            unresolved = true;
        } else if (std::abs(delta) + delta_error <= tolerance / scale) {
            // Both roots are independently confined to this endpoint band.
        } else if (std::abs(delta) <= delta_error) {
            unresolved = true;
        } else {
            const auto offset = delta * scale;
            admit_verified_point({std::fma(unit.x, offset, endpoint.x),
                                  std::fma(unit.y, offset, endpoint.y)});
        }
        if (unresolved) {
            result.kind = IntersectionKind::indeterminate;
            result.point_count = 0;
        }
        return result;
    }
    if (a == 0.0) {
        if (b != 0.0) admit_root(-k / b);
        else unresolved = true;
    } else {
        const auto discriminant = std::fma(b, b, -4.0 * a * k);
        const auto discriminant_error = 2.0 * std::abs(b) * error_b +
            4.0 * (std::abs(k) * error_a + std::abs(a) * error_k) +
            64.0 * epsilon * (b * b + 4.0 * std::abs(a * k));
        if (!std::isfinite(discriminant) || !std::isfinite(discriminant_error)) unresolved = true;
        else if (std::abs(discriminant) <= discriminant_error) {
            // Normalizing a direction can move a line across a circle by one
            // ulp. For half turns, independently evaluate
            // R^2*(D.D)-cross(D,P-M)^2 from the represented inputs, preserving
            // product tails. Other uncertain signs do not produce contacts.
            const auto exact_difference = [](double x, double y) {
                return compensated_sum(x, -y).tail == 0.0;
            };
            const auto center_x = compensated_sum(arc_segment.start.x * 0.5,
                                                   arc_segment.end.x * 0.5);
            const auto center_y = compensated_sum(arc_segment.start.y * 0.5,
                                                   arc_segment.end.y * 0.5);
            const auto offset = line.start - midpoint;
            if (!half_turn ||
                center_x.tail != 0.0 || center_y.tail != 0.0 ||
                center_x.value != midpoint.x || center_y.value != midpoint.y ||
                !exact_difference(arc_segment.end.x, arc_segment.start.x) ||
                !exact_difference(arc_segment.end.y, arc_segment.start.y) ||
                !exact_difference(line.end.x, line.start.x) ||
                !exact_difference(line.end.y, line.start.y) ||
                !exact_difference(line.start.x, midpoint.x) ||
                !exact_difference(line.start.y, midpoint.y)) {
                unresolved = true;
            } else {
                const auto radius = chord_length * 0.5;
                int direction_exponent = 0;
                int position_exponent = 0;
                std::frexp(std::max(std::abs(direction.x), std::abs(direction.y)), &direction_exponent);
                std::frexp(std::max({radius, std::abs(offset.x), std::abs(offset.y)}), &position_exponent);
                const auto dx = std::ldexp(direction.x, -direction_exponent);
                const auto dy = std::ldexp(direction.y, -direction_exponent);
                const auto px = std::ldexp(offset.x, -position_exponent);
                const auto py = std::ldexp(offset.y, -position_exponent);
                const auto r = std::ldexp(radius, -position_exponent);
                const auto squared = [&](double x) { return compensated_product({x, 0.0}, {x, 0.0}); };
                // The circle is defined by its endpoints. Squaring a rounded
                // hypot radius would enlarge a tilted diameter's circle.
                const auto half_x = std::ldexp(chord.x, -position_exponent) * 0.5;
                const auto half_y = std::ldexp(chord.y, -position_exponent) * 0.5;
                const auto radius_squared = compensated_add(squared(half_x), squared(half_y));
                const auto norm_squared = compensated_add(squared(dx), squared(dy));
                auto second_cross_product = compensated_product({dy, 0.0}, {px, 0.0});
                second_cross_product.value = -second_cross_product.value;
                second_cross_product.tail = -second_cross_product.tail;
                const auto determinant = compensated_add(
                    compensated_product({dx, 0.0}, {py, 0.0}), second_cross_product);
                const auto radial_product = compensated_product(radius_squared, norm_squared);
                auto determinant_squared = compensated_product(determinant, determinant);
                const auto proof_error = 512.0 * epsilon * epsilon *
                    (std::abs(radial_product.value) + std::abs(determinant_squared.value)) +
                    128.0 * std::numeric_limits<double>::denorm_min();
                determinant_squared.value = -determinant_squared.value;
                determinant_squared.tail = -determinant_squared.tail;
                const auto proof = compensated_add(radial_product, determinant_squared);
                const auto proof_value = proof.value + proof.tail;
                // An axis line through the endpoint of an axis diameter is
                // independently an exact tangent. Its known endpoint is the
                // sole circle contact, including when the discriminant is zero.
                bool exact_endpoint_tangent = false;
                for (const auto endpoint : std::array{arc_segment.start, arc_segment.end}) {
                    if ((chord.y == 0.0 && direction.x == 0.0 && endpoint.x == line.start.x) ||
                        (chord.x == 0.0 && direction.y == 0.0 && endpoint.y == line.start.y)) {
                        exact_endpoint_tangent = true;
                        admit_verified_point(endpoint);
                    }
                }
                if (exact_endpoint_tangent) {
                    // Admission also verifies the finite line range.
                    if (unresolved) result.kind = IntersectionKind::indeterminate;
                    return result;
                }
                const bool axis_diameter = chord.x == 0.0 || chord.y == 0.0;
                if (axis_diameter && direction.y == 0.0 && std::abs(offset.y) == radius) {
                    admit_verified_point({midpoint.x, line.start.y});
                    if (unresolved) result.kind = IntersectionKind::indeterminate;
                    return result;
                }
                if (axis_diameter && direction.x == 0.0 && std::abs(offset.x) == radius) {
                    admit_verified_point({line.start.x, midpoint.y});
                    if (unresolved) result.kind = IntersectionKind::indeterminate;
                    return result;
                }
                if (!std::isfinite(proof_value) || !(r > 0.0) || !std::isfinite(proof_error)) {
                    unresolved = true;
                } else if (proof_value > proof_error) {
                    const auto norm = norm_squared.value + norm_squared.tail;
                    const auto signed_offset = (determinant.value + determinant.tail) / norm;
                    const auto spacing = std::sqrt(proof_value) / norm;
                    for (const double sign : {-1.0, 1.0}) {
                        const auto x = std::fma(-dy, signed_offset, sign * dx * spacing);
                        const auto y = std::fma(dx, signed_offset, sign * dy * spacing);
                        admit_verified_point({midpoint.x + std::ldexp(x, position_exponent),
                                              midpoint.y + std::ldexp(y, position_exponent)});
                    }
                } else if (proof_value >= -proof_error) {
                    unresolved = true;
                }
            }
        }
        else if (discriminant > 0.0) {
            // A small positive discriminant still has two genuine contacts.
            const auto q = -0.5 * (b + std::copysign(std::sqrt(discriminant), b));
            if (q == 0.0) unresolved = true;
            else { admit_root(q / a); admit_root(k / q); }
        } else if (discriminant == 0.0) admit_root(-b / (2.0 * a));
        else if (discriminant >= -discriminant_error) unresolved = true;
    }
    if (unresolved) result.kind = IntersectionKind::indeterminate;
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
                                  double tolerance, bool exact_input_origin) {
    IntersectionResult result;
    // Boundary validation calls this kernel directly, unlike the public
    // contact API. Preserve the same local circle arithmetic in both paths.
    if (left_segment.start.x != 0.0 || left_segment.start.y != 0.0) {
        const auto origin = left_segment.start;
        const Segment local_left{{0, 0}, left_segment.end - origin, left_segment.sweep_radians};
        const Segment local_right{right_segment.start - origin, right_segment.end - origin,
                                  right_segment.sweep_radians};
        if (!finite(local_left.end) || !finite(local_right.start) || !finite(local_right.end)) {
            result.kind = IntersectionKind::indeterminate;
            return result;
        }
        const bool exact_local_origin = exact_input_origin &&
            exact_point_difference(left_segment.start, origin) &&
            exact_point_difference(left_segment.end, origin) &&
            exact_point_difference(right_segment.start, origin) &&
            exact_point_difference(right_segment.end, origin);
        auto local = intersect_arcs(local_left, local_right, tolerance, exact_local_origin);
        for (std::size_t index = 0; index < local.point_count; ++index) {
            const auto point = local.points[index];
            const auto restored = point + origin;
            if (!finite(restored) || distance(restored - origin, point) > tolerance) {
                result.kind = IntersectionKind::indeterminate;
                return result;
            }
            local.points[index] = restored;
        }
        return local;
    }
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

    const auto add_if_on_both = [&](Vec2 point) {
        if (point_on_arc(left, point, tolerance) && point_on_arc(right, point, tolerance)) {
            add_point(result, point, tolerance);
        }
    };

    // A shared source station is an exact root of both endpoint-defined
    // circles. In coordinates X from that station, each circle satisfies
    // S*|X|^2 - V.X = 0, where V = sign*S*chord + C*J(chord),
    // S = sin(sweep/2), C = cos(sweep/2), and sign selects start/end.
    // Their radical axis is N.X = 0, N = S_right*V_left-S_left*V_right.
    // The only other root has distance |cross(V_left,V_right)|/|N|.
    // Bound that entire distance before merging it with the known station;
    // a small radial discriminant alone cannot establish a tangent.
    const auto same_station = [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; };
    const bool shared_station = same_station(left_segment.start, right_segment.start) ||
        same_station(left_segment.start, right_segment.end) ||
        same_station(left_segment.end, right_segment.start) ||
        same_station(left_segment.end, right_segment.end);
    if (exact_input_origin && shared_station &&
        exact_point_difference(left_segment.end, left_segment.start) &&
        exact_point_difference(right_segment.end, right_segment.start)) {
        const auto left_chord = left_segment.end - left_segment.start;
        const auto right_chord = right_segment.end - right_segment.start;
        int exponent = 0;
        std::frexp(std::max({std::abs(left_chord.x), std::abs(left_chord.y),
                            std::abs(right_chord.x), std::abs(right_chord.y)}), &exponent);
        const Vec2 q_left{std::ldexp(left_chord.x, -exponent),
                          std::ldexp(left_chord.y, -exponent)};
        const Vec2 q_right{std::ldexp(right_chord.x, -exponent),
                           std::ldexp(right_chord.y, -exponent)};
        const bool exact_scale = std::ldexp(q_left.x, exponent) == left_chord.x &&
            std::ldexp(q_left.y, exponent) == left_chord.y &&
            std::ldexp(q_right.x, exponent) == right_chord.x &&
            std::ldexp(q_right.y, exponent) == right_chord.y;
        const auto scaled_tolerance = std::ldexp(tolerance, -exponent);
        constexpr auto epsilon = std::numeric_limits<double>::epsilon();
        constexpr auto arithmetic_error = 64.0 * epsilon;
        constexpr auto trig_error = 16.0 * epsilon;
        constexpr auto underflow_error = 128.0 * std::numeric_limits<double>::denorm_min();
        const auto half_angle = [](double sweep) {
            return std::abs(sweep) == std::numbers::pi
                ? Vec2{std::copysign(1.0, sweep), 0.0}
                : Vec2{std::sin(sweep * 0.5), std::cos(sweep * 0.5)};
        };
        const auto left_angle = half_angle(left_segment.sweep_radians);
        const auto right_angle = half_angle(right_segment.sweep_radians);
        if (exact_scale && std::isfinite(scaled_tolerance) && scaled_tolerance > 0.0 &&
            finite(left_angle) && finite(right_angle) &&
            std::abs(left_angle.x) > trig_error && std::abs(right_angle.x) > trig_error) {
            for (const auto station : std::array{left_segment.start, left_segment.end}) {
                if (!(same_station(station, right_segment.start) || same_station(station, right_segment.end))) continue;
                const auto left_sign = station.x == left_segment.start.x &&
                    station.y == left_segment.start.y ? 1.0 : -1.0;
                const auto right_sign = station.x == right_segment.start.x &&
                    station.y == right_segment.start.y ? 1.0 : -1.0;
                const auto coefficient = [](Vec2 q, Vec2 angle, double sign) {
                    return Vec2{std::fma(sign * angle.x, q.x, -angle.y * q.y),
                                std::fma(sign * angle.x, q.y, angle.y * q.x)};
                };
                const auto v_left = coefficient(q_left, left_angle, left_sign);
                const auto v_right = coefficient(q_right, right_angle, right_sign);
                const auto left_error = arithmetic_error *
                    (std::abs(q_left.x) + std::abs(q_left.y)) + underflow_error;
                const auto right_error = arithmetic_error *
                    (std::abs(q_right.x) + std::abs(q_right.y)) + underflow_error;
                const Vec2 normal{
                    std::fma(right_angle.x, v_left.x, -left_angle.x * v_right.x),
                    std::fma(right_angle.x, v_left.y, -left_angle.x * v_right.y)};
                const auto component_error = [&](double a, double b) {
                    return std::abs(right_angle.x) * left_error +
                        std::abs(left_angle.x) * right_error +
                        trig_error * (std::abs(a) + std::abs(b) + left_error + right_error) +
                        arithmetic_error * (std::abs(right_angle.x * a) +
                                            std::abs(left_angle.x * b)) + underflow_error;
                };
                const auto norm = length(normal);
                const auto norm_error = length({component_error(v_left.x, v_right.x),
                                               component_error(v_left.y, v_right.y)}) +
                    arithmetic_error * norm + underflow_error;
                const auto product = v_left.y * v_right.x;
                const auto determinant = std::fma(v_left.x, v_right.y, -product) +
                    std::fma(-v_left.y, v_right.x, product);
                const auto determinant_error = left_error *
                    (std::abs(v_right.x) + std::abs(v_right.y)) + right_error *
                    (std::abs(v_left.x) + std::abs(v_left.y)) +
                    2.0 * left_error * right_error + arithmetic_error *
                    (std::abs(v_left.x * v_right.y) + std::abs(product)) + underflow_error;
                const auto lower_norm = norm - norm_error;
                if (!finite(normal) || !std::isfinite(norm_error) || !(lower_norm > 0.0)) continue;
                const auto upper_distance = (std::abs(determinant) + determinant_error) /
                    lower_norm;
                if (std::isfinite(upper_distance) && upper_distance >= 0.0 &&
                    upper_distance <= scaled_tolerance * (1.0 - arithmetic_error)) {
                    add_point(result, station, tolerance);
                    return result;
                }
                // A coincident/uncertain radical axis or a separated second
                // root retains the existing two-root/indeterminate path.
            }
        }
    }

    // Axis diameters can establish an exact tangent independently of the
    // cancellation-prone circle discriminant only if every earlier origin
    // subtraction retained the actual endpoints. Other unresolved tangencies
    // fail closed rather than manufacture a contact within radial tolerance.
    const auto exact_axis_circle = [](const Segment& segment, const ArcGeometry& arc) {
        if (std::abs(segment.sweep_radians) != std::numbers::pi) return false;
        const auto chord_x = compensated_sum(segment.end.x, -segment.start.x);
        const auto chord_y = compensated_sum(segment.end.y, -segment.start.y);
        const auto center_x = compensated_sum(segment.start.x * 0.5, segment.end.x * 0.5);
        const auto center_y = compensated_sum(segment.start.y * 0.5, segment.end.y * 0.5);
        return chord_x.tail == 0.0 && chord_y.tail == 0.0 &&
            (chord_x.value == 0.0 || chord_y.value == 0.0) &&
            center_x.tail == 0.0 && center_y.tail == 0.0 &&
            center_x.value == arc.center.x && center_y.value == arc.center.y;
    };
    const auto center_x = compensated_sum(right.center.x, -left.center.x);
    const auto center_y = compensated_sum(right.center.y, -left.center.y);
    const auto radius_sum = compensated_sum(left.radius, right.radius);
    const auto radius_difference = compensated_sum(left.radius, -right.radius);
    const bool external_tangent = radius_sum.tail == 0.0 && center_distance == radius_sum.value;
    const bool internal_tangent = radius_difference.tail == 0.0 &&
        center_distance == std::abs(radius_difference.value);
    if (exact_input_origin && exact_axis_circle(left_segment, left) &&
        exact_axis_circle(right_segment, right) &&
        center_x.tail == 0.0 && center_y.tail == 0.0 &&
        (center_x.value == 0.0 || center_y.value == 0.0) &&
        (external_tangent || internal_tangent)) {
        const auto direction = external_tangent || left.radius > right.radius ? 1.0 : -1.0;
        add_if_on_both(left.center + centers * (direction * left.radius / center_distance));
        return result;
    }

    const auto left_squared = left.radius * left.radius;
    const auto right_squared = right.radius * right.radius;
    const auto distance_squared = center_distance * center_distance;
    const auto along = (left_squared - right_squared + distance_squared) /
                       (2.0 * center_distance);
    const auto along_squared = along * along;
    const auto height_squared = left_squared - along_squared;
    constexpr auto roundoff = 64.0 * std::numeric_limits<double>::epsilon();
    const auto along_error = roundoff * (left_squared + right_squared + distance_squared) /
                             (2.0 * center_distance);
    const auto height_error = roundoff * (left_squared + along_squared) +
        2.0 * std::abs(along) * along_error + along_error * along_error;
    if (!std::isfinite(height_squared) || !std::isfinite(height_error) ||
        height_squared <= height_error) {
        result.kind = IntersectionKind::indeterminate;
        return result;
    }

    // A positive height has two genuine circle roots even when their radial
    // penetration is below the metre tolerance. Merge only contacts whose
    // actual separation is within tolerance, through add_point above.
    const auto base = left.center + centers * (along / center_distance);
    const auto height = std::sqrt(height_squared);
    const Vec2 perpendicular{-centers.y / center_distance, centers.x / center_distance};
    add_if_on_both(base + perpendicular * height);
    add_if_on_both(base - perpendicular * height);
    return result;
}

IntersectionResult intersect_segments(const Segment& left, const Segment& right,
                                      double tolerance, bool exact_input_origin = true) {
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
    return intersect_arcs(left, right, tolerance, exact_input_origin);
}

// Clearance is a topology decision, not an intersection coordinate. A line
// and arc can miss while their interior stationary points are within tolerance.
// Only analytical endpoints and circle normals are needed to find that minimum;
// arithmetic uncertainty at the tolerance boundary rejects conservatively.
bool line_arc_clearance_unresolved_or_within(const Segment& source_line,
                                            const Segment& source_arc,
                                            double tolerance) {
    const auto origin = source_arc.start;
    const Segment line{source_line.start - origin, source_line.end - origin, 0.0};
    const Segment segment{source_arc.start - origin, source_arc.end - origin,
                          source_arc.sweep_radians};
    const auto line_bounds = segment_bounds(line);
    const auto arc_bounds = segment_bounds(segment);
    if (line_bounds.maximum.x < arc_bounds.minimum.x - tolerance ||
        arc_bounds.maximum.x < line_bounds.minimum.x - tolerance ||
        line_bounds.maximum.y < arc_bounds.minimum.y - tolerance ||
        arc_bounds.maximum.y < line_bounds.minimum.y - tolerance) return false;

    const auto arc = arc_geometry(segment);
    const auto direction = line.end - line.start;
    const auto line_length = length(direction);
    const Vec2 unit{direction.x / line_length, direction.y / line_length};
    const Vec2 normal{-unit.y, unit.x};
    const Vec2 line_middle{std::midpoint(line.start.x, line.end.x),
                           std::midpoint(line.start.y, line.end.y)};
    const Vec2 arc_middle{std::midpoint(segment.start.x, segment.end.x),
                          std::midpoint(segment.start.y, segment.end.y)};
    const auto chord = segment.end - segment.start;
    const auto chord_length = length(chord);
    const Vec2 arc_normal{-chord.y / chord_length, chord.x / chord_length};
    constexpr auto roundoff = 128.0 * std::numeric_limits<double>::epsilon();
    const auto checked_dot = [](Vec2 a, Vec2 b) {
        return std::fma(a.x, b.x, a.y * b.y);
    };
    const auto dot_error = [&](Vec2 a, Vec2 b) {
        return roundoff * (std::abs(a.x * b.x) + std::abs(a.y * b.y));
    };
    const auto on_chosen_arc_or_unresolved = [&](Vec2 point, double point_error) {
        const auto offset = point - arc_middle;
        const auto side = std::copysign(1.0, segment.sweep_radians) *
            checked_dot(offset, arc_normal);
        const auto error = point_error + dot_error(offset, arc_normal);
        return !std::isfinite(side) || !std::isfinite(error) || side <= error;
    };
    const auto arc_point_near_line = [&](Vec2 point) {
        const auto offset = point - line_middle;
        const auto along = checked_dot(offset, unit);
        const auto half_length = line_length * 0.5;
        const auto nearest = line_middle + unit * std::clamp(along, -half_length, half_length);
        const auto gap = distance(point, nearest);
        const auto error = roundoff * (length(offset) + line_length + length(point));
        return !finite(nearest) || !std::isfinite(gap) || !std::isfinite(error) ||
            gap <= tolerance + error;
    };
    if (arc_point_near_line(segment.start) || arc_point_near_line(segment.end)) return true;

    const auto line_point_near_arc = [&](Vec2 point) {
        if (distance(point, segment.start) <= tolerance ||
            distance(point, segment.end) <= tolerance) return true;
        const auto radial = point - arc.center;
        const auto radius = length(radial);
        const auto error = roundoff * (radius + arc.radius + length(arc.center));
        if (!std::isfinite(radius) || !std::isfinite(error)) return true;
        if (std::abs(radius - arc.radius) > tolerance + error) return false;
        if (!(radius > 0.0)) return true;
        const auto nearest = arc.center + radial * (arc.radius / radius);
        return !finite(nearest) || on_chosen_arc_or_unresolved(nearest, error);
    };
    if (line_point_near_arc(line.start) || line_point_near_arc(line.end)) return true;

    const auto center_offset = arc.center - line_middle;
    const auto signed_distance = checked_dot(center_offset, normal);
    const auto distance_error = dot_error(center_offset, normal) +
        roundoff * (arc.radius + length(arc.center));
    for (const double sign : {-1.0, 1.0}) {
        const auto gap = std::abs(signed_distance + sign * arc.radius);
        if (!std::isfinite(gap) || !std::isfinite(distance_error)) return true;
        if (gap > tolerance + distance_error) continue;
        const auto candidate = arc.center + normal * (sign * arc.radius);
        if (!finite(candidate)) return true;
        const auto along = checked_dot(candidate - line_middle, unit);
        const auto along_error = dot_error(candidate - line_middle, unit) + distance_error;
        if (!std::isfinite(along) || !std::isfinite(along_error)) return true;
        if (std::abs(along) > line_length * 0.5 + along_error) continue;
        if (on_chosen_arc_or_unresolved(candidate, distance_error)) return true;
    }
    return false;
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

SegmentIntersection segment_intersection(const Segment& first, const Segment& second, double tolerance) {
    if (!std::isfinite(tolerance) || tolerance<=0 ||
        !finite(first.start) || !finite(first.end) || !finite(second.start) || !finite(second.end) ||
        !std::isfinite(first.sweep_radians) || !std::isfinite(second.sweep_radians) ||
        segment_length(first)<=tolerance || segment_length(second)<=tolerance)
        throw std::invalid_argument("Segment intersection requires finite nondegenerate geometry");
    // Local coordinates avoid cancellation far from the document origin.
    // Retain subtraction fidelity for the kernel's exact-tangency exception.
    const auto origin = first.sweep_radians != 0.0 ? first.start :
        (second.sweep_radians != 0.0 ? second.start : first.start);
    const bool exact_input_origin = exact_point_difference(first.start, origin) &&
        exact_point_difference(first.end, origin) && exact_point_difference(second.start, origin) &&
        exact_point_difference(second.end, origin);
    auto left=first; auto right=second;
    left.start=first.start-origin; left.end=first.end-origin;
    right.start=second.start-origin; right.end=second.end-origin;
    const auto hit=intersect_segments(left,right,tolerance,exact_input_origin);
    SegmentIntersection result;
    if (hit.kind==IntersectionKind::overlap) result.kind=SegmentIntersectionKind::overlap;
    else if (hit.kind==IntersectionKind::indeterminate) result.kind=SegmentIntersectionKind::indeterminate;
    else if (hit.kind==IntersectionKind::points) {
        result.kind=SegmentIntersectionKind::touch;
        for (std::size_t i=0;i<hit.point_count;++i) {
            const auto point=hit.points[i];
            if (distance(point,left.start)>tolerance && distance(point,left.end)>tolerance &&
                distance(point,right.start)>tolerance && distance(point,right.end)>tolerance)
                result.kind=SegmentIntersectionKind::proper;
            result.points.push_back(point+origin);
        }
    }
    return result;
}

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
    if (std::abs(segment.sweep_radians) == std::numbers::pi) {
        for (const auto direction : std::array{Vec2{1, 0}, Vec2{0, 1}, Vec2{-1, 0}, Vec2{0, -1}}) {
            if (std::copysign(1.0, segment.sweep_radians) * dot(direction, normal) > 0.0) continue;
            const Vec2 point{midpoint.x + direction.x * arc.radius,
                             midpoint.y + direction.y * arc.radius};
            if (!finite(point)) throw std::invalid_argument("arc bounds exceed numeric range");
            result.minimum.x = std::min(result.minimum.x, point.x);
            result.minimum.y = std::min(result.minimum.y, point.y);
            result.maximum.x = std::max(result.maximum.x, point.x);
            result.maximum.y = std::max(result.maximum.y, point.y);
        }
        return result;
    }
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

Boundary clip_boundary_to_bounds(const Boundary& boundary, const Bounds2& bounds,
                                 double tolerance_metres) {
    if (!finite(bounds.minimum) || !finite(bounds.maximum) ||
        !std::isfinite(tolerance_metres) || !(tolerance_metres > 0.0) ||
        bounds.maximum.x - bounds.minimum.x <= tolerance_metres ||
        bounds.maximum.y - bounds.minimum.y <= tolerance_metres) {
        throw std::invalid_argument(
            "boundary crop requires finite ordered bounds and a positive tolerance");
    }
    const auto inside = [&](Vec2 point) {
        return point.x >= bounds.minimum.x - tolerance_metres &&
               point.x <= bounds.maximum.x + tolerance_metres &&
               point.y >= bounds.minimum.y - tolerance_metres &&
               point.y <= bounds.maximum.y + tolerance_metres;
    };
    Boundary result;
    for (const auto& segment : boundary) {
        require_finite_segment(segment);
        const auto source_length = segment_length(segment);
        const auto parameter_tolerance = std::min(
            0.25, tolerance_metres / std::max(source_length, tolerance_metres));
        std::vector<double> parameters{0.0, 1.0};
        const auto add_parameter = [&](double value) {
            if (!std::isfinite(value) || value < -parameter_tolerance ||
                value > 1.0 + parameter_tolerance) return;
            parameters.push_back(std::clamp(value, 0.0, 1.0));
        };

        std::optional<ArcGeometry> arc;
        if (segment.sweep_radians == 0.0) {
            const auto dx = segment.end.x - segment.start.x;
            const auto dy = segment.end.y - segment.start.y;
            if (dx != 0.0) {
                add_parameter((bounds.minimum.x - segment.start.x) / dx);
                add_parameter((bounds.maximum.x - segment.start.x) / dx);
            }
            if (dy != 0.0) {
                add_parameter((bounds.minimum.y - segment.start.y) / dy);
                add_parameter((bounds.maximum.y - segment.start.y) / dy);
            }
        } else {
            arc = arc_geometry(segment);
            const auto add_angle = [&](double angle) {
                const Vec2 point{arc->center.x + arc->radius * std::cos(angle),
                                 arc->center.y + arc->radius * std::sin(angle)};
                add_parameter(arc_parameter(*arc, point));
            };
            for (const auto x : {bounds.minimum.x, bounds.maximum.x}) {
                const auto ratio = (x - arc->center.x) / arc->radius;
                const auto margin = tolerance_metres / arc->radius;
                if (ratio >= -1.0 - margin && ratio <= 1.0 + margin) {
                    const auto angle = std::acos(std::clamp(ratio, -1.0, 1.0));
                    add_angle(angle);
                    add_angle(-angle);
                }
            }
            for (const auto y : {bounds.minimum.y, bounds.maximum.y}) {
                const auto ratio = (y - arc->center.y) / arc->radius;
                const auto margin = tolerance_metres / arc->radius;
                if (ratio >= -1.0 - margin && ratio <= 1.0 + margin) {
                    const auto angle = std::asin(std::clamp(ratio, -1.0, 1.0));
                    add_angle(angle);
                    add_angle(std::numbers::pi - angle);
                }
            }
        }

        std::sort(parameters.begin(), parameters.end());
        parameters.erase(std::unique(parameters.begin(), parameters.end(),
            [&](double left, double right) {
                return std::abs(left - right) <= parameter_tolerance;
            }), parameters.end());
        const auto point_at_parameter = [&](double parameter) {
            if (parameter <= parameter_tolerance) return segment.start;
            if (parameter >= 1.0 - parameter_tolerance) return segment.end;
            if (arc) return point_at(*arc, parameter);
            return Vec2{segment.start.x + (segment.end.x - segment.start.x) * parameter,
                        segment.start.y + (segment.end.y - segment.start.y) * parameter};
        };
        std::optional<double> retained_start;
        double retained_end = 0.0;
        const auto flush_retained = [&] {
            if (!retained_start) return;
            const auto start = point_at_parameter(*retained_start);
            const auto end = point_at_parameter(retained_end);
            if (distance(start, end) > tolerance_metres) {
                result.push_back({start, end,
                    segment.sweep_radians * (retained_end - *retained_start)});
            }
            retained_start.reset();
        };
        for (std::size_t index = 0; index + 1 < parameters.size(); ++index) {
            const auto first = parameters[index];
            const auto last = parameters[index + 1];
            if (last - first <= parameter_tolerance) continue;
            if (inside(point_at_parameter(std::midpoint(first, last)))) {
                if (!retained_start) retained_start = first;
                retained_end = last;
            } else {
                flush_retained();
            }
        }
        flush_retained();
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
                    if (left.sweep_radians == 0.0 && right.sweep_radians != 0.0 &&
                        line_arc_clearance_unresolved_or_within(left, right, tolerance_metres))
                        return true;
                    if (right.sweep_radians == 0.0 && left.sweep_radians != 0.0 &&
                        line_arc_clearance_unresolved_or_within(right, left, tolerance_metres))
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

Vec2 area_label_anchor(const Boundary& boundary) {
    const auto bounds = boundary_bounds(boundary);
    // Work near the source rather than multiplying large world coordinates.
    const auto origin = boundary.front().start;
    bool lines = true;
    double twice_area = 0.0, x_sum = 0.0, y_sum = 0.0;
    for (const auto& edge : boundary) {
        if (edge.sweep_radians != 0.0) lines = false;
        const auto a = edge.start - origin, b = edge.end - origin;
        const auto product = cross(a, b);
        twice_area += product;
        x_sum += (a.x + b.x) * product;
        y_sum += (a.y + b.y) * product;
    }
    if (lines && std::abs(twice_area) > default_geometry_tolerance_metres &&
        std::isfinite(x_sum) && std::isfinite(y_sum)) {
        const Vec2 anchor{origin.x + x_sum / (3.0 * twice_area),
                          origin.y + y_sum / (3.0 * twice_area)};
        if (std::isfinite(anchor.x) && std::isfinite(anchor.y)) return anchor;
    }
    return {std::midpoint(bounds.minimum.x, bounds.maximum.x),
            std::midpoint(bounds.minimum.y, bounds.maximum.y)};
}

}  // namespace sketch
