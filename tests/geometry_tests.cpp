#include "sketch/geometry.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace {

using sketch::Boundary;
using sketch::BoundaryIssue;
using sketch::Segment;
using sketch::Vec2;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "geometry_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "geometry_tests: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        std::exit(1);
    }
}

bool has_issue(const std::vector<sketch::BoundaryDiagnostic>& diagnostics, BoundaryIssue issue) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.issue == issue) {
            return true;
        }
    }
    return false;
}

template <typename Function>
void require_invalid_argument(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    fail(message);
}

Boundary rectangle(double left, double bottom, double right, double top) {
    return {
        {{left, bottom}, {right, bottom}, 0.0},
        {{right, bottom}, {right, top}, 0.0},
        {{right, top}, {left, top}, 0.0},
        {{left, top}, {left, bottom}, 0.0},
    };
}

void test_linear_boundaries() {
    const auto box = rectangle(0.0, 0.0, 4.0, 3.0);
    require_near(sketch::signed_area(box), 12.0, 1e-12, "rectangle area");
    require_near(sketch::perimeter(box), 14.0, 1e-12, "rectangle perimeter");
    require(sketch::validate_boundary(box).empty(), "rectangle should validate");

    const Boundary triangle{
        {{0.0, 0.0}, {4.0, 0.0}, 0.0},
        {{4.0, 0.0}, {0.0, 3.0}, 0.0},
        {{0.0, 3.0}, {0.0, 0.0}, 0.0},
    };
    require_near(sketch::signed_area(triangle), 6.0, 1e-12, "triangle area");
    require_near(sketch::perimeter(triangle), 12.0, 1e-12, "triangle perimeter");
    require(sketch::validate_boundary(triangle).empty(), "triangle should validate");
}

void test_arcs_have_analytic_length_and_area() {
    const auto lower_ccw = sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi);
    const Boundary semicircle{
        lower_ccw,
        {{1.0, 0.0}, {-1.0, 0.0}, 0.0},
    };
    require_near(sketch::segment_length(lower_ccw), std::numbers::pi, 1e-12, "semicircle length");
    require_near(sketch::signed_area(semicircle), std::numbers::pi / 2.0, 1e-12,
                 "counter-clockwise semicircle area");
    require(sketch::validate_boundary(semicircle).empty(), "semicircle should validate");

    const Boundary clockwise{
        sketch::arc_from_chord_angle({1.0, 0.0}, {-1.0, 0.0}, -std::numbers::pi),
        {{-1.0, 0.0}, {1.0, 0.0}, 0.0},
    };
    require_near(sketch::signed_area(clockwise), -std::numbers::pi / 2.0, 1e-12,
                 "clockwise semicircle area");

    const auto reflex = sketch::arc_from_chord_angle(
        {0.0, 0.0}, {2.0, 0.0}, 3.0 * std::numbers::pi / 2.0);
    const Boundary reflex_shape{reflex, {{2.0, 0.0}, {0.0, 0.0}, 0.0}};
    require_near(sketch::signed_area(reflex_shape), 3.0 * std::numbers::pi / 2.0 + 1.0,
                 1e-12, "reflex arc area");
    require_near(sketch::segment_length(reflex), 3.0 * std::numbers::pi / std::sqrt(2.0),
                 1e-12, "reflex arc length");

    const auto height_arc = sketch::arc_from_chord_height({-1.0, 0.0}, {1.0, 0.0}, 1.0);
    require_near(height_arc.sweep_radians, std::numbers::pi, 1e-12,
                 "positive chord height sweep");
    const auto negative_height_arc =
        sketch::arc_from_chord_height({-1.0, 0.0}, {1.0, 0.0}, -1.0);
    require_near(negative_height_arc.sweep_radians, -std::numbers::pi, 1e-12,
                 "negative chord height sweep");
}

