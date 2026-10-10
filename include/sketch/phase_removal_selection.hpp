#pragma once

#include "sketch/architectural_selection_removal.hpp"

namespace sketch {

// Resource and known-demolition-grammar screening before a retained command
// decoder allocates its typed leaves. This does not grant source authority.
void preflight_phase_removal_selection_proof(const nlohmann::json& proof);

// Extract only explicit ordinary Delete selections from a closed, canonical
// outer34 demolition proof bound to the complete actual captured snapshot.
// Returned object IDs and qualified component keys are ascending and unique;
// their combined count is at most 1000. Derived dependents, room decisions and
// fresh destinations remain in the original proof, never selection authority.
// This performs no native geometry work or Document preview. The caller must
// compare the exact captured selection and independently preview the original
// complete command before publication. Unsupported/edit proofs throw.
[[nodiscard]] ArchitecturalSelectionRemovalIntent phase_removal_selection_authority(
    const DocumentSnapshot& source,
    const ApplyBoundaryConstraintChanges& pure_phase_command);

} // namespace sketch
