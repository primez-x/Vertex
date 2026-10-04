#include "sketch/survey_source_version.hpp"
#include <cmath>

namespace sketch {
namespace {
using Json = nlohmann::json;
bool version(const Json& value, int expected) {
    return value.is_object() && value.contains("version") &&
        value.at("version").is_number_integer() && value.at("version") == expected;
}
bool text(const Json& value, const char* key, std::size_t maximum) {
    return value.contains(key) && value.at(key).is_string() &&
        value.at(key).get_ref<const std::string&>().size() <= maximum;
}
bool finite_number(const Json& value, const char* key) {
    return value.contains(key) && value.at(key).is_number() && std::isfinite(value.at(key).get<double>());
}
bool closure(const Json& value) {
    if (!value.is_object() || !value.contains("added_closing_segment") ||
        !value.at("added_closing_segment").is_boolean() || !value.contains("adjusted_final_endpoint") ||
        !value.at("adjusted_final_endpoint").is_boolean() || !value.contains("endpoint_adjustment_m")) return false;
    const auto& offset = value.at("endpoint_adjustment_m");
    return offset.is_null() || (offset.is_object() && finite_number(offset, "east") && finite_number(offset, "north"));
}
bool structure(const Json& report) {
    if (!report.is_object() || !text(report, "provenance", 4096) ||
        !finite_number(report, "closure_tolerance_m") || report.at("closure_tolerance_m").get<double>() < 0 ||
        !report.contains("legs") || !report.at("legs").is_array() ||
        report.at("legs").empty() || report.at("legs").size() > 4096) return false;
    const auto& input = report.at("input_provenance");
    if (!text(input, "legs_text", 1024 * 1024) || !text(input, "source_text", 4096) ||
        !text(input, "closure_tolerance_expression", 4096) || !text(input, "default_unit", 2) ||
        (input.at("default_unit") != "m" && input.at("default_unit") != "ft") ||
        !input.contains("distances") || !input.at("distances").is_array() || input.at("distances").size() > 4096)
        return false;
    for (const auto& leg : report.at("legs")) {
        if (!leg.is_object() || !text(leg, "id", 4096) || !text(leg, "quadrant", 2) ||
            !finite_number(leg, "angle_degrees") || !finite_number(leg, "distance_m") ||
            (version(report, 2) && !finite_number(leg, "sweep_radians"))) return false;
    }
    if (version(report, 2)) {
        if (!input.contains("curves") || !input.at("curves").is_array() || input.at("curves").empty() ||
            input.at("curves").size() > 4096) return false;
        for (const auto& curve : input.at("curves")) {
            if (!version(curve, 1) || !text(curve, "construction_kind", 32)) return false;
            const auto& kind = curve.at("construction_kind");
            if (kind != "chord_angle" && kind != "chord_height" && kind != "chord_arc_length") return false;
        }
    }
    return true;
}
}
SurveySourceAdmission inspect_survey_source(const Json& source) {
    SurveySourceAdmission result;
    const auto fail = [&] { result.unsupported = "Unsupported or malformed survey source provenance"; };
    if (!version(source, 1) && !version(source, 2)) {
        result.modern_reader = true; fail(); return result;
    }
    result.modern_reader = version(source, 2); // Sticky even when both reports are now straight.
    for (const auto* key : {"report", "original_report"}) {
        if (!source.contains(key)) {
            if (result.modern_reader && std::string_view(key) == "report") fail();
            continue;
        }
        const auto& report = source.at(key);
        if (!version(report, 1) && !version(report, 2)) {
            result.modern_reader = true; fail(); continue;
        }
        if (!report.contains("input_provenance")) {
            if (result.modern_reader || version(report, 2)) { result.modern_reader = true; fail(); }
            continue; // Legacy reports without typed entered-input receipts stay archival.
        }
        const auto& input = report.at("input_provenance");
        if ((!version(input, 1) && !version(input, 2)) ||
            input.at("version") != report.at("version")) {
            result.modern_reader = true; fail(); continue;
        }
        if (version(report, 2)) {
            result.modern_reader = true;
            if (version(source, 1)) fail();
        }
        if (version(source, 2) && !structure(report)) fail();
    }
    if (version(source, 2) && (!closure(source) ||
        (source.contains("original_closure") && !closure(source.at("original_closure"))))) fail();
    return result;
}
}
