#pragma once

// Vertex measurement text: the one formatter for every displayed length, area
// and angle (canvas labels, dimension labels, Details, status-bar readouts,
// transform/size readouts, dialogs and report/PDF output).
//
// It grew out of PlanCanvas::drawingLengthText (which now delegates here) so
// non-canvas code can share the exact same rules without depending on the
// canvas. Rules follow docs/design/vertex-design-system/README.md "Units":
//
// Lengths
//   * Imperial is feet-inches with straight primes and a space, rounded to the
//     nearest 1/16 inch with reduced fractions: 24' 6", 39' 4 7/16", 6",
//     7/16", 0". Feet are omitted below one foot. Decimal feet are used only
//     where decimal mode is chosen (Preferences "Decimal feet", or
//     decimal_feet_text directly): 24.50 ft.
//   * Metric is always metres (never mm in a readout, so one readout never
//     mixes units), rounded to the millimetre with trailing zeros trimmed:
//     2.25 m, 0.95 m, 3 m, 1.235 m.
//   * The imperial style and rounding steps above are the defaults of the
//     process-wide DisplayPrecision (set from Preferences): feet-inches or
//     decimal feet (hundredths), 1/16, 1/8, 1/4, 1/2 or 1 inch, and 1, 5 or
//     10 mm. ANSI helpers and appraisal text never follow it (their standard
//     owns the rounding); appraisal code passes default_display_precision().
//   * Lengths have no thousands separator so that every displayed length is
//     also valid input for sketch::parse_quantity.
//
// The "≈" rule
//   The active display precision (1/16 inch, 1 mm by default; 0.01 ft in
//   decimal feet) already states how far the
//   shown value may be from the exact one: up to half a step. Rounding within
//   that precision is therefore not marked. "≈ " is shown only when the text
//   drops more than the display precision promises, i.e. when
//     (a) a nonzero value displays as zero (e.g. a 1/64" segment would read
//         0"; it reads ≈ 0" instead), or
//     (b) the value is too large for exact step arithmetic and falls back to
//         six significant digits.
//   Input-field text (length_input_text) never carries "≈" so it always parses.
//   Underlying values are never rounded; all of this is presentation only.
//
// Areas
//   * Imperial: lowercase "sf", thousands separator, one decimal: 1,842.5 sf.
//   * Metric: 171.2 m². Acres: 1.25 ac.
//   * A calculation profile's rounding (DisplayArea) is the reporting basis
//     and keeps its own decimal places; only grouping and the suffix are
//     applied (e.g. ANSI whole square feet: 1,843 sf).
//
// Angles: 45°, 22.5° (no space, up to one decimal, trailing zeros trimmed).
//
// ANSI Z765 appraisal readouts keep the standard's rounding basis: lengths in
// tenths of a foot (39.4 ft) and areas in whole square feet (1,843 sf). They
// are imperial by definition, so they are never followed by a metric
// parenthetical (one system per readout).

#include "sketch/calculations.hpp"

#include <QString>

#include <cmath>
#include <cstdint>
#include <numeric>

namespace sketch::desktop::units {

inline constexpr double metres_per_inch = 0.0254;
inline constexpr double metres_per_foot = 0.3048;
inline constexpr double square_metres_per_square_foot = 0.09290304;
inline const QString approximate_prefix = QStringLiteral("≈ ");

// Inserts "," between thousands of the integer part of a plain decimal
// number such as "-1234.5" -> "-1,234.5".
inline QString group_thousands(const QString& number) {
    int start = number.startsWith(QLatin1Char('-')) || number.startsWith(QLatin1Char('+')) ? 1 : 0;
    int end = number.indexOf(QLatin1Char('.'));
    if (end < 0) end = static_cast<int>(number.size());
    QString result = number;
    for (int position = end - 3; position > start; position -= 3)
        result.insert(position, QLatin1Char(','));
    return result;
}

// Fixed-point text without a negative zero ("-0.0" -> "0.0").
inline QString fixed_number(double value, int decimals) {
    auto text = QString::number(value, 'f', decimals);
    bool nonzero = false;
    for (const auto character : text)
        if (character.isDigit() && character != QLatin1Char('0')) { nonzero = true; break; }
    if (!nonzero && text.startsWith(QLatin1Char('-'))) text.remove(0, 1);
    return text;
}

inline QString trim_decimal_zeros(QString text) {
    if (!text.contains(QLatin1Char('.'))) return text;
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return text;
}

// Display rounding chosen in Preferences. Presentation only: stored values
// are never rounded and parsing is unaffected.
struct DisplayPrecision {
    bool decimal_feet{false};       // imperial as 24.50 ft instead of 24' 6"
    int imperial_denominator{16};   // feet-inches step: 1/N inch (16, 8, 4, 2, 1)
    double metric_step_mm{1.0};     // metric step in millimetres (1, 5, 10)

