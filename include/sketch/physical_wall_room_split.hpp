#pragma once

#include "sketch/document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/physical_room_split_ids.hpp"

namespace sketch {
// The physical map has already undergone the authoritative wall partition.
// Preparation and completion share the same analytical correspondence; only
// preparation allocates identities. Both refuse changed or ambiguous regions.
// The default retains historical exact-lineage command meaning. New versioned
// commands may explicitly continue phase bookkeeping alone; that continuation
// captures an independently admitted current-source descriptor in a v2 proof.
[[nodiscard]] std::vector<WallSplitPhysicalRoomIds> prepare_wall_split_physical_room_ids(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& physical,
    const WallSplitIntent& intent, bool allow_phase_metadata_refresh = false);
[[nodiscard]] std::map<std::string, Entity, std::less<>> complete_wall_split_physical_room_sources(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& physical,
    const WallSplitIntent& intent, bool allow_phase_metadata_refresh = false);
// Intrinsic strict retained proof. The enclosing history replayer additionally
// chains descriptors and reconciles the final marker; command replay proves
// live physical source authority and lifetime freshness of the frozen IDs.
// V1 remains exact; v2 additionally admits the captured phase-only refresh.
[[nodiscard]] IdentifiedBoundary replay_physical_room_wall_split(
    const Entity& owner, const IdentifiedBoundary& preceding,
    const nlohmann::json& value);
} // namespace sketch
