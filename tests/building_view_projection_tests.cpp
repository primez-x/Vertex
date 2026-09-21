#include "sketch/building_view_projection.hpp"
#include "sketch/architecture.hpp"

#include <algorithm>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <limits>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {

using sketch::Boundary;
using sketch::BuildingViewDepth;
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

void test_conservative_far_depth_filter() {
    const RectangularColumn near_column{
        .id = "near-depth-column",
        .base_center = {0.0, -2.0, 0.0},
        .width = 1.0,
        .depth = 1.0,
        .height = 2.0,
        .rotation_radians = 0.0,
    };
    const auto near_shape = sketch::make_building_shape(near_column);
    const BuildingViewDepth near_depth{{0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, 3.0};
    require(sketch::shape_intersects_view_depth(near_shape, near_depth),
            "a shape before the far depth must remain visible");

    const RectangularColumn far_column{
        .id = "far-depth-column",
        .base_center = {0.0, -4.0, 0.0},
        .width = 1.0,
        .depth = 1.0,
        .height = 2.0,
        .rotation_radians = 0.0,
    };
    const auto far_shape = sketch::make_building_shape(far_column);
    const BuildingViewDepth clipped_depth{{0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, 3.4};
    require(!sketch::shape_intersects_view_depth(far_shape, clipped_depth),
            "a shape entirely beyond the far depth must be culled");
    const BuildingViewDepth boundary_depth{{0.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, 3.6};
    require(sketch::shape_intersects_view_depth(far_shape, boundary_depth),
            "a shape crossing the far depth must remain visible for exact clipping");
    const auto clipped = sketch::clip_shape_to_view_depth(far_shape, boundary_depth);
    require(!clipped.IsNull(), "a crossing shape must produce a clipped solid");
    const auto full_volume = sketch::solid_volume(far_shape);
    const auto clipped_volume = sketch::solid_volume(clipped);
    require(clipped_volume > 0.0 && clipped_volume < full_volume,
            "far-depth clipping must retain only the near portion of a crossing solid");
    require(sketch::clip_shape_to_view_depth(far_shape, clipped_depth).IsNull(),
            "a wholly distant shape must clip to a null solid");

    rejected(
        [&] {
            (void)sketch::shape_intersects_view_depth(
                near_shape, BuildingViewDepth{{}, {0.0, 0.0, 0.0}, 1.0});
        },
        "far-depth filtering must reject a non-unit direction");
    rejected(
        [&] {
            (void)sketch::shape_intersects_view_depth(
                near_shape, BuildingViewDepth{{}, {0.0, -1.0, 0.0}, -1.0});
        },
        "far-depth filtering must reject a negative limit");
}

void test_view_crop() {
    const auto shape = BRepPrimAPI_MakeBox(4.0, 6.0, 2.0).Shape();
    const sketch::BuildingViewCrop inside{{}, -1, 5, -1, 7};
    require(sketch::shape_intersects_view_crop(shape, inside), "inside crop intersects");
    require(sketch::clip_shape_to_view_crop(shape, inside).IsSame(shape),
            "inside crop preserves original shape");
    near(sketch::solid_volume(shape), 48, 1e-6, "source volume");
    require(sketch::clip_shape_to_view_crop(shape, {{}, 0,4,0,6}).IsSame(shape),
            "crop exactly on solid boundary preserves source");
    for (const auto crop : {sketch::BuildingViewCrop{{}, 5, 7, -1, 7},
                            sketch::BuildingViewCrop{{}, -3, -1, -1, 7},
                            sketch::BuildingViewCrop{{}, -1, 5, 7, 8},
                            sketch::BuildingViewCrop{{}, -1, 5, -3, -1}}) {
        require(!sketch::shape_intersects_view_crop(shape, crop), "outside crop misses");
        require(sketch::clip_shape_to_view_crop(shape, crop).IsNull(), "outside crop is null");
    }
    for (const auto crop : {sketch::BuildingViewCrop{{}, 1, 5, -1, 7},
                            sketch::BuildingViewCrop{{}, -1, 3, -1, 7},
                            sketch::BuildingViewCrop{{}, -1, 5, 1, 7},
                            sketch::BuildingViewCrop{{}, -1, 5, -1, 5},
                            sketch::BuildingViewCrop{{}, 1, 3, 1, 5}}) {
        require(sketch::shape_intersects_view_crop(shape, crop), "crossing crop intersects");
        const auto clipped = sketch::clip_shape_to_view_crop(shape, crop);
        const auto x0 = std::max(0.0, crop.min_horizontal_m);
        const auto x1 = std::min(4.0, crop.max_horizontal_m);
        const auto y0 = std::max(0.0, crop.min_vertical_m);
        const auto y1 = std::min(6.0, crop.max_vertical_m);
        near(sketch::solid_volume(clipped), (x1-x0)*(y1-y0)*2, 1e-6, "crossing crop volume");
        const auto extent = bounds(sketch::project_shape_view(clipped, BuildingViewKind::plan));
        near(extent.min_x, x0, 1e-6, "crop minimum horizontal");
        near(extent.max_x, x1, 1e-6, "crop maximum horizontal");
        near(extent.min_y, y0, 1e-6, "crop minimum vertical");
        near(extent.max_y, y1, 1e-6, "crop maximum vertical");
    }
    // Rotate both the shape and view about a non-axis-aligned axis, then translate.
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0,0,0), gp_Dir(1,2,3)), 0.7);
    transform.SetTranslationPart(gp_Vec(12,-7,4));
    const auto moved = BRepBuilderAPI_Transform(shape, transform, true).Shape();
    const auto direction = gp_Vec(0,0,-1).Transformed(transform);
    const auto up = gp_Vec(0,1,0).Transformed(transform);
    const BuildingViewFrame frame{{12,-7,4}, {direction.X(),direction.Y(),direction.Z()},
                                  {up.X(),up.Y(),up.Z()}};
    require(sketch::clip_shape_to_view_crop(moved, {frame, -1,5,-1,7}).IsSame(moved),
            "rotated contained crop preserves original shape");
    const auto clipped = sketch::clip_shape_to_view_crop(moved, {frame, 1,3,1,5});
    near(sketch::solid_volume(clipped), 16, 1e-6, "rotated translated crop volume");
    const auto extent = bounds(sketch::project_shape_view(clipped, BuildingViewKind::plan, frame));
    near(extent.min_x, 1, 1e-6, "rotated crop minimum horizontal");
    near(extent.max_x, 3, 1e-6, "rotated crop maximum horizontal");
    near(extent.min_y, 1, 1e-6, "rotated crop minimum vertical");
    near(extent.max_y, 5, 1e-6, "rotated crop maximum vertical");
    near(sketch::solid_volume(shape), 48, 1e-6, "crop never changes source volume");
    require(!sketch::shape_intersects_view_crop({}, inside), "null shape misses crop");
    require(sketch::clip_shape_to_view_crop({}, inside).IsNull(), "null shape stays null");

    const auto cylinder = BRepPrimAPI_MakeCylinder(2.0, 2.0).Shape();
    const sketch::BuildingViewCrop corner{{}, 1.5,2.5,1.5,2.5};
    require(sketch::shape_intersects_view_crop(cylinder, corner),
            "conservative bbox may overlap an empty corner");
    require(sketch::clip_shape_to_view_crop(cylinder, corner).IsNull(),
            "exact empty boolean intersection returns null");
    const auto half_cylinder = sketch::clip_shape_to_view_crop(cylinder, {{}, 0,3,-3,3});
    near(sketch::solid_volume(half_cylinder), 4 * std::numbers::pi, 1e-6,
         "crop preserves analytic curved solid volume");
    const auto curved_projection = sketch::project_shape_view(half_cylinder, BuildingViewKind::plan);
    require(std::any_of(curved_projection.begin(), curved_projection.end(),
                       [](const auto& edge) { return edge.sweep_radians != 0.0; }),
            "cropped circular solid retains analytic projected arcs");
}