void test_area_and_length_are_transform_invariant() {
    const auto original = rectangle(-2.0, -1.0, 3.0, 2.0);
    Boundary transformed;
    transformed.reserve(original.size());
    for (const auto& segment : original) {
        const auto transform = [](Vec2 point) {
            return Vec2{-point.y + 1234.5, point.x - 987.25};
        };
        transformed.push_back({transform(segment.start), transform(segment.end), segment.sweep_radians});
    }
    require_near(sketch::signed_area(transformed), 15.0, 1e-9, "transformed area");
    require_near(sketch::perimeter(transformed), 16.0, 1e-12, "transformed perimeter");
    require(sketch::validate_boundary(transformed).empty(), "transformed rectangle should validate");

    const Boundary original_arc{
        sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi),
        {{1.0, 0.0}, {-1.0, 0.0}, 0.0},
    };
    Boundary transformed_arc;
    transformed_arc.reserve(original_arc.size());
    for (const auto& segment : original_arc) {
        const auto transform = [](Vec2 point) {
            return Vec2{-point.y + 1234.5, point.x - 987.25};
        };
        transformed_arc.push_back(
            {transform(segment.start), transform(segment.end), segment.sweep_radians});
    }
    require_near(sketch::signed_area(transformed_arc), std::numbers::pi / 2.0, 1e-9,
                 "transformed arc area");
    require_near(sketch::perimeter(transformed_arc), std::numbers::pi + 2.0, 1e-12,
                 "transformed arc perimeter");
    require(sketch::validate_boundary(transformed_arc).empty(),
            "transformed semicircle should validate");

    const auto distant = rectangle(1.0e9, 1.0e9, 1.0e9 + 4.0, 1.0e9 + 3.0);
    require_near(sketch::signed_area(distant), 12.0, 1e-12,
                 "large translation should not cancel closed area");
    require_near(sketch::perimeter(distant), 14.0, 1e-12,
                 "large translation should not affect perimeter");

    auto tolerance_closed = rectangle(1.0e8, 1.0e8, 1.0e8 + 4.0, 1.0e8 + 3.0);
    tolerance_closed.back().end.y += 5.0e-8;
    require(sketch::validate_boundary(tolerance_closed).empty(),
            "sub-tolerance closure gap should validate");
    require_near(sketch::signed_area(tolerance_closed), 12.0, 1e-12,
                 "sub-tolerance closed area should use the stable local origin");

    const Boundary open_integral{{{2.0, 3.0}, {5.0, 7.0}, 0.0}};
    require_near(sketch::signed_area(open_integral), -0.5, 1e-12,
                 "open path should retain supplied-segment line integral semantics");
}

void test_invalid_construction_is_rejected() {
    const auto infinity = std::numeric_limits<double>::infinity();
    require_invalid_argument(
        [] { (void)sketch::arc_from_chord_angle({0.0, 0.0}, {0.0, 0.0}, 1.0); },
        "coincident arc endpoints should be rejected");
    require_invalid_argument(
        [] { (void)sketch::arc_from_chord_angle({0.0, 0.0}, {1.0, 0.0}, 0.0); },
        "zero arc sweep should be rejected");
    require_invalid_argument(
        [] {
            (void)sketch::arc_from_chord_angle(
                {0.0, 0.0}, {1.0, 0.0}, 2.0 * std::numbers::pi);
        },
        "full-circle sweep should be rejected");
    require_invalid_argument(
        [infinity] { (void)sketch::arc_from_chord_angle({0.0, 0.0}, {1.0, 0.0}, infinity); },
        "non-finite arc sweep should be rejected");
    require_invalid_argument(
        [] { (void)sketch::arc_from_chord_height({0.0, 0.0}, {1.0, 0.0}, 0.0); },
        "zero chord height should be rejected");
}

void test_validation_reports_invalid_topology_and_values() {
    const Boundary open{
        {{0.0, 0.0}, {1.0, 0.0}, 0.0},
        {{1.0, 0.0}, {1.0, 1.0}, 0.0},
        {{1.0, 1.0}, {0.0, 1.0}, 0.0},
    };
    require(has_issue(sketch::validate_boundary(open), BoundaryIssue::open_boundary),
            "open boundary should be diagnosed");

    auto disconnected = rectangle(0.0, 0.0, 1.0, 1.0);
    disconnected[1].start = {2.0, 0.0};
    require(has_issue(sketch::validate_boundary(disconnected), BoundaryIssue::disconnected),
            "internal gap should be diagnosed");

    auto non_finite = rectangle(0.0, 0.0, 1.0, 1.0);
    non_finite[2].end.x = std::numeric_limits<double>::quiet_NaN();
    require(has_issue(sketch::validate_boundary(non_finite), BoundaryIssue::non_finite),
            "non-finite coordinate should be diagnosed");

    auto degenerate = rectangle(0.0, 0.0, 1.0, 1.0);
    degenerate[1].end = degenerate[1].start;
    require(has_issue(sketch::validate_boundary(degenerate), BoundaryIssue::degenerate_segment),
            "zero-length segment should be diagnosed");

    auto invalid_sweep = rectangle(0.0, 0.0, 1.0, 1.0);
    invalid_sweep[0].sweep_radians = 2.0 * std::numbers::pi;
    require(has_issue(sketch::validate_boundary(invalid_sweep), BoundaryIssue::invalid_sweep),
            "invalid aggregate sweep should be diagnosed without throwing");
}

