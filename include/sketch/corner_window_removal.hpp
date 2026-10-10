#pragma once

#include "sketch/document.hpp"

namespace sketch {

using CornerWindowRemovalEntities = std::map<std::string, Entity, std::less<>>;

// Sorted explicit actual owners; cuts are derived from reciprocal source
// ownership. Active baseline owners/cuts are retained and demolished only in
// the saved alternative. The complete source and its history remain immutable.
// Catalog-host completion is separate authority; the default retains historical
// corner removal admission and native budgets.
[[nodiscard]] CornerWindowRemovalEntities replay_corner_window_removal(
    const CornerWindowRemovalEntities& actual,
    const std::vector<std::string>& sorted_corner_owner_ids,
    bool active_phase_constraints = true,
    bool complete_corner_catalog_hosts = false);

[[nodiscard]] ApplyEntityChanges prepare_corner_window_removal(
    const DocumentSnapshot& source,
    const std::vector<std::string>& sorted_corner_owner_ids,
    const std::string& message,
    bool complete_corner_catalog_hosts = false);

} // namespace sketch
