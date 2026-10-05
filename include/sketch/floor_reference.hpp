#pragma once

#include "sketch/document.hpp"

#include <optional>
#include <string_view>

namespace sketch {

struct FloorReferenceOffset {
    double x{};
    double y{};
    bool operator==(const FloorReferenceOffset&) const = default;
};

// Optional floor metadata; a reference never changes its source geometry.
struct FloorReferenceSettings {
    std::string source_floor_id;
    bool visible{true};
    double opacity{0.25};
    FloorReferenceOffset offset_m;

    [[nodiscard]] static FloorReferenceSettings from_json(const nlohmann::json& value);
    [[nodiscard]] static std::optional<FloorReferenceSettings> from_entity(const Entity& floor);
    [[nodiscard]] nlohmann::json to_json() const;
    bool operator==(const FloorReferenceSettings&) const = default;
};

// Both floors must have valid organization contexts in the same building/property.
// Neither floor requires a layer. Missing metadata returns nullopt; invalid data throws.
[[nodiscard]] std::optional<FloorReferenceSettings> resolve_floor_reference(
    const DocumentSnapshot& snapshot, std::string_view destination_floor_id);

}  // namespace sketch
