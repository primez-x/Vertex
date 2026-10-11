#pragma once

#include "sketch/architectural_selection_removal.hpp"

namespace sketch {

struct OpeningArchitecturalRemovalIntent {
    std::vector<std::string> opening_ids;
    ArchitecturalSelectionRemovalIntent other;
    // Separate qualified hosted-extent retirement authority. Historical/default
    // opening intents retain their original dimension consequences.
    bool complete_component_dimensions = false;
};

// Closed complete-actual-map composition. Both lanes are required, and each
// producer sees the same immutable source. No wall or independent assembly
// deletion authority and no arbitrary change/proof authority is granted.
[[nodiscard]] RoofRemovalEntities replay_opening_architectural_removal(
    const RoofRemovalEntities& actual, const OpeningArchitecturalRemovalIntent& intent,
    bool active_phase_constraints);

// Real captured-snapshot admission, deterministic asset-free raw delta and
// exact preview/geometry validation. Live publication retains the source fence.
[[nodiscard]] ApplyEntityChanges prepare_opening_architectural_removal(
    const DocumentSnapshot& source, const OpeningArchitecturalRemovalIntent& intent,
    const std::string& message);

} // namespace sketch