void test_terrain_crop_preserves_crossing_faces() {
    const sketch::TerrainSurface terrain(
        "cropped projection fixture",
        {sketch::TerrainPoint{"p0", 0.0, 0.0, 0.0},
         sketch::TerrainPoint{"p1", 6.0, 0.0, 1.0},
         sketch::TerrainPoint{"p2", 6.0, 5.0, 3.0},
         sketch::TerrainPoint{"p3", 0.0, 5.0, 1.0}},
        {sketch::TerrainTriangle{{0, 1, 2}}, sketch::TerrainTriangle{{0, 2, 3}}});
    const auto shape = sketch::make_terrain_surface(terrain);
    const sketch::BuildingViewCrop crop{{}, 1.0, 4.0, 1.0, 4.0};
    const auto clipped = sketch::clip_shape_to_view_crop(shape, crop);
    require(!clipped.IsNull(), "a terrain surface crossing a crop must retain clipped faces");
    for (const auto kind : {BuildingViewKind::plan, BuildingViewKind::elevation}) {
        require(!sketch::project_shape_view(clipped, kind).empty(),
                "cropped terrain must still project in plan and elevation");
    }
    const auto section = sketch::project_shape_view(
        clipped, BuildingViewKind::section,
        BuildingViewFrame{{0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}});
    require(!section.empty(), "cropped terrain must still intersect a section plane");

    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), 0.45);
    transform.SetTranslationPart(gp_Vec(7, -3, 2));
    const auto moved = BRepBuilderAPI_Transform(shape, transform, true).Shape();
    const auto direction = gp_Vec(0, 0, -1).Transformed(transform);
    const auto up = gp_Vec(0, 1, 0).Transformed(transform);
    const BuildingViewFrame frame{{7, -3, 2},
        {direction.X(), direction.Y(), direction.Z()},
        {up.X(), up.Y(), up.Z()}};
    const auto rotated = sketch::clip_shape_to_view_crop(
        moved, {frame, 1.0, 4.0, 1.0, 4.0});
    require(!rotated.IsNull() &&
                !sketch::project_shape_view(rotated, BuildingViewKind::plan, frame).empty(),
            "rotated view crops must retain crossing terrain faces");

    const sketch::Vec3 depth_origin{
        frame.origin.x - frame.direction.x * 4.0,
        frame.origin.y - frame.direction.y * 4.0,
        frame.origin.z - frame.direction.z * 4.0};
    const sketch::BuildingViewDepth depth{depth_origin, frame.direction, 2.5};
    const auto depth_clipped = sketch::clip_shape_to_view_depth(moved, depth);
    require(!depth_clipped.IsNull() && !depth_clipped.IsSame(moved),
            "far-depth clipping must remove part of a crossing terrain surface");
    const auto combined = sketch::clip_shape_to_view_crop(
        depth_clipped, {frame, 1.0, 4.0, 1.0, 4.0});
    require(!combined.IsNull() && !combined.IsSame(depth_clipped) &&
                !sketch::project_shape_view(combined, BuildingViewKind::plan, frame).empty(),
            "combined far-depth and crop clipping must retain terrain faces");
}

