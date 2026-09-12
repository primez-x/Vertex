#include "sketch/boundary_receipt.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr double full_turn = 2.0 * std::numbers::pi;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::string lower_and_trim(std::string_view value) {
    value = trim(value);
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

std::string format_double(double value) {
    char buffer[64]{};
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value,
                                      std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) invalid("angle cannot be formatted exactly");
    return std::string(buffer, result.ptr);
}

double parse_decimal_angle(std::string_view value) {
    value = trim(value);
    if (value.empty()) invalid("angle expression is empty");
    double parsed = 0.0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        invalid("angle expression must be a finite decimal or fraction");
    }
    return parsed;
}

double parse_unsigned_real(std::string_view value) {
    value = trim(value);
    if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
        invalid("angle fraction components must be unsigned");
    }
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) return parse_decimal_angle(value);
    if (value.find('/', slash + 1) != std::string_view::npos) {
        invalid("angle fraction contains more than one slash");
    }
    const auto numerator = parse_decimal_angle(value.substr(0, slash));
    const auto denominator = parse_decimal_angle(value.substr(slash + 1));
    if (!(denominator > 0.0)) invalid("angle fraction denominator must be positive");
    const auto result = numerator / denominator;
    if (!std::isfinite(result)) invalid("angle fraction is not finite");
    return result;
}

double parse_pi_angle(std::string_view value) {
    const auto compact = lower_and_trim(value);
    if (compact.empty()) invalid("angle expression is empty");

    auto expression = std::string_view(compact);
    double sign = 1.0;
    if (!expression.empty() && (expression.front() == '+' || expression.front() == '-')) {
        if (expression.front() == '-') sign = -1.0;
        expression.remove_prefix(1);
    }
    const auto pi_position = expression.find("pi");
    if (pi_position == std::string_view::npos) {
        return sign * parse_unsigned_real(expression);
    }
    if (expression.find("pi", pi_position + 2) != std::string_view::npos) {
        invalid("angle expression contains more than one pi term");
    }

    auto coefficient = trim(expression.substr(0, pi_position));
    if (!coefficient.empty() && coefficient.back() == '*') coefficient.remove_suffix(1);
    const double multiplier = coefficient.empty() ? 1.0 : parse_unsigned_real(coefficient);
    auto remainder = trim(expression.substr(pi_position + 2));
    double divisor = 1.0;
    if (!remainder.empty()) {
        if (remainder.front() != '/') invalid("angle pi expression has an unsupported suffix");
        remainder.remove_prefix(1);
        divisor = parse_unsigned_real(remainder);
        if (!(divisor > 0.0)) invalid("angle pi divisor must be positive");
    }
    const auto result = sign * multiplier * std::numbers::pi / divisor;
    if (!std::isfinite(result)) invalid("angle expression is not finite");
    return result;
}

double parse_angle_value(std::string_view expression) {
    expression = trim(expression);
    if (expression.empty()) invalid("angle expression is empty");
    auto compact = lower_and_trim(expression);
    bool degrees = false;
    if (compact.ends_with("deg")) {
        degrees = true;
        compact.resize(compact.size() - 3);
    } else if (compact.ends_with("rad")) {
        compact.resize(compact.size() - 3);
    }
    if (compact.empty()) invalid("angle suffix requires a value");
    const auto parsed = parse_pi_angle(compact);
    const auto radians = degrees ? parsed * std::numbers::pi / 180.0 : parsed;
    if (!std::isfinite(radians)) invalid("angle must be finite");
    return radians;
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return left.x == right.x && left.y == right.y;
}

bool finite_point(Vec2 point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool same_segment(const Segment& left, const Segment& right) noexcept {
    return same_point(left.start, right.start) && same_point(left.end, right.end) &&
           left.sweep_radians == right.sweep_radians;
}

bool same_quantity(const Quantity& left, const Quantity& right) noexcept {
    return left.metres == right.metres && left.exact_metres == right.exact_metres &&
           left.entered_unit == right.entered_unit &&
           left.original_expression == right.original_expression;
}

void require_point(Vec2 point, std::string_view label) {
    if (!finite_point(point)) invalid(std::string(label) + " must be finite");
}

void require_identifier(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must contain 1..128 supported ASCII characters");
    }
}

template <typename T>
void require_absent(const std::optional<T>& value, std::string_view label) {
    if (value.has_value()) invalid(std::string(label) + " is extraneous for this receipt kind");
}

template <typename T>
const T& require_present(const std::optional<T>& value, std::string_view label) {
    if (!value.has_value()) invalid(std::string(label) + " is required for this receipt kind");
    return *value;
}

void require_tolerance(double tolerance) {
    if (!std::isfinite(tolerance) || !(tolerance > 0.0)) {
        invalid("geometry tolerance must be finite and positive");
    }
}

double point_distance(Vec2 left, Vec2 right) {
    const auto result = std::hypot(left.x - right.x, left.y - right.y);
    if (!std::isfinite(result)) invalid("world point distance is not finite");
    return result;
}

Vec2 heading_vector(double radians) noexcept {
    const auto normalized = std::remainder(radians, full_turn);
    // Exact comparisons are intentionally narrower than a tolerance. A
    // nearby heading therefore retains the ordinary trigonometric result.
    if (normalized == 0.0) return {1.0, 0.0};
    if (normalized == std::numbers::pi / 2.0) return {0.0, 1.0};
    if (normalized == -std::numbers::pi / 2.0) return {0.0, -1.0};
    if (normalized == std::numbers::pi || normalized == -std::numbers::pi) {
        return {-1.0, 0.0};
    }
    return {std::cos(normalized), std::sin(normalized)};
}

struct ArcGeometry {
    Vec2 center;
    double radius{};
    double start_angle{};
};

ArcGeometry arc_geometry(const Segment& segment) {
    if (segment.sweep_radians == 0.0) invalid("line segment has no arc geometry");
    const auto chord_x = segment.end.x - segment.start.x;
    const auto chord_y = segment.end.y - segment.start.y;
    const auto chord = std::hypot(chord_x, chord_y);
    if (!std::isfinite(chord) || !(chord > 0.0)) invalid("arc chord is degenerate");
    const auto half_sweep = segment.sweep_radians / 2.0;
    const auto sine = std::sin(std::abs(half_sweep));
    const auto tangent = std::tan(half_sweep);
    if (!(sine > 0.0) || tangent == 0.0 || !std::isfinite(tangent)) {
        invalid("arc sweep cannot be represented");
    }
    const Vec2 midpoint{(segment.start.x + segment.end.x) / 2.0,
                        (segment.start.y + segment.end.y) / 2.0};
    const Vec2 left_normal{-chord_y / chord, chord_x / chord};
    const auto offset = chord / (2.0 * tangent);
    const Vec2 center{midpoint.x + left_normal.x * offset,
                      midpoint.y + left_normal.y * offset};
    const auto radius = chord / (2.0 * sine);
    if (!finite_point(center) || !std::isfinite(radius)) invalid("arc geometry is not finite");
    return {center, radius,
            std::atan2(segment.start.y - center.y, segment.start.x - center.x)};
}

