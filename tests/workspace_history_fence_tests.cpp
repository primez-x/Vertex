#include "sketch/workspace_history_fence.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
template<class F> void rejected(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid history fence was accepted");
}
void check() {
    auto document = Document::create();
    document.apply(NameRevision{document.revision(), "Named"});
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "Draft"}}))}});
    document.undo(document.revision());
    const auto fence = capture_workspace_history_fence(document.snapshot());
    if (fence.redo_stack.empty()) throw std::runtime_error("fixture needs retained redo");
    document.redo(document.revision());
    document.apply(NameRevision{document.revision(), "Later name"});
    document.mark_saved(document.revision());
    const auto current = document.snapshot();
    validate_workspace_history_fence(current, fence);
    auto bad = fence; bad.redo_stack.clear();
    rejected([&] { validate_workspace_history_fence(current, bad); });
    bad = fence; bad.undo_stack.push_back(0);
    rejected([&] { validate_workspace_history_fence(current, bad); });
    bad = fence; bad.source_digest[0] = bad.source_digest[0] == 'a' ? 'b' : 'a';
    rejected([&] { validate_workspace_history_fence(current, bad); });
    bad = fence; bad.document_id = "foreign";
    rejected([&] { validate_workspace_history_fence(current, bad); });
    bad = fence; bad.baseline_revision = current.revision() + 1;
    rejected([&] { validate_workspace_history_fence(current, bad); });
    auto malformed = current;
    const_cast<std::vector<RevisionRecord>&>(malformed.history()).back().undo_stack.push_back(999);
    rejected([&] { validate_workspace_history_fence(malformed, fence); });
    if (current.history().back().undo_stack != document.snapshot().history().back().undo_stack)
        throw std::runtime_error("fence validation mutated navigation");
}
void check_command_identity_mapping() {
    auto document = Document::create();
    auto empty = derive_workspace_baseline_navigation(document.snapshot(),
        capture_workspace_history_fence(document.snapshot()));
    if (!empty.undo_stack.empty() || !empty.redo_stack.empty())
        throw std::runtime_error("empty baseline invented a command identity");
    document.apply(NameRevision{0, "Name"});
    document.apply(ApplyEntityChanges{.expected_revision = 1,
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "A"}}))},
        .message = "undo"});
    document.undo(2);
    const auto early = capture_workspace_history_fence(document.snapshot());
    document.redo(3);
    document.apply(NameRevision{4, "Later"});
    document.undo(5);
    document.undo(6);
    const auto navigated = capture_workspace_history_fence(document.snapshot());
    document.apply(ApplyEntityChanges{.expected_revision = 7,
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "Branch"}}))},
        .message = "redo"});
    const auto current = document.snapshot();
    const auto early_map = derive_workspace_baseline_navigation(current, early);
    if (early_map.undo_stack != std::vector<WorkspaceBaselineCommandReference>{{1, 0}} ||
        early_map.redo_stack != std::vector<WorkspaceBaselineCommandReference>{{2, 2}})
        throw std::runtime_error("historical baseline lost original command identities");
    const auto map = derive_workspace_baseline_navigation(current, navigated);
    if (map.undo_stack != std::vector<WorkspaceBaselineCommandReference>{{1, 0}} ||
        map.redo_stack != std::vector<WorkspaceBaselineCommandReference>{{5, 5}, {2, 6}})
        throw std::runtime_error("navigation snapshot was mistaken for an original command");
    const auto branch = derive_workspace_baseline_navigation(current,
        capture_workspace_history_fence(current));
    if (branch.undo_stack != std::vector<WorkspaceBaselineCommandReference>{{1, 0}, {8, 7}} ||
        !branch.redo_stack.empty())
        throw std::runtime_error("new branch failed to retain command lineage and clear redo");
    auto forged = navigated;
    std::swap(forged.redo_stack[0], forged.redo_stack[1]);
    rejected([&] { (void)derive_workspace_baseline_navigation(current, forged); });
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check(); check_command_identity_mapping(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
