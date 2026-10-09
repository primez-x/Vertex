#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Actual-map replay of the same bounded semantic opening retirement below.
// The caller supplies the complete actual source and its captured constraint
// validation policy; phase registries alone cannot determine history policy.
// Empty/non-opening selections return nullopt; mixed selections containing an
// opening refuse. The result is the complete candidate after shared source and
// candidate preflight/native admission, never a filtered or fabricated snapshot.
// This facade has no history, assets, revision or arbitrary-change authority.
// The outer command must retain real Document policy admission and complete
// architectural geometry validation before applying the composed candidate.
[[nodiscard]] std::optional<std::map<std::string, Entity, std::less<>>>
replay_hosted_opening_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<std::string>& selected_opening_ids,
    bool active_phase_constraints);

// Pure, bounded physical retirement of actual semantic opening entities from
// the full captured source. Empty/non-opening selections return nullopt; a
// mixed selection containing an opening refuses. Shared baseline openings are
// reserved for phase_opening_demolition_command, never physically removed.
// Retires supported dimensions, constraints, catalog rows and presentation
// memberships atomically. Required/protected owners and affected opaque
// references refuse. Walls, rooms/lineage, raw catalog definitions, survivors
// and assets remain intact. The caller must retain its full source/selection
// fence through Delete/Cut publication; the command captures source.revision().
[[nodiscard]] std::optional<ApplyEntityChanges> prepare_hosted_opening_removal(
    const DocumentSnapshot& source,
    const std::vector<std::string>& selected_opening_ids,
    const std::string& message);

} // namespace sketch
