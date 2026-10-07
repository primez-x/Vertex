#pragma once

#include "sketch/document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/physical_room_split_ids.hpp"

namespace sketch {
// The physical map has already undergone the authoritative wall partition.
// Preparation and completion share the same analytical correspondence; only
// preparation allocates identities. Both refuse changed or ambiguous regions.
[[nodiscard]] std::vector<WallSplitPhysicalRoomIds> prepare_wall_split_physical_room_ids(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& physical,
    const WallSplitIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> complete_wall_split_physical_room_sources(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& physical,
    const WallSplitIntent& intent);
// Intrinsic strict retained proof. The enclosing history replayer additionally
// chains descriptors and reconciles the final marker; command replay proves
// live physical source authority and lifetime freshness of the frozen IDs.
[[nodiscard]] IdentifiedBoundary replay_physical_room_wall_split(
    const Entity& owner, const IdentifiedBoundary& preceding,
    const nlohmann::json& value);
} // namespace sketch
