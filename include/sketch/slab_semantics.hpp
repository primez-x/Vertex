#pragma once

#include "sketch/wall_semantics.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace sketch {

// A horizontal assembly layer is ordered from the slab's lower surface to
// its upper surface. Thicknesses are contiguous and sum to Slab::thickness;
// material references point to an assembly_model catalog when present.
struct SlabLayer {
    std::string id;
    double thickness{};
    std::optional<WallLayerMaterial> material;
    bool operator==(const SlabLayer&) const = default;
};

void validate_slab_layers(const std::vector<SlabLayer>& layers,
                          std::optional<double> slab_thickness = std::nullopt);
[[nodiscard]] std::vector<SlabLayer> parse_slab_layers(const nlohmann::json& value,
                                                        double slab_thickness);
[[nodiscard]] nlohmann::json slab_layers_json(const std::vector<SlabLayer>& layers);

} // namespace sketch
