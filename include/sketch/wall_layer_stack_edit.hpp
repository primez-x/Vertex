#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"
#include "sketch/wall_semantics.hpp"

namespace sketch {

enum class WallLayerMaterialEditMode { retain, clear, set };

struct WallLayerStackRow {
    std::string layer_id;
    std::optional<Quantity> thickness;
    WallLayerMaterialEditMode material_mode{WallLayerMaterialEditMode::retain};
    std::optional<WallLayerMaterial> material;
};

struct WallLayerStackEditIntent {
    std::string wall_id;
    std::optional<Quantity> thickness;
    std::vector<WallLayerStackRow> layers;
};

// Strict bounded v1. Rows declare the complete resulting order; omitted old
// IDs are removed, and an empty array clears the stack. Null thickness retains
// the actual existing value and receipt. New rows require entered thickness
// and clear/set material. The wall total is retained or explicitly entered.
[[nodiscard]] nlohmann::json encode_wall_layer_stack_edit_intent(const WallLayerStackEditIntent& intent);
[[nodiscard]] WallLayerStackEditIntent decode_wall_layer_stack_edit_intent(const nlohmann::json& value);

// Derive from the complete native source, preserving unaffected JSON exactly.
// Qualified quantity_entries move with layer identity. Removed known receipts
// are retained verbatim in extensions.wall_layer_stack_retirement:
// {"version":1,"receipts":[{"layer_id":ID,"pointer":SOURCE_POINTER,
//                         "receipt":ACTUAL_RECEIPT}, ...]}.
// This append-only archive is limited to 4096 rows and 1 MiB. Unsupported
// affected receipt bindings or archive versions refuse without mutation.
// Singular replay admits actual native profile geometry but cannot resolve
// document catalogs or global identity/context; publication uses map replay.
// Admission of a known archive does not require retired IDs to remain live.
// Map replay resolves actual placement/context and catalogs, admits source and
// result hosted cuts, assembly fit and affected fused joins, and completes
// active exterior measured sources when total thickness changes. Physical rooms
// and inactive owners remain exact. False defers only final constraint residual
// admission to enclosing room review and final Document admission.
void validate_wall_layer_stack_retirement(const nlohmann::json& archive);
[[nodiscard]] Entity replay_wall_layer_stack_entity(const Entity& source, const WallLayerStackEditIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_wall_layer_stack_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<WallLayerStackEditIntent>& intents,
    bool validate_final_constraints = true);

// Candidate capture requires actual new/changed entered-quantity receipts and
// exact independent replay of the complete entity. An authored intent bypasses
// inference, never replay/equality admission. Exact source no-ops return null.
[[nodiscard]] std::optional<WallLayerStackEditIntent> capture_wall_layer_stack_edit(
    const Entity& original, const Entity& candidate,
    const std::optional<WallLayerStackEditIntent>& authored = std::nullopt);

} // namespace sketch
