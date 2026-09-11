#include "sketch/building_view_projection.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {

using sketch::Boundary;
using sketch::BuildingViewFrame;
using sketch::BuildingViewKind;
using sketch::CircularColumn;
using sketch::RectangularColumn;
using sketch::Vec2;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

void near(double actual, double expected, double tolerance, std::string_view message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

struct Bounds {
    double min_x{1e300};
    double min_y{1e300};
    double max_x{-1e300};
    double max_y{-1e300};
    void add(Vec2 point) {
        require(std::isfinite(point.x) && std::isfinite(point.y),
                "projection point must be finite");
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }
};

Bounds bounds(const Boundary& edges) {
    require(!edges.empty(), "projection must contain edges");
    Bounds result;
    for (const auto& edge : edges) {
        result.add(edge.start);
        result.add(edge.end);
    }
    return result;
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

void test_elevation_preserves_horizontal_width_and_height() {
    const RectangularColumn column{
        .id = "elevation-column",
        .base_center = {4.0, 9.0, 1.25},
        .width = 2.0,
        .depth = 0.75,
        .height = 3.5,
        .rotation_radians = 0.0,
    };
    const auto projected = sketch::project_building_view(
        column, BuildingViewKind::elevation,
        BuildingViewFrame{{0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}});
    const auto actual = bounds(projected);
    // The view's local horizontal axis is intentionally allowed to point
    // either way; extents prove that the frame, rather than screen pixels,
    // controls the result.
    near(actual.min_x, -5.0, 1e-6, "elevation minimum horizontal extent");
    near(actual.max_x, -3.0, 1e-6, "elevation maximum horizontal extent");
    near(actual.min_y, 1.25, 1e-6, "elevation base height");
    near(actual.max_y, 4.75, 1e-6, "elevation top height");
}

void test_horizontal_section_retains_analytic_circle() {
    const CircularColumn column{
        .id = "section-column",
        .base_center = {2.0, -3.0, 0.0},
        .radius = 1.25,
        .height = 4.0,
    };
    const auto projected = sketch::project_building_view(
        column, BuildingViewKind::section,
        BuildingViewFrame{{0.0, 0.0, 2.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}});
    require(projected.size() == 4, "horizontal circular section must have four arcs");
    for (const auto& edge : projected) {
        require(edge.sweep_radians != 0.0, "circular section must retain analytic arcs");
        const Vec2 chord{edge.end.x - edge.start.x, edge.end.y - edge.start.y};
        near(std::hypot(chord.x, chord.y) /
                 (2.0 * std::sin(std::abs(edge.sweep_radians) * 0.5)),
             column.radius, 1e-5, "circular section radius");
    }
    const auto actual = bounds(projected);
    near(actual.min_x, 0.75, 1e-5, "section circle minimum X");
    near(actual.max_x, 3.25, 1e-5, "section circle maximum X");
    near(actual.min_y, -4.25, 1e-5, "section circle minimum Y");
    near(actual.max_y, -1.75, 1e-5, "section circle maximum Y");
}

void test_vertical_section_intersects_rectangular_column() {
    const RectangularColumn column{
        .id = "vertical-section-column",
        .base_center = {0.0, 0.0, 0.0},
        .width = 2.0,
        .depth = 1.0,
        .height = 3.0,
        .rotation_radians = 0.0,
    };
    const auto projected = sketch::project_building_view(
        column, BuildingViewKind::section,
        BuildingViewFrame{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}});
    const auto actual = bounds(projected);
    near(actual.min_x, -0.5, 1e-5, "vertical section depth minimum");
    near(actual.max_x, 0.5, 1e-5, "vertical section depth maximum");
    near(actual.min_y, 0.0, 1e-5, "vertical section base");
    near(actual.max_y, 3.0, 1e-5, "vertical section top");
}

void test_invalid_frame_and_missed_section_fail_closed() {
    const RectangularColumn column{
        .id = "invalid-frame-column", .base_center = {}, .width = 1.0,
        .depth = 1.0, .height = 1.0, .rotation_radians = 0.0};
    rejected(
        [&] {
            (void)sketch::project_building_view(
                column, BuildingViewKind::elevation,
                BuildingViewFrame{{}, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}});
        },
        "zero direction must be rejected");
    rejected(
        [&] {
            (void)sketch::project_building_view(
                column, BuildingViewKind::section,
                BuildingViewFrame{{0.0, 0.0, 4.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}});
        },
        "a section plane missing the solid must be rejected");
}

}  // namespace

int main() {
    try {
        test_elevation_preserves_horizontal_width_and_height();
        test_horizontal_section_retains_analytic_circle();
        test_vertical_section_intersects_rectangular_column();
        test_invalid_frame_and_missed_section_fail_closed();
        std::cout << "Building view projection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

