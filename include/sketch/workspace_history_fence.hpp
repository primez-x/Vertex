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
}
