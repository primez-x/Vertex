#include "sketch/building_plan_projection.hpp"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using sketch::Boundary;
using sketch::BuildingObject;
using sketch::Segment;
using sketch::Vec2;
using sketch::Vec3;

constexpr double tolerance = 1e-5;
constexpr double full_turn = 2.0 * std::numbers::pi;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void near(double actual, double expected, double allowed, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > allowed) {
        throw std::runtime_error(std::string(message) + ": expected "
                                 + std::to_string(expected) + ", got "
                                 + std::to_string(actual));
    }
}

struct Bounds {
    double min_x{std::numeric_limits<double>::infinity()};
    double min_y{std::numeric_limits<double>::infinity()};
    double max_x{-std::numeric_limits<double>::infinity()};
    double max_y{-std::numeric_limits<double>::infinity()};

    void add(Vec2 point) {
        require(std::isfinite(point.x) && std::isfinite(point.y),
                "projected point must be finite");
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }
};

double positive_angle(double angle) {
    double result = std::fmod(angle, full_turn);
    if (result < 0.0) {
        result += full_turn;
    }
    return result;
}

bool on_arc(double start_angle, double sweep, double angle) {
    const double travelled = sweep > 0.0
        ? positive_angle(angle - start_angle)
        : positive_angle(start_angle - angle);
    return travelled <= std::abs(sweep) + 1e-9;
}

void add_segment_bounds(Bounds& bounds, const Segment& segment) {
    bounds.add(segment.start);
    bounds.add(segment.end);
    if (segment.sweep_radians == 0.0) {
        return;
    }

    const Vec2 chord{segment.end.x - segment.start.x,
                     segment.end.y - segment.start.y};
    const double chord_length = std::hypot(chord.x, chord.y);
    const double half_sweep = segment.sweep_radians * 0.5;
    const double tangent = std::tan(half_sweep);
    const double sine = std::sin(std::abs(half_sweep));
    require(chord_length > 0.0 && std::isfinite(tangent) && tangent != 0.0
                && sine > 0.0,
            "projected arc must have finite geometry");
    const Vec2 midpoint{(segment.start.x + segment.end.x) * 0.5,
                        (segment.start.y + segment.end.y) * 0.5};
    const Vec2 left_normal{-chord.y / chord_length, chord.x / chord_length};
    const Vec2 center{midpoint.x + left_normal.x * chord_length / (2.0 * tangent),
                      midpoint.y + left_normal.y * chord_length / (2.0 * tangent)};
    const double radius = chord_length / (2.0 * sine);
    const double start_angle = std::atan2(segment.start.y - center.y,
                                          segment.start.x - center.x);
    for (int quadrant = -8; quadrant <= 8; ++quadrant) {
        const double candidate = static_cast<double>(quadrant)
            * std::numbers::pi * 0.5;
        if (on_arc(start_angle, segment.sweep_radians, candidate)) {
            bounds.add({center.x + radius * std::cos(candidate),
                        center.y + radius * std::sin(candidate)});
        }
    }
}

Bounds projected_bounds(const Boundary& boundary) {
    require(!boundary.empty(), "projection must contain visible edges");
    Bounds result;
    for (const auto& segment : boundary) {
        require(std::isfinite(segment.sweep_radians),
                "projection sweep must be finite");
        require(segment.sweep_radians == 0.0
                    || std::abs(segment.sweep_radians) < full_turn,
                "projection sweep must be below a full turn");
        add_segment_bounds(result, segment);
    }
    return result;
}

Bounds shape_bounds(const BuildingObject& object) {
    Bnd_Box box;
    BRepBndLib::Add(sketch::make_building_shape(object), box);
    require(!box.IsVoid(), "building shape bounds must be finite");
    double min_x = 0.0;
    double min_y = 0.0;
    double min_z = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    double max_z = 0.0;
    box.Get(min_x, min_y, min_z, max_x, max_y, max_z);
    require(std::isfinite(min_x) && std::isfinite(min_y)
                && std::isfinite(max_x) && std::isfinite(max_y),
            "building shape bounds must be finite");
    return {min_x, min_y, max_x, max_y};
}

