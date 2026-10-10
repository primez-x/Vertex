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

    bool operator==(const HostedOpening&) const = default;
};

// Derived from an actual pocket door's retained operation and manufactured
// assembly. This is a partial-depth cavity alongside the opening, expressed
// in directed wall station/left-normal coordinates, never persisted separately.
struct PocketDoorRecess {
    std::string opening_id;
    double offset{}, width{}, sill{}, height{}, normal_offset{}, depth{};
    bool operator==(const PocketDoorRecess&) const = default;
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
    // Without a retained gradient, the top is a plane with its gradient along
    // the start-to-end chord, including for circular baselines. The bottom
    // remains at `elevation` and `height` is the centreline start height.
    // A curved wall's station heights follow chord projection, not a linear
    // ramp along arc length.
    std::optional<double> slope_rise;
    // Optional retained top plane, in metres of vertical rise per horizontal
    // metre. This preserves a plane across subarc splits whose chords differ.
    // When set, height is the relative top at baseline.start and slope_rise,
    // if supplied, must agree with the gradient projected along the chord.
    std::optional<Vec2> top_gradient_m_per_m;
    std::vector<PocketDoorRecess> pocket_recesses;
};

struct OpeningAssembly;
struct DoorOperation;
// Computes and admits the cavity even when the leaf is closed. Non-pocket
// operations return no recess. The opening entity remains the authority.
[[nodiscard]] std::optional<PocketDoorRecess> pocket_door_recess(
    const Wall&, const HostedOpening&, const OpeningAssembly&, const DoorOperation&);

struct WallTopHeightRange {
    double minimum{};
    double maximum{};
};

// Shared planar-top equation. Heights exclude Wall::elevation. Station is
// distance along the directed centreline; normal offsets follow its left
// normal, matching the layer order. Unset gradients derive from slope_rise;
// scalar rises within geometry tolerance are flat. Explicit gradients retain
// their supplied plane, including gradients perpendicular to the chord.
// Invalid/non-finite or unrepresentable inputs throw std::invalid_argument;
// these helpers do not validate openings, materials or positive top heights.
[[nodiscard]] Vec2 wall_top_gradient(const Wall& wall);
[[nodiscard]] double wall_top_height(const Wall& wall, double station_metres,
                                     double normal_offset_metres = 0.0);
// Analytic extrema over the entire closed station/normal-offset rectangle,
// including interior arc extrema. Equal stations or offsets are permitted.
// Stations outside the baseline by at most geometry tolerance are clamped.
[[nodiscard]] WallTopHeightRange wall_top_height_range(
    const Wall& wall, double from_metres, double to_metres,
    double inner_offset_metres, double outer_offset_metres);
// Centreline stations where the top crosses a relative height. Circular
// extrema partition monotone intervals; roots settle at double precision.
// A constant top has no isolated crossings.
[[nodiscard]] std::vector<double> wall_top_height_crossings(
    const Wall& wall, double relative_height_metres);

// Strict version-1 retained plane codec:
// {"version":1,"gradient_m_per_m":[gx,gy]}.
[[nodiscard]] Vec2 parse_wall_top_plane(const nlohmann::json& value);
[[nodiscard]] nlohmann::json wall_top_plane_json(Vec2 gradient);

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
