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
    if (join.singleton_material_scope &&
        (join.roof_ids.size() != 1 || !join.material_assignment)) {
        reject("Singleton material scopes require exactly one roof and a material assignment");
    }
    if (!join.singleton_material_scope && (join.roof_ids.size() < 2 || join.roof_ids.size() > 16)) {
        reject("Roof joins require between two and sixteen roofs");
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& roof_id : join.roof_ids) {
        if (!valid_reference_id(roof_id) || !ids.insert(roof_id).second) {
            reject("Roof join roof IDs must be unique ASCII identifiers");
        }
    }
    if (join.material_assignment &&
        (!valid_reference_id(join.material_assignment->catalog_id) ||
         !valid_reference_id(join.material_assignment->material_id)))
        reject("Roof join material IDs must be non-empty bounded ASCII identifiers");
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
    if (!value.is_object() || !value.contains("version") ||
        !value.contains("style") || !value.contains("roof_ids")) {
        reject("Roof join properties must contain version, style, and roof_ids");
    }
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) ||
        (version != 1 && version != 2 && version != 3)) {
        reject("Roof join version must be 1, 2 or 3");
    }
    result.singleton_material_scope = version == 3;
    const bool assigned = value.contains("material_assignment");
    if (value.size() != (assigned ? 4u : 3u) || (version == 1 && assigned) ||
        (result.singleton_material_scope && !assigned))
        reject("Roof join properties contain unsupported fields for their version");
    if (assigned) {
        const auto& assignment = value.at("material_assignment");
        if (!assignment.is_object() || assignment.size() != 3 ||
            !assignment.contains("version") ||
            (!assignment.at("version").is_number_integer() && !assignment.at("version").is_number_unsigned()) ||
            assignment.at("version") != 1 ||
            !assignment.contains("catalog_id") || !assignment.contains("material_id") ||
            !assignment.at("catalog_id").is_string() || !assignment.at("material_id").is_string())
            reject("Roof join material assignment must contain exactly version 1, catalog_id and material_id");
        result.material_assignment = RoofJoinMaterialAssignment{
            assignment.at("catalog_id").get<std::string>(),
            assignment.at("material_id").get<std::string>()};
    }
    const auto& style = value.at("style");
    if (!style.is_string()) reject("Roof join style must be a string");
    const auto parsed_style = parse_roof_join_style(style.get<std::string>());
    if (!parsed_style.has_value()) reject("Roof join style is unsupported");
    result.style = *parsed_style;
    const auto& roof_ids = value.at("roof_ids");
    if (!roof_ids.is_array()) reject("Roof join roof_ids must be an array");
    if (result.singleton_material_scope ? roof_ids.size() != 1 : (roof_ids.size() < 2 || roof_ids.size() > 16))
        reject("Roof join member count is unsupported for its version");
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
    nlohmann::json value{{"version", join.singleton_material_scope ? 3 : join.material_assignment ? 2 : 1},
        {"style", roof_join_style_name(join.style)}, {"roof_ids", join.roof_ids}};
    if (join.material_assignment) value["material_assignment"] = {
        {"version", 1},
        {"catalog_id", join.material_assignment->catalog_id},
        {"material_id", join.material_assignment->material_id}};
    return value;
}

}  // namespace sketch
