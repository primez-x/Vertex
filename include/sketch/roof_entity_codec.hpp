#pragma once

#include "sketch/building_objects.hpp"
#include <nlohmann/json.hpp>
#include <variant>

namespace sketch {
struct Entity;

using RoofObject = std::variant<SlopedRoofPanel, GableRoof, HipRoof>;

// Document-independent authoring codec. Unknown properties are ignored;
// schema 1 forbids openings and schema 2 requires the retained opening roster.
// Entity creation and placement/context resolution belong to the caller.
[[nodiscard]] RoofObject decode_roof_entity(const Entity& entity);
[[nodiscard]] nlohmann::json encode_roof_properties(const RoofObject& object);
[[nodiscard]] TopoDS_Shape make_roof_shape(const RoofObject& object);
} // namespace sketch
