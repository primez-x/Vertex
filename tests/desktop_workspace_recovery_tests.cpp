#include "sketch/desktop/main_window.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/recovery_discovery.hpp"
#include "sketch/workspace_history_record.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAction>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void draft_click(sketch::desktop::PlanCanvas* canvas, QPoint offset) {
    const QPointF point(canvas->rect().center() + offset);
    QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
}
void draft_key(QWidget* canvas, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(canvas, &event);
}
void test_discard_navigation() {
    using namespace sketch;
    desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.show();
    QApplication::processEvents();
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living_area"), "begin discarded draft");
    draft_click(canvas, {-60, -60});
    draft_click(canvas, {60, -60});
    draft_click(canvas, {60, 60});
    require(window.undoCommand(), "draft local undo before discard");
    draft_key(canvas, Qt::Key_Escape);
    require(window.undoCommand(), "undo discard");
    require(canvas->boundaryDraftPreview() && canvas->boundaryDraftPreview()->segments.size() == 1,
        "undo discard reconstructs visible interactive draft");
    const auto actions = window.findChildren<QAction*>();
    const auto redo = std::find_if(actions.begin(), actions.end(), [](QAction* action) {
        return action->text() == QStringLiteral("Redo");
    });
    require(redo != actions.end() && (*redo)->isEnabled(), "lifecycle redo is available through toolbar");
    require(window.redoCommand() && !canvas->boundaryDraftPreview(), "redo discard clears restored draft");
    require(window.undoCommand(), "undo discard again");
    draft_click(canvas, {60, 60});
    draft_click(canvas, {-60, 60});
    const auto before = window.document().snapshot().entities().size();
    draft_key(canvas, Qt::Key_Return);
    require(window.document().snapshot().entities().size() > before, "restored draft continues and finishes without reopen");
}
void test_redefine_recovery() {
    using namespace sketch;
    for (const bool recovery : {false, true}) {
        QTemporaryDir temporary;
        require(temporary.isValid(), "redefine temporary directory");
        const auto destination = temporary.filePath("draft.bldproj");
        QString id;
        std::size_t count{};
        nlohmann::json original;
        IdentifiedBoundary original_boundary;
        Entity reference;
        {
            desktop::MainWindow window;
            window.setAttribute(Qt::WA_DontShowOnScreen, true);
            window.show();
            QApplication::processEvents();
            id = window.createBoundary({{{0,0},{4,0},0}, {{4,0},{4,4},0},
                {{4,4},{0,4},0}, {{0,4},{0,0},0}}, "living_area");
            require(!id.isEmpty() && window.selectEntity(id), "select redefine target");
            original_boundary = decode_identified_boundary_entity(window.document().snapshot().entities().at(id.toStdString()));
            const auto dimension_id = window.createAngleDimension(id,
                QString::fromStdString(original_boundary.segments[0].segment_id),
                QString::fromStdString(original_boundary.segments[1].segment_id),
                QString::fromStdString(original_boundary.segments[0].end_vertex_id), {2, 1});
            require(!dimension_id.isEmpty() && window.selectEntity(id), "create retained edge reference");
            reference = window.document().snapshot().entities().at(dimension_id.toStdString());
            count = window.document().snapshot().entities().size();
            original = window.document().snapshot().entities().at(id.toStdString()).properties;
            require(window.saveProjectAs(temporary.filePath("source.bldproj")), "save redefine source");
            auto* action = window.findChild<QAction*>("boundaryRedefinition");
            require(action != nullptr, "redefine action");
            action->trigger();
            auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
            draft_click(canvas, {-60,-60});
            draft_click(canvas, {60,-60});
            if (recovery) {
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < 10000 && !std::filesystem::exists(std::filesystem::path(window.recoveryCopyPath().toStdWString()))) {
                    QApplication::processEvents();
                    QThread::msleep(5);
                }
                std::filesystem::copy_file(std::filesystem::path(window.recoveryCopyPath().toStdWString()),
                    std::filesystem::path(destination.toStdWString()));
            } else require(window.saveProjectAs(destination), "save redefine draft");
        }
        desktop::MainWindow restored;
        restored.setAttribute(Qt::WA_DontShowOnScreen, true);
        restored.show();
        QApplication::processEvents();
        restored.document().mark_saved(restored.document().revision());
        require(restored.openProject(destination), "reopen redefine draft");
        auto* canvas = dynamic_cast<desktop::PlanCanvas*>(restored.findChild<QWidget*>("measurementPlanCanvas"));
        draft_click(canvas, {60,60});
        draft_click(canvas, {-60,60});
        draft_key(canvas, Qt::Key_Return);
        const auto snapshot = restored.document().snapshot();
        require(snapshot.entities().size() == count && restored.selectedEntityId() == id,
            "recovered redefine preserves target identity and entity count");
        require(snapshot.entities().at(id.toStdString()).properties != original,
            "recovered redefine changes existing geometry");
        require(snapshot.entities().at(reference.id) == reference, "recovered redefine retains dependent reference");
        const auto replaced = decode_identified_boundary_entity(snapshot.entities().at(id.toStdString()));
        for (std::size_t index = 0; index < replaced.segments.size(); ++index) {
            require(replaced.segments[index].segment_id == original_boundary.segments[index].segment_id &&
                replaced.segments[index].start_vertex_id == original_boundary.segments[index].start_vertex_id &&
                replaced.segments[index].end_vertex_id == original_boundary.segments[index].end_vertex_id,
                "recovered redefine preserves referenced segment and vertex identities");
        }
    }
}
void test_invalid_redefine_recovery() {
    using namespace sketch;
    QTemporaryDir temporary;
    desktop::MainWindow window;
    window.document().mark_saved(window.document().revision());
    const auto before = document_snapshot_digest(window.document().snapshot());
    int fixture_index = 0;
    for (const auto& operation : {
        nlohmann::json{{"version", 1}, {"kind", "unknown"}, {"target_id", "missing"}},
        nlohmann::json{{"version", 1}, {"kind", "redefine"}, {"target_id", "missing"}},
        nlohmann::json{{"version", 1}, {"kind", "redefine"}, {"target_id", 7}}}) {
        ProjectWorkspace workspace(window.document().snapshot());
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        (void)session.anchor({0, 0});
        BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(),
            {"property-1", "building-1", "floor-1", "layer-1"}), session.recovery_checkpoint()};
        active.extensions["desktop_operation"] = operation;
        auto edit = workspace.prepare_boundary_checkpoint(active);
        (void)workspace.commit(edit);
        const auto capture = workspace.capture();
        const RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(
            capture.document(), capture_workspace_history_record(capture), capture.active_boundary())},
            {"active", "boundary_active", encode_boundary_active_recovery(active)}};
        const auto path = std::filesystem::path(temporary.filePath(
            QStringLiteral("invalid-%1.bldproj").arg(fixture_index++)).toStdWString());
        (void)ProjectStore::save_archive(path, ProjectArchiveSnapshot(capture.document(), ledger, ArchiveRole::ordinary));
        require(!window.openProject(QString::fromStdWString(path.wstring())) &&
            document_snapshot_digest(window.document().snapshot()) == before,
            "invalid or absent redefine target fails closed before replacing current project");
    }
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

