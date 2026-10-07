#pragma once

#include "sketch/document.hpp"
#include "sketch/boundary_entity.hpp"

namespace sketch {
// Completes only initially current clear-room owners in the merged physical
// context/plane. Identity continuation requires unchanged analytical regions,
// captured source-interval continuation and stable child correspondence. Stale
// and future rooms remain verbatim. Unsupported pinned children or changed
// regions require explicit review and throw; no room identities are minted.
// The caller must independently replay the entire exclusive wall-merge command.
[[nodiscard]] std::map<std::string, Entity, std::less<>> complete_wall_merge_physical_room_sources(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& physical,
    const WallMergeIntent& intent);
// Strict retained geometry-proof replay. The enclosing history replayer must
// additionally chain complete descriptors between operations and reconcile the
// last descriptor with the final room marker; exclusive command replay proves
// live physical source and dependency authority independently.
[[nodiscard]] IdentifiedBoundary replay_physical_room_wall_merge(
    const Entity& owner, const IdentifiedBoundary& preceding,
    const nlohmann::json& value);
} // namespace sketch
