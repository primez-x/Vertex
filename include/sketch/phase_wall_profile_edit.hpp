#pragma once

#include "sketch/document.hpp"
#include "sketch/quantity.hpp"
#include <set>

namespace sketch {

struct WallProfileLayerThickness {
    std::string layer_id;
    Quantity thickness;
};

// Scalar dimensions and signed start-to-end top rise are editable here.
// Context, baseline, elevation, layer identity/order and materials are retained.
struct WallProfileEditIntent {
    std::string wall_id;
    std::optional<Quantity> thickness;
    std::optional<Quantity> height;
    // If supplied, this is the complete existing layer inventory in its actual
    // order. There is no implicit redistribution when total thickness changes.
    std::optional<std::vector<WallProfileLayerThickness>> layer_thicknesses;
    // Derives a planar top from the captured baseline and resulting start
    // height. Zero clears a retained slope plane; equal effective rise retains
    // the exact source. This signed quantity has no positive-dimension policy.
    std::optional<Quantity> top_rise;
};

// Strict, bounded version-1 codec with exactly version, wall_id, thickness,
// height and layer_thicknesses. Version 2 adds exactly a nonnull top_rise;
// absent top rise retains version 1. Optional dimensions are explicit nulls;
// all quantities use exact receipts. Empty intents are rejected.
[[nodiscard]] nlohmann::json encode_wall_profile_edit_intent(const WallProfileEditIntent& intent);
[[nodiscard]] WallProfileEditIntent decode_wall_profile_edit_intent(const nlohmann::json& value);

// Detached scalar replay for recognizing a captured desktop edit. Checks wall
// semantics without a hosted graph; full-map replay additionally checks hosted
// opening assemblies and affected saved-active wall joins. Equal dimensions
// and effective top rise preserve the exact original entity. A changed rise
// retains the start-height anchor and replaces only a supported top plane.
[[nodiscard]] Entity replay_wall_profile_entity(
    const Entity& source, const WallProfileEditIntent& intent);

// Admit actual saved-active wall solids, every hosted cut/assembly and affected
// fused joins from a complete entity map. Derived solids are discarded. Used
// by profile/opening replay and detached geometry preview preparation.
void validate_active_wall_physical_dependencies(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::set<std::string, std::less<>>& affected_wall_ids,
    // Recorded earlier dialects retain their original admission. Current
    // authoring and new dialects opt into complete profile/receipt admission
    // for targets and every affected join member.
    bool strict_profiles = false);

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
    bool validate_final_constraints = true,
    bool strict_current_profiles = false);

} // namespace sketch