void test_startup_recovery_selection() {
    using namespace sketch;
    const auto original_name = QCoreApplication::applicationName();
    const auto original_test_mode = QStandardPaths::isTestModeEnabled();
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-startup-recovery-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto recovery_directory = std::filesystem::path(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toStdWString()) /
        "recovery";
    std::filesystem::create_directories(recovery_directory);
    const auto recovery_path = recovery_directory / "recovery-startup.bldproj";
    std::string recovered_document_id;
    {
        ProjectWorkspace workspace(Document::create().snapshot());
        auto edit = workspace.prepare(NameRevision{workspace.snapshot().revision(), "Startup edit"});
        (void)workspace.commit(edit);
        const auto snapshot = workspace.capture();
        recovered_document_id = snapshot.document().document_id();
        const auto history = capture_workspace_history_record(snapshot);
        RecoveryCopyRecord record;
        record.archive_id = "startup-recovery";
        record.owner_token = "test-owner";
        record.document_id = snapshot.document().document_id();
        record.workspace_epoch = snapshot.epoch();
        // Model a recovery capture that is one authoring generation ahead of
        // the last explicit save. The copy metadata is what startup uses to
        // decide whether asking the user is warranted.
        record.edited_generation = 1;
        record.checkpoint_generation = 1;
        record.autosaved_checkpoint_generation = 1;
        record.saved_edited_generation = 0;
        const RecoveryLedger ledger{
            {"history", "workspace_history",
             encode_workspace_history_record(snapshot.document(), history, snapshot.active_boundary())},
            {"copy", "recovery_copy", encode_recovery_copy_record(record)}};
        (void)ProjectStore::save_archive(recovery_path,
            ProjectArchiveSnapshot(snapshot.document(), ledger, ArchiveRole::recovery_copy));
    }
    bool responded = false;
    {
        desktop::MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.show();
        QApplication::processEvents();
        QTimer responder;
        responder.setInterval(1);
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (message == nullptr || responded) return;
            auto* accept = message->button(QMessageBox::Yes);
            require(accept != nullptr, "startup recovery prompt must offer recovery");
            responded = true;
            accept->click();
        });
        responder.start();
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(5000);
        QObject::connect(&timeout, &QTimer::timeout, &window, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
            }
        });
        timeout.start();
        const auto opened = window.offerStartupRecovery();
        timeout.stop();
        require(opened, "startup recovery prompt must open the selected copy");
        responder.stop();
        require(responded && window.document().snapshot().document_id() == recovered_document_id,
                "startup recovery must load the persisted recovery document");
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(recovery_directory, cleanup_error);
    require(!cleanup_error, "startup recovery fixture cleanup");
    QCoreApplication::setApplicationName(original_name);
    QStandardPaths::setTestModeEnabled(original_test_mode);
}

