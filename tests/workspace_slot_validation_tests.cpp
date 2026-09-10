#include "sketch/workspace_slot_validation.hpp"
#include "sketch/workspace_lifecycle_validation.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
Document fixture() {
    return Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}, {"label", "label", {{"text", "Original"}}}});
}
void check(const ProjectWorkspaceSnapshot& s, const std::optional<BoundaryActiveRecovery>& active,
           const WorkspaceRetiredBoundaries& retired) {
    validate_workspace_lifecycle_slots(s.document(), s.document_history(), s.lifecycle_history(), s.navigation(),
        active, retired, s.resource_policy());
}
void validate(const ProjectWorkspace& w) { const auto s = w.capture(); check(s, s.active_boundary(), s.retired_boundaries()); }
void commit(ProjectWorkspace& w, PreparedWorkspaceEdit ticket) { (void)w.commit(ticket); validate(w); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("corrupt draft slot was accepted");
}
BoundaryActiveRecovery draft(const ProjectWorkspace& w) {
    BoundaryAuthoringSession s(BoundaryAuthoringMode::draw_first);
    (void)s.anchor({0, 0}); (void)s.add_line_to({4, 0});
    return {capture_boundary_recovery_source(w.snapshot(), {"p", "b", "f", "l"}), s.recovery_checkpoint()};
}
void edit_document(ProjectWorkspace& w) {
    auto label = w.snapshot().entities().at("label"); label.properties["text"] = "Changed";
    commit(w, w.prepare(ApplyEntityChanges{.expected_revision = w.snapshot().revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Edit"}));
}
void check_unrecorded_update_windows() {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot()); auto a = draft(w);
    commit(w, w.prepare_boundary_checkpoint(a));
    auto local = BoundaryAuthoringSession::from_recovery_checkpoint(a.checkpoint);
    (void)local.add_line_to({4, 3}); a.checkpoint = local.recovery_checkpoint();
    a.extensions["future"] = 1.0;
    commit(w, w.prepare_boundary_checkpoint(a));
    edit_document(w); // Last unrecorded observation may have preceded this edit.
    commit(w, w.prepare_discard_boundary());
    commit(w, w.prepare_undo()); // Restored stale input cannot accept pointer edits.
    const auto s = w.capture(); auto wrong = s.active_boundary(); wrong->checkpoint.pointer = Vec2{99, 99};
    rejects([&] { check(s, wrong, s.retired_boundaries()); });
    rejects([&] { check(s, std::nullopt, s.retired_boundaries()); });
    commit(w, w.prepare_redo());
}
void check_pending_redo_semantics_and_floors() {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot()); auto a = draft(w);
    a.checkpoint.counters.next_boundary_id += 100;
    commit(w, w.prepare_boundary_checkpoint(a));
    auto s = w.capture(); auto rewound = s.active_boundary(); --rewound->checkpoint.counters.next_boundary_id;
    rejects([&] { check(s, rewound, s.retired_boundaries()); });
    commit(w, w.prepare_discard_boundary()); commit(w, w.prepare_undo());
    a.checkpoint.pointer = Vec2{5, 8}; commit(w, w.prepare_boundary_checkpoint(a));
    s = w.capture(); auto semantic = s.active_boundary(); semantic->extensions["changed"] = true;
    rejects([&] { check(s, semantic, s.retired_boundaries()); });
    a.checkpoint.pointer.reset(); commit(w, w.prepare_boundary_checkpoint(a));
    commit(w, w.prepare_redo()); commit(w, w.prepare_undo());
    a.extensions["changed"] = true; commit(w, w.prepare_boundary_checkpoint(a));
    commit(w, w.prepare_discard_boundary());
}
void check_retired_corruption() {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot()); auto a = draft(w);
    auto local = BoundaryAuthoringSession::from_recovery_checkpoint(a.checkpoint);
    local.set_classification("living_area"); (void)local.add_line_to({4, 3}); (void)local.add_line_to({0, 3});
    (void)local.add_closing_segment(); (void)local.close_chain(); a.checkpoint = local.recovery_checkpoint();
    commit(w, w.prepare_boundary_checkpoint(a)); commit(w, w.prepare_finish_boundary()); commit(w, w.prepare_undo());
    const auto s = w.capture();
    rejects([&] { check(s, s.active_boundary(), {}); });
    auto wrong = s.retired_boundaries(); wrong.begin()->second.status = WorkspaceInputStatus::active;
    rejects([&] { check(s, s.active_boundary(), wrong); });
    wrong = s.retired_boundaries(); wrong.begin()->second.finish_event_id = "wrong";
    rejects([&] { check(s, s.active_boundary(), wrong); });
    rejects([&] { check(s, std::optional{a}, s.retired_boundaries()); });
}
void check_false_finish_delta() {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot()); auto a = draft(w);
    auto local = BoundaryAuthoringSession::from_recovery_checkpoint(a.checkpoint);
    local.set_classification("living_area"); (void)local.add_line_to({4, 3}); (void)local.add_line_to({0, 3});
    (void)local.add_closing_segment(); (void)local.close_chain(); a.checkpoint = local.recovery_checkpoint();
    commit(w, w.prepare_boundary_checkpoint(a));
    auto label = w.snapshot().entities().at("label"); label.properties["text"] = "Not boundary geometry";
    commit(w, w.prepare(ApplyEntityChanges{.expected_revision = w.snapshot().revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Finish boundary"}));
    const auto s = w.capture(); auto events = s.lifecycle_history();
    auto& fake = events.back(); fake.kind = WorkspaceLifecycleKind::boundary_finish;
    fake.input = WorkspaceArchivedInput{fake.event_id, std::make_shared<const BoundaryActiveRecovery>(a)};
    auto nav = s.navigation(); nav.operations[fake.event_id] = WorkspaceOperationKind::boundary_finish;
    // The document and navigation are individually valid; only the promised
    // finish geometry is false.
    validate_workspace_lifecycle_order(s.document(), s.document_history(), events, nav);
    validate_workspace_archival_inputs(events);
    rejects([&] { validate_workspace_finish_deltas(s.document(), events); });
    rejects([&] { validate_workspace_lifecycle_slots(s.document(), s.document_history(), events, nav,
        std::nullopt, {}, s.resource_policy()); });
}
void check_barrier_and_wrong_restoration() {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot()); auto a = draft(w);
    commit(w, w.prepare_boundary_checkpoint(a));
    auto s = w.capture(); auto events = s.lifecycle_history();
    WorkspaceLifecycleEvent barrier;
    barrier.event_id = "spurious-barrier"; barrier.sequence = events.size() + 1;
    barrier.kind = WorkspaceLifecycleKind::clear_redo;
    barrier.before_revision = barrier.after_revision = s.document().revision();
    events.push_back(barrier);
    validate_workspace_lifecycle_order(s.document(), s.document_history(), events, s.navigation());
    rejects([&] { validate_workspace_lifecycle_slots(s.document(), s.document_history(), events, s.navigation(),
        s.active_boundary(), s.retired_boundaries()); });
    commit(w, w.prepare_discard_boundary());
    const auto first = *w.capture().lifecycle_history().back().input;
    commit(w, w.prepare_undo());
    a.extensions["versioned-number"] = 1.0; commit(w, w.prepare_boundary_checkpoint(a));
    commit(w, w.prepare_discard_boundary()); commit(w, w.prepare_undo());
    s = w.capture(); events = s.lifecycle_history(); events.back().input = first;
    validate_workspace_archival_inputs(events);
    rejects([&] { validate_workspace_lifecycle_slots(s.document(), s.document_history(), events, s.navigation(),
        s.active_boundary(), s.retired_boundaries()); });
}
void check_revision_provenance() {
    auto doc = fixture(); ProjectWorkspace w(doc.snapshot()); auto a = draft(w);
    auto local = BoundaryAuthoringSession::from_recovery_checkpoint(a.checkpoint);
    local.set_classification("living_area"); (void)local.add_line_to({4, 3}); (void)local.add_line_to({0, 3});
    (void)local.add_closing_segment(); (void)local.close_chain(); a.checkpoint = local.recovery_checkpoint();
    commit(w, w.prepare_boundary_checkpoint(a)); commit(w, w.prepare_finish_boundary()); commit(w, w.prepare_undo());
    commit(w, w.prepare_revise_boundary(a.checkpoint.identity_namespace));
    const auto s = w.capture();
    const auto mutate = [&](auto mutation) {
        auto events = s.lifecycle_history(); mutation(*events.back().session);
        rejects([&] { validate_workspace_lifecycle_slots(s.document(), s.document_history(), events, s.navigation(),
            s.active_boundary(), s.retired_boundaries()); });
    };
    mutate([](auto& identity) { identity.revised_from_finish_event_id.reset(); });
    mutate([](auto& identity) { identity.revised_from_finish_event_id = "wrong-finish"; });
    mutate([](auto& identity) { identity.revised_from_namespace = "missing-retired"; });
    mutate([](auto& identity) { ++identity.initial_counters.next_boundary_id; });
}
Revision add_abandoned_constraint(Document& document) {
    const Entity wall{"wall-a", "wall", {{"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
        {"sweep_radians", 0.0}}}, {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.0}}};
    const auto binding = [](const char* role) { return nlohmann::json{{"owner_id", "wall-a"},
        {"feature", "baseline"}, {"role", role}}; };
    const Entity constraint{"horizontal-a", "constraint", {{"version", 1}, {"relation", "horizontal"},
        {"wall_ids", {"wall-a"}}, {"bindings", {binding("start"), binding("end")}}}};
    const auto revision = document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(wall), EntityChange::upsert(constraint)}, .message = "Add constraint"});
    document.undo(document.revision());
    auto label = document.snapshot().entities().at("label"); label.properties["text"] = "Branch";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Branch"});
    return revision;
}
void finish_rectangle(ProjectWorkspace& w) {
    auto a = draft(w); auto local = BoundaryAuthoringSession::from_recovery_checkpoint(a.checkpoint);
    local.set_classification("living_area"); (void)local.add_line_to({4, 3}); (void)local.add_line_to({0, 3});
    (void)local.add_closing_segment(); (void)local.close_chain(); a.checkpoint = local.recovery_checkpoint();
    commit(w, w.prepare_boundary_checkpoint(a)); commit(w, w.prepare_finish_boundary());
}
void check_historical_editability() {
    auto document = fixture(); const auto unknown_revision = add_abandoned_constraint(document);
    ProjectWorkspace w(document.snapshot()); finish_rectangle(w);
    auto future = w.snapshot();
    const_cast<std::vector<RevisionRecord>&>(future.history()).at(static_cast<std::size_t>(unknown_revision))
        .entities.at("horizontal-a").properties["version"] = 99;
    auto events = w.capture().lifecycle_history();
    auto& event = events.back(); auto input = *event.input->value;
    // Rebind the digest to this structurally valid future-format history, so
    // rejection depends on original editability rather than a stale checksum.
    input.source.authoring_digest = document_authoring_source_digest_v1_at_revision(future, input.source.revision);
    event.input->value = std::make_shared<const BoundaryActiveRecovery>(input);
    if (Document::fork_at_revision(future, event.before_revision).is_editable())
        throw std::runtime_error("abandoned unsupported history lost its read-only restriction");
    rejects([&] { validate_historical_boundary_recovery_source(future, input.source); });
    rejects([&] { validate_workspace_finish_deltas(future, events); });

    auto earlier = fixture(); ProjectWorkspace good(earlier.snapshot()); finish_rectangle(good);
    auto later = Document::fork(good.snapshot()); const auto later_unknown = add_abandoned_constraint(later);
    auto late_future = later.snapshot();
    const_cast<std::vector<RevisionRecord>&>(late_future.history()).at(static_cast<std::size_t>(later_unknown))
        .entities.at("horizontal-a").properties["version"] = 99;
    if (Document::fork(late_future).is_editable()) throw std::runtime_error("future history fixture must be read-only");
    validate_workspace_finish_deltas(late_future, good.capture().lifecycle_history());
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check_unrecorded_update_windows(); check_pending_redo_semantics_and_floors(); check_retired_corruption();
        check_false_finish_delta(); check_barrier_and_wrong_restoration(); check_revision_provenance(); check_historical_editability(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
