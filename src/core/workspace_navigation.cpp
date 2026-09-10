#include "sketch/workspace_navigation.hpp"
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
void validate_kind(WorkspaceOperationKind kind) {
    switch (kind) {
    case WorkspaceOperationKind::document_edit:
    case WorkspaceOperationKind::boundary_activate:
    case WorkspaceOperationKind::boundary_discard:
    case WorkspaceOperationKind::boundary_finish:
        return;
    }
    throw std::invalid_argument("Unknown workspace operation kind");
}
void validate_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid workspace operation ID");
}
}
WorkspaceNavigationState initialize_workspace_navigation(
    const DocumentSnapshot& document, const WorkspaceHistoryFence& fence) {
    const auto baseline = derive_workspace_baseline_navigation(document, fence);
    WorkspaceNavigationState result;
    result.undo_stack.reserve(baseline.undo_stack.size());
    result.redo_stack.reserve(baseline.redo_stack.size());
    for (const auto& command : baseline.undo_stack)
        result.undo_stack.push_back({command.command_revision});
    for (const auto& command : baseline.redo_stack)
        result.redo_stack.push_back({command.command_revision});
    return result;
}
WorkspaceNavigationState record_workspace_operation(
    const WorkspaceNavigationState& source, const std::string& event_id, WorkspaceOperationKind kind) {
    validate_id(event_id);
    validate_kind(kind);
    if (source.operations.contains(event_id))
        throw std::invalid_argument("Duplicate workspace operation ID");
    auto result = source;
    result.operations.emplace(event_id, kind);
    result.undo_stack.push_back({event_id});
    result.redo_stack.clear();
    return result;
}
WorkspaceNavigationTransition navigate_workspace_history(
    const WorkspaceNavigationState& source, bool redo) {
    const auto& stack = redo ? source.redo_stack : source.undo_stack;
    if (stack.empty()) throw std::invalid_argument("Workspace navigation stack is empty");
    const auto target = stack.back();
    auto kind = WorkspaceOperationKind::document_edit;
    if (const auto* id = std::get_if<std::string>(&target.identity)) {
        validate_id(*id);
        const auto found = source.operations.find(*id);
        if (found == source.operations.end())
            throw std::invalid_argument("Workspace navigation operation is missing");
        kind = found->second;
        validate_kind(kind);
    }
    auto result = source;
    auto& from = redo ? result.redo_stack : result.undo_stack;
    auto& to = redo ? result.undo_stack : result.redo_stack;
    from.pop_back();
    to.push_back(target);
    return {std::move(result), target, kind};
}
WorkspaceNavigationState clear_workspace_redo(const WorkspaceNavigationState& source) {
    auto result = source;
    result.redo_stack.clear();
    return result;
}
}
