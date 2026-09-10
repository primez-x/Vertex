#include "sketch/project_workspace.hpp"
#include "sketch/boundary_recovery_source.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace sketch;
using Json = nlohmann::json;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
template <class F> void rejected(F&& f, std::string_view message) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string(message));
}
template <class F> void rejected_no_redo(F&& f, std::string_view message) {
    try { f(); }
    catch (const DocumentError& error) {
        if (error.code() == DocumentErrorCode::no_redo) return;
    }
    throw std::runtime_error(std::string(message));
}

Document fixture() {
    return Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}, {"label", "label", {{"text", "Original"}}}});
}
DrawingContext context() { return {"p", "b", "f", "l"}; }
BoundaryAuthoringSession make_session() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({1, 2}); (void)session.add_line_to({5, 2}); session.set_pointer({8, 9});
    return session;
}
BoundaryActiveRecovery active_record(const BoundaryRecoverySource& source,
                                     const BoundaryAuthoringSession& session) {
    return {source, session.recovery_checkpoint(),
            {{"future", {{"items", Json::array({1, "two", nullptr})}}}}};
}
Command edit_label(const DocumentSnapshot& snapshot, std::string_view value) {
    auto label = snapshot.entities().at("label"); label.properties["text"] = std::string(value);
    return ApplyEntityChanges{.expected_revision = snapshot.revision(),
        .entity_changes = {EntityChange::upsert(std::move(label))}, .message = "Edit label"};
}

void check_discard_round_trip_and_pointer_redo() {
    auto document = fixture(); const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context());
    const auto active = active_record(source, make_session()); ProjectWorkspace workspace(source_snapshot);
    require(!workspace.can_undo() && !workspace.can_redo(), "new workspace has no global history");
    auto activation = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(activation);
    require(workspace.can_undo() && !workspace.can_redo(), "first activation must be global undoable");
    auto discard = workspace.prepare_discard_boundary(); (void)workspace.commit(discard);
    require(!workspace.active_boundary() && workspace.snapshot().revision() == source_snapshot.revision(),
            "discard removes active state without changing document revision");
    require(workspace.epoch() == 2 && workspace.edited_generation() == 2 &&
                workspace.checkpoint_generation() == 2, "activation and discard generations");
    auto undo_discard = workspace.prepare_undo(); (void)workspace.commit(undo_discard);
    require(workspace.active_boundary() == std::optional{active} && workspace.can_redo(),
            "undo discard restores the complete checkpoint and exposes redo");
    auto pointer_edit = active; pointer_edit.checkpoint.pointer = Vec2{20, 21};
    auto pointer_ticket = workspace.prepare_boundary_checkpoint(pointer_edit); (void)workspace.commit(pointer_ticket);
    require(workspace.active_boundary() == std::optional{pointer_edit} && workspace.can_redo() &&
                workspace.edited_generation() == 3 && workspace.checkpoint_generation() == 4,
            "pointer replacement preserves redo and leaves edited generation unchanged");
    auto redo_discard = workspace.prepare_redo(); (void)workspace.commit(redo_discard);
    require(!workspace.active_boundary() && !workspace.can_redo(), "redo removes latest checkpoint");
    auto undo_pointer_discard = workspace.prepare_undo(); (void)workspace.commit(undo_pointer_discard);
    require(workspace.active_boundary() == std::optional{pointer_edit},
            "undo after redo restores the new pointer exactly");
}

void check_global_ordering_keeps_exact_ids() {
    auto document = fixture(); const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context());
    const auto active_a = active_record(source, make_session()); const auto active_b = active_record(source, make_session());
    require(active_a != active_b, "independent sessions must have distinct IDs"); ProjectWorkspace workspace(source_snapshot);
    auto activate_a = workspace.prepare_boundary_checkpoint(active_a); (void)workspace.commit(activate_a);
    auto discard_a = workspace.prepare_discard_boundary(); (void)workspace.commit(discard_a);
    auto activate_b = workspace.prepare_boundary_checkpoint(active_b); (void)workspace.commit(activate_b);
    auto undo_b = workspace.prepare_undo(); (void)workspace.commit(undo_b); require(!workspace.active_boundary(), "first undo removes B");
    auto undo_a = workspace.prepare_undo(); (void)workspace.commit(undo_a); require(workspace.active_boundary() == std::optional{active_a}, "second undo restores exact A");
    auto redo_a = workspace.prepare_redo(); (void)workspace.commit(redo_a); require(!workspace.active_boundary(), "first redo discards A");
    auto redo_b = workspace.prepare_redo(); (void)workspace.commit(redo_b); require(workspace.active_boundary() == std::optional{active_b}, "second redo restores exact B");
    require(workspace.epoch() == 7 && workspace.edited_generation() == 7 && workspace.checkpoint_generation() == 7, "global lifecycle generations");
}

