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
} // namespace

int main() {
    try {
        using namespace sketch;
        CalculationProfile profile{"custom-metric",
                                   1,
                                   AreaUnit::square_metre,
                                   2,
                                   {{"living", {true, true}}, {"garage", {true, false}}}};
        auto area = room("a", rectangle(0, 0, 12, 8));
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