void test_live_boundary_recovery() {
    using namespace sketch;
    QTemporaryDir temporary;
    require(temporary.isValid(), "live draft temporary directory");
    desktop::MainWindow drawing;
    drawing.setAttribute(Qt::WA_DontShowOnScreen, true);
    drawing.show();
    QApplication::processEvents();
    require(drawing.saveProjectAs(temporary.filePath("clean.bldproj")), "save clean draft source");
    const auto entities = drawing.document().snapshot().entities();
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(
        drawing.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas != nullptr, "live draft canvas");
    const auto click = [](desktop::PlanCanvas* target, QPoint offset) {
        const QPointF point(target->rect().center() + offset);
        QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &release);
    };
    require(drawing.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living_area"), "start live draft");
    click(canvas, {-80, -80});
    click(canvas, {80, -80});
    click(canvas, {80, 80});
    require(drawing.undoCommand(), "undo live draft edge");
    require(drawing.document().snapshot().entities() == entities, "draft leaves committed geometry unchanged");
    std::optional<BoundaryActiveRecovery> captured;
    wait_until([&] {
        if (drawing.recoveryCopyPath().isEmpty()) return false;
        const auto path = std::filesystem::path(drawing.recoveryCopyPath().toStdWString());
        if (!std::filesystem::exists(path)) return false;
        const auto archive = ProjectStore::load_archive(path, ArchiveRole::recovery_copy);
        if (!archive.supported() || !archive.recovery.decoded->active) return false;
        captured = archive.recovery.decoded->active;
        return true;
    }, "draft-only drawing publishes automatic recovery");
    const auto recovered_path = temporary.filePath("recovered.bldproj");
    std::filesystem::copy_file(std::filesystem::path(drawing.recoveryCopyPath().toStdWString()),
        std::filesystem::path(recovered_path.toStdWString()));
    desktop::MainWindow recovered;
    recovered.setAttribute(Qt::WA_DontShowOnScreen, true);
    recovered.show();
    recovered.document().mark_saved(recovered.document().revision());
    require(recovered.openProject(recovered_path), "open live draft recovery");
    auto* restored = dynamic_cast<desktop::PlanCanvas*>(
        recovered.findChild<QWidget*>("measurementPlanCanvas"));
    require(restored && restored->boundaryDraftPreview() &&
        restored->boundaryDraftPreview()->segments.size() == canvas->boundaryDraftPreview()->segments.size(),
        "restored draft preview matches unfinished drawing");
    const bool saved_recovered = recovered.saveProjectAs(temporary.filePath("exact.bldproj"));
    require(saved_recovered,
        ("save recovered draft: " + recovered.lastError().toStdString()).c_str());
    const auto exact = ProjectStore::load_archive(
        std::filesystem::path(temporary.filePath("exact.bldproj").toStdWString()), ArchiveRole::ordinary);
    require(exact.supported() && exact.recovery.decoded->active == captured,
        "restoration preserves exact receipts, undo redo history and drawing context");
    require(!exact.recovery.decoded->recovery_copy,
        "ordinary save removes only destination-specific recovery copy metadata");
    QTimer reject_replacement;
    QObject::connect(&reject_replacement, &QTimer::timeout, &recovered, [] {
        if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if (auto* button = prompt->button(QMessageBox::Cancel)) button->click();
    });
    reject_replacement.start(1);
    require(!recovered.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living_area"),
        "existing recovered draft cannot be replaced without discard");
    reject_replacement.stop();
    const auto invalid_path = temporary.filePath("invalid.bldproj");
    { std::ofstream invalid(std::filesystem::path(invalid_path.toStdWString())); invalid << "invalid archive"; }
    QTimer allow_open;
    QObject::connect(&allow_open, &QTimer::timeout, &recovered, [] {
        if (auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if (auto* button = prompt->button(QMessageBox::Discard)) button->click();
    });
    allow_open.start(1);
    require(!recovered.openProject(invalid_path), "invalid recovery open fails safely");
    allow_open.stop();
    require(restored->boundaryDraftPreview() && recovered.document().snapshot().entities() == entities,
        "failed open preserves live session and geometry");
    require(recovered.redoCommand(), "recovered draft retains redo edge");
    click(restored, {-80, 80});
    QKeyEvent finish(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(restored, &finish);
    require(!restored->boundaryDraftPreview() && recovered.document().snapshot().entities().size() > entities.size(),
        "recovered live draft continues and commits");
    const auto await_retirement = [](desktop::MainWindow& window) {
        std::optional<std::filesystem::file_time_type> inspected_write_time;
        bool retired = false;
        wait_until([&] {
            if (retired) return true;
            if (window.recoveryCopyPath().isEmpty()) return false;
            const auto path = std::filesystem::path(window.recoveryCopyPath().toStdWString());
            if (!std::filesystem::exists(path)) return false;
            const auto write_time = std::filesystem::last_write_time(path);
            if (inspected_write_time == write_time) return false;
            // Loading holds a Windows read lock that denies replacement. Inspect
            // each publication once so polling an old active checkpoint cannot
            // repeatedly block the worker from publishing its retired successor.
            const auto archive = ProjectStore::load_archive(path, ArchiveRole::recovery_copy);
            inspected_write_time = write_time;
            retired = archive.supported() && !archive.recovery.decoded->active;
            return retired;
        }, "automatic recovery retires finished or discarded input");
    };
    await_retirement(recovered);
    require(recovered.saveProject(), "save finalized draft");
    const auto finalized = ProjectStore::load_archive(
        std::filesystem::path(temporary.filePath("exact.bldproj").toStdWString()), ArchiveRole::ordinary);
    require(finalized.supported() && !finalized.recovery.decoded->active, "finalized draft is retired");
    require(recovered.openProject(temporary.filePath("exact.bldproj")) && !restored->boundaryDraftPreview(),
        "finalized draft does not reappear on reopen");
    QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &cancel);
    await_retirement(drawing);
    require(drawing.saveProject(), "save discarded draft");
    require(drawing.openProject(temporary.filePath("clean.bldproj")) && !canvas->boundaryDraftPreview(),
        "discarded draft does not reappear on reopen");
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
    // Opening a current-source record now resumes the actual tool. Retire it
    // before exercising the independent document-command history below.
    auto* recovered_canvas = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>("measurementPlanCanvas"));
    QKeyEvent discard_recovered(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(recovered_canvas, &discard_recovered);
    const auto wall = window.createStraightWall({0, 0}, {4, 0});
    require(!wall.isEmpty(), "recovery workspace accepts desktop wall command");
    require(window.undoCommand() && !window.document().snapshot().entities().contains(wall.toStdString()),
        "recovery workspace undo removes desktop wall");
    require(window.redoCommand() && window.document().snapshot().entities().contains(wall.toStdString()),
        "recovery workspace redo restores desktop wall");
    require(window.saveProject(), "edited recovery workspace saves its updated ledger");
    const auto edited = ProjectStore::load_archive(destination, ArchiveRole::ordinary);
    require(edited.supported() && !edited.recovery.decoded->active &&
        edited.recovery.decoded->history->extensions == history.extensions,
        "edited archive retires discarded input and preserves history extensions");
    std::error_code copy_error;
    const auto independent_copy = directory / "independent-recovery.bldproj";
    std::filesystem::copy_file(destination, independent_copy,
        std::filesystem::copy_options::none, copy_error);
    require(!copy_error, "recovery archive copy for independent reopen");
    require(reopened.openProject(qt_path(destination)) && !reopened.document().dirty(),
        "edited recovery archive reopens read-only beside its owner");
    require(!reopened.document().is_editable() && !reopened.undoCommand(),
        "second open keeps the owned recovery archive read-only");
    require(reopened.openProject(qt_path(independent_copy)) && reopened.document().is_editable(),
        "independent recovery archive reopens writable");
    require(reopened.undoCommand() && !reopened.document().snapshot().entities().contains(wall.toStdString()),
        "reopened recovery archive retains command undo");
    require(reopened.redoCommand() && reopened.saveProject(), "reopened recovery archive redoes and saves");
    require(reopened.openProject(qt_path(independent_copy)),
        "saved independent recovery archive reopens");
    require(reopened.undoCommand() && !reopened.document().snapshot().entities().contains(wall.toStdString()),
        "reopened recovery archive retains command undo after reload");
    require(reopened.redoCommand() && reopened.saveProject(),
        "reopened recovery archive redoes and saves after reload");
    require(reopened.createNewProject(), "release independent recovery ownership");
    desktop::MainWindow lifecycle;
    lifecycle.document().mark_saved(lifecycle.document().revision());
    require(lifecycle.openProject(qt_path(source)), "open lifecycle fixture");
    const auto lifecycle_path = directory / "lifecycle.bldproj";
    require(lifecycle.saveProjectAs(qt_path(lifecycle_path)), "save lifecycle fixture");
    require(lifecycle.undoCommand() && lifecycle.windowTitle().endsWith(" *"),
        "lifecycle-only undo marks the project dirty");
    require(lifecycle.saveProject(), "save lifecycle-only undo");
    const auto retired = ProjectStore::load_archive(lifecycle_path, ArchiveRole::ordinary);
    require(retired.supported() && retired.recovery.decoded->active &&
        BoundaryAuthoringSession::from_recovery_checkpoint(retired.recovery.decoded->active->checkpoint).can_redo(),
        "draft undo persists its redo history without discarding the session");
    require(lifecycle.openProject(qt_path(lifecycle_path)) && lifecycle.redoCommand() && lifecycle.saveProject(),
        "draft redo survives reopening");
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
    require(seed.createNewProject(), "release legacy source ownership");
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
        require(autosaved.openProject(qt_path(independent_copy)), "open automatic recovery fixture");
        const auto source_hash = ProjectStore::file_sha256(independent_copy);
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
        // for the worker to release its Windows read/write locks. Save As
        // must retire the old owned copy and rebase future recovery beside
        // the new project destination.
        const auto migrated_directory = directory / "migrated";
        std::filesystem::create_directories(migrated_directory);
        const auto migrated_destination = migrated_directory / "autosave-barrier.bldproj";
        const auto old_recovery_path = recovery_path;
        require(autosaved.saveProjectAs(qt_path(migrated_destination)),
            "drain automatic recovery publication before reading it");
        const auto rebased_recovery_path =
            std::filesystem::path(autosaved.recoveryCopyPath().toStdWString());
        require(rebased_recovery_path != old_recovery_path &&
            rebased_recovery_path.parent_path() == migrated_directory &&
            !std::filesystem::exists(old_recovery_path),
            "Save As removes the owned recovery copy and rebases its destination");
        require(!std::filesystem::exists(rebased_recovery_path),
            "rebased recovery destination waits for a newer edit");
        require(!autosaved.createStraightWall({0, 4}, {4, 4}).isEmpty(),
            "edit after recovery destination rebase");
        wait_until([&] { return std::filesystem::exists(rebased_recovery_path); },
            "rebased recovery destination receives the next edit");
        recovery_path = rebased_recovery_path;
        const auto recovered = ProjectStore::load_archive(recovery_path, ArchiveRole::recovery_copy);
        require(recovered.supported() && recovered.recovery.decoded->recovery_copy &&
            recovered.recovery.decoded->recovery_copy->checkpoint_generation ==
                recovered.recovery.decoded->recovery_copy->autosaved_checkpoint_generation,
            "automatic recovery is a supported recovery-role archive with checkpoint metadata");
        require(document_authoring_source_digest_v1(recovered.archive->document()) ==
            document_authoring_source_digest_v1(autosaved.document().snapshot()),
            "automatic recovery contains the captured desktop edit");
        require(ProjectStore::file_sha256(independent_copy) == source_hash,
            "automatic recovery never overwrites source");
        { std::ofstream changed(recovery_path, std::ios::binary | std::ios::app); changed << "external change"; }
        const auto changed_hash = ProjectStore::file_sha256(recovery_path);
        require(!autosaved.createStraightWall({0, 5}, {4, 5}).isEmpty(), "edit after external recovery modification");
        wait_until([&] { return autosaved.lastError().contains("Recovery copy failed"); },
            "external recovery change causes guarded worker failure");
        require(autosaved.document().dirty() && ProjectStore::file_sha256(recovery_path) == changed_hash,
            "failed automatic publication preserves dirty state and external bytes");
    }
    // Destruction joins the queue even following errors; no job keeps files open.
    require(std::filesystem::remove(recovery_path), "shutdown releases recovery destination");
    const auto cleanup_source = directory / "cleanup-source.bldproj";
    std::error_code cleanup_copy_error;
    std::filesystem::copy_file(destination, cleanup_source,
        std::filesystem::copy_options::none, cleanup_copy_error);
    require(!cleanup_copy_error, "cleanup fixture project copy");
    {
        desktop::MainWindow cleanup_guard;
        cleanup_guard.document().mark_saved(cleanup_guard.document().revision());
        require(cleanup_guard.openProject(qt_path(cleanup_source)), "open cleanup guard fixture");
        require(!cleanup_guard.createStraightWall({0, 9}, {4, 9}).isEmpty(),
            "edit cleanup guard fixture");
        wait_until([&] {
            if (cleanup_guard.recoveryCopyPath().isEmpty()) return false;
            recovery_path = std::filesystem::path(cleanup_guard.recoveryCopyPath().toStdWString());
            return std::filesystem::exists(recovery_path);
        }, "cleanup guard publishes its recovery copy");
        const auto changed_recovery_path = recovery_path;
        { std::ofstream changed(changed_recovery_path, std::ios::binary | std::ios::app); changed << "foreign change"; }
        const auto cleanup_destination_directory = directory / "cleanup-migrated";
        std::filesystem::create_directories(cleanup_destination_directory);
        require(cleanup_guard.saveProjectAs(qt_path(
            cleanup_destination_directory / "cleanup-guard.bldproj")),
            "Save As succeeds when its prior recovery copy changed");
        require(std::filesystem::exists(changed_recovery_path),
            "changed recovery copy is retained when cleanup ownership cannot be proven");
    }
    require(std::filesystem::remove(recovery_path), "changed recovery destination is released");
    {
        desktop::MainWindow closing;
        closing.document().mark_saved(closing.document().revision());
        require(closing.openProject(qt_path(cleanup_source)), "open shutdown fixture");
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
        const auto old_untitled_recovery_path = recovery_path;
        const auto pre_save_untitled_recovery =
            ProjectStore::load_archive(old_untitled_recovery_path, ArchiveRole::recovery_copy);
        require(pre_save_untitled_recovery.supported() &&
            pre_save_untitled_recovery.recovery.decoded->recovery_copy &&
            !pre_save_untitled_recovery.recovery.decoded->recovery_copy->source_path,
            "untitled recovery copy carries no source path before its first Save As");
        const auto untitled_migrated_directory = directory / "untitled-migrated";
        std::filesystem::create_directories(untitled_migrated_directory);
        const auto untitled_destination =
            untitled_migrated_directory / "untitled-autosave-barrier.bldproj";
        require(untitled.saveProjectAs(qt_path(untitled_destination)),
            "drain untitled recovery publication before reading it");
        const auto rebased_untitled_recovery_path =
            std::filesystem::path(untitled.recoveryCopyPath().toStdWString());
        require(rebased_untitled_recovery_path != old_untitled_recovery_path &&
            rebased_untitled_recovery_path.parent_path() == untitled_migrated_directory &&
            !std::filesystem::exists(old_untitled_recovery_path),
            "untitled Save As removes its owned recovery copy and rebases the destination");
        require(!std::filesystem::exists(rebased_untitled_recovery_path),
            "untitled rebased recovery destination waits for a newer edit");
        require(!untitled.createStraightWall({0, 8}, {4, 8}).isEmpty(),
            "edit after untitled recovery destination rebase");
        wait_until([&] { return std::filesystem::exists(rebased_untitled_recovery_path); },
            "untitled rebased recovery destination receives the next edit");
        recovery_path = rebased_untitled_recovery_path;
        const auto untitled_recovery = ProjectStore::load_archive(recovery_path, ArchiveRole::recovery_copy);
        require(untitled_recovery.supported() && untitled_recovery.recovery.decoded->recovery_copy &&
            untitled_recovery.recovery.decoded->recovery_copy->source_path &&
            std::filesystem::path(std::u8string(
                untitled_recovery.recovery.decoded->recovery_copy->source_path->begin(),
                untitled_recovery.recovery.decoded->recovery_copy->source_path->end())) ==
                untitled_destination,
            "untitled recovery copy records its new source path after Save As");
        require(document_authoring_source_digest_v1(untitled_recovery.archive->document()) ==
            document_authoring_source_digest_v1(untitled.document().snapshot()),
            "untitled recovery copy contains legacy direct edits");
    }
    require(std::filesystem::remove(recovery_path), "untitled recovery destination is released");
    {
        desktop::MainWindow constrained;
        constrained.document().mark_saved(constrained.document().revision());
        require(constrained.openProject(qt_path(source)), "open constraint workspace fixture");
        QKeyEvent discard_constraint_draft(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(constrained.findChild<QWidget*>("measurementPlanCanvas"),
            &discard_constraint_draft);
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
    test_startup_recovery_selection();
    test_live_boundary_recovery();
    test_discard_navigation();
    test_redefine_recovery();
    test_invalid_redefine_recovery();
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--live-boundary"))
            test_live_boundary_recovery();
        else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--discard-navigation"))
            test_discard_navigation();
        else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--redefine-recovery"))
            { test_redefine_recovery(); test_invalid_redefine_recovery(); }
        else run();
    }
    catch (const std::exception& error) {
        std::cerr << "desktop_workspace_recovery: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
