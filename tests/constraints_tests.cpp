#include "sketch/constraints.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace sketch;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "constraints_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "constraints_tests: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        std::exit(1);
    }
}

const ConstraintPoint& point(const ConstraintPreview& preview, std::string_view id) {
    for (const auto& candidate : preview.points) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    fail("preview omitted a stable point id");
}

bool has_diagnostic(const ConstraintPreview& preview, ConstraintDiagnosticKind kind) {
    for (const auto& diagnostic : preview.diagnostics) {
        if (diagnostic.kind == kind) {
            return true;
        }
    }
    return false;
}

void print_rejection(const ConstraintPreview& preview) {
    if (preview.accepted()) {
        return;
    }
    std::cerr << "constraints_tests: rejected status=" << static_cast<int>(preview.status)
              << " dof=" << preview.degrees_of_freedom
              << " linear-residual=" << preview.maximum_linear_residual_metres
              << " angular-residual=" << preview.maximum_angular_residual_radians << '\n';
    for (const auto& diagnostic : preview.diagnostics) {
        std::cerr << "  diagnostic=" << static_cast<int>(diagnostic.kind)
                  << " message=" << diagnostic.message;
        if (diagnostic.constraint_id) {
            std::cerr << " constraint=" << *diagnostic.constraint_id;
        }
        std::cerr << '\n';
    }
}

ConstraintSolveRequest wall_resize_request(std::uint64_t revision = 7) {
    constexpr double twelve_feet = 12.0 * 0.3048;
    constexpr double fourteen_feet = 14.0 * 0.3048;
    ConstraintSolveRequest request;
    request.expected_revision = revision;
    request.points = {
        {"a", 0.0, 0.0},
        {"b", twelve_feet, 0.0},
        {"c", twelve_feet, 2.5},
        {"d", 0.0, 2.5},
    };
    request.constraints = {
        FixedAnchorConstraint{"anchor-a", "a", 0.0, 0.0},
        FixedAnchorConstraint{"anchor-d", "d", 0.0, 2.5},
        HorizontalConstraint{"bottom-horizontal", "a", "b"},
        VerticalConstraint{"right-vertical", "b", "c"},
        HorizontalConstraint{"top-horizontal", "d", "c"},
        FixedLengthConstraint{"wall-length", "a", "b", fourteen_feet},
    };
    request.winding_invariants = {
        {{"a", "b", "c", "d"}, WindingOrientation::counter_clockwise},
    };
    return request;
}

void test_anchored_rectangle_previews_twelve_to_fourteen_feet() {
    auto request = wall_resize_request();
    const auto original = request.points;
    const auto preview = solve_planar_constraints(request);

    print_rejection(preview);
    require(preview.accepted(), "anchored wall resize should produce an accepted preview");
    require(preview.expected_revision == 7, "preview should carry the expected revision");
    require(request.points == original, "solving should not mutate request geometry");
    require(preview.degrees_of_freedom == 0, "anchored rectangle should be fully constrained");
    require_near(point(preview, "a").x, 0.0, 1e-12, "first anchor x");
    require_near(point(preview, "a").y, 0.0, 1e-12, "first anchor y");
    require_near(point(preview, "d").x, 0.0, 1e-12, "second anchor x");
    require_near(point(preview, "d").y, 2.5, 1e-12, "second anchor y");
    require_near(point(preview, "b").x, 14.0 * 0.3048, 1e-8, "resized wall endpoint");
    require_near(point(preview, "c").x, 14.0 * 0.3048, 1e-8, "dependent corner movement");
    require(preview.maximum_linear_residual_metres <= constraint_linear_tolerance_metres,
            "accepted preview must pass application linear residuals");
    require(preview.maximum_angular_residual_radians <= constraint_angular_tolerance_radians,
            "accepted preview must pass application angular residuals");
}

void test_impossible_locked_measurements_reject_atomically() {
    ConstraintSolveRequest request;
    request.expected_revision = 11;
    request.points = {{"a", 0.0, 0.0}, {"b", 1.0, 0.0}};
    request.constraints = {
        FixedAnchorConstraint{"anchor-a", "a", 0.0, 0.0},
        FixedAnchorConstraint{"anchor-b", "b", 1.0, 0.0},
        FixedLengthConstraint{"impossible-length", "a", "b", 2.0},
    };
    const auto original = request.points;
    const auto preview = solve_planar_constraints(request);

    require(!preview.accepted(), "contradictory locked measurements must reject");
    require(preview.status == ConstraintSolveStatus::rejected_conflict ||
                preview.status == ConstraintSolveStatus::rejected_residual,
            "locked-measurement rejection should be a conflict or verified residual failure");
    require(preview.points == original, "rejected solve should return only original geometry");
    require(request.points == original, "rejected solve must not mutate its immutable input");
    require(!preview.conflicting_constraints.empty() ||
                has_diagnostic(preview, ConstraintDiagnosticKind::residual_exceeded),
            "rejection should expose conflict tags or the failed hard residual");
}

