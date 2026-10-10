#pragma once

#include "sketch/building_objects.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <span>
#include <variant>

namespace sketch {
struct Entity;
struct RoofJoin;

using RoofObject = std::variant<SlopedRoofPanel, GableRoof, HipRoof>;

// Document-independent authoring codec. Unknown properties are ignored;
// schema 1 forbids openings; schemas 2/3/4 require the retained opening roster.
// Schemas 3/4 permit the strictly owned version-1 skylight object on a row.
// Only schema 4 owns optional rotation_rad; older schemas keep it opaque.
// Entity creation and placement/context resolution belong to the caller.
[[nodiscard]] RoofObject decode_roof_entity(const Entity& entity);
[[nodiscard]] nlohmann::json encode_roof_properties(const RoofObject& object);
[[nodiscard]] TopoDS_Shape make_roof_structure_shape(const RoofObject& object);
[[nodiscard]] TopoDS_Shape make_roof_skylight_shape(const RoofObject& object, const std::string& opening_id);
// Objects must already have their actual vertical placements resolved. Refuses
// skylight fills or real removed cavities intersecting another member's cut
// structure, and fills intersecting fills owned by another member. Face contact
// is allowed. Empty skylight rosters return without constructing any geometry.
// Skylit cohorts are limited to 4096 members/openings, 256 fills, 4096 retained
// opening rebuilds and 2048 bounding-box-admitted intersection operations.
void validate_roof_skylight_cohort(std::span<const RoofObject> objects);
// Resolves only actual roof members from the supplied authoritative entity map;
// no reconstructed relationship source or cached geometry is admitted.
void validate_roof_join_skylights(const RoofJoin& join,
    const std::map<std::string, Entity, std::less<>>& actual_entities);
// Complete assembly. Quantity callers use make_roof_structure_shape so fixed
// skylights cannot be counted as roof material.
[[nodiscard]] TopoDS_Shape make_roof_shape(const RoofObject& object);
} // namespace sketch