double segment_end_tangent(const Segment& segment) {
    if (segment.sweep_radians == 0.0) {
        return std::atan2(segment.end.y - segment.start.y,
                          segment.end.x - segment.start.x);
    }
    const auto arc = arc_geometry(segment);
    const auto end_angle = arc.start_angle + segment.sweep_radians;
    return end_angle + (segment.sweep_radians > 0.0 ? std::numbers::pi / 2.0
                                                      : -std::numbers::pi / 2.0);
}

void validate_replayed_segment(const Segment& segment, double tolerance) {
    if (!finite_point(segment.start) || !finite_point(segment.end) ||
        !std::isfinite(segment.sweep_radians)) {
        invalid("replayed segment is not finite");
    }
    const auto length = segment_length(segment);
    const auto chord = point_distance(segment.start, segment.end);
    if (!std::isfinite(length) || !(length > tolerance) || !std::isfinite(chord) ||
        !(chord > tolerance)) {
        invalid("replayed segment is degenerate");
    }
}

std::string kind_name(BoundaryConstructionKind kind) {
    switch (kind) {
        case BoundaryConstructionKind::line_heading:
            return "line_heading";
        case BoundaryConstructionKind::line_rise_run:
            return "line_rise_run";
        case BoundaryConstructionKind::line_relative_turn:
            return "line_relative_turn";
        case BoundaryConstructionKind::line_closure:
            return "line_closure";
        case BoundaryConstructionKind::line_to_point:
            return "line_to_point";
        case BoundaryConstructionKind::arc_chord_angle:
            return "arc_chord_angle";
        case BoundaryConstructionKind::arc_chord_height:
            return "arc_chord_height";
        case BoundaryConstructionKind::arc_chord_length:
            return "arc_chord_length";
        case BoundaryConstructionKind::arc_start_tangent:
            return "arc_start_tangent";
    }
    invalid("unsupported construction receipt kind");
}

BoundaryConstructionKind kind_from_name(std::string_view value) {
    if (value == "line_heading") return BoundaryConstructionKind::line_heading;
    if (value == "line_rise_run") return BoundaryConstructionKind::line_rise_run;
    if (value == "line_relative_turn") return BoundaryConstructionKind::line_relative_turn;
    if (value == "line_closure") return BoundaryConstructionKind::line_closure;
    if (value == "line_to_point") return BoundaryConstructionKind::line_to_point;
    if (value == "arc_chord_angle") return BoundaryConstructionKind::arc_chord_angle;
    if (value == "arc_chord_height") return BoundaryConstructionKind::arc_chord_height;
    if (value == "arc_chord_length") return BoundaryConstructionKind::arc_chord_length;
    if (value == "arc_start_tangent") return BoundaryConstructionKind::arc_start_tangent;
    invalid("unsupported construction receipt kind");
}

std::string unit_name(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "metre";
        case Unit::millimetre:
            return "millimetre";
        case Unit::centimetre:
            return "centimetre";
        case Unit::foot:
            return "foot";
        case Unit::inch:
            return "inch";
    }
    invalid("unsupported quantity unit");
}

Unit unit_from_name(std::string_view value) {
    if (value == "metre") return Unit::metre;
    if (value == "millimetre") return Unit::millimetre;
    if (value == "centimetre") return Unit::centimetre;
    if (value == "foot") return Unit::foot;
    if (value == "inch") return Unit::inch;
    invalid("unsupported quantity unit");
}

void require_object(const Json& value, std::string_view label) {
    if (!value.is_object()) invalid(std::string(label) + " must be an object");
}

void require_array(const Json& value, std::string_view label) {
    if (!value.is_array()) invalid(std::string(label) + " must be an array");
}

void require_keys(const Json& value, std::initializer_list<std::string_view> required,
                 std::initializer_list<std::string_view> allowed, std::string_view label) {
    require_object(value, label);
    for (const auto key : required) {
        if (!value.contains(key)) {
            invalid(std::string(label) + " is missing " + std::string(key));
        }
    }
    for (const auto& item : value.items()) {
        const auto known = std::find(allowed.begin(), allowed.end(), item.key()) != allowed.end();
        if (!known) invalid(std::string(label) + " contains unknown field " + item.key());
    }
}

void require_required_keys(const Json& value, std::initializer_list<std::string_view> required,
                           std::string_view label) {
    require_object(value, label);
    for (const auto key : required) {
        if (!value.contains(key)) {
            invalid(std::string(label) + " is missing " + std::string(key));
        }
    }
}

double read_double(const Json& value, std::string_view label) {
    if (!value.is_number()) invalid(std::string(label) + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string(label) + " must be finite");
    return result;
}

std::uint64_t read_positive_uint(const Json& value, std::string_view label) {
    if (value.is_number_unsigned()) {
        const auto result = value.get<std::uint64_t>();
        if (result == 0) invalid(std::string(label) + " must be positive");
        return result;
    }
    if (!value.is_number_integer()) invalid(std::string(label) + " must be a positive integer");
    const auto result = value.get<std::int64_t>();
    if (result <= 0) invalid(std::string(label) + " must be positive");
    return static_cast<std::uint64_t>(result);
}

std::int64_t read_int64(const Json& value, std::string_view label) {
    if (value.is_number_integer()) {
        if (value.is_number_unsigned()) {
            const auto unsigned_value = value.get<std::uint64_t>();
            if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                invalid(std::string(label) + " exceeds signed integer range");
            }
            return static_cast<std::int64_t>(unsigned_value);
        }
        return value.get<std::int64_t>();
    }
    invalid(std::string(label) + " must be an integer");
}

std::string read_string(const Json& value, std::string_view label) {
    if (!value.is_string()) invalid(std::string(label) + " must be a string");
    return value.get<std::string>();
}

