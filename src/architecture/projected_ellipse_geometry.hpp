#pragma once

#include "sketch/geometry.hpp"

#include <HLRAlgo_EdgeIterator.hxx>
#include <HLRBRep_Data.hxx>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace sketch::projected_curve_detail {

// Presentation/output only: Boundary has no elliptical primitive. Keep native
// lines/circles exact; these chords never replace authored geometry or supply
// lengths, areas or quantities. The maximum physical chord deviation is 0.1 mm.
// Half is reserved for numerical arithmetic within the admitted coordinate range.
inline constexpr double maximum_deviation_metres = 1e-4;
inline constexpr double geometric_deviation_metres = maximum_deviation_metres * 0.5;
inline constexpr double maximum_coordinate_metres = 1e6;
inline constexpr std::size_t maximum_source_edges = 16384;
inline constexpr std::size_t maximum_segments = 262144;
inline constexpr std::size_t maximum_points = 1048576;
inline constexpr std::size_t maximum_curve_segments = 4096;
inline constexpr int maximum_spline_poles = 1024;
inline constexpr int maximum_spline_knots = 1024;
inline constexpr int maximum_degree = 16;
inline constexpr unsigned maximum_subdivision_depth = 32;

[[noreturn]] inline void refuse(const char* message) {
    throw std::invalid_argument(message);
}

struct Budget {
    std::size_t edges{};
    std::size_t points{};
    std::size_t segments{};

    void edge() {
        if (edges >= maximum_source_edges) refuse("Derived view exceeds its source-edge budget");
        ++edges;
    }
    void point(std::size_t count = 1) {
        if (count > maximum_points - points) refuse("Derived view exceeds its point/work budget");
        points += count;
    }
    void segment() {
        if (segments >= maximum_segments) refuse("Derived view exceeds its total segment budget");
        ++segments;
    }
};

template<class Algorithm>
void require_bounded_hlr_curves(const Algorithm& algorithm) {
    const auto data = algorithm->DataStructure();
    if (data.IsNull()) refuse("Derived view has no native projection data");
    if (data->NbEdges() < 0 || static_cast<std::size_t>(data->NbEdges()) > maximum_source_edges)
        refuse("Derived view exceeds its native edge budget");
    for (int index = 1; index <= data->NbEdges(); ++index) {
        auto& edge = data->EDataArray().ChangeValue(index);
        if (!edge.Selected() || edge.Vertical()) continue;
        HLRAlgo_EdgeIterator visible;
        visible.InitVisible(edge.Status());
        if (!visible.MoreVisible()) continue;
        switch (edge.Geometry().GetType()) {
        case GeomAbs_Line:
        case GeomAbs_Circle:
        case GeomAbs_Ellipse:
            break;
        case GeomAbs_BSplineCurve:
            // Bound allocations made by HLRToShape before it hands us a
            // p-curve; decomposition preflight below repeats the same contract.
            if (edge.Geometry().Degree() > maximum_degree ||
                edge.Geometry().NbPoles() > maximum_spline_poles ||
                edge.Geometry().NbKnots() > maximum_spline_knots)
                refuse("Derived native spline exceeds its complexity budget");
            break;
        default:
            // OCCT 8.0.1 HLRBRep::MakeEdge's OtherCurve fallback produces a
            // 15-point linear spline without a physical deflection bound.
            // Reject before extraction; its spline type cannot prove fidelity.
            refuse("Derived view contains an unsupported native projection curve");
        }
    }
}

inline Vec2 checked_point(Vec2 point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        std::abs(point.x) > maximum_coordinate_metres ||
        std::abs(point.y) > maximum_coordinate_metres) {
        refuse("Derived curve coordinates exceed the finite one-million-metre range");
    }
    return point;
}

inline void chord(Boundary& result, Vec2 start, Vec2 end, Budget& budget,
                  std::size_t& curve_segments) {
    checked_point(start);
    checked_point(end);
    if (std::hypot(end.x - start.x, end.y - start.y) <= default_geometry_tolerance_metres)
        refuse("Derived curve chord collapses below geometry tolerance");
    if (curve_segments >= maximum_curve_segments)
        refuse("Derived curve exceeds its per-edge segment budget");
    budget.segment();
    ++curve_segments;
    result.push_back({start, end, 0.0});
}

