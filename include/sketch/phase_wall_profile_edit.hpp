#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"

namespace sketch {

struct WallProfileLayerThickness {
    std::string layer_id;
    Quantity thickness;
};

// Only scalar profile dimensions are editable here. Context, baseline, top
// plane, elevation, layer identity/order and material assignments are retained.
struct WallProfileEditIntent {
    std::string wall_id;
    std::optional<Quantity> thickness;
    std::optional<Quantity> height;
    // If supplied, this is the complete existing layer inventory in its actual
    // order. There is no implicit redistribution when total thickness changes.
    std::optional<std::vector<WallProfileLayerThickness>> layer_thicknesses;
};

// Strict, bounded version-1 codec, with exactly version, wall_id, thickness,
// height and layer_thicknesses. Optional values are explicit nulls; dimensions
// use the existing exact quantity-receipt codec. Empty intents are rejected.
[[nodiscard]] nlohmann::json encode_wall_profile_edit_intent(const WallProfileEditIntent& intent);
[[nodiscard]] WallProfileEditIntent decode_wall_profile_edit_intent(const nlohmann::json& value);

// Detached scalar replay for recognizing a captured desktop edit. Checks wall
// semantics without a hosted graph; full-map replay additionally checks hosted
// opening assemblies and affected saved-active wall joins. Equal dimensions
// preserve the exact original entity.
[[nodiscard]] Entity replay_wall_profile_entity(
    const Entity& source, const WallProfileEditIntent& intent);

// Requires the actual complete copied source, after qualified phase identity
// mapping. Resolves saved-active scope; rejects inactive/duplicate targets.
// Completes active exterior measurements through stable boundary edits, leaving
// physical rooms intact for explicit room completion. The input is immutable.
// Opening assembly and join admission use the shared physical factories with
// actual resolved walls and hosted cuts; their derived solids are discarded.
// False defers final constraint residual admission only; enclosing room replay
// and Document must perform that check on the completed candidate.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_wall_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<WallProfileEditIntent>& intents,
    bool validate_final_constraints = true);

} // namespace sketch