void test_derived_numeric_overflow_is_rejected_or_diagnosed() {
    const Segment overflowing_chord{{-1.0e308, 0.0}, {1.0e308, 0.0}, 0.0};
    require_invalid_argument([&] { (void)sketch::segment_length(overflowing_chord); },
                             "overflowing chord length should be rejected");
    require(has_issue(sketch::validate_boundary({overflowing_chord}),
                      BoundaryIssue::numeric_overflow),
            "overflowing chord should receive a numeric diagnostic");

    const Boundary overflowing_perimeter{
        {{0.0, 0.0}, {1.0e308, 0.0}, 0.0},
        {{1.0e308, 0.0}, {0.0, 0.0}, 0.0},
    };
    require_invalid_argument([&] { (void)sketch::perimeter(overflowing_perimeter); },
                             "overflowing perimeter sum should be rejected");
    require(has_issue(sketch::validate_boundary(overflowing_perimeter),
                      BoundaryIssue::numeric_overflow),
            "overflowing perimeter should receive a numeric diagnostic");

    const auto overflowing_area = rectangle(0.0, 0.0, 1.0e200, 1.0e200);
    require_invalid_argument([&] { (void)sketch::signed_area(overflowing_area); },
                             "overflowing area should be rejected");
    require(has_issue(sketch::validate_boundary(overflowing_area),
                      BoundaryIssue::numeric_overflow),
            "overflowing area should receive a numeric diagnostic");

    const auto small_sweep =
        sketch::arc_from_chord_angle({0.0, 0.0}, {1.0e100, 0.0}, 1.0e-100);
    const Boundary stable_small_sweep{
        small_sweep,
        {{1.0e100, 0.0}, {0.0, 0.0}, 0.0},
    };
    require_near(sketch::segment_length(small_sweep), 1.0e100, 1.0e86,
                 "small-sweep length should remain finite");
    require_near(sketch::signed_area(stable_small_sweep), 1.0e200 * 1.0e-100 / 12.0,
                 1.0e85, "small-sweep area should avoid radius-squared overflow");
}

void test_validation_reports_crossings_tangencies_and_overlaps() {
    const auto tolerance = sketch::default_geometry_tolerance_metres;
    const Boundary near_parallel_crossing{
        {{0, 0}, {10, 0}, 0},
        {{0, 1.5 * tolerance}, {10, -0.25 * tolerance}, 0},
    };
    require(has_issue(sketch::validate_boundary(near_parallel_crossing),
                      BoundaryIssue::indeterminate_intersection),
            "near-parallel crossing must retain the conservative indeterminate diagnostic");

    const Boundary near_parallel_uncertain{
        {{0, 0}, {10, 0}, 0},
        {{0, 1.5 * tolerance}, {10, 0.25 * tolerance}, 0},
    };
    require(has_issue(sketch::validate_boundary(near_parallel_uncertain),
                      BoundaryIssue::indeterminate_intersection),
            "same-side endpoints within tolerance must not prove separation");

    const Boundary bow_tie{
        {{0.0, 0.0}, {2.0, 2.0}, 0.0},
        {{2.0, 2.0}, {0.0, 2.0}, 0.0},
        {{0.0, 2.0}, {2.0, 0.0}, 0.0},
        {{2.0, 0.0}, {0.0, 0.0}, 0.0},
    };
    require(has_issue(sketch::validate_boundary(bow_tie), BoundaryIssue::self_intersection),
            "crossing lines should be diagnosed");

    const Boundary overlapping_lines{
        {{0.0, 0.0}, {3.0, 0.0}, 0.0},
        {{1.0, 0.0}, {2.0, 0.0}, 0.0},
    };
    require(has_issue(sketch::validate_boundary(overlapping_lines),
                      BoundaryIssue::overlapping_segments),
            "collinear overlap should be diagnosed");

    const Boundary tangent_line{
        sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi),
        {{-2.0, -1.0}, {2.0, -1.0}, 0.0},
    };
    require(has_issue(sketch::validate_boundary(tangent_line), BoundaryIssue::self_intersection),
            "non-adjacent line-arc tangency should be diagnosed");

    const Boundary crossing_line{
        sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi),
        {{0.0, -2.0}, {0.0, 1.0}, 0.0},
    };
    require(has_issue(sketch::validate_boundary(crossing_line), BoundaryIssue::self_intersection),
            "line crossing an arc should be diagnosed");

    const Boundary crossing_arcs{
        sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi),
        sketch::arc_from_chord_angle({-1.0, -1.0}, {1.0, -1.0}, -std::numbers::pi),
    };
    require(has_issue(sketch::validate_boundary(crossing_arcs), BoundaryIssue::self_intersection),
            "crossing arcs should be diagnosed");

    const Boundary tangent_arcs{
        sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi),
        sketch::arc_from_chord_angle({-1.0, -2.0}, {1.0, -2.0}, -std::numbers::pi),
    };
    require(has_issue(sketch::validate_boundary(tangent_arcs), BoundaryIssue::self_intersection),
            "non-adjacent arc tangency should be diagnosed");

    const auto arc = sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi);
    const Boundary overlapping_arcs{arc, arc};
    require(has_issue(sketch::validate_boundary(overlapping_arcs),
                      BoundaryIssue::overlapping_segments),
            "coincident arc overlap should be diagnosed");
}

void require_contacts(const Segment& line, const Segment& arc,
                      std::initializer_list<Vec2> expected, double tolerance,
                      double coordinate_tolerance, std::string_view message) {
    const auto hit = sketch::segment_intersection(line, arc, tolerance);
    require(hit.points.size() == expected.size(), message);
    require(hit.kind == (expected.size() == 0 ? sketch::SegmentIntersectionKind::none :
                        sketch::SegmentIntersectionKind::proper), message);
    for (const auto point : expected) {
        bool found = false;
        for (const auto actual : hit.points) {
            if (std::hypot(actual.x - point.x, actual.y - point.y) <= coordinate_tolerance)
                found = true;
        }
        require(found, message);
    }
}

