#include "sketch/slab_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {

constexpr double tolerance = default_geometry_tolerance_metres;

[[noreturn]] void reject(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

bool valid_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.' || character == ':';
    });
}

void positive(double value, std::string_view message) {
    if (!std::isfinite(value) || value <= tolerance) reject(message);
}

std::string required_string(const nlohmann::json& value, const char* field) {
    if (!value.is_object() || !value.contains(field) || !value.at(field).is_string()) {
        throw std::invalid_argument(std::string("Slab layer ") + field +
                                    " must be a non-empty string");
    }
    const auto result = value.at(field).get<std::string>();
    if (result.empty()) {
        throw std::invalid_argument(std::string("Slab layer ") + field +
                                    " must be a non-empty string");
    }
    return result;
}

double required_number(const nlohmann::json& value, const char* field) {
    if (!value.is_object() || !value.contains(field) || !value.at(field).is_number()) {
        throw std::invalid_argument(std::string("Slab layer ") + field +
                                    " must be a finite number");
    }
    const auto result = value.at(field).get<double>();
    if (!std::isfinite(result)) {
        throw std::invalid_argument(std::string("Slab layer ") + field +
                                    " must be a finite number");
    }
    return result;
}

} // namespace

void validate_slab_layers(const std::vector<SlabLayer>& layers,
                          std::optional<double> slab_thickness) {
    if (layers.empty()) return;
    if (layers.size() > 1024) reject("Slab layer count exceeds the supported limit");
    std::set<std::string, std::less<>> ids;
    double total = 0.0;
    for (const auto& layer : layers) {
        if (!valid_id(layer.id) || !ids.insert(layer.id).second) {
            reject("Slab layer IDs must be unique ASCII identifiers");
        }
        positive(layer.thickness, "Slab layer thickness must be positive");
        if (!std::isfinite(total + layer.thickness)) {
            reject("Slab layer thickness total exceeds the supported numeric range");
        }
        total += layer.thickness;
        if (layer.material.has_value() &&
            (!valid_id(layer.material->catalog_id) ||
             !valid_id(layer.material->material_id))) {
            reject("Slab layer material references must be paired identifiers");
        }
    }
    if (slab_thickness.has_value()) {
        const auto limit = std::max(tolerance, std::abs(*slab_thickness) * 1e-9);
        if (!std::isfinite(*slab_thickness) ||
            std::abs(total - *slab_thickness) > limit) {
            reject("Slab layer thicknesses must sum to the slab thickness");
        }
    }
}

std::vector<SlabLayer> parse_slab_layers(const nlohmann::json& value,
                                          double slab_thickness) {
    if (!std::isfinite(slab_thickness) || slab_thickness <= tolerance) {
        throw std::invalid_argument("Slab layer stack requires a positive slab thickness");
    }
    if (!value.is_array()) throw std::invalid_argument("Slab layers must be an array");
    std::vector<SlabLayer> result;
    result.reserve(value.size());
    for (const auto& entry : value) {
        if (!entry.is_object() || (entry.size() != 2 && entry.size() != 3) ||
            !entry.contains("id") || !entry.contains("thickness_m")) {
            throw std::invalid_argument(
                "Slab layer requires id, thickness_m, and optional material_assignment");
        }
        for (const auto& [key, unused] : entry.items()) {
            (void)unused;
            if (key != "id" && key != "thickness_m" && key != "material_assignment") {
                throw std::invalid_argument("Slab layer contains an unknown field");
            }
        }
        SlabLayer layer;
        layer.id = required_string(entry, "id");
        layer.thickness = required_number(entry, "thickness_m");
        if (entry.contains("material_assignment")) {
            const auto& assignment = entry.at("material_assignment");
            if (!assignment.is_object() || assignment.size() != 3 ||
                !assignment.contains("version") || !assignment.contains("catalog_id") ||
                !assignment.contains("material_id") ||
                !assignment.at("version").is_number_integer() ||
                assignment.at("version") != 1) {
                throw std::invalid_argument(
                    "Slab layer material_assignment must be version 1");
            }
            layer.material = WallLayerMaterial{
                required_string(assignment, "catalog_id"),
                required_string(assignment, "material_id")};
        }
        result.push_back(std::move(layer));
    }
    validate_slab_layers(result, slab_thickness);
    return result;
}

nlohmann::json slab_layers_json(const std::vector<SlabLayer>& layers) {
    validate_slab_layers(layers, std::nullopt);
    auto result = nlohmann::json::array();
    for (const auto& layer : layers) {
        nlohmann::json value{{"id", layer.id}, {"thickness_m", layer.thickness}};
        if (layer.material.has_value()) {
            value["material_assignment"] = {
                {"version", 1}, {"catalog_id", layer.material->catalog_id},
                {"material_id", layer.material->material_id}};
        }
        result.push_back(std::move(value));
    }
    return result;
}

} // namespace sketch
