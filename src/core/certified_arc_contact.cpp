#include "certified_arc_contact.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <numbers>
#include <utility>

namespace sketch::detail {
namespace {

using Integer = boost::multiprecision::cpp_int;
using Rational = boost::multiprecision::cpp_rational;

struct Exhausted {};
struct Unresolved {};

// Bound both work and the size of intermediate integer products. The checks
// precede rational operations, so an adversarial input cannot first allocate an
// unbounded product and only then discover that its budget has been exceeded.
struct Arithmetic {
    static constexpr unsigned maximum_bits = 32768;
    static constexpr unsigned maximum_operations = 20000;
    unsigned operations{};

    static unsigned bits(const Integer& value) {
        if (value == 0) return 0;
        const Integer magnitude = value < 0 ? -value : value;
        return boost::multiprecision::msb(magnitude) + 1;
    }

    void charge(const Rational& a, const Rational& b) {
        if (++operations > maximum_operations) throw Exhausted{};
        const auto width = [](const Rational& value) {
            return std::max(bits(numerator(value)), bits(denominator(value)));
        };
        // Addition, multiplication and division need at most this many bits
        // in each unreduced numerator/denominator (plus an addition carry).
        if (width(a) + width(b) + 1 > maximum_bits) throw Exhausted{};
    }

