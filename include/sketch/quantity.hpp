#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sketch {

enum class Unit {
    metre,
    millimetre,
    centimetre,
    foot,
    inch,
};

inline constexpr Unit default_input_unit = Unit::foot;

struct ExactRational {
    std::int64_t numerator{};
    std::int64_t denominator{1};

    friend bool operator==(const ExactRational&, const ExactRational&) = default;
};

struct Quantity {
    double metres{};
    ExactRational exact_metres{};
    Unit entered_unit{default_input_unit};
    std::string original_expression;
};

// Parses exact decimal, scientific-decimal and fractional input. Supported suffixes are m, mm, cm,
// ft, foot, feet, in, inch, inches, apostrophe, and quote. A missing suffix uses
// default_unit. Feet plus explicit inches may use one architectural hyphen
// before unsigned inches. A leading sign applies to the entire expression.
[[nodiscard]] Quantity parse_quantity(
    std::string_view expression,
    Unit default_unit = default_input_unit);

// Captures an existing scalar as a bounded, parseable metre input without
// rounding it. Throws when no admitted int64 rational reproduces the scalar.
[[nodiscard]] Quantity quantity_from_metres(double metres);

[[nodiscard]] double quantity_value(const Quantity& quantity, Unit unit);

// Produces an exact, parseable representation without changing quantity.
[[nodiscard]] std::string format_quantity(const Quantity& quantity, Unit unit);

}  // namespace sketch
