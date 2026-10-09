#include "sketch/quantity.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace sketch {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::uint64_t magnitude(std::int64_t value) {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

std::int64_t checked_multiply(std::int64_t left, std::int64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    const auto negative = (left < 0) != (right < 0);
    const auto left_magnitude = magnitude(left);
    const auto right_magnitude = magnitude(right);
    const auto positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto limit = negative ? positive_limit + 1U : positive_limit;
    if (left_magnitude > limit / right_magnitude) {
        throw std::overflow_error("rational multiplication overflow");
    }
    const auto product = left_magnitude * right_magnitude;
    if (!negative) {
        return static_cast<std::int64_t>(product);
    }
    if (product == positive_limit + 1U) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(product);
}

std::int64_t checked_add(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        throw std::overflow_error("rational addition overflow");
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        throw std::overflow_error("rational addition overflow");
    }
    return left + right;
}

ExactRational normalize(ExactRational value) {
    if (value.denominator <= 0) {
        throw std::invalid_argument("rational denominator must be positive");
    }
    if (value.numerator == 0) {
        return {0, 1};
    }
    const auto divisor = std::gcd(magnitude(value.numerator),
                                  static_cast<std::uint64_t>(value.denominator));
    value.numerator /= static_cast<std::int64_t>(divisor);
    value.denominator /= static_cast<std::int64_t>(divisor);
    return value;
}

ExactRational multiply(ExactRational left, ExactRational right) {
    left = normalize(left);
    right = normalize(right);
    const auto first_divisor = std::gcd(magnitude(left.numerator),
                                        static_cast<std::uint64_t>(right.denominator));
    const auto second_divisor = std::gcd(magnitude(right.numerator),
                                         static_cast<std::uint64_t>(left.denominator));
    left.numerator /= static_cast<std::int64_t>(first_divisor);
    right.denominator /= static_cast<std::int64_t>(first_divisor);
    right.numerator /= static_cast<std::int64_t>(second_divisor);
    left.denominator /= static_cast<std::int64_t>(second_divisor);
    return normalize({checked_multiply(left.numerator, right.numerator),
                      checked_multiply(left.denominator, right.denominator)});
}

ExactRational add(ExactRational left, ExactRational right) {
    left = normalize(left);
    right = normalize(right);
    const auto common = std::gcd(left.denominator, right.denominator);
    const auto left_factor = right.denominator / common;
    const auto right_factor = left.denominator / common;
    const auto numerator = checked_add(checked_multiply(left.numerator, left_factor),
                                       checked_multiply(right.numerator, right_factor));
    const auto denominator = checked_multiply(left.denominator, left_factor);
    return normalize({numerator, denominator});
}

ExactRational negate(ExactRational value) {
    if (value.numerator == std::numeric_limits<std::int64_t>::min()) {
        throw std::overflow_error("rational negation overflow");
    }
    value.numerator = -value.numerator;
    return value;
}

std::int64_t parse_digits(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("number requires digits");
    }
    std::int64_t value = 0;
    for (const auto character : text) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument("number contains an invalid character");
        }
        const auto digit = static_cast<std::int64_t>(character - '0');
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            throw std::overflow_error("integer input overflow");
        }
        value = value * 10 + digit;
    }
    return value;
}

ExactRational parse_fraction(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos || text.find('/', slash + 1) != std::string_view::npos) {
        throw std::invalid_argument("fraction requires one slash");
    }
    const auto numerator = parse_digits(trim(text.substr(0, slash)));
    const auto denominator = parse_digits(trim(text.substr(slash + 1)));
    if (denominator == 0) {
        throw std::invalid_argument("fraction denominator cannot be zero");
    }
    return normalize({numerator, denominator});
}

