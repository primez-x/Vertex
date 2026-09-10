#pragma once

#include "sketch/workspace_lifecycle_validation.hpp"

namespace sketch {
// Validates lifecycle ordering, archives, sources, and active/retired slot
// replay, finish Document deltas, and reachable redo. Does not validate
// aggregate admission or grant mutation authority.
void validate_workspace_lifecycle_slots(
    const DocumentSnapshot&, const WorkspaceDocumentHistory&,
    const std::vector<WorkspaceLifecycleEvent>&, const WorkspaceNavigationState&,
    const std::optional<BoundaryActiveRecovery>&, const WorkspaceRetiredBoundaries&,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy);
}
