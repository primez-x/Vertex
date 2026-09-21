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
    check(empty.property.by_category.size() == 10 && empty.property.gla().total.display.text == "0.00",
          "Empty reports expose all zero-valued categories");
}
} // namespace

int main() {
    try {
        using namespace sketch;
        appraisal_tests();
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