bool read_bool(const Json& value, std::string_view label) {
    if (!value.is_boolean()) invalid(std::string(label) + " must be boolean");
    return value.get<bool>();
}

Vec2 read_point(const Json& value, std::string_view label) {
    require_array(value, label);
    if (value.size() != 2) invalid(std::string(label) + " must contain exactly two coordinates");
    const Vec2 result{read_double(value[0], std::string(label) + " x"),
                      read_double(value[1], std::string(label) + " y")};
    require_point(result, label);
    return result;
}

Json write_point(Vec2 point, std::string_view label) {
    require_point(point, label);
    return Json::array({point.x, point.y});
}

Quantity read_quantity(const Json& value, std::string_view label) {
    require_keys(value, {"metres", "exact_metres", "entered_unit", "original_expression"},
                 {"metres", "exact_metres", "entered_unit", "original_expression"}, label);
    const auto metres = read_double(value.at("metres"), std::string(label) + " metres");
    const auto& exact = value.at("exact_metres");
    require_keys(exact, {"numerator", "denominator"}, {"numerator", "denominator"},
                 std::string(label) + " exact_metres");
    const ExactRational exact_metres{
        read_int64(exact.at("numerator"), std::string(label) + " numerator"),
        read_int64(exact.at("denominator"), std::string(label) + " denominator")};
    if (exact_metres.denominator <= 0) invalid(std::string(label) + " denominator must be positive");
    const auto entered_unit = unit_from_name(
        read_string(value.at("entered_unit"), std::string(label) + " entered_unit"));
    const auto original = read_string(value.at("original_expression"),
                                      std::string(label) + " original_expression");
    if (original.empty()) invalid(std::string(label) + " original_expression is empty");
    Quantity result{metres, exact_metres, entered_unit, original};
    const auto normalized = normalize_exact_quantity(result, label);
    if (!same_quantity(normalized, result)) {
        invalid(std::string(label) + " is not a canonical exact quantity");
    }
    return normalized;
}

Json write_quantity(const Quantity& input, std::string_view label) {
    const auto quantity = normalize_exact_quantity(input, label);
    return Json{{"metres", quantity.metres},
                {"exact_metres", Json{{"numerator", quantity.exact_metres.numerator},
                                        {"denominator", quantity.exact_metres.denominator}}},
                {"entered_unit", unit_name(quantity.entered_unit)},
                {"original_expression", quantity.original_expression}};
}

AngleInput read_angle(const Json& value, std::string_view label) {
    require_keys(value, {"radians", "original_expression", "normalized_expression"},
                 {"radians", "original_expression", "normalized_expression"}, label);
    const auto radians = read_double(value.at("radians"), std::string(label) + " radians");
    const auto original = read_string(value.at("original_expression"),
                                      std::string(label) + " original_expression");
    const auto normalized = read_string(value.at("normalized_expression"),
                                        std::string(label) + " normalized_expression");
    if (original.empty() || normalized.empty()) invalid(std::string(label) + " expressions are required");
    const AngleInput input{radians, original, normalized};
    const auto result = normalize_exact_angle(input, label);
    if (!(result == input)) invalid(std::string(label) + " is not a canonical exact angle");
    return result;
}

Json write_angle(const AngleInput& input, std::string_view label) {
    const auto angle = normalize_exact_angle(input, label);
    if (angle.original_expression.empty() || angle.normalized_expression.empty()) {
        invalid(std::string(label) + " expressions are required");
    }
    return Json{{"radians", angle.radians},
                {"original_expression", angle.original_expression},
                {"normalized_expression", angle.normalized_expression}};
}

void require_receipt_key_shape(const Json& value, BoundaryConstructionKind kind) {
    switch (kind) {
        case BoundaryConstructionKind::line_heading:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "distance", "heading"},
                         {"segment_id", "kind", "start", "clockwise", "distance", "heading"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::line_rise_run:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "rise", "run"},
                         {"segment_id", "kind", "start", "clockwise", "rise", "run"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::line_relative_turn:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "distance", "turn"},
                         {"segment_id", "kind", "start", "clockwise", "distance", "turn"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::line_closure:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "closure_delta"},
                         {"segment_id", "kind", "start", "clockwise", "closure_delta"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::line_to_point:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "chord_end"},
                         {"segment_id", "kind", "start", "clockwise", "chord_end"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::arc_chord_angle:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "chord_end", "angle"},
                         {"segment_id", "kind", "start", "clockwise", "chord_end", "angle"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::arc_chord_height:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "chord_end", "height"},
                         {"segment_id", "kind", "start", "clockwise", "chord_end", "height"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::arc_chord_length:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "chord_end", "arc_length"},
                         {"segment_id", "kind", "start", "clockwise", "chord_end", "arc_length"},
                         "construction receipt");
            return;
        case BoundaryConstructionKind::arc_start_tangent:
            require_keys(value, {"segment_id", "kind", "start", "clockwise", "tangent", "arc_length", "sweep"},
                         {"segment_id", "kind", "start", "clockwise", "tangent", "arc_length", "sweep"},
                         "construction receipt");
            return;
    }
    invalid("unsupported construction receipt kind");
}

ConstructionReceipt read_receipt(const Json& value) {
    require_object(value, "construction receipt");
    if (!value.contains("kind")) invalid("construction receipt is missing kind");
    const auto kind = kind_from_name(
        read_string(value.at("kind"), "construction receipt kind"));
    require_receipt_key_shape(value, kind);

    ConstructionReceipt result;
    result.segment_id = read_string(value.at("segment_id"), "construction receipt segment_id");
    require_identifier(result.segment_id, "construction receipt segment_id");
    result.kind = kind;
    result.start = read_point(value.at("start"), "construction receipt start");
    result.clockwise = read_bool(value.at("clockwise"), "construction receipt clockwise");
    switch (kind) {
        case BoundaryConstructionKind::line_heading:
            result.distance = read_quantity(value.at("distance"), "line heading distance");
            result.heading = read_angle(value.at("heading"), "line heading angle");
            break;
        case BoundaryConstructionKind::line_rise_run:
            result.rise = read_quantity(value.at("rise"), "line rise");
            result.run = read_quantity(value.at("run"), "line run");
            break;
        case BoundaryConstructionKind::line_relative_turn:
            result.distance = read_quantity(value.at("distance"), "relative line distance");
            result.turn = read_angle(value.at("turn"), "relative line turn");
            break;
        case BoundaryConstructionKind::line_closure:
            result.closure_delta = read_point(value.at("closure_delta"), "line closure delta");
            break;
        case BoundaryConstructionKind::line_to_point:
            result.chord_end = read_point(value.at("chord_end"), "line endpoint");
            break;
        case BoundaryConstructionKind::arc_chord_angle:
            result.chord_end = read_point(value.at("chord_end"), "chord angle endpoint");
            result.angle = read_angle(value.at("angle"), "chord angle sweep");
            break;
        case BoundaryConstructionKind::arc_chord_height:
            result.chord_end = read_point(value.at("chord_end"), "chord height endpoint");
            result.height = read_quantity(value.at("height"), "chord height");
            break;
        case BoundaryConstructionKind::arc_chord_length:
            result.chord_end = read_point(value.at("chord_end"), "chord length endpoint");
            result.arc_length = read_quantity(value.at("arc_length"), "chord arc length");
            break;
        case BoundaryConstructionKind::arc_start_tangent:
            result.tangent = read_angle(value.at("tangent"), "start tangent");
            result.arc_length = read_quantity(value.at("arc_length"), "start tangent arc length");
            result.sweep = read_angle(value.at("sweep"), "start tangent sweep");
            break;
    }
    return result;
}

