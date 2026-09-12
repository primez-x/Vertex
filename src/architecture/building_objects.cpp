#include "sketch/building_objects.hpp"

#include "sketch/architecture.hpp"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace sketch {
namespace {

constexpr double tolerance = default_geometry_tolerance_metres;
constexpr double maximum_dimension = 1'000'000.0;
constexpr double maximum_coordinate = 1'000'000'000.0;
constexpr double maximum_angle = 1'000'000.0;

void finite_coordinate(const Vec3& point, const char* what) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)
        || std::abs(point.x) > maximum_coordinate
        || std::abs(point.y) > maximum_coordinate
        || std::abs(point.z) > maximum_coordinate) {
        throw std::invalid_argument(what);
    }
}

void finite_angle(double angle, const char* what) {
    if (!std::isfinite(angle) || std::abs(angle) > maximum_angle) {
        throw std::invalid_argument(what);
    }
}

void positive_dimension(double value, const char* what) {
    if (!std::isfinite(value) || value <= tolerance || value > maximum_dimension) {
        throw std::invalid_argument(what);
    }
}

void nonnegative_dimension(double value, const char* what) {
    if (!std::isfinite(value) || value < 0.0 || value > maximum_dimension) {
        throw std::invalid_argument(what);
    }
}

void finite_derived(double value, const char* what) {
    if (!std::isfinite(value) || std::abs(value) > maximum_coordinate) {
        throw std::invalid_argument(what);
    }
}

gp_Pnt point(const Vec3& value) {
    return {value.x, value.y, value.z};
}

gp_Vec vector(const Vec3& value) {
    return {value.x, value.y, value.z};
}

gp_Pnt translated(const gp_Pnt& origin, const gp_Vec& offset) {
    return {origin.X() + offset.X(), origin.Y() + offset.Y(), origin.Z() + offset.Z()};
}

void check_point(const gp_Pnt& value, const char* what) {
    finite_derived(value.X(), what);
    finite_derived(value.Y(), what);
    finite_derived(value.Z(), what);
}

TopoDS_Shape finish_solid(TopoDS_Shape shape, const char* what) {
    if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid()) {
        throw std::invalid_argument(what);
    }
    const double volume = solid_volume(shape);
    if (!std::isfinite(volume) || volume <= tolerance * tolerance * tolerance) {
        throw std::invalid_argument(what);
    }
    return shape;
}

template <typename Function>
TopoDS_Shape build_solid(Function&& function, const char* what) {
    try {
        return finish_solid(function(), what);
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string(what) + ": " + error.what());
    }
}

TopoDS_Shape make_box(const gp_Ax2& axes, double dx, double dy, double dz,
                     const char* what) {
    return build_solid(
        [&] {
            BRepPrimAPI_MakeBox builder(axes, dx, dy, dz);
            builder.Build();
            if (!builder.IsDone()) {
                throw std::invalid_argument(what);
            }
            return TopoDS_Shape(builder.Shape());
        },
        what);
}

TopoDS_Shape make_cylinder(const gp_Ax2& axes, double radius, double height,
                           const char* what) {
    return build_solid(
        [&] {
            BRepPrimAPI_MakeCylinder builder(axes, radius, height);
            builder.Build();
            if (!builder.IsDone()) {
                throw std::invalid_argument(what);
            }
            return TopoDS_Shape(builder.Shape());
        },
        what);
}

TopoDS_Face make_polygon_face(const std::vector<gp_Pnt>& points, const char* what) {
    if (points.size() < 3) {
        throw std::invalid_argument(what);
    }
    BRepBuilderAPI_MakePolygon polygon;
    for (const auto& current : points) {
        check_point(current, what);
        polygon.Add(current);
    }
    polygon.Close();
    if (!polygon.IsDone()) {
        throw std::invalid_argument(what);
    }
    BRepBuilderAPI_MakeFace face(polygon.Wire(), true);
    if (!face.IsDone()) {
        throw std::invalid_argument(what);
    }
    return face.Face();
}

TopoDS_Shape make_prism(const std::vector<gp_Pnt>& points, const gp_Vec& extrusion,
                        const char* what) {
    return build_solid(
        [&] {
            BRepPrimAPI_MakePrism prism(make_polygon_face(points, what), extrusion, true);
            prism.Build();
            if (!prism.IsDone()) {
                throw std::invalid_argument(what);
            }
            return TopoDS_Shape(prism.Shape());
        },
        what);
}

TopoDS_Shape make_compound(const TopoDS_Shape& first, const TopoDS_Shape& second,
                           const char* what) {
    return build_solid(
        [&] {
            TopoDS_Compound compound;
            BRep_Builder builder;
            builder.MakeCompound(compound);
            builder.Add(compound, first);
            builder.Add(compound, second);
            return TopoDS_Shape(compound);
        },
        what);
}

