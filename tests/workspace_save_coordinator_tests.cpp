#include "sketch/workspace_save_coordinator.hpp"
#include "sketch/boundary_recovery_source.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace sketch;
static_assert(!std::is_copy_constructible_v<SavePublicationTicket>);
static_assert(!std::is_copy_assignable_v<SavePublicationTicket>);
static_assert(std::is_nothrow_move_constructible_v<SavePublicationTicket>);
const std::string digest(64, 'a');
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void invalid(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("malformed publication input accepted");
}
SavePublicationBinding binding() {
    return {"owner", ArchiveRole::ordinary, "destination", "C:/projects/example.sketch"};
}
SaveReceipt receipt(const ProjectWorkspaceSnapshot& captured) {
    return {captured.document().revision(), std::string(64, 'b'), std::nullopt};
}
Document fixture() {
    return Document::create({
        {"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}},
        {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}},
        {"label", "label", {{"text", "Original"}}}});
}
void edit(ProjectWorkspace& workspace) {
    auto label = workspace.snapshot().entities().at("label");
    label.properties["text"] = "Edited";
    auto edit = workspace.prepare(ApplyEntityChanges{
        .expected_revision = workspace.snapshot().revision(),
        .entity_changes = {EntityChange::upsert(label)}, .message = "Edit"});
    (void)workspace.commit(edit);
}
void check_matching_and_replay() {
    ProjectWorkspace workspace(fixture().snapshot());
    edit(workspace);
    const auto captured = workspace.capture();
    const auto before = document_snapshot_digest(captured.document());
    for (auto role : {ArchiveRole::ordinary, ArchiveRole::recovery_copy}) {
        auto target = binding(); target.role = role;
        auto ticket = WorkspaceSaveCoordinator::capture(captured, target, digest);
        auto moved = std::move(ticket);
        require(WorkspaceSaveCoordinator::accept(ticket, receipt(captured), captured, target, digest).status ==
                    SaveAcknowledgementStatus::consumed_ticket, "moved-from ticket accepted");
        const auto accepted = WorkspaceSaveCoordinator::accept(moved, receipt(captured), captured, target, digest);
        require(accepted.acknowledged() && accepted.publication_valid, "current publication not acknowledged");
        require(!WorkspaceSaveCoordinator::accept(moved, receipt(captured), captured, target, digest).acknowledged(),
                "replayed publication acknowledged");
    }
    require(workspace.epoch() == captured.epoch() &&
                workspace.snapshot().saved_revision_optional() == captured.document().saved_revision_optional() &&
                document_snapshot_digest(workspace.snapshot()) == before,
            "acknowledgement gate changed workspace or saved marker");
    auto mutable_binding = binding();
    auto mutable_digest = digest;
    auto sealed = WorkspaceSaveCoordinator::capture(captured, mutable_binding, mutable_digest);
    mutable_binding.owner_token = "mutated-after-capture";
    mutable_digest[0] = 'c';
    require(WorkspaceSaveCoordinator::accept(sealed, receipt(captured), captured, binding(), digest).acknowledged(),
            "ticket retained mutable caller binding or digest");
}
void check_binding_and_receipt_rejections() {
    ProjectWorkspace workspace(fixture().snapshot());
    const auto captured = workspace.capture();
    for (int field = 0; field != 4; ++field) {
        auto current = binding();
        if (field == 0) current.owner_token = "different-owner";
        if (field == 1) current.role = ArchiveRole::recovery_copy;
        if (field == 2) current.destination_identity = "different-destination";
        if (field == 3) current.destination_path = "C:/projects/other.sketch";
        auto ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
        const auto result = WorkspaceSaveCoordinator::accept(ticket, receipt(captured), captured, current, digest);
        require(result.status == SaveAcknowledgementStatus::binding_mismatch && result.publication_valid,
                "changed publication binding accepted or valid old file discarded");
    }
    for (int field = 0; field != 4; ++field) {
        auto saved = receipt(captured);
        if (field == 0) ++saved.revision;
        if (field == 1) saved.file_sha256 = "not-a-hash";
        if (field == 2) saved.file_sha256 = std::string(64, 'B');
        if (field == 3) saved.backup_path = std::filesystem::path{};
        auto ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
        const auto result = WorkspaceSaveCoordinator::accept(ticket, saved, captured, binding(), digest);
        require(result.status == SaveAcknowledgementStatus::invalid_receipt && !result.publication_valid,
                "wrong revision or malformed receipt accepted");
        require(WorkspaceSaveCoordinator::accept(ticket, receipt(captured), captured, binding(), digest).status ==
                    SaveAcknowledgementStatus::consumed_ticket, "failed receipt did not consume ticket");
    }
    auto ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    require(WorkspaceSaveCoordinator::accept(ticket, receipt(captured), captured, binding(), std::string(64, 'c')).status ==
                SaveAcknowledgementStatus::source_mismatch, "different authoring source accepted");
    for (const auto& path : {std::string{}, std::string("bad\0path", 8),
                             std::string(131073, 'p'), std::string("\xff", 1)}) {
        auto target = binding(); target.destination_path = path;
        invalid([&] { (void)WorkspaceSaveCoordinator::capture(captured, target, digest); });
        auto pending = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
        require(WorkspaceSaveCoordinator::accept(pending, receipt(captured), captured, target, digest).status ==
                    SaveAcknowledgementStatus::invalid_current_binding, "malformed current path accepted");
    }
    auto target = binding(); target.role = static_cast<ArchiveRole>(123);
    invalid([&] { (void)WorkspaceSaveCoordinator::capture(captured, target, digest); });
    invalid([&] { (void)WorkspaceSaveCoordinator::capture(captured, binding(), "bad-digest"); });
}
void check_stale_navigation_and_instance() {
    ProjectWorkspace workspace(fixture().snapshot());
    const auto captured = workspace.capture();
    auto ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    edit(workspace);
    auto undo = workspace.prepare_undo(); (void)workspace.commit(undo);
    // Document undo is itself a retained revision; the workspace still has a
    // new epoch/generation and the old save must remain stale.
    require(workspace.snapshot().revision() != captured.document().revision(), "undo did not create a retained revision");
    const auto stale = WorkspaceSaveCoordinator::accept(ticket, receipt(captured), workspace.capture(), binding(), digest);
    require(stale.status == SaveAcknowledgementStatus::stale_workspace && stale.publication_valid,
            "navigation to old revision acknowledged old generations or lost valid file fact");
    ProjectWorkspace foreign(captured.document());
    auto foreign_ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    require(WorkspaceSaveCoordinator::accept(foreign_ticket, receipt(captured), foreign.capture(), binding(), digest).status ==
                SaveAcknowledgementStatus::stale_workspace, "different workspace instance accepted");
    ProjectWorkspace other_document(fixture().snapshot());
    auto document_ticket = WorkspaceSaveCoordinator::capture(captured, binding(), digest);
    require(!WorkspaceSaveCoordinator::accept(document_ticket, receipt(captured), other_document.capture(), binding(), digest)
                 .acknowledged(), "different document accepted");
}
void check_pointer_checkpoint() {
    ProjectWorkspace workspace(fixture().snapshot());
    const auto source = capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"});
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({1, 2});
    auto activate = workspace.prepare_boundary_checkpoint({source, session.recovery_checkpoint()});
    (void)workspace.commit(activate);
    const auto captured = workspace.capture();
    auto ticket = WorkspaceSaveCoordinator::capture(captured, binding(), source.authoring_digest);
    session.set_pointer({8, 9});
    auto pointer = workspace.prepare_boundary_checkpoint({source, session.recovery_checkpoint()});
    (void)workspace.commit(pointer);
    require(workspace.edited_generation() == captured.edited_generation() &&
                workspace.checkpoint_generation() != captured.checkpoint_generation() &&
                workspace.epoch() != captured.epoch(), "pointer generation semantics changed");
    const auto result = WorkspaceSaveCoordinator::accept(ticket, receipt(captured), workspace.capture(), binding(), source.authoring_digest);
    require(result.status == SaveAcknowledgementStatus::stale_workspace && result.publication_valid,
            "pointer-only stale completion acknowledged current checkpoint");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        check_matching_and_replay(); check_binding_and_receipt_rejections();
        check_stale_navigation_and_instance(); check_pointer_checkpoint();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
