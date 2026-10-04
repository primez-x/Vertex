#include "sketch/survey_report.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(std::string("Survey: ") + message);
}
std::string_view trim(std::string_view text) {
    // QString-compatible UTF-8 whitespace for saved entered text.
    const auto space_width = [](std::string_view value) -> std::size_t {
        if (value.empty()) return 0;
        const auto first = static_cast<unsigned char>(value[0]);
        if (first < 128) return std::isspace(first) ? 1 : 0;
        if (value.starts_with("\xc2\x85") || value.starts_with("\xc2\xa0")) return 2;
        if (value.starts_with("\xe1\x9a\x80") || value.starts_with("\xe2\x80\xa8") ||
            value.starts_with("\xe2\x80\xa9") || value.starts_with("\xe2\x80\xaf") ||
            value.starts_with("\xe2\x81\x9f") || value.starts_with("\xe3\x80\x80")) return 3;
        if (value.size() >= 3 && first == 0xe2 && static_cast<unsigned char>(value[1]) == 0x80 &&
            static_cast<unsigned char>(value[2]) >= 0x80 && static_cast<unsigned char>(value[2]) <= 0x8a) return 3;
        return 0;
    };
    while (const auto width = space_width(text)) text.remove_prefix(width);
    while (!text.empty()) {
        auto start = text.size() - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
        const auto width = space_width(text.substr(start));
        if (width == 0 || width != text.size() - start) break;
        text.remove_suffix(width);
    }
    return text;
}
std::string upper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}
void valid_text(const std::string& value, std::size_t limit) {
    require(value.size() <= limit && value.find('\0') == std::string::npos, "entered text exceeds limits or contains NUL");
    try { (void)Json(value).dump(); } catch (const Json::exception&) { throw std::invalid_argument("Survey: invalid UTF-8"); }
}
std::string text_field(const Json& object, const char* key, std::size_t limit) {
    require(object.contains(key) && object.at(key).is_string(), "missing entered text field");
    auto result = object.at(key).get<std::string>();
    valid_text(result, limit);
    return result;
}
Json quantity_receipt(const Quantity& value) {
    return {{"original_expression", value.original_expression},
        {"exact_metres", {{"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
}
double signed_angle(std::string_view value) {
    require(!value.empty() && value.size() <= 128, "missing or excessive signed curve angle");
    const auto body = (value.front() == '+' || value.front() == '-') ? value.substr(1) : value;
    require(!body.empty() && std::all_of(body.begin(), body.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }),
        "curve angle requires signed decimal degrees");
    if (value.front() == '+') value.remove_prefix(1);
    double degrees{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), degrees);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(degrees) &&
        std::abs(degrees) > 0 && std::abs(degrees) < 360, "curve angle magnitude must be greater than zero and below 360 degrees");
    return degrees * std::numbers::pi / 180;
}
struct Parsed { Json report; std::vector<SurveyLeg> legs; Boundary segments; };
Parsed parse_input(const SurveyInput& input) {
    require(input.default_unit == Unit::metre || input.default_unit == Unit::foot, "unsupported default input unit");
    valid_text(input.legs_text, 1024 * 1024);
    valid_text(input.source_text, 4096);
    valid_text(input.closure_tolerance_expression, 4096);
    std::vector<SurveyLeg> legs;
    auto distances = Json::array(), curves = Json::array();
    std::string_view remaining = input.legs_text;
    std::size_t line_number = 0;
    do {
        ++line_number;
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        if (!trim(line).empty()) {
            try {
                require(legs.size() < SurveyTraverse::maximum_legs, "too many entered legs");
                std::vector<std::string_view> fields;
                auto rest = line;
                while (true) {
                    const auto comma = rest.find(',');
                    fields.push_back(trim(rest.substr(0, comma)));
                    if (comma == std::string_view::npos) break;
                    rest.remove_prefix(comma + 1);
                }
                const auto kind = upper(fields.front());
                const bool curved = kind == "CURVE" || kind == "ARC_HEIGHT" || kind == "ARC_LENGTH";
                require(fields.size() == (kind == "ARC_LENGTH" ? 6 : curved ? 5 : 3), "use quadrant, bearing, distance or a supported curve call");
                const auto offset = curved ? 1u : 0u;
                const auto quadrant = upper(fields[offset]);
                BearingQuadrant bearing;
                if (quadrant == "NE") bearing = BearingQuadrant::north_east;
                else if (quadrant == "SE") bearing = BearingQuadrant::south_east;
                else if (quadrant == "SW") bearing = BearingQuadrant::south_west;
                else if (quadrant == "NW") bearing = BearingQuadrant::north_west;
                else throw std::invalid_argument("use NE, SE, SW, or NW");
                const auto angle = parse_survey_angle(fields[offset + 1]);
                const auto chord = parse_quantity(fields[offset + 2], input.default_unit);
                require(chord.metres > 0, "distance or chord must be positive");
                const auto id = "leg-" + std::to_string(legs.size() + 1);
                double sweep = 0;
                if (curved) {
                    Json receipt{{"version", 1}, {"leg_id", id}, {"line_number", line_number}};
                    if (kind == "CURVE") {
                        sweep = signed_angle(fields[4]);
                        (void)arc_from_chord_angle({}, {chord.metres, 0}, sweep);
                        receipt["construction_kind"] = "chord_angle";
                        receipt["original_expression"] = std::string(fields[4]);
                        receipt["sweep_radians"] = sweep;
                    } else {
                        const auto quantity = parse_quantity(fields[4], input.default_unit);
                        const auto values = quantity_receipt(quantity);
                        receipt["original_expression"] = values.at("original_expression");
                        receipt["exact_metres"] = values.at("exact_metres");
                        if (kind == "ARC_HEIGHT") {
                            receipt["construction_kind"] = "chord_height";
                            sweep = arc_from_chord_height({}, {chord.metres, 0}, quantity.metres).sweep_radians;
                        } else {
                            const auto direction = upper(fields[5]);
                            require(direction == "CW" || direction == "CCW", "arc length direction requires CW or CCW");
                            receipt["construction_kind"] = "chord_arc_length";
                            receipt["clockwise"] = direction == "CW";
                            sweep = arc_from_chord_arc_length({}, {chord.metres, 0}, quantity.metres, direction == "CW").sweep_radians;
                        }
                    }
                    curves.push_back(std::move(receipt));
                }
                legs.push_back({id, bearing, angle, chord.metres, sweep});
                auto receipt = quantity_receipt(chord);
                receipt["leg_id"] = id;
                receipt["line_number"] = line_number;
                distances.push_back(std::move(receipt));
            } catch (const std::exception& error) {
                throw std::invalid_argument("Survey line " + std::to_string(line_number) + ": " + error.what());
            }
        }
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1);
    } while (true);
    const SurveyTraverse traverse(std::string(trim(input.source_text)), legs,
        parse_quantity(trim(input.closure_tolerance_expression), input.default_unit).metres);
    auto report = Json::parse(traverse.serialize());
    report["input_provenance"] = {{"version", curves.empty() ? 1 : 2}, {"default_unit", input.default_unit == Unit::metre ? "m" : "ft"},
        {"legs_text", input.legs_text}, {"source_text", input.source_text},
        {"closure_tolerance_expression", input.closure_tolerance_expression}, {"distances", distances}};
    if (!curves.empty()) report["input_provenance"]["curves"] = std::move(curves);
    return {std::move(report), std::move(legs), traverse.measured_segments()};
}
void compare_receipts(const Json& actual, const Json& expected, bool curves) {
    require(actual.is_array() && actual.size() == expected.size(), "entered receipts are missing or inconsistent");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].is_object(), "malformed entered receipt");
        // Future curve semantics cannot be discarded as an opaque extension.
        if (curves) require(actual[i].contains("construction_kind") && actual[i].at("construction_kind") == expected[i].at("construction_kind"), "unsupported curve construction kind");
        for (auto it = expected[i].begin(); it != expected[i].end(); ++it) {
            require(actual[i].contains(it.key()) && actual[i].at(it.key()) == it.value(), "entered receipt does not match entered calls");
            if (it.value().is_number_integer()) require(actual[i].at(it.key()).is_number_integer(), "malformed integer receipt");
        }
        if (expected[i].contains("exact_metres")) {
            const auto& exact = actual[i].at("exact_metres");
            require(exact.is_object() && exact.at("numerator").is_number_integer() && exact.at("denominator").is_number_integer(), "malformed exact quantity receipt");
        }
    }
}
} // namespace

