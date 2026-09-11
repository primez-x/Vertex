#include "sketch/desktop/main_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/workspace_history_record.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QMouseEvent>
#include <QKeyEvent>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
QString qt_path(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}
template<class Predicate> void wait_until(Predicate predicate, const char* message) {
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate() && timeout.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(predicate(), message);
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
    const auto wall = window.createStraightWall({0, 0}, {4, 0});
    require(!wall.isEmpty(), "recovery workspace accepts desktop wall command");
    require(window.undoCommand() && !window.document().snapshot().entities().contains(wall.toStdString()),
        "recovery workspace undo removes desktop wall");
    require(window.redoCommand() && window.document().snapshot().entities().contains(wall.toStdString()),
        "recovery workspace redo restores desktop wall");
    require(window.saveProject(), "edited recovery workspace saves its updated ledger");
    const auto edited = ProjectStore::load_archive(destination, ArchiveRole::ordinary);
    require(edited.supported() && edited.recovery.decoded->active == active &&
        edited.recovery.decoded->history->extensions == history.extensions,
        "edited archive preserves recovered input and history extensions");
    require(reopened.openProject(qt_path(destination)), "edited recovery archive reopens");
    require(reopened.undoCommand() && !reopened.document().snapshot().entities().contains(wall.toStdString()),
        "reopened recovery archive retains command undo");
    require(reopened.redoCommand() && reopened.saveProject(), "reopened recovery archive redoes and saves");
    desktop::MainWindow lifecycle;
    lifecycle.document().mark_saved(lifecycle.document().revision());
    require(lifecycle.openProject(qt_path(source)), "open lifecycle fixture");
    const auto lifecycle_path = directory / "lifecycle.bldproj";
    require(lifecycle.saveProjectAs(qt_path(lifecycle_path)), "save lifecycle fixture");
    require(lifecycle.undoCommand() && lifecycle.windowTitle().endsWith(" *"),
        "lifecycle-only undo marks the project dirty");
    require(lifecycle.saveProject(), "save lifecycle-only undo");
    const auto retired = ProjectStore::load_archive(lifecycle_path, ArchiveRole::ordinary);
    require(retired.supported() && !retired.recovery.decoded->active,
        "lifecycle undo removes the active record without losing history");
    require(lifecycle.openProject(qt_path(lifecycle_path)) && lifecycle.redoCommand() && lifecycle.saveProject(),
        "lifecycle redo survives reopening");
    const auto reactivated = ProjectStore::load_archive(lifecycle_path, ArchiveRole::ordinary);
    require(reactivated.supported() && reactivated.recovery.decoded->active == active,
        "lifecycle redo restores the retained input record");
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
    const auto divergent_digest = document_snapshot_digest(window.document().snapshot());
    require(window.createStraightWall({0, 1}, {4, 1}).isEmpty() && !window.undoCommand() &&
        document_snapshot_digest(window.document().snapshot()) == divergent_digest,
        "commands reject out-of-band divergence without discarding edits");
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

    // Explicit Save still returns after publication, including worker failures.
    const auto legacy_hash = ProjectStore::file_sha256(legacy);
    const auto invalid_destination = directory / "directory-destination.bldproj";
    std::filesystem::create_directory(invalid_destination);
    require(!legacy_window.createStraightWall({0, 2}, {4, 2}).isEmpty(), "edit before failed queued save");
    require(!legacy_window.saveProjectAs(qt_path(invalid_destination)) && legacy_window.document().dirty(),
        "worker storage failure preserves dirty state");
    require(ProjectStore::file_sha256(legacy) == legacy_hash, "failed Save As preserves current source");
    require(legacy_window.saveProject(), "queue continues after a failed save");

    std::filesystem::path recovery_path;
    {
        desktop::MainWindow autosaved;
        autosaved.document().mark_saved(autosaved.document().revision());
        require(autosaved.openProject(qt_path(destination)), "open automatic recovery fixture");
        const auto source_hash = ProjectStore::file_sha256(destination);
        const auto saved_marker = autosaved.document().snapshot().saved_revision_optional();
        require(!autosaved.createStraightWall({0, 3}, {4, 3}).isEmpty(), "edit automatic recovery fixture");
        wait_until([&] {
            if (autosaved.recoveryCopyPath().isEmpty()) return false;
            recovery_path = std::filesystem::path(autosaved.recoveryCopyPath().toStdWString());
            return std::filesystem::exists(recovery_path);
        }, "scheduled worker publishes recovery copy");
        require(autosaved.document().dirty() &&
            autosaved.document().snapshot().saved_revision_optional() == saved_marker,
            "recovery completion does not mark ordinary document saved");
        // Existence follows the atomic rename; the owner barrier also waits
        // for the worker to release its Windows read/write locks.
        require(autosaved.saveProjectAs(qt_path(directory / "autosave-barrier.bldproj")),
            "drain automatic recovery publication before reading it");
        const auto recovered = ProjectStore::load_archive(recovery_path, ArchiveRole::recovery_copy);
        require(recovered.supported() && recovered.recovery.decoded->recovery_copy &&
            recovered.recovery.decoded->recovery_copy->checkpoint_generation ==
                recovered.recovery.decoded->recovery_copy->autosaved_checkpoint_generation,
            "automatic recovery is a supported recovery-role archive with checkpoint metadata");
        require(document_authoring_source_digest_v1(recovered.archive->document()) ==
            document_authoring_source_digest_v1(autosaved.document().snapshot()),
            "automatic recovery contains the captured desktop edit");
        require(ProjectStore::file_sha256(destination) == source_hash, "automatic recovery never overwrites source");
        { std::ofstream changed(recovery_path, std::ios::binary | std::ios::app); changed << "external change"; }
        const auto changed_hash = ProjectStore::file_sha256(recovery_path);
        require(!autosaved.createStraightWall({0, 4}, {4, 4}).isEmpty(), "edit after external recovery modification");
        wait_until([&] { return autosaved.lastError().contains("Recovery copy failed"); },
            "external recovery change causes guarded worker failure");
        require(autosaved.document().dirty() && ProjectStore::file_sha256(recovery_path) == changed_hash,
            "failed automatic publication preserves dirty state and external bytes");
    }
    // Destruction joins the queue even following errors; no job keeps files open.
    require(std::filesystem::remove(recovery_path), "shutdown releases recovery destination");
    {
        desktop::MainWindow closing;
        closing.document().mark_saved(closing.document().revision());
        require(closing.openProject(qt_path(destination)), "open shutdown fixture");
        require(!closing.createStraightWall({0, 5}, {4, 5}).isEmpty(), "edit shutdown fixture");
        auto* poll = closing.findChild<QTimer*>("workspaceSavePoll");
        require(poll != nullptr, "owner-thread save poll exists");
        // Drive exactly two owner-thread ticks, with no event processing after
        // capture. Destruction must drain the submitted job and join its worker.
        require(QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection), "observe shutdown edit");
        QThread::msleep(2100);
        require(QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection), "enqueue shutdown save");
        recovery_path = std::filesystem::path(closing.recoveryCopyPath().toStdWString());
    }
    require(ProjectStore::load_archive(recovery_path, ArchiveRole::recovery_copy).supported(),
        "destructor drains queued automatic save before returning");
    {
        desktop::MainWindow untitled;
        untitled.document().mark_saved(untitled.document().revision());
        require(!untitled.createStraightWall({0, 7}, {4, 7}).isEmpty(),
            "edit untitled project for automatic recovery");
        auto* untitled_poll = untitled.findChild<QTimer*>("workspaceSavePoll");
        require(untitled_poll != nullptr, "untitled owner-thread save poll exists");
        require(QMetaObject::invokeMethod(untitled_poll, "timeout", Qt::DirectConnection),
            "observe untitled edit");
        QThread::msleep(2100);
        require(QMetaObject::invokeMethod(untitled_poll, "timeout", Qt::DirectConnection),
            "enqueue untitled recovery save");
        wait_until([&] {
            if (untitled.recoveryCopyPath().isEmpty()) return false;
            recovery_path = std::filesystem::path(untitled.recoveryCopyPath().toStdWString());
            return std::filesystem::exists(recovery_path);
        }, "untitled projects receive automatic recovery copies");
        require(untitled.saveProjectAs(qt_path(directory / "untitled-autosave-barrier.bldproj")),
            "drain untitled recovery publication before reading it");
        const auto untitled_recovery = ProjectStore::load_archive(recovery_path, ArchiveRole::recovery_copy);
        require(untitled_recovery.supported() && untitled_recovery.recovery.decoded->recovery_copy &&
            !untitled_recovery.recovery.decoded->recovery_copy->source_path,
            "untitled recovery copy carries role metadata without a source path");
        require(document_authoring_source_digest_v1(untitled_recovery.archive->document()) ==
            document_authoring_source_digest_v1(untitled.document().snapshot()),
            "untitled recovery copy contains legacy direct edits");
    }
    require(std::filesystem::remove(recovery_path), "untitled recovery destination is released");
    {
        desktop::MainWindow constrained;
        constrained.document().mark_saved(constrained.document().revision());
        require(constrained.openProject(qt_path(source)), "open constraint workspace fixture");
        const auto constraint_path = directory / "constraint-workspace.bldproj";
        require(constrained.saveProjectAs(qt_path(constraint_path)), "save before sealed constraint preview");
        const auto constrained_wall = constrained.createStraightWall({0, 6}, {4, 6});
        require(!constrained_wall.isEmpty() && constrained.selectEntity(constrained_wall), "select workspace wall");
        const auto before_constraint = constrained.document().snapshot().entities();
        require(constrained.editSelectedLength("5 m"), "constraint preview commits through workspace after saved-marker change");
        const auto after_constraint = constrained.document().snapshot().entities();
        require(constrained.undoCommand() && constrained.document().snapshot().entities() == before_constraint,
            "workspace undo restores pre-constraint wall");
        require(constrained.redoCommand() && constrained.document().snapshot().entities() == after_constraint,
            "workspace redo restores constraint result");
        require(constrained.saveProject(), "constraint workspace remains saveable with recovery history");
        constrained.setAttribute(Qt::WA_DontShowOnScreen, true);
        constrained.show();
        QApplication::processEvents();
        require(constrained.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living_area"),
            "begin receipt-bearing boundary in recovered workspace");
        auto* canvas = constrained.findChild<QWidget*>("measurementPlanCanvas");
        require(canvas && canvas->width() > 200 && canvas->height() > 200, "boundary canvas is laid out");
        const auto origin = canvas->rect().center();
        for (const auto offset : {QPoint(-60, -60), QPoint(60, -60), QPoint(60, 60), QPoint(-60, 60)}) {
            const QPointF point(origin + offset);
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &release);
        }
        const auto before_boundary = constrained.document().snapshot().entities();
        QKeyEvent finish(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas, &finish);
        require(constrained.document().snapshot().entities().size() > before_boundary.size(),
            "receipt-bearing boundary commits through workspace adapter");
        require(constrained.undoCommand() && constrained.document().snapshot().entities() == before_boundary,
            "workspace undo removes receipt-bearing boundary and dimensions");
        require(constrained.redoCommand() && constrained.saveProject(),
            "workspace boundary redo preserves saveable recovery ledger");
    }
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
