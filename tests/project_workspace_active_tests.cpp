#include "sketch/project_workspace.hpp"
#include "sketch/boundary_recovery_source.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace sketch;
using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <class F>
void rejected(F&& operation, std::string_view message) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

Document fixture() {
    return Document::create({
        {"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}},
        {"label", "label", {{"text", "Original"}}}});
}

DrawingContext context() { return {"p", "b", "f", "l"}; }

Command edit_label(const DocumentSnapshot& snapshot, std::string_view value) {
    auto label = snapshot.entities().at("label");
    label.properties["text"] = std::string(value);
    return ApplyEntityChanges{.expected_revision = snapshot.revision(),
                              .entity_changes = {EntityChange::upsert(std::move(label))},
                              .message = "Edit label"};
}

BoundaryAuthoringSession make_session() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({1, 2});
    (void)session.add_line_to({5, 2});
    return session;
}

BoundaryActiveRecovery active_record(const BoundaryRecoverySource& source,
                                     const BoundaryAuthoringSession& session) {
    return {source, session.recovery_checkpoint(),
            {{"future", {{"items", Json::array({1, "two", nullptr})}}}}};
}

void check_publication_and_detachment() {
    auto document = fixture();
    const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context());
    auto session = make_session();
    session.set_pointer({8, 9});
    auto first = active_record(source, session);
    const auto expected_first = first;

    ProjectWorkspace workspace(source_snapshot);
    require(!workspace.active_boundary(), "a new workspace has no active boundary");

    const auto document_before = workspace.snapshot();
    auto ticket = workspace.prepare_boundary_checkpoint(first);
    require(workspace.epoch() == 0 && workspace.edited_generation() == 0 &&
                workspace.checkpoint_generation() == 0 &&
                document_snapshot_digest(workspace.snapshot()) ==
                    document_snapshot_digest(document_before),
            "checkpoint preparation must not mutate the workspace");

    // The prepared ticket owns a detached candidate and must not observe
    // caller mutations made after preparation.
    first.checkpoint.pointer = Vec2{100, 100};
    first.checkpoint.extensions["future"]["tampered"] = true;
    first.source.document_id = "tampered-source";
    const auto returned_revision = workspace.commit(ticket);
    require(returned_revision == document_before.revision() && workspace.snapshot().revision() ==
                document_before.revision(),
            "checkpoint publication must retain the document revision");
    require(workspace.epoch() == 1 && workspace.edited_generation() == 1 &&
                workspace.checkpoint_generation() == 1,
            "first checkpoint publication must advance both generations");
    require(workspace.document_history().events.empty(),
            "checkpoint publication must not append a document event");
    require(workspace.active_boundary() && *workspace.active_boundary() == expected_first,
            "checkpoint publication must use the sealed payload");

    // Mutating a returned active record or document snapshot must not mutate
    // authoritative workspace state.
    auto detached_active = workspace.active_boundary();
    detached_active->checkpoint.pointer = Vec2{-1, -2};
    detached_active->checkpoint.extensions["future"]["detached"] = true;
    detached_active->source.authoring_digest = std::string(64, '0');
    auto detached_document = workspace.snapshot();
    const_cast<std::map<std::string, Entity, std::less<>>&>(detached_document.entities())
        .at("label")
        .properties["text"] = "detached";
    require(workspace.active_boundary() && *workspace.active_boundary() == expected_first,
            "active boundary getter must return detached values");
    require(document_snapshot_digest(workspace.snapshot()) ==
                document_snapshot_digest(document_before),
            "workspace snapshot getter must return detached values");

    rejected([&] { (void)workspace.prepare_boundary_checkpoint(expected_first); },
             "an exact checkpoint repeat must be rejected");
}

