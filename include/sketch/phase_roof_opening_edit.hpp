#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

struct RoofOpeningQuantityInput {
    Quantity quantity;
    Unit default_unit{Unit::metre};
};

struct RoofOpeningCloneSource {
    // Captured passive envelope: it supplies only the named child's content,
    // never destination owner, pose, context, or roster authority.
    Entity roof;
    std::string opening_id;
};

struct RoofOpeningUpsertIntent {
    std::string opening_id;
    std::optional<RoofOpeningQuantityInput> x;
    std::optional<RoofOpeningQuantityInput> y;
    std::optional<RoofOpeningQuantityInput> width;
    std::optional<RoofOpeningQuantityInput> depth;
    // Disengaged retains the actual profile; engaged null removes it. An
    // engaged object authors the strict version-one skylight profile.
    std::optional<nlohmann::json> skylight;
    std::optional<RoofOpeningCloneSource> clone_source;
};

struct RoofOpeningEditIntent {
    std::string roof_id;
    std::vector<RoofOpeningUpsertIntent> upserts;
    std::vector<std::string> removed_opening_ids;
    // Retain an explicitly decoded v2 proof even when every profile is retained.
    bool uses_skylight_schema{false};
    // Retain an explicitly decoded v3 proof even when no transfer is authored.
    bool uses_clone_schema{false};
};

// Strict version 1: exactly version, roof_id, upserts, removed_opening_ids.
// Each upsert has exactly opening_id, x, y, width, depth; null retains an
// existing scalar. A new identity requires all four quantities. Coordinates
// permit signed/zero input; width/depth are positive and native fit is required.
// A nonnull field has exactly quantity (strict quantity receipt), default_unit.
// Strict v2 retains the four top-level fields and adds skylight_edit to every
// upsert. null retains; exactly {value:null} removes; {value:profile} authors.
// Profile has exactly version:1, frame_width_m, curb_height_m,
// glazing_thickness_m. Schema-three roster edits require v2, including removals.
// Strict v3 adds clone_source to every upsert: null retains ordinary authority;
// {roof:{id,type,properties,required,extensions},opening_id} transfers the
// named actual skylight child to a fresh identity with all four inputs and its
// exact nonnull profile. Known child/indexed receipt ownership is retargeted;
// row and receipt opaque siblings remain exact. Future/ambiguous bindings refuse.
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

// Source-admitted staging defers the final envelope fit so a composite roof
// resize/cut edit can be admitted once. It confers no candidate/source authority.
[[nodiscard]] Entity stage_roof_opening_entity(const Entity& actual_source, const RoofOpeningEditIntent& intent);
// Form-aware inference/normalization is reserved for complete composite replay;
// standalone capture keeps the default same-form policy and gains no form authority.
[[nodiscard]] std::optional<RoofOpeningEditIntent> infer_roof_opening_edit(
    const Entity& original, const Entity& candidate, bool allow_form_change = false);
[[nodiscard]] Entity normalize_equivalent_roof_opening_inputs(
    const Entity& original, const Entity& candidate, bool allow_form_change = false);

// Captures actual roster changes only. Changed/new fields require exact child
// input receipts, including their actual parsing default unit. The complete
// candidate must equal independent replay after equal scalar normalization.
// Clone transfers require an explicit v3 intent; candidate inference cannot
// reconstruct their captured passive source authority.
[[nodiscard]] std::optional<RoofOpeningEditIntent> capture_roof_opening_edit(
    const Entity& original, const Entity& candidate);

// Source-derived, injective fresh IDs for the caller's retained-history guard.
// Every fresh ID is checked against all entity IDs, opaque JSON strings/keys,
// current roof children and overlays in the complete source map, plus all
// captured passive clone envelopes in the batch.
[[nodiscard]] std::vector<std::string> new_roof_opening_identity_ids(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofOpeningEditIntent>& intents);
} // namespace sketch
