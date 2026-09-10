#include "sketch/constraints.hpp"

#include "GCS.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sketch {
namespace {

struct MutablePoint {
    double x{};
    double y{};
};

const ConstraintId& constraint_id(const PlanarConstraint& constraint) {
    return std::visit([](const auto& item) -> const ConstraintId& { return item.id; }, constraint);
}

void add_diagnostic(ConstraintPreview& preview, ConstraintDiagnosticKind kind,
                    std::string message, std::optional<ConstraintId> id = std::nullopt) {
    preview.diagnostics.push_back({kind, std::move(message), std::move(id)});
}

ConstraintPreview rejected_preview(const ConstraintSolveRequest& request,
                                   ConstraintSolveStatus status,
                                   ConstraintDiagnosticKind kind,
                                   std::string message,
                                   std::optional<ConstraintId> id = std::nullopt) {
    ConstraintPreview preview;
    preview.status = status;
    preview.expected_revision = request.expected_revision;
    preview.points = request.points;
    add_diagnostic(preview, kind, std::move(message), std::move(id));
    return preview;
}

bool finite_difference(double first, double second) {
    const long double difference = static_cast<long double>(first) - static_cast<long double>(second);
    return std::isfinite(difference)
        && std::abs(difference) <= static_cast<long double>(std::numeric_limits<double>::max());
}

bool finite_separation(const ConstraintPoint& first, const ConstraintPoint& second) {
    if (!finite_difference(first.x, second.x) || !finite_difference(first.y, second.y)) {
        return false;
    }
    return std::isfinite(std::hypot(first.x - second.x, first.y - second.y));
}

double stable_distance(const ConstraintPoint& first, const ConstraintPoint& second) {
    const long double dx = static_cast<long double>(first.x) - static_cast<long double>(second.x);
    const long double dy = static_cast<long double>(first.y) - static_cast<long double>(second.y);
    const long double value = std::hypot(dx, dy);
    if (!std::isfinite(value)
        || value > static_cast<long double>(std::numeric_limits<double>::max())) {
        return std::numeric_limits<double>::infinity();
    }
    return static_cast<double>(value);
}

double stable_distance(const MutablePoint& first, const MutablePoint& second) {
    const long double dx = static_cast<long double>(first.x) - static_cast<long double>(second.x);
    const long double dy = static_cast<long double>(first.y) - static_cast<long double>(second.y);
    const long double value = std::hypot(dx, dy);
    if (!std::isfinite(value)
        || value > static_cast<long double>(std::numeric_limits<double>::max())) {
        return std::numeric_limits<double>::infinity();
    }
    return static_cast<double>(value);
}

template<typename PointLookup>
long double signed_twice_area(const WindingInvariant& invariant, PointLookup&& lookup) {
    const auto& origin = lookup(invariant.loop.front());
    const long double ox = static_cast<long double>(origin.x);
    const long double oy = static_cast<long double>(origin.y);
    long double sum = 0.0L;
    long double compensation = 0.0L;
    for (std::size_t index = 0; index < invariant.loop.size(); ++index) {
        const auto& first = lookup(invariant.loop[index]);
        const auto& second = lookup(invariant.loop[(index + 1) % invariant.loop.size()]);
        const long double x1 = static_cast<long double>(first.x) - ox;
        const long double y1 = static_cast<long double>(first.y) - oy;
        const long double x2 = static_cast<long double>(second.x) - ox;
        const long double y2 = static_cast<long double>(second.y) - oy;
        const long double term = x1 * y2 - x2 * y1;
        const long double adjusted = term - compensation;
        const long double next = sum + adjusted;
        compensation = (next - sum) - adjusted;
        sum = next;
    }
    return sum;
}

int orientation_sign(long double signed_area) {
    return signed_area > 0.0L ? 1 : (signed_area < 0.0L ? -1 : 0);
}

template<typename T>
bool validate_segment_ids(const T& constraint,
                          const std::map<ConstraintPointId, std::size_t>& point_indices,
                          const std::vector<ConstraintPoint>& points,
                          std::string& message) {
    const auto first_start = point_indices.find(constraint.first_start);
    const auto first_end = point_indices.find(constraint.first_end);
    const auto second_start = point_indices.find(constraint.second_start);
    const auto second_end = point_indices.find(constraint.second_end);
    if (first_start == point_indices.end() || first_end == point_indices.end()
        || second_start == point_indices.end() || second_end == point_indices.end()) {
        message = "constraint references a missing point id";
        return false;
    }
    if (constraint.first_start == constraint.first_end
        || constraint.second_start == constraint.second_end) {
        message = "direction constraints require distinct segment endpoint ids";
        return false;
    }
    const auto& a = points[first_start->second];
    const auto& b = points[first_end->second];
    const auto& c = points[second_start->second];
    const auto& d = points[second_end->second];
    if (!finite_separation(a, b) || !finite_separation(c, d)
        || stable_distance(a, b) == 0.0 || stable_distance(c, d) == 0.0) {
        message = "direction constraints require finite non-degenerate segments";
        return false;
    }
    return true;
}

bool validate_request(const ConstraintSolveRequest& request,
                      std::map<ConstraintPointId, std::size_t>& point_indices,
                      std::string& message,
                      std::optional<ConstraintId>& bad_constraint) {
    std::set<ConstraintPointId> point_ids;
    for (std::size_t index = 0; index < request.points.size(); ++index) {
        const auto& point = request.points[index];
        if (point.id.empty() || !point_ids.insert(point.id).second) {
            message = "point ids must be non-empty and unique";
            return false;
        }
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            message = "point coordinates must be finite";
            return false;
        }
        point_indices.emplace(point.id, index);
    }
    if (request.points.empty()) {
        message = "at least one point is required";
        return false;
    }
    if (request.constraints.size()
        > static_cast<std::size_t>(std::numeric_limits<int>::max() - 1)) {
        message = "constraint count exceeds the solver tag range";
        return false;
    }

