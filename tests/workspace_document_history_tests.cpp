#include "sketch/workspace_document_history.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
template<class F> void rejected(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid document event history was accepted");
}
void check() {
    auto document = Document::create();
    document.apply(ApplyEntityChanges{.expected_revision = 0,
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "Draft"}}))}});
    document.undo(1);
    auto history = capture_workspace_document_history(document.snapshot());
    if (!history.events.empty() || history.baseline.baseline_revision != 2 ||
        history.baseline.redo_stack.empty())
        throw std::runtime_error("capture lost the baseline or retained redo");
    validate_workspace_document_history(document.snapshot(), history);
    document.redo(2);
    document.undo(3);
    document.redo(4);
    document.apply(NameRevision{5, "Later name"});
    document.apply(ApplyEntityChanges{.expected_revision = 6,
        .entity_changes = {EntityChange::upsert(Entity::create("label", {{"text", "Other"}}))},
        .message = "undo"});
    history.events = {{"redo-one", 1, WorkspaceDocumentEventKind::redo, 2, 3},
                      {"undo-one", 2, WorkspaceDocumentEventKind::undo, 3, 4},
                      {"redo-two", 3, WorkspaceDocumentEventKind::redo, 4, 5},
                      {"name", 4, WorkspaceDocumentEventKind::edit, 5, 6},
                      {"edit", 5, WorkspaceDocumentEventKind::edit, 6, 7}};
    document.mark_saved(7);
    const auto snapshot = document.snapshot();
    validate_workspace_document_history(snapshot, history);
    const auto original = history;
    auto bad = history;
    const auto invalid = [&](auto mutate) {
        bad = original; mutate(bad);
        rejected([&] { validate_workspace_document_history(snapshot, bad); });
    };
    invalid([](auto& h) { h.events.clear(); });
    invalid([](auto& h) { h.events.erase(h.events.begin() + 1); });
    invalid([](auto& h) { h.events.push_back({"extra", 6, WorkspaceDocumentEventKind::edit, 7, 8}); });
    invalid([](auto& h) { h.events[0].kind = WorkspaceDocumentEventKind::edit; });
    invalid([](auto& h) { h.events[1].kind = WorkspaceDocumentEventKind::redo; });
    invalid([](auto& h) { h.events[4].kind = WorkspaceDocumentEventKind::undo; });
    invalid([](auto& h) { h.events[0].kind = static_cast<WorkspaceDocumentEventKind>(99); });
    invalid([](auto& h) { h.events[1].event_id = h.events[0].event_id; });
    invalid([](auto& h) { h.events[0].event_id.clear(); });
    invalid([](auto& h) { h.events[0].event_id = std::string(129, 'x'); });
    invalid([](auto& h) { h.events[0].event_id = std::string("a\0b", 3); });
    invalid([](auto& h) { h.events[0].sequence = 0; });
    invalid([](auto& h) { h.events[1].sequence = 1; });
    invalid([](auto& h) { h.events[0].before_revision = 1; });
    invalid([](auto& h) { h.events[0].after_revision = 4; });
    invalid([](auto& h) { std::swap(h.events[0], h.events[1]); });
    invalid([](auto& h) { h.baseline.redo_stack.clear(); });
    invalid([](auto& h) { h.baseline.source_digest = "forged"; });
    invalid([](auto& h) { h.baseline.document_id = "foreign"; });
    invalid([](auto& h) { h.baseline.baseline_revision = 99; });
    auto malformed = snapshot;
    const_cast<std::vector<RevisionRecord>&>(malformed.history()).back().undo_stack.push_back(999);
    rejected([&] { validate_workspace_document_history(malformed, history); });
    history.events[0].event_id = std::string(128, 'x');
    validate_workspace_document_history(snapshot, history);
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