    bool operator==(const DisplayPrecision&) const = default;
};

[[nodiscard]] inline constexpr DisplayPrecision default_display_precision() noexcept { return {}; }

namespace detail {
inline DisplayPrecision& display_precision_storage() noexcept {
    static DisplayPrecision value{};
    return value;
}
} // namespace detail

// The active process-wide precision (GUI thread). Invalid steps fall back to
// the defaults (1/16 inch, 1 mm).
[[nodiscard]] inline DisplayPrecision display_precision() noexcept {
    return detail::display_precision_storage();
}

inline void set_display_precision(DisplayPrecision precision) noexcept {
    if (precision.imperial_denominator < 1 || precision.imperial_denominator > 64)
        precision.imperial_denominator = 16;
    if (!std::isfinite(precision.metric_step_mm) || precision.metric_step_mm <= 0.0)
        precision.metric_step_mm = 1.0;
    detail::display_precision_storage() = precision;
}

namespace detail {

inline QString decimal_feet_length(double metres, bool allow_approximate) {
    if (!std::isfinite(metres)) return QStringLiteral("—");
    const auto feet = metres / metres_per_foot;
    const auto hundredths = std::round(std::abs(feet) * 100.0);
    if (!std::isfinite(hundredths) || hundredths > 9e15) {
        return allow_approximate
            ? approximate_prefix + QStringLiteral("%1 ft").arg(feet, 0, 'g', 6)
            : QStringLiteral("%1 ft").arg(feet, 0, 'g', 17);
    }
    const auto prefix = allow_approximate && hundredths == 0.0 && feet != 0.0
        ? approximate_prefix : QString{};
    return prefix + fixed_number(feet, 2) + QStringLiteral(" ft");
}

inline QString imperial_length(double metres, bool allow_approximate,
                               const DisplayPrecision& precision = display_precision()) {
    if (precision.decimal_feet) return decimal_feet_length(metres, allow_approximate);
    if (!std::isfinite(metres)) return QStringLiteral("—");
    const std::int64_t denominator = precision.imperial_denominator;
    const auto inches = std::abs(metres) / metres_per_inch;
    const auto steps = std::round(inches * static_cast<double>(denominator));
    if (!std::isfinite(steps)) return QStringLiteral("—");
    const auto negative = metres < 0.0;
    if (steps > 9e15) {
        // Beyond exact step arithmetic: decimal feet, six significant digits.
        const auto feet = metres / metres_per_foot;
        return allow_approximate
            ? approximate_prefix + QStringLiteral("%1 ft").arg(feet, 0, 'g', 6)
            : QStringLiteral("%1 ft").arg(feet, 0, 'g', 17);
    }
    const auto ticks = static_cast<std::int64_t>(steps);
    const auto per_foot = 12 * denominator;
    const auto feet = ticks / per_foot;
    const auto whole = (ticks % per_foot) / denominator;
    const auto numerator = ticks % denominator;
    QString inch_text = whole || !numerator ? QString::number(whole) : QString{};
    if (numerator) {
        const auto divisor = std::gcd(numerator, denominator);
        if (!inch_text.isEmpty()) inch_text += QLatin1Char(' ');
        inch_text += QStringLiteral("%1/%2").arg(numerator / divisor).arg(denominator / divisor);
    }
    const auto sign = negative && ticks > 0 ? QStringLiteral("-") : QString{};
    const auto prefix = allow_approximate && ticks == 0 && inches > 0.0
        ? approximate_prefix : QString{};
    return feet > 0 ? QStringLiteral("%1%2%3' %4\"").arg(prefix, sign).arg(feet).arg(inch_text)
                    : QStringLiteral("%1%2%3\"").arg(prefix, sign, inch_text);
}

inline QString metric_length(double metres, bool allow_approximate,
                             const DisplayPrecision& precision = display_precision()) {
    if (!std::isfinite(metres)) return QStringLiteral("—");
    const auto step_mm = precision.metric_step_mm;
    const auto millimetres = std::round(std::round(std::abs(metres) * 1000.0 / step_mm) * step_mm);
    if (!std::isfinite(millimetres) || millimetres > 9e15) {
        return allow_approximate
            ? approximate_prefix + QStringLiteral("%1 m").arg(metres, 0, 'g', 6)
            : QStringLiteral("%1 m").arg(metres, 0, 'g', 17);
    }
    const auto ticks = static_cast<std::int64_t>(millimetres);
    auto number = QString::number(ticks / 1000);
    if (const auto fraction = ticks % 1000)
        // Trim the whole "N.fff" text: trim_decimal_zeros only trims text
        // that contains the decimal point (2.500 -> 2.5, 0.950 -> 0.95).
        number = trim_decimal_zeros(number + QLatin1Char('.') +
            QStringLiteral("%1").arg(fraction, 3, 10, QLatin1Char('0')));
    const auto sign = metres < 0.0 && ticks > 0 ? QStringLiteral("-") : QString{};
    const auto prefix = allow_approximate && ticks == 0 && metres != 0.0
        ? approximate_prefix : QString{};
    return QStringLiteral("%1%2%3 m").arg(prefix, sign, number);
}

} // namespace detail

// Displayed length (canvas, dimensions, Details, readouts, reports) at the
// active display precision, or at an explicit one (appraisal text passes
// default_display_precision() so Preferences never change its rounding).
inline QString length_text(double metres, bool metric, const DisplayPrecision& precision) {
    return metric ? detail::metric_length(metres, true, precision)
                  : detail::imperial_length(metres, true, precision);
}
inline QString length_text(double metres, bool metric) {
    return length_text(metres, metric, display_precision());
}

// Same text without "≈"; always accepted by sketch::parse_quantity.
inline QString length_input_text(double metres, bool metric, const DisplayPrecision& precision) {
    return metric ? detail::metric_length(metres, false, precision)
                  : detail::imperial_length(metres, false, precision);
}
inline QString length_input_text(double metres, bool metric) {
    return length_input_text(metres, metric, display_precision());
}

// Decimal-foot mode: 24.50 ft.
inline QString decimal_feet_text(double metres, int decimals = 2) {
    if (!std::isfinite(metres)) return QStringLiteral("—");
    return fixed_number(metres / metres_per_foot, decimals) + QStringLiteral(" ft");
}

inline QString area_unit_suffix(AreaUnit unit) {
    switch (unit) {
    case AreaUnit::square_metre: return QStringLiteral("m²");
    case AreaUnit::square_foot: return QStringLiteral("sf");
    case AreaUnit::acre: return QStringLiteral("ac");
    }
    return QStringLiteral("area");
}

inline QString area_unit_suffix(bool metric) {
    return area_unit_suffix(metric ? AreaUnit::square_metre : AreaUnit::square_foot);
}

// Area as a number with grouping (no suffix), in the active unit.
inline QString area_number(double square_metres, bool metric, int decimals = 1) {
    if (!std::isfinite(square_metres)) return QStringLiteral("—");
    return group_thousands(fixed_number(
        metric ? square_metres : square_metres / square_metres_per_square_foot, decimals));
}

// Displayed area: 1,842.5 sf / 171.2 m².
inline QString area_text(double square_metres, bool metric, int decimals = 1) {
    if (!std::isfinite(square_metres)) return QStringLiteral("—");
    return area_number(square_metres, metric, decimals) + QLatin1Char(' ') + area_unit_suffix(metric);
}

// A profile-rounded calculation value: keeps the profile's decimal places.
inline QString area_text(const DisplayArea& display) {
    return group_thousands(QString::fromStdString(display.text)) + QLatin1Char(' ') +
           area_unit_suffix(display.unit);
}

// Angles: 45°, 22.5°.
inline QString angle_text(double degrees, int max_decimals = 1) {
    if (!std::isfinite(degrees)) return QStringLiteral("—");
    return trim_decimal_zeros(fixed_number(degrees, max_decimals)) + QStringLiteral("°");
}

// ANSI Z765 appraisal basis: tenths of a foot, whole square feet.
inline QString ansi_length_text(double metres) { return decimal_feet_text(metres, 1); }
inline QString ansi_area_text(double square_metres) { return area_text(square_metres, false, 0); }

} // namespace sketch::desktop::units
