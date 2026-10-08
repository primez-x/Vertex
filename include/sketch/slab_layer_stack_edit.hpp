#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"
#include "sketch/slab_semantics.hpp"

namespace sketch {

enum class SlabLayerMaterialEditMode { retain, clear, set };

struct SlabLayerStackRow {
    std::string layer_id;
    std::optional<Quantity> thickness;
    SlabLayerMaterialEditMode material_mode{SlabLayerMaterialEditMode::retain};
    std::optional<WallLayerMaterial> material;
};

struct SlabLayerStackEditIntent {
    std::string slab_id;
    std::optional<Quantity> thickness;
    std::vector<SlabLayerStackRow> layers;
};

// Strict bounded v1. Rows declare the complete resulting order; omitted old
// IDs are removed, and an empty array clears the stack. Null thickness retains
// the actual existing value and receipt. New rows require entered thickness
// and clear/set material. The slab total is retained or explicitly entered.
[[nodiscard]] nlohmann::json encode_slab_layer_stack_edit_intent(const SlabLayerStackEditIntent& intent);
[[nodiscard]] SlabLayerStackEditIntent decode_slab_layer_stack_edit_intent(const nlohmann::json& value);

// Derive from the complete native source, preserving unaffected JSON exactly.
// Qualified quantity_entries move with layer identity. Removed known receipts
// are retained verbatim in extensions.slab_layer_stack_retirement:
// {"version":1,"receipts":[{"layer_id":ID,"pointer":SOURCE_POINTER,
//                         "receipt":ACTUAL_RECEIPT}, ...]}.
// This append-only archive is limited to 4096 rows and 1 MiB. Unsupported
// affected receipt bindings or archive versions refuse without mutation.
// Singular replay admits geometry but cannot resolve document catalogs or
// global identity/context; publication must use the map replay below.
// Admission of a known archive does not require retired IDs to remain live.
void validate_slab_layer_stack_retirement(const nlohmann::json& archive);
[[nodiscard]] Entity replay_slab_layer_stack_entity(const Entity& source, const SlabLayerStackEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_slab_layer_stack_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<SlabLayerStackEditIntent>& intents);

// Candidate capture requires actual new/changed entered-quantity receipts and
// exact independent replay of the complete entity. An authored intent bypasses
// inference, never replay/equality admission. Exact source no-ops return null.
[[nodiscard]] std::optional<SlabLayerStackEditIntent> capture_slab_layer_stack_edit(
    const Entity& original, const Entity& candidate,
    const std::optional<SlabLayerStackEditIntent>& authored = std::nullopt);

} // namespace sketch
