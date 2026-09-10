#include "sketch/building_plan_projection.hpp"

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepLib.hxx>
#include <Geom2d_Circle.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_Plane.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sketch {
namespace {

constexpr double tolerance = default_geometry_tolerance_metres;
constexpr double full_turn = 2.0 * std::numbers::pi;
constexpr double quarter_turn = std::numbers::pi * 0.5;
constexpr double full_circle_tolerance = 1e-7;

[[noreturn]] void projection_error(std::string message) {
    throw std::invalid_argument(std::move(message));
}

Vec2 point_2d(const gp_Pnt2d& point, const char* context) {
    if (!std::isfinite(point.X()) || !std::isfinite(point.Y())) {
        projection_error(std::string("Building plan projection produced a non-finite ")
                         + context);
    }
    return {point.X(), point.Y()};
}

double chord_length(const Vec2& start, const Vec2& end) {
    return std::hypot(end.x - start.x, end.y - start.y);
}

bool finite_point(Vec2 point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void append_segment(Boundary& result, Vec2 start, Vec2 end, double sweep,
                    const char* context) {
    if (!std::isfinite(sweep) || !finite_point(start) || !finite_point(end)) {
        projection_error(std::string("Building plan projection produced a non-finite ")
                         + context);
    }
    if (!(chord_length(start, end) > tolerance)) {
        // HLR can retain an edge whose projection collapses to a point (for
        // example a vertical side).  Such an edge has no drawable plan
        // geometry and is omitted rather than being converted to a fake line.
        return;
    }
    if (sweep != 0.0 && !(std::abs(sweep) < full_turn)) {
        projection_error(std::string("Building plan projection produced an invalid ")
                         + context + " sweep");
    }
    result.push_back({start, end, sweep});
}

occ::handle<Geom2d_Curve> basis_curve(occ::handle<Geom2d_Curve> curve) {
    while (!curve.IsNull()) {
        const auto trimmed = occ::down_cast<Geom2d_TrimmedCurve>(curve);
        if (trimmed.IsNull()) {
            break;
        }
        curve = trimmed->BasisCurve();
    }
    return curve;
}

void append_line(const TopoDS_Edge& edge, const occ::handle<Geom2d_Curve>& curve,
                 double first, double last, Boundary& result) {
    if (edge.Orientation() == TopAbs_REVERSED) {
        std::swap(first, last);
    }
    if (!std::isfinite(first) || !std::isfinite(last)) {
        projection_error("Building plan projection line parameters are not finite");
    }
    const auto line = occ::down_cast<Geom2d_Line>(curve);
    if (line.IsNull()) {
        projection_error("Building plan projection line type changed during conversion");
    }
    append_segment(result, point_2d(line->Value(first), "line endpoint"),
                   point_2d(line->Value(last), "line endpoint"), 0.0, "line");
}

void append_circle(const TopoDS_Edge& edge, const occ::handle<Geom2d_Curve>& curve,
                   double first, double last, Boundary& result) {
    if (edge.Orientation() == TopAbs_REVERSED) {
        std::swap(first, last);
    }
    if (!std::isfinite(first) || !std::isfinite(last)) {
        projection_error("Building plan projection circle parameters are not finite");
    }
    const auto circle = occ::down_cast<Geom2d_Circle>(curve);
    if (circle.IsNull()) {
        projection_error("Building plan projection circle type changed during conversion");
    }

    const double radius = circle->Radius();
    const double orientation = circle->Position().XDirection().Crossed(
        circle->Position().YDirection());
    if (!std::isfinite(radius) || radius <= tolerance || !std::isfinite(orientation)
        || std::abs(orientation) <= tolerance) {
        projection_error("Building plan projection circle is invalid");
    }

    const double parameter_span = last - first;
    const double sweep = parameter_span * (orientation > 0.0 ? 1.0 : -1.0);
    if (!std::isfinite(parameter_span) || !std::isfinite(sweep)) {
        projection_error("Building plan projection circle sweep is not finite");
    }
    const double sweep_magnitude = std::abs(sweep);
    if (!(sweep_magnitude > tolerance)) {
        return;
    }
    if (sweep_magnitude > full_turn + full_circle_tolerance) {
        projection_error("Building plan projection circle sweep exceeds one turn");
    }

    // Boundary deliberately disallows a full-turn Segment because its chord
    // is zero.  Splitting into quarter turns preserves the exact circle and
    // keeps arc reconstruction numerically well-conditioned.
    const bool is_full_circle = std::abs(sweep_magnitude - full_turn)
        <= full_circle_tolerance;
    const auto pieces = static_cast<std::size_t>(std::max(
        1.0, std::ceil(sweep_magnitude / quarter_turn)));
    const auto count = is_full_circle ? std::size_t{4} : pieces;
    const double parameter_step = parameter_span / static_cast<double>(count);
    const double sweep_step = sweep / static_cast<double>(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double parameter_start = first + parameter_step * static_cast<double>(index);
        const double parameter_end = first
            + parameter_step * static_cast<double>(index + 1);
        append_segment(result, point_2d(circle->Value(parameter_start), "circle endpoint"),
                       point_2d(circle->Value(parameter_end), "circle endpoint"),
                       sweep_step, "circle");
    }
}

void append_projected_edge(const TopoDS_Edge& edge, Boundary& result) {
    double first = 0.0;
    double last = 0.0;
    // HLRBRep_HLRToShape creates 2D edges on BRepLib's canonical XY plane.
    // Reading the p-curve is required: BRep_Tool::Curve() intentionally only
    // returns a 3D curve and is null for these exact projected edges.
    auto curve = BRep_Tool::CurveOnSurface(edge, BRepLib::Plane(),
                                           TopLoc_Location(), first, last);
    curve = basis_curve(std::move(curve));
    if (curve.IsNull()) {
        projection_error("Building plan projection edge has no XY p-curve");
    }

    if (!occ::down_cast<Geom2d_Line>(curve).IsNull()) {
        append_line(edge, curve, first, last, result);
    } else if (!occ::down_cast<Geom2d_Circle>(curve).IsNull()) {
        append_circle(edge, curve, first, last, result);
    } else {
        // No approximation is permitted here.  A spline/conic from an
        // unsupported future object must fail the derived view explicitly.
        projection_error("Building plan projection contains an unsupported curve type");
    }
}

void append_shape_edges(const TopoDS_Shape& shape, Boundary& result) {
    if (shape.IsNull()) {
        return;
    }
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        append_projected_edge(TopoDS::Edge(explorer.Current()), result);
    }
}

void validate_result(const Boundary& result) {
    if (result.empty()) {
        projection_error("Building plan projection produced no visible edges");
    }
    for (const auto& segment : result) {
        if (!finite_point(segment.start) || !finite_point(segment.end)
            || !std::isfinite(segment.sweep_radians)
            || !(chord_length(segment.start, segment.end) > tolerance)
            || (segment.sweep_radians != 0.0
                && !(std::abs(segment.sweep_radians) < full_turn))) {
            projection_error("Building plan projection contains invalid derived geometry");
        }
    }
}

}  // namespace

