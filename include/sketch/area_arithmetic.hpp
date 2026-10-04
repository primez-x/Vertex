#pragma once

#include "sketch/geometry.hpp"

namespace sketch {

enum class AreaArithmeticMethod { rectangular_components, triangle, polygon_integral, chord_and_arcs };
struct AreaRectangleComponent {
    double width_metres{};
    double depth_metres{};
    double area_square_metres{};
};
struct AreaArithmetic {
    AreaArithmeticMethod method{AreaArithmeticMethod::polygon_integral};
    std::vector<AreaRectangleComponent> rectangles;
    std::optional<Vec2> triangle_base_and_height_metres;
    // Directed chord contribution, normalized to the validated area's winding.
    // Major arcs can make this negative; it is not an independent measured area.
    double chord_contribution_square_metres{};
    double curve_adjustment_square_metres{};
    double gross_square_metres{};
};

// Explanation of a single valid closed analytical boundary in metres. Does not
// include deductions, factors, rounding, appraisal eligibility or cached data.
// Throws for invalid geometry or a numerically unreconciled decomposition.
[[nodiscard]] AreaArithmetic derive_area_arithmetic(const Boundary& boundary);

} // namespace sketch