ExactRational parse_decimal(std::string_view text) {
    const auto exponent = text.find_first_of("eE");
    if (exponent != std::string_view::npos) {
        if (text.find_first_of("eE", exponent + 1) != std::string_view::npos)
            throw std::invalid_argument("decimal contains more than one exponent");
        auto power_text = text.substr(exponent + 1);
        bool negative_power = false;
        if (!power_text.empty() && (power_text.front() == '+' || power_text.front() == '-')) {
            negative_power = power_text.front() == '-';
            power_text.remove_prefix(1);
        }
        const auto power = parse_digits(power_text);
        if (power > 4096) throw std::invalid_argument("decimal exponent exceeds the supported range");
        const auto mantissa = text.substr(0, exponent);
        std::string coefficient;
        coefficient.reserve(mantissa.size());
        bool decimal_seen = false;
        std::size_t fractional_digits = 0;
        for (const auto character : mantissa) {
            if (character == '.') {
                if (decimal_seen) throw std::invalid_argument("decimal contains more than one point");
                decimal_seen = true;
            } else {
                if (character < '0' || character > '9')
                    throw std::invalid_argument("number contains an invalid character");
                coefficient.push_back(character);
                if (decimal_seen) ++fractional_digits;
            }
        }
        if (coefficient.empty()) throw std::invalid_argument("decimal requires digits");
        const auto first = coefficient.find_first_not_of('0');
        if (first == std::string::npos) return {0, 1};
        coefficient.erase(0, first);
        std::size_t trailing_zeros = 0;
        while (coefficient.back() == '0') {
            coefficient.pop_back();
            ++trailing_zeros;
        }
        // Apply the decimal point and exponent to the lexical coefficient first.
        // An oversized mantissa may reduce to a small, exactly representable value.
        if (fractional_digits > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
            trailing_zeros > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::overflow_error("decimal scale overflow");
        auto scale = checked_add(static_cast<std::int64_t>(trailing_zeros),
                                 -static_cast<std::int64_t>(fractional_digits));
        scale = checked_add(scale, negative_power ? -power : power);
        if (scale >= 0) {
            auto numerator = parse_digits(coefficient);
            for (std::int64_t index = 0; index < scale; ++index)
                numerator = checked_multiply(numerator, 10);
            return {numerator, 1};
        }
        if (scale == std::numeric_limits<std::int64_t>::min())
            throw std::overflow_error("decimal scale overflow");
        auto twos = -scale;
        auto fives = -scale;
        // With trailing zeroes removed, the coefficient cannot cancel both 2
        // and 5. Bound the uncancellable denominator before doing long division.
        const auto final_digit = coefficient.back() - '0';
        if ((final_digit % 5 != 0 && fives > 27) ||
            (final_digit % 2 != 0 && twos > 62))
            throw std::overflow_error("rational denominator overflow");
        const auto divide_coefficient = [&](int divisor) {
            int remainder = 0;
            for (auto& character : coefficient) {
                const auto digit = remainder * 10 + character - '0';
                character = static_cast<char>('0' + digit / divisor);
                remainder = digit % divisor;
            }
            const auto nonzero = coefficient.find_first_not_of('0');
            if (nonzero != 0) coefficient.erase(0, nonzero);
        };
        while (twos > 0 && (coefficient.back() - '0') % 2 == 0) {
            divide_coefficient(2);
            --twos;
        }
        while (fives > 0 && (coefficient.back() - '0') % 5 == 0) {
            divide_coefficient(5);
            --fives;
        }
        const auto numerator = parse_digits(coefficient);
        std::int64_t denominator = 1;
        while (twos-- > 0) denominator = checked_multiply(denominator, 2);
        while (fives-- > 0) denominator = checked_multiply(denominator, 5);
        return {numerator, denominator};
    }
    const auto decimal = text.find('.');
    if (decimal == std::string_view::npos) {
        return {parse_digits(text), 1};
    }
    if (text.find('.', decimal + 1) != std::string_view::npos) {
        throw std::invalid_argument("decimal contains more than one point");
    }
    const auto whole = text.substr(0, decimal);
    const auto fraction = text.substr(decimal + 1);
    if (whole.empty() && fraction.empty()) {
        throw std::invalid_argument("decimal requires digits");
    }
    if (!whole.empty()) {
        (void)parse_digits(whole);
    }
    if (!fraction.empty()) {
        (void)parse_digits(fraction);
    }

    auto significant_fraction = fraction;
    while (!significant_fraction.empty() && significant_fraction.back() == '0') {
        significant_fraction.remove_suffix(1);
    }
    if (significant_fraction.empty()) {
        return {whole.empty() ? 0 : parse_digits(whole), 1};
    }

    std::string combined;
    combined.reserve(whole.size() + significant_fraction.size());
    combined.append(whole.empty() ? "0" : whole);
    combined.append(significant_fraction);
    const auto numerator = parse_digits(combined);
    std::int64_t denominator = 1;
    for (std::size_t index = 0; index < significant_fraction.size(); ++index) {
        denominator = checked_multiply(denominator, 10);
    }
    return normalize({numerator, denominator});
}

ExactRational parse_unsigned_number(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        throw std::invalid_argument("quantity requires a number");
    }
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        return parse_decimal(text);
    }

    const auto whitespace = text.find_first_of(" \t\r\n");
    if (whitespace == std::string_view::npos) {
        return parse_fraction(text);
    }
    const auto whole_text = trim(text.substr(0, whitespace));
    const auto fraction_text = trim(text.substr(whitespace));
    if (whole_text.empty() || fraction_text.empty() ||
        fraction_text.find_first_of(" \t\r\n") != std::string_view::npos) {
        throw std::invalid_argument("mixed number has invalid spacing");
    }
    return add({parse_digits(whole_text), 1}, parse_fraction(fraction_text));
}