void test_line_arc_contacts_preserve_small_geometric_offsets() {
    // At y=-1e-9 this arc's chord-frame equation gives x=1 +/- sqrt(0.6).
    const Segment shallow{{0, 0}, {2, 0}, 1e-8};
    const Segment line{{-1, -1e-9}, {3, -1e-9}, 0};
    const std::initializer_list<Vec2> contacts{{0.2254033307585166, -1e-9},
                                            {1.7745966692414834, -1e-9}};
    require_contacts(line, shallow, contacts, 1e-14, 1e-12,
                     "shallow arc must retain two proper contacts rather than a midpoint tangent");
    require_contacts(line, shallow, contacts, sketch::default_geometry_tolerance_metres, 1e-12,
                     "default tolerance must retain both actual shallow contacts");
    require_contacts({{-1, 1e-9}, {3, 1e-9}, 0}, shallow, {},
                     sketch::default_geometry_tolerance_metres, 0,
                     "default tolerance must exclude circle roots on the opposite shallow sweep");
    const auto ambiguous_bound = sketch::segment_intersection(
        {{-1, -2.5e-9}, {3, -2.5e-9}, 0}, shallow, 1e-14);
    require(ambiguous_bound.kind == sketch::SegmentIntersectionKind::indeterminate &&
                ambiguous_bound.points.empty(),
            "rounded shallow bound must fail closed rather than manufacture a tangent");
    require_contacts({line.end, line.start, 0}, shallow, contacts, 1e-14, 1e-12,
                     "reversing the finite line must retain both shallow contacts");
    require_contacts(line, {shallow.end, shallow.start, -shallow.sweep_radians},
                     contacts, 1e-14, 1e-12, "reversing the arc must retain both contacts");
    const auto swapped = sketch::segment_intersection(shallow, line, 1e-14);
    require(swapped.kind == sketch::SegmentIntersectionKind::proper && swapped.points.size() == 2,
            "swapping line and arc must retain proper shared contacts");
    require_contacts({{-1, 1e-9}, {3, 1e-9}, 0}, {{0, 0}, {2, 0}, -1e-8},
                     {{0.2254033307585166, 1e-9}, {1.7745966692414834, 1e-9}},
                     1e-14, 1e-12, "negative shallow sweep must retain both contacts");
    const auto rotate = [](Vec2 point) {
        return Vec2{std::fma(0.6, point.x, -0.8 * point.y),
                    std::fma(0.8, point.x, 0.6 * point.y)};
    };
    require_contacts({rotate(line.start), rotate(line.end), 0},
                     {rotate(shallow.start), rotate(shallow.end), shallow.sweep_radians},
                     {rotate(*contacts.begin()), rotate(*(contacts.begin() + 1))},
                     1e-14, 1e-7, "diagonal line and chord must preserve shallow contacts");

    for (const double sign : {-1.0, 1.0}) {
        require_contacts({{0, -sign * 0.5}, {4, -sign * 0.5}, 0},
                         {{3, 0}, {2, -sign}, sign * 1.5 * std::numbers::pi},
                         {{1.1339745962155614, -sign * 0.5}}, 1e-14, 1e-12,
                         "signed major sweep must exclude only its missing quadrant");
    }

    const Segment semicircle{{3, 0}, {1, 0}, std::numbers::pi};
    const double near_height = 0.9999999;
    require_contacts({{-1e12, near_height}, {1e12, near_height}, 0}, semicircle,
                     {{1.999552786415789, near_height}, {2.000447213584211, near_height}},
                     1e-14, 2e-11, "long finite line must preserve nearby near-tangent roots");
    require_contacts({{1e8 - 1e12, near_height}, {1e8 + 1e12, near_height}, 0},
                     {{1e8 + 3, 0}, {1e8 + 1, 0}, std::numbers::pi},
                     {{1e8 + 1.999552786415789, near_height},
                      {1e8 + 2.000447213584211, near_height}},
                     1e-14, 2e-8, "translated long line must preserve both nearby roots");

    const Segment diagonal{{2 - 3e6, -4e6 + 1.6666665},
                           {2 + 3e6, 4e6 + 1.6666665}, 0};
    // Independent projection oracle uses the actual represented endpoints,
    // rather than assuming the entered offset survived the large coordinates.
    const auto dx = static_cast<long double>(diagonal.end.x) - diagonal.start.x;
    const auto dy = static_cast<long double>(diagonal.end.y) - diagonal.start.y;
    const auto norm = std::hypot(dx, dy);
    const auto ux = dx / norm;
    const auto uy = dy / norm;
    const auto middle_x = std::midpoint(static_cast<long double>(diagonal.start.x),
                                      static_cast<long double>(diagonal.end.x));
    const auto middle_y = std::midpoint(static_cast<long double>(diagonal.start.y),
                                      static_cast<long double>(diagonal.end.y));
    const auto signed_offset = -uy * (middle_x - 2) + ux * middle_y;
    const auto half_contact_spacing = std::sqrt((1 - signed_offset) * (1 + signed_offset));
    const Vec2 first_contact{static_cast<double>(2 - uy * signed_offset - ux * half_contact_spacing),
                             static_cast<double>(ux * signed_offset - uy * half_contact_spacing)};
    const Vec2 second_contact{static_cast<double>(2 - uy * signed_offset + ux * half_contact_spacing),
                              static_cast<double>(ux * signed_offset + uy * half_contact_spacing)};
    require_contacts(diagonal, semicircle, {first_contact, second_contact}, 1e-14, 1e-9,
                     "rotated long finite line must retain both represented near-tangent roots");
    require_contacts({diagonal.end, diagonal.start, 0}, semicircle,
                     {first_contact, second_contact}, 1e-14, 1e-9,
                     "reversed rotated long line must retain both represented roots");
    const auto diagonal_swapped = sketch::segment_intersection(semicircle, diagonal, 1e-14);
    require(diagonal_swapped.kind == sketch::SegmentIntersectionKind::proper &&
                diagonal_swapped.points.size() == 2,
            "swapped rotated long line must retain both proper contacts");
}

