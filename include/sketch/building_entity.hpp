#pragma once

#include "sketch/building_objects.hpp"
#include "sketch/document.hpp"

#include <nlohmann/json.hpp>

#include <string_view>
#include <variant>

namespace sketch {

// The entity codec is the single semantic boundary for the supported
// architectural object forms.  BRep geometry remains a derived value created
// by make_building_shape; the variant and its Entity properties are the
// persisted authoring data.
using BuildingObject = std::variant<RectangularColumn, CircularColumn, Beam,
                                    StairFlight, Railing, SlopedRoofPanel, GableRoof, HipRoof>;

// This predicate only recognizes the canonical entity type vocabulary.  A
// true result does not imply that an Entity has a supported version, form, or
// valid geometry; decode_building_entity performs those stricter checks.
[[nodiscard]] bool can_recognize_building_entity_type(std::string_view type) noexcept;

// Encode creates a fresh Entity through Entity::create.  If the variant's id
// is nonempty it replaces the generated id so stable authoring identity is
// preserved.  Metadata must be a JSON object and is stored in Entity's
// extension object; unrelated semantic properties are never copied into the
// canonical output.
[[nodiscard]] Entity encode_building_entity(
    const BuildingObject& object,
    nlohmann::json metadata = nlohmann::json::object());

// Decode accepts only the canonical schema emitted by encode.  Unknown
// properties and extension metadata are ignored, while required fields,
// finite numbers, exact coordinate arrays, version, form, and geometry are
// validated before the variant is returned.
[[nodiscard]] BuildingObject decode_building_entity(const Entity& entity);

// Dispatch to the existing OCCT builders.  This is also the geometry gate
// used by encoding and decoding before a caller can create an authoring
// command.
[[nodiscard]] TopoDS_Shape make_building_shape(const BuildingObject& object);

}  // namespace sketch