    Rational add(const Rational& a, const Rational& b) { charge(a, b); return a + b; }
    Rational sub(const Rational& a, const Rational& b) { charge(a, b); return a - b; }
    Rational mul(const Rational& a, const Rational& b) { charge(a, b); return a * b; }
    Rational div(const Rational& a, const Rational& b) {
        charge(a, b);
        if (b == 0) throw Exhausted{};
        return a / b;
    }
};

Rational absolute(const Rational& value) { return value < 0 ? -value : value; }

Rational binary64(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::radix == 2 &&
                  std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::min_exponent == -1021 &&
                  std::numeric_limits<double>::max_exponent == 1024);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
    if (exponent == 0x7ff) throw Exhausted{};
    Integer significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (exponent != 0) significand += Integer{1} << 52;
    if (bits >> 63) significand = -significand;
    const int shift = exponent == 0 ? -1074 : static_cast<int>(exponent) - 1075;
    if (shift >= 0) return Rational{significand << shift};
    Rational result{significand};
    result /= Integer{1} << -shift;
    return result;
}

struct Interval {
    Rational low;
    Rational high;
    Interval() : low(0), high(0) {}
    explicit Interval(const Rational& exact) : low(exact), high(exact) {}
    Interval(Rational minimum, Rational maximum)
        : low(std::move(minimum)), high(std::move(maximum)) {}
};

Interval add(Arithmetic& work, const Interval& a, const Interval& b) {
    return {work.add(a.low, b.low), work.add(a.high, b.high)};
}
Interval negate(const Interval& a) { return {-a.high, -a.low}; }
Interval sub(Arithmetic& work, const Interval& a, const Interval& b) {
    return add(work, a, negate(b));
}
Interval mul(Arithmetic& work, const Interval& a, const Interval& b) {
    const std::array<Rational, 4> products{
        work.mul(a.low, b.low), work.mul(a.low, b.high),
        work.mul(a.high, b.low), work.mul(a.high, b.high)};
    return {*std::min_element(products.begin(), products.end()),
            *std::max_element(products.begin(), products.end())};
}
Interval square(Arithmetic& work, const Interval& a) {
    const auto low_square = work.mul(a.low, a.low);
    const auto high_square = work.mul(a.high, a.high);
    return {a.low <= 0 && a.high >= 0 ? Rational{0} : std::min(low_square, high_square),
            std::max(low_square, high_square)};
}
Interval div(Arithmetic& work, const Interval& a, const Interval& b) {
    if (b.low <= 0 && b.high >= 0) throw Exhausted{};
    return mul(work, a, {work.div(1, b.high), work.div(1, b.low)});
}

struct Point { Interval x; Interval y; };
Point add(Arithmetic& work, const Point& a, const Point& b) {
    return {add(work, a.x, b.x), add(work, a.y, b.y)};
}
Point sub(Arithmetic& work, const Point& a, const Point& b) {
    return {sub(work, a.x, b.x), sub(work, a.y, b.y)};
}
Point mul(Arithmetic& work, const Point& a, const Interval& scalar) {
    return {mul(work, a.x, scalar), mul(work, a.y, scalar)};
}
Point perpendicular(const Point& point) { return {negate(point.y), point.x}; }
Interval dot(Arithmetic& work, const Point& a, const Point& b) {
    return add(work, mul(work, a.x, b.x), mul(work, a.y, b.y));
}
Interval norm_squared(Arithmetic& work, const Point& a) {
    return add(work, square(work, a.x), square(work, a.y));
}
Point exact_point(Vec2 value) { return {Interval{binary64(value.x)}, Interval{binary64(value.y)}}; }

// Taylor's theorem uses real derivatives bounded by one. Include the zero
// coefficient after the last retained term: the next nonzero term's absolute
// magnitude is a rigorous remainder bound, without a libm accuracy assumption.
std::array<Interval, 2> half_angle(Arithmetic& work, double sweep, unsigned terms) {
    if (std::abs(sweep) == std::numbers::pi)
        return {Interval{Rational{sweep > 0 ? 1 : -1}}, Interval{Rational{0}}};
    const auto x = work.div(binary64(sweep), 2);
    const Rational negative_square = -work.mul(x, x);
    Rational sine{0}, cosine{0}, sine_term{x}, cosine_term{1};
    for (unsigned index = 0; index < terms; ++index) {
        sine = work.add(sine, sine_term);
        cosine = work.add(cosine, cosine_term);
        sine_term = work.div(work.mul(sine_term, negative_square),
                             Rational{(2 * index + 2) * (2 * index + 3)});
        cosine_term = work.div(work.mul(cosine_term, negative_square),
                               Rational{(2 * index + 1) * (2 * index + 2)});
    }
    return {Interval{work.sub(sine, absolute(sine_term)), work.add(sine, absolute(sine_term))},
            Interval{work.sub(cosine, absolute(cosine_term)), work.add(cosine, absolute(cosine_term))}};
}

struct Circle { Interval s; Point b; Interval c; Point start; Point chord; double sweep; };
Circle circle(Arithmetic& work, const Segment& segment, const Point& origin, unsigned terms) {
    const auto start = sub(work, exact_point(segment.start), origin);
    const auto chord = sub(work, exact_point(segment.end), exact_point(segment.start));
    const auto angle = half_angle(work, segment.sweep_radians, terms);
    const auto v = add(work, mul(work, chord, angle[0]),
                       mul(work, perpendicular(chord), angle[1]));
    // F(X)=S|X-P|^2-V.(X-P)=S|X|^2+B.X+C in the exact local frame.
    const auto b = sub(work, mul(work, start, mul(work, Interval{Rational{-2}}, angle[0])), v);
    const auto c = add(work, mul(work, angle[0], norm_squared(work, start)), dot(work, v, start));
    return {angle[0], b, c, start, chord, segment.sweep_radians};
}

Rational power_two(int exponent) {
    if (exponent < -16384 || exponent > 16384) throw Exhausted{};
    if (exponent >= 0) return Rational{Integer{1} << exponent};
    Rational result{1};
    result /= Integer{1} << -exponent;
    return result;
}

// A fixed number of exact bisections gives an outward sqrt enclosure. Very
// large dynamic ranges can fail the final spatial tolerance instead of silently
// rounding away a second root.
Interval sqrt_bounds(Arithmetic& work, const Interval& value) {
    if (value.low < 0) throw Exhausted{};
    const auto bound_one = [&](const Rational& exact) {
        if (exact == 0) return Interval{Rational{0}};
        const int exponent = static_cast<int>(Arithmetic::bits(numerator(exact))) -
                             static_cast<int>(Arithmetic::bits(denominator(exact)));
        // exact < 2^(exponent+1); floor(exponent/2)+1 bounds its sqrt.
        const int upper_exponent = exponent >= 0 ? exponent / 2 + 1 : (exponent - 1) / 2 + 1;
        Rational low{0}, high{power_two(upper_exponent)};
        for (unsigned index = 0; index < 192; ++index) {
            const auto middle = work.div(work.add(low, high), 2);
            const auto squared = work.mul(middle, middle);
            if (squared == exact) return Interval{middle};
            if (squared < exact) low = middle;
            else high = middle;
        }
        return Interval{low, high};
    };
    return {bound_one(value.low).low, bound_one(value.high).high};
}

enum class Membership { inside, outside, uncertain };
Membership membership(Arithmetic& work, const Circle& arc, const Point& point,
                      const Rational& tolerance_squared) {
    const auto offset = sub(work, point, arc.start);
    auto side = dot(work, perpendicular(arc.chord), offset);
    if (arc.sweep < 0) side = negate(side);
    // The directed chord cuts the supporting circle into the requested arc
    // (right side for positive sweep) and its complement, also for major arcs.
    if (side.high <= 0) return Membership::inside;
    const auto end_offset = sub(work, offset, arc.chord);
    if (norm_squared(work, offset).high <= tolerance_squared ||
        norm_squared(work, end_offset).high <= tolerance_squared) return Membership::inside;
    if (side.low > 0) {
        // A root outside the selected side can still be within endpoint
        // tolerance. Exclude it only after both endpoint envelopes are clear.
        if (norm_squared(work, offset).low > tolerance_squared &&
            norm_squared(work, end_offset).low > tolerance_squared) return Membership::outside;
    }
    return Membership::uncertain;
}

// Binary64's positive finite bit patterns are ordered. Search that finite grid
// using exact rationals, then round to nearest/ties-to-even. No unchecked
// convert_to<double>, overflow, or host rounding mode enters this certificate.
double rounded(Arithmetic& work, const Rational& exact) {
    const bool negative = exact < 0;
    const auto magnitude = absolute(exact);
    constexpr std::uint64_t largest = 0x7fefffffffffffffULL;
    if (magnitude > binary64(std::bit_cast<double>(largest))) throw Unresolved{};
    std::uint64_t low = 0, high = largest;
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        work.charge(magnitude, binary64(std::bit_cast<double>(middle)));
        if (binary64(std::bit_cast<double>(middle)) <= magnitude) low = middle;
        else high = middle - 1;
    }
    auto chosen = low;
    const auto floor = binary64(std::bit_cast<double>(low));
    if (floor != magnitude && low < largest) {
        const auto ceiling = binary64(std::bit_cast<double>(low + 1));
        const auto below = work.sub(magnitude, floor);
        const auto above = work.sub(ceiling, magnitude);
        if (above < below || (above == below && (low & 1))) chosen = low + 1;
    }
    if (negative) chosen |= std::uint64_t{1} << 63;
    return std::bit_cast<double>(chosen);
}

