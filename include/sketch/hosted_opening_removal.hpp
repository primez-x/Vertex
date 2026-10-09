#pragma once

#include "sketch/document.hpp"

namespace sketch {

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
