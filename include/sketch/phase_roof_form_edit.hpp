#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

enum class RoofForm { sloped_roof_panel, gable_roof, hip_roof };

struct RoofFormEditIntent {
    std::string roof_id;
    RoofForm target_form{RoofForm::sloped_roof_panel};
    // All five exact entered dimensions are required. Length is horizontal
    // run for a panel and plan length for a gable/hip. There is no fallback.
    Quantity length;
    Quantity span;
    Quantity rise;
    Quantity overhang;
    Quantity thickness;
};

// Closed version one: version, roof_id, target_form, length, span, rise,
// overhang, thickness. Each dimension is a strict exact quantity receipt.
// Overhang permits zero; rise permits zero only for a sloped panel.
[[nodiscard]] nlohmann::json encode_roof_form_edit_intent(const RoofFormEditIntent& intent);
[[nodiscard]] RoofFormEditIntent decode_roof_form_edit_intent(const nlohmann::json& value);

// Every stage reads the admitted actual original. Only form/profile scalars
// and their schema-known receipts change. Pose, schema version, child roster,
// context, materials and opaque metadata remain retained. A changed form is
// mandatory; same-form scalar work belongs to RoofProfileEditIntent.
// Staging defers final native cut-fit admission for atomic roster/pose edits.
[[nodiscard]] Entity stage_roof_form_entity(const Entity& actual_source, const RoofFormEditIntent& intent);
[[nodiscard]] Entity replay_roof_form_entity(const Entity& actual_source, const RoofFormEditIntent& intent);

// Infer target form and all five actual target dimensions from understood
// exact candidate receipts. No companion fields acquire authority. Same form
// returns nullopt so existing profile contracts continue to apply.
[[nodiscard]] std::optional<RoofFormEditIntent> infer_roof_form_edit(
    const Entity& original, const Entity& candidate);

// Retain only source-equivalent numeric/input representation, preserving all
// opaque siblings. Changed-form target receipts are authored, not normalized
// away. No fabricated intermediate source is used for admission.
[[nodiscard]] Entity normalize_equivalent_roof_form_inputs(
    const Entity& original, const Entity& candidate);

// Full independent native replay and Entity/property/extension dumps must
// match. Accompanying changes are refused. Nullopt is an equivalent no-op.
[[nodiscard]] std::optional<RoofFormEditIntent> capture_roof_form_edit(
    const Entity& original, const Entity& candidate);

} // namespace sketch
