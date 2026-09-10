#pragma once

#include "sketch/constraint_tolerances.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sketch {

using ConstraintPointId = std::string;
using ConstraintId = std::string;

struct ConstraintPoint {
    ConstraintPointId id;
    double x{};
    double y{};

    bool operator==(const ConstraintPoint&) const = default;
};

struct HorizontalConstraint {
    ConstraintId id;
    ConstraintPointId first;
    ConstraintPointId second;
};

struct VerticalConstraint {
    ConstraintId id;
    ConstraintPointId first;
    ConstraintPointId second;
};

struct CoincidentConstraint {
    ConstraintId id;
    ConstraintPointId first;
    ConstraintPointId second;
};

struct FixedLengthConstraint {
    ConstraintId id;
    ConstraintPointId first;
    ConstraintPointId second;
    double length_metres{};
};

struct ParallelConstraint {
    ConstraintId id;
    ConstraintPointId first_start;
    ConstraintPointId first_end;
    ConstraintPointId second_start;
    ConstraintPointId second_end;
};

struct PerpendicularConstraint {
    ConstraintId id;
    ConstraintPointId first_start;
    ConstraintPointId first_end;
    ConstraintPointId second_start;
    ConstraintPointId second_end;
};

struct FixedAnchorConstraint {
    ConstraintId id;
    ConstraintPointId point;
    double x{};
    double y{};
};

using PlanarConstraint =
    std::variant<HorizontalConstraint, VerticalConstraint, CoincidentConstraint,
                 FixedLengthConstraint, ParallelConstraint, PerpendicularConstraint,
                 FixedAnchorConstraint>;

enum class WindingOrientation { clockwise = -1, counter_clockwise = 1 };

struct WindingInvariant {
    std::vector<ConstraintPointId> loop;
    WindingOrientation orientation{WindingOrientation::counter_clockwise};
};

struct ConstraintSolveRequest {
    std::uint64_t expected_revision{};
    std::vector<ConstraintPoint> points;
    std::vector<PlanarConstraint> constraints;
    std::vector<WindingInvariant> winding_invariants;
};

enum class ConstraintSolveStatus {
    accepted,
    rejected_invalid_input,
    rejected_solver_failure,
    rejected_conflict,
    rejected_residual,
    rejected_anchor,
    rejected_orientation,
};

enum class ConstraintDiagnosticKind {
    invalid_input,
    underconstrained,
    solver_failure,
    conflicting,
    redundant,
    residual_exceeded,
    anchor_moved,
    orientation_changed,
};

struct ConstraintDiagnostic {
    ConstraintDiagnosticKind kind{};
    std::string message;
    std::optional<ConstraintId> constraint_id;
};

struct ConstraintPreview {
    ConstraintSolveStatus status{ConstraintSolveStatus::rejected_invalid_input};
    std::uint64_t expected_revision{};
    std::vector<ConstraintPoint> points;
    int degrees_of_freedom{-1};
    std::vector<ConstraintId> conflicting_constraints;
    std::vector<ConstraintId> redundant_constraints;
    std::vector<ConstraintDiagnostic> diagnostics;
    double maximum_linear_residual_metres{};
    double maximum_angular_residual_radians{};

    [[nodiscard]] bool accepted() const noexcept {
        return status == ConstraintSolveStatus::accepted;
    }
};

// The immutable request is never modified. Rejected previews contain the
// original points, so callers cannot accidentally publish a partial solve.
[[nodiscard]] ConstraintPreview solve_planar_constraints(const ConstraintSolveRequest& request);

}  // namespace sketch