Vec2 enclosed_point(Arithmetic& work, const Point& point, const Rational& tolerance_squared) {
    const Vec2 output{rounded(work, work.div(work.add(point.x.low, point.x.high), 2)),
                      rounded(work, work.div(work.add(point.y.low, point.y.high), 2))};
    const auto error = sub(work, point, exact_point(output));
    // Reserve most of the caller tolerance for topology classification. The
    // complete enclosure, including binary64 output rounding, must fit here.
    if (norm_squared(work, error).high > work.div(tolerance_squared, 64)) throw Unresolved{};
    return output;
}

bool endpoint_classification(Arithmetic& work, const Point& root, Vec2 output,
                             const std::array<Vec2, 4>& endpoints, double tolerance,
                             const Rational& tolerance_squared) {
    for (const auto endpoint : endpoints) {
        const auto separation = norm_squared(work, sub(work, root, exact_point(endpoint)));
        // Match the downstream distance predicate exactly, then require its
        // near/far result to hold for every possible actual root in the box.
        // A small output-error bound alone cannot protect a tolerance boundary.
        const bool rounded_near = std::hypot(output.x - endpoint.x, output.y - endpoint.y) <= tolerance;
        if (rounded_near ? separation.high > tolerance_squared : separation.low <= tolerance_squared)
            return false;
    }
    return true;
}

