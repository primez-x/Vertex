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