void require_same_extents(const Bounds& projected, const Bounds& expected,
                          std::string_view label) {
    near(projected.min_x, expected.min_x, tolerance,
         std::string(label) + " minimum X");
    near(projected.min_y, expected.min_y, tolerance,
         std::string(label) + " minimum Y");
    near(projected.max_x, expected.max_x, tolerance,
         std::string(label) + " maximum X");
    near(projected.max_y, expected.max_y, tolerance,
         std::string(label) + " maximum Y");
}

bool same_point(Vec2 first, Vec2 second) {
    return std::hypot(first.x - second.x, first.y - second.y) < tolerance;
}

void require_edge_set(const Boundary& actual, const Boundary& expected,
                      std::string_view label) {
    require(actual.size() == expected.size(),
            std::string(label) + " must contain exactly the expected number of edges");
    std::vector<bool> matched(expected.size());
    for (const auto& edge : actual) {
        bool found = false;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (matched[index]) continue;
            const auto& candidate = expected[index];
            const bool forward = same_point(edge.start, candidate.start) &&
                same_point(edge.end, candidate.end) &&
                std::abs(edge.sweep_radians - candidate.sweep_radians) < tolerance;
            const bool reverse = same_point(edge.start, candidate.end) &&
                same_point(edge.end, candidate.start) &&
                std::abs(edge.sweep_radians + candidate.sweep_radians) < tolerance;
            if (forward || reverse) {
                matched[index] = true;
                found = true;
                break;
            }
        }
        require(found, std::string(label) + " contains an unexpected or duplicate edge");
    }
}

Boundary quadrilateral(const std::vector<Vec2>& corners) {
    Boundary result;
    for (std::size_t index = 0; index < corners.size(); ++index)
        result.push_back({corners[index], corners[(index + 1) % corners.size()], 0.0});
    return result;
}

Vec2 frame_point(const Vec3& base, double orientation, double along, double across) {
    const double cosine = std::cos(orientation);
    const double sine = std::sin(orientation);
    return {base.x + along * cosine - across * sine,
            base.y + along * sine + across * cosine};
}

Boundary stair_plan_edges(const sketch::StairFlight& flight, bool landing) {
    const double total_run = static_cast<double>(flight.riser_count) * flight.going;
    Boundary expected;
    expected.reserve(2 * flight.riser_count + 2 + (landing ? 4 : 0));
    for (std::size_t index = 0; index <= flight.riser_count; ++index) {
        const double run = static_cast<double>(index) * flight.going;
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         run, 0.0),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        run, flight.width),
                            0.0});
    }
    for (std::size_t index = 0; index < flight.riser_count; ++index) {
        const double start_run = static_cast<double>(index) * flight.going;
        const double end_run = static_cast<double>(index + 1) * flight.going;
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         start_run, 0.0),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        end_run, 0.0),
                            0.0});
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         start_run, flight.width),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        end_run, flight.width),
                            0.0});
    }
    if (landing) {
        const double landing_end = total_run + flight.top_landing->depth;
        // The landing is a separate solid sharing the finished flight edge.
        // Its two side edges, far end, and the shared interface are therefore
        // all part of the projected visible compound, with the interface
        // occurring once more than in the stair prism itself.
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         total_run, 0.0),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        landing_end, 0.0),
                            0.0});
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         total_run, flight.width),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        landing_end, flight.width),
                            0.0});
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         landing_end, 0.0),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        landing_end, flight.width),
                            0.0});
        expected.push_back({frame_point(flight.base_position, flight.orientation_radians,
                                         total_run, 0.0),
                            frame_point(flight.base_position, flight.orientation_radians,
                                        total_run, flight.width),
                            0.0});
    }
    return expected;
}

void test_rotated_stairs_have_exact_edge_sets() {
    using namespace sketch;
    const StairFlight flight{
        .id = "rotated-plan-stair",
        .base_position = {1.0, -2.0, 0.5},
        .orientation_radians = 0.4,
        .riser_count = 3,
        .total_rise = 1.8,
        .going = 1.0,
        .width = 2.0,
    };
    require_edge_set(project_building_plan(flight), stair_plan_edges(flight, false),
                     "rotated stair without landing");

    StairFlight with_landing = flight;
    with_landing.id = "rotated-plan-stair-landing";
    with_landing.top_landing = StairLanding{.depth = 0.8, .thickness = 0.2};
    require_edge_set(project_building_plan(with_landing), stair_plan_edges(with_landing, true),
                     "rotated stair with landing");
}

