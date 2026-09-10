#pragma once

#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <map>
#include <string>
#include <vector>

namespace sketch {

enum class AreaUnit { square_metre, square_foot, acre };

struct ClassificationRule {
    bool building_total{};
    bool living_total{};
};

// This is an application calculation policy, not a claim of compliance with a
// measurement standard. Explicit classification rules never infer living area.
struct CalculationProfile {
    std::string id;
    unsigned version{1};
    AreaUnit display_unit{AreaUnit::square_metre};
    unsigned decimal_places{2};
    std::map<std::string, ClassificationRule> classifications;
};

struct AreaDeduction {
    std::string id;
    Boundary boundary;
};

struct MeasurementArea {
    std::string id;
    std::string building_id;
    std::string floor_id;
    std::string classification;
    Boundary boundary;
    std::vector<AreaDeduction> deductions;
    ExactRational factor{1, 1};
};

struct DisplayArea {
    AreaUnit unit{};
    double unrounded{};
    double rounded{};
    double rounding_delta{};
    std::string text;
};

struct DeductionTrace {
    std::string id;
    double requested_square_metres{};
    // Deduction IDs are sorted. Marginal area excludes portions already
    // removed by preceding deductions, preventing double subtraction.
    double applied_square_metres{};
};

struct AreaCalculation {
    std::string area_id;
    std::string building_id;
    std::string floor_id;
    std::string classification;
    std::string profile_id;
    unsigned profile_version{};
    double base_square_metres{};
    double perimeter_metres{};
    std::vector<DeductionTrace> deductions;
    double deducted_square_metres{};
    double net_square_metres{};
    ExactRational factor;
    double factored_square_metres{};
    DisplayArea display;
};

struct AreaTotal {
    double square_metres{};
    DisplayArea display;
};

struct CalculationReport {
    std::string profile_id;
    unsigned profile_version{};
    std::vector<AreaCalculation> areas;
    std::map<std::string, AreaTotal> by_classification;
    AreaTotal building;
    AreaTotal living;
};

// All topology and dimensions are analytical. Deductions must be contained in
// the area; touching the boundary is allowed and overlaps subtract only once.
// Invalid/unresolved geometry throws without returning a plausible total.
[[nodiscard]] AreaCalculation calculate_area(const MeasurementArea& area, const CalculationProfile& profile);
// Aggregation sums unrounded factored results, then rounds once. Duplicate area
// IDs and positive overlaps on the same building/floor are errors. Adjacent
// regions and coincident geometry on different floors remain independent.
[[nodiscard]] CalculationReport calculate_areas(const std::vector<MeasurementArea>& areas,
                                                const CalculationProfile& profile);
[[nodiscard]] DisplayArea display_area(double square_metres, const CalculationProfile& profile);

} // namespace sketch