void test_invalid_view_crop() {
    const auto shape = BRepPrimAPI_MakeBox(1.0,1.0,1.0).Shape();
    const sketch::BuildingViewCrop valid{{}, -1,2,-1,2};
    const auto check = [&](const sketch::BuildingViewCrop& crop) {
        rejected([&] { (void)sketch::shape_intersects_view_crop(shape, crop); }, "invalid crop filter rejected");
        rejected([&] { (void)sketch::clip_shape_to_view_crop(shape, crop); }, "invalid crop clip rejected");
    };
    for (int index = 0; index < 4; ++index) {
        for (double value : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), -1000001.0, 1000001.0}) {
            auto crop = valid;
            double* fields[]{&crop.min_horizontal_m, &crop.max_horizontal_m,
                             &crop.min_vertical_m, &crop.max_vertical_m};
            *fields[index] = value;
            check(crop);
        }
    }
    auto crop = valid; crop.max_horizontal_m = crop.min_horizontal_m; check(crop);
    crop = valid; crop.max_vertical_m = -2; check(crop);
    crop = valid; crop.max_horizontal_m = crop.min_horizontal_m + 0.5e-6; check(crop);
    crop = valid; crop.max_vertical_m = crop.min_vertical_m + 0.5e-6; check(crop);
    crop = valid; crop.frame.direction = {}; check(crop);
    crop = valid; crop.frame.up = crop.frame.direction; check(crop);
    crop = valid; crop.frame.origin.x = std::numeric_limits<double>::infinity(); check(crop);
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
        test_conservative_far_depth_filter();
        test_view_crop();
        test_terrain_crop_preserves_crossing_faces();
        test_invalid_view_crop();
        std::cout << "Building view projection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
