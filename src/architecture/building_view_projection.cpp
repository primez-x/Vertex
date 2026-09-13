#include "sketch/building_view_projection.hpp"

#include <BRepAlgoAPI_Section.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Circle.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

constexpr double tolerance = default_geometry_tolerance_metres;
constexpr double full_turn = 2.0 * std::numbers::pi;
constexpr double quarter_turn = std::numbers::pi * 0.5;
constexpr double full_circle_tolerance = 1e-7;

[[noreturn]] void projection_error(std::string message) {
    throw std::invalid_argument(std::move(message));
}

double length(Vec3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

double dot(Vec3 left, Vec3 right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 cross(Vec3 left, Vec3 right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

void validate_frame(const BuildingViewFrame& frame) {
    if (!std::isfinite(frame.origin.x) || !std::isfinite(frame.origin.y) ||
        !std::isfinite(frame.origin.z) || !std::isfinite(frame.direction.x) ||
        !std::isfinite(frame.direction.y) || !std::isfinite(frame.direction.z) ||
        !std::isfinite(frame.up.x) || !std::isfinite(frame.up.y) ||
        !std::isfinite(frame.up.z)) {
        projection_error("Building view frame contains a non-finite value");
    }
    const double direction_length = length(frame.direction);
    const double up_length = length(frame.up);
    if (std::abs(direction_length - 1.0) > 1e-9 ||
        std::abs(up_length - 1.0) > 1e-9 ||
        std::abs(dot(frame.direction, frame.up)) > 1e-9) {
        projection_error("Building view frame direction and up must be orthonormal");
    }
}

struct FrameBasis {
    gp_Pnt origin;
    gp_Vec horizontal;
    gp_Vec vertical;
    gp_Vec normal;
};

FrameBasis basis(const BuildingViewFrame& frame) {
    validate_frame(frame);
    // The public direction points from the viewer toward the model. OCCT's
    // projector uses the opposite normal, from model toward the viewer.
    const Vec3 normal{-frame.direction.x, -frame.direction.y, -frame.direction.z};
    const Vec3 horizontal_value = cross(frame.up, normal);
    const double horizontal_length = length(horizontal_value);
    if (!std::isfinite(horizontal_length) || horizontal_length <= tolerance) {
        projection_error("Building view frame has no horizontal axis");
    }
    const Vec3 horizontal_unit{horizontal_value.x / horizontal_length,
                               horizontal_value.y / horizontal_length,
                               horizontal_value.z / horizontal_length};
    return {gp_Pnt(frame.origin.x, frame.origin.y, frame.origin.z),
            gp_Vec(horizontal_unit.x, horizontal_unit.y, horizontal_unit.z),
            gp_Vec(frame.up.x, frame.up.y, frame.up.z),
            gp_Vec(normal.x, normal.y, normal.z)};
}

double chord_length(const Vec2& start, const Vec2& end) {
    return std::hypot(end.x - start.x, end.y - start.y);
}

bool finite_point(Vec2 point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void append_segment(Boundary& result, Vec2 start, Vec2 end, double sweep,
                    std::string_view context) {
    if (!std::isfinite(sweep) || !finite_point(start) || !finite_point(end)) {
        projection_error("Building view projection produced a non-finite " +
                         std::string(context));
    }
    if (chord_length(start, end) <= tolerance) {
        return;
    }
    if (sweep != 0.0 && std::abs(sweep) >= full_turn) {
        projection_error("Building view projection produced an invalid " +
                         std::string(context) + " sweep");
    }
    result.push_back({start, end, sweep});
}

occ::handle<Geom2d_Curve> basis_curve_2d(occ::handle<Geom2d_Curve> curve) {
    while (!curve.IsNull()) {
        const auto trimmed = occ::down_cast<Geom2d_TrimmedCurve>(curve);
        if (trimmed.IsNull()) break;
        curve = trimmed->BasisCurve();
    }
    return curve;
}

occ::handle<Geom_Curve> basis_curve_3d(occ::handle<Geom_Curve> curve) {
    while (!curve.IsNull()) {
        const auto trimmed = occ::down_cast<Geom_TrimmedCurve>(curve);
        if (trimmed.IsNull()) break;
        curve = trimmed->BasisCurve();
    }
    return curve;
}

void append_circle_2d(const TopoDS_Edge& edge, const occ::handle<Geom2d_Curve>& curve,
                      double first, double last, Boundary& result) {
    if (edge.Orientation() == TopAbs_REVERSED) std::swap(first, last);
    const auto circle = occ::down_cast<Geom2d_Circle>(curve);
    if (circle.IsNull() || !std::isfinite(first) || !std::isfinite(last) ||
        !std::isfinite(circle->Radius()) || circle->Radius() <= tolerance) {
        projection_error("Building view projection circle is invalid");
    }
    const double parameter_span = last - first;
    if (!std::isfinite(parameter_span)) {
        projection_error("Building view projection circle sweep is not finite");
    }
    const double orientation = circle->Position().XDirection().Crossed(
        circle->Position().YDirection());
    const double sweep = parameter_span * (orientation > 0.0 ? 1.0 : -1.0);
    const double magnitude = std::abs(sweep);
    if (magnitude <= tolerance || magnitude > full_turn + full_circle_tolerance) {
        projection_error("Building view projection circle sweep is invalid");
    }
    const bool full_circle = std::abs(magnitude - full_turn) <= full_circle_tolerance;
    const auto pieces = static_cast<std::size_t>(std::max(1.0, std::ceil(magnitude / quarter_turn)));
    const auto count = full_circle ? std::size_t{4} : pieces;
    const double parameter_step = parameter_span / static_cast<double>(count);
    const double sweep_step = sweep / static_cast<double>(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double start = first + parameter_step * static_cast<double>(index);
        const double end = first + parameter_step * static_cast<double>(index + 1);
        const auto start_point = circle->Value(start);
        const auto end_point = circle->Value(end);
        append_segment(result, {start_point.X(), start_point.Y()},
                       {end_point.X(), end_point.Y()}, sweep_step, "circle");
    }
}

void append_hlr_edge(const TopoDS_Edge& edge, Boundary& result) {
    double first = 0.0;
    double last = 0.0;
    auto curve = BRep_Tool::CurveOnSurface(edge, BRepLib::Plane(),
                                           TopLoc_Location(), first, last);
    curve = basis_curve_2d(std::move(curve));
    if (curve.IsNull()) projection_error("Building view projection edge has no XY p-curve");
    if (const auto line = occ::down_cast<Geom2d_Line>(curve); !line.IsNull()) {
        if (edge.Orientation() == TopAbs_REVERSED) std::swap(first, last);
        append_segment(result, {line->Value(first).X(), line->Value(first).Y()},
                       {line->Value(last).X(), line->Value(last).Y()}, 0.0, "line");
    } else if (!occ::down_cast<Geom2d_Circle>(curve).IsNull()) {
        append_circle_2d(edge, curve, first, last, result);
    } else {
        projection_error("Building view projection contains an unsupported projected curve");
    }
}

Boundary project_hlr(const TopoDS_Shape& shape, const gp_Ax2& axes) {
    const occ::handle<HLRBRep_Algo> algorithm = new HLRBRep_Algo();
    algorithm->Projector(HLRAlgo_Projector(axes));
    algorithm->Add(shape);
    algorithm->Update();
    algorithm->Hide();

    HLRBRep_HLRToShape extractor(algorithm);
    TopoDS_Compound visible;
    BRep_Builder builder;
    builder.MakeCompound(visible);
    for (const auto& projected : {
             extractor.VCompound(), extractor.OutLineVCompound(),
             extractor.Rg1LineVCompound(), extractor.RgNLineVCompound()}) {
        if (!projected.IsNull()) builder.Add(visible, projected);
    }

    Boundary result;
    for (TopExp_Explorer explorer(visible, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        append_hlr_edge(TopoDS::Edge(explorer.Current()), result);
    }
    std::sort(result.begin(), result.end(), [](const Segment& left, const Segment& right) {
        if (left.start.x != right.start.x) return left.start.x < right.start.x;
        if (left.start.y != right.start.y) return left.start.y < right.start.y;
        if (left.end.x != right.end.x) return left.end.x < right.end.x;
        if (left.end.y != right.end.y) return left.end.y < right.end.y;
        return left.sweep_radians < right.sweep_radians;
    });
    if (result.empty()) projection_error("Building view projection produced no visible edges");
    return result;
}

Vec2 map_point(const gp_Pnt& point, const FrameBasis& frame) {
    const gp_Vec offset(frame.origin, point);
    return {offset.Dot(frame.horizontal), offset.Dot(frame.vertical)};
}

void append_section_circle(const TopoDS_Edge& edge, const occ::handle<Geom_Curve>& curve,
                           double first, double last, const FrameBasis& frame,
                           Boundary& result) {
    if (edge.Orientation() == TopAbs_REVERSED) std::swap(first, last);
    const auto circle = occ::down_cast<Geom_Circle>(curve);
    if (circle.IsNull() || !std::isfinite(first) || !std::isfinite(last) ||
        !std::isfinite(circle->Radius()) || circle->Radius() <= tolerance) {
        projection_error("Building section circle is invalid");
    }
    const double span = last - first;
    const gp_Vec circle_normal(circle->Position().Axis().Direction());
    const double orientation = circle_normal.Dot(frame.normal) >= 0.0 ? 1.0 : -1.0;
    const double sweep = span * orientation;
    const double magnitude = std::abs(sweep);
    if (!std::isfinite(magnitude) || magnitude <= tolerance ||
        magnitude > full_turn + full_circle_tolerance) {
        projection_error("Building section circle sweep is invalid");
    }
    const bool full_circle = std::abs(magnitude - full_turn) <= full_circle_tolerance;
    const auto pieces = static_cast<std::size_t>(std::max(1.0, std::ceil(magnitude / quarter_turn)));
    const auto count = full_circle ? std::size_t{4} : pieces;
    const double parameter_step = span / static_cast<double>(count);
    const double sweep_step = sweep / static_cast<double>(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double start = first + parameter_step * static_cast<double>(index);
        const double end = first + parameter_step * static_cast<double>(index + 1);
        append_segment(result, map_point(circle->Value(start), frame),
                       map_point(circle->Value(end), frame), sweep_step, "section circle");
    }
}

void append_section_edge(const TopoDS_Edge& edge, const FrameBasis& frame,
                         Boundary& result) {
    double first = 0.0;
    double last = 0.0;
    auto curve = BRep_Tool::Curve(edge, first, last);
    curve = basis_curve_3d(std::move(curve));
    if (curve.IsNull()) projection_error("Building section edge has no 3D curve");
    if (const auto line = occ::down_cast<Geom_Line>(curve); !line.IsNull()) {
        if (edge.Orientation() == TopAbs_REVERSED) std::swap(first, last);
        append_segment(result, map_point(line->Value(first), frame),
                       map_point(line->Value(last), frame), 0.0, "section line");
    } else if (!occ::down_cast<Geom_Circle>(curve).IsNull()) {
        append_section_circle(edge, curve, first, last, frame, result);
    } else {
        projection_error("Building section contains an unsupported intersection curve");
    }
}

Boundary project_section(const TopoDS_Shape& shape, const FrameBasis& frame) {
    const gp_Pln plane(frame.origin, gp_Dir(frame.normal));
    BRepAlgoAPI_Section section(shape, plane, true);
    if (!section.IsDone() || section.Shape().IsNull()) {
        projection_error("Building section intersection failed");
    }
    Boundary result;
    for (TopExp_Explorer explorer(section.Shape(), TopAbs_EDGE); explorer.More(); explorer.Next()) {
        append_section_edge(TopoDS::Edge(explorer.Current()), frame, result);
    }
    std::sort(result.begin(), result.end(), [](const Segment& left, const Segment& right) {
        if (left.start.x != right.start.x) return left.start.x < right.start.x;
        if (left.start.y != right.start.y) return left.start.y < right.start.y;
        if (left.end.x != right.end.x) return left.end.x < right.end.x;
        if (left.end.y != right.end.y) return left.end.y < right.end.y;
        return left.sweep_radians < right.sweep_radians;
    });
    if (result.empty()) projection_error("Building section plane does not intersect the solid");
    return result;
}

}  // namespace

Boundary project_shape_view(const TopoDS_Shape& shape, BuildingViewKind kind,
                            const BuildingViewFrame& frame) {
    try {
        const auto frame_basis = basis(frame);
        if (shape.IsNull()) projection_error("Building view received a null solid");
        if (kind == BuildingViewKind::section) return project_section(shape, frame_basis);
        if (kind != BuildingViewKind::plan && kind != BuildingViewKind::elevation) {
            projection_error("Unknown building view kind");
        }
        const gp_Ax2 axes(frame_basis.origin, gp_Dir(frame_basis.normal),
                          gp_Dir(frame_basis.horizontal));
        return project_hlr(shape, axes);
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Building view projection failed: ") +
                                     error.what());
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Building view projection failed: ") +
                                     error.what());
    }
}

Boundary project_building_view(const BuildingObject& object, BuildingViewKind kind,
                               const BuildingViewFrame& frame) {
    return project_shape_view(make_building_shape(object), kind, frame);
}

bool shape_intersects_view_depth(const TopoDS_Shape& shape,
                                 const BuildingViewDepth& depth) {
    const auto finite_vec3 = [](const Vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    if (!finite_vec3(depth.origin) || !finite_vec3(depth.direction)) {
        throw std::invalid_argument("Building view depth contains a non-finite vector");
    }
    const auto direction_length = std::sqrt(
        depth.direction.x * depth.direction.x +
        depth.direction.y * depth.direction.y +
        depth.direction.z * depth.direction.z);
    if (!std::isfinite(direction_length) ||
        std::abs(direction_length - 1.0) > 1e-9) {
        throw std::invalid_argument("Building view depth direction must be unit length");
    }
    if ((!std::isfinite(depth.far_depth_m) && !std::isinf(depth.far_depth_m)) ||
        depth.far_depth_m < 0.0) {
        throw std::invalid_argument("Building view far depth must be nonnegative or infinity");
    }
    if (shape.IsNull()) return false;
    if (std::isinf(depth.far_depth_m)) return true;

    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) return false;
    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    for (const auto x : {xmin, xmax}) {
        for (const auto y : {ymin, ymax}) {
            for (const auto z : {zmin, zmax}) {
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                    throw std::invalid_argument("Building view depth found a non-finite bound");
                }
                const auto dx = x - depth.origin.x;
                const auto dy = y - depth.origin.y;
                const auto dz = z - depth.origin.z;
                const auto value = dx * depth.direction.x + dy * depth.direction.y +
                                   dz * depth.direction.z;
                if (!std::isfinite(value)) {
                    throw std::invalid_argument("Building view depth exceeded numeric range");
                }
                if (value <= depth.far_depth_m + tolerance) return true;
            }
        }
    }
    return false;
}

}  // namespace sketch