Json write_receipt(const ConstructionReceipt& receipt) {
    Json result{{"segment_id", receipt.segment_id},
                {"kind", kind_name(receipt.kind)},
                {"start", write_point(receipt.start, "construction receipt start")},
                {"clockwise", receipt.clockwise}};
    switch (receipt.kind) {
        case BoundaryConstructionKind::line_heading:
            result["distance"] = write_quantity(
                require_present(receipt.distance, "line heading distance"), "line heading distance");
            result["heading"] = write_angle(
                require_present(receipt.heading, "line heading angle"), "line heading angle");
            break;
        case BoundaryConstructionKind::line_rise_run:
            result["rise"] = write_quantity(require_present(receipt.rise, "line rise"), "line rise");
            result["run"] = write_quantity(require_present(receipt.run, "line run"), "line run");
            break;
        case BoundaryConstructionKind::line_relative_turn:
            result["distance"] = write_quantity(
                require_present(receipt.distance, "relative line distance"), "relative line distance");
            result["turn"] = write_angle(require_present(receipt.turn, "relative line turn"),
                                          "relative line turn");
            break;
        case BoundaryConstructionKind::line_closure:
            result["closure_delta"] = write_point(
                require_present(receipt.closure_delta, "line closure delta"), "line closure delta");
            break;
        case BoundaryConstructionKind::line_to_point:
            result["chord_end"] = write_point(require_present(receipt.chord_end, "line endpoint"),
                                               "line endpoint");
            break;
        case BoundaryConstructionKind::arc_chord_angle:
            result["chord_end"] = write_point(
                require_present(receipt.chord_end, "chord angle endpoint"), "chord angle endpoint");
            result["angle"] = write_angle(require_present(receipt.angle, "chord angle sweep"),
                                           "chord angle sweep");
            break;
        case BoundaryConstructionKind::arc_chord_height:
            result["chord_end"] = write_point(
                require_present(receipt.chord_end, "chord height endpoint"), "chord height endpoint");
            result["height"] = write_quantity(require_present(receipt.height, "chord height"),
                                               "chord height");
            break;
        case BoundaryConstructionKind::arc_chord_length:
            result["chord_end"] = write_point(
                require_present(receipt.chord_end, "chord length endpoint"), "chord length endpoint");
            result["arc_length"] = write_quantity(
                require_present(receipt.arc_length, "chord arc length"), "chord arc length");
            break;
        case BoundaryConstructionKind::arc_start_tangent:
            result["tangent"] = write_angle(require_present(receipt.tangent, "start tangent"),
                                             "start tangent");
            result["arc_length"] = write_quantity(
                require_present(receipt.arc_length, "start tangent arc length"),
                "start tangent arc length");
            result["sweep"] = write_angle(require_present(receipt.sweep, "start tangent sweep"),
                                           "start tangent sweep");
            break;
    }
    return result;
}

}  // namespace

ConstructionReceipt decode_construction_receipt(const nlohmann::json& value) {
    return read_receipt(value);
}

nlohmann::json encode_construction_receipt(const ConstructionReceipt& receipt) {
    auto encoded = write_receipt(receipt);
    if (!(read_receipt(encoded) == receipt)) {
        invalid("construction receipt cannot be encoded without changing its inputs");
    }
    return encoded;
}

AngleInput AngleInput::from_radians(double radians) {
    if (!std::isfinite(radians)) invalid("angle must be finite");
    const auto expression = format_double(radians);
    return {radians, expression, expression};
}

AngleInput AngleInput::parse(std::string_view expression) {
    const auto radians = parse_angle_value(expression);
    return {radians, std::string(expression), format_double(radians)};
}

AngleInput angle_from_radians(double radians) {
    return AngleInput::from_radians(radians);
}

AngleInput parse_angle(std::string_view expression) {
    return AngleInput::parse(expression);
}

Quantity normalize_exact_quantity(const Quantity& input, std::string_view label) {
    Quantity parsed;
    try {
        parsed = parse_quantity(input.original_expression, input.entered_unit);
    } catch (const std::exception&) {
        invalid(std::string(label) + " is not exactly parseable");
    }
    if (!same_quantity(parsed, input) || !std::isfinite(parsed.metres)) {
        invalid(std::string(label) + " is internally inconsistent");
    }
    return parsed;
}

AngleInput normalize_exact_angle(const AngleInput& input, std::string_view label) {
    if (!std::isfinite(input.radians)) invalid(std::string(label) + " must be finite");
    AngleInput result = input;
    const auto parse_and_match = [&](std::string_view expression) {
        double parsed = 0.0;
        try {
            parsed = parse_angle_value(expression);
        } catch (const std::invalid_argument&) {
            invalid(std::string(label) + " has a malformed exact expression");
        }
        if (parsed != input.radians) {
            invalid(std::string(label) + " expression does not match its radians value");
        }
    };
    if (!input.original_expression.empty()) {
        parse_and_match(input.original_expression);
        result.original_expression = input.original_expression;
    } else {
        result.original_expression = format_double(input.radians);
    }
    if (!input.normalized_expression.empty()) parse_and_match(input.normalized_expression);
    result.normalized_expression = format_double(input.radians);
    return result;
}

