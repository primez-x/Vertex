#include "sketch/project_store.hpp"
#include "sketch/boundary_commit.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid phase/history operation accepted");
}
void commit(ProjectWorkspace& workspace, PreparedWorkspaceEdit edit) { (void)workspace.commit(edit); }
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("phase-history-" + make_stable_id());
    TemporaryDirectory() { std::filesystem::create_directory(path); }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void run(const char* name) {
    const auto path = std::filesystem::path(VERTEX_TEST_SOURCE_ROOT) / "tests" / "fixtures" /
        "phase-subtraction-history" / name;
    const auto loaded = ProjectStore::load_archive(path, ArchiveRole::ordinary);
    require(loaded.supported(), "pre-change Finish archive remains supported");
    auto workspace = ProjectWorkspace::restore_archive(*loaded.archive, *loaded.recovery.decoded);
    const auto before = workspace->capture();
    require(before.retired_boundaries().size() == 1 && !before.active_boundary(), "golden archive retains an undone Finish");
    const auto namespace_id = before.retired_boundaries().begin()->first;
    const auto input = before.retired_boundaries().begin()->second.value;
    require(input && input->auto_subtract_target_id == "parent", "golden retains active-v2 Auto-Subtract target");
    const auto encoded = encode_workspace_history_record(before.document(),
        capture_workspace_history_record(before), before.active_boundary());
    require(encoded.dump() == loaded.archive->recovery().front().envelope.dump(),
        "restore/re-encode preserves exact history and opaque number representation");
    TemporaryDirectory directory;
    const ProjectArchiveSnapshot roundtrip(before.document(),
        {{"history", "workspace_history", encoded}}, ArchiveRole::ordinary);
    (void)ProjectStore::save_archive(directory.path / "reencoded.sketch", roundtrip);
    const auto reopened = ProjectStore::load_archive(directory.path / "reencoded.sketch", ArchiveRole::ordinary);
    require(reopened.supported() && reopened.archive->recovery().front().envelope.dump() == encoded.dump(),
        "pre-change Finish saves and reopens without history loss");

    const auto session = BoundaryAuthoringSession::from_recovery_checkpoint(input->checkpoint);
    const BoundaryCommitIntent intent{session.options(), session.accepted_chains(), input->source.context,
        "Finish boundary", input->auto_subtract_target_id};
    const auto prefix = Document::fork_at_revision(before.document(), input->source.revision);
    require(!preview_boundary_commit(prefix.snapshot(), intent).accepted(),
        "fresh preview refuses the historical inactive/demolished target");
    commit(*workspace, workspace->prepare_redo());
    const auto after = workspace->snapshot();
    require(after.entities().at("parent").properties.at("deduction_ids").size() == 1,
        "historical Redo restores its original deduction");
    validate_historical_boundary_commit(prefix.snapshot(), intent, after.entities());
    auto forged = after.entities();
    forged.at("parent").properties["opaque_number"] = 1;
    rejects([&] { validate_historical_boundary_commit(prefix.snapshot(), intent, forged); });
    forged = after.entities(); forged.at("parent").required = true;
    rejects([&] { validate_historical_boundary_commit(prefix.snapshot(), intent, forged); });
    forged = after.entities(); forged.at("parent").extensions["extra"] = 1;
    rejects([&] { validate_historical_boundary_commit(prefix.snapshot(), intent, forged); });
    forged = after.entities(); forged.emplace("unrelated", Entity{"unrelated", "label", {{"text", "forged"}}});
    rejects([&] { validate_historical_boundary_commit(prefix.snapshot(), intent, forged); });
    forged = after.entities(); forged.erase("parent");
    rejects([&] { validate_historical_boundary_commit(prefix.snapshot(), intent, forged); });

    commit(*workspace, workspace->prepare_undo());
    require(entity_map_digest(workspace->snapshot().entities()) == entity_map_digest(before.document().entities()),
        "historical Undo restores the exact pre-Finish entity map");
    const auto before_revise = workspace->capture();
    rejects([&] { (void)workspace->prepare_revise_boundary(namespace_id); });
    require(document_snapshot_digest(workspace->snapshot()) == document_snapshot_digest(before_revise.document()) &&
        workspace->active_boundary() == before_revise.active_boundary(), "refused live Revise preserves archived geometry and input");
    ProjectWorkspace fresh_workspace(prefix.snapshot());
    auto active = *input;
    active.source = capture_boundary_recovery_source(fresh_workspace.snapshot(), input->source.context);
    commit(fresh_workspace, fresh_workspace.prepare_boundary_checkpoint(active));
    const auto fresh = fresh_workspace.capture();
    rejects([&] { (void)fresh_workspace.prepare_finish_boundary(); });
    require(document_snapshot_digest(fresh_workspace.snapshot()) == document_snapshot_digest(fresh.document()) &&
        fresh_workspace.active_boundary() == fresh.active_boundary(), "fresh refused Finish changes neither geometry nor pending input");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { run("inactive-parent.sketch"); run("demolished-parent.sketch"); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
