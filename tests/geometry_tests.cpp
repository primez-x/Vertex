#include "sketch/geometry.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
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

}  // namespace

int main() {
    test_linear_boundaries();
    test_arcs_have_analytic_length_and_area();
    test_area_and_length_are_transform_invariant();
    test_invalid_construction_is_rejected();
    test_validation_reports_invalid_topology_and_values();
    test_derived_numeric_overflow_is_rejected_or_diagnosed();
    test_validation_reports_crossings_tangencies_and_overlaps();
    return 0;
}
