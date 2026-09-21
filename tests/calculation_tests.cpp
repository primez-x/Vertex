#include "sketch/calculations.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
template <class Function> void rejected(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid calculation was accepted");
}
sketch::Boundary rectangle(double x, double y, double w, double h) {
    return {{{x, y}, {x + w, y}, 0},
            {{x + w, y}, {x + w, y + h}, 0},
            {{x + w, y + h}, {x, y + h}, 0},
            {{x, y + h}, {x, y}, 0}};
}
sketch::MeasurementArea room(std::string id, sketch::Boundary boundary) {
    return {std::move(id), "building-1", "floor-1", "living", std::move(boundary), {}, {1, 1}};
}
void appraisal_tests() {
    using namespace sketch;
    using Category = AppraisalAreaCategory;
    auto profile = builtin_appraisal_profile();
    check(profile.version == 1 && profile.display_unit == AreaUnit::square_foot,
          "Built-in appraisal policy has a version and square-foot display");
    const std::vector<std::pair<Category, std::string>> categories{
        {Category::above_grade_finished, "above_grade_finished"},
        {Category::above_grade_unfinished, "above_grade_unfinished"},
        {Category::below_grade_finished, "below_grade_finished"},
        {Category::below_grade_unfinished, "below_grade_unfinished"},
        {Category::garage, "garage"}, {Category::carport, "carport"},
        {Category::porch, "porch"}, {Category::patio, "patio"},
        {Category::deck, "deck"}, {Category::other_non_living, "other_non_living"}};
    std::vector<MeasurementArea> areas;
    for (const auto& [category, name] : categories) {
        check(appraisal_category_name(category) == name && parse_appraisal_category(name) == category,
              "Category persistence names round trip");
        auto area = room(name, rectangle(0, 0, 10, 10));
        area.classification = name;
        area.floor_id = name;
        areas.push_back(area);
    }
    check(parse_appraisal_category("none") == Category::none &&
              appraisal_category_name(Category::none) == "none" &&
              !parse_appraisal_category("Finished basement") && !parse_appraisal_category("GARAGE"),
          "Category parsing is explicit and rejects unknown values");
    rejected([] { (void)appraisal_category_name(static_cast<Category>(999)); });
    auto second = areas.front();
    second.id = "second";
    second.floor_id = "second-floor";
    second.deductions = {{"hole", rectangle(1, 1, 2, 5)}};
    second.factor = {1, 2};
    areas.push_back(second);
    auto other_building = second;
    other_building.id = "other-building";
    other_building.building_id = "building-2";
    areas.push_back(other_building);
    auto site = areas.front();
    site.id = "site";
    site.scope = AreaScope::site;
    areas.push_back(site);
    const auto report = calculate_appraisal_areas(areas, profile);
    near(report.property.gla().total.square_metres, 190, 1e-7,
         "GLA sums finished above-grade floors after deductions and factors only");
    near(report.by_building.at("building-1").gla().total.square_metres, 145, 1e-7,
         "Building appraisal totals stay separate");
    near(report.by_floor.at({"building-1", "second-floor"}).gla().total.square_metres, 45, 1e-7,
         "Per-floor subtotal applies deduction before factor");
    near(report.by_floor.at({"building-2", "second-floor"}).gla().total.square_metres, 45, 1e-7,
         "Floor identity includes building");
    for (const auto& [category, name] : categories) {
        const auto& bucket = report.property.by_category.at(category);
        near(bucket.total.square_metres, category == Category::above_grade_finished ? 190 : 100,
             1e-7, "All appraisal categories remain separate");
        if (category != Category::above_grade_finished)
            check(bucket.area_ids == std::vector<std::string>{name}, "Bucket provenance is exact");
    }
    check(report.property.gla().area_ids ==
              std::vector<std::string>{"above_grade_finished", "other-building", "second"},
          "GLA provenance is sorted and excludes site and nonliving areas");
    check(report.calculation.areas.size() == areas.size(), "Base report retains site calculations");
    auto enclosed_finished = room("enclosed-finished", rectangle(0, 0, 10, 10));
    enclosed_finished.classification = "above_grade_finished";
    enclosed_finished.deductions = {{"internal-garage", rectangle(1, 1, 2, 5)}};
    auto internal_garage = room("internal-garage", rectangle(1, 1, 2, 5));
    internal_garage.classification = "garage";
    const auto enclosed_report =
        calculate_appraisal_areas({enclosed_finished, internal_garage}, profile);
    near(enclosed_report.property.gla().total.square_metres, 90, 1e-7,
         "A categorized internal garage subtracts from enclosing GLA");
    near(enclosed_report.property.by_category.at(Category::garage).total.square_metres,
         10, 1e-7, "A categorized internal garage contributes once to its own bucket");
    check(enclosed_report.property.by_category.at(Category::garage).area_ids ==
              std::vector<std::string>{"internal-garage"},
          "Internal ancillary category retains exact contribution provenance");
    areas.front().classification = "below_grade_finished";
    near(calculate_appraisal_areas(areas, profile).property.gla().total.square_metres, 90, 1e-7,
         "Classification changes immediately recalculate GLA");
    profile.classifications["living"] = {true, true};
    auto legacy = room("legacy", rectangle(0, 0, 2, 2));
    const auto legacy_report = calculate_appraisal_areas({legacy}, profile);
    near(legacy_report.property.gla().total.square_metres, 0, 0,
         "Legacy living flag does not imply an appraisal category");
    check(profile.classifications.at("living").appraisal_category == Category::none,
          "Two-boolean legacy rule defaults to none");
    near(legacy_report.calculation.living.square_metres, 4, 1e-9,
         "Appraisal extension preserves legacy totals");
    profile.classifications["living"].appraisal_category = Category::above_grade_finished;
    near(calculate_appraisal_areas({legacy}, profile).property.gla().total.square_metres, 4, 1e-9,
         "Explicit rule category works independently of classification spelling");
    legacy.boundary = rectangle(0, 0, 0.0004, 1);
    auto tiny = legacy;
    tiny.id = "tiny";
    tiny.floor_id = "floor-2";
    check(calculate_appraisal_areas({legacy, tiny}, profile).property.gla().total.display.text == "0.01",
          "Appraisal aggregate rounds once after summing unrounded areas");
    profile.classifications["living"].appraisal_category = static_cast<Category>(999);
    rejected([&] { (void)calculate_appraisal_areas({legacy}, profile); });
    const auto empty = calculate_appraisal_areas({}, builtin_appraisal_profile());
    check(empty.property.by_category.size() == 16 && empty.property.gla().total.display.text == "0.00",
          "Empty reports expose all zero-valued categories");
}
void declared_policy_tests() {
    using namespace sketch;
    using C = AppraisalAreaCategory;
    const AppraisalFacts standard{PropertyKind::detached_single_family, MeasurementBasis::exterior,
        GradeStatus::above, FinishStatus::finished, AccessStatus::direct_interior,
        CeilingEligibility::standard, AreaUse::dwelling, BoundaryRole::measured_area};
    for (auto value : {AppraisalPolicyKind::residential_declared, AppraisalPolicyKind::light_commercial_declared}) {
        check(parse_appraisal_policy_kind(appraisal_policy_kind_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_appraisal_policy_kind("invalid"), "Unknown fact token rejected");
    rejected([] { (void)appraisal_policy_kind_name(static_cast<AppraisalPolicyKind>(999)); });
    for (auto value : {PropertyKind::detached_single_family, PropertyKind::attached_single_family, PropertyKind::manufactured_home, PropertyKind::apartment_unit, PropertyKind::multifamily, PropertyKind::light_commercial}) {
        check(parse_property_kind(property_kind_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_property_kind("invalid"), "Unknown fact token rejected");
    rejected([] { (void)property_kind_name(static_cast<PropertyKind>(999)); });
    for (auto value : {MeasurementBasis::exterior, MeasurementBasis::interior_perimeter, MeasurementBasis::plans, MeasurementBasis::unknown}) {
        check(parse_measurement_basis(measurement_basis_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_measurement_basis("invalid"), "Unknown fact token rejected");
    rejected([] { (void)measurement_basis_name(static_cast<MeasurementBasis>(999)); });
    for (auto value : {GradeStatus::above, GradeStatus::below, GradeStatus::unknown}) {
        check(parse_grade_status(grade_status_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_grade_status("invalid"), "Unknown fact token rejected");
    rejected([] { (void)grade_status_name(static_cast<GradeStatus>(999)); });
    for (auto value : {FinishStatus::finished, FinishStatus::unfinished, FinishStatus::unknown}) {
        check(parse_finish_status(finish_status_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_finish_status("invalid"), "Unknown fact token rejected");
    rejected([] { (void)finish_status_name(static_cast<FinishStatus>(999)); });
    for (auto value : {AccessStatus::direct_interior, AccessStatus::noncontinuous, AccessStatus::unknown}) {
        check(parse_access_status(access_status_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_access_status("invalid"), "Unknown fact token rejected");
    rejected([] { (void)access_status_name(static_cast<AccessStatus>(999)); });
    for (auto value : {CeilingEligibility::standard, CeilingEligibility::nonstandard, CeilingEligibility::unknown}) {
        check(parse_ceiling_eligibility(ceiling_eligibility_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_ceiling_eligibility("invalid"), "Unknown fact token rejected");
    rejected([] { (void)ceiling_eligibility_name(static_cast<CeilingEligibility>(999)); });
    for (auto value : {AreaUse::dwelling, AreaUse::garage, AreaUse::carport, AreaUse::porch, AreaUse::patio, AreaUse::deck, AreaUse::commercial_occupiable, AreaUse::commercial_common, AreaUse::commercial_service, AreaUse::other_non_living}) {
        check(parse_area_use(area_use_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_area_use("invalid"), "Unknown fact token rejected");
    rejected([] { (void)area_use_name(static_cast<AreaUse>(999)); });
    for (auto value : {BoundaryRole::measured_area, BoundaryRole::open_to_below, BoundaryRole::stair_footprint, BoundaryRole::other_void}) {
        check(parse_boundary_role(boundary_role_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_boundary_role("invalid"), "Unknown fact token rejected");
    rejected([] { (void)boundary_role_name(static_cast<BoundaryRole>(999)); });
    { auto invalid = standard; invalid.property_kind = static_cast<PropertyKind>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.measurement_basis = static_cast<MeasurementBasis>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.grade = static_cast<GradeStatus>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.finish = static_cast<FinishStatus>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.access = static_cast<AccessStatus>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.ceiling = static_cast<CeilingEligibility>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.use = static_cast<AreaUse>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.role = static_cast<BoundaryRole>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    rejected([&] { (void)derive_appraisal_category(standard, {static_cast<AppraisalPolicyKind>(999), 1}); });
    auto expect = [](const AppraisalFacts& facts, C category, AppraisalPolicy policy = {}) {
        const auto result = derive_appraisal_category(facts, policy);
        check(result.qualified && result.issues.empty() && result.derived_category == category,
              "Declared facts derive the expected category");
    };
    expect(standard, C::above_grade_finished);
    for (auto kind : {PropertyKind::attached_single_family, PropertyKind::manufactured_home}) {
        auto facts = standard; facts.property_kind = kind;
        expect(facts, C::above_grade_finished);
    }
    for (auto grade : {GradeStatus::above, GradeStatus::below}) {
        auto facts = standard; facts.grade = grade;
        expect(facts, grade == GradeStatus::above ? C::above_grade_finished : C::below_grade_finished);
        facts.ceiling = CeilingEligibility::nonstandard;
        expect(facts, grade == GradeStatus::above ? C::above_grade_nonstandard_finished : C::below_grade_nonstandard_finished);
        facts.finish = FinishStatus::unfinished;
        facts.access = AccessStatus::unknown; facts.ceiling = CeilingEligibility::unknown;
        expect(facts, grade == GradeStatus::above ? C::above_grade_unfinished : C::below_grade_unfinished);
    }
    auto facts = standard; facts.access = AccessStatus::noncontinuous;
    expect(facts, C::noncontinuous_finished);
    facts.ceiling = CeilingEligibility::nonstandard;
    expect(facts, C::noncontinuous_finished); // Noncontinuous takes precedence above grade.
    facts.grade = GradeStatus::below;
    check(!derive_appraisal_category(facts).qualified,
          "Below-grade noncontinuous finished space has no supported category");
    for (const auto& [use, category] : std::vector<std::pair<AreaUse, C>>{
             {AreaUse::garage, C::garage}, {AreaUse::carport, C::carport},
             {AreaUse::porch, C::porch}, {AreaUse::patio, C::patio}, {AreaUse::deck, C::deck},
             {AreaUse::other_non_living, C::other_non_living}}) {
        facts = standard; facts.use = use; facts.grade = GradeStatus::unknown;
        facts.finish = FinishStatus::unknown; facts.ceiling = CeilingEligibility::unknown;
        facts.access = AccessStatus::unknown;
        expect(facts, category);
    }
    facts = standard; facts.property_kind = PropertyKind::apartment_unit;
    check(!derive_appraisal_category(facts).qualified, "Apartment exterior basis is incompatible");
    facts.measurement_basis = MeasurementBasis::interior_perimeter;
    expect(facts, C::above_grade_finished);
    for (auto kind : {PropertyKind::multifamily, PropertyKind::light_commercial}) {
        facts = standard; facts.property_kind = kind;
        check(!derive_appraisal_category(facts).qualified, "Residential policy rejects unsupported property kinds");
    }
    facts = standard; facts.measurement_basis = MeasurementBasis::interior_perimeter;
    check(!derive_appraisal_category(facts).qualified, "Whole house needs exterior or plans basis");
    facts.measurement_basis = MeasurementBasis::plans;
    expect(facts, C::above_grade_finished);
    facts = standard; facts.measurement_basis = MeasurementBasis::unknown;
    facts.grade = GradeStatus::unknown; facts.finish = FinishStatus::unknown;
    const auto unknown = derive_appraisal_category(facts);
    check(!unknown.qualified && !unknown.derived_category && unknown.issues.size() == 3 &&
          unknown.issues[0].code == "measurement_basis_unknown" &&
          unknown.issues[1].code == "grade_unknown" && unknown.issues[2].code == "finish_unknown",
          "Unknown required facts produce deterministic structured issues");
    for (bool access : {false, true}) {
        facts = standard;
        if (access) facts.access = AccessStatus::unknown;
        else facts.ceiling = CeilingEligibility::unknown;
        check(!derive_appraisal_category(facts).qualified, "Finished dwelling requires access and ceiling facts");
    }
    for (auto factor : {ExactRational{1, 2}, ExactRational{0, 1}, ExactRational{2, 1}}) {
        const auto result = derive_appraisal_category(standard, {}, factor);
        check(!result.qualified && !result.derived_category && result.issues[0].code == "factor_not_unity",
              "Factored arithmetic is not a qualified physical area");
    }
    check(derive_appraisal_category(standard, {}, {2, 2}).qualified, "Exact rational unity is accepted");
    rejected([&] { (void)derive_appraisal_category(standard, {}, {1, 0}); });
    rejected([&] { (void)derive_appraisal_category(standard, {}, {-1, 1}); });
    for (auto role : {BoundaryRole::open_to_below, BoundaryRole::stair_footprint, BoundaryRole::other_void}) {
        facts = standard; facts.role = role; facts.grade = GradeStatus::unknown;
        facts.finish = FinishStatus::unknown; facts.access = AccessStatus::unknown;
        facts.ceiling = CeilingEligibility::unknown;
        const auto result = derive_appraisal_category(facts);
        check(result.qualified && !result.derived_category,
              "Exclusion roles cannot create standalone category contributions");
    }
    AppraisalPolicy commercial{AppraisalPolicyKind::light_commercial_declared, 1};
    facts = {}; facts.property_kind = PropertyKind::light_commercial;
    facts.measurement_basis = MeasurementBasis::exterior;
    facts.role = BoundaryRole::open_to_below;
    const auto commercial_exclusion = derive_appraisal_category(facts, commercial);
    check(commercial_exclusion.qualified && !commercial_exclusion.derived_category,
          "Commercial exclusion roles do not require a fictitious contributing area use");
    facts.role = BoundaryRole::measured_area;
    for (auto use : {AreaUse::commercial_occupiable, AreaUse::commercial_common, AreaUse::commercial_service}) {
        facts.use = use;
        expect(facts, use == AreaUse::commercial_occupiable ? C::commercial_occupiable :
                      use == AreaUse::commercial_common ? C::commercial_common : C::commercial_service, commercial);
        check(!derive_appraisal_category(facts).qualified, "Commercial use cannot enter residential policy");
    }
    facts.use = AreaUse::dwelling;
    check(!derive_appraisal_category(facts, commercial).qualified, "Commercial policy rejects dwelling use");
    rejected([&] { (void)derive_appraisal_category(standard, {AppraisalPolicyKind::residential_declared, 2}); });
    auto area = room("qualified", rectangle(0, 0, 10, 10));
    area.deductions = {{"void", rectangle(1, 1, 2, 5)}};
    area.factor = {1, 2};
    auto measured = qualify_appraisal_area(area, standard);
    check(!measured.qualified && measured.policy_id == "vertex-residential-declared-v1" && measured.policy_version == 1,
          "Measurement retains declared policy provenance");
    near(measured.physical_square_metres.value(), 90, 1e-7, "Physical measurement stays separate from adjustment");
    near(measured.adjusted_square_metres.value(), 45, 1e-7, "Custom adjustment remains inspectable");
    area.factor = {1, 1}; area.scope = AreaScope::site;
    check(!qualify_appraisal_area(area, standard).qualified, "Site scope cannot qualify as building area");
    std::vector<MeasurementArea> commercial_areas;
    for (const auto& [category, name] : std::vector<std::pair<C, std::string>>{
             {C::above_grade_nonstandard_finished, "above_grade_nonstandard_finished"},
             {C::below_grade_nonstandard_finished, "below_grade_nonstandard_finished"},
             {C::noncontinuous_finished, "noncontinuous_finished"},
             {C::commercial_occupiable, "commercial_occupiable"},
             {C::commercial_common, "commercial_common"},
             {C::commercial_service, "commercial_service"}}) {
        check(parse_appraisal_category(name) == category && appraisal_category_name(category) == name,
              "New categories have stable persistence names");
        auto item = room(name, rectangle(0, 0, 10, 10));
        item.classification = name; item.floor_id = name;
        commercial_areas.push_back(item);
    }
    const auto totals = calculate_appraisal_areas(commercial_areas, builtin_appraisal_profile()).property;
    near(totals.commercial_gross_square_metres(), 300, 1e-7,
         "Commercial gross sums occupiable common service once, excluding residential categories");
    near(totals.by_category.at(C::commercial_occupiable).total.square_metres, 100, 1e-7,
         "Commercial occupiable excludes common and service areas");
    near(totals.nonstandard_finished_square_metres(), 200, 1e-7,
         "Nonstandard subtotal excludes noncontinuous and regular finished area");
    near(totals.gla().total.square_metres, 0, 1e-7, "New categories never enter legacy GLA");
}
} // namespace

int main() {
    try {
        using namespace sketch;
        appraisal_tests();
        declared_policy_tests();
        CalculationProfile profile{"custom-metric",
                                   1,
                                   AreaUnit::square_metre,
                                   2,
                                   {{"living", {true, true}}, {"garage", {true, false}}}};
        auto area = room("a", rectangle(0, 0, 12, 8));
        auto parcel = room("parcel", rectangle(-1, -1, 100, 100));
        parcel.scope = AreaScope::site;
        const auto mixed = calculate_areas({area, parcel}, profile);
        near(mixed.building.square_metres, 96, 1e-9, "Site enclosure must not add building area");
        near(mixed.living.square_metres, 96, 1e-9, "Site scope must override a living classification rule");
        auto overlapping_site = parcel;
        overlapping_site.id = "second-parcel";
        rejected([&] { (void)calculate_areas({parcel, overlapping_site}, profile); });
        overlapping_site.scope = static_cast<AreaScope>(99);
        rejected([&] { (void)calculate_area(overlapping_site, profile); });
        auto result = calculate_area(area, profile);
        near(result.base_square_metres, 96, 1e-9, "Base area");
        near(result.perimeter_metres, 40, 1e-9, "Boundary perimeter");
        check(result.area_id == "a" && result.profile_id == profile.id && result.profile_version == 1,
              "Calculation provenance");
        area.deductions = {{"b", rectangle(3, 1, 3, 2)}, {"a", rectangle(1, 1, 4, 2)}};
        area.factor = {1, 2};
        result = calculate_area(area, profile);
        near(result.deducted_square_metres, 10, 1e-7, "Overlapping deductions subtract union once");
        near(result.net_square_metres, 86, 1e-7, "Net boundary area");
        near(result.factored_square_metres, 43, 1e-7, "Factor applies after deductions");
        check(result.deductions[0].id == "a", "Deduction provenance order is deterministic");
        near(result.deductions[0].applied_square_metres, 8, 1e-7, "First deduction trace");
        near(result.deductions[1].applied_square_metres, 2, 1e-7, "Overlapping deduction trace");
        std::reverse(area.deductions.begin(), area.deductions.end());
        near(calculate_area(area, profile).factored_square_metres, 43, 1e-7,
             "Input order cannot alter total");
        area.deductions = {{"all", area.boundary}};
        near(calculate_area(area, profile).net_square_metres, 0, 1e-7, "Full-area deduction yields zero");
        area.deductions = {{"outside", rectangle(11, 1, 3, 2)}};
        rejected([&] { (void)calculate_area(area, profile); });
        area.deductions = {{"duplicate", rectangle(1, 1, 1, 1)}, {"duplicate", rectangle(3, 1, 1, 1)}};
        rejected([&] { (void)calculate_area(area, profile); });

        area = room("circle", {{{1, 0}, {-1, 0}, std::numbers::pi}, {{-1, 0}, {1, 0}, std::numbers::pi}});
        result = calculate_area(area, profile);
        near(result.base_square_metres, std::numbers::pi, 1e-8, "Circular area remains analytical");
        near(result.perimeter_metres, 2 * std::numbers::pi, 1e-8, "Circular perimeter remains analytical");
        for (auto& edge : area.boundary) {
            std::swap(edge.start, edge.end);
            edge.sweep_radians *= -1;
        }
        std::reverse(area.boundary.begin(), area.boundary.end());
        near(calculate_area(area, profile).base_square_metres, std::numbers::pi, 1e-8,
             "Winding invariant area");
        area = room("translated", rectangle(1e9, 1e9, 4, 3));
        area.deductions = {{"hole", rectangle(1e9 + 1, 1e9 + 1, 1, 1)}};
        near(calculate_area(area, profile).net_square_metres, 11, 1e-7, "Translation preserves deductions");

        auto a = room("a", rectangle(0, 0, 1.004, 1));
        auto b = room("b", rectangle(2, 0, 1.004, 1));
        b.classification = "garage";
        auto report = calculate_areas({a, b}, profile);
        near(report.building.square_metres, 2.008, 1e-10, "Aggregate unrounded values");
        check(report.building.display.text == "2.01", "Round aggregate only after summing");
        check(report.living.display.text == "1.00", "Explicit living classification rule");
        check(report.by_classification.size() == 2, "Classification subtotals");
        b.boundary = rectangle(0.5, 0, 1, 1);
        rejected([&] { (void)calculate_areas({a, b}, profile); });
        b.floor_id = "floor-2";
        check(calculate_areas({a, b}, profile).areas.size() == 2, "Separate floors may share footprints");
        b.id = a.id;
        rejected([&] { (void)calculate_areas({a, b}, profile); });
        a.classification = "unknown";
        rejected([&] { (void)calculate_area(a, profile); });
        a.classification = "living";
        a.factor = {-1, 1};
        rejected([&] { (void)calculate_area(a, profile); });
        a.factor = {1, 0};
        rejected([&] { (void)calculate_area(a, profile); });
        a.factor = {1, 1};
        a.boundary.pop_back();
        rejected([&] { (void)calculate_area(a, profile); });

        profile.display_unit = AreaUnit::square_foot;
        near(display_area(0.09290304, profile).unrounded, 1, 1e-12, "Exact SI-square-foot conversion");
        profile.display_unit = AreaUnit::acre;
        near(display_area(4046.8564224, profile).unrounded, 1, 1e-12, "Acre conversion");
        profile.display_unit = AreaUnit::square_metre;
        profile.decimal_places = 1;
        check(display_area(1.25, profile).text == "1.3", "Half-away rounding policy");
        near(display_area(1.25, profile).rounding_delta, 0.05, 1e-12, "Visible rounding delta");
        rejected([&] { (void)display_area(std::numeric_limits<double>::infinity(), profile); });
        profile.decimal_places = 20;
        rejected([&] { (void)display_area(1, profile); });
        std::cout << "Calculation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