void test_underconstrained_system_is_explicit() {
    ConstraintSolveRequest request;
    request.expected_revision = 19;
    request.points = {{"a", 0.0, 0.0}, {"b", 1.0, 0.2}};
    request.constraints = {HorizontalConstraint{"horizontal", "a", "b"}};

    const auto preview = solve_planar_constraints(request);
    require(preview.accepted(), "solvable underconstrained input may preview");
    require(preview.degrees_of_freedom > 0, "underconstrained preview should expose positive DOF");
    require(has_diagnostic(preview, ConstraintDiagnosticKind::underconstrained),
            "underconstrained preview should carry a diagnostic");
    require_near(point(preview, "a").y, point(preview, "b").y,
                 constraint_linear_tolerance_metres, "horizontal residual");
}

void test_redundant_constraint_tags_are_stable_application_ids() {
    ConstraintSolveRequest request;
    request.points = {{"a", 0.0, 0.0}, {"b", 1.0, 0.2}};
    request.constraints = {
        FixedAnchorConstraint{"anchor-a", "a", 0.0, 0.0},
        FixedAnchorConstraint{"duplicate-anchor-a", "a", 0.0, 0.0},
        HorizontalConstraint{"horizontal", "a", "b"},
    };

    const auto preview = solve_planar_constraints(request);
    require(preview.accepted(), "consistent redundancy should still produce a preview");
    require(!preview.redundant_constraints.empty(),
            "redundancy should be exposed through stable application ids");
    require(has_diagnostic(preview, ConstraintDiagnosticKind::redundant),
            "redundancy should carry a diagnostic");
    for (const auto& id : preview.redundant_constraints) {
        require(id == "anchor-a" || id == "duplicate-anchor-a",
                "solver tags must map back to application constraint ids");
    }
}

void test_all_initial_constraint_primitives_are_solved() {
    ConstraintSolveRequest coincident;
    coincident.points = {{"a", 0.0, 0.0}, {"b", 0.2, -0.1}};
    coincident.constraints = {
        FixedAnchorConstraint{"anchor", "a", 0.0, 0.0},
        CoincidentConstraint{"coincident", "a", "b"},
    };
    const auto coincident_preview = solve_planar_constraints(coincident);
    require(coincident_preview.accepted(), "coincident primitive should solve");
    require_near(point(coincident_preview, "b").x, 0.0, 1e-8, "coincident x");
    require_near(point(coincident_preview, "b").y, 0.0, 1e-8, "coincident y");

    ConstraintSolveRequest vertical;
    vertical.points = {{"a", 0.0, 0.0}, {"b", 0.2, 1.9}};
    vertical.constraints = {
        FixedAnchorConstraint{"anchor", "a", 0.0, 0.0},
        VerticalConstraint{"vertical", "a", "b"},
        FixedLengthConstraint{"length", "a", "b", 2.0},
    };
    const auto vertical_preview = solve_planar_constraints(vertical);
    require(vertical_preview.accepted(), "vertical primitive should solve");
    require_near(point(vertical_preview, "b").x, 0.0, 1e-8, "vertical x");
    require_near(point(vertical_preview, "b").y, 2.0, 1e-8, "minimum-motion branch");

    ConstraintSolveRequest parallel;
    parallel.points = {
        {"a", 0.0, 0.0}, {"b", 2.0, 0.0}, {"c", 0.0, 1.0}, {"d", 1.8, 1.2},
    };
    parallel.constraints = {
        FixedAnchorConstraint{"anchor-a", "a", 0.0, 0.0},
        FixedAnchorConstraint{"anchor-b", "b", 2.0, 0.0},
        FixedAnchorConstraint{"anchor-c", "c", 0.0, 1.0},
        ParallelConstraint{"parallel", "a", "b", "c", "d"},
        FixedLengthConstraint{"cd-length", "c", "d", 2.0},
    };
    const auto parallel_preview = solve_planar_constraints(parallel);
    require(parallel_preview.accepted(), "parallel primitive should solve");
    require_near(point(parallel_preview, "d").x, 2.0, 1e-8, "parallel endpoint x");
    require_near(point(parallel_preview, "d").y, 1.0, 1e-8, "parallel endpoint y");

    ConstraintSolveRequest perpendicular = parallel;
    perpendicular.points.back() = {"d", 0.2, 2.8};
    perpendicular.constraints[3] =
        PerpendicularConstraint{"perpendicular", "a", "b", "c", "d"};
    const auto perpendicular_preview = solve_planar_constraints(perpendicular);
    require(perpendicular_preview.accepted(), "perpendicular primitive should solve");
    require_near(point(perpendicular_preview, "d").x, 0.0, 1e-8,
                 "perpendicular endpoint x");
    require_near(point(perpendicular_preview, "d").y, 3.0, 1e-8,
                 "perpendicular minimum-motion branch");
}

