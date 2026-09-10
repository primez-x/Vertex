#pragma once
#include "sketch/workspace_slot_validation.hpp"

namespace sketch {
// Conservative engineering accounting, not an RSS or elapsed-time guarantee.
// These limits supplement per-checkpoint limits and the storage wire preflight.
struct WorkspaceRecoveryLimits {
    std::size_t max_events{10'000};
    std::size_t max_inputs{1'000};
    std::size_t max_actions{100'000};
    std::size_t max_encoded_bytes{64U * 1024U * 1024U};
    std::size_t max_json_values{2'000'000};
    std::size_t max_string_bytes{16U * 1024U * 1024U};
    std::size_t max_replay_work{20'000'000};
    std::size_t max_document_revisions{10'000};
    std::size_t max_entity_rows{250'000};
    std::size_t max_asset_rows{100'000};
    std::size_t max_asset_bytes{512U * 1024U * 1024U};
    std::size_t max_validation_work{200'000'000};
};
struct WorkspaceRecoveryUsage {
    std::size_t events{}, inputs{}, actions{}, encoded_bytes{}, json_values{}, string_bytes{}, replay_work{};
    std::size_t document_revisions{}, entity_rows{}, asset_rows{}, asset_bytes{}, validation_work{};
};
// Walks borrowed values before document forks, canonical checkpoint replay or
// aggregate copies. Shared immutable inputs are charged once by object identity;
// active copies and distinct substituted reference payloads are charged too.
[[nodiscard]] WorkspaceRecoveryUsage preflight_workspace_recovery(
    const DocumentSnapshot&, const WorkspaceDocumentHistory&, const std::vector<WorkspaceLifecycleEvent>&,
    const WorkspaceNavigationState&, const std::optional<BoundaryActiveRecovery>&,
    const WorkspaceRetiredBoundaries&, const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy,
    const WorkspaceRecoveryLimits& = {});
// Mandatory ordering for callers validating untrusted typed recovery state.
void validate_workspace_recovery(
    const DocumentSnapshot&, const WorkspaceDocumentHistory&, const std::vector<WorkspaceLifecycleEvent>&,
    const WorkspaceNavigationState&, const std::optional<BoundaryActiveRecovery>&,
    const WorkspaceRetiredBoundaries&, const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy,
    const WorkspaceRecoveryLimits& = {});
}