    std::set<ConstraintId> constraint_ids;
    for (const auto& constraint : request.constraints) {
        const auto& id = constraint_id(constraint);
        bad_constraint = id;
        if (id.empty() || !constraint_ids.insert(id).second) {
            message = "constraint ids must be non-empty and unique";
            return false;
        }

        const bool valid = std::visit(
            [&](const auto& item) {
                using Item = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<Item, HorizontalConstraint>
                              || std::is_same_v<Item, VerticalConstraint>
                              || std::is_same_v<Item, CoincidentConstraint>
                              || std::is_same_v<Item, FixedLengthConstraint>) {
                    const auto first = point_indices.find(item.first);
                    const auto second = point_indices.find(item.second);
                    if (first == point_indices.end() || second == point_indices.end()) {
                        message = "constraint references a missing point id";
                        return false;
                    }
                    if (item.first == item.second) {
                        message = "constraint endpoints must use distinct point ids";
                        return false;
                    }
                    const auto& first_point = request.points[first->second];
                    const auto& second_point = request.points[second->second];
                    if (!finite_separation(first_point, second_point)) {
                        message = "constraint coordinate separation is not representable";
                        return false;
                    }
                    if constexpr (std::is_same_v<Item, FixedLengthConstraint>) {
                        if (!std::isfinite(item.length_metres) || item.length_metres <= 0.0) {
                            message = "fixed length must be finite and positive";
                            return false;
                        }
                        if (stable_distance(first_point, second_point) == 0.0) {
                            message = "fixed length requires a non-degenerate initial segment";
                            return false;
                        }
                    }
                    return true;
                }
                else if constexpr (std::is_same_v<Item, ParallelConstraint>
                                   || std::is_same_v<Item, PerpendicularConstraint>) {
                    return validate_segment_ids(item, point_indices, request.points, message);
                }
                else {
                    const auto found = point_indices.find(item.point);
                    if (found == point_indices.end()) {
                        message = "anchor references a missing point id";
                        return false;
                    }
                    if (!std::isfinite(item.x) || !std::isfinite(item.y)
                        || !finite_difference(request.points[found->second].x, item.x)
                        || !finite_difference(request.points[found->second].y, item.y)) {
                        message = "anchor coordinates and displacement must be finite";
                        return false;
                    }
                    return true;
                }
            },
            constraint);
        if (!valid) {
            return false;
        }
    }
    bad_constraint.reset();

    for (const auto& invariant : request.winding_invariants) {
        if (invariant.loop.size() < 3) {
            message = "winding invariants require at least three points";
            return false;
        }
        std::set<ConstraintPointId> loop_ids;
        for (const auto& id : invariant.loop) {
            if (point_indices.find(id) == point_indices.end()) {
                message = "winding invariant references a missing point id";
                return false;
            }
            if (!loop_ids.insert(id).second) {
                message = "winding invariant point ids must be unique";
                return false;
            }
        }
        const auto input_lookup = [&](const ConstraintPointId& id) -> const ConstraintPoint& {
            return request.points[point_indices.at(id)];
        };
        const long double input_area = signed_twice_area(invariant, input_lookup);
        if (!std::isfinite(input_area)) {
            message = "winding invariant area is not representable";
            return false;
        }
        const int actual = orientation_sign(input_area);
        const int required = static_cast<int>(invariant.orientation);
        if (actual == 0 || actual != required) {
            message = "input winding is degenerate or differs from its declared orientation";
            return false;
        }
    }
    return true;
}