Boundary project_building_plan(const BuildingObject& object) {
    try {
        const auto shape = make_building_shape(object);
        if (shape.IsNull()) {
            projection_error("Building object produced a null solid");
        }

        const occ::handle<HLRBRep_Algo> algorithm = new HLRBRep_Algo();
        // The identity-oriented Ax2 is the OCCT top projector: HLRAlgo's
        // special top case maps world X/Y directly into the result plane.
        algorithm->Projector(HLRAlgo_Projector(
            gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0),
                   gp_Dir(1.0, 0.0, 0.0))));
        algorithm->Add(shape);
        algorithm->Update();
        algorithm->Hide();

        HLRBRep_HLRToShape extractor(algorithm);
        TopoDS_Compound visible;
        BRep_Builder builder;
        builder.MakeCompound(visible);
        // The sharp, outline, smooth, and sewn visible groups together form
        // the complete visible edge set.  Hidden groups and isoparameters are
        // intentionally excluded from this first plan view style.
        for (const auto& projected : {
                 extractor.VCompound(), extractor.OutLineVCompound(),
                 extractor.Rg1LineVCompound(), extractor.RgNLineVCompound()}) {
            if (!projected.IsNull()) {
                builder.Add(visible, projected);
            }
        }

        Boundary result;
        append_shape_edges(visible, result);
        std::sort(result.begin(), result.end(), [](const Segment& left,
                                                   const Segment& right) {
            if (left.start.x != right.start.x) return left.start.x < right.start.x;
            if (left.start.y != right.start.y) return left.start.y < right.start.y;
            if (left.end.x != right.end.x) return left.end.x < right.end.x;
            if (left.end.y != right.end.y) return left.end.y < right.end.y;
            return left.sweep_radians < right.sweep_radians;
        });
        validate_result(result);
        return result;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Building plan projection failed: ")
                                     + error.what());
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Building plan projection failed: ")
                                     + error.what());
    }
}

}  // namespace sketch