void check_semantic_replacement_clears_redo() {
    auto document = fixture(); const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context()); const auto session = make_session();
    const auto active = active_record(source, session); ProjectWorkspace workspace(source_snapshot);
    auto activation = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(activation);
    auto discard = workspace.prepare_discard_boundary(); (void)workspace.commit(discard);
    auto undo_discard = workspace.prepare_undo(); (void)workspace.commit(undo_discard); require(workspace.can_redo(), "undo discard leaves redo");
    auto semantic_session = session; (void)semantic_session.add_line_to({9, 2});
    auto replacement = workspace.prepare_boundary_checkpoint(active_record(source, semantic_session)); (void)workspace.commit(replacement);
    require(!workspace.can_redo() && workspace.edited_generation() == 4, "semantic replacement clears global redo");
    rejected_no_redo([&] { (void)workspace.prepare_redo(); }, "redo must not fall back to document redo");
}

void check_document_redo_and_active_source_ordering() {
    auto baseline = fixture(); baseline.apply(edit_label(baseline.snapshot(), "branched")); (void)baseline.undo(baseline.revision());
    const auto baseline_snapshot = baseline.snapshot(); require(!baseline_snapshot.history().back().redo_stack.empty(), "imported baseline retains document redo");
    const auto baseline_source = capture_boundary_recovery_source(baseline_snapshot, context()); ProjectWorkspace imported(baseline_snapshot);
    auto activate = imported.prepare_boundary_checkpoint(active_record(baseline_source, make_session())); (void)imported.commit(activate);
    require(!imported.can_redo() && !imported.snapshot().history().back().redo_stack.empty(), "activation clears global redo but preserves imported document redo");

    auto document = fixture(); const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context()); const auto active = active_record(source, make_session());
    ProjectWorkspace workspace(source_snapshot); auto activation = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(activation);
    auto edit = workspace.prepare(edit_label(workspace.snapshot(), "changed")); (void)workspace.commit(edit);
    auto discard = workspace.prepare_discard_boundary(); (void)workspace.commit(discard);
    auto undo_discard = workspace.prepare_undo(); (void)workspace.commit(undo_discard);
    require(workspace.active_boundary() == std::optional{active} && workspace.snapshot().entities().at("label").properties.at("text") == "changed", "undo discard preserves stale source across edit");
    auto undo_document = workspace.prepare_undo(); (void)workspace.commit(undo_document);
    require(workspace.active_boundary() == std::optional{active} && workspace.snapshot().entities().at("label").properties.at("text") == "Original" && workspace.snapshot().document_id() == source_snapshot.document_id(), "undo document restores document and old source");
}

void check_discard_ticket_rejections() {
    auto document = fixture(); const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context()); const auto active = active_record(source, make_session());
    ProjectWorkspace workspace(source_snapshot), foreign(source_snapshot); auto activation = workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(activation);
    auto foreign_ticket = workspace.prepare_discard_boundary(); rejected([&] { (void)foreign.commit(foreign_ticket); }, "foreign discard ticket must be rejected");
    require(workspace.active_boundary() == std::optional{active} && foreign.epoch() == 0, "foreign rejection leaves both workspaces unchanged");
    auto stale_discard = workspace.prepare_discard_boundary(); auto stale_document = workspace.prepare(edit_label(workspace.snapshot(), "stale"));
    auto pointer_edit = active; pointer_edit.checkpoint.pointer = Vec2{30, 31}; auto pointer_ticket = workspace.prepare_boundary_checkpoint(pointer_edit); (void)workspace.commit(pointer_ticket);
    rejected([&] { (void)workspace.commit(stale_discard); }, "stale discard ticket must be rejected"); rejected([&] { (void)workspace.commit(stale_document); }, "stale document ticket must be rejected");
    require(workspace.active_boundary() == std::optional{pointer_edit} && workspace.epoch() == 2, "stale rejection preserves newer lifecycle state");
    auto consumed = workspace.prepare_discard_boundary(); (void)workspace.commit(consumed); const auto epoch = workspace.epoch(); const auto generation = workspace.checkpoint_generation(); const auto digest = document_snapshot_digest(workspace.snapshot());
    rejected([&] { (void)workspace.commit(consumed); }, "consumed discard ticket must be rejected");
    require(!workspace.active_boundary() && workspace.epoch() == epoch && workspace.checkpoint_generation() == generation && document_snapshot_digest(workspace.snapshot()) == digest, "consumed rejection must not mutate state");
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        check_discard_round_trip_and_pointer_redo(); check_global_ordering_keeps_exact_ids();
        check_semantic_replacement_clears_redo(); check_document_redo_and_active_source_ordering();
        check_discard_ticket_rejections();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