bool ConstructionReceipt::operator==(const ConstructionReceipt& other) const noexcept {
    const auto equal_quantity = [](const std::optional<Quantity>& left,
                                   const std::optional<Quantity>& right) {
        if (left.has_value() != right.has_value()) return false;
        return !left.has_value() || same_quantity(*left, *right);
    };
    const auto equal_point = [](const std::optional<Vec2>& left,
                                const std::optional<Vec2>& right) {
        if (left.has_value() != right.has_value()) return false;
        return !left.has_value() || same_point(*left, *right);
    };
    return segment_id == other.segment_id && kind == other.kind && same_point(start, other.start) &&
           equal_point(chord_end, other.chord_end) && equal_quantity(distance, other.distance) &&
           heading == other.heading && equal_quantity(rise, other.rise) &&
           equal_quantity(run, other.run) && turn == other.turn && angle == other.angle &&
           equal_quantity(height, other.height) && equal_quantity(arc_length, other.arc_length) &&
           tangent == other.tangent && sweep == other.sweep &&
           equal_point(closure_delta, other.closure_delta) && clockwise == other.clockwise;
}

bool ReplayedConstructionReceipt::operator==(const ReplayedConstructionReceipt& other) const noexcept {
    return same_segment(segment, other.segment) && receipt == other.receipt;
}

bool BoundaryConstructionRecord::operator==(const BoundaryConstructionRecord& other) const noexcept {
    return schema_version == other.schema_version && replay_version == other.replay_version &&
           same_point(anchor, other.anchor) && boundary_id == other.boundary_id &&
           edges == other.edges && extensions == other.extensions;
}

