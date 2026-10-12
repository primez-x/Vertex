#include "sketch/assembly_profile_geometry.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace sketch {
namespace {

constexpr std::size_t hard_evaluation_limit = 1048576;
constexpr std::size_t hard_depth_limit = 32;
constexpr std::size_t initial_panels = 8;

bool finite(Vec2 value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

double checked(long double value, const char* message) {
    const auto result = static_cast<double>(value);
    if (!std::isfinite(value) || !std::isfinite(result)) {
        throw std::invalid_argument(message);
    }
    return result;
}

Vec2 checked_point(long double x, long double y) {
    return {checked(x, "affine profile X exceeds numeric range"),
            checked(y, "affine profile Y exceeds numeric range")};
}

using Exact = boost::multiprecision::cpp_rational;
using Integer = boost::multiprecision::cpp_int;

// These fixed-size expressions contain only binary64 operands: at most a
// handful of products/sums, with bounded binary exponents. They cannot acquire
// unbounded input-driven integer work. Exact dyadic arithmetic is the fallback
// for cancellation/range cases; do not depend on wider long double on Windows.
Exact binary64(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559 &&
        std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
    if (exponent == 0x7ff) throw std::invalid_argument("affine arithmetic operand must be finite");
    Integer significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (exponent != 0) significand += Integer{1} << 52;
    if (bits >> 63) significand = -significand;
    const int shift = exponent == 0 ? -1074 : static_cast<int>(exponent) - 1075;
    if (shift >= 0) return Exact{significand << shift};
    Exact result{significand};
    result /= Integer{1} << -shift;
    return result;
}

double checked_exact(const Exact& value, const char* message) {
    // Boost's rational_adaptor can leave an exact zero sum as 0/d with d != 1,
    // and `value == 0` then compares false. Test the numerator so msb() below
    // never sees zero.
    if (value.is_zero()) return 0.0;
    const bool negative = value < 0;
    const Exact magnitude = negative ? -value : value;
    constexpr std::uint64_t maximum_bits = 0x7fefffffffffffffULL;
    if (magnitude > binary64(std::bit_cast<double>(maximum_bits)))
        throw std::invalid_argument(message);
    const Integer numerator_value = numerator(magnitude);
    const Integer denominator_value = denominator(magnitude);
    int exponent = static_cast<int>(boost::multiprecision::msb(numerator_value)) -
        static_cast<int>(boost::multiprecision::msb(denominator_value));
    if (exponent >= 0) {
        if (numerator_value < (denominator_value << exponent)) --exponent;
    } else if ((numerator_value << -exponent) < denominator_value) --exponent;
    // Round directly on the target binary64 grid. Subnormals have a fixed
    // 2^-1074 quantum, rather than 53 significant bits followed by ldexp
    // rounding a second time. Integer division preserves midpoint ties.
    const int quantum = std::max(exponent - 52, -1074);
    const Integer scaled_numerator = quantum < 0 ?
        Integer{numerator_value << -quantum} : numerator_value;
    const Integer scaled_denominator = quantum > 0 ?
        Integer{denominator_value << quantum} : denominator_value;
    Integer significand = scaled_numerator / scaled_denominator;
    const Integer remainder = scaled_numerator % scaled_denominator;
    const Integer twice_remainder = remainder << 1;
    if (twice_remainder > scaled_denominator ||
        (twice_remainder == scaled_denominator && (significand & 1) != 0)) ++significand;
    // The rounded integer is <=2^53, so this conversion and the final exact
    // power-of-two scaling cannot introduce a second rounding on this grid.
    const auto result = std::ldexp(significand.convert_to<double>(), quantum);
    if (!std::isfinite(result)) throw std::invalid_argument(message);
    return negative ? -result : result;
}

Exact exact_determinant(const AssemblyProfilePlanarAffine& map) {
    return binary64(map.xx) * binary64(map.yy) - binary64(map.xy) * binary64(map.yx);
}

double determinant(const AssemblyProfilePlanarAffine& map) {
    const auto result = map.xy == 0.0 && map.yx == 0.0
        ? map.xx * map.yy
        : checked_exact(exact_determinant(map), "affine profile determinant exceeds numeric range");
    if (!std::isfinite(result)) throw std::invalid_argument("affine profile determinant exceeds numeric range");
    if (!(result > 0.0)) {
        throw std::invalid_argument("affine profile determinant must be positive and nonsingular");
    }
    return result;
}

double product_sum(double a, double b, double c, double d) {
    const auto first = a * b, second = c * d;
    const auto safe_product = [](double product, double x, double y) {
        return std::isfinite(product) && (x == 0.0 || y == 0.0 ||
            std::abs(product) >= 64.0 * std::numeric_limits<double>::min());
    };
    if (safe_product(first, a, b) && safe_product(second, c, d)) {
        const auto sum = first + second;
        if (std::isfinite(sum)) {
            const auto restored = sum - first;
            const auto residual = (first - (sum - restored)) + (second - restored);
            const auto first_error = std::fma(a, b, -first);
            const auto second_error = std::fma(c, d, -second);
            const auto result = sum + (residual + first_error + second_error);
            const auto magnitude = std::max(std::abs(first), std::abs(second));
            // Compensated normal-range arithmetic avoids big-integer work in
            // ordinary quadrature samples. Cancellation and range extremes
            // use the exact fallback, including products overflowing alone.
            if (std::isfinite(result) && (magnitude == 0.0 || std::abs(result) >
                64.0 * std::numeric_limits<double>::epsilon() * magnitude)) return result;
        }
    }
    return checked_exact(binary64(a) * binary64(b) + binary64(c) * binary64(d),
        "affine profile linear expression exceeds numeric range");
}

double inverse_coefficient(double numerator, const Exact& det) {
    const auto value = checked_exact(binary64(numerator) / det, "affine profile inverse exceeds numeric range");
    if (numerator != 0.0 && value == 0.0)
        throw std::invalid_argument("affine profile inverse coefficient underflows numeric range");
    return value;
}

std::array<double, 4> inverse_coefficients(const AssemblyProfilePlanarAffine& map) {
    if (map.xy == 0.0 && map.yx == 0.0) {
        const auto x = 1.0 / map.xx, y = 1.0 / map.yy;
        if (!std::isfinite(x) || !std::isfinite(y) || x == 0.0 || y == 0.0)
            throw std::invalid_argument("affine profile inverse exceeds numeric range");
        return {x, 0.0, 0.0, y};
    }
    const auto det = exact_determinant(map);
    return {inverse_coefficient(map.yy, det), inverse_coefficient(-map.xy, det),
        inverse_coefficient(-map.yx, det), inverse_coefficient(map.xx, det)};
}

Vec2 apply_direction(Vec2 value, const AssemblyProfilePlanarAffine& map) {
    if (map.xx == 1.0 && map.xy == 0.0 && map.yx == 0.0 && map.yy == 1.0) return value;
    if (map.xy == 0.0 && map.yx == 0.0) {
        return checked_point(static_cast<long double>(map.xx) * value.x,
                             static_cast<long double>(map.yy) * value.y);
    }
    return {product_sum(map.xx, value.x, map.xy, value.y),
        product_sum(map.yx, value.x, map.yy, value.y)};
}

// Admit the inexpensive diagonal path only when the source subtraction is
// exact. Otherwise retain its residual through multiplication and translation.
bool exact_difference(double a, double b, double& difference) {
    difference = a - b;
    if (!std::isfinite(difference)) return false;
    const auto restored_b = difference - a;
    return (a - (difference - restored_b)) + (-b - restored_b) == 0.0;
}

double mapped_coordinate(double point_x, double point_y, double displacement_x, double displacement_y,
    double source_x, double source_y, double target, double a, double b) {
    return checked_exact(binary64(target) + binary64(a) *
            (binary64(point_x) - binary64(source_x) + binary64(displacement_x)) +
        binary64(b) * (binary64(point_y) - binary64(source_y) + binary64(displacement_y)),
        "affine profile coordinate exceeds numeric range");
}

Vec2 apply_point(Vec2 value, const AssemblyProfilePlanarAffine& map) {
    if (map.xx == 1.0 && map.xy == 0.0 && map.yx == 0.0 && map.yy == 1.0 &&
        map.source_origin.x == map.target_origin.x && map.source_origin.y == map.target_origin.y) return value;
    if (map.xy == 0.0 && map.yx == 0.0) {
        double x{}, y{};
        if (exact_difference(value.x, map.source_origin.x, x) &&
            exact_difference(value.y, map.source_origin.y, y)) {
            return checked_point(std::fma(map.xx, x, map.target_origin.x),
                std::fma(map.yy, y, map.target_origin.y));
        }
    }
    return {mapped_coordinate(value.x, value.y, 0.0, 0.0, map.source_origin.x, map.source_origin.y,
            map.target_origin.x, map.xx, map.xy),
        mapped_coordinate(value.x, value.y, 0.0, 0.0, map.source_origin.x, map.source_origin.y,
            map.target_origin.y, map.yx, map.yy)};
}

void require_parameter(double parameter) {
    if (!std::isfinite(parameter) || parameter < 0.0 || parameter > 1.0) {
        throw std::invalid_argument("affine profile parameter must be in [0,1]");
    }
}

void require_boundary_count(const Boundary& boundary) {
    if (boundary.empty() || boundary.size() > assembly_profile_geometry_max_segments) {
        throw std::invalid_argument("affine profile boundary segment count is outside the work budget");
    }
}

Vec2 rotate(Vec2 value, double angle) {
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    return {std::fma(cosine, value.x, -sine * value.y),
            std::fma(sine, value.x, cosine * value.y)};
}

double sinc(double angle) {
    if (std::abs(angle) < 1e-4) {
        const auto squared = angle * angle;
        return 1.0 + squared * (-1.0 / 6.0 + squared * (1.0 / 120.0 - squared / 5040.0));
    }
    return std::sin(angle) / angle;
}

struct SourceSegment {
    const Segment& segment;
    double length;
    Vec2 initial_tangent;
};

SourceSegment prepare(const Segment& segment) {
    const auto length = segment_length(segment); // Shared finite/sweep/range admission.
    if (!std::isfinite(length) || !(length > 0.0)) {
        throw std::invalid_argument("affine profile source length must be finite and positive");
    }
    const Vec2 chord{segment.end.x - segment.start.x, segment.end.y - segment.start.y};
    const auto chord_length = std::hypot(chord.x, chord.y);
    const Vec2 unit{chord.x / chord_length, chord.y / chord_length};
    return {segment, length, rotate(unit, -segment.sweep_radians / 2.0)};
}

Vec2 point_at(const SourceSegment& source, const AssemblyProfilePlanarAffine& map, double u) {
    if (u == 0.0) return apply_point(source.segment.start, map);
    if (u == 1.0) return apply_point(source.segment.end, map);
    const auto half_angle = source.segment.sweep_radians * u / 2.0;
    const auto direction = rotate(source.initial_tangent, half_angle);
    const auto distance = static_cast<long double>(source.length) * u * sinc(half_angle);
    // Carry displacement directly through M and add it at the anchored start.
    // There is no center/radius subtraction even for extremely shallow arcs.
    const auto dx = source.segment.sweep_radians == 0.0
        ? static_cast<long double>(u) * (source.segment.end.x - source.segment.start.x) : distance * direction.x;
    const auto dy = source.segment.sweep_radians == 0.0
        ? static_cast<long double>(u) * (source.segment.end.y - source.segment.start.y) : distance * direction.y;
    if (map.xx == 1.0 && map.xy == 0.0 && map.yx == 0.0 && map.yy == 1.0 &&
        map.source_origin.x == map.target_origin.x && map.source_origin.y == map.target_origin.y)
        return checked_point(static_cast<long double>(source.segment.start.x) + dx,
            static_cast<long double>(source.segment.start.y) + dy);
    const auto displacement = checked_point(dx, dy);
    return {mapped_coordinate(source.segment.start.x, source.segment.start.y, displacement.x, displacement.y,
            map.source_origin.x, map.source_origin.y, map.target_origin.x, map.xx, map.xy),
        mapped_coordinate(source.segment.start.x, source.segment.start.y, displacement.x, displacement.y,
            map.source_origin.x, map.source_origin.y, map.target_origin.y, map.yx, map.yy)};
}

Vec2 tangent_at(const SourceSegment& source, const AssemblyProfilePlanarAffine& map, double u) {
    if (source.segment.sweep_radians == 0.0) {
        const auto result = apply_direction({source.segment.end.x - source.segment.start.x,
                                            source.segment.end.y - source.segment.start.y}, map);
        if (result.x == 0.0 && result.y == 0.0) {
            throw std::invalid_argument("affine profile tangent underflows numeric range");
        }
        return result;
    }
    const auto unit = rotate(source.initial_tangent, source.segment.sweep_radians * u);
    const auto mapped = apply_direction(unit, map);
    const auto result = checked_point(static_cast<long double>(source.length) * mapped.x,
                                     static_cast<long double>(source.length) * mapped.y);
    if (result.x == 0.0 && result.y == 0.0) {
        throw std::invalid_argument("affine profile tangent underflows numeric range");
    }
    return result;
}

void include_point(Bounds2& bounds, Vec2 point) {
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
}

Bounds2 bounds_of(const SourceSegment& source, const AssemblyProfilePlanarAffine& map) {
    const auto start = point_at(source, map, 0.0);
    Bounds2 result{start, start};
    include_point(result, point_at(source, map, 1.0));
    const auto sweep = source.segment.sweep_radians;
    if (sweep == 0.0) return result;
    const auto t0 = source.initial_tangent;
    const auto a = apply_direction(t0, map);
    const auto b = apply_direction({-t0.y, t0.x}, map);
    const std::array<std::array<double, 2>, 2> components{{{a.x, b.x}, {a.y, b.y}}};
    for (const auto& component : components) {
        // Derivative component is A*cos(theta)+B*sin(theta). All its roots
        // inside the signed sweep interval are extrema. With |sweep|<2*pi,
        // the fixed seven candidates cover every root without unbounded loops.
        const auto phase = std::atan2(-component[0], component[1]);
        for (int turn = -3; turn <= 3; ++turn) {
            const auto angle = phase + static_cast<double>(turn) * std::numbers::pi;
            const auto u = angle / sweep;
            if (u > 0.0 && u < 1.0) include_point(result, point_at(source, map, u));
        }
    }
    return result;
}

void validate_options(const AssemblyProfileLengthOptions& options) {
    if (!std::isfinite(options.absolute_tolerance_metres) || !(options.absolute_tolerance_metres > 0.0) ||
        !std::isfinite(options.relative_tolerance) || !(options.relative_tolerance > 0.0) ||
        options.maximum_evaluations == 0 || options.maximum_evaluations > hard_evaluation_limit ||
        options.maximum_subdivision_depth == 0 || options.maximum_subdivision_depth > hard_depth_limit) {
        throw std::invalid_argument("affine profile quadrature options are outside the finite work budget");
    }
}

double measurement_tolerance(double absolute, double relative, double length) {
    return checked(static_cast<long double>(absolute) + static_cast<long double>(relative) * length,
                   "affine profile measurement tolerance exceeds numeric range");
}

struct Sum {
    double value{}, compensation{};
    void add(double term) {
        const auto adjusted = term - compensation;
        const auto next = checked(static_cast<long double>(value) + adjusted,
                                  "affine profile measurement sum exceeds numeric range");
        compensation = (next - value) - adjusted;
        if (!std::isfinite(compensation)) {
            throw std::invalid_argument("affine profile measurement compensation exceeds numeric range");
        }
        value = next;
    }
};

struct QuadratureWork {
    const AssemblyProfileLengthOptions& options;
    std::size_t evaluations{};
    double speed(const SourceSegment& source, const AssemblyProfilePlanarAffine& map, double u) {
        if (evaluations == options.maximum_evaluations) {
            throw std::invalid_argument("affine profile quadrature evaluation budget exhausted");
        }
        ++evaluations;
        const auto direction = apply_direction(
            rotate(source.initial_tangent, source.segment.sweep_radians * u), map);
        const auto value = checked(static_cast<long double>(source.length) *
                                   std::hypot(direction.x, direction.y),
                                   "affine profile speed exceeds numeric range");
        if (!(value > 0.0)) throw std::invalid_argument("affine profile speed underflows numeric range");
        return value;
    }
};

double simpson(double width, double first, double middle, double last) {
    return checked(static_cast<long double>(width) *
                       (static_cast<long double>(first) + 4.0L * middle + last) / 6.0L,
                   "affine profile quadrature exceeds numeric range");
}

struct Panel {
    double first_u, last_u, first_speed, middle_speed, last_speed, coarse, absolute_budget;
    std::size_t depth;
};

AssemblyProfileLengthResult length_of(const SourceSegment& source, const AssemblyProfilePlanarAffine& map,
                                      double absolute_budget, QuadratureWork& work) {
    if (map.xx == map.yy && map.xy == -map.yx) {
        const auto value = checked(static_cast<long double>(source.length) * std::hypot(map.xx, map.yx),
                                   "affine profile similarity length exceeds numeric range");
        if (!(value > 0.0)) throw std::invalid_argument("affine profile length underflows numeric range");
        return {value, 0.0, 0};
    }
    if (source.segment.sweep_radians == 0.0) {
        const auto tangent = tangent_at(source, map, 0.0);
        const auto value = std::hypot(tangent.x, tangent.y);
        if (!std::isfinite(value) || !(value > 0.0)) {
            throw std::invalid_argument("affine profile line length exceeds numeric range");
        }
        return {value, 0.0, 0};
    }

    const auto initial_evaluations = work.evaluations;
    Sum length, error;
    std::vector<Panel> stack;
    stack.reserve(hard_depth_limit + 1);
    for (std::size_t index = 0; index < initial_panels; ++index) {
        const auto a = static_cast<double>(index) / initial_panels;
        const auto b = static_cast<double>(index + 1) / initial_panels;
        const auto m = (a + b) / 2.0;
        const auto fa = work.speed(source, map, a);
        const auto fm = work.speed(source, map, m);
        const auto fb = work.speed(source, map, b);
        stack.push_back({a, b, fa, fm, fb, simpson(b - a, fa, fm, fb),
                         absolute_budget / initial_panels, 0});
        while (!stack.empty()) {
            const auto panel = stack.back();
            stack.pop_back();
            const auto midpoint = (panel.first_u + panel.last_u) / 2.0;
            const auto left_midpoint = (panel.first_u + midpoint) / 2.0;
            const auto right_midpoint = (midpoint + panel.last_u) / 2.0;
            if (!(panel.first_u < left_midpoint && left_midpoint < midpoint &&
                  midpoint < right_midpoint && right_midpoint < panel.last_u)) {
                throw std::invalid_argument("affine profile quadrature subdivision cannot be represented");
            }
            const auto fl = work.speed(source, map, left_midpoint);
            const auto fr = work.speed(source, map, right_midpoint);
            const auto left = simpson(midpoint - panel.first_u, panel.first_speed, fl, panel.middle_speed);
            const auto right = simpson(panel.last_u - midpoint, panel.middle_speed, fr, panel.last_speed);
            const auto fine = checked(static_cast<long double>(left) + right,
                                       "affine profile quadrature exceeds numeric range");
            const auto correction = (fine - panel.coarse) / 15.0;
            const auto estimate = checked(static_cast<long double>(fine) + correction,
                                           "affine profile quadrature correction exceeds numeric range");
            const auto estimated_error = checked(static_cast<long double>(std::abs(correction)) +
                64.0L * std::numeric_limits<double>::epsilon() * fine,
                "affine profile quadrature error exceeds numeric range");
            const auto tolerance = measurement_tolerance(panel.absolute_budget, work.options.relative_tolerance, estimate);
            if (estimate > 0.0 && estimated_error <= tolerance) {
                length.add(estimate);
                error.add(estimated_error);
                continue;
            }
            if (panel.depth == work.options.maximum_subdivision_depth) {
                throw std::invalid_argument("affine profile quadrature did not converge within the depth budget");
            }
            const auto next_budget = panel.absolute_budget / 2.0;
            if (!(next_budget > 0.0)) {
                throw std::invalid_argument("affine profile quadrature tolerance underflows numeric range");
            }
            // LIFO: right is pushed first so traversal is deterministic left-first.
            stack.push_back({midpoint, panel.last_u, panel.middle_speed, fr, panel.last_speed,
                             right, next_budget, panel.depth + 1});
            stack.push_back({panel.first_u, midpoint, panel.first_speed, fl, panel.middle_speed,
                             left, next_budget, panel.depth + 1});
        }
    }
    if (!(length.value > 0.0) || error.value > measurement_tolerance(
        absolute_budget, work.options.relative_tolerance, length.value)) {
        throw std::invalid_argument("affine profile quadrature aggregate tolerance was not met");
    }
    return {length.value, error.value, work.evaluations - initial_evaluations};
}

} // namespace

bool AssemblyProfilePlanarAffine::operator==(const AssemblyProfilePlanarAffine& other) const noexcept {
    return source_origin.x == other.source_origin.x && source_origin.y == other.source_origin.y &&
        target_origin.x == other.target_origin.x && target_origin.y == other.target_origin.y &&
        xx == other.xx && xy == other.xy && yx == other.yx && yy == other.yy;
}

void validate_assembly_profile_affine(const AssemblyProfilePlanarAffine& affine) {
    if (!finite(affine.source_origin) || !finite(affine.target_origin) ||
        !std::isfinite(affine.xx) || !std::isfinite(affine.xy) ||
        !std::isfinite(affine.yx) || !std::isfinite(affine.yy)) {
        throw std::invalid_argument("affine profile map must be finite");
    }
    (void)determinant(affine);
    (void)inverse_coefficients(affine);
}

double assembly_profile_affine_determinant(const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    return determinant(affine);
}

Vec2 assembly_profile_affine_point(Vec2 point, const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    if (!finite(point)) throw std::invalid_argument("affine profile source point must be finite");
    return apply_point(point, affine);
}

Vec2 assembly_profile_affine_direction(Vec2 direction, const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    if (!finite(direction)) throw std::invalid_argument("affine profile direction must be finite");
    return apply_direction(direction, affine);
}

Vec2 assembly_profile_affine_inverse_point(Vec2 point, const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    if (!finite(point)) throw std::invalid_argument("affine profile inverse point must be finite");
    if (affine.xx == 1.0 && affine.xy == 0.0 && affine.yx == 0.0 && affine.yy == 1.0 &&
        affine.source_origin.x == affine.target_origin.x && affine.source_origin.y == affine.target_origin.y)
        return point;
    const Exact x = binary64(point.x) - binary64(affine.target_origin.x);
    const Exact y = binary64(point.y) - binary64(affine.target_origin.y);
    const auto det = exact_determinant(affine);
    // Keep the original coefficients and exact determinant through the whole
    // inverse expression; rounded coefficients can destroy cancellation.
    return {checked_exact(binary64(affine.source_origin.x) +
            (binary64(affine.yy) * x - binary64(affine.xy) * y) / det,
            "affine profile inverse X exceeds numeric range"),
        checked_exact(binary64(affine.source_origin.y) +
            (binary64(affine.xx) * y - binary64(affine.yx) * x) / det,
            "affine profile inverse Y exceeds numeric range")};
}

AssemblyProfilePlanarAffine compose_assembly_profile_affine(
    const AssemblyProfilePlanarAffine& outer, const AssemblyProfilePlanarAffine& inner) {
    validate_assembly_profile_affine(outer);
    validate_assembly_profile_affine(inner);
    AssemblyProfilePlanarAffine result;
    result.source_origin = inner.source_origin;
    result.target_origin = apply_point(inner.target_origin, outer);
    result.xx = product_sum(outer.xx, inner.xx, outer.xy, inner.yx);
    result.xy = product_sum(outer.xx, inner.xy, outer.xy, inner.yy);
    result.yx = product_sum(outer.yx, inner.xx, outer.yy, inner.yx);
    result.yy = product_sum(outer.yx, inner.xy, outer.yy, inner.yy);
    validate_assembly_profile_affine(result);
    return result;
}

Vec2 assembly_profile_segment_point(const Segment& source, const AssemblyProfilePlanarAffine& affine, double parameter) {
    validate_assembly_profile_affine(affine);
    require_parameter(parameter);
    return point_at(prepare(source), affine, parameter);
}

Vec2 assembly_profile_segment_tangent(const Segment& source, const AssemblyProfilePlanarAffine& affine, double parameter) {
    validate_assembly_profile_affine(affine);
    require_parameter(parameter);
    return tangent_at(prepare(source), affine, parameter);
}

Bounds2 assembly_profile_segment_bounds(const Segment& source, const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    return bounds_of(prepare(source), affine);
}

Boundary reexpress_assembly_profile_boundary(const Boundary& boundary,
    const AssemblyProfilePlanarAffine& source, const AssemblyProfilePlanarAffine& destination) {
    validate_assembly_profile_affine(source);
    validate_assembly_profile_affine(destination);
    require_boundary_count(boundary);
    for (const auto& segment : boundary) (void)prepare(segment);

    // Compare adj(destination) * source exactly. Its common positive
    // determinant divisor cannot affect the proper-similarity equalities.
    if (std::any_of(boundary.begin(), boundary.end(),
        [](const Segment& segment) { return segment.sweep_radians != 0.0; })) {
        const Exact a = binary64(destination.yy) * binary64(source.xx) -
            binary64(destination.xy) * binary64(source.yx);
        const Exact b = binary64(destination.yy) * binary64(source.xy) -
            binary64(destination.xy) * binary64(source.yy);
        const Exact c = binary64(destination.xx) * binary64(source.yx) -
            binary64(destination.yx) * binary64(source.xx);
        const Exact d = binary64(destination.xx) * binary64(source.yy) -
            binary64(destination.yx) * binary64(source.xy);
        if (a != d || b != -c)
            throw std::invalid_argument("Circular cutouts require an exact relative similarity between profile mappings. Replace the whole outer profile to use its mapping, or edit the cutout in local source coordinates.");
    }
    if (source == destination) return boundary;
    const Exact det = exact_determinant(destination);
    const auto point = [&](Vec2 value) -> Vec2 {
        const Exact x = binary64(value.x) - binary64(source.source_origin.x);
        const Exact y = binary64(value.y) - binary64(source.source_origin.y);
        // Keep the world displacement exact, including distant anchors and
        // individually overflowing products, until the destination conversion.
        const Exact world_x = binary64(source.target_origin.x) - binary64(destination.target_origin.x) +
            binary64(source.xx) * x + binary64(source.xy) * y;
        const Exact world_y = binary64(source.target_origin.y) - binary64(destination.target_origin.y) +
            binary64(source.yx) * x + binary64(source.yy) * y;
        return {checked_exact(binary64(destination.source_origin.x) +
                (binary64(destination.yy) * world_x - binary64(destination.xy) * world_y) / det,
                "Reexpressed profile X exceeds numeric range"),
            checked_exact(binary64(destination.source_origin.y) +
                (binary64(destination.xx) * world_y - binary64(destination.yx) * world_x) / det,
                "Reexpressed profile Y exceeds numeric range")};
    };
    auto result = boundary;
    for (auto& segment : result) {
        segment.start = point(segment.start);
        segment.end = point(segment.end);
        (void)prepare(segment);
    }
    return result;
}

Bounds2 assembly_profile_boundary_bounds(const Boundary& source, const AssemblyProfilePlanarAffine& affine) {
    validate_assembly_profile_affine(affine);
    require_boundary_count(source);
    auto result = bounds_of(prepare(source.front()), affine);
    for (std::size_t index = 1; index < source.size(); ++index) {
        const auto bounds = bounds_of(prepare(source[index]), affine);
        include_point(result, bounds.minimum);
        include_point(result, bounds.maximum);
    }
    return result;
}

Boundary assembly_profile_boundary_display_path(const Boundary& source,
                                                const AssemblyProfilePlanarAffine& affine,
                                                double maximum_deviation_metres,
                                                std::size_t maximum_segments) {
    validate_assembly_profile_affine(affine);
    require_boundary_count(source);
    if (!std::isfinite(maximum_deviation_metres) || !(maximum_deviation_metres > 0.0) ||
        maximum_segments == 0 || maximum_segments > 65536 || source.size() > maximum_segments) {
        throw std::invalid_argument("affine profile display deviation or segment budget is invalid");
    }
    Boundary result;
    result.reserve(source.size());
    for (const auto& segment : source) {
        const auto prepared = prepare(segment);
        std::size_t pieces = 1;
        if (segment.sweep_radians != 0.0) {
            // ||p''(u)|| <= L*|sweep|*||M||_F. A chord on an interval
            // of width h deviates by at most that bound*h*h/8. Retain
            // products exactly and round the upper bound outward once.
            const auto matrix_norm = std::hypot(std::hypot(affine.xx, affine.xy),
                                                std::hypot(affine.yx, affine.yy));
            if (!std::isfinite(matrix_norm))
                throw std::invalid_argument("affine profile display curvature exceeds numeric range");
            const auto norm_upper = std::nextafter(matrix_norm, std::numeric_limits<double>::infinity());
            const auto curvature = checked_exact(binary64(segment_length(segment)) *
                binary64(std::abs(segment.sweep_radians)) * binary64(norm_upper),
                "affine profile display curvature exceeds numeric range");
            const auto curvature_upper = std::nextafter(curvature, std::numeric_limits<double>::infinity());
            const auto needed = std::ceil(std::nextafter(
                std::sqrt(curvature_upper) / (std::sqrt(maximum_deviation_metres) * std::sqrt(8.0)),
                std::numeric_limits<double>::infinity()));
            if (!std::isfinite(needed) || needed > static_cast<double>(maximum_segments - result.size()))
                throw std::invalid_argument("affine profile display exceeds its segment budget");
            pieces = static_cast<std::size_t>(std::max(1.0, needed));
        }
        if (pieces > maximum_segments - result.size())
            throw std::invalid_argument("affine profile display exceeds its segment budget");
        auto start = point_at(prepared, affine, 0.0);
        for (std::size_t index = 1; index <= pieces; ++index) {
            const auto end = point_at(prepared, affine, static_cast<double>(index) / static_cast<double>(pieces));
            if (start.x == end.x && start.y == end.y)
                throw std::invalid_argument("affine profile display chord is unrepresentable");
            result.push_back({start, end, 0.0});
            start = end;
        }
    }
    return result;
}

double assembly_profile_closed_boundary_signed_area(const Boundary& source, const AssemblyProfilePlanarAffine& affine,
                                                    double tolerance_metres) {
    validate_assembly_profile_affine(affine);
    require_boundary_count(source);
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto end = source[index].end;
        const auto next = source[(index + 1) % source.size()].start;
        if (end.x != next.x || end.y != next.y) {
            throw std::invalid_argument("affine profile area requires exactly connected closed geometry");
        }
    }
    const auto diagnostics = validate_boundary(source, tolerance_metres);
    if (!diagnostics.empty()) {
        throw std::invalid_argument("affine profile area requires an admitted closed boundary: " +
                                    diagnostics.front().message);
    }
    // Verify the map can represent the actual outline before returning a quantity.
    (void)assembly_profile_boundary_bounds(source, affine);
    const auto source_area = signed_area(source);
    const auto result = checked_exact(exact_determinant(affine) * binary64(source_area),
        "affine profile area exceeds numeric range");
    if (source_area != 0.0 && result == 0.0) {
        throw std::invalid_argument("affine profile area underflows numeric range");
    }
    return result;
}

