#pragma once

#include "sketch/geometry.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace sketch {

// Offsets and widths follow the hosted wall's centreline, including its arc.
struct HostedOpening {
    std::string id;
    double offset{};
    double width{};
    double sill{};
    double height{};
};

// A wall layer is ordered through the containing wall's `layers` vector from
// the negative to positive normal side of the baseline. Layers are contiguous
// and their thicknesses must sum exactly (within the geometry tolerance) to
// Wall::thickness. Material references are optional, but catalog and material
// IDs are always paired when present.
struct WallLayerMaterial {
    std::string catalog_id;
    std::string material_id;
    bool operator==(const WallLayerMaterial&) const = default;
};

struct WallLayer {
    std::string id;
    double thickness{};
    std::optional<WallLayerMaterial> material;
    bool operator==(const WallLayer&) const = default;
};

struct Wall {
    std::string id;
    Segment baseline;
    double thickness{};
    double height{};
    double elevation{};
    std::vector<HostedOpening> openings;
    std::vector<WallLayer> layers;
};

// Validate the shared semantic contract used by document editing and solid
// construction. The function throws std::invalid_argument on invalid input
// and never modifies the supplied wall.
void validate_wall_semantics(const Wall& wall);

// Validate and decode the optional persisted wall-layer array. The JSON form
// is versioned at the containing project format boundary by the wall schema;
// each layer object contains `id`, `thickness_m`, and an optional version-1
// `material_assignment` object with `catalog_id` and `material_id`.
[[nodiscard]] std::vector<WallLayer> parse_wall_layers(const nlohmann::json& value,
                                                       double wall_thickness);
[[nodiscard]] nlohmann::json wall_layers_json(const std::vector<WallLayer>& layers);

}  // namespace sketch