void test_canonical_semicircle_tangent_and_adjacent_heights() {
    const Segment semicircle{{3, 0}, {1, 0}, std::numbers::pi};
    const auto bounds = sketch::segment_bounds(semicircle);
    require(bounds.minimum.x == 1 && bounds.maximum.x == 3 &&
                bounds.minimum.y == 0 && bounds.maximum.y == 1,
            "exact half-turn bounds must not contain trigonometric center residue");
    require_contacts({{0, 1}, {4, 1}, 0}, semicircle, {{2, 1}}, 1e-17, 1e-15,
                     "canonical semicircle tangent must have one proper contact");
    const auto outside = std::nextafter(1.0, std::numeric_limits<double>::infinity());
    require_contacts({{0, outside}, {4, outside}, 0}, semicircle, {}, 1e-17, 0,
                     "one representable step outside canonical semicircle must have no contact");
    const auto inside = std::nextafter(1.0, 0.0);
    require_contacts({{0, inside}, {4, inside}, 0}, semicircle,
                     {{1.9999999850988388, inside}, {2.0000000149011612, inside}},
                     1e-17, 2e-15, "one representable step inside must retain two true contacts");
    const Segment unit_semicircle{{1, 0}, {-1, 0}, std::numbers::pi};
    const Segment endpoint_secant{{1, 0}, {0, 2}, 0};
    require_contacts(endpoint_secant, unit_semicircle, {{1, 0}, {0.6, 0.8}}, 1e-14, 1e-12,
                     "shared endpoint secant must retain its second proper arc contact");
    require_contacts({endpoint_secant.end, endpoint_secant.start, 0},
                     {unit_semicircle.end, unit_semicircle.start, -std::numbers::pi},
                     {{1, 0}, {0.6, 0.8}}, 1e-14, 1e-12,
                     "reversed shared endpoint secant must retain both contacts");
    const auto endpoint_tangent = sketch::segment_intersection(
        {{1, -1}, {1, 1}, 0}, unit_semicircle, 1e-14);
    require(endpoint_tangent.kind == sketch::SegmentIntersectionKind::touch &&
                endpoint_tangent.points.size() == 1 &&
                endpoint_tangent.points.front().x == 1 && endpoint_tangent.points.front().y == 0,
            "canonical arc endpoint tangent must retain its sole exact touch");
    const Boundary tangent_endpoint_boundary{
        unit_semicircle, {{-1, 0}, {-1, -1}, 0},
        {{-1, -1}, {1, -1}, 0}, {{1, -1}, {1, 0}, 0},
    };
    require(sketch::validate_boundary(tangent_endpoint_boundary, 1e-14).empty(),
            "valid closed boundary must retain both expected adjacent arc endpoint tangencies");
    for (const double angle : {0.37, -0.81}) {
        for (const bool mirrored : {false, true}) {
            sketch::PlanarTransform transform;
            transform.rotation_radians = angle;
            transform.flip_horizontal = mirrored;
            transform.offset = {7.25, -3.5};
            Boundary transformed;
            for (const auto& edge : tangent_endpoint_boundary)
                transformed.push_back(sketch::transform_segment(edge, transform));
            const auto issues = sketch::validate_boundary(transformed);
            for (const auto& issue : issues)
                std::cerr << "transformed tangent angle=" << angle << " mirrored=" << mirrored
                          << " segment=" << issue.segment_index << " other="
                          << issue.other_segment_index.value_or(999) << " " << issue.message << '\n';
            require(issues.empty(),
                    "rotated translated and mirrored boundary must preserve exact shared tangent stations");
        }
    }
    const auto quarter_endpoint = sketch::segment_intersection(
        {{-2, 1.5}, {2, 1.5}, 0}, {{-2, 1.5}, {-2, -1.5}, std::numbers::pi / 2}, 1e-14);
    require(quarter_endpoint.kind == sketch::SegmentIntersectionKind::touch &&
                quarter_endpoint.points.size() == 1 &&
                quarter_endpoint.points.front().x == -2 && quarter_endpoint.points.front().y == 1.5,
            "noncanonical quarter arc endpoint must retain its exact adjacent contact");
    const auto outside_diagonal_offset = std::sqrt(2.0);
    // This represented sqrt(2) lies strictly above real sqrt(2), so the exact
    // supporting line x+y=offset has no unit-circle contact. Unit-vector
    // normalization must not change that sign and invent two intersections.
    const auto diagonal_line = [](double offset) {
        return Segment{{0.5, offset - 0.5}, {1.5, offset - 1.5}, 0};
    };
    const auto diagonal_miss = sketch::segment_intersection(
        diagonal_line(outside_diagonal_offset), unit_semicircle, 1e-17);
    require(diagonal_miss.points.empty() &&
                (diagonal_miss.kind == sketch::SegmentIntersectionKind::none ||
                 diagonal_miss.kind == sketch::SegmentIntersectionKind::indeterminate),
            "represented diagonal line outside unit circle must not fabricate contacts");
    const auto inside_diagonal_offset = std::nextafter(outside_diagonal_offset, 0.0);
    const auto diagonal_crossing = sketch::segment_intersection(
        diagonal_line(inside_diagonal_offset), unit_semicircle, 1e-17);
    require(diagonal_crossing.kind == sketch::SegmentIntersectionKind::proper &&
                diagonal_crossing.points.size() == 2 &&
                std::hypot(diagonal_crossing.points[0].x - diagonal_crossing.points[1].x,
                           diagonal_crossing.points[0].y - diagonal_crossing.points[1].y) > 1e-9,
            "represented diagonal line inside unit circle must preserve two distinct contacts");
    const Segment tilted_diameter{{1, 1}, {-1, -1}, std::numbers::pi};
    const auto rounded_bound_miss = sketch::segment_intersection(
        {{-1, outside_diagonal_offset}, {1, outside_diagonal_offset}, 0}, tilted_diameter, 1e-17);
    require(rounded_bound_miss.points.empty() &&
                (rounded_bound_miss.kind == sketch::SegmentIntersectionKind::none ||
                 rounded_bound_miss.kind == sketch::SegmentIntersectionKind::indeterminate),
            "rounded non-axis semicircle bound must not prove a false tangent");
    const auto tilted_inside = sketch::segment_intersection(
        {{-1, inside_diagonal_offset}, {1, inside_diagonal_offset}, 0}, tilted_diameter, 1e-17);
    require(tilted_inside.kind == sketch::SegmentIntersectionKind::proper && tilted_inside.points.size() == 2,
            "non-axis diameter must preserve both true adjacent inside contacts");
    constexpr double distant_x = 9007199254740992.0;
    const Boundary distant_crossing{
        {{distant_x, 0}, {distant_x + 2, 0}, -std::numbers::pi},
        {{distant_x + 2, 0}, {distant_x + 2, 2}, 0},
        {{distant_x + 2, 2}, {distant_x, 0}, 0},
    };
    require(!sketch::validate_boundary(distant_crossing).empty(),
            "rounded distant midpoint must not hide a shared-endpoint secant crossing");
    const Boundary tilted_distant_crossing{
        {{distant_x, 0}, {distant_x + 2, 2}, -std::numbers::pi},
        {{distant_x + 2, 2}, {distant_x + 2, 3}, 0},
        {{distant_x + 2, 3}, {distant_x, 0}, 0},
    };
    require(!sketch::validate_boundary(tilted_distant_crossing).empty(),
            "rounded world contact must not hide a tilted shared-endpoint secant crossing");
    const auto huge = sketch::segment_bounds(
        {{1e308, 0}, {1e308, 2e292}, std::numbers::pi});
    require(std::isfinite(huge.minimum.x) && std::isfinite(huge.maximum.x) &&
                huge.maximum.x > 1e308 && huge.minimum.y == 0 && huge.maximum.y == 2e292,
            "finite translated semicircle must not overflow its midpoint");
}