Json build_survey_report(const SurveyInput& input) { return parse_input(input).report; }

RebuiltSurveyReport rebuild_survey_report(const Json& report) {
    require(report.is_object() && report.contains("version") && report.at("version").is_number_integer() &&
        (report.at("version") == 1 || report.at("version") == 2), "unsupported report version");
    require(report.dump().size() <= 4 * 1024 * 1024, "report exceeds 4 MiB");
    require(report.contains("input_provenance"), "report has no editable entered calls");
    const auto& input = report.at("input_provenance");
    require(input.is_object() && input.contains("version") && input.at("version").is_number_integer() &&
        (input.at("version") == 1 || input.at("version") == 2), "unsupported entered-input version");
    const auto unit = text_field(input, "default_unit", 2);
    require(unit == "m" || unit == "ft", "unsupported default input unit");
    auto parsed = parse_input({text_field(input, "legs_text", 1024 * 1024), text_field(input, "source_text", 4096),
        text_field(input, "closure_tolerance_expression", 4096), unit == "m" ? Unit::metre : Unit::foot});
    require(report.at("version") == parsed.report.at("version") && input.at("version") == parsed.report.at("input_provenance").at("version"), "report/input version does not match entered calls");
    for (const auto* key : {"provenance", "legs", "closure_tolerance_m"})
        require(report.contains(key) && report.at(key) == parsed.report.at(key), "normalized report inputs do not match entered calls");
    require(input.contains("distances"), "missing distance receipts");
    compare_receipts(input.at("distances"), parsed.report.at("input_provenance").at("distances"), false);
    if (parsed.report.at("version") == 2) {
        require(input.contains("curves"), "missing curve receipts");
        compare_receipts(input.at("curves"), parsed.report.at("input_provenance").at("curves"), true);
    } else require(!input.contains("curves"), "curve receipts require version two calls");
    // Preserve opaque input extensions, including receipt extensions. The
    // normalized known fields were compared, and derived display is rebuilt.
    parsed.report["input_provenance"] = input;
    return RebuiltSurveyReport(std::move(parsed.report), std::move(parsed.legs), std::move(parsed.segments));
}