template<class Ellipse, class MapPoint>
void append_ellipse(const Ellipse& ellipse, double first, double last,
                    MapPoint map_point, Boundary& result, Budget& budget) {
    constexpr double full_turn = 2.0 * std::numbers::pi;
    const double major = ellipse->MajorRadius();
    const double minor = ellipse->MinorRadius();
    const double span = last - first;
    if (!std::isfinite(first) || !std::isfinite(last) ||
        std::abs(first) > 64 * std::numbers::pi ||
        std::abs(last) > 64 * std::numbers::pi ||
        !std::isfinite(span) || !(std::abs(span) > 0.0) ||
        std::abs(span) > full_turn + 1e-7 ||
        !std::isfinite(major) || !std::isfinite(minor) ||
        minor <= default_geometry_tolerance_metres || major < minor ||
        major > maximum_coordinate_metres) {
        refuse("Derived ellipse has an unsupported radius or parameter range");
    }
    const auto center = checked_point(map_point(ellipse->Location()));
    // Orthogonal projection cannot enlarge either radius. This deliberately
    // conservative full-ellipse bound also covers trimmed interior extrema.
    if (std::abs(center.x) + major > maximum_coordinate_metres ||
        std::abs(center.y) + major > maximum_coordinate_metres)
        refuse("Derived ellipse exceeds the supported coordinate range");

    // |E''(u)| <= major, also after orthogonal section/view projection.
    // Linear interpolation error <= major * h^2 / 8 throughout each interval.
    const double maximum_step = std::min(std::numbers::pi * 0.5,
        std::sqrt(8.0 * geometric_deviation_metres / major));
    const double pieces = std::ceil(std::abs(span) / maximum_step);
    if (!std::isfinite(pieces) || pieces > maximum_curve_segments)
        refuse("Derived ellipse cannot meet the physical deviation within its edge budget");
    const auto count = static_cast<std::size_t>(std::max(1.0, pieces));
    budget.point(count + 1);
    auto previous = checked_point(map_point(ellipse->Value(first)));
    std::size_t curve_segments = 0;
    for (std::size_t index = 1; index <= count; ++index) {
        // Retain exact edge endpoints, including reversed and trimmed edges.
        const double parameter = index == count ? last
            : first + span * (static_cast<double>(index) / static_cast<double>(count));
        const auto next = checked_point(map_point(ellipse->Value(parameter)));
        chord(result, previous, next, budget, curve_segments);
        previous = next;
    }
}

struct WeightedPoint {
    double x;
    double y;
    double weight;
};

inline Vec2 unweight(const WeightedPoint& point) {
    if (!std::isfinite(point.weight) || point.weight < 1e-12)
        refuse("Derived spline weights exceed the supported conditioning range");
    return checked_point({point.x / point.weight, point.y / point.weight});
}

inline double distance_to_chord(Vec2 point, Vec2 start, Vec2 end) {
    const double dx = end.x - start.x, dy = end.y - start.y;
    const double squared = dx * dx + dy * dy;
    const double fraction = squared == 0.0 ? 0.0 : std::clamp(
        ((point.x - start.x) * dx + (point.y - start.y) * dy) / squared, 0.0, 1.0);
    return std::hypot(point.x - (start.x + fraction * dx),
                      point.y - (start.y + fraction * dy));
}

inline void append_bezier_hull(const std::vector<WeightedPoint>& poles,
                               unsigned depth, Boundary& result, Budget& budget,
                               std::size_t& curve_segments) {
    budget.point(poles.size());
    const auto start = unweight(poles.front());
    const auto end = unweight(poles.back());
    bool within_bound = true;
    for (const auto& pole : poles) {
        if (distance_to_chord(unweight(pole), start, end) > geometric_deviation_metres)
            within_bound = false;
    }
    // Positive rational Bernstein weights put the entire curve inside this
    // control hull. Distance to the finite chord is convex, so this bounds all
    // interior points, unlike a midpoint-only deflection test.
    if (within_bound) {
        chord(result, start, end, budget, curve_segments);
        return;
    }
    if (depth >= maximum_subdivision_depth)
        refuse("Derived spline cannot meet its deviation within the subdivision budget");
    auto work = poles;
    std::vector<WeightedPoint> left(poles.size()), right(poles.size());
    left.front() = work.front();
    right.back() = work.back();
    for (std::size_t level = 1; level < poles.size(); ++level) {
        // Charge each homogeneous interpolation as work, before doing it.
        budget.point(poles.size() - level);
        for (std::size_t index = 0; index < poles.size() - level; ++index) {
            const auto& a = work[index];
            const auto& b = work[index + 1];
            work[index] = {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5,
                           (a.weight + b.weight) * 0.5};
        }
        left[level] = work.front();
        right[poles.size() - level - 1] = work[poles.size() - level - 1];
    }
    append_bezier_hull(left, depth + 1, result, budget, curve_segments);
    append_bezier_hull(right, depth + 1, result, budget, curve_segments);
}

