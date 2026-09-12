#include "sketch/building_objects.hpp"
#include "sketch/architecture.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(message) + ": expected "
                                 + std::to_string(expected) + ", got "
                                 + std::to_string(actual));
    }
}

void valid_solid(const TopoDS_Shape& shape, std::string_view message) {
    require(!shape.IsNull(), message);
    require(BRepCheck_Analyzer(shape).IsValid(), message);
    require(sketch::solid_volume(shape) > sketch::default_geometry_tolerance_metres,
            message);
}

struct Bounds {
    double xmin{};
    double ymin{};
    double zmin{};
    double xmax{};
    double ymax{};
    double zmax{};
};

Bounds bounds(const TopoDS_Shape& shape) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        throw std::runtime_error("shape has no bounds");
    }
    Bounds result;
    box.Get(result.xmin, result.ymin, result.zmin, result.xmax, result.ymax, result.zmax);
    return result;
}

int solid_count(const TopoDS_Shape& shape) {
    int count = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

double common_volume(const TopoDS_Shape& left, const TopoDS_Shape& right) {
    BRepAlgoAPI_Common operation(left, right);
    operation.Build();
    require(operation.IsDone(), "gable ridge common operation should finish");
    return sketch::solid_volume(operation.Shape());
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

void test_vertical_columns_are_real_solids() {
    using namespace sketch;
    const auto rectangular = make_rectangular_column(RectangularColumn{
        .id = "rectangular",
        .base_center = {1.0, -2.0, 0.75},
        .width = 0.4,
        .depth = 0.6,
        .height = 3.0,
        .rotation_radians = 0.0,
    });
    valid_solid(rectangular, "rectangular column should be a valid solid");
    near(solid_volume(rectangular), 0.4 * 0.6 * 3.0, 1e-9,
         "rectangular column volume");
    const auto rectangular_bounds = bounds(rectangular);
    near(rectangular_bounds.xmin, 0.8, 2e-6, "rectangular column x minimum");
    near(rectangular_bounds.xmax, 1.2, 2e-6, "rectangular column x maximum");
    near(rectangular_bounds.ymin, -2.3, 2e-6, "rectangular column y minimum");
    near(rectangular_bounds.ymax, -1.7, 2e-6, "rectangular column y maximum");
    near(rectangular_bounds.zmin, 0.75, 2e-6, "rectangular column base elevation");
    near(rectangular_bounds.zmax, 3.75, 2e-6, "rectangular column top elevation");

    const auto rotated = make_rectangular_column(RectangularColumn{
        .id = "rotated",
        .base_center = {},
        .width = 0.4,
        .depth = 0.6,
        .height = 3.0,
        .rotation_radians = std::numbers::pi / 2.0,
    });
    near(solid_volume(rotated), 0.4 * 0.6 * 3.0, 1e-9,
         "rotated rectangular column volume");
    const auto rotated_bounds = bounds(rotated);
    near(rotated_bounds.xmin, -0.3, 2e-6, "rotated column x minimum");
    near(rotated_bounds.xmax, 0.3, 2e-6, "rotated column x maximum");
    near(rotated_bounds.ymin, -0.2, 2e-6, "rotated column y minimum");
    near(rotated_bounds.ymax, 0.2, 2e-6, "rotated column y maximum");

    const auto circular = make_circular_column(CircularColumn{
        .id = "round",
        .base_center = {-3.0, 4.0, -0.5},
        .radius = 0.25,
        .height = 2.0,
    });
    valid_solid(circular, "circular column should be a valid solid");
    near(solid_volume(circular), std::numbers::pi * 0.25 * 0.25 * 2.0, 1e-8,
         "circular column volume");
    const auto circular_bounds = bounds(circular);
    near(circular_bounds.zmin, -0.5, 2e-6, "circular column base elevation");
    near(circular_bounds.zmax, 1.5, 2e-6, "circular column top elevation");

    auto invalid_rectangular = RectangularColumn{
        .id = "invalid",
        .base_center = {},
        .width = -0.4,
        .depth = 0.6,
        .height = 3.0,
        .rotation_radians = 0.0,
    };
    rejected([&] { (void)make_rectangular_column(invalid_rectangular); },
             "negative rectangular column width should be rejected");
    invalid_rectangular.width = 0.4;
    invalid_rectangular.base_center.x = std::numeric_limits<double>::infinity();
    rejected([&] { (void)make_rectangular_column(invalid_rectangular); },
             "infinite rectangular column position should be rejected");
    rejected([] {
        (void)make_circular_column(CircularColumn{.radius = 0.0, .height = 2.0});
    }, "zero circular column radius should be rejected");
}

void test_beam_uses_arbitrary_axis_and_local_up() {
    using namespace sketch;
    const Beam beam{
        .id = "diagonal-beam",
        .start = {-1.0, 2.0, 0.5},
        .end = {2.0, 6.0, 3.5},
        .up = {0.0, 0.0, 1.0},
        .width = 0.2,
        .depth = 0.3,
    };
    const auto shape = make_beam(beam);
    valid_solid(shape, "beam should be a valid solid");
    const double length = std::sqrt(3.0 * 3.0 + 4.0 * 4.0 + 3.0 * 3.0);
    near(solid_volume(shape), length * 0.2 * 0.3, 1e-8, "beam volume");

    rejected([] {
        (void)make_beam(Beam{.start = {}, .end = {1.0, 0.0, 0.0},
                             .up = {1.0, 0.0, 0.0}, .width = 0.2, .depth = 0.3});
    }, "beam with parallel up vector should be rejected");
    rejected([] {
        (void)make_beam(Beam{.start = {}, .end = {}, .up = {0.0, 0.0, 1.0},
                             .width = 0.2, .depth = 0.3});
    }, "degenerate beam axis should be rejected");
    rejected([] {
        (void)make_beam(Beam{.start = {}, .end = {1.0, 0.0, 0.0},
                             .up = {0.0, std::numeric_limits<double>::quiet_NaN(), 1.0},
                             .width = 0.2, .depth = 0.3});
    }, "NaN beam up vector should be rejected");
}

void test_stairs_have_step_volume_and_optional_landing() {
    using namespace sketch;
    const StairFlight flight{
        .id = "flight",
        .base_position = {1.0, 2.0, 0.5},
        .orientation_radians = 0.0,
        .riser_count = 4,
        .total_rise = 2.0,
        .going = 0.25,
        .width = 1.5,
        .top_landing = std::nullopt,
    };
    const auto steps = make_stair_flight(flight);
    valid_solid(steps, "stair flight should be a valid solid");
    const double riser = flight.total_rise / static_cast<double>(flight.riser_count);
    const double expected = flight.width * flight.going * riser
        * static_cast<double>(flight.riser_count * (flight.riser_count + 1)) / 2.0;
    near(solid_volume(steps), expected, 1e-8, "stair step volume");
    const auto step_bounds = bounds(steps);
    near(step_bounds.xmin, 1.0, 2e-6, "stair run minimum");
    near(step_bounds.xmax, 2.0, 2e-6, "stair run maximum");
    near(step_bounds.ymin, 2.0, 2e-6, "stair width minimum");
    near(step_bounds.ymax, 3.5, 2e-6, "stair width maximum");
    near(step_bounds.zmin, 0.5, 2e-6, "stair base elevation");
    near(step_bounds.zmax, 2.5, 2e-6, "stair top elevation");

    auto with_landing = flight;
    with_landing.top_landing = StairLanding{.depth = 0.5, .thickness = 0.2};
    const auto landed = make_stair_flight(with_landing);
    valid_solid(landed, "stair landing should be a valid solid");
    near(solid_volume(landed), expected + flight.width * 0.5 * 0.2, 1e-8,
         "stair landing volume");
    near(bounds(landed).xmax, 2.5, 2e-6, "stair landing run maximum");

    auto rotated = flight;
    rotated.orientation_radians = std::numbers::pi / 2.0;
    near(solid_volume(make_stair_flight(rotated)), expected, 1e-8,
         "rotated stair volume");

    auto invalid = flight;
    invalid.riser_count = 0;
    rejected([&] { (void)make_stair_flight(invalid); },
             "zero stair risers should be rejected");
    invalid = flight;
    invalid.total_rise = -1.0;
    rejected([&] { (void)make_stair_flight(invalid); },
             "negative stair rise should be rejected");
    invalid = flight;
    invalid.top_landing = StairLanding{.depth = 0.5, .thickness = 2.1};
    rejected([&] { (void)make_stair_flight(invalid); },
             "landing extending below stair base should be rejected");
}

void test_roofs_are_planar_thickened_panels() {
    using namespace sketch;
    const SlopedRoofPanel panel{
        .id = "shed",
        .base_position = {0.0, 0.0, 4.0},
        .orientation_radians = 0.0,
        .run = 4.0,
        .span = 3.0,
        .rise = 1.0,
        .pitch_radians = std::atan(1.0 / 4.0),
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const auto panel_shape = make_sloped_roof_panel(panel);
    valid_solid(panel_shape, "sloped roof panel should be a valid solid");
    const double panel_slope = 1.0 / 4.0;
    const double expected_panel_volume = (4.0 + 2.0 * 0.2)
        * std::sqrt(1.0 + panel_slope * panel_slope)
        * (3.0 + 2.0 * 0.2) * 0.1;
    near(solid_volume(panel_shape), expected_panel_volume, 1e-8,
         "sloped roof panel volume");

    const SlopedRoofPanel flat{
        .id = "flat",
        .base_position = {1.0, -2.0, 4.0},
        .orientation_radians = std::numbers::pi / 8.0,
        .run = 4.0,
        .span = 3.0,
        .rise = 0.0,
        .pitch_radians = 0.0,
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const auto flat_shape = make_sloped_roof_panel(flat);
    valid_solid(flat_shape, "flat roof panel should be a valid solid");
    near(solid_volume(flat_shape), (4.0 + 2.0 * 0.2) * (3.0 + 2.0 * 0.2) * 0.1,
         1e-8, "flat roof panel volume");

    const GableRoof gable{
        .id = "gable",
        .base_position = {0.0, 0.0, 4.0},
        .orientation_radians = std::numbers::pi / 6.0,
        .length = 5.0,
        .span = 4.0,
        .rise = 1.0,
        .pitch_radians = std::atan(1.0 / 2.0),
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const auto gable_shape = make_gable_roof(gable);
    valid_solid(gable_shape, "gable roof should be a valid solid");
    require(solid_count(gable_shape) == 2,
            "gable roof should contain two ridge-clipped panel solids");
    TopExp_Explorer solids(gable_shape, TopAbs_SOLID);
    const TopoDS_Shape left_panel = solids.Current();
    solids.Next();
    const TopoDS_Shape right_panel = solids.Current();
    near(common_volume(left_panel, right_panel), 0.0, 1e-9,
         "gable ridge panels must not have positive-volume overlap");
    const double half_span_with_overhang = 2.0 + 0.2;
    const double sloped_half_length = half_span_with_overhang
        * std::sqrt(1.0 + 0.5 * 0.5);
    const double ridge_overlap_removed = (5.0 + 2.0 * 0.2)
        * 0.5 * 0.1 * 0.1;
    const double expected_gable_volume = 2.0 * (5.0 + 2.0 * 0.2)
        * sloped_half_length * 0.1 - ridge_overlap_removed;
    near(solid_volume(gable_shape), expected_gable_volume, 1e-8,
         "gable roof volume");

    auto invalid = panel;
    invalid.rise = std::numeric_limits<double>::quiet_NaN();
    rejected([&] { (void)make_sloped_roof_panel(invalid); },
             "NaN roof rise should be rejected");
    invalid = panel;
    invalid.pitch_radians += 0.01;
    rejected([&] { (void)make_sloped_roof_panel(invalid); },
             "inconsistent roof pitch should be rejected");
    invalid = flat;
    invalid.pitch_radians = 0.01;
    rejected([&] { (void)make_sloped_roof_panel(invalid); },
             "flat roof with nonzero pitch should be rejected");
    invalid = flat;
    invalid.rise = 0.01;
    rejected([&] { (void)make_sloped_roof_panel(invalid); },
             "flat roof with nonzero rise should be rejected by pitch consistency");
    invalid = panel;
    invalid.overhang = -0.01;
    rejected([&] { (void)make_sloped_roof_panel(invalid); },
             "negative roof overhang should be rejected");
    auto invalid_gable = gable;
    invalid_gable.thickness = 0.0;
    rejected([&] { (void)make_gable_roof(invalid_gable); },
             "zero gable thickness should be rejected");
    invalid_gable = gable;
    invalid_gable.thickness = 10.0;
    rejected([&] { (void)make_gable_roof(invalid_gable); },
             "gable thickness crossing the ridge domain should be rejected");
}

}  // namespace

void test_hip_roofs() {
    using namespace sketch;
    for (const double length : {8.0, 4.0}) {
      for (const double slope : {0.25, 1.0}) {
        const HipRoof roof{"hip", {10, -5, 3}, std::numbers::pi / 2.0,
            length, 4.0, 2.0 * slope, std::atan(slope), 0.3, 0.2};
        const auto shape = make_hip_roof(roof);
        valid_solid(shape, "hip roof must contain valid closed solids");
        require(solid_count(shape) == 4, "hip roof must have four slopes including square pyramids");
        const auto drop = 0.2 * std::sqrt(1.0 + slope * slope);
        near(solid_volume(shape), (length + 0.6) * 4.6 * drop, 1e-7,
            "hip volume equals projected area times vertical thickness");
        const auto box = bounds(shape);
        near(box.xmin, 7.7, 1e-6, "rotated hip minimum X");
        near(box.xmax, 12.3, 1e-6, "rotated hip maximum X");
        near(box.ymin, -5.0 - length * 0.5 - 0.3, 1e-6, "rotated hip minimum Y");
        near(box.ymax, -5.0 + length * 0.5 + 0.3, 1e-6, "rotated hip maximum Y");
        near(box.zmin, 3.0 - 0.3 * slope - drop, 1e-6, "overhung hip lower eave");
        near(box.zmax, 3.0 + 2.0 * slope, 1e-6, "hip ridge elevation");
        std::vector<TopoDS_Shape> panels;
        for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next())
            panels.push_back(explorer.Current());
        for (std::size_t i = 0; i < panels.size(); ++i)
            for (std::size_t j = i + 1; j < panels.size(); ++j)
                near(common_volume(panels[i], panels[j]), 0.0, 1e-8,
                    "hip panels must not overlap in positive volume");
        auto invalid = roof;
        invalid.length = 3.0;
        rejected([&] { (void)make_hip_roof(invalid); }, "shorter length than span rejected");
        invalid = roof; invalid.pitch_radians = 0.2;
        rejected([&] { (void)make_hip_roof(invalid); }, "inconsistent hip pitch rejected");
        invalid = roof; invalid.base_position.x = std::numeric_limits<double>::infinity();
        rejected([&] { (void)make_hip_roof(invalid); }, "nonfinite hip location rejected");
        invalid = roof; invalid.thickness = 0.0;
        rejected([&] { (void)make_hip_roof(invalid); }, "zero hip thickness rejected");
      }
    }
}

void test_roof_openings() {
    using namespace sketch;
    HipRoof hip{"cut-hip", {2, 3, 4}, 0.4, 8, 6, 1.5, std::atan(0.5), 0.2, 0.15};
    const auto original = make_hip_roof(hip);
    hip.openings = {{"skylight", -0.5, -0.5, 1.0, 1.0}};
    const auto cut = make_hip_roof(hip);
    valid_solid(cut, "hip opening must retain valid roof solids");
    near(solid_volume(original) - solid_volume(cut), 0.15 * std::sqrt(1.25), 1e-7,
        "hip opening removes projected area times vertical thickness across ridge");
    GableRoof gable{"cut-gable", {2, 3, 4}, 0.4, 8, 6, 1.5, std::atan(0.5), 0.2, 0.15};
    const auto full_gable = make_gable_roof(gable);
    gable.openings = hip.openings;
    near(solid_volume(full_gable) - solid_volume(make_gable_roof(gable)),
         0.15 * std::sqrt(1.25), 1e-7, "gable ridge opening cut volume");
    SlopedRoofPanel panel{"cut-panel", {2, 3, 4}, 0.4, 6, 4, 1.5, std::atan(0.25), 0.2, 0.15};
    const auto full_panel = make_sloped_roof_panel(panel);
    panel.openings = {{"skylight", 1, 1, 1, 1}};
    near(solid_volume(full_panel) - solid_volume(make_sloped_roof_panel(panel)),
         0.15 * std::sqrt(1.0625), 1e-7, "sloped panel opening cut volume");
    hip.openings.push_back({"overlapping", 0, 0, 1, 1});
    rejected([&] { (void)make_hip_roof(hip); }, "overlapping openings rejected");
    hip.openings = {{"outside", 20, 20, 1, 1}};
    rejected([&] { (void)make_hip_roof(hip); }, "outside openings rejected");
    hip.openings = {{"edge", -4, -3, 1, 1}};
    rejected([&] { (void)make_hip_roof(hip); }, "edge notches are not through-openings");
    hip.openings = {{"bad", 0, 0, 0, 1}};
    rejected([&] { (void)make_hip_roof(hip); }, "zero opening width rejected");
}

int main() {
    try {
        test_vertical_columns_are_real_solids();
        test_beam_uses_arbitrary_axis_and_local_up();
        test_stairs_have_step_volume_and_optional_landing();
        test_roofs_are_planar_thickened_panels();
        test_hip_roofs();
        test_roof_openings();
        std::cout << "Building object solid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
