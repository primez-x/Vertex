#include "sketch/workspace_navigation.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace sketch;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejected(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid navigation was accepted");
}
WorkspaceNavigationTarget revision(Revision value) { return {value}; }
WorkspaceNavigationTarget event(const std::string& value) { return {value}; }
void baseline_commands() {
    auto document = Document::create();
    const auto empty = initialize_workspace_navigation(document.snapshot(),
        capture_workspace_history_fence(document.snapshot()));
    require(empty == WorkspaceNavigationState{}, "Empty baseline invented navigation");
    document.apply(NameRevision{0, "First"});
    document.apply(NameRevision{1, "Second"});
    document.undo(2);
    document.redo(3);
    document.undo(4);
    const auto snapshot = document.snapshot();
    const auto state = initialize_workspace_navigation(snapshot, capture_workspace_history_fence(snapshot));
    require(state.undo_stack == std::vector{revision(1)} &&
        state.redo_stack == std::vector{revision(2)} && state.operations.empty(),
        "Imported baseline used navigation revisions instead of original commands");
    auto current = state;
    for (int repeat = 0; repeat < 4; ++repeat) {
        auto forward = navigate_workspace_history(current, true);
        require(forward.target == revision(2) && forward.kind == WorkspaceOperationKind::document_edit,
            "Imported redo lost command identity");
        auto backward = navigate_workspace_history(forward.state, false);
        require(backward.target == revision(2) && backward.state == state,
            "Repeated baseline navigation changed identity or stacks");
        current = backward.state;
    }
    auto bad_fence = capture_workspace_history_fence(snapshot);
    bad_fence.redo_stack.clear();
    rejected([&] { (void)initialize_workspace_navigation(snapshot, bad_fence); });
}
void lifecycle_and_branches() {
    auto document = Document::create();
    document.apply(NameRevision{0, "Baseline"});
    auto state = initialize_workspace_navigation(document.snapshot(),
        capture_workspace_history_fence(document.snapshot()));
    const std::vector<std::pair<std::string, WorkspaceOperationKind>> commands{
        {"activate", WorkspaceOperationKind::boundary_activate},
        {"edit", WorkspaceOperationKind::document_edit},
        {"discard", WorkspaceOperationKind::boundary_discard},
        {"finish", WorkspaceOperationKind::boundary_finish}};
    for (const auto& [id, kind] : commands) state = record_workspace_operation(state, id, kind);
    const auto full = state;
    for (auto item = commands.rbegin(); item != commands.rend(); ++item) {
        const auto transition = navigate_workspace_history(state, false);
        require(transition.target == event(item->first) && transition.kind == item->second,
            "Lifecycle undo order or kind changed");
        state = transition.state;
    }
    const auto baseline = navigate_workspace_history(state, false);
    require(baseline.target == revision(1), "Lifecycle undo failed to reach baseline");
    state = navigate_workspace_history(baseline.state, true).state;
    for (const auto& [id, kind] : commands) {
        const auto transition = navigate_workspace_history(state, true);
        require(transition.target == event(id) && transition.kind == kind,
            "Lifecycle redo order or kind changed");
        state = transition.state;
    }
    require(state == full, "Lifecycle round trip changed registry or stacks");
    state = navigate_workspace_history(state, false).state;
    const auto barrier = clear_workspace_redo(state);
    require(barrier.redo_stack.empty() && barrier.undo_stack == state.undo_stack &&
        barrier.operations == full.operations && !state.redo_stack.empty(),
        "Redo barrier changed immutable history or source");
    rejected([&] { (void)navigate_workspace_history(barrier, true); });
    rejected([&] { (void)record_workspace_operation(barrier, "finish", WorkspaceOperationKind::boundary_finish); });
    const auto branch = record_workspace_operation(state, "branch", WorkspaceOperationKind::document_edit);
    require(branch.redo_stack.empty() && branch.operations.contains("finish") &&
        branch.undo_stack.back() == event("branch"), "Branch failed to abandon redo and preserve registry");
    require(navigate_workspace_history(branch, false).target == event("branch"), "Branch undo chose old redo");

    document.undo(document.revision());
    const auto imported = initialize_workspace_navigation(document.snapshot(),
        capture_workspace_history_fence(document.snapshot()));
    const auto cleared = clear_workspace_redo(imported);
    require(!document.snapshot().history().back().redo_stack.empty(), "Fixture needs raw document redo");
    rejected([&] { (void)navigate_workspace_history(cleared, true); });
}
void invalid_operations_preserve_source() {
    const auto state = record_workspace_operation({}, std::string(128, 'a'), WorkspaceOperationKind::document_edit);
    const auto before = state;
    for (const auto& id : std::vector<std::string>{"", std::string(129, 'a'), std::string("a\0b", 3), std::string(128, 'a')})
        rejected([&] { (void)record_workspace_operation(state, id, WorkspaceOperationKind::document_edit); });
    rejected([&] { (void)record_workspace_operation(state, "invalid-kind", static_cast<WorkspaceOperationKind>(999)); });
    rejected([&] { (void)navigate_workspace_history(state, true); });
    rejected([&] { (void)navigate_workspace_history({}, false); });
    require(state == before, "Rejected operation mutated its source");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { baseline_commands(); lifecycle_and_branches(); invalid_operations_preserve_source(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