struct HorizontalFrame {
    gp_Vec along;
    gp_Vec across;
};

HorizontalFrame horizontal_frame(double orientation_radians, const char* what) {
    finite_angle(orientation_radians, what);
    const double c = std::cos(orientation_radians);
    const double s = std::sin(orientation_radians);
    if (!std::isfinite(c) || !std::isfinite(s)) {
        throw std::invalid_argument(what);
    }
    return {{c, s, 0.0}, {-s, c, 0.0}};
}

gp_Pnt local_point(const gp_Pnt& base, const gp_Vec& along, double along_distance,
                   const gp_Vec& across, double across_distance, double elevation) {
    const gp_Vec offset = along * along_distance + across * across_distance
        + gp_Vec(0.0, 0.0, elevation);
    return translated(base, offset);
}

double checked_pitch_rise(double run, double rise, double pitch, const char* what,
                          bool allow_flat = false) {
    positive_dimension(run, what);
    if (allow_flat && rise == 0.0 && pitch == 0.0) {
        return 0.0;
    }
    positive_dimension(rise, what);
    if (!std::isfinite(pitch) || pitch <= tolerance
        || pitch >= (std::numbers::pi * 0.5 - tolerance)) {
        throw std::invalid_argument(what);
    }
    const double expected_rise = run * std::tan(pitch);
    finite_derived(expected_rise, what);
    const double allowed = std::max(1e-9, std::abs(rise) * 1e-9);
    if (std::abs(expected_rise - rise) > allowed) {
        throw std::invalid_argument(what);
    }
    return rise / run;
}

}  // namespace

TopoDS_Shape make_rectangular_column(const RectangularColumn& column) {
    finite_coordinate(column.base_center, "Rectangular column base must be finite");
    finite_angle(column.rotation_radians, "Rectangular column rotation must be finite");
    positive_dimension(column.width, "Rectangular column width must be positive");
    positive_dimension(column.depth, "Rectangular column depth must be positive");
    positive_dimension(column.height, "Rectangular column height must be positive");

    const auto frame = horizontal_frame(column.rotation_radians,
                                        "Rectangular column rotation is invalid");
    const gp_Pnt base = point(column.base_center);
    const gp_Vec corner_offset = frame.along * (-column.width * 0.5)
        + frame.across * (-column.depth * 0.5);
    const gp_Pnt corner = translated(base, corner_offset);
    check_point(corner, "Rectangular column corner is outside the supported range");
    const gp_Ax2 axes(corner, gp_Dir(0.0, 0.0, 1.0), gp_Dir(frame.along));
    return make_box(axes, column.width, column.depth, column.height,
                    "Rectangular column construction failed");
}

TopoDS_Shape make_circular_column(const CircularColumn& column) {
    finite_coordinate(column.base_center, "Circular column base must be finite");
    positive_dimension(column.radius, "Circular column radius must be positive");
    positive_dimension(column.height, "Circular column height must be positive");
    const gp_Ax2 axes(point(column.base_center), gp_Dir(0.0, 0.0, 1.0));
    return make_cylinder(axes, column.radius, column.height,
                         "Circular column construction failed");
}

TopoDS_Shape make_beam(const Beam& beam) {
    finite_coordinate(beam.start, "Beam start must be finite");
    finite_coordinate(beam.end, "Beam end must be finite");
    finite_coordinate(beam.up, "Beam up vector must be finite");
    positive_dimension(beam.width, "Beam width must be positive");
    positive_dimension(beam.depth, "Beam depth must be positive");

    const gp_Vec axis = vector(beam.end) - vector(beam.start);
    const double axis_length = axis.Magnitude();
    if (!std::isfinite(axis_length) || axis_length <= tolerance) {
        throw std::invalid_argument("Beam axis must have nonzero length");
    }
    finite_derived(axis_length, "Beam axis is outside the supported range");

    const gp_Vec axis_unit = axis / axis_length;
    const gp_Vec up = vector(beam.up);
    const double up_length = up.Magnitude();
    if (!std::isfinite(up_length) || up_length <= tolerance) {
        throw std::invalid_argument("Beam up vector must have nonzero length");
    }
    const gp_Vec projected_up = up - axis_unit * up.Dot(axis_unit);
    const double projected_length = projected_up.Magnitude();
    if (!std::isfinite(projected_length) || projected_length <= tolerance) {
        throw std::invalid_argument("Beam up vector must not be parallel to its axis");
    }
    const gp_Vec local_up = projected_up / projected_length;
    const gp_Vec local_right = local_up.Crossed(axis_unit);
    if (!std::isfinite(local_right.Magnitude()) || local_right.Magnitude() <= tolerance) {
        throw std::invalid_argument("Beam local frame could not be constructed");
    }

    const gp_Pnt start = point(beam.start);
    const gp_Vec corner_offset = local_right * (-beam.width * 0.5)
        + local_up * (-beam.depth * 0.5);
    const gp_Pnt corner = translated(start, corner_offset);
    check_point(corner, "Beam corner is outside the supported range");
    const gp_Ax2 axes(corner, gp_Dir(axis_unit), gp_Dir(local_right));
    return make_box(axes, beam.width, beam.depth, axis_length,
                    "Beam construction failed");
}

