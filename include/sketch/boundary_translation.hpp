#pragma once

#include "sketch/document.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sketch {

// Versioned command intent. Authorization is established by Document replay,
// never by the presence of this envelope or a human-readable action label.
inline void validate_boundary_translation(const BoundaryTranslation& value) {
    const auto& id = value.boundary_id;
    if (id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }) || !std::isfinite(value.offset.x) || !std::isfinite(value.offset.y))
        throw std::invalid_argument("Invalid boundary translation identity or finite offset");
}

inline nlohmann::json encode_boundary_translation(const BoundaryTranslation& value) {
    validate_boundary_translation(value);
    return {{"version", 1}, {"boundary_id", value.boundary_id},
            {"offset", {value.offset.x, value.offset.y}}};
}

inline BoundaryTranslation decode_boundary_translation(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 3 || !value.contains("version") ||
        !value.contains("boundary_id") || !value.contains("offset") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.at("boundary_id").is_string() || !value.at("offset").is_array() ||
        value.at("offset").size() != 2 || !value.at("offset")[0].is_number() ||
        !value.at("offset")[1].is_number())
        throw std::invalid_argument("Invalid boundary translation version or envelope shape");
    BoundaryTranslation result{value.at("boundary_id").get<std::string>(),
        {value.at("offset")[0].get<double>(), value.at("offset")[1].get<double>()}};
    validate_boundary_translation(result);
    return result;
}

} // namespace sketch