ExactRational unit_factor(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return {1, 1};
        case Unit::millimetre:
            return {1, 1000};
        case Unit::centimetre:
            return {1, 100};
        case Unit::foot:
            return {381, 1250};
        case Unit::inch:
            return {127, 5000};
    }
    throw std::invalid_argument("unknown unit");
}

std::string_view unit_suffix(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "m";
        case Unit::millimetre:
            return "mm";
        case Unit::centimetre:
            return "cm";
        case Unit::foot:
            return "ft";
        case Unit::inch:
            return "in";
    }
    throw std::invalid_argument("unknown unit");
}

struct NumberAndUnit {
    ExactRational number;
    Unit unit;
};

struct FeetMarker {
    std::size_t position{std::string_view::npos};
    std::size_t length{};
};

FeetMarker find_feet_marker(std::string_view lowered) {
    FeetMarker result;
    for (std::size_t index = 0; index < lowered.size(); ++index) {
        std::size_t length = 0;
        if (lowered[index] == '\'') {
            length = 1;
        } else {
            for (const auto alias : {std::string_view{"feet"}, std::string_view{"foot"},
                                     std::string_view{"ft"}}) {
                if (lowered.substr(index).starts_with(alias) &&
                    (index == 0 || std::isalpha(static_cast<unsigned char>(lowered[index - 1])) == 0) &&
                    (index + alias.size() == lowered.size() ||
                     std::isalpha(static_cast<unsigned char>(lowered[index + alias.size()])) == 0)) {
                    length = alias.size();
                    break;
                }
            }
        }
        if (length == 0) continue;
        if (result.position != std::string_view::npos) {
            throw std::invalid_argument("quantity contains multiple feet markers");
        }
        result = {index, length};
        index += length - 1;
    }
    return result;
}

std::size_t inch_suffix_length(std::string_view lowered) {
    for (const auto suffix : {std::string_view{"inches"}, std::string_view{"inch"},
                              std::string_view{"in"}, std::string_view{"\""}}) {
        if (lowered.ends_with(suffix)) return suffix.size();
    }
    return 0;
}