double assembly_profile_affine_scaled_area(double admitted_source_area,
                                          const AssemblyProfilePlanarAffine& affine,
                                          std::span<const double> positive_factors) {
    validate_assembly_profile_affine(affine);
    if (!std::isfinite(admitted_source_area) || !(admitted_source_area > 0.0) || positive_factors.size() > 8) {
        throw std::invalid_argument("affine profile scaled area requires a positive area and at most eight factors");
    }
    Exact quantity = exact_determinant(affine) * binary64(admitted_source_area);
    for (const auto factor : positive_factors) {
        if (!std::isfinite(factor) || !(factor > 0.0)) {
            throw std::invalid_argument("affine profile scaled area requires positive finite factors");
        }
        quantity *= binary64(factor);
    }
    const auto result = checked_exact(quantity, "affine profile scaled area exceeds numeric range");
    if (!(result > 0.0)) {
        throw std::invalid_argument("affine profile scaled area underflows numeric range");
    }
    return result;
}

AssemblyProfileLengthResult assembly_profile_segment_length(const Segment& source, const AssemblyProfilePlanarAffine& affine,
                                                             const AssemblyProfileLengthOptions& options) {
    validate_assembly_profile_affine(affine);
    validate_options(options);
    const auto prepared = prepare(source);
    (void)bounds_of(prepared, affine);
    QuadratureWork work{options};
    return length_of(prepared, affine, options.absolute_tolerance_metres, work);
}

AssemblyProfileLengthResult assembly_profile_boundary_perimeter(const Boundary& source, const AssemblyProfilePlanarAffine& affine,
                                                                 const AssemblyProfileLengthOptions& options) {
    validate_assembly_profile_affine(affine);
    validate_options(options);
    require_boundary_count(source);
    QuadratureWork work{options};
    Sum length, error;
    const auto budget = options.absolute_tolerance_metres / static_cast<double>(source.size());
    if (!(budget > 0.0)) throw std::invalid_argument("affine profile perimeter tolerance underflows numeric range");
    for (const auto& segment : source) {
        const auto prepared = prepare(segment);
        (void)bounds_of(prepared, affine);
        const auto measured = length_of(prepared, affine, budget, work);
        length.add(measured.length_metres);
        error.add(measured.estimated_error_metres);
    }
    if (error.value > measurement_tolerance(options.absolute_tolerance_metres, options.relative_tolerance, length.value)) {
        throw std::invalid_argument("affine profile perimeter aggregate tolerance was not met");
    }
    return {length.value, error.value, work.evaluations};
}

} // namespace sketch
