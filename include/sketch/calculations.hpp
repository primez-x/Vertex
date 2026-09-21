#pragma once

#include "sketch/geometry.hpp"
#include "sketch/quantity.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {

enum class AreaUnit { square_metre, square_foot, acre };
enum class AreaScope { building, site };
enum class AppraisalAreaCategory {
    none,
    above_grade_finished,
    above_grade_unfinished,
    below_grade_finished,
    below_grade_unfinished,
    garage,
    carport,
    porch,
    patio,
    deck,
    other_non_living
};

// Stable persistence tokens; unknown strings return nullopt, invalid enums throw.
[[nodiscard]] std::string_view appraisal_category_name(AppraisalAreaCategory category);
[[nodiscard]] std::optional<AppraisalAreaCategory> parse_appraisal_category(std::string_view name);

struct ClassificationRule {
    bool building_total{};
    bool living_total{};
    AppraisalAreaCategory appraisal_category{AppraisalAreaCategory::none};
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
    AreaScope scope{AreaScope::building};
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
    AreaScope scope{AreaScope::building};
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

struct AppraisalAreaBucket {
    AreaTotal total;
    std::vector<std::string> area_ids;
};

struct AppraisalTotals {
    // Calculated reports contain every named category, excluding none.
    std::map<AppraisalAreaCategory, AppraisalAreaBucket> by_category;
    [[nodiscard]] const AppraisalAreaBucket& gla() const {
        return by_category.at(AppraisalAreaCategory::above_grade_finished);
    }
};

struct AppraisalCalculationReport {
    CalculationReport calculation;
    AppraisalTotals property;
    std::map<std::string, AppraisalTotals> by_building;
    std::map<std::pair<std::string, std::string>, AppraisalTotals> by_floor;
};

// Application policy, not a measurement-standard compliance assertion.
[[nodiscard]] CalculationProfile builtin_appraisal_profile();
// Reuses calculate_areas validation and unrounded factored values. Only building
// scope and explicitly mapped categories contribute; GLA is above-grade finished.
[[nodiscard]] AppraisalCalculationReport calculate_appraisal_areas(
    const std::vector<MeasurementArea>& areas, const CalculationProfile& profile);

// All topology and dimensions are analytical. Deductions must be contained in
// the area; touching the boundary is allowed and overlaps subtract only once.
// Invalid/unresolved geometry throws without returning a plausible total.
[[nodiscard]] AreaCalculation calculate_area(const MeasurementArea& area, const CalculationProfile& profile);
// Aggregation sums unrounded factored results, then rounds once. Duplicate area
// IDs and positive overlaps on the same building/floor are errors. Adjacent
// regions and coincident geometry on different floors remain independent.
// Site and building scopes may overlap; site results never enter building or
// living totals. Within each scope, ordinary overlap checks still apply.
[[nodiscard]] CalculationReport calculate_areas(const std::vector<MeasurementArea>& areas,
                                                const CalculationProfile& profile);
[[nodiscard]] DisplayArea display_area(double square_metres, const CalculationProfile& profile);

} // namespace sketch