void check_pointer_replacement_and_ticket_invalidation() {
    auto document = fixture();
    const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context());
    auto session = make_session();
    session.set_pointer({8, 9});
    const auto first = active_record(source, session);

    ProjectWorkspace workspace(source_snapshot);
    auto first_ticket = workspace.prepare_boundary_checkpoint(first);
    (void)workspace.commit(first_ticket);

    auto pointer_update = first;
    pointer_update.checkpoint.pointer = Vec2{9, 10};
    auto stale_document_ticket = workspace.prepare(edit_label(workspace.snapshot(), "document edit"));
    auto pointer_ticket = workspace.prepare_boundary_checkpoint(pointer_update);
    const auto before_pointer = workspace.snapshot();
    const auto returned_revision = workspace.commit(pointer_ticket);
    require(returned_revision == before_pointer.revision() && workspace.snapshot().revision() ==
                before_pointer.revision(),
            "pointer replacement must retain the document revision");
    require(workspace.epoch() == 2 && workspace.edited_generation() == 1 &&
                workspace.checkpoint_generation() == 2,
            "pointer-only replacement must advance epoch and checkpoint generation only");
    require(workspace.active_boundary() && *workspace.active_boundary() == pointer_update,
            "pointer replacement must publish the new pointer");

    rejected([&] { (void)workspace.commit(stale_document_ticket); },
             "a document ticket concurrent with checkpoint publication must be stale");
    require(workspace.epoch() == 2 && workspace.edited_generation() == 1 &&
                workspace.checkpoint_generation() == 2 &&
                document_snapshot_digest(workspace.snapshot()) ==
                    document_snapshot_digest(before_pointer) &&
                workspace.active_boundary() && *workspace.active_boundary() == pointer_update,
            "stale document rejection must preserve the complete checkpoint state");

    auto foreign_document = fixture();
    ProjectWorkspace foreign(foreign_document.snapshot());
    auto foreign_payload = pointer_update;
    foreign_payload.checkpoint.pointer = Vec2{10, 11};
    auto foreign_ticket = workspace.prepare_boundary_checkpoint(foreign_payload);
    rejected([&] { (void)foreign.commit(foreign_ticket); },
             "a checkpoint ticket must be bound to its originating workspace");
    require(foreign.epoch() == 0 && foreign.edited_generation() == 0 &&
                foreign.checkpoint_generation() == 0 && !foreign.active_boundary(),
            "foreign checkpoint rejection must preserve the receiver");
}

void check_immutable_binding_and_monotonic_counters() {
    auto document = fixture();
    const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context());
    auto session = make_session();
    session.set_pointer({8, 9});
    const auto first = active_record(source, session);

    ProjectWorkspace workspace(source_snapshot);
    auto first_ticket = workspace.prepare_boundary_checkpoint(first);
    (void)workspace.commit(first_ticket);

    auto pointer_update = first;
    pointer_update.checkpoint.pointer = Vec2{9, 10};
    auto pointer_ticket = workspace.prepare_boundary_checkpoint(pointer_update);
    (void)workspace.commit(pointer_ticket);

    auto changed_namespace = pointer_update;
    changed_namespace.checkpoint.identity_namespace += "-foreign";
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(changed_namespace); },
             "checkpoint replacement must preserve the authoring identity namespace");

    auto changed_source = pointer_update;
    changed_source.source.document_id = "foreign-document";
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(changed_source); },
             "checkpoint replacement must preserve the captured source");

    auto changed_mode = pointer_update;
    changed_mode.checkpoint.mode = BoundaryAuthoringMode::define_first;
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(changed_mode); },
             "checkpoint replacement must preserve the authoring mode");

    // Publish each local transition independently. Local navigation changes
    // saved draft content without creating a Document revision or event.
    (void)session.add_line_to({9, 2});
    auto line_ticket = workspace.prepare_boundary_checkpoint(active_record(source, session));
    (void)workspace.commit(line_ticket);
    require(workspace.edited_generation() == 2, "a semantic line advances edited generation");
    require(session.undo(), "local semantic undo fixture");
    auto undo_ticket = workspace.prepare_boundary_checkpoint(active_record(source, session));
    (void)workspace.commit(undo_ticket);
    require(workspace.edited_generation() == 3, "local undo advances edited generation");
    require(session.redo(), "local semantic redo fixture");
    const auto later = active_record(source, session);
    require(later.checkpoint.counters.next_segment_id >
                first.checkpoint.counters.next_segment_id,
            "semantic line and local undo/redo must retain monotonic counters");
    auto later_ticket = workspace.prepare_boundary_checkpoint(later);
    (void)workspace.commit(later_ticket);
    require(workspace.edited_generation() == 4 && workspace.checkpoint_generation() == 5 &&
                workspace.epoch() == 5 && workspace.snapshot().revision() == source_snapshot.revision() &&
                workspace.document_history().events.empty(),
            "local redo advances draft generations while preserving document history");
    const auto before_decrease = workspace.active_boundary();
    const auto before_epoch = workspace.epoch();
    const auto before_checkpoint_generation = workspace.checkpoint_generation();
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(pointer_update); },
             "a valid earlier checkpoint must not decrease counters");
    require(workspace.epoch() == before_epoch &&
                workspace.checkpoint_generation() == before_checkpoint_generation &&
                workspace.active_boundary() && before_decrease &&
                *workspace.active_boundary() == *before_decrease,
            "decreasing-counter rejection must preserve the current checkpoint");
}

