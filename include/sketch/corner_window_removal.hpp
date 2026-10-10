#pragma once

#include "sketch/document.hpp"

namespace sketch {

using CornerWindowRemovalEntities = std::map<std::string, Entity, std::less<>>;

// Sorted explicit actual owners; cuts are derived from reciprocal source
// ownership. Active baseline owners/cuts are retained and demolished only in
// the saved alternative. The complete source and its history remain immutable.
[[nodiscard]] CornerWindowRemovalEntities replay_corner_window_removal(
    const CornerWindowRemovalEntities& actual,
    const std::vector<std::string>& sorted_corner_owner_ids,
    bool active_phase_constraints = true);

[[nodiscard]] ApplyEntityChanges prepare_corner_window_removal(
    const DocumentSnapshot& source,
    const std::vector<std::string>& sorted_corner_owner_ids,
    const std::string& message);

} // namespace sketch
