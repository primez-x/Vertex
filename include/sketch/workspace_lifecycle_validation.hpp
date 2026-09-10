#pragma once

#include "sketch/project_workspace.hpp"

namespace sketch {
// Validates the complete Document history/baseline, lifecycle event ordering,
// structural payload presence, exact Document-event projection, and replayed
// navigation stacks and operation registry. Throws invalid_argument on invalid
// input. Does not validate archived input contents, source/session bindings,
// active/retired slots, or storage admission; grants no mutation authority.
void validate_workspace_lifecycle_order(
    const DocumentSnapshot&, const WorkspaceDocumentHistory&,
    const std::vector<WorkspaceLifecycleEvent>&, const WorkspaceNavigationState&);
// Validates canonical input owners, direct backward references, pointer
// overrides and retired finish provenance. Per-owner policy only; aggregate
// admission and replay of active/retired slots are separate requirements.
void validate_workspace_archival_inputs(
    const std::vector<WorkspaceLifecycleEvent>&,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy);
// Checks historical bindings for activation, archived and current input, with
// activation bound to its event revision. Does not replay session slots.
void validate_workspace_recovery_sources(
    const DocumentSnapshot&, const std::vector<WorkspaceLifecycleEvent>&,
    const std::optional<BoundaryActiveRecovery>& active);
// Rebuilds each finish's geometric effect from its archived input and compares
// the retained document delta. No live document or workspace is modified.
void validate_workspace_finish_deltas(
    const DocumentSnapshot&, const std::vector<WorkspaceLifecycleEvent>&,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy);
}
