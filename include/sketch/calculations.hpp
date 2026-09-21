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
    other_non_living,
    above_grade_nonstandard_finished,
    below_grade_nonstandard_finished,
    noncontinuous_finished,
    commercial_occupiable,
    commercial_common,
    commercial_service
};

// Stable persistence tokens; unknown strings return nullopt, invalid enums throw.
[[nodiscard]] std::string_view appraisal_category_name(AppraisalAreaCategory category);
[[nodiscard]] std::optional<AppraisalAreaCategory> parse_appraisal_category(std::string_view name);

enum class AppraisalPolicyKind { residential_declared, light_commercial_declared };
enum class PropertyKind { detached_single_family, attached_single_family, manufactured_home,
                          apartment_unit, multifamily, light_commercial };
enum class MeasurementBasis { exterior, interior_perimeter, plans, unknown };
// Below includes any level with even a portion below grade.
enum class GradeStatus { above, below, unknown };
enum class FinishStatus { finished, unfinished, unknown };
enum class AccessStatus { direct_interior, noncontinuous, unknown };
enum class CeilingEligibility { standard, nonstandard, unknown };
enum class AreaUse { dwelling, garage, carport, porch, patio, deck, commercial_occupiable,
                     commercial_common, commercial_service, other_non_living };
enum class BoundaryRole { measured_area, open_to_below, stair_footprint, other_void };

#define SKETCH_DECLARE_FACT_TOKENS(Type, name) \
    [[nodiscard]] std::string_view name##_name(Type value); \
    [[nodiscard]] std::optional<Type> parse_##name(std::string_view token);
SKETCH_DECLARE_FACT_TOKENS(AppraisalPolicyKind, appraisal_policy_kind)
SKETCH_DECLARE_FACT_TOKENS(PropertyKind, property_kind)
SKETCH_DECLARE_FACT_TOKENS(MeasurementBasis, measurement_basis)
SKETCH_DECLARE_FACT_TOKENS(GradeStatus, grade_status)
SKETCH_DECLARE_FACT_TOKENS(FinishStatus, finish_status)
SKETCH_DECLARE_FACT_TOKENS(AccessStatus, access_status)
SKETCH_DECLARE_FACT_TOKENS(CeilingEligibility, ceiling_eligibility)
SKETCH_DECLARE_FACT_TOKENS(AreaUse, area_use)
SKETCH_DECLARE_FACT_TOKENS(BoundaryRole, boundary_role)
#undef SKETCH_DECLARE_FACT_TOKENS

struct AppraisalPolicy {
    AppraisalPolicyKind kind{AppraisalPolicyKind::residential_declared};
    unsigned version{1};
};
[[nodiscard]] std::string_view appraisal_policy_id(AppraisalPolicy policy);

struct AppraisalFacts {
    PropertyKind property_kind{PropertyKind::detached_single_family};
    MeasurementBasis measurement_basis{MeasurementBasis::unknown};
    GradeStatus grade{GradeStatus::unknown};
    FinishStatus finish{FinishStatus::unknown};
    AccessStatus access{AccessStatus::unknown};
    CeilingEligibility ceiling{CeilingEligibility::unknown};
    AreaUse use{AreaUse::dwelling};
    BoundaryRole role{BoundaryRole::measured_area};
};

struct QualificationIssue {
    std::string code;
    std::string message;
};
struct AppraisalQualification {
    std::string policy_id;
    unsigned policy_version{};
    bool qualified{};
    std::optional<AppraisalAreaCategory> derived_category;
    std::vector<QualificationIssue> issues;
    // Net physical geometry after deductions, before factor. Absent until measured.
    std::optional<double> physical_square_metres;
    std::optional<double> adjusted_square_metres;
};
// Qualification covers only Vertex's declared-facts contract, not ANSI/BOMA or
// lender certification. Invalid enum values throw; unknown facts produce issues.
// Non-measured roles have no standalone contribution and need no dwelling facts.
[[nodiscard]] AppraisalQualification derive_appraisal_category(
    const AppraisalFacts& facts, AppraisalPolicy policy = {}, ExactRational factor = {1, 1});

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
    // Legacy alias for the above_grade_finished bucket; no compliance assertion.
    [[nodiscard]] const AppraisalAreaBucket& gla() const {
        return by_category.at(AppraisalAreaCategory::above_grade_finished);
    }
    [[nodiscard]] double commercial_gross_square_metres() const;
    [[nodiscard]] double nonstandard_finished_square_metres() const;
};

// Derives classification independently of the manual classification string.
// Invalid geometry remains an exception, matching calculate_area.
[[nodiscard]] AppraisalQualification qualify_appraisal_area(
    const MeasurementArea& area, const AppraisalFacts& facts, AppraisalPolicy policy = {});

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