void test_winding_change_is_rejected_after_solver_success() {
    ConstraintSolveRequest request;
    request.points = {{"a", 0.0, 0.0}, {"b", 1.0, 0.0}, {"c", 0.0, 1.0}};
    request.constraints = {
        FixedAnchorConstraint{"anchor-a", "a", 0.0, 0.0},
        FixedAnchorConstraint{"anchor-b", "b", 1.0, 0.0},
        FixedAnchorConstraint{"move-c-across-branch", "c", 0.0, -1.0},
    };
    request.winding_invariants = {
        {{"a", "b", "c"}, WindingOrientation::counter_clockwise},
    };
    const auto original = request.points;

    const auto preview = solve_planar_constraints(request);
    require(preview.status == ConstraintSolveStatus::rejected_orientation,
            "mirrored winding should be rejected after solve");
    require(preview.points == original, "orientation rejection should be atomic");
    require(has_diagnostic(preview, ConstraintDiagnosticKind::orientation_changed),
            "orientation rejection should explain the guard");
}

void test_preview_is_revision_bound_and_deterministic() {
    const auto request = wall_resize_request(42);
    const auto first = solve_planar_constraints(request);
    const auto second = solve_planar_constraints(request);

    require(first.expected_revision == 42 && second.expected_revision == 42,
            "preview should carry the revision it expects and never auto-apply");
    require(first.status == second.status, "repeat solve status should be deterministic");
    require(first.degrees_of_freedom == second.degrees_of_freedom,
            "repeat solve DOF should be deterministic");
    require(first.conflicting_constraints == second.conflicting_constraints,
            "repeat conflict tags should be deterministic");
    require(first.redundant_constraints == second.redundant_constraints,
            "repeat redundancy tags should be deterministic");
    require(first.points.size() == second.points.size(), "repeat solve point count");
    for (std::size_t index = 0; index < first.points.size(); ++index) {
        require(first.points[index].id == second.points[index].id, "repeat stable point order");
        require_near(first.points[index].x, second.points[index].x, 1e-12,
                     "repeat deterministic x");
        require_near(first.points[index].y, second.points[index].y, 1e-12,
                     "repeat deterministic y");
    }
}

void test_invalid_input_is_rejected_without_solver_entry() {
    ConstraintSolveRequest request;
    request.points = {{"a", 0.0, 0.0}, {"a", 1.0, 0.0}};
    request.constraints = {HorizontalConstraint{"horizontal", "a", "missing"}};
    const auto preview = solve_planar_constraints(request);
    require(preview.status == ConstraintSolveStatus::rejected_invalid_input,
            "duplicate and missing stable ids should reject input");
    require(preview.points == request.points, "invalid input rejection should preserve points");
    require(has_diagnostic(preview, ConstraintDiagnosticKind::invalid_input),
            "invalid input should carry a diagnostic");

    ConstraintSolveRequest nonfinite;
    nonfinite.points = {{"a", std::numeric_limits<double>::infinity(), 0.0}};
    const auto nonfinite_preview = solve_planar_constraints(nonfinite);
    require(nonfinite_preview.status == ConstraintSolveStatus::rejected_invalid_input,
            "non-finite point coordinates should reject before solver entry");

    ConstraintSolveRequest degenerate_direction;
    degenerate_direction.points = {
        {"a", 0.0, 0.0}, {"b", 0.0, 0.0}, {"c", 0.0, 1.0}, {"d", 1.0, 1.0},
    };
    degenerate_direction.constraints = {
        ParallelConstraint{"parallel", "a", "b", "c", "d"},
    };
    const auto degenerate_preview = solve_planar_constraints(degenerate_direction);
    require(degenerate_preview.status == ConstraintSolveStatus::rejected_invalid_input,
            "degenerate direction segments should reject before solver entry");
}

}  // namespace

int main() {
    test_anchored_rectangle_previews_twelve_to_fourteen_feet();
    test_impossible_locked_measurements_reject_atomically();
    test_underconstrained_system_is_explicit();
    test_redundant_constraint_tags_are_stable_application_ids();
    test_all_initial_constraint_primitives_are_solved();
    test_winding_change_is_rejected_after_solver_success();
    test_preview_is_revision_bound_and_deterministic();
    test_invalid_input_is_rejected_without_solver_entry();
    return 0;
}
