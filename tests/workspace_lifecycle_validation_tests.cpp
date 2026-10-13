#include "sketch/workspace_lifecycle_validation.hpp"
#include "sketch/workspace_slot_validation.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void validate(const ProjectWorkspace& workspace) {
    const auto snapshot = workspace.capture();
    validate_workspace_lifecycle_order(snapshot.document(), snapshot.document_history(),
        snapshot.lifecycle_history(), snapshot.navigation());
    validate_workspace_archival_inputs(snapshot.lifecycle_history(), snapshot.resource_policy());
    validate_workspace_recovery_sources(snapshot.document(), snapshot.lifecycle_history(), snapshot.active_boundary());
    validate_workspace_lifecycle_slots(snapshot.document(), snapshot.document_history(), snapshot.lifecycle_history(),
        snapshot.navigation(), snapshot.active_boundary(), snapshot.retired_boundaries(), snapshot.resource_policy());
}
void commit(ProjectWorkspace& workspace, PreparedWorkspaceEdit ticket) {
    (void)workspace.commit(ticket); validate(workspace);
}
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("corrupt lifecycle ordering was accepted");
}
void test_area_defaults_lifecycle() {
    const auto document = Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}});
    ProjectWorkspace workspace(document.snapshot());
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    session.set_classification("measurement");
    (void)session.anchor({0, 0});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
    active.area_defaults = BoundaryAreaDefaultsIntent{1, std::string{"garage"}};
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    require(*decode_boundary_active_recovery(encode_boundary_active_recovery(*workspace.active_boundary())).active == active,
        "unfinished staged category survives archival checkpoint");
    (void)session.add_line_to({4, 0}); (void)session.add_line_to({4, 3});
    (void)session.add_line_to({0, 3}); (void)session.add_closing_segment(); (void)session.close_chain();
    active.checkpoint = session.recovery_checkpoint();
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    const auto before = workspace.snapshot();
    commit(workspace, workspace.prepare_finish_boundary());
    const auto captured = workspace.capture();
    require(captured.document().revision() == before.revision() + 1 &&
        captured.document().entities().at("p").properties.at("calculation_workflow") == "appraisal",
        "workspace Finish commits setup and area in one revision");
    validate_workspace_finish_deltas(captured.document(), captured.lifecycle_history());
    auto events = captured.lifecycle_history();
    auto altered = *events.back().input->value;
    altered.area_defaults->staged_category = "above_grade_finished";
    events.back().input->value = std::make_shared<const BoundaryActiveRecovery>(altered);
    rejects([&] { validate_workspace_finish_deltas(captured.document(), events); });
    altered = *captured.lifecycle_history().back().input->value;
    altered.area_defaults.reset();
    events.back().input->value = std::make_shared<const BoundaryActiveRecovery>(altered);
    rejects([&] { validate_workspace_finish_deltas(captured.document(), events); });
    commit(workspace, workspace.prepare_undo());
    require(workspace.snapshot().entities() == before.entities(), "workspace undo removes setup and area together");
    commit(workspace, workspace.prepare_redo());
    require(workspace.snapshot().entities() == captured.document().entities(), "workspace redo restores exact atomic Finish map");
}

void run() {
    test_area_defaults_lifecycle();
    auto document = Document::create({{"p", "property", {{"name", "Property"}}}, {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}}, {"l", "layer", {{"floor_id", "f"}}},
        {"label", "label", {{"text", "Original"}}}});
    auto label = document.snapshot().entities().at("label"); label.properties["text"] = "Baseline";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Baseline edit"});
    ProjectWorkspace workspace(document.snapshot()); validate(workspace);
    commit(workspace, workspace.prepare_undo()); commit(workspace, workspace.prepare_redo());
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    session.set_classification("living_area");
    (void)session.anchor({0, 0}); (void)session.add_line_to({4, 0});
    (void)session.add_line_to({4, 3}); (void)session.add_line_to({0, 3});
    (void)session.add_closing_segment(); (void)session.close_chain();
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint()};
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    commit(workspace, workspace.prepare_discard_boundary());
    commit(workspace, workspace.prepare_undo()); commit(workspace, workspace.prepare_redo());
    commit(workspace, workspace.prepare_undo());
    // Pointer changes preserve redo, while an opaque semantic update clears it.
    active.checkpoint.pointer = Vec2{8, 9};
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    active.extensions["future"] = 1;
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    commit(workspace, workspace.prepare_finish_boundary());
    commit(workspace, workspace.prepare_undo());
    commit(workspace, workspace.prepare_revise_boundary(active.checkpoint.identity_namespace));
    commit(workspace, workspace.prepare_finish_boundary());
    commit(workspace, workspace.prepare_undo()); commit(workspace, workspace.prepare_undo());
    commit(workspace, workspace.prepare_redo()); commit(workspace, workspace.prepare_redo());
    const auto captured = workspace.capture();
    const auto check = [&](const auto& events, const auto& nav) {
        validate_workspace_lifecycle_order(captured.document(), captured.document_history(), events, nav);
    };
    const auto mutate = [&](auto mutation) {
        auto events = captured.lifecycle_history(); mutation(events);
        rejects([&] { check(events, captured.navigation()); });
    };
    mutate([](auto& e) { e.front().event_id.clear(); });
    mutate([](auto& e) { e.back().event_id = e.front().event_id; });
    mutate([](auto& e) { ++e.back().sequence; });
    mutate([](auto& e) { ++e.front().before_revision; });
    mutate([](auto& e) { e.front().target = WorkspaceNavigationTarget{Revision{999}}; });
    mutate([](auto& e) { e.front().kind = static_cast<WorkspaceLifecycleKind>(999); });
    mutate([](auto& e) { e.pop_back(); });
    mutate([](auto& e) { for (auto& item : e) if (item.kind == WorkspaceLifecycleKind::boundary_finish) {
        item.input.reset(); break;
    }});
    mutate([](auto& e) { for (auto& item : e) if (item.kind == WorkspaceLifecycleKind::boundary_activate) {
        ++item.after_revision; break;
    }});
    auto nav = captured.navigation(); nav.redo_stack.push_back({Revision{0}});
    rejects([&] { check(captured.lifecycle_history(), nav); });
    nav = captured.navigation(); nav.operations.clear();
    rejects([&] { check(captured.lifecycle_history(), nav); });
    const auto bad_input = [&](auto mutation) {
        auto events = captured.lifecycle_history();
        for (auto& event : events) if (event.input && event.input->owner_event_id != event.event_id) {
            mutation(*event.input); break;
        }
        rejects([&] { validate_workspace_archival_inputs(events); });
    };
    bad_input([](auto& input) { input.value.reset(); });
    bad_input([](auto& input) { input.owner_event_id = "missing-owner"; });
    bad_input([](auto& input) { input.status = static_cast<WorkspaceInputStatus>(999); });
    bad_input([](auto& input) { input.finish_event_id = "missing-finish"; input.status = WorkspaceInputStatus::retired; });
    bad_input([](auto& input) {
        auto replacement = *input.value; replacement.extensions["substitution"] = true;
        input.value = std::make_shared<const BoundaryActiveRecovery>(replacement);
    });
    auto source_events = captured.lifecycle_history();
    for (auto& event : source_events) if (event.session) {
        event.session->source.authoring_digest = "forged"; break;
    }
    rejects([&] { validate_workspace_recovery_sources(captured.document(), source_events, captured.active_boundary()); });
    require(captured.lifecycle_history().size() > 10, "mixed fixture did not exercise lifecycle history");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { run(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