void test_holes_reject_sub_tolerance_arc_clearance() {
    const Boundary upper_half_disk{
        {{3, 0}, {1, 0}, std::numbers::pi},
        {{1, 0}, {3, 0}, 0},
    };
    const auto near_hole = rectangle(1.9999, 0.9, 2.0001, 1.0 - 5e-8);
    require(sketch::validate_boundary_holes(upper_half_disk, {near_hole}, 1e-7).has_value(),
            "hole with sub-tolerance curved boundary clearance must fail closed");
    const auto clear_hole = rectangle(1.9999, 0.9, 2.0001, 1.0 - 5e-7);
    require(!sketch::validate_boundary_holes(upper_half_disk, {clear_hole}, 1e-7).has_value(),
            "hole farther than tolerance from curved boundary must remain valid");
    const auto outer_rectangle = rectangle(-2, -3, 2, 0);
    const auto upper_arc_hole = [](double delta) {
        return Boundary{{{-1, -1 - delta}, {1, -1 - delta}, -std::numbers::pi},
                        {{1, -1 - delta}, {-1, -1 - delta}, 0}};
    };
    require(sketch::validate_boundary_holes(outer_rectangle, {upper_arc_hole(5e-8)}, 1e-7).has_value(),
            "hole arc tangent extremum within tolerance of a nonintersecting outer line must reject");
    require(!sketch::validate_boundary_holes(outer_rectangle, {upper_arc_hole(2e-7)}, 1e-7).has_value(),
            "hole arc tangent extremum farther than tolerance from an outer line must remain valid");
    const auto rotate = [](Vec2 point) {
        return Vec2{std::fma(0.6, point.x, -0.8 * point.y),
                    std::fma(0.8, point.x, 0.6 * point.y)};
    };
    const auto rotated = [&](Boundary boundary) {
        for (auto& segment : boundary) {
            segment.start = rotate(segment.start);
            segment.end = rotate(segment.end);
        }
        return boundary;
    };
    const auto rotated_outer = rotated(outer_rectangle);
    require(sketch::validate_boundary_holes(rotated_outer,
                {rotated(upper_arc_hole(5e-8))}, 1e-7).has_value(),
            "rotated interior arc/line clearance within tolerance must reject");
    require(!sketch::validate_boundary_holes(rotated_outer,
                {rotated(upper_arc_hole(2e-7))}, 1e-7).has_value(),
            "rotated interior arc/line clearance above tolerance must remain valid");
    auto complement = upper_arc_hole(5e-8);
    complement.front().sweep_radians = std::numbers::pi;
    require(!sketch::validate_boundary_holes(rotated_outer, {rotated(complement)}, 1e-7).has_value(),
            "clearance must exclude a nearby full-circle extremum on the opposite arc");
}

}  // namespace

