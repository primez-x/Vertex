#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

struct SlabProfileLayerThickness {
    std::string layer_id;
    Quantity thickness;
};

struct SlabProfileEditIntent {
    std::string slab_id;
    std::optional<Quantity> thickness;
    std::optional<Quantity> elevation;
    std::optional<std::vector<SlabProfileLayerThickness>> layer_thicknesses;
};

// Strict bounded version 1; all three optional inputs have explicit nulls.
// Elevation admits exact signed/zero input; thicknesses must be positive.
[[nodiscard]] nlohmann::json encode_slab_profile_edit_intent(const SlabProfileEditIntent& intent);
[[nodiscard]] SlabProfileEditIntent decode_slab_profile_edit_intent(const nlohmann::json& value);

// Admit the actual native source and bind known receipts to existing fields.
// Unknown receipt versions remain opaque unless the target field changes.
void validate_slab_profile_source_entity(const Entity& source);

// Derive only the entered profile fields from the actual native slab source.
// Retain kind, context, boundaries, holes, materials and opaque data exactly.
// Layers require the complete existing ordered inventory; no redistribution
// or total thickness is inferred. Equal inputs retain the exact source.
[[nodiscard]] Entity replay_slab_profile_entity(const Entity& source, const SlabProfileEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_slab_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<SlabProfileEditIntent>& intents);

// An explicit authored intent or actual candidate receipts supply quantities.
// Numeric-only changes cannot manufacture an entered measurement. The entire
// candidate must equal independent replay, including exact serialized fields.
// Unsupported changes throw; nullopt means an exact source no-op.
[[nodiscard]] std::optional<SlabProfileEditIntent> capture_slab_profile_edit(
    const Entity& original, const Entity& candidate,
    const std::optional<SlabProfileEditIntent>& authored = std::nullopt);

} // namespace sketch