CertifiedArcContact attempt(const Segment& left_source, const Segment& right_source,
                            double tolerance, unsigned terms) {
    Arithmetic work;
    const auto tolerance_exact = binary64(tolerance);
    const auto tolerance_squared = work.mul(tolerance_exact, tolerance_exact);
    const auto origin = exact_point(left_source.start);
    const auto left = circle(work, left_source, origin, terms);
    const auto right = circle(work, right_source, origin, terms);
    if ((left.s.low <= 0 && left.s.high >= 0) ||
        (right.s.low <= 0 && right.s.high >= 0)) return {};
    const auto n = sub(work, mul(work, left.b, right.s), mul(work, right.b, left.s));
    const auto k = sub(work, mul(work, left.c, right.s), mul(work, right.c, left.s));
    const auto h = norm_squared(work, n);
    // A zero/uncertain radical axis may mean coincidence. It does not define
    // a unique root, and must never manufacture a contact.
    if (h.low <= 0) return {};
    const auto jn = perpendicular(n);
    const auto a = left.s;
    const auto b = dot(work, left.b, jn);
    const auto c = add(work, sub(work, mul(work, left.s, square(work, k)),
                               mul(work, k, dot(work, left.b, n))), mul(work, left.c, h));
    const auto discriminant = sub(work, square(work, b),
                                  mul(work, Interval{Rational{4}}, mul(work, a, c)));
    if (discriminant.high < 0) return {CertifiedArcContactKind::none};
    const bool zero = discriminant.low == 0 && discriminant.high == 0;
    if (!zero && discriminant.low <= 0) return {};
    const auto root = sqrt_bounds(work, discriminant);
    const auto denominator = mul(work, Interval{Rational{2}}, a);
    const auto base = mul(work, n, negate(k));
    std::array<Point, 2> accepted;
    std::size_t count = 0;
    for (unsigned index = 0; index < (zero ? 1u : 2u); ++index) {
        const auto t = div(work, add(work, negate(b), index == 0 ? root : negate(root)), denominator);
        const auto point = mul(work, add(work, base, mul(work, jn, t)), div(work, Interval{Rational{1}}, h));
        const auto on_left = membership(work, left, point, tolerance_squared);
        const auto on_right = membership(work, right, point, tolerance_squared);
        if (on_left == Membership::outside || on_right == Membership::outside) continue;
        if (on_left == Membership::uncertain || on_right == Membership::uncertain) return {};
        accepted[count++] = point;
    }
    if (count == 0) return {CertifiedArcContactKind::none};
    CertifiedArcContact result{CertifiedArcContactKind::points};
    const std::array endpoints{left_source.start, left_source.end, right_source.start, right_source.end};
    std::array<Point, 2> world;
    for (std::size_t index = 0; index < count; ++index) {
        world[index] = add(work, accepted[index], origin);
        result.points[index] = enclosed_point(work, world[index], tolerance_squared);
        if (!endpoint_classification(work, world[index], result.points[index], endpoints,
                                     tolerance, tolerance_squared)) throw Unresolved{};
    }
    if (count == 2 &&
        norm_squared(work, sub(work, accepted[0], accepted[1])).high <= tolerance_squared &&
        norm_squared(work, sub(work, world[1], exact_point(result.points[0]))).high <= tolerance_squared &&
        endpoint_classification(work, world[1], result.points[0], endpoints, tolerance, tolerance_squared)) {
        // Root one is already within tolerance/8 of this rounded output. Root
        // two must also fit completely within tolerance of the retained output;
        // proximity to root one alone would omit its final rounding allowance.
        // Both roots must also share every downstream endpoint classification.
        count = 1;
    }
    result.point_count = count;
    return result;
}