ReplayedConstructionReceipt replay_construction_receipt(
    const ConstructionReceipt& receipt, const ConstructionReplayContext& context) {
    require_tolerance(context.tolerance_metres);
    require_point(context.expected_start, "expected construction start");
    require_point(receipt.start, "receipt captured start");
    if (!same_point(receipt.start, context.expected_start)) {
        invalid("receipt captured start does not match the analytical join");
    }
    if (receipt.kind != BoundaryConstructionKind::line_relative_turn &&
        context.previous_segment.has_value()) {
        invalid("previous segment context is only valid for a relative turn");
    }
    if (receipt.kind != BoundaryConstructionKind::line_closure &&
        context.closure_anchor.has_value()) {
        invalid("closure anchor context is only valid for a closure receipt");
    }

    const auto tolerance = context.tolerance_metres;
    Segment rebuilt{};
    ConstructionReceipt canonical{};
    canonical.segment_id = receipt.segment_id;
    canonical.kind = receipt.kind;
    canonical.start = context.expected_start;
    switch (receipt.kind) {
        case BoundaryConstructionKind::line_heading: {
            const auto& distance = require_present(receipt.distance, "line heading distance");
            const auto& heading = require_present(receipt.heading, "line heading angle");
            const auto normalized_distance = normalize_exact_quantity(distance, "line heading distance");
            const auto normalized_heading = normalize_exact_angle(heading, "line heading angle");
            require_absent(receipt.chord_end, "line heading chord endpoint");
            require_absent(receipt.rise, "line heading rise");
            require_absent(receipt.run, "line heading run");
            require_absent(receipt.turn, "line heading turn");
            require_absent(receipt.angle, "line heading angle alias");
            require_absent(receipt.height, "line heading height");
            require_absent(receipt.arc_length, "line heading arc length");
            require_absent(receipt.tangent, "line heading tangent");
            require_absent(receipt.sweep, "line heading sweep");
            require_absent(receipt.closure_delta, "line heading closure delta");
            if (receipt.clockwise) invalid("line heading cannot be clockwise");
            if (!(normalized_distance.metres > tolerance)) invalid("line heading distance is degenerate");
            const auto direction = heading_vector(normalized_heading.radians);
            rebuilt = {context.expected_start,
                       {context.expected_start.x + normalized_distance.metres * direction.x,
                        context.expected_start.y + normalized_distance.metres * direction.y},
                       0.0};
            canonical.distance = normalized_distance;
            canonical.heading = normalized_heading;
            break;
        }
        case BoundaryConstructionKind::line_rise_run: {
            const auto& rise = require_present(receipt.rise, "line rise");
            const auto& run = require_present(receipt.run, "line run");
            const auto normalized_rise = normalize_exact_quantity(rise, "line rise");
            const auto normalized_run = normalize_exact_quantity(run, "line run");
            require_absent(receipt.chord_end, "rise/run chord endpoint");
            require_absent(receipt.distance, "rise/run distance");
            require_absent(receipt.heading, "rise/run heading");
            require_absent(receipt.turn, "rise/run turn");
            require_absent(receipt.angle, "rise/run angle");
            require_absent(receipt.height, "rise/run height");
            require_absent(receipt.arc_length, "rise/run arc length");
            require_absent(receipt.tangent, "rise/run tangent");
            require_absent(receipt.sweep, "rise/run sweep");
            require_absent(receipt.closure_delta, "rise/run closure delta");
            if (receipt.clockwise) invalid("rise/run line cannot be clockwise");
            if (!(std::hypot(normalized_rise.metres, normalized_run.metres) > tolerance)) {
                invalid("line rise and run are degenerate");
            }
            rebuilt = {context.expected_start,
                       {context.expected_start.x + normalized_run.metres,
                        context.expected_start.y + normalized_rise.metres},
                       0.0};
            canonical.rise = normalized_rise;
            canonical.run = normalized_run;
            break;
        }
        case BoundaryConstructionKind::line_relative_turn: {
            const auto& distance = require_present(receipt.distance, "relative line distance");
            const auto& turn = require_present(receipt.turn, "relative line turn");
            const auto normalized_distance = normalize_exact_quantity(distance, "relative line distance");
            const auto normalized_turn = normalize_exact_angle(turn, "relative line turn");
            require_absent(receipt.chord_end, "relative line chord endpoint");
            require_absent(receipt.heading, "relative line heading");
            require_absent(receipt.rise, "relative line rise");
            require_absent(receipt.run, "relative line run");
            require_absent(receipt.angle, "relative line angle");
            require_absent(receipt.height, "relative line height");
            require_absent(receipt.arc_length, "relative line arc length");
            require_absent(receipt.tangent, "relative line tangent");
            require_absent(receipt.sweep, "relative line sweep");
            require_absent(receipt.closure_delta, "relative line closure delta");
            if (receipt.clockwise) invalid("relative line cannot be clockwise");
            const auto& previous = require_present(context.previous_segment,
                                                   "relative turn previous segment");
            validate_replayed_segment(previous, tolerance);
            if (!same_point(previous.end, context.expected_start)) {
                invalid("relative turn previous segment does not join the current start");
            }
            if (!(normalized_distance.metres > tolerance)) {
                invalid("relative line distance is degenerate");
            }
            const auto heading = segment_end_tangent(previous) + normalized_turn.radians;
            const auto direction = heading_vector(heading);
            rebuilt = {context.expected_start,
                       {context.expected_start.x + normalized_distance.metres * direction.x,
                        context.expected_start.y + normalized_distance.metres * direction.y},
                       0.0};
            canonical.distance = normalized_distance;
            canonical.turn = normalized_turn;
            break;
        }
        case BoundaryConstructionKind::line_closure: {
            const auto& delta = require_present(receipt.closure_delta, "line closure delta");
            const auto& anchor = require_present(context.closure_anchor, "line closure anchor");
            require_point(anchor, "line closure anchor");
            require_absent(receipt.chord_end, "line closure chord endpoint");
            require_absent(receipt.distance, "line closure distance");
            require_absent(receipt.heading, "line closure heading");
            require_absent(receipt.rise, "line closure rise");
            require_absent(receipt.run, "line closure run");
            require_absent(receipt.turn, "line closure turn");
            require_absent(receipt.angle, "line closure angle");
            require_absent(receipt.height, "line closure height");
            require_absent(receipt.arc_length, "line closure arc length");
            require_absent(receipt.tangent, "line closure tangent");
            require_absent(receipt.sweep, "line closure sweep");
            if (receipt.clockwise) invalid("line closure cannot be clockwise");
            require_point(delta, "line closure delta");
            const Vec2 expected_delta{anchor.x - context.expected_start.x,
                                      anchor.y - context.expected_start.y};
            if (!same_point(delta, expected_delta)) {
                invalid("line closure delta does not match the captured anchor");
            }
            rebuilt = {context.expected_start, anchor, 0.0};
            canonical.closure_delta = expected_delta;
            break;
        }
        case BoundaryConstructionKind::line_to_point: {
            const auto& end = require_present(receipt.chord_end, "line endpoint");
            require_absent(receipt.distance, "point line distance");
            require_absent(receipt.heading, "point line heading");
            require_absent(receipt.rise, "point line rise");
            require_absent(receipt.run, "point line run");
            require_absent(receipt.turn, "point line turn");
            require_absent(receipt.angle, "point line angle");
            require_absent(receipt.height, "point line height");
            require_absent(receipt.arc_length, "point line arc length");
            require_absent(receipt.tangent, "point line tangent");
            require_absent(receipt.sweep, "point line sweep");
            require_absent(receipt.closure_delta, "point line closure delta");
            if (receipt.clockwise) invalid("point line cannot be clockwise");
            require_point(end, "line endpoint");
            rebuilt = {context.expected_start, end, 0.0};
            canonical.chord_end = end;
            break;
        }
        case BoundaryConstructionKind::arc_chord_angle: {
            const auto& end = require_present(receipt.chord_end, "chord angle endpoint");
            const auto& angle = require_present(receipt.angle, "chord angle sweep");
            const auto normalized_angle = normalize_exact_angle(angle, "chord angle sweep");
            require_absent(receipt.distance, "chord angle distance");
            require_absent(receipt.heading, "chord angle heading");
            require_absent(receipt.rise, "chord angle rise");
            require_absent(receipt.run, "chord angle run");
            require_absent(receipt.turn, "chord angle turn");
            require_absent(receipt.height, "chord angle height");
            require_absent(receipt.arc_length, "chord angle arc length");
            require_absent(receipt.tangent, "chord angle tangent");
            require_absent(receipt.sweep, "chord angle sweep alias");
            require_absent(receipt.closure_delta, "chord angle closure delta");
            if (receipt.clockwise) invalid("chord angle cannot be clockwise");
            require_point(end, "chord angle endpoint");
            rebuilt = arc_from_chord_angle(context.expected_start, end, normalized_angle.radians);
            canonical.chord_end = end;
            canonical.angle = normalized_angle;
            break;
        }
        case BoundaryConstructionKind::arc_chord_height: {
            const auto& end = require_present(receipt.chord_end, "chord height endpoint");
            const auto& height = require_present(receipt.height, "chord height");
            const auto normalized_height = normalize_exact_quantity(height, "chord height");
            require_absent(receipt.distance, "chord height distance");
            require_absent(receipt.heading, "chord height heading");
            require_absent(receipt.rise, "chord height rise");
            require_absent(receipt.run, "chord height run");
            require_absent(receipt.turn, "chord height turn");
            require_absent(receipt.angle, "chord height angle");
            require_absent(receipt.arc_length, "chord height arc length");
            require_absent(receipt.tangent, "chord height tangent");
            require_absent(receipt.sweep, "chord height sweep");
            require_absent(receipt.closure_delta, "chord height closure delta");
            if (receipt.clockwise) invalid("chord height cannot be clockwise");
            require_point(end, "chord height endpoint");
            rebuilt = arc_from_chord_height(context.expected_start, end,
                                             normalized_height.metres);
            canonical.chord_end = end;
            canonical.height = normalized_height;
            break;
        }
        case BoundaryConstructionKind::arc_chord_length: {
            const auto& end = require_present(receipt.chord_end, "chord length endpoint");
            const auto& length = require_present(receipt.arc_length, "chord arc length");
            const auto normalized_length = normalize_exact_quantity(length, "chord arc length");
            require_absent(receipt.distance, "chord length distance");
            require_absent(receipt.heading, "chord length heading");
            require_absent(receipt.rise, "chord length rise");
            require_absent(receipt.run, "chord length run");
            require_absent(receipt.turn, "chord length turn");
            require_absent(receipt.angle, "chord length angle");
            require_absent(receipt.height, "chord length height");
            require_absent(receipt.tangent, "chord length tangent");
            require_absent(receipt.sweep, "chord length sweep");
            require_absent(receipt.closure_delta, "chord length closure delta");
            require_point(end, "chord length endpoint");
            rebuilt = arc_from_chord_arc_length(context.expected_start, end,
                                                normalized_length.metres,
                                                receipt.clockwise);
            canonical.chord_end = end;
            canonical.arc_length = normalized_length;
            canonical.clockwise = receipt.clockwise;
            break;
        }
        case BoundaryConstructionKind::arc_start_tangent: {
            const auto& tangent = require_present(receipt.tangent, "start tangent");
            const auto& length = require_present(receipt.arc_length, "start tangent arc length");
            const auto& sweep = require_present(receipt.sweep, "start tangent sweep");
            const auto normalized_tangent = normalize_exact_angle(tangent, "start tangent");
            const auto normalized_length = normalize_exact_quantity(length, "start tangent arc length");
            const auto normalized_sweep = normalize_exact_angle(sweep, "start tangent sweep");
            require_absent(receipt.chord_end, "start tangent chord endpoint");
            require_absent(receipt.distance, "start tangent distance");
            require_absent(receipt.heading, "start tangent heading");
            require_absent(receipt.rise, "start tangent rise");
            require_absent(receipt.run, "start tangent run");
            require_absent(receipt.turn, "start tangent turn");
            require_absent(receipt.angle, "start tangent angle");
            require_absent(receipt.height, "start tangent height");
            require_absent(receipt.closure_delta, "start tangent closure delta");
            if (receipt.clockwise) invalid("start tangent cannot be clockwise");
            rebuilt = arc_from_start_tangent(context.expected_start, normalized_tangent.radians,
                                             normalized_length.metres, normalized_sweep.radians);
            canonical.tangent = normalized_tangent;
            canonical.arc_length = normalized_length;
            canonical.sweep = normalized_sweep;
            break;
        }
        default:
            invalid("unsupported construction receipt kind");
    }

    validate_replayed_segment(rebuilt, tolerance);
    return {rebuilt, std::move(canonical)};
}