std::vector<ConstraintId> ids_for_tags(const GCS::VEC_I& tags,
                                       const std::vector<ConstraintId>& tag_ids) {
    std::vector<ConstraintId> ids;
    for (const int tag : tags) {
        if (tag > 0 && static_cast<std::size_t>(tag) < tag_ids.size()) {
            ids.push_back(tag_ids[static_cast<std::size_t>(tag)]);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

struct DirectionResidual {
    double value{};
    bool finite{true};
};

DirectionResidual parallel_residual(const MutablePoint& a, const MutablePoint& b,
                                    const MutablePoint& c, const MutablePoint& d,
                                    bool perpendicular) {
    const long double ux = static_cast<long double>(b.x) - static_cast<long double>(a.x);
    const long double uy = static_cast<long double>(b.y) - static_cast<long double>(a.y);
    const long double vx = static_cast<long double>(d.x) - static_cast<long double>(c.x);
    const long double vy = static_cast<long double>(d.y) - static_cast<long double>(c.y);
    const long double u_length = std::hypot(ux, uy);
    const long double v_length = std::hypot(vx, vy);
    if (!(u_length > 0.0L) || !(v_length > 0.0L) || !std::isfinite(u_length)
        || !std::isfinite(v_length)) {
        return {std::numeric_limits<double>::infinity(), false};
    }
    const long double unx = ux / u_length;
    const long double uny = uy / u_length;
    const long double vnx = vx / v_length;
    const long double vny = vy / v_length;
    const long double normalized = perpendicular ? unx * vnx + uny * vny
                                                  : unx * vny - uny * vnx;
    const long double clamped = std::clamp(normalized, -1.0L, 1.0L);
    const double angle = std::abs(std::asin(static_cast<double>(clamped)));
    return {angle, std::isfinite(angle)};
}

}  // namespace

ConstraintPreview solve_planar_constraints(const ConstraintSolveRequest& request) {
    std::map<ConstraintPointId, std::size_t> point_indices;
    std::string validation_message;
    std::optional<ConstraintId> bad_constraint;
    if (!validate_request(request, point_indices, validation_message, bad_constraint)) {
        return rejected_preview(request, ConstraintSolveStatus::rejected_invalid_input,
                                ConstraintDiagnosticKind::invalid_input,
                                std::move(validation_message), std::move(bad_constraint));
    }

    ConstraintPreview preview;
    preview.status = ConstraintSolveStatus::rejected_solver_failure;
    preview.expected_revision = request.expected_revision;
    preview.points = request.points;

    try {
        std::vector<MutablePoint> solved_points;
        solved_points.reserve(request.points.size());
        for (const auto& point : request.points) {
            solved_points.push_back({point.x, point.y});
        }

        std::vector<GCS::Point> solver_points;
        solver_points.reserve(solved_points.size());
        GCS::VEC_pD parameters;
        parameters.reserve(solved_points.size() * 2);
        for (auto& point : solved_points) {
            solver_points.emplace_back(&point.x, &point.y);
            parameters.push_back(&point.x);
            parameters.push_back(&point.y);
        }

        GCS::System system;
        system.autoChooseAlgorithm = true;
        system.dogLegGaussStep = GCS::LeastNormFullPivLU;
        std::deque<double> target_values;
        std::vector<ConstraintId> tag_ids(1);
        tag_ids.reserve(request.constraints.size() + 1);

        for (const auto& constraint : request.constraints) {
            const int tag = static_cast<int>(tag_ids.size());
            tag_ids.push_back(constraint_id(constraint));
            std::visit(
                [&](const auto& item) {
                    using Item = std::decay_t<decltype(item)>;
                    const auto solver_point = [&](const ConstraintPointId& id) -> GCS::Point& {
                        return solver_points[point_indices.at(id)];
                    };
                    if constexpr (std::is_same_v<Item, HorizontalConstraint>) {
                        system.addConstraintHorizontal(solver_point(item.first),
                                                       solver_point(item.second), tag);
                    }
                    else if constexpr (std::is_same_v<Item, VerticalConstraint>) {
                        system.addConstraintVertical(solver_point(item.first),
                                                     solver_point(item.second), tag);
                    }
                    else if constexpr (std::is_same_v<Item, CoincidentConstraint>) {
                        system.addConstraintP2PCoincident(solver_point(item.first),
                                                          solver_point(item.second), tag);
                    }
                    else if constexpr (std::is_same_v<Item, FixedLengthConstraint>) {
                        target_values.push_back(item.length_metres);
                        system.addConstraintP2PDistance(solver_point(item.first),
                                                        solver_point(item.second),
                                                        &target_values.back(), tag);
                    }
                    else if constexpr (std::is_same_v<Item, ParallelConstraint>) {
                        GCS::Line first_line;
                        first_line.p1 = solver_point(item.first_start);
                        first_line.p2 = solver_point(item.first_end);
                        GCS::Line second_line;
                        second_line.p1 = solver_point(item.second_start);
                        second_line.p2 = solver_point(item.second_end);
                        system.addConstraintParallel(first_line, second_line, tag);
                    }
                    else if constexpr (std::is_same_v<Item, PerpendicularConstraint>) {
                        system.addConstraintPerpendicular(solver_point(item.first_start),
                                                          solver_point(item.first_end),
                                                          solver_point(item.second_start),
                                                          solver_point(item.second_end), tag);
                    }
                    else {
                        target_values.push_back(item.x);
                        double* anchor_x = &target_values.back();
                        target_values.push_back(item.y);
                        double* anchor_y = &target_values.back();
                        system.addConstraintCoordinateX(solver_point(item.point), anchor_x, tag);
                        system.addConstraintCoordinateY(solver_point(item.point), anchor_y, tag);
                    }
                },
                constraint);
        }

        system.declareUnknowns(parameters);
        preview.degrees_of_freedom = system.diagnose(GCS::DogLeg);
        GCS::VEC_I conflicting_tags;
        GCS::VEC_I redundant_tags;
        system.getConflicting(conflicting_tags);
        system.getRedundant(redundant_tags);
        preview.conflicting_constraints = ids_for_tags(conflicting_tags, tag_ids);
        preview.redundant_constraints = ids_for_tags(redundant_tags, tag_ids);

        for (const auto& id : preview.redundant_constraints) {
            add_diagnostic(preview, ConstraintDiagnosticKind::redundant,
                           "solver diagnosed a redundant constraint", id);
        }
        if (!preview.conflicting_constraints.empty()) {
            preview.status = ConstraintSolveStatus::rejected_conflict;
            for (const auto& id : preview.conflicting_constraints) {
                add_diagnostic(preview, ConstraintDiagnosticKind::conflicting,
                               "solver diagnosed a conflicting constraint", id);
            }
            return preview;
        }

        system.initSolution(GCS::DogLeg);
        const int solver_status = system.solve(true, GCS::DogLeg, false);
        if (solver_status != GCS::Success && solver_status != GCS::Converged) {
            preview.status = ConstraintSolveStatus::rejected_solver_failure;
            add_diagnostic(preview, ConstraintDiagnosticKind::solver_failure,
                           "PlaneGCS did not produce a candidate solution");
            return preview;
        }
        system.applySolution();
        for (const auto& point : solved_points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                preview.status = ConstraintSolveStatus::rejected_solver_failure;
                add_diagnostic(preview, ConstraintDiagnosticKind::solver_failure,
                               "PlaneGCS produced a non-finite coordinate");
                return preview;
            }
        }

        bool anchor_failed = false;
        bool residual_failed = false;
        for (const auto& constraint : request.constraints) {
            const auto& id = constraint_id(constraint);
            std::visit(
                [&](const auto& item) {
                    using Item = std::decay_t<decltype(item)>;
                    const auto solved = [&](const ConstraintPointId& point_id) -> const MutablePoint& {
                        return solved_points[point_indices.at(point_id)];
                    };
                    double linear_residual = 0.0;
                    double angular_residual = 0.0;
                    if constexpr (std::is_same_v<Item, HorizontalConstraint>) {
                        linear_residual = std::abs(solved(item.first).y - solved(item.second).y);
                    }
                    else if constexpr (std::is_same_v<Item, VerticalConstraint>) {
                        linear_residual = std::abs(solved(item.first).x - solved(item.second).x);
                    }
                    else if constexpr (std::is_same_v<Item, CoincidentConstraint>) {
                        linear_residual = stable_distance(solved(item.first), solved(item.second));
                    }
                    else if constexpr (std::is_same_v<Item, FixedLengthConstraint>) {
                        linear_residual = std::abs(stable_distance(solved(item.first),
                                                                  solved(item.second))
                                                   - item.length_metres);
                    }
                    else if constexpr (std::is_same_v<Item, ParallelConstraint>) {
                        const auto residual = parallel_residual(
                            solved(item.first_start), solved(item.first_end),
                            solved(item.second_start), solved(item.second_end), false);
                        angular_residual = residual.value;
                    }
                    else if constexpr (std::is_same_v<Item, PerpendicularConstraint>) {
                        const auto residual = parallel_residual(
                            solved(item.first_start), solved(item.first_end),
                            solved(item.second_start), solved(item.second_end), true);
                        angular_residual = residual.value;
                    }
                    else {
                        const MutablePoint target{item.x, item.y};
                        linear_residual = stable_distance(solved(item.point), target);
                        if (!std::isfinite(linear_residual)
                            || linear_residual > constraint_linear_tolerance_metres) {
                            anchor_failed = true;
                            add_diagnostic(preview, ConstraintDiagnosticKind::anchor_moved,
                                           "fixed anchor moved beyond the application tolerance", id);
                        }
                    }

                    if (std::isfinite(linear_residual)) {
                        preview.maximum_linear_residual_metres =
                            std::max(preview.maximum_linear_residual_metres, linear_residual);
                    }
                    else {
                        preview.maximum_linear_residual_metres =
                            std::numeric_limits<double>::infinity();
                    }
                    if (std::isfinite(angular_residual)) {
                        preview.maximum_angular_residual_radians =
                            std::max(preview.maximum_angular_residual_radians, angular_residual);
                    }
                    else {
                        preview.maximum_angular_residual_radians =
                            std::numeric_limits<double>::infinity();
                    }
                    if ((!std::isfinite(linear_residual)
                         || linear_residual > constraint_linear_tolerance_metres)
                        || (!std::isfinite(angular_residual)
                            || angular_residual > constraint_angular_tolerance_radians)) {
                        residual_failed = true;
                        add_diagnostic(preview, ConstraintDiagnosticKind::residual_exceeded,
                                       "hard constraint residual exceeds the application tolerance", id);
                    }
                },
                constraint);
        }

        if (anchor_failed) {
            preview.status = ConstraintSolveStatus::rejected_anchor;
            return preview;
        }
        if (residual_failed) {
            preview.status = ConstraintSolveStatus::rejected_residual;
            return preview;
        }

        const auto solved_lookup = [&](const ConstraintPointId& id) -> const MutablePoint& {
            return solved_points[point_indices.at(id)];
        };
        for (const auto& invariant : request.winding_invariants) {
            const long double solved_area = signed_twice_area(invariant, solved_lookup);
            const int solved_orientation = orientation_sign(solved_area);
            if (!std::isfinite(solved_area) || solved_orientation == 0
                || solved_orientation != static_cast<int>(invariant.orientation)) {
                preview.status = ConstraintSolveStatus::rejected_orientation;
                add_diagnostic(preview, ConstraintDiagnosticKind::orientation_changed,
                               "candidate solution changed or collapsed a protected winding");
                return preview;
            }
        }

        preview.points.clear();
        preview.points.reserve(request.points.size());
        for (std::size_t index = 0; index < request.points.size(); ++index) {
            preview.points.push_back(
                {request.points[index].id, solved_points[index].x, solved_points[index].y});
        }
        preview.status = ConstraintSolveStatus::accepted;
        if (preview.degrees_of_freedom > 0) {
            add_diagnostic(preview, ConstraintDiagnosticKind::underconstrained,
                           "candidate solution remains underconstrained");
        }
        return preview;
    }
    catch (const std::exception& error) {
        preview.status = ConstraintSolveStatus::rejected_solver_failure;
        add_diagnostic(preview, ConstraintDiagnosticKind::solver_failure,
                       std::string("PlaneGCS exception: ") + error.what());
        return preview;
    }
    catch (...) {
        preview.status = ConstraintSolveStatus::rejected_solver_failure;
        add_diagnostic(preview, ConstraintDiagnosticKind::solver_failure,
                       "PlaneGCS raised an unknown exception");
        return preview;
    }
}

}  // namespace sketch
