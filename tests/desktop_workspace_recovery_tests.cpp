#include "sketch/desktop/main_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/workspace_history_record.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
QString qt_path(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}
void run() {
    using namespace sketch;
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory");
    const std::filesystem::path directory(temporary.path().toStdWString());
    desktop::MainWindow seed;
    ProjectWorkspace workspace(seed.document().snapshot());
    require(!workspace.snapshot().saved_revision_optional(),
        "fixture must begin with an unsaved recovery marker");
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    session.set_classification("living_area");
    (void)session.anchor({0, 0});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(),
        {"property-1", "building-1", "floor-1", "layer-1"}), session.recovery_checkpoint()};
    auto checkpoint = workspace.prepare_boundary_checkpoint(active);
    (void)workspace.commit(checkpoint);
    auto history = capture_workspace_history_record(workspace.capture());
    history.extensions = {{"desktop_preserves", "future metadata"}};
    RecoveryLedger ledger{{"retained-history", "workspace_history",
        encode_workspace_history_record(workspace.snapshot(), history, active)},
        {"retained-input", "boundary_active", encode_boundary_active_recovery(active)}};
    const auto source = directory / "recovery.bldproj";
    (void)ProjectStore::save_archive(source,
        ProjectArchiveSnapshot(workspace.snapshot(), ledger, ArchiveRole::ordinary));

    desktop::MainWindow window;
    window.document().mark_saved(window.document().revision());
    require(window.openProject(qt_path(source)), "desktop opens supported recovery archive");
    require(document_authoring_source_digest_v1(window.document().snapshot()) ==
        document_authoring_source_digest_v1(workspace.snapshot()), "restored document is exact");
    const auto destination = directory / "saved-recovery.bldproj";
    require(window.saveProjectAs(qt_path(destination)), "desktop saves recovery archive");
    const auto first_loaded = ProjectStore::load_archive(destination, ArchiveRole::ordinary);
    require(first_loaded.supported() && first_loaded.archive->document().saved_revision_optional() ==
        std::optional<Revision>(first_loaded.archive->document().revision()),
        "first recovery save persists the current saved marker");
    desktop::MainWindow reopened;
    reopened.document().mark_saved(reopened.document().revision());
    require(reopened.openProject(qt_path(destination)) && !reopened.document().dirty(),
        "reopening a first-saved recovery archive remains clean");
    require(window.saveProject(), "repeated save accepts current workspace after saved-marker update");
    const auto loaded = ProjectStore::load_archive(destination, ArchiveRole::ordinary);
    require(loaded.supported() && loaded.archive->recovery().size() == ledger.size() &&
        loaded.archive->recovery().front().record_id == ledger.front().record_id &&
        loaded.archive->recovery().front().envelope == ledger.front().envelope,
        "save preserves complete recovery ledger and extensions");
    require(loaded.recovery.decoded->active == active, "save retains active recovery input");
    const auto clean_digest = document_snapshot_digest(window.document().snapshot());
    require(!window.openProject(qt_path(directory / "missing.bldproj")), "missing archive is rejected");
    require(document_snapshot_digest(window.document().snapshot()) == clean_digest,
        "failed open preserves current document");
    require(window.workspaceDocumentsShareDocument(), "workspace tabs retain shared document API");
    const auto file_hash = ProjectStore::file_sha256(destination);
    auto entity = window.document().snapshot().entities().at("property-1");
    entity.properties["name"] = "Direct edit";
    window.document().apply(ApplyEntityChanges{.expected_revision = window.document().revision(),
        .entity_changes = {EntityChange::upsert(std::move(entity))}, .message = "direct edit"});
    require(!window.saveProject() && window.document().dirty(),
        "out-of-band recovery edits fail closed without acknowledging saved state");
    require(ProjectStore::file_sha256(destination) == file_hash,
        "rejected save leaves recovery archive untouched");

    const auto legacy = directory / "legacy.bldproj";
    require(seed.saveProjectAs(qt_path(legacy)), "legacy save remains supported");
    desktop::MainWindow legacy_window;
    legacy_window.document().mark_saved(legacy_window.document().revision());
    require(legacy_window.openProject(qt_path(legacy)), "legacy load remains supported");
    require(!legacy_window.createStraightWall({0, 0}, {4, 0}).isEmpty(), "legacy direct editing");
    require(legacy_window.saveProject() && !legacy_window.document().dirty(), "legacy edited save");
    require(ProjectStore::load(legacy).document.revision() == legacy_window.document().revision(),
        "legacy save contains current direct edits");
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try { run(); }
    catch (const std::exception& error) {
        std::cerr << "desktop_workspace_recovery: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
