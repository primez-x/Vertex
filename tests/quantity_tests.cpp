#include "sketch/quantity.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using sketch::ExactRational;
using sketch::Unit;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "quantity_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "quantity_tests: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        std::exit(1);
    }
}

void require_exact(const sketch::Quantity& quantity, std::int64_t numerator,
                   std::int64_t denominator, std::string_view message) {
    if (quantity.exact_metres != ExactRational{numerator, denominator}) {
        std::cerr << "quantity_tests: " << message << ": expected " << numerator << '/'
                  << denominator << ", got " << quantity.exact_metres.numerator << '/'
                  << quantity.exact_metres.denominator << '\n';
        std::exit(1);
    }
}

void require_invalid(std::string_view expression, std::string_view message) {
    try {
        (void)sketch::parse_quantity(expression);
    } catch (const std::invalid_argument&) {
        return;
    }
    fail(message);
}

void test_decimal_units_are_exact() {
    const auto metres = sketch::parse_quantity("2.5m");
    require_exact(metres, 5, 2, "decimal metres");
    require_near(metres.metres, 2.5, 1e-15, "decimal metre value");
    require(metres.entered_unit == Unit::metre, "metre unit should be recorded");

    require_exact(sketch::parse_quantity("1250mm"), 5, 4, "millimetres");
    require_exact(sketch::parse_quantity("12.5cm"), 1, 8, "centimetres");
    require_exact(sketch::parse_quantity("5ft"), 381, 250, "decimal feet");
    require_exact(sketch::parse_quantity("6in"), 381, 2500, "decimal inches");
    require_exact(sketch::parse_quantity("1/8m"), 1, 8, "fractional metres");
    require_exact(sketch::parse_quantity("-2.5cm"), -1, 40, "negative centimetres");
    require_exact(sketch::parse_quantity("9223372036854775807.000m"),
                  9223372036854775807LL, 1,
                  "insignificant decimal zeros should not cause overflow");
    require_exact(sketch::parse_quantity("0.0000000000000000000m"), 0, 1,
                  "zero with a long decimal scale");

    require_exact(sketch::parse_quantity("2"), 381, 625, "default feet");
    require_exact(sketch::parse_quantity("2", Unit::metre), 2, 1, "explicit default metres");
}

void test_feet_and_inches_preserve_exact_fraction() {
    const std::string expression = "12' 3 1/2\"";
    const auto quantity = sketch::parse_quantity(expression);
    require_exact(quantity, 7493, 2000, "feet plus fractional inches");
    require_near(quantity.metres, 3.7465, 1e-15, "feet plus inches value");
    require(quantity.entered_unit == Unit::foot, "composite expression should record feet");
    require(quantity.original_expression == expression, "original expression should be preserved");

    require_exact(sketch::parse_quantity("-5' 6\""), -4191, 2500,
                  "negative feet plus inches");
    require_exact(sketch::parse_quantity("+2ft 3/4in"), 12573, 20000,
                  "explicit positive feet plus fractional inches");
}

void test_imperial_and_metric_entries_share_exact_geometry() {
    const auto imperial = sketch::parse_quantity("12' 3 1/2\"");
    const auto metres = sketch::parse_quantity("3.7465m");
    const auto centimetres = sketch::parse_quantity("374.65cm");
    require(imperial.exact_metres == metres.exact_metres,
            "equivalent imperial and metre entries should share exact geometry");
    require(imperial.exact_metres == centimetres.exact_metres,
            "equivalent imperial and centimetre entries should share exact geometry");
    require(metres.original_expression == "3.7465m",
            "metric entry should preserve its original expression");
    require(metres.entered_unit == Unit::metre,
            "metre entry should preserve its entered unit");
    require(centimetres.original_expression == "374.65cm",
            "centimetre entry should preserve its original expression");
    require(centimetres.entered_unit == Unit::centimetre,
            "centimetre entry should preserve its entered unit");
}

void test_invalid_and_overflowing_input_is_rejected() {
    require_invalid("", "empty input should be rejected");
    require_invalid("12cm trailing", "trailing text should be rejected");
    require_invalid("nan m", "NaN should be rejected");
    require_invalid("inf ft", "infinity should be rejected");
    require_invalid("1/0m", "zero denominator should be rejected");
    require_invalid("5ft 6", "feet plus inches requires an inch marker");
    require_invalid("1e3m", "exponential notation outside the grammar should be rejected");
    require_invalid("--1m", "multiple signs should be rejected");
    require_invalid("9223372036854775808m", "integer overflow should be rejected");
    require_invalid("9223372036854775807ft", "unit conversion overflow should be rejected");
    require_invalid("0.0000000000000000001m", "decimal denominator overflow should be rejected");
}

void test_display_round_trip_is_exact_and_non_mutating() {
    auto quantity = sketch::parse_quantity("12' 3 1/2\"");
    const auto original = quantity;

    const auto inches = sketch::format_quantity(quantity, Unit::inch);
    require(inches == "295/2 in", "inch display should be canonical and exact");
    require_exact(sketch::parse_quantity(inches), 7493, 2000, "inch display round trip");

    const auto metres = sketch::format_quantity(quantity, Unit::metre);
    require(metres == "7493/2000 m", "metre display should be canonical and exact");
    require_exact(sketch::parse_quantity(metres), 7493, 2000, "metre display round trip");

    require(quantity.exact_metres == original.exact_metres,
            "formatting should not mutate exact value");
    require(quantity.original_expression == original.original_expression,
            "formatting should not mutate original expression");
    require_near(sketch::quantity_value(quantity, Unit::foot), 12.291666666666666, 1e-12,
                 "feet display value");
    require_near(sketch::quantity_value(quantity, Unit::centimetre), 374.65, 1e-12,
                 "centimetre display value");
}

}  // namespace

int main() {
    test_decimal_units_are_exact();
    test_feet_and_inches_preserve_exact_fraction();
    test_imperial_and_metric_entries_share_exact_geometry();
    test_invalid_and_overflowing_input_is_rejected();
    test_display_round_trip_is_exact_and_non_mutating();
    return 0;
}