bool ReplayedConstructionEdge::operator==(const ReplayedConstructionEdge& other) const noexcept {
    return segment_id == other.segment_id && start_vertex_id == other.start_vertex_id &&
           end_vertex_id == other.end_vertex_id && same_segment(segment, other.segment);
}

bool BoundaryConstructionReplayResult::operator==(
    const BoundaryConstructionReplayResult& other) const noexcept {
    return replay_version == other.replay_version && same_point(anchor, other.anchor) &&
           boundary_id == other.boundary_id && edges == other.edges && receipts == other.receipts;
}

BoundaryConstructionRecord translated_boundary_construction(
    const BoundaryConstructionRecord& record, Vec2 offset,
    const std::map<std::string, std::string, std::less<>>& identity_map) {
    (void)replay_boundary_construction(record);
    require_point(offset, "boundary translation offset");
    auto result = record;
    const auto translate = [offset](Vec2 point) {
        const Vec2 translated{point.x + offset.x, point.y + offset.y};
        require_point(translated, "translated boundary point");
        return translated;
    };
    const auto remap = [&identity_map](std::string& identity) {
        if (const auto found = identity_map.find(identity); found != identity_map.end()) {
            identity = found->second;
        }
    };
    result.anchor = translate(result.anchor);
    remap(result.boundary_id);
    for (auto& edge : result.edges) {
        remap(edge.segment_id);
        remap(edge.start_vertex_id);
        remap(edge.end_vertex_id);
        remap(edge.receipt.segment_id);
        edge.receipt.start = translate(edge.receipt.start);
        if (edge.receipt.chord_end) {
            edge.receipt.chord_end = translate(*edge.receipt.chord_end);
        }
    }
    (void)replay_boundary_construction(result);
    return result;
}

BoundaryConstructionReplayResult replay_boundary_construction(
    const BoundaryConstructionRecord& record, double tolerance_metres) {
    require_tolerance(tolerance_metres);
    if (record.schema_version != boundary_receipt_schema_version_v1 &&
        record.schema_version != boundary_receipt_schema_version_v2) {
        invalid("unsupported boundary receipt schema version");
    }
    if (record.replay_version != boundary_receipt_replay_version) {
        invalid("unsupported boundary receipt replay version");
    }
    require_point(record.anchor, "boundary construction anchor");
    require_identifier(record.boundary_id, "boundary construction boundary_id");
    if (!record.extensions.is_object()) invalid("boundary receipt extensions must be an object");
    if (record.edges.empty()) invalid("boundary construction has no segments");

    std::set<std::string, std::less<>> segment_ids;
    std::set<std::string, std::less<>> start_vertex_ids;
    std::vector<ReplayedConstructionEdge> edges;
    std::vector<ConstructionReceipt> receipts;
    edges.reserve(record.edges.size());
    receipts.reserve(record.edges.size());
    Vec2 expected_start = record.anchor;
    for (std::size_t index = 0; index < record.edges.size(); ++index) {
        const auto& edge = record.edges[index];
        require_identifier(edge.segment_id, "construction segment_id");
        require_identifier(edge.start_vertex_id, "construction start_vertex_id");
        require_identifier(edge.end_vertex_id, "construction end_vertex_id");
        if (!segment_ids.insert(edge.segment_id).second) {
            invalid("boundary construction contains duplicate segment identity");
        }
        if (!start_vertex_ids.insert(edge.start_vertex_id).second) {
            invalid("boundary construction revisits a start vertex identity");
        }
        if (edge.receipt.segment_id != edge.segment_id) {
            invalid("receipt segment identity does not match construction topology");
        }
        if (record.schema_version == boundary_receipt_schema_version_v1 &&
            edge.receipt.kind == BoundaryConstructionKind::line_to_point) {
            invalid("line_to_point requires boundary receipt schema version two");
        }
        if (index > 0 && edge.start_vertex_id != record.edges[index - 1].end_vertex_id) {
            invalid("construction vertex identities do not join");
        }
        const std::optional<Segment> previous =
            edge.receipt.kind == BoundaryConstructionKind::line_relative_turn && index > 0
                ? std::optional<Segment>(edges.back().segment)
                : std::nullopt;
        const std::optional<Vec2> closure_anchor =
            edge.receipt.kind == BoundaryConstructionKind::line_closure
                ? std::optional<Vec2>(record.anchor)
                : std::nullopt;
        const auto rebuilt = replay_construction_receipt(
            edge.receipt,
            ConstructionReplayContext{expected_start, previous, closure_anchor, tolerance_metres});
        if (!(rebuilt.receipt == edge.receipt)) {
            invalid("construction receipt is not canonically normalized");
        }
        const auto end = rebuilt.segment.end;
        edges.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id,
                         rebuilt.segment});
        receipts.push_back(rebuilt.receipt);
        expected_start = end;
    }

    if (!same_point(expected_start, record.anchor)) {
        invalid("boundary construction is not exactly closed at its captured anchor");
    }
    if (record.edges.back().end_vertex_id != record.edges.front().start_vertex_id) {
        invalid("boundary construction closure does not reuse the starting vertex identity");
    }
    Boundary geometry;
    geometry.reserve(edges.size());
    for (const auto& edge : edges) geometry.push_back(edge.segment);
    const auto diagnostics = validate_boundary(geometry, tolerance_metres);
    if (!diagnostics.empty()) {
        const auto& diagnostic = diagnostics.front();
        invalid("replayed boundary geometry [" + std::to_string(diagnostic.segment_index) + "]: " +
                diagnostic.message);
    }

    return {record.replay_version, record.anchor, record.boundary_id, std::move(edges),
            std::move(receipts)};
}