void check_invalid_codec_and_stale_source_rollback() {
    auto document = fixture();
    const auto source_snapshot = document.snapshot();
    const auto source = capture_boundary_recovery_source(source_snapshot, context());
    auto session = make_session();
    session.set_pointer({8, 9});
    const auto first = active_record(source, session);

    ProjectWorkspace workspace(source_snapshot);
    auto first_ticket = workspace.prepare_boundary_checkpoint(first);
    (void)workspace.commit(first_ticket);
    const auto before_invalid = workspace.active_boundary();
    const auto before_invalid_epoch = workspace.epoch();
    const auto before_invalid_edited_generation = workspace.edited_generation();
    const auto before_invalid_checkpoint_generation = workspace.checkpoint_generation();

    auto invalid_checkpoint = first;
    invalid_checkpoint.checkpoint.version = 0;
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(invalid_checkpoint); },
             "codec-invalid checkpoints must be rejected before publication");
    require(workspace.epoch() == before_invalid_epoch &&
                workspace.edited_generation() == before_invalid_edited_generation &&
                workspace.checkpoint_generation() == before_invalid_checkpoint_generation &&
                workspace.active_boundary() && before_invalid &&
                *workspace.active_boundary() == *before_invalid,
            "codec rejection must roll back the complete active state");

    auto edit_ticket = workspace.prepare(edit_label(workspace.snapshot(), "changed"));
    (void)workspace.commit(edit_ticket);
    require(workspace.active_boundary() && *workspace.active_boundary() == *before_invalid,
            "document edits must preserve the visible active checkpoint");
    require(inspect_boundary_recovery_source(workspace.snapshot(), source) ==
                BoundaryRecoverySourceStatus::stale_revision,
            "a document edit must make the captured source stale");

    auto stale_replacement = *before_invalid;
    stale_replacement.checkpoint.pointer = Vec2{10, 11};
    const auto before_stale = workspace.active_boundary();
    const auto before_stale_epoch = workspace.epoch();
    rejected([&] { (void)workspace.prepare_boundary_checkpoint(stale_replacement); },
             "checkpoint mutation must reject a stale document source");
    require(workspace.epoch() == before_stale_epoch && workspace.active_boundary() &&
                before_stale && *workspace.active_boundary() == *before_stale &&
                workspace.snapshot().entities().at("label").properties.at("text") == "changed",
            "stale-source rejection must preserve the document and active record");
    auto undo = workspace.prepare_undo();
    (void)workspace.commit(undo);
    require(workspace.active_boundary() == before_stale &&
                inspect_boundary_recovery_source(workspace.snapshot(), source) ==
                    BoundaryRecoverySourceStatus::stale_revision,
            "document undo preserves the draft without silently rebinding identical geometry");
    auto redo = workspace.prepare_redo();
    (void)workspace.commit(redo);
    require(workspace.active_boundary() == before_stale,
            "document redo preserves the original draft and source");
}