void test_analytic_bounds() {
    using namespace sketch;
    const auto line=segment_bounds({{4,-3},{-2,5},0});
    require(line.minimum.x==-2 && line.minimum.y==-3 && line.maximum.x==4 && line.maximum.y==5,
        "line bounds must include both endpoints");
    const auto lower=boundary_bounds({{{-2,0},{2,0},std::numbers::pi},{{2,0},{-2,0},0}});
    require_near(lower.minimum.y,-2,1e-12,"semicircle lower extremum");
    require_near(lower.maximum.y,0,1e-12,"semicircle chord upper bound");
    const auto shallow=segment_bounds({{-500000,0},{500000,0},4e-12});
    require_near(shallow.minimum.y,-5e-7,1e-18,"shallow arc height must survive radius cancellation");
    const auto shallow_vertical=segment_bounds({{100,-500000},{100,500000},-4e-12});
    require_near(shallow_vertical.minimum.x,100-5e-7,1e-12,"vertical shallow arc must retain its small extremum");
    for (const double direction : {-1.0,1.0}) {
        const auto major=segment_bounds({{1,0},{0,-direction},direction*1.5*std::numbers::pi});
        require_near(major.minimum.x,-1,1e-12,"major arc left extremum");
        require_near(major.minimum.y,-1,1e-12,"major arc lower extremum");
        require_near(major.maximum.x,1,1e-12,"major arc right extremum");
        require_near(major.maximum.y,1,1e-12,"major arc upper extremum");
    }
    constexpr double start=0.123, sweep=4.321, radius=3;
    const Vec2 center{5,-7};
    const Segment arc{{center.x+radius*std::cos(start),center.y+radius*std::sin(start)},
        {center.x+radius*std::cos(start+sweep),center.y+radius*std::sin(start+sweep)},sweep};
    const auto partial=segment_bounds(arc);
    require_near(partial.minimum.x,2,1e-12,"partial major arc left bound");
    require_near(partial.maximum.y,-4,1e-12,"partial major arc upper bound");
    require_near(partial.maximum.x,arc.start.x,1e-12,"excluded cardinal angle must not enlarge bounds");
    require_near(partial.minimum.y,arc.end.y,1e-12,"excluded lower extremum must not enlarge bounds");
    const auto reversed=segment_bounds({arc.end,arc.start,-sweep});
    require_near(reversed.minimum.x,partial.minimum.x,1e-12,"reversing an arc retains lower X bound");
    require_near(reversed.maximum.y,partial.maximum.y,1e-12,"reversing an arc retains upper Y bound");
    for(int index=0;index<=1000;++index) {
        const auto angle=start+sweep*index/1000.0;
        const Vec2 point{center.x+radius*std::cos(angle),center.y+radius*std::sin(angle)};
        require(point.x>=partial.minimum.x-1e-12 && point.x<=partial.maximum.x+1e-12 &&
            point.y>=partial.minimum.y-1e-12 && point.y<=partial.maximum.y+1e-12,
            "analytic bounds must contain the entire represented arc");
    }
    bool rejected=false;
    try { (void)boundary_bounds({}); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,"empty geometry must not invent a bounding box");
    rejected=false;
    try { (void)segment_bounds({{0,0},{1,1},std::numeric_limits<double>::infinity()}); }
    catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,"invalid arc values must reject bounds");
}

