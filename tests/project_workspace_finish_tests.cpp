#include "sketch/project_workspace.hpp"
#include "sketch/workspace_slot_validation.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
Document fixture() {
    return Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}, {"label", "label", {{"text", "Original"}}}});
}
BoundaryActiveRecovery input(const DocumentSnapshot& source, BoundaryAuthoringMode mode,
                             bool complete = true, bool classified = true) {
    BoundaryAuthoringOptions options; options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(mode, options);
    if (classified) session.set_classification("living_area");
    (void)session.anchor({0, 0}); (void)session.add_line_to({4, 0});
    (void)session.add_line_to({4, 3}); (void)session.add_line_to({0, 3});
    if (complete) { (void)session.add_closing_segment(); (void)session.close_chain(); }
    auto checkpoint = session.recovery_checkpoint();
    checkpoint.extensions["future"] = {{"number", 1.0}, {"items", Json::array({nullptr, "x"})}};
    return {capture_boundary_recovery_source(source, {"p", "b", "f", "l"}),
            checkpoint, {{"future", {{"number", 2.0}}}}};
}
void activate(ProjectWorkspace& workspace, const BoundaryActiveRecovery& active) {
    auto ticket = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(ticket);
}
void validate_slots(const ProjectWorkspace& workspace) {
    const auto s = workspace.capture();
    validate_workspace_lifecycle_slots(s.document(), s.document_history(), s.lifecycle_history(), s.navigation(),
        s.active_boundary(), s.retired_boundaries(), s.resource_policy());
}
void undo(ProjectWorkspace& workspace) {
    auto ticket = workspace.prepare_undo(); (void)workspace.commit(ticket);
    validate_workspace_document_history(workspace.snapshot(), workspace.document_history());
    validate_slots(workspace);
}
void redo(ProjectWorkspace& workspace) {
    auto ticket = workspace.prepare_redo(); (void)workspace.commit(ticket);
    validate_workspace_document_history(workspace.snapshot(), workspace.document_history());
    validate_slots(workspace);
}
template<class F> void unchanged_rejection(ProjectWorkspace& workspace, F&& operation) {
    const auto before = workspace.capture(); const auto digest = document_snapshot_digest(before.document());
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid finish operation must reject");
    const auto after = workspace.capture();
    require(document_snapshot_digest(after.document()) == digest &&
            after.active_boundary() == before.active_boundary() &&
            after.retired_boundaries().size() == before.retired_boundaries().size() &&
            after.navigation() == before.navigation() && after.document_history() == before.document_history() &&
            after.epoch() == before.epoch() && after.edited_generation() == before.edited_generation() &&
            after.checkpoint_generation() == before.checkpoint_generation(),
            "rejected finish must leave document, draft, retirement and generations unchanged");
}
void check_finish_round_trip() {
    for (auto mode : {BoundaryAuthoringMode::draw_first, BoundaryAuthoringMode::define_first}) {
        auto document = fixture(); const auto source = document.snapshot(); ProjectWorkspace workspace(source);
        const auto active = input(source, mode); const auto ns = active.checkpoint.identity_namespace;
        activate(workspace, active); const auto before = workspace.capture();
        auto finish = workspace.prepare_finish_boundary(); auto preview = finish.preview();
        require(preview.entities().size() == source.entities().size() + 5,
                "finish preview contains one boundary and all four dimensions");
        require(workspace.active_boundary() == std::optional{active} &&
                document_snapshot_digest(workspace.snapshot()) == document_snapshot_digest(source) &&
                workspace.epoch() == before.epoch(), "preparing finish must be isolated");
        auto detached = Document::fork(preview);
        auto label = detached.snapshot().entities().at("label"); label.properties["text"] = "detached";
        detached.apply(ApplyEntityChanges{.expected_revision = detached.revision(),
            .entity_changes = {EntityChange::upsert(label)}, .message = "Mutate preview"});
        (void)workspace.commit(finish); const auto finished = workspace.snapshot();
        require(finished.entities() == preview.entities() && finished.revision() == source.revision() + 1 &&
                finished.history().size() == source.history().size() + 1 && !workspace.active_boundary(),
                "finish publishes one atomic document revision and removes active state");
        require(workspace.epoch() == before.epoch() + 1 &&
                workspace.edited_generation() == before.edited_generation() + 1 &&
                workspace.checkpoint_generation() == before.checkpoint_generation() + 1,
                "finish advances each workspace generation once");
        unchanged_rejection(workspace, [&] { (void)workspace.commit(finish); });
        for (int cycle = 0; cycle < 3; ++cycle) {
            undo(workspace);
            require(workspace.snapshot().entities() == source.entities() && !workspace.active_boundary() &&
                    workspace.retired_boundary(ns) == std::optional{active},
                    "undo finish restores exact source entities and retires original input");
            require(workspace.retired_boundary(ns)->checkpoint.extensions.at("future").at("number").is_number_float(),
                    "retired input preserves opaque floating point values");
            unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(); });
            unchanged_rejection(workspace, [&] { activate(workspace, active); });
            redo(workspace);
            require(workspace.snapshot().entities() == finished.entities() && !workspace.active_boundary() &&
                    !workspace.retired_boundary(ns), "redo finish restores original entity and dimension IDs");
        }
        undo(workspace); const auto retired_capture = workspace.capture(); undo(workspace);
        require(!workspace.active_boundary() && !workspace.retired_boundary(ns),
                "undo activation removes the matching retired session");
        require(retired_capture.retired_boundaries().size() == 1, "retired capture remains detached");
        redo(workspace);
        require(!workspace.active_boundary() && workspace.retired_boundary(ns) == std::optional{active},
                "redo activation preserves retired status");
        redo(workspace);
        require(workspace.snapshot().entities() == finished.entities() && !workspace.retired_boundary(ns),
                "redo finish after activation navigation restores exact geometry");
    }
}
void check_multiple_retired_inputs() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    const auto a = input(workspace.snapshot(), BoundaryAuthoringMode::draw_first);
    const auto a_ns = a.checkpoint.identity_namespace;
    activate(workspace, a);
    auto finish_a = workspace.prepare_finish_boundary(); (void)workspace.commit(finish_a);
    undo(workspace);
    const auto b = input(workspace.snapshot(), BoundaryAuthoringMode::define_first);
    const auto b_ns = b.checkpoint.identity_namespace;
    require(a_ns != b_ns, "new input has a fresh namespace");
    activate(workspace, b);
    auto finish_b = workspace.prepare_finish_boundary(); (void)workspace.commit(finish_b);
    const auto finished_b = workspace.snapshot();
    undo(workspace);
    require(workspace.retired_boundary(a_ns) == std::optional{a} &&
            workspace.retired_boundary(b_ns) == std::optional{b},
            "branch finish preserves both retired inputs");
    undo(workspace);
    require(workspace.retired_boundary(a_ns) && !workspace.retired_boundary(b_ns),
            "undo second activation removes only its retired input");
    undo(workspace);
    require(workspace.capture().retired_boundaries().empty(), "undo first activation removes remaining input");
    redo(workspace); redo(workspace);
    require(!workspace.active_boundary() && workspace.retired_boundary(a_ns) == std::optional{a} &&
            workspace.retired_boundary(b_ns) == std::optional{b},
            "redo activations preserves both retired statuses");
    redo(workspace);
    require(workspace.snapshot().entities() == finished_b.entities() &&
            workspace.retired_boundary(a_ns) == std::optional{a} && !workspace.retired_boundary(b_ns),
            "redo branched finish restores exact geometry and preserves older retired input");
}
void check_mixed_document_navigation() {
    auto document = fixture();
    auto label = document.snapshot().entities().at("label"); label.properties["text"] = "Baseline edit";
    document.apply(ApplyEntityChanges{.expected_revision = document.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Baseline edit"});
    const auto baseline = document.snapshot(); ProjectWorkspace workspace(baseline);
    const auto active = input(baseline, BoundaryAuthoringMode::draw_first);
    activate(workspace, active);
    auto finish = workspace.prepare_finish_boundary(); (void)workspace.commit(finish);
    const auto finished = workspace.snapshot();
    label.properties["text"] = "After finish";
    auto edit = workspace.prepare(ApplyEntityChanges{.expected_revision = finished.revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "After finish"});
    (void)workspace.commit(edit); const auto edited = workspace.snapshot();
    validate_workspace_document_history(edited, workspace.document_history());
    undo(workspace);
    require(workspace.snapshot().entities() == finished.entities(), "undo edit preserves finished geometry");
    undo(workspace); undo(workspace); undo(workspace);
    require(workspace.snapshot().entities().at("label").properties.at("text") == "Original",
            "navigation crosses the imported baseline command");
    redo(workspace); redo(workspace); redo(workspace); redo(workspace);
    require(workspace.snapshot().entities() == edited.entities() && !workspace.active_boundary() &&
            workspace.capture().retired_boundaries().empty(),
            "mixed round trip preserves exact finished entities and lifecycle status");
}
void check_revise_input() {
    auto document = fixture(); ProjectWorkspace workspace(document.snapshot());
    const auto original = input(workspace.snapshot(), BoundaryAuthoringMode::draw_first);
    const auto ns = original.checkpoint.identity_namespace;
    activate(workspace, original);
    auto finish = workspace.prepare_finish_boundary(); (void)workspace.commit(finish);
    const auto old_geometry = workspace.snapshot().entities();
    undo(workspace);
    const auto before = workspace.capture();
    auto revise = workspace.prepare_revise_boundary(ns);
    require(!workspace.active_boundary() && workspace.epoch() == before.epoch(), "revise preparation is isolated");
    (void)workspace.commit(revise);
    const auto revised = *workspace.active_boundary();
    require(revised.checkpoint.identity_namespace != ns && revised.checkpoint.counters == original.checkpoint.counters &&
            revised.checkpoint.extensions.dump() == original.checkpoint.extensions.dump() &&
            revised.extensions.dump() == original.extensions.dump(), "revise regenerates identity and preserves opaque data");
    require(inspect_boundary_recovery_source(workspace.snapshot(), revised.source) == BoundaryRecoverySourceStatus::current &&
            workspace.retired_boundary(ns) == std::optional{original} && !workspace.can_redo(),
            "revise rebinds current source, preserves retired input and clears old finish redo");
    const auto captured = workspace.capture();
    require(captured.lifecycle_history().back().session->revised_from_namespace == ns &&
            captured.lifecycle_history().back().session->revised_from_finish_event_id.has_value(),
            "revision activation records origin provenance");
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_revise_boundary(ns); });
    undo(workspace); redo(workspace);
    require(workspace.active_boundary() == std::optional{revised}, "revision activation round trip retains fresh identities");
    auto second = workspace.prepare_finish_boundary(); (void)workspace.commit(second);
    const auto revised_geometry = workspace.snapshot();
    for (const auto& [id, entity] : revised_geometry.entities()) {
        if (entity.type == "boundary" || entity.type == "dimension")
            require(!old_geometry.contains(id), "revised finish must not reuse old geometry IDs");
    }
    require(workspace.retired_boundary(ns) == std::optional{original}, "revised finish retains original retired view");
}
void check_invalid_inputs_and_tickets() {
    auto document = fixture(); const auto source = document.snapshot(); ProjectWorkspace empty(source);
    unchanged_rejection(empty, [&] { (void)empty.prepare_finish_boundary(); });
    for (const auto complete : {false, true}) {
        ProjectWorkspace invalid(source);
        activate(invalid, input(source, BoundaryAuthoringMode::draw_first, complete, !complete));
        unchanged_rejection(invalid, [&] { (void)invalid.prepare_finish_boundary(); });
    }
    ProjectWorkspace workspace(source), foreign(source);
    const auto active = input(source, BoundaryAuthoringMode::draw_first); activate(workspace, active);
    auto foreign_ticket = workspace.prepare_finish_boundary();
    unchanged_rejection(foreign, [&] { (void)foreign.commit(foreign_ticket); });
    require(workspace.active_boundary() == std::optional{active}, "foreign commit leaves owner active");
    auto stale = workspace.prepare_finish_boundary(); auto pointer = active; pointer.checkpoint.pointer = Vec2{9, 8};
    activate(workspace, pointer);
    unchanged_rejection(workspace, [&] { (void)workspace.commit(stale); });
    auto label = workspace.snapshot().entities().at("label"); label.properties["text"] = "changed";
    auto edit = workspace.prepare(ApplyEntityChanges{.expected_revision = workspace.snapshot().revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Make source stale"});
    (void)workspace.commit(edit);
    unchanged_rejection(workspace, [&] { (void)workspace.prepare_finish_boundary(); });
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try { check_finish_round_trip(); check_multiple_retired_inputs(); check_mixed_document_navigation(); check_revise_input(); check_invalid_inputs_and_tickets(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