SurveyBoundaryResult make_survey_boundary(const RebuiltSurveyReport& report, SurveyClosureMode mode) {
    require(mode == SurveyClosureMode::retain_measured_calls || mode == SurveyClosureMode::adjust_final_endpoint, "unsupported closure mode");
    const auto& diagnostics = report.report().at("diagnostics");
    require(diagnostics.at("closed") == true && !diagnostics.at("area_m2").is_null(), "open or degenerate traverse cannot form an area boundary");
    SurveyBoundaryResult result{report.measured_segments()};
    const auto end = result.boundary.back().end;
    const auto start = result.boundary.front().start;
    const bool residual = end.x != start.x || end.y != start.y;
    if (mode == SurveyClosureMode::adjust_final_endpoint) {
        require(result.boundary.back().sweep_radians == 0, "cannot adjust the final endpoint of a measured curve");
        result.boundary.back().end = start;
        result.adjusted_final_endpoint = true;
        result.endpoint_adjustment_m = Vec2{start.x - end.x, start.y - end.y};
    } else if (residual) {
        require(std::hypot(end.x - start.x, end.y - start.y) > default_geometry_tolerance_metres,
            "measured residual requires a closing segment at or below geometry tolerance; choose an explicit supported endpoint adjustment");
        result.boundary.push_back({end, start, 0});
        result.added_closing_segment = true;
    }
    const auto issues = validate_boundary(result.boundary);
    if (!issues.empty()) throw std::invalid_argument("Survey boundary: " + issues.front().message);
    return result;
}
} // namespace sketch
