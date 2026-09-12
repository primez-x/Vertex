#pragma once

#include "sketch/document.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sketch {

// Intent is authorized only by deterministic Document history replay.
inline void validate_boundary_transform(const BoundaryTransformation& value) {
    const auto& id = value.boundary_id;
    const auto& t = value.transform;
    if (id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }) || !std::isfinite(t.pivot.x) || !std::isfinite(t.pivot.y) ||
        !std::isfinite(t.rotation_radians) || !std::isfinite(t.offset.x) || !std::isfinite(t.offset.y))
        throw std::invalid_argument("Invalid boundary transform identity or finite parameters");
}

inline nlohmann::json encode_boundary_transform(const BoundaryTransformation& value) {
    validate_boundary_transform(value);
    const auto& t = value.transform;
    return {{"version", 1}, {"boundary_id", value.boundary_id},
            {"pivot", {t.pivot.x, t.pivot.y}}, {"rotation_radians", t.rotation_radians},
            {"flip_horizontal", t.flip_horizontal}, {"flip_vertical", t.flip_vertical},
            {"offset", {t.offset.x, t.offset.y}}};
}

inline BoundaryTransformation decode_boundary_transform(const nlohmann::json& value) {
    const auto point = [&](const char* key) {
        return value.contains(key) && value.at(key).is_array() && value.at(key).size() == 2 &&
            value.at(key)[0].is_number() && value.at(key)[1].is_number();
    };
    if (!value.is_object() || value.size() != 7 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("boundary_id") || !value.at("boundary_id").is_string() ||
        !point("pivot") || !point("offset") || !value.contains("rotation_radians") ||
        !value.at("rotation_radians").is_number() || !value.contains("flip_horizontal") ||
        !value.at("flip_horizontal").is_boolean() || !value.contains("flip_vertical") ||
        !value.at("flip_vertical").is_boolean())
        throw std::invalid_argument("Invalid boundary transform version or envelope shape");
    BoundaryTransformation result{value.at("boundary_id").get<std::string>(),
        {{value.at("pivot")[0].get<double>(), value.at("pivot")[1].get<double>()},
         value.at("rotation_radians").get<double>(), value.at("flip_horizontal").get<bool>(),
         value.at("flip_vertical").get<bool>(),
         {value.at("offset")[0].get<double>(), value.at("offset")[1].get<double>()}}};
    validate_boundary_transform(result);
    return result;
}

} // namespace sketch
