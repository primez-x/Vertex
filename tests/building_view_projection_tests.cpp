#include "sketch/building_view_projection.hpp"
#include "sketch/architecture.hpp"

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
using sketch::SlopedRoofPanel;
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

void test_flat_roof_projects_in_all_views() {
    const SlopedRoofPanel panel{
        .id = "flat-roof-view",
        .base_position = {1.0, 2.0, 4.0},
        .orientation_radians = 0.0,
        .run = 4.0,
        .span = 3.0,
        .rise = 0.0,
        .pitch_radians = 0.0,
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const BuildingViewFrame plan_frame{
        {0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}};
    const auto plan = sketch::project_building_view(panel, BuildingViewKind::plan,
                                                     plan_frame);
    const auto plan_bounds = bounds(plan);
    near(plan_bounds.min_x, 0.8, 1e-5, "flat roof plan minimum X");
    near(plan_bounds.max_x, 5.2, 1e-5, "flat roof plan maximum X");
    near(plan_bounds.min_y, 1.8, 1e-5, "flat roof plan minimum Y");
    near(plan_bounds.max_y, 5.2, 1e-5, "flat roof plan maximum Y");

    const BuildingViewFrame elevation_frame{
        {0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}};
    const auto elevation = sketch::project_building_view(
        panel, BuildingViewKind::elevation, elevation_frame);
    const auto elevation_bounds = bounds(elevation);
    near(elevation_bounds.min_x, -5.2, 1e-5, "flat roof elevation minimum X");
    near(elevation_bounds.max_x, -0.8, 1e-5, "flat roof elevation maximum X");
    near(elevation_bounds.min_y, 3.9, 1e-5, "flat roof elevation minimum Z");
    near(elevation_bounds.max_y, 4.0, 1e-5, "flat roof elevation maximum Z");

    const BuildingViewFrame section_frame{
        {0.0, 3.3, 4.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}};
    const auto section = sketch::project_building_view(
        panel, BuildingViewKind::section, section_frame);
    const auto section_bounds = bounds(section);
    near(section_bounds.min_x, -5.2, 1e-5, "flat roof section minimum X");
    near(section_bounds.max_x, -0.8, 1e-5, "flat roof section maximum X");
    near(section_bounds.min_y, -0.1, 1e-5, "flat roof section minimum Z");
    near(section_bounds.max_y, 0.0, 1e-5, "flat roof section maximum Z");
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

void test_terrain_shape_projects_in_all_views() {
    const sketch::TerrainSurface terrain(
        "projection fixture",
        {sketch::TerrainPoint{"p0", 0.0, 0.0, 0.0},
         sketch::TerrainPoint{"p1", 4.0, 0.0, 1.0},
         sketch::TerrainPoint{"p2", 4.0, 3.0, 2.0},
         sketch::TerrainPoint{"p3", 0.0, 3.0, 0.5}},
        {sketch::TerrainTriangle{{0, 1, 2}}, sketch::TerrainTriangle{{0, 2, 3}}});
    const auto shape = sketch::make_terrain_surface(terrain);
    for (const auto kind : {BuildingViewKind::plan, BuildingViewKind::elevation}) {
        require(!sketch::project_shape_view(shape, kind).empty(),
                "terrain shape must project to plan and elevation lines");
    }
    const auto section = sketch::project_shape_view(
        shape, BuildingViewKind::section,
        BuildingViewFrame{{0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}});
    require(!section.empty(), "terrain shape must intersect a horizontal section");
}

}  // namespace

void test_hip_roof_views() {
    const sketch::HipRoof roof{"hip-views", {0, 0, 3}, 0, 6, 4, 1, std::atan(0.5), 0.2, 0.1};
    const BuildingViewFrame plan_frame{{0, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    const auto plan = sketch::project_building_view(roof, BuildingViewKind::plan, plan_frame);
    const auto plan_bounds = bounds(plan);
    near(plan_bounds.min_x, -3.2, 1e-5, "hip plan left eave");
    near(plan_bounds.max_x, 3.2, 1e-5, "hip plan right eave");
    near(plan_bounds.min_y, -2.2, 1e-5, "hip plan lower eave");
    near(plan_bounds.max_y, 2.2, 1e-5, "hip plan upper eave");
    const BuildingViewFrame vertical_frame{{0, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    for (const auto kind : {BuildingViewKind::elevation, BuildingViewKind::section}) {
        const auto projected = sketch::project_building_view(roof, kind, vertical_frame);
        const auto extent = bounds(projected);
        near(extent.min_x, -3.2, 1e-5, "hip vertical view left extent");
        near(extent.max_x, 3.2, 1e-5, "hip vertical view right extent");
        near(extent.max_y, 4.0, 1e-5, "hip vertical view ridge");
        near(extent.min_y, 2.9 - 0.1 * std::sqrt(1.25), 1e-5, "hip lower eave underside");
    }
}

int main() {
    try {
        test_elevation_preserves_horizontal_width_and_height();
        test_horizontal_section_retains_analytic_circle();
        test_vertical_section_intersects_rectangular_column();
        test_flat_roof_projects_in_all_views();
        test_hip_roof_views();
        test_invalid_frame_and_missed_section_fail_closed();
        test_terrain_shape_projects_in_all_views();
        std::cout << "Building view projection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
