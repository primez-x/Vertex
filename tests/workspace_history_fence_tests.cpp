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
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