void test_boundary_crop_preserves_analytic_segments() {
    using namespace sketch;
    const Bounds2 crop{{-1.0, -2.0}, {1.0, 1.0}};

    const Boundary contained{{{-0.5, -0.5}, {0.5, 0.5}, 0.0}};
    const auto unchanged = clip_boundary_to_bounds(contained, crop);
    require(unchanged.size() == 1 &&
                unchanged.front().start.x == contained.front().start.x &&
                unchanged.front().start.y == contained.front().start.y &&
                unchanged.front().end.x == contained.front().end.x &&
                unchanged.front().end.y == contained.front().end.y,
            "a contained segment must preserve its exact endpoints");

    const auto line = clip_boundary_to_bounds(
        Boundary{{{-3.0, 0.25}, {4.0, 0.25}, 0.0}}, crop);
    require(line.size() == 1 && line.front().sweep_radians == 0.0,
            "a crossing line must retain one exact line interval");
    require_near(line.front().start.x, -1.0, 1e-12, "cropped line left endpoint");
    require_near(line.front().end.x, 1.0, 1e-12, "cropped line right endpoint");
    require_near(line.front().start.y, 0.25, 1e-12, "cropped line height");

    const Segment lower_semicircle{{-2.0, 0.0}, {2.0, 0.0}, std::numbers::pi};
    const auto arc = clip_boundary_to_bounds(Boundary{lower_semicircle}, crop);
    require(arc.size() == 1 && arc.front().sweep_radians != 0.0,
            "a cropped circular arc must remain one analytic arc");
    require_near(arc.front().start.x, -1.0, 1e-12, "cropped arc left endpoint");
    require_near(arc.front().end.x, 1.0, 1e-12, "cropped arc right endpoint");
    require_near(arc.front().start.y, -std::sqrt(3.0), 1e-12,
                 "cropped arc start height");
    require_near(arc.front().end.y, -std::sqrt(3.0), 1e-12,
                 "cropped arc end height");
    require_near(arc.front().sweep_radians, std::numbers::pi / 3.0, 1e-12,
                 "cropped arc sweep");

    require(clip_boundary_to_bounds(
                Boundary{{{2.0, -3.0}, {2.0, 3.0}, 0.0}}, crop).empty(),
            "a line outside the crop must disappear");
    require_invalid_argument(
        [&] { (void)clip_boundary_to_bounds(contained, {{1.0, 0.0}, {1.0, 2.0}}); },
        "a zero-width crop must be rejected");
    require_invalid_argument(
        [&] {
            (void)clip_boundary_to_bounds(contained,
                {{0.0, 0.0}, {std::numeric_limits<double>::infinity(), 2.0}});
        },
        "a nonfinite crop must be rejected");
}

void test_planar_transforms() {
    using namespace sketch;
    const PlanarTransform transform{{2,1},std::numbers::pi/2,true,false,{5,-3}};
    const auto point = transform_point({1,-0.5},transform);
    require_near(point.x,5.5,1e-12,"compound transform X");
    require_near(point.y,-3,1e-12,"compound transform Y");
    const Segment arc{{-2,0},{2,0},std::numbers::pi};
    const auto moved = transform_segment(arc,transform);
    require_near(segment_length(moved),segment_length(arc),1e-12,"rigid transform preserves arc length");
    require(moved.sweep_radians == -arc.sweep_radians,"one reflection reverses arc direction");
    auto twice = transform;
    twice.flip_vertical = true;
    require(transform_segment(arc,twice).sweep_radians == arc.sweep_radians,"two reflections retain arc direction");
    const auto untouched = transform_point({-0.0,7},{});
    require(std::signbit(untouched.x) && untouched.y == 7,"identity transform preserves signed zero");
    require_invalid_argument([&] { auto bad=transform; bad.offset.x=std::numeric_limits<double>::infinity();
        (void)transform_point({0,0},bad); },"nonfinite transform must reject");
    require_invalid_argument([&] { (void)transform_point({std::numeric_limits<double>::max(),0},
        PlanarTransform{{},0,false,false,{std::numeric_limits<double>::max(),0}}); },"overflowing output must reject");
    const PlanarTransform rotation{{0,0},0.321,false,false,{}};
    auto reverse = rotation; reverse.rotation_radians = -rotation.rotation_radians;
    const auto restored=transform_segment(transform_segment(arc,rotation),reverse);
    require_near(restored.start.x,arc.start.x,1e-12,"inverse rotation restores start");
    require_near(restored.end.y,arc.end.y,1e-12,"inverse rotation restores end");
}

int main() {
    sketch::testing::noninteractive_errors();
    test_line_arc_contacts_preserve_small_geometric_offsets();
    test_canonical_semicircle_tangent_and_adjacent_heights();
    test_holes_reject_sub_tolerance_arc_clearance();
    test_planar_transforms();
    test_analytic_bounds();
    test_boundary_crop_preserves_analytic_segments();
    test_linear_boundaries();
    test_arcs_have_analytic_length_and_area();
    test_area_and_length_are_transform_invariant();
    test_invalid_construction_is_rejected();
    test_validation_reports_invalid_topology_and_values();
    test_derived_numeric_overflow_is_rejected_or_diagnosed();
    test_validation_reports_crossings_tangencies_and_overlaps();
    return 0;
}
