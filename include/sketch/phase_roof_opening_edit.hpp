#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

struct RoofOpeningQuantityInput {
    Quantity quantity;
    Unit default_unit{Unit::metre};
};

struct RoofOpeningUpsertIntent {
    std::string opening_id;
    std::optional<RoofOpeningQuantityInput> x;
    std::optional<RoofOpeningQuantityInput> y;
    std::optional<RoofOpeningQuantityInput> width;
    std::optional<RoofOpeningQuantityInput> depth;
};

struct RoofOpeningEditIntent {
    std::string roof_id;
    std::vector<RoofOpeningUpsertIntent> upserts;
    std::vector<std::string> removed_opening_ids;
};

// Strict version 1: exactly version, roof_id, upserts, removed_opening_ids.
// Each upsert has exactly opening_id, x, y, width, depth; null retains an
// existing scalar. A new identity requires all four quantities. Coordinates
// permit signed/zero input; width/depth are positive and native fit is required.
// A nonnull field has exactly quantity (strict quantity receipt), default_unit.
[[nodiscard]] nlohmann::json encode_roof_opening_edit_intent(const RoofOpeningEditIntent& intent);
[[nodiscard]] RoofOpeningEditIntent decode_roof_opening_edit_intent(const nlohmann::json& value);

// Pure same-form roster replay. Existing row/owner opaque fields, profile,
// pose and context remain exact. Schema-owned indexed quantity receipts follow
// their actual stable child; unrelated quantity_entries remain exact. Changed
// child receipts retain opaque siblings; removal owns only its declared key.
[[nodiscard]] Entity replay_roof_opening_entity(const Entity& source, const RoofOpeningEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_roof_opening_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofOpeningEditIntent>& intents);

// Captures actual roster changes only. Changed/new fields require exact child
// input receipts, including their actual parsing default unit. The complete
// candidate must equal independent replay after equal scalar normalization.
[[nodiscard]] std::optional<RoofOpeningEditIntent> capture_roof_opening_edit(
    const Entity& original, const Entity& candidate);

// Source-derived, injective fresh IDs for the caller's retained-history guard.
// Every fresh ID is checked against all entity IDs, opaque JSON strings/keys,
// current roof children and overlays in the complete source map.
[[nodiscard]] std::vector<std::string> new_roof_opening_identity_ids(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofOpeningEditIntent>& intents);
} // namespace sketch