struct CircleMetric { Point center; Interval radius; };

CircleMetric metric(Arithmetic& work, const Circle& arc) {
    if (arc.s.low <= 0 && arc.s.high >= 0) throw Unresolved{};
    const auto center = mul(work, arc.b,
                            div(work, Interval{Rational{-1}}, mul(work, Interval{Rational{2}}, arc.s)));
    const auto radius_squared = sub(work, norm_squared(work, center), div(work, arc.c, arc.s));
    if (radius_squared.low < 0) throw Unresolved{};
    return {center, sqrt_bounds(work, radius_squared)};
}

bool full_circle_gap_exceeds(Arithmetic& work, const CircleMetric& left, const CircleMetric& right,
                             const Interval& center_distance, const Rational& tolerance) {
    return work.sub(work.sub(center_distance.low, left.radius.high), right.radius.high) > tolerance ||
        work.sub(work.sub(left.radius.low, right.radius.high), center_distance.high) > tolerance ||
        work.sub(work.sub(right.radius.low, left.radius.high), center_distance.high) > tolerance;
}

bool clearance_attempt(const Segment& left_source, const Segment& right_source,
                       double tolerance, unsigned terms) {
    Arithmetic work;
    const auto tolerance_exact = binary64(tolerance);
    const auto tolerance_squared = work.mul(tolerance_exact, tolerance_exact);
    const auto origin = exact_point(left_source.start);
    const auto left = circle(work, left_source, origin, terms);
    const auto right = circle(work, right_source, origin, terms);
    const auto left_metric = metric(work, left);
    const auto right_metric = metric(work, right);
    const auto center_offset = sub(work, right_metric.center, left_metric.center);
    const auto center_distance = sqrt_bounds(work, norm_squared(work, center_offset));

    // A lower gap bound for complete supporting circles is a sufficient proof
    // for any selected subarcs, without solving or classifying their contacts.
    if (full_circle_gap_exceeds(work, left_metric, right_metric, center_distance, tolerance_exact))
        return true;

    // Zero-distance intersections are also stationary minima and need not lie
    // on the center axis. The remaining finite candidate list is complete only
    // after actual selected-arc contacts have been excluded by a certificate.
    // This uses its own bounded Arithmetic context; a fast floating-point miss
    // is never a substitute for this prerequisite.
    if (attempt(left_source, right_source, tolerance, terms).kind != CertifiedArcContactKind::none)
        return false;
    if (center_distance.low <= 0) return false;

    const std::array left_endpoints{left.start, add(work, left.start, left.chord)};
    const std::array right_endpoints{right.start, add(work, right.start, right.chord)};
    const auto separated = [&](const Point& a, const Point& b) {
        return norm_squared(work, sub(work, a, b)).low > tolerance_squared;
    };
    for (const auto& a : left_endpoints)
        for (const auto& b : right_endpoints)
            if (!separated(a, b)) return false;

    // With one endpoint fixed, circle-distance stationary points lie on the
    // radial line through that endpoint and the other circle's center. Retain
    // uncertain arc-membership candidates: uncertainty cannot prove clearance.
    const auto endpoint_clear = [&](const Point& endpoint, const Circle& target,
                                    const CircleMetric& target_metric) {
        const auto radial = sub(work, endpoint, target_metric.center);
        const auto radial_distance = sqrt_bounds(work, norm_squared(work, radial));
        if (work.sub(radial_distance.low, target_metric.radius.high) > tolerance_exact ||
            work.sub(target_metric.radius.low, radial_distance.high) > tolerance_exact) return true;
        // At/near the center, direction is unresolved. The complete-circle gap
        // above is the only safe way to discard all radial directions at once.
        if (radial_distance.low <= 0) return false;
        const auto displacement = mul(work, radial, div(work, target_metric.radius, radial_distance));
        for (const int sign : std::array{-1, 1}) {
            const auto candidate = add(work, target_metric.center,
                                       mul(work, displacement, Interval{Rational{sign}}));
            if (membership(work, target, candidate, tolerance_squared) == Membership::outside) continue;
            if (!separated(endpoint, candidate)) return false;
        }
        return true;
    };
    for (const auto& endpoint : left_endpoints)
        if (!endpoint_clear(endpoint, right, right_metric)) return false;
    for (const auto& endpoint : right_endpoints)
        if (!endpoint_clear(endpoint, left, left_metric)) return false;

    // At an interior, nonzero minimum, the connecting segment is normal to
    // both circle tangents. Distinct centers therefore put both points on the
    // center axis. All four radius-sign combinations are candidates.
    const auto unit = mul(work, center_offset, div(work, Interval{Rational{1}}, center_distance));
    for (const int left_sign : std::array{-1, 1}) {
        const auto a = add(work, left_metric.center,
                           mul(work, unit, mul(work, left_metric.radius, Interval{Rational{left_sign}})));
        if (membership(work, left, a, tolerance_squared) == Membership::outside) continue;
        for (const int right_sign : std::array{-1, 1}) {
            const auto b = add(work, right_metric.center,
                               mul(work, unit, mul(work, right_metric.radius, Interval{Rational{right_sign}})));
            if (membership(work, right, b, tolerance_squared) == Membership::outside) continue;
            if (!separated(a, b)) return false;
        }
    }
    return true;
}