void test_roof_forms_have_exact_edge_sets() {
    using namespace sketch;
    const SlopedRoofPanel panel{
        .id = "analytic-plan-shed",
        .base_position = {3.0, -4.0, 5.0},
        .orientation_radians = 0.35,
        .run = 3.0,
        .span = 2.0,
        .rise = 0.75,
        .pitch_radians = std::atan(0.75 / 3.0),
        .overhang = 0.3,
        .thickness = 0.15,
    };
    const double slope = panel.rise / panel.run;
    const double normal_scale = std::sqrt(1.0 + slope * slope);
    const double normal_shift = panel.thickness * slope / normal_scale;
    const double x0 = -panel.overhang;
    const double x1 = panel.run + panel.overhang;
    const double y0 = -panel.overhang;
    const double y1 = panel.span + panel.overhang;
    const auto top = [&](double x, double y) {
        return frame_point(panel.base_position, panel.orientation_radians, x, y);
    };
    const auto bottom = [&](double x, double y) {
        return frame_point(panel.base_position, panel.orientation_radians,
                           x + normal_shift, y);
    };
    const Vec2 top_left_back = top(x0, y1);
    const Vec2 top_left_front = top(x0, y0);
    const Vec2 top_right_front = top(x1, y0);
    const Vec2 top_right_back = top(x1, y1);
    const Vec2 bottom_right_front = bottom(x1, y0);
    const Vec2 bottom_right_back = bottom(x1, y1);
    const Boundary panel_expected{
        {top_left_back, top_left_front, 0.0},
        {top_left_front, top_right_front, 0.0},
        {top_right_front, top_right_back, 0.0},
        {top_right_back, top_left_back, 0.0},
        {top_right_front, bottom_right_front, 0.0},
        {top_right_back, bottom_right_back, 0.0},
        {bottom_right_front, bottom_right_back, 0.0},
    };
    require_edge_set(project_building_plan(panel), panel_expected,
                     "sloped roof panel visible edges");

    const SlopedRoofPanel flat_panel{
        .id = "analytic-plan-flat-roof",
        .base_position = {-2.0, 1.0, 4.0},
        .orientation_radians = 0.35,
        .run = 3.0,
        .span = 2.0,
        .rise = 0.0,
        .pitch_radians = 0.0,
        .overhang = 0.3,
        .thickness = 0.15,
    };
    const double flat_x0 = -flat_panel.overhang;
    const double flat_x1 = flat_panel.run + flat_panel.overhang;
    const double flat_y0 = -flat_panel.overhang;
    const double flat_y1 = flat_panel.span + flat_panel.overhang;
    const Boundary flat_expected = quadrilateral({
        frame_point(flat_panel.base_position, flat_panel.orientation_radians,
                    flat_x0, flat_y0),
        frame_point(flat_panel.base_position, flat_panel.orientation_radians,
                    flat_x1, flat_y0),
        frame_point(flat_panel.base_position, flat_panel.orientation_radians,
                    flat_x1, flat_y1),
        frame_point(flat_panel.base_position, flat_panel.orientation_radians,
                    flat_x0, flat_y1),
    });
    require_edge_set(project_building_plan(flat_panel), flat_expected,
                     "flat roof panel visible edges");

    const GableRoof roof{
        .id = "analytic-plan-gable",
        .base_position = {5.0, -3.0, 4.0},
        .orientation_radians = 0.4,
        .length = 4.0,
        .span = 3.0,
        .rise = 0.9,
        .pitch_radians = std::atan(0.9 / 1.5),
        .overhang = 0.25,
        .thickness = 0.12,
    };
    const double ridge_start = -(roof.length + 2.0 * roof.overhang) * 0.5;
    const double ridge_end = -ridge_start;
    const double outer_half_span = roof.span * 0.5 + roof.overhang;
    const Vec2 left_start = frame_point(roof.base_position, roof.orientation_radians,
                                        ridge_start, -outer_half_span);
    const Vec2 left_end = frame_point(roof.base_position, roof.orientation_radians,
                                      ridge_end, -outer_half_span);
    const Vec2 right_start = frame_point(roof.base_position, roof.orientation_radians,
                                         ridge_start, outer_half_span);
    const Vec2 right_end = frame_point(roof.base_position, roof.orientation_radians,
                                       ridge_end, outer_half_span);
    const Vec2 ridge_start_point = frame_point(roof.base_position, roof.orientation_radians,
                                               ridge_start, 0.0);
    const Vec2 ridge_end_point = frame_point(roof.base_position, roof.orientation_radians,
                                             ridge_end, 0.0);
    // Each clipped top panel contributes its own perimeter.  The compound
    // intentionally retains the common ridge edge twice, while all bottom
    // and vertical edges are hidden in a top-down view.
    const Boundary roof_expected{
        {left_start, ridge_start_point, 0.0},
        {ridge_start_point, ridge_end_point, 0.0},
        {ridge_end_point, left_end, 0.0},
        {left_end, left_start, 0.0},
        {ridge_start_point, right_start, 0.0},
        {right_start, right_end, 0.0},
        {right_end, ridge_end_point, 0.0},
        {ridge_end_point, ridge_start_point, 0.0},
    };
    require_edge_set(project_building_plan(roof), roof_expected,
                     "gable roof visible edges including ridge multiplicity");
}

