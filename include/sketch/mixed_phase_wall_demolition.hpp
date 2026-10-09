#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Prepare one raw registry upsert for the existing phase-room review. Both
// lanes independently replay the same complete captured source. No qualifying
// baseline wall returns nullopt; qualifying walls retain the wall facade's
// protection and same saved registry/alternative rules.
//
// The optional command must be canonical outer34 semantic demolition, with
// full captured revision/history/save/digest bindings. Only active demolition
// additions in that same registry are admitted. Bodies, catalogs, aliases,
// membership order, metadata and all other alternatives remain exact. Ordinary
// retirement, fresh destinations and competing edit authority refuse.
//
// This incomplete stage is not Document publication authority. The caller must
// retain the full captured-source fence, complete explicit phase-room review,
// and preview/apply that complete result exactly once.
[[nodiscard]] std::optional<ApplyEntityChanges> prepare_mixed_phase_wall_demolition(
    const DocumentSnapshot& source, const std::vector<std::string>& wall_ids,
    const std::optional<ApplyBoundaryConstraintChanges>& other_demolition,
    const std::string& message);

} // namespace sketch