BoundaryReceiptEnvelopeVersion inspect_boundary_receipt_envelope(const Json& envelope) {
    require_object(envelope, "boundary_authoring envelope");
    if (!envelope.contains("version")) {
        invalid("boundary_authoring envelope is missing version");
    }
    const auto value = read_positive_uint(envelope.at("version"),
                                          "boundary_authoring envelope version");
    if (value == boundary_receipt_schema_version_v1) {
        return {BoundaryReceiptEnvelopeFormat::supported_v1, value, {}};
    }
    if (value == boundary_receipt_schema_version_v2) {
        return {BoundaryReceiptEnvelopeFormat::supported_v2, value, {}};
    }
    return {BoundaryReceiptEnvelopeFormat::unsupported_version, value,
            "unsupported boundary_authoring envelope version"};
}

BoundaryReceiptDecodeResult decode_boundary_receipt_envelope(const Json& envelope) {
    const auto inspected = inspect_boundary_receipt_envelope(envelope);
    if (inspected.format == BoundaryReceiptEnvelopeFormat::unsupported_version) {
        return {std::nullopt, envelope, inspected.version, inspected.diagnostic};
    }

    // A recognized schema may still carry a future replay dialect. Validate
    // only the version discriminators before deciding whether its remaining
    // fields are ours to interpret; preserving an opaque future replay must
    // not assume today's topology or extension fields are present.
    require_required_keys(envelope, {"version", "replay_version"},
                          "boundary_authoring envelope");
    const auto version = read_positive_uint(envelope.at("version"),
                                            "boundary_authoring envelope version");
    if (version != boundary_receipt_schema_version_v1 &&
        version != boundary_receipt_schema_version_v2) {
        invalid("unsupported boundary receipt version");
    }
    const auto replay_version = read_positive_uint(envelope.at("replay_version"),
                                                   "boundary_authoring replay_version");
    if (replay_version != boundary_receipt_replay_version) {
        return {std::nullopt, envelope, version,
                "unsupported boundary_authoring replay_version"};
    }
    require_keys(
        envelope,
        {"version", "replay_version", "boundary_id", "anchor", "segments", "extensions"},
        {"version", "replay_version", "boundary_id", "anchor", "segments", "extensions"},
        "boundary_authoring envelope");
    const auto boundary_id = read_string(envelope.at("boundary_id"),
                                         "boundary_authoring boundary_id");
    require_identifier(boundary_id, "boundary_authoring boundary_id");
    const auto anchor = read_point(envelope.at("anchor"), "boundary_authoring anchor");
    const auto& encoded_segments = envelope.at("segments");
    require_array(encoded_segments, "boundary_authoring segments");
    if (encoded_segments.empty()) invalid("boundary_authoring segments cannot be empty");
    const auto& extensions = envelope.at("extensions");
    if (!extensions.is_object()) invalid("boundary_authoring extensions must be an object");

    BoundaryConstructionRecord record;
    record.schema_version = static_cast<std::uint32_t>(version);
    record.replay_version = static_cast<std::uint32_t>(replay_version);
    record.anchor = anchor;
    record.boundary_id = boundary_id;
    record.extensions = extensions;
    std::set<std::string, std::less<>> segment_ids;
    for (std::size_t index = 0; index < encoded_segments.size(); ++index) {
        const auto& value = encoded_segments[index];
        require_keys(value, {"segment_id", "start_vertex_id", "end_vertex_id", "receipt"},
                     {"segment_id", "start_vertex_id", "end_vertex_id", "receipt"},
                     "boundary_authoring segment");
        ConstructionTopologyEdge edge;
        edge.segment_id = read_string(value.at("segment_id"), "construction segment_id");
        edge.start_vertex_id = read_string(value.at("start_vertex_id"),
                                           "construction start_vertex_id");
        edge.end_vertex_id = read_string(value.at("end_vertex_id"),
                                         "construction end_vertex_id");
        require_identifier(edge.segment_id, "construction segment_id");
        require_identifier(edge.start_vertex_id, "construction start_vertex_id");
        require_identifier(edge.end_vertex_id, "construction end_vertex_id");
        if (!segment_ids.insert(edge.segment_id).second) {
            invalid("boundary_authoring contains duplicate segment identity");
        }
        edge.receipt = read_receipt(value.at("receipt"));
        if (edge.receipt.segment_id != edge.segment_id) {
            invalid("boundary_authoring receipt segment identity mismatch");
        }
        record.edges.push_back(std::move(edge));
    }

    (void)replay_boundary_construction(record);
    return {std::move(record), std::nullopt, version, {}};
}

Json encode_boundary_receipt_envelope(const BoundaryConstructionRecord& record) {
    if (record.schema_version != boundary_receipt_schema_version_v1 &&
        record.schema_version != boundary_receipt_schema_version_v2) {
        invalid("unsupported boundary receipt schema version");
    }
    if (record.replay_version != boundary_receipt_replay_version) {
        invalid("unsupported boundary receipt replay version");
    }
    if (!record.extensions.is_object()) invalid("boundary receipt extensions must be an object");
    const auto replay = replay_boundary_construction(record);
    Json encoded_segments = Json::array();
    for (std::size_t index = 0; index < record.edges.size(); ++index) {
        const auto& edge = record.edges[index];
        const auto& receipt = replay.receipts[index];
        encoded_segments.push_back(
            Json{{"segment_id", edge.segment_id},
                 {"start_vertex_id", edge.start_vertex_id},
                 {"end_vertex_id", edge.end_vertex_id},
                 {"receipt", write_receipt(receipt)}});
    }
    return Json{{"version", record.schema_version},
                {"replay_version", record.replay_version},
                {"boundary_id", record.boundary_id},
                {"anchor", write_point(record.anchor, "boundary construction anchor")},
                {"segments", std::move(encoded_segments)},
                {"extensions", record.extensions}};
}

}  // namespace sketch
