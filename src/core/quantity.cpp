#include "sketch/quantity.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
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
    } else if (lowered.ends_with("in")) {
        suffix_length = 2;
        unit = Unit::inch;
    } else if (lowered.ends_with('"')) {
        suffix_length = 1;
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
        const auto apostrophe = lowered.find('\'');
        const auto feet_text = lowered.find("ft");
        if (apostrophe != std::string::npos && feet_text != std::string::npos) {
            throw std::invalid_argument("quantity contains multiple feet markers");
        }

        ExactRational exact_metres;
        Unit entered_unit = default_unit;
        const auto feet_marker = apostrophe != std::string::npos ? apostrophe : feet_text;
        if (feet_marker != std::string::npos) {
            const auto marker_length = apostrophe != std::string::npos ? std::size_t{1} : std::size_t{2};
            if (lowered.find(apostrophe != std::string::npos ? "'" : "ft",
                             feet_marker + marker_length) != std::string::npos) {
                throw std::invalid_argument("quantity contains multiple feet markers");
            }
            const auto feet = parse_unsigned_number(body.substr(0, feet_marker));
            auto remainder = trim(body.substr(feet_marker + marker_length));
            exact_metres = multiply(feet, unit_factor(Unit::foot));
            entered_unit = Unit::foot;

            if (!remainder.empty()) {
                const auto lowered_remainder = lowercase(remainder);
                std::size_t inch_marker_length = 0;
                if (lowered_remainder.ends_with("in")) {
                    inch_marker_length = 2;
                } else if (lowered_remainder.ends_with('"')) {
                    inch_marker_length = 1;
                } else {
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