TopoDS_Shape make_stair_flight(const StairFlight& flight) {
    finite_coordinate(flight.base_position, "Stair base position must be finite");
    finite_angle(flight.orientation_radians, "Stair orientation must be finite");
    if (flight.riser_count == 0 || flight.riser_count > 10'000) {
        throw std::invalid_argument("Stair riser count is outside the supported range");
    }
    positive_dimension(flight.total_rise, "Stair total rise must be positive");
    positive_dimension(flight.going, "Stair going must be positive");
    positive_dimension(flight.width, "Stair width must be positive");

    const double count = static_cast<double>(flight.riser_count);
    const double total_run = flight.going * count;
    finite_derived(total_run, "Stair total run is outside the supported range");
    if (total_run > maximum_dimension) {
        throw std::invalid_argument("Stair total run is outside the supported range");
    }
    const double riser = flight.total_rise / count;
    positive_dimension(riser, "Stair riser height is too small");

    if (flight.top_landing.has_value()) {
        positive_dimension(flight.top_landing->depth,
                           "Stair landing depth must be positive");
        positive_dimension(flight.top_landing->thickness,
                           "Stair landing thickness must be positive");
        if (flight.top_landing->thickness > flight.total_rise) {
            throw std::invalid_argument("Stair landing must not extend below the base");
        }
    }

    const auto frame = horizontal_frame(flight.orientation_radians,
                                        "Stair orientation is invalid");
    const gp_Pnt base = point(flight.base_position);
    std::vector<gp_Pnt> profile;
    profile.reserve(flight.riser_count * 2 + 4);
    const auto profile_point = [&](double run, double elevation) {
        return local_point(base, frame.along, run, frame.across, 0.0, elevation);
    };
    profile.push_back(profile_point(0.0, 0.0));
    profile.push_back(profile_point(total_run, 0.0));
    profile.push_back(profile_point(total_run, flight.total_rise));
    for (std::size_t index = flight.riser_count; index-- > 0;) {
        const double run = static_cast<double>(index) * flight.going;
        profile.push_back(profile_point(run,
                                        static_cast<double>(index + 1) * riser));
        if (index != 0) {
            profile.push_back(profile_point(run, static_cast<double>(index) * riser));
        }
    }

    const auto steps = make_prism(profile, frame.across * flight.width,
                                  "Stair step construction failed");
    if (!flight.top_landing.has_value()) {
        return steps;
    }

    const auto& landing = *flight.top_landing;
    const gp_Pnt landing_corner = local_point(
        base, frame.along, total_run, frame.across, 0.0,
        flight.total_rise - landing.thickness);
    check_point(landing_corner,
                "Stair landing corner is outside the supported range");
    const gp_Ax2 landing_axes(landing_corner, gp_Dir(0.0, 0.0, 1.0),
                              gp_Dir(frame.along));
    const auto landing_shape = make_box(landing_axes, landing.depth, flight.width,
                                        landing.thickness,
                                        "Stair landing construction failed");
    return make_compound(steps, landing_shape, "Stair flight construction failed");
}

TopoDS_Shape make_sloped_roof_panel(const SlopedRoofPanel& panel) {
    finite_coordinate(panel.base_position, "Roof panel base must be finite");
    positive_dimension(panel.run, "Roof panel run must be positive");
    positive_dimension(panel.span, "Roof panel span must be positive");
    nonnegative_dimension(panel.overhang, "Roof panel overhang must be nonnegative");
    positive_dimension(panel.thickness, "Roof panel thickness must be positive");
    const double slope = checked_pitch_rise(panel.run, panel.rise,
                                            panel.pitch_radians,
                                            "Roof panel pitch and rise must agree", true);
    const auto frame = horizontal_frame(panel.orientation_radians,
                                        "Roof panel orientation is invalid");
    const gp_Pnt base = point(panel.base_position);
    const double x0 = -panel.overhang;
    const double x1 = panel.run + panel.overhang;
    const double y0 = -panel.overhang;
    const double y1 = panel.span + panel.overhang;
    const auto top_point = [&](double x, double y) {
        return local_point(base, frame.along, x, frame.across, y, slope * x);
    };
    const std::vector<gp_Pnt> top = {
        top_point(x0, y0), top_point(x1, y0), top_point(x1, y1), top_point(x0, y1)};
    const double normal_scale = std::sqrt(1.0 + slope * slope);
    finite_derived(normal_scale, "Roof panel slope is outside the supported range");
    const gp_Vec normal = (gp_Vec(0.0, 0.0, 1.0) - frame.along * slope) / normal_scale;
    return make_prism(top, normal * (-panel.thickness),
                      "Roof panel construction failed");
}