Vec3 add(Vec3 first, Vec3 second) {
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Vec3 subtract(Vec3 first, Vec3 second) {
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Vec3 scale(Vec3 value, double factor) {
    return {value.x * factor, value.y * factor, value.z * factor};
}

double dot(Vec3 first, Vec3 second) {
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

double magnitude(Vec3 value) {
    return std::sqrt(dot(value, value));
}

Vec3 unit(Vec3 value) {
    return scale(value, 1.0 / magnitude(value));
}

Vec3 cross(Vec3 first, Vec3 second) {
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

Vec2 plan(Vec3 point) {
    return {point.x, point.y};
}

void test_nonhorizontal_beam_has_exact_visible_edges() {
    using namespace sketch;
    const Beam beam{
        .id = "analytic-nonhorizontal-beam",
        .start = {-1.0, 1.0, 0.0},
        .end = {3.0, 3.0, 3.0},
        .up = {0.0, 0.0, 1.0},
        .width = 0.6,
        .depth = 0.8,
    };
    const Vec3 axis = subtract(beam.end, beam.start);
    const Vec3 axis_unit = unit(axis);
    const Vec3 projected_up = subtract(beam.up, scale(axis_unit, dot(beam.up, axis_unit)));
    const Vec3 local_up = unit(projected_up);
    const Vec3 local_right = cross(local_up, axis_unit);
    const auto corner = [&](Vec3 center, double right_sign, double up_sign) {
        return add(center, add(scale(local_right, right_sign * beam.width * 0.5),
                               scale(local_up, up_sign * beam.depth * 0.5)));
    };
    const Vec2 top_right_start = plan(corner(beam.start, 1.0, 1.0));
    const Vec2 top_left_start = plan(corner(beam.start, -1.0, 1.0));
    const Vec2 top_right_end = plan(corner(beam.end, 1.0, 1.0));
    const Vec2 top_left_end = plan(corner(beam.end, -1.0, 1.0));
    const Vec2 bottom_right_end = plan(corner(beam.end, 1.0, -1.0));
    const Vec2 bottom_left_end = plan(corner(beam.end, -1.0, -1.0));
    // The four edges around the upper face are visible.  The projected end
    // cap contributes its two lower-to-upper side edges and lower edge; the
    // corresponding start-cap edges are hidden from +Z.
    const Boundary expected{
        {top_right_start, top_right_end, 0.0},
        {top_left_start, top_right_start, 0.0},
        {top_left_start, top_left_end, 0.0},
        {top_left_end, top_right_end, 0.0},
        {bottom_right_end, top_right_end, 0.0},
        {bottom_left_end, top_left_end, 0.0},
        {bottom_left_end, bottom_right_end, 0.0},
    };
    require_edge_set(project_building_plan(beam), expected,
                     "nonhorizontal beam visible edges");
}

void test_simple_solids_have_exact_edge_sets() {
    using namespace sketch;
    constexpr double angle = 0.37;
    const RectangularColumn column{"edge-set-column", {10.0, -7.0, 2.0}, 2.0, 1.0, 3.0, angle};
    std::vector<Vec2> corners;
    for (const auto& local : std::vector<Vec2>{{-1.0, -0.5}, {1.0, -0.5}, {1.0, 0.5}, {-1.0, 0.5}})
        corners.push_back({10.0 + local.x * std::cos(angle) - local.y * std::sin(angle),
                           -7.0 + local.x * std::sin(angle) + local.y * std::cos(angle)});
    require_edge_set(project_building_plan(column), quadrilateral(corners), "rotated column");

    const Beam beam{"edge-set-beam", {-1.0, 2.0, 3.0}, {2.0, 6.0, 3.0}, {0.0, 0.0, 1.0}, 0.2, 0.3};
    // An independent plan construction: the unit plan axis is (3/5, 4/5),
    // so the half-width normal is (-0.08, 0.06). Depth affects Z only.
    require_edge_set(project_building_plan(beam),
                     quadrilateral({{-1.08, 2.06}, {1.92, 6.06}, {2.08, 5.94}, {-0.92, 1.94}}),
                     "rotated horizontal beam");
}

void test_all_eight_forms_have_exact_plan_extents() {
    using namespace sketch;
    const std::vector<BuildingObject> objects{
        RectangularColumn{
            .id = "plan-rect",
            .base_center = {1.0, -2.0, 0.75},
            .width = 0.4,
            .depth = 0.6,
            .height = 3.0,
            .rotation_radians = 0.25,
        },
        CircularColumn{
            .id = "plan-round",
            .base_center = {-3.0, 4.0, -0.5},
            .radius = 0.25,
            .height = 2.0,
        },
        Beam{
            .id = "plan-beam",
            .start = {-1.0, 2.0, 0.5},
            .end = {2.0, 6.0, 3.5},
            .up = {0.0, 0.0, 1.0},
            .width = 0.2,
            .depth = 0.3,
        },
        StairFlight{
            .id = "plan-stair",
            .base_position = {1.0, 2.0, 0.5},
            .orientation_radians = 0.1,
            .riser_count = 4,
            .total_rise = 2.0,
            .going = 0.25,
            .width = 1.5,
            .top_landing = StairLanding{.depth = 0.5, .thickness = 0.2},
        },
        Railing{
            .id = "plan-railing",
            .base_position = {1.0, -1.0, 0.5},
            .orientation_radians = 0.2,
            .length = 3.0,
            .height = 1.1,
            .thickness = 0.08,
            .post_spacing = 0.9,
        },
        SlopedRoofPanel{
            .id = "plan-shed",
            .base_position = {0.0, 0.0, 4.0},
            .orientation_radians = 0.15,
            .run = 4.0,
            .span = 3.0,
            .rise = 1.0,
            .pitch_radians = std::atan(1.0 / 4.0),
            .overhang = 0.2,
            .thickness = 0.1,
        },
        GableRoof{
            .id = "plan-gable",
            .base_position = {0.0, 0.0, 4.0},
            .orientation_radians = std::numbers::pi / 6.0,
            .length = 5.0,
            .span = 4.0,
            .rise = 1.0,
            .pitch_radians = std::atan(1.0 / 2.0),
            .overhang = 0.2,
            .thickness = 0.1,
        },
    };

    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto projected = project_building_plan(objects[index]);
        require_same_extents(projected_bounds(projected), shape_bounds(objects[index]),
                              "eight-form projection " + std::to_string(index));
    }
}

void test_translation_and_rotation_are_preserved() {
    using namespace sketch;
    constexpr double angle = std::numbers::pi / 4.0;
    const RectangularColumn column{
        .id = "translated-rotated-column",
        .base_center = {10.0, -7.0, 0.0},
        .width = 2.0,
        .depth = 1.0,
        .height = 3.0,
        .rotation_radians = angle,
    };
    const auto bounds = projected_bounds(project_building_plan(column));
    const double half_extent = (column.width + column.depth)
        / (2.0 * std::sqrt(2.0));
    near(bounds.min_x, column.base_center.x - half_extent, tolerance,
         "translated rotated column minimum X");
    near(bounds.max_x, column.base_center.x + half_extent, tolerance,
         "translated rotated column maximum X");
    near(bounds.min_y, column.base_center.y - half_extent, tolerance,
         "translated rotated column minimum Y");
    near(bounds.max_y, column.base_center.y + half_extent, tolerance,
         "translated rotated column maximum Y");
}

void test_circular_column_retains_analytic_arcs() {
    using namespace sketch;
    const CircularColumn column{
        .id = "analytic-circle",
        .base_center = {5.0, -2.0, 0.0},
        .radius = 1.25,
        .height = 4.0,
    };
    const auto projected = project_building_plan(column);
    std::size_t arc_count = 0;
    for (const auto& segment : projected) {
        if (segment.sweep_radians == 0.0) {
            continue;
        }
        ++arc_count;
        const Vec2 chord{segment.end.x - segment.start.x,
                         segment.end.y - segment.start.y};
        const double chord_length = std::hypot(chord.x, chord.y);
        const double radius = chord_length
            / (2.0 * std::sin(std::abs(segment.sweep_radians) * 0.5));
        near(radius, column.radius, tolerance, "analytic circular column radius");
    }
    require(arc_count == 4 && projected.size() == 4,
            "a circular column must project to exactly four analytic arcs");
    constexpr double quarter = std::numbers::pi / 2.0;
    const Boundary expected{
        {{6.25, -2.0}, {5.0, -0.75}, quarter},
        {{5.0, -0.75}, {3.75, -2.0}, quarter},
        {{3.75, -2.0}, {5.0, -3.25}, quarter},
        {{5.0, -3.25}, {6.25, -2.0}, quarter},
    };
    require_edge_set(projected, expected, "single complete circular outline");
    double sweep_sum = 0.0;
    for (const auto& segment : projected) sweep_sum += segment.sweep_radians;
    near(std::abs(sweep_sum), full_turn, tolerance, "circle has one consistent winding");
}

void test_arbitrary_beam_has_nontrivial_plan_projection() {
    using namespace sketch;
    const Beam beam{
        .id = "arbitrary-beam",
        .start = {-4.0, 1.0, 0.0},
        .end = {3.0, 5.0, 2.0},
        .up = {0.0, 0.0, 1.0},
        .width = 0.4,
        .depth = 0.6,
    };
    const auto projected = project_building_plan(beam);
    const auto bounds = projected_bounds(projected);
    require(bounds.max_x - bounds.min_x > 6.0,
            "arbitrary beam projection must preserve its plan axis");
    require(bounds.max_y - bounds.min_y > 3.0,
            "arbitrary beam projection must preserve its plan axis");
    require_same_extents(bounds, shape_bounds(beam), "arbitrary beam");
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void test_invalid_geometry_is_rejected() {
    using namespace sketch;
    rejected(
        [] {
            (void)project_building_plan(RectangularColumn{
                .id = "invalid-column",
                .base_center = {},
                .width = 0.0,
                .depth = 1.0,
                .height = 2.0,
                .rotation_radians = 0.0,
            });
        },
        "zero-width column must be rejected");
    rejected(
        [] {
            (void)project_building_plan(Beam{
                .id = "invalid-beam",
                .start = {},
                .end = {1.0, 0.0, 0.0},
                .up = {1.0, 0.0, 0.0},
                .width = 0.1,
                .depth = 0.1,
            });
        },
        "parallel beam up vector must be rejected");
}

}  // namespace

int main() {
    try {
        test_all_eight_forms_have_exact_plan_extents();
        test_simple_solids_have_exact_edge_sets();
        test_rotated_stairs_have_exact_edge_sets();
        test_roof_forms_have_exact_edge_sets();
        test_nonhorizontal_beam_has_exact_visible_edges();
        test_translation_and_rotation_are_preserved();
        test_circular_column_retains_analytic_arcs();
        test_arbitrary_beam_has_nontrivial_plan_projection();
        test_invalid_geometry_is_rejected();
        std::cout << "Building plan projection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