bool valid_input(const Segment& left, const Segment& right, double tolerance) {
    const auto finite_point = [](Vec2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    return std::isfinite(tolerance) && tolerance > 0 && left.sweep_radians != 0 &&
        right.sweep_radians != 0 && std::isfinite(left.sweep_radians) &&
        std::isfinite(right.sweep_radians) &&
        finite_point(left.start) && finite_point(left.end) &&
        finite_point(right.start) && finite_point(right.end) &&
        (left.start.x != left.end.x || left.start.y != left.end.y) &&
        (right.start.x != right.end.x || right.start.y != right.end.y) &&
        std::abs(left.sweep_radians) < 2 * std::numbers::pi &&
        std::abs(right.sweep_radians) < 2 * std::numbers::pi;
}

} // namespace

CertifiedArcContact certified_arc_contact(const Segment& left, const Segment& right, double tolerance) {
    if (!valid_input(left, right, tolerance)) return {};
    // Finite deterministic refinements. If a general stored-angle tangency has
    // a discriminant enclosure containing zero, increasing precision is not a
    // license to label it exactly tangent.
    try {
        for (const unsigned terms : std::array{12u, 24u, 48u}) {
            try {
                const auto result = attempt(left, right, tolerance, terms);
                if (result.kind != CertifiedArcContactKind::indeterminate) return result;
            } catch (const Unresolved&) {
                // Spatial/rounding uncertainty may improve with tighter trig
                // intervals; exhaustion of the arithmetic cap cannot.
            }
        }
    } catch (const Exhausted&) {
        return {};
    } catch (const std::bad_alloc&) {
        return {};
    }
    return {};
}

bool certified_arc_clearance_unresolved_or_within(
    const Segment& left, const Segment& right, double tolerance) {
    if (!valid_input(left, right, tolerance)) return true;
    try {
        for (const unsigned terms : std::array{12u, 24u, 48u}) {
            try {
                if (clearance_attempt(left, right, tolerance, terms)) return false;
            } catch (const Unresolved&) {
                // Only a strict lower-distance proof admits these boundaries.
            }
        }
    } catch (const Exhausted&) {
        return true;
    } catch (const std::bad_alloc&) {
        return true;
    }
    return true;
}

} // namespace sketch::detail
