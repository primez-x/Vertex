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
    // Optional signed change in wall-top height from baseline start to end.
    // A nonzero slope is currently supported for straight baselines; the
    // bottom remains at `elevation` and `height` is the start height.
    std::optional<double> slope_rise;
};

// A wall join is a first-class architectural relationship.  The v1 fused
// style keeps each wall's semantic identity and hosted openings while the
// derived geometry is represented by one boolean union for coordinated
// views.  Wall joins are deliberately separate from measurement boundaries.
enum class WallJoinStyle { fused };

struct WallJoin {
    std::string id;
    std::vector<std::string> wall_ids;
    WallJoinStyle style{WallJoinStyle::fused};

    bool operator==(const WallJoin&) const = default;
};

// Validate the shared semantic contract used by document editing and solid
// construction. The function throws std::invalid_argument on invalid input
// and never modifies the supplied wall.
void validate_wall_semantics(const Wall& wall);

// Validate a detached wall-join value without resolving its wall IDs.  The
// document layer performs target existence/type checks; geometry builders
// additionally require endpoint connectivity before fusing the solids.
void validate_wall_join_semantics(const WallJoin& join);

[[nodiscard]] std::string_view wall_join_style_name(WallJoinStyle style) noexcept;
[[nodiscard]] std::optional<WallJoinStyle>
parse_wall_join_style(std::string_view value) noexcept;
[[nodiscard]] WallJoin parse_wall_join(const nlohmann::json& value,
                                       std::string_view id);
[[nodiscard]] nlohmann::json wall_join_json(const WallJoin& join);

// Validate and decode the optional persisted wall-layer array. The JSON form
// is versioned at the containing project format boundary by the wall schema;
// each layer object contains `id`, `thickness_m`, and an optional version-1
// `material_assignment` object with `catalog_id` and `material_id`.
[[nodiscard]] std::vector<WallLayer> parse_wall_layers(const nlohmann::json& value,
                                                       double wall_thickness);
[[nodiscard]] nlohmann::json wall_layers_json(const std::vector<WallLayer>& layers);

}  // namespace sketch