void check_aggregate_capture() {
    auto document = fixture();
    ProjectWorkspace workspace(document.snapshot());
    auto session = make_session();
    const auto source = capture_boundary_recovery_source(workspace.snapshot(), context());
    const auto initial = workspace.capture();
    auto checkpoint = active_record(source, session);
    auto ticket = workspace.prepare_boundary_checkpoint(checkpoint);
    (void)workspace.commit(ticket);
    auto captured = workspace.capture();
    const auto copied = captured;
    require(captured.identity() == workspace.identity() && captured.epoch() == 1 &&
                captured.edited_generation() == 1 && captured.checkpoint_generation() == 1 &&
                captured.active_boundary() == workspace.active_boundary() &&
                captured.document_history() == workspace.document_history() &&
                captured.resource_policy() == boundary_authoring_default_resource_policy,
            "capture must contain one complete current workspace generation");
    require(initial.epoch() == 0 && !initial.active_boundary(),
            "a retained capture must not observe later workspace publication");
    const auto original_digest = document_snapshot_digest(captured.document());
    checkpoint.checkpoint.pointer = Vec2{14, 15};
    auto pointer_ticket = workspace.prepare_boundary_checkpoint(checkpoint);
    (void)workspace.commit(pointer_ticket);
    const auto pointer_capture = workspace.capture();
    require(pointer_capture.epoch() == 2 && pointer_capture.edited_generation() == 1 &&
                pointer_capture.checkpoint_generation() == 2 && captured.epoch() == 1 &&
                captured.active_boundary() == copied.active_boundary(),
            "pointer capture must retain associated counters without changing older captures");
    const_cast<std::optional<BoundaryActiveRecovery>&>(captured.active_boundary())
        ->source.document_id = "tampered detached capture";
    const_cast<std::map<std::string, Entity, std::less<>>&>(captured.document().entities())
        .at("label").properties["text"] = "tampered detached capture";
    require(document_snapshot_digest(workspace.snapshot()) == original_digest &&
                document_snapshot_digest(copied.document()) == original_digest &&
                copied.active_boundary()->source == source &&
                workspace.active_boundary()->source == source,
            "capture and its copies must own detached document and recovery data");
    auto edit = workspace.prepare(edit_label(workspace.snapshot(), "later"));
    (void)workspace.commit(edit);
    const auto edited = workspace.capture();
    require(edited.document_history().events.size() == 1 &&
                edited.document().revision() == 1 && edited.epoch() == 3 &&
                edited.edited_generation() == 2 && edited.checkpoint_generation() == 3 &&
                edited.active_boundary()->source == source,
            "capture must pair document history and stale active provenance with the new generation");
    validate_workspace_document_history(edited.document(), edited.document_history());
    const auto detached_after_destruction = [&] {
        ProjectWorkspace temporary(document.snapshot());
        auto active = temporary.prepare_boundary_checkpoint(active_record(source, session));
        (void)temporary.commit(active);
        return temporary.capture();
    }();
    require(detached_after_destruction.epoch() == 1 &&
                detached_after_destruction.active_boundary()->source == source &&
                document_snapshot_digest(detached_after_destruction.document()) == original_digest,
            "background capture must remain usable after its workspace is destroyed");
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        check_publication_and_detachment();
        check_pointer_replacement_and_ticket_invalidation();
        check_immutable_binding_and_monotonic_counters();
        check_invalid_codec_and_stale_source_rollback();
        check_aggregate_capture();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
