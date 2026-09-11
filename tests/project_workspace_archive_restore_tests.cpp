#include "sketch/project_store.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid archive restoration accepted");
}
void commit(ProjectWorkspace& workspace, PreparedWorkspaceEdit edit) { (void)workspace.commit(edit); }
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("workspace-restore-" + make_stable_id());
    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
ProjectArchiveSnapshot archive_for(const ProjectWorkspaceSnapshot& snapshot, ArchiveRole role) {
    auto history = capture_workspace_history_record(snapshot);
    history.extensions = {{"preserve", Json::array({nullptr, 1.0, "x"})}};
    RecoveryLedger rows{{"history", "workspace_history",
        encode_workspace_history_record(snapshot.document(), history, snapshot.active_boundary())}};
    if (snapshot.active_boundary()) rows.push_back({"active", "boundary_active",
        encode_boundary_active_recovery(*snapshot.active_boundary())});
    if (role == ArchiveRole::recovery_copy) {
        RecoveryCopyRecord copy;
        copy.archive_id = "copy"; copy.owner_token = "owner";
        copy.document_id = snapshot.document().document_id();
        copy.workspace_epoch = snapshot.epoch(); copy.edited_generation = snapshot.edited_generation();
        copy.checkpoint_generation = snapshot.checkpoint_generation();
        copy.explicitly_saved_document_revision = snapshot.document().saved_revision_optional();
        rows.push_back({"copy", "recovery_copy", encode_recovery_copy_record(copy)});
    }
    return {snapshot.document(), std::move(rows), role};
}
std::unique_ptr<ProjectWorkspace> roundtrip(const ProjectWorkspaceSnapshot& before, ArchiveRole role) {
    TemporaryDirectory directory;
    const auto archive = archive_for(before, role);
    (void)ProjectStore::save_archive(directory.path / "archive.sketch", archive);
    const auto loaded = ProjectStore::load_archive(directory.path / "archive.sketch", role);
    require(loaded.supported(), "archive must load supported");
    auto restored = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
    const auto after = restored->capture();
    require(document_snapshot_digest(before.document()) == document_snapshot_digest(after.document()),
        "restoration must preserve complete document and saved markers");
    require(after.identity() != before.identity(), "restoration must create a new instance identity");
    require(after.epoch() == before.epoch() && after.edited_generation() == before.edited_generation() &&
        after.checkpoint_generation() == before.checkpoint_generation(), "restoration must preserve counters");
    require(after.active_boundary() == before.active_boundary(), "restoration must preserve active input");
    require(encode_workspace_history_record(after.document(), capture_workspace_history_record(after),
        after.active_boundary()).dump() == archive.recovery().front().envelope.dump(),
        "restoration must preserve history, navigation, retired input and exact extensions");
    return restored;
}
void run(BoundaryAuthoringMode mode) {
    auto document = Document::create({{"p", "property", {{"name", "Property"}}},
        {"b", "building", {{"property_id", "p"}}}, {"f", "floor", {{"building_id", "b"}}},
        {"l", "layer", {{"floor_id", "f"}}}});
    document.mark_saved(document.revision());
    ProjectWorkspace workspace(document.snapshot());
    BoundaryAuthoringSession session(mode);
    session.set_classification("living_area");
    (void)session.anchor({0, 0});
    const auto dimension = [&] { if (mode == BoundaryAuthoringMode::define_first)
        (void)session.place_automatic_dimension(); };
    (void)session.add_line_to({4, 0}); dimension();
    (void)session.add_line_to({4, 3}); dimension();
    (void)session.add_line_to({0, 3}); dimension();
    (void)session.add_closing_segment(); dimension(); (void)session.close_chain();
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(), {"p", "b", "f", "l"}),
        session.recovery_checkpoint(), {{"number", 1.0}}};
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    active.checkpoint.pointer = Vec2{8, 9};
    commit(workspace, workspace.prepare_boundary_checkpoint(active));
    for (const auto role : {ArchiveRole::ordinary, ArchiveRole::recovery_copy}) {
        auto restored = roundtrip(workspace.capture(), role);
        commit(*restored, restored->prepare_finish_boundary());
        require(!restored->active_boundary(), "restored active input must remain finishable");
        require(restored->capture().history_extensions().at("preserve")[1].is_number_float(),
            "edits must retain exact extension values");
    }
    commit(workspace, workspace.prepare_finish_boundary());
    commit(workspace, workspace.prepare_undo());
    auto restored = roundtrip(workspace.capture(), ArchiveRole::ordinary);
    require(restored->retired_boundary(active.checkpoint.identity_namespace).has_value(),
        "finish undo must restore retired input");
    require(!restored->active_boundary(), "retired input must not become active");
    commit(*restored, restored->prepare_redo());
    require(!restored->retired_boundary(active.checkpoint.identity_namespace), "redo must consume retired input");
    commit(workspace, workspace.prepare_revise_boundary(active.checkpoint.identity_namespace));
    (void)roundtrip(workspace.capture(), ArchiveRole::recovery_copy); // Active and retired coexist.

    const auto archive = archive_for(workspace.capture(), ArchiveRole::ordinary);
    const auto decoded = decode_recovery_ledger(archive.document(), archive.recovery(), archive.role());
    const auto& valid = *decoded.decoded;
    auto missing = valid; missing.history.reset();
    rejects([&] { (void)ProjectWorkspace::restore_archive(archive, missing); });
    auto absent_active = valid; absent_active.active.reset();
    rejects([&] { (void)ProjectWorkspace::restore_archive(archive, absent_active); });
    auto absent_retired = valid; absent_retired.history->retired.clear();
    rejects([&] { (void)ProjectWorkspace::restore_archive(archive, absent_retired); });
    auto altered = valid; ++altered.history->checkpoint_generation;
    rejects([&] { (void)ProjectWorkspace::restore_archive(archive, altered); });

    // A typed aggregate supplied by a caller may contain a mutable owner behind
    // shared_ptr<const>. Restoration must install the independently decoded
    // payload rather than retaining that alias.
    auto aliased = valid;
    std::shared_ptr<BoundaryActiveRecovery> mutable_owner;
    for (auto& event : aliased.history->events) {
        if (!event.input || !event.input->value) continue;
        const auto original = event.input->value;
        mutable_owner = std::make_shared<BoundaryActiveRecovery>(*original);
        for (auto& candidate : aliased.history->events)
            if (candidate.input && candidate.input->value.get() == original.get())
                candidate.input->value = mutable_owner;
        for (auto& [name, candidate] : aliased.history->retired)
            if (candidate.value.get() == original.get()) candidate.value = mutable_owner;
        break;
    }
    require(mutable_owner != nullptr, "fixture must contain a historical recovery owner");
    auto aliased_restored = ProjectWorkspace::restore_archive(archive, aliased);
    const auto stable_before = encode_workspace_history_record(aliased_restored->snapshot(),
        capture_workspace_history_record(aliased_restored->capture()), aliased_restored->active_boundary()).dump();
    mutable_owner->checkpoint.pointer = Vec2{123, 456};
    const auto stable_after = encode_workspace_history_record(aliased_restored->snapshot(),
        capture_workspace_history_record(aliased_restored->capture()), aliased_restored->active_boundary()).dump();
    require(stable_before == stable_after, "restoration must detach caller-owned recovery payloads");

    auto future = archive.recovery(); future.front().envelope["version"] = 99;
    ProjectArchiveSnapshot opaque(archive.document(), future, archive.role());
    rejects([&] { (void)ProjectWorkspace::restore_archive(opaque, valid); });
    ProjectArchiveSnapshot wrong_role(archive.document(), archive.recovery(), ArchiveRole::recovery_copy);
    rejects([&] { (void)ProjectWorkspace::restore_archive(wrong_role, valid); });
    auto rows = archive.recovery(); rows.erase(rows.begin());
    ProjectArchiveSnapshot no_history(archive.document(), rows, archive.role());
    rejects([&] { (void)ProjectWorkspace::restore_archive(no_history, valid); });
    auto other = Document::create({});
    ProjectArchiveSnapshot wrong_document(other.snapshot(), archive.recovery(), archive.role());
    rejects([&] { (void)ProjectWorkspace::restore_archive(wrong_document, valid); });
}
void read_only_restoration() {
    auto document = Document::create({{"future", "future_required_type", Json::object(), true}});
    ProjectWorkspace workspace(document.snapshot());
    auto restored = roundtrip(workspace.capture(), ArchiveRole::ordinary);
    require(!restored->snapshot().is_editable() &&
        restored->snapshot().read_only_reason() == document.read_only_reason(),
        "restoration must retain computed read-only protection");
    try {
        (void)restored->prepare(ApplyEntityChanges{.expected_revision = restored->snapshot().revision(),
            .entity_changes = {EntityChange::upsert({"label", "label", {{"text", "blocked"}}})}});
    } catch (const DocumentError& error) {
        require(error.code() == DocumentErrorCode::read_only, "restored edit must fail for read-only protection");
        return;
    }
    throw std::runtime_error("restored read-only document accepted an edit");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { run(BoundaryAuthoringMode::draw_first); run(BoundaryAuthoringMode::define_first); read_only_restoration(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
