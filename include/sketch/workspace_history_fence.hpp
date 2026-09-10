#pragma once
#include "sketch/document.hpp"

namespace sketch {
// References retained Document history without storing duplicate entity states.
struct WorkspaceHistoryFence {
    std::string document_id;
    Revision baseline_revision{};
    std::string source_digest;
    std::vector<Revision> undo_stack;
    std::vector<Revision> redo_stack;
    bool operator==(const WorkspaceHistoryFence&) const = default;
};
[[nodiscard]] WorkspaceHistoryFence capture_workspace_history_fence(const DocumentSnapshot&);
// Validates the whole document before checking the historical binding. Does
// not navigate history, normalize stacks or grant workspace mutation authority.
void validate_workspace_history_fence(const DocumentSnapshot&, const WorkspaceHistoryFence&);

// Document navigation targets a saved snapshot, which can itself be an undo
// or redo revision. Keep that target separate from the original command ID.
struct WorkspaceBaselineCommandReference {
    Revision command_revision{};
    Revision snapshot_revision{};
    bool operator==(const WorkspaceBaselineCommandReference&) const = default;
};
struct WorkspaceBaselineNavigation {
    std::vector<WorkspaceBaselineCommandReference> undo_stack;
    std::vector<WorkspaceBaselineCommandReference> redo_stack;
    bool operator==(const WorkspaceBaselineNavigation&) const = default;
};
// Derives both logical command stacks from the validated retained prefix and
// pairs them with the fence's physical Document snapshot targets. No entity
// copies are persisted and no new post-fence operation IDs are invented.
[[nodiscard]] WorkspaceBaselineNavigation derive_workspace_baseline_navigation(
    const DocumentSnapshot&, const WorkspaceHistoryFence&);
}
