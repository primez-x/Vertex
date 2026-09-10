#pragma once
#include "sketch/workspace_history_fence.hpp"
#include <map>
#include <variant>

namespace sketch {
enum class WorkspaceOperationKind {
    document_edit, boundary_activate, boundary_discard, boundary_finish
};
struct WorkspaceNavigationTarget {
    std::variant<Revision, std::string> identity;
    bool operator==(const WorkspaceNavigationTarget&) const = default;
};
// Trusted runtime state, not a validator for a persisted event ledger. Revision
// identities name original baseline commands, never navigation snapshots.
struct WorkspaceNavigationState {
    std::vector<WorkspaceNavigationTarget> undo_stack;
    std::vector<WorkspaceNavigationTarget> redo_stack;
    std::map<std::string, WorkspaceOperationKind, std::less<>> operations;
    bool operator==(const WorkspaceNavigationState&) const = default;
};
struct WorkspaceNavigationTransition {
    WorkspaceNavigationState state;
    WorkspaceNavigationTarget target;
    WorkspaceOperationKind kind;
};
[[nodiscard]] WorkspaceNavigationState initialize_workspace_navigation(
    const DocumentSnapshot&, const WorkspaceHistoryFence&);
[[nodiscard]] WorkspaceNavigationState record_workspace_operation(
    const WorkspaceNavigationState&, const std::string& event_id, WorkspaceOperationKind);
[[nodiscard]] WorkspaceNavigationTransition navigate_workspace_history(
    const WorkspaceNavigationState&, bool redo);
// Semantic local updates abandon global redo without deleting immutable IDs.
[[nodiscard]] WorkspaceNavigationState clear_workspace_redo(const WorkspaceNavigationState&);
}