NumberAndUnit parse_simple(std::string_view body, Unit default_unit) {
    const auto lowered = lowercase(body);
    std::size_t suffix_length = 0;
    auto unit = default_unit;
    if (lowered.ends_with("mm")) {
        suffix_length = 2;
        unit = Unit::millimetre;
    } else if (lowered.ends_with("cm")) {
        suffix_length = 2;
        unit = Unit::centimetre;
    } else if (const auto inches = inch_suffix_length(lowered); inches != 0) {
        suffix_length = inches;
        unit = Unit::inch;
    } else if (lowered.ends_with('m')) {
        suffix_length = 1;
        unit = Unit::metre;
    }
    const auto number_text = trim(body.substr(0, body.size() - suffix_length));
    return {parse_unsigned_number(number_text), unit};
}

ExactRational in_unit(ExactRational metres, Unit unit) {
    const auto factor = unit_factor(unit);
    return multiply(metres, {factor.denominator, factor.numerator});
}

}  // namespace

Quantity parse_quantity(std::string_view expression, Unit default_unit) {
    try {
        const auto original = std::string(expression);
        auto body = trim(expression);
        if (body.empty()) {
            throw std::invalid_argument("quantity expression is empty");
        }

        bool negative = false;
        if (body.front() == '+' || body.front() == '-') {
            negative = body.front() == '-';
            body.remove_prefix(1);
            body = trim(body);
            if (body.empty()) {
                throw std::invalid_argument("quantity sign requires a number");
            }
        }

        const auto lowered = lowercase(body);
        const auto feet_marker = find_feet_marker(lowered);

        ExactRational exact_metres;
        Unit entered_unit = default_unit;
        if (feet_marker.position != std::string_view::npos) {
            const auto feet = parse_unsigned_number(body.substr(0, feet_marker.position));
            auto remainder = trim(body.substr(feet_marker.position + feet_marker.length));
            exact_metres = multiply(feet, unit_factor(Unit::foot));
            entered_unit = Unit::foot;

            if (!remainder.empty()) {
                const auto architectural_separator = remainder.front() == '-';
                if (architectural_separator) {
                    remainder.remove_prefix(1);
                    remainder = trim(remainder);
                }
                const auto lowered_remainder = lowercase(remainder);
                const auto inch_marker_length = inch_suffix_length(lowered_remainder);
                if (inch_marker_length == 0) {
                    throw std::invalid_argument("feet plus inches requires an inch marker");
                }
                const auto inches = parse_unsigned_number(
                    trim(remainder.substr(0, remainder.size() - inch_marker_length)));
                exact_metres = add(exact_metres, multiply(inches, unit_factor(Unit::inch)));
            }
        } else {
            const auto parsed = parse_simple(body, default_unit);
            exact_metres = multiply(parsed.number, unit_factor(parsed.unit));
            entered_unit = parsed.unit;
        }

        if (negative) {
            exact_metres = negate(exact_metres);
        }
        exact_metres = normalize(exact_metres);
        const auto metres = static_cast<double>(exact_metres.numerator) /
                            static_cast<double>(exact_metres.denominator);
        if (!std::isfinite(metres) || (exact_metres.numerator != 0 && metres == 0.0)) {
            throw std::invalid_argument("quantity is not representable as finite metres");
        }
        return {metres, exact_metres, entered_unit, original};
    } catch (const std::overflow_error&) {
        throw std::invalid_argument("quantity exceeds exact numeric range");
    }
}

double quantity_value(const Quantity& quantity, Unit unit) {
    const auto normalized = normalize(quantity.exact_metres);
    const auto factor = unit_factor(unit);
    const auto exact_value = static_cast<long double>(normalized.numerator) /
                             static_cast<long double>(normalized.denominator);
    const auto exact_factor = static_cast<long double>(factor.numerator) /
                              static_cast<long double>(factor.denominator);
    return static_cast<double>(exact_value / exact_factor);
}

std::string format_quantity(const Quantity& quantity, Unit unit) {
    const auto value = in_unit(quantity.exact_metres, unit);
    auto result = std::to_string(value.numerator);
    if (value.denominator != 1) {
        result += '/';
        result += std::to_string(value.denominator);
    }
    result += ' ';
    result += unit_suffix(unit);
    return result;
}

}  // namespace sketch
