#pragma once

#include "sketch/architectural_drawing_removal.hpp"

namespace sketch {

struct CornerSelectionRemovalIntent {
    // Explicit actual owners only; neither host walls nor child cuts promote
    // into selection authority. The additive lane keeps historical codecs.
    std::vector<std::string> corner_ids;
    ArchitecturalDrawingRemovalIntent other;
    // Version two alone admits actual corner-owner catalog copies and owner
    // dominance. Retained version-one producers keep their original authority.
    bool complete_corner_catalog_hosts{false};
};

using CornerSelectionRemovalEntities = DrawingSelectionRemovalEntities;

[[nodiscard]] nlohmann::json encode_corner_selection_removal_intent(
    const CornerSelectionRemovalIntent& intent);
[[nodiscard]] CornerSelectionRemovalIntent decode_corner_selection_removal_intent(
    const nlohmann::json& value);

// Selection comparison/roof dominance only. This projected inventory must not
// be replayed through the historical architectural-selection removal lane.
[[nodiscard]] ArchitecturalDrawingRemovalIntent corner_selection_removal_authority(
    const CornerSelectionRemovalIntent& intent);

// Independent leaves reconstruct their consequences from the same full actual
// source. No caller-supplied stage confers retirement or reference authority.
[[nodiscard]] CornerSelectionRemovalEntities replay_corner_selection_removal_architectural(
    const CornerSelectionRemovalEntities& actual, const CornerSelectionRemovalIntent& intent,
    bool active_phase_constraints = true);

// Snapshot owner reserves all roof destinations against retained history, then
// delegates selected drawing retirement under the new lane's active policy.
// The factory still admits its raw command under the actual captured policy.
[[nodiscard]] CornerSelectionRemovalEntities replay_corner_selection_removal(
    const DocumentSnapshot& source, const CornerSelectionRemovalIntent& intent);
[[nodiscard]] ApplyEntityChanges prepare_corner_selection_removal(
    const DocumentSnapshot& source, const CornerSelectionRemovalIntent& intent,
    const std::string& message);

} // namespace sketch