template<class Converter, class Spline, class MapPoint>
void append_bspline(const Spline& spline, double first, double last,
                    MapPoint map_point, Boundary& result, Budget& budget) {
    const double low = std::min(first, last), high = std::max(first, last);
    if (!std::isfinite(low) || !std::isfinite(high) || !(high > low) ||
        !std::isfinite(high - low) || std::abs(low) > 1e6 || std::abs(high) > 1e6 ||
        spline->Degree() < 1 || spline->Degree() > maximum_degree ||
        spline->NbPoles() < 2 || spline->NbPoles() > maximum_spline_poles ||
        spline->NbKnots() < 2 || spline->NbKnots() > maximum_spline_knots ||
        !std::isfinite(spline->FirstParameter()) || !std::isfinite(spline->LastParameter()) ||
        low < spline->FirstParameter() || high > spline->LastParameter()) {
        refuse("Derived spline exceeds the supported trim or complexity range");
    }
    // Preflight before OCCT copies/inserts knots. Degree, input poles and knots
    // bound the native decomposition's allocation; no unbounded discretizer is
    // called and arbitrary spline oscillations are handled by their exact hulls.
    budget.point(static_cast<std::size_t>(spline->NbPoles()));
    double maximum_weight = 0.0;
    for (int index = 1; index <= spline->NbPoles(); ++index) {
        checked_point(map_point(spline->Pole(index)));
        const auto weight = spline->Weight(index);
        if (!std::isfinite(weight) || weight < 1e-12 || weight > 1e12)
            refuse("Derived spline rational weights exceed the supported numeric range");
        maximum_weight = std::max(maximum_weight, weight);
    }
    for (int index = 1; index <= spline->NbPoles(); ++index) {
        if (spline->Weight(index) / maximum_weight < 1e-12)
            refuse("Derived spline weights exceed the supported conditioning range");
    }
    for (int index = 1; index <= spline->NbKnots(); ++index) {
        if (!std::isfinite(spline->Knot(index)) || std::abs(spline->Knot(index)) > 1e6)
            refuse("Derived spline knots exceed the supported numeric range");
        if (index > 1 && !(spline->Knot(index) > spline->Knot(index - 1)))
            refuse("Derived spline knots are not strictly increasing");
    }
    // Zero parameter tolerance prevents a short trim from snapping to a knot.
    // OCCT's conversion works on a copy, preserving native source geometry.
    Converter converter(spline, low, high, 0.0);
    const auto arcs = converter.NbArcs();
    if (arcs < 1 || arcs > maximum_spline_knots)
        refuse("Derived spline decomposition exceeds its span budget");
    const bool reversed = first > last;
    std::size_t curve_segments = 0;
    const auto initial_size = result.size();
    for (int arc_index = 1; arc_index <= arcs; ++arc_index) {
        const auto arc = converter.Arc(reversed ? arcs - arc_index + 1 : arc_index);
        const auto count = arc->NbPoles();
        if (count < 2 || count > maximum_degree + 1)
            refuse("Derived spline decomposition has an unsupported degree");
        budget.point(static_cast<std::size_t>(count));
        double scale = 0.0;
        for (int index = 1; index <= count; ++index) {
            const double weight = arc->Weight(index);
            if (!std::isfinite(weight) || weight <= 0.0)
                refuse("Derived Bezier span has an invalid weight");
            scale = std::max(scale, weight);
        }
        std::vector<WeightedPoint> poles;
        poles.reserve(static_cast<std::size_t>(count));
        for (int index = 1; index <= count; ++index) {
            const int source_index = reversed ? count - index + 1 : index;
            const auto point = checked_point(map_point(arc->Pole(source_index)));
            const double weight = arc->Weight(source_index) / scale;
            if (weight < 1e-12)
                refuse("Derived Bezier span exceeds the supported conditioning range");
            poles.push_back({point.x * weight, point.y * weight, weight});
        }
        append_bezier_hull(poles, 0, result, budget, curve_segments);
    }
    // Refuse a native trim conversion that moved an endpoint materially. Chord
    // subdivision already retains each span's endpoint and signed traversal.
    budget.point(2);
    const auto start = checked_point(map_point(spline->Value(first)));
    const auto end = checked_point(map_point(spline->Value(last)));
    if (distance_to_chord(start, result[initial_size].start, result[initial_size].start)
            > default_geometry_tolerance_metres ||
        distance_to_chord(end, result.back().end, result.back().end)
            > default_geometry_tolerance_metres) {
        refuse("Derived spline decomposition changed a trimmed endpoint");
    }
    result[initial_size].start = start;
    result.back().end = end;
    if (std::hypot(result[initial_size].end.x - start.x, result[initial_size].end.y - start.y)
            <= default_geometry_tolerance_metres ||
        std::hypot(end.x - result.back().start.x, end.y - result.back().start.y)
            <= default_geometry_tolerance_metres)
        refuse("Derived spline endpoint retention collapses a chord");
}

} // namespace sketch::projected_curve_detail
