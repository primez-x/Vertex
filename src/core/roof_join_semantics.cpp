#include "sketch/roof_join_semantics.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {

[[noreturn]] void reject(const std::string& message) {
    throw std::invalid_argument(message);
}

bool valid_reference_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character != 0 &&
               ((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' ||
                character == '_' || character == '.' || character == ':');
    });
}

}  // namespace

void validate_roof_join_semantics(const RoofJoin& join) {
    if (!valid_reference_id(join.id)) {
        reject("Roof join ID must be a non-empty ASCII identifier");
    }
    if (join.style != RoofJoinStyle::fused) {
        reject("Roof join style is unsupported");
    }
    if (join.roof_ids.size() < 2 || join.roof_ids.size() > 16) {
        reject("Roof joins require between two and sixteen roofs");
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& roof_id : join.roof_ids) {
        if (!valid_reference_id(roof_id) || !ids.insert(roof_id).second) {
            reject("Roof join roof IDs must be unique ASCII identifiers");
        }
    }
}

std::string_view roof_join_style_name(RoofJoinStyle style) noexcept {
    switch (style) {
    case RoofJoinStyle::fused:
        return "fused";
    }
    return "invalid";
}

std::optional<RoofJoinStyle> parse_roof_join_style(std::string_view value) noexcept {
    if (value == "fused") return RoofJoinStyle::fused;
    return std::nullopt;
}

RoofJoin parse_roof_join(const nlohmann::json& value, std::string_view id) {
    RoofJoin result;
    result.id = std::string(id);
    if (!value.is_object() || value.size() != 3 || !value.contains("version") ||
        !value.contains("style") || !value.contains("roof_ids")) {
        reject("Roof join properties must contain exactly version, style, and roof_ids");
    }
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || version != 1) {
        reject("Roof join version must be 1");
    }
    const auto& style = value.at("style");
    if (!style.is_string()) reject("Roof join style must be a string");
    const auto parsed_style = parse_roof_join_style(style.get<std::string>());
    if (!parsed_style.has_value()) reject("Roof join style is unsupported");
    result.style = *parsed_style;
    const auto& roof_ids = value.at("roof_ids");
    if (!roof_ids.is_array()) reject("Roof join roof_ids must be an array");
    result.roof_ids.reserve(roof_ids.size());
    for (const auto& roof_id : roof_ids) {
        if (!roof_id.is_string()) reject("Roof join roof_ids must contain strings");
        result.roof_ids.push_back(roof_id.get<std::string>());
    }
    validate_roof_join_semantics(result);
    return result;
}

nlohmann::json roof_join_json(const RoofJoin& join) {
    validate_roof_join_semantics(join);
    return {{"version", 1}, {"style", roof_join_style_name(join.style)},
            {"roof_ids", join.roof_ids}};
}

}  // namespace sketch