TopoDS_Shape make_gable_roof(const GableRoof& roof) {
    finite_coordinate(roof.base_position, "Gable roof base must be finite");
    positive_dimension(roof.length, "Gable roof length must be positive");
    positive_dimension(roof.span, "Gable roof span must be positive");
    nonnegative_dimension(roof.overhang, "Gable roof overhang must be nonnegative");
    positive_dimension(roof.thickness, "Gable roof thickness must be positive");
    const double half_span = roof.span * 0.5;
    positive_dimension(half_span, "Gable roof half span must be positive");
    const double slope = checked_pitch_rise(half_span, roof.rise,
                                            roof.pitch_radians,
                                            "Gable roof pitch and rise must agree");
    const auto frame = horizontal_frame(roof.orientation_radians,
                                        "Gable roof orientation is invalid");
    const gp_Pnt base = point(roof.base_position);
    const double ridge_length = roof.length + 2.0 * roof.overhang;
    finite_derived(ridge_length, "Gable roof ridge length is outside the supported range");
    if (ridge_length > maximum_dimension) {
        throw std::invalid_argument("Gable roof ridge length is outside the supported range");
    }

    const double outer_half_span = half_span + roof.overhang;
    finite_derived(outer_half_span, "Gable roof outer span is outside the supported range");
    const double normal_scale = std::sqrt(1.0 + slope * slope);
    finite_derived(normal_scale, "Gable roof slope is outside the supported range");
    const double inward_shift = slope * roof.thickness / normal_scale;
    const double vertical_drop = roof.thickness / normal_scale;
    // The ridge trim is vertical.  Keeping the normal offset inside each
    // half's horizontal domain removes the normal-offset overlap at the ridge
    // while retaining two closed, real solids with a shared ridge face.
    if (!std::isfinite(inward_shift) || inward_shift <= tolerance
        || inward_shift >= outer_half_span - tolerance) {
        throw std::invalid_argument("Gable roof thickness crosses its ridge domain");
    }

    const double x0 = -ridge_length * 0.5;
    const double left_outer_y = -outer_half_span;
    const double right_outer_y = outer_half_span;
    const double left_outer_top_z = roof.rise + slope * left_outer_y;
    const double right_outer_top_z = roof.rise - slope * right_outer_y;
    const double left_outer_bottom_y = left_outer_y + inward_shift;
    const double right_outer_bottom_y = right_outer_y - inward_shift;
    const double left_outer_bottom_z = left_outer_top_z - vertical_drop;
    const double right_outer_bottom_z = right_outer_top_z - vertical_drop;
    const double ridge_bottom_z = roof.rise - roof.thickness * normal_scale;
    finite_derived(left_outer_top_z, "Gable roof elevation is outside the supported range");
    finite_derived(right_outer_top_z, "Gable roof elevation is outside the supported range");
    finite_derived(left_outer_bottom_z, "Gable roof elevation is outside the supported range");
    finite_derived(right_outer_bottom_z, "Gable roof elevation is outside the supported range");
    finite_derived(ridge_bottom_z, "Gable roof elevation is outside the supported range");

    const auto cross_section_point = [&](double x, double y, double z) {
        return local_point(base, frame.along, x, frame.across, y, z);
    };
    const std::vector<gp_Pnt> left_profile = {
        cross_section_point(x0, left_outer_y, left_outer_top_z),
        cross_section_point(x0, 0.0, roof.rise),
        cross_section_point(x0, 0.0, ridge_bottom_z),
        cross_section_point(x0, left_outer_bottom_y, left_outer_bottom_z),
    };
    const std::vector<gp_Pnt> right_profile = {
        cross_section_point(x0, 0.0, roof.rise),
        cross_section_point(x0, right_outer_y, right_outer_top_z),
        cross_section_point(x0, right_outer_bottom_y, right_outer_bottom_z),
        cross_section_point(x0, 0.0, ridge_bottom_z),
    };
    const auto left_panel = make_prism(left_profile, frame.along * ridge_length,
                                       "Left gable roof panel construction failed");
    const auto right_panel = make_prism(right_profile, frame.along * ridge_length,
                                        "Right gable roof panel construction failed");
    return make_compound(left_panel, right_panel, "Gable roof construction failed");
}

}  // namespace sketch
