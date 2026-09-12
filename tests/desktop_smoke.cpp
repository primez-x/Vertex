#include "sketch/desktop/main_window.hpp"

#include "sketch/document.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/project_store.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/draft_image_stamp.hpp"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPageSize>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QGroupBox>
#include <QToolButton>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QSplitter>
#include <QTimer>
#include <QToolBar>
#include <QUuid>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "desktop_smoke: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

QTreeWidgetItem* navigator_item(sketch::desktop::MainWindow& window, const QString& id) {
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(tree != nullptr, "project navigator must exist");
    QTreeWidgetItem* result = nullptr;
    for (QTreeWidgetItemIterator item(tree); *item; ++item) {
        if ((*item)->data(0, Qt::UserRole).toString() == id) {
            require(result == nullptr, "navigator must list each identity exactly once");
            result = *item;
        }
    }
    return result;
}

void test_shortcuts_and_measurement_keypad(const QString& capture_directory) {
    const auto original_name = QCoreApplication::applicationName();
    const auto original_test_mode = QStandardPaths::isTestModeEnabled();
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName(QStringLiteral("PropertyStudio-shortcut-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto settings_directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const auto settings_path = settings_directory + QStringLiteral("/keyboard-shortcuts.json");
    {
        sketch::desktop::MainWindow window;
        auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("primaryToolbar"));
            auto* more_tools = window.findChild<QToolButton*>(QStringLiteral("moreTools"));
            auto* theme_menu = window.findChild<QToolButton*>(QStringLiteral("themeMenu"));
            require(toolbar != nullptr && toolbar->minimumHeight() == 12 && toolbar->maximumHeight() == 12 &&
                    toolbar->height() == 12 &&
                    toolbar->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                    toolbar->iconSize() == QSize(8, 8) && more_tools && theme_menu &&
                    more_tools->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                    theme_menu->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                    !more_tools->accessibleName().isEmpty() && !theme_menu->accessibleName().isEmpty() &&
                    window.findChild<QWidget*>(QStringLiteral("workspaceTabs")) != nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("workspaceHeader")) == nullptr &&
                    window.findChild<QLabel*>(QStringLiteral("appMark")) == nullptr &&
                    window.findChild<QLabel*>(QStringLiteral("appTitle")) == nullptr &&
                    window.findChild<QLabel*>(QStringLiteral("projectHeader")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("checkpointBanner")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("offlineBadge")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("appSubtitle")) == nullptr,
                "modern workspace shell must expose a compact toolbar and tabs without redundant branding or status copy");
        auto* settings = window.findChild<QAction*>(QStringLiteral("keyboardShortcutSettings"));
        require(settings, "shortcut editor must be discoverable");
        const auto* copy = window.findChild<QAction*>(QStringLiteral("copySelection"));
        const auto* cut = window.findChild<QAction*>(QStringLiteral("cutSelection"));
        const auto* paste = window.findChild<QAction*>(QStringLiteral("pasteSelection"));
        const auto* remove = window.findChild<QAction*>(QStringLiteral("deleteSelection"));
        const auto* insert_vertex = window.findChild<QAction*>(QStringLiteral("insertBoundaryVertex"));
        const auto* redefine = window.findChild<QAction*>(QStringLiteral("boundaryRedefinition"));
        require(copy && cut && paste && remove && copy->shortcut() == QKeySequence::Copy &&
                    cut->shortcut() == QKeySequence::Cut && paste->shortcut() == QKeySequence::Paste &&
                    remove->shortcut() == QKeySequence::Delete && insert_vertex && redefine,
                "clipboard, delete, and boundary editing commands must be discoverable");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("keyboardShortcutDialog"));
            require(dialog, "shortcut editor must open");
            auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("keyboardShortcutPreset"));
            auto* save = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut-save"));
            auto* open = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("shortcut-open"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("keyboardShortcutButtons"));
            auto* status = dialog->findChild<QLabel*>(QStringLiteral("keyboardShortcutStatus"));
            require(preset && save && open && buttons && status, "shortcut controls must exist");
            preset->setCurrentIndex(2);
            if (!capture_directory.isEmpty())
                require(dialog->grab().save(capture_directory + QStringLiteral("/shortcuts.png")), "shortcut capture");
            require(save->keySequence() == QKeySequence("F2") && open->keySequence() == QKeySequence("F3"),
                    "Apex preset must map the documented save and open keys");
            open->setKeySequence(save->keySequence());
            buttons->button(QDialogButtonBox::Save)->click();
            require(dialog->isVisible() && status->text().contains("more than once") && !QFile::exists(settings_path),
                    "duplicate bindings must fail without persisting or dismissing the editor");
            open->setKeySequence(QKeySequence("Ctrl+Z"));
            buttons->button(QDialogButtonBox::Save)->click();
            require(dialog->isVisible() && status->text().contains("reserved"),
                    "canvas undo must remain reserved");
            open->setKeySequence(QKeySequence("F3"));
            buttons->button(QDialogButtonBox::Save)->click();
        });
        settings->trigger();
        require(QFile::exists(settings_path), "custom shortcuts must be saved locally");
        auto* define = window.findChild<QAction*>(QStringLiteral("defineAreaShortcut"));
        require(define && define->shortcut() == QKeySequence("F4"), "Apex Define Area binding must activate");
        sketch::desktop::MainWindow reopened;
        require(reopened.findChild<QAction*>(QStringLiteral("defineAreaShortcut"))->shortcut() == QKeySequence("F4"),
                "shortcuts must survive a new workspace window");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("keyboardShortcutDialog"));
            dialog->findChild<QComboBox*>(QStringLiteral("keyboardShortcutPreset"))->setCurrentIndex(1);
            dialog->reject();
        });
        settings->trigger();
        require(define->shortcut() == QKeySequence("F4"), "cancel must preserve active bindings");

        const auto wall = window.createStraightWall({0, 0}, {4, 0});
        require(!wall.isEmpty() && window.selectEntity(wall), "keypad fixture wall must be selectable");
        auto* keypad = window.findChild<QPushButton*>(QStringLiteral("measurementKeypad"));
        require(keypad, "inspector keypad must be discoverable");
        const auto revision = window.document().revision();
        const auto original_height = window.document().snapshot().entities().at(wall.toStdString()).properties.at("height_m");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("measurementKeypadDialog"));
            require(dialog, "keypad must open");
            auto* target = dialog->findChild<QComboBox*>(QStringLiteral("measurementKeypadTarget"));
            auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("measurementKeypadValue"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("measurementKeypadButtons"));
            require(target && input && buttons, "keypad controls must exist");
            target->setCurrentIndex(target->findData(1));
            require(target->currentData().toInt() == 1, "wall height must be editable through keypad");
            if (!capture_directory.isEmpty())
                require(dialog->grab().save(capture_directory + QStringLiteral("/measurement-keypad.png")), "keypad capture");
            input->setText(QStringLiteral("1/0 ft"));
            buttons->button(QDialogButtonBox::Apply)->click();
            require(dialog->isVisible() && window.document().revision() == revision &&
                    !dialog->findChild<QLabel*>(QStringLiteral("measurementKeypadStatus"))->text().isEmpty(),
                    "invalid keypad measurement must preserve document and show a useful error");
            input->clear();
            dialog->findChild<QPushButton*>(QStringLiteral("measurementKeypadToken10"))->click();
            dialog->findChild<QPushButton*>(QStringLiteral("measurementKeypadToken15"))->click();
            require(input->text() == QStringLiteral("3m"), "on-screen keys must compose an explicit-unit quantity");
            buttons->button(QDialogButtonBox::Apply)->click();
        });
        keypad->click();
        require(window.document().snapshot().entities().at(wall.toStdString()).properties.at("height_m") == 3.0,
                "keypad must commit the exact selected dimension through the document command");
        require(window.undoCommand() &&
                window.document().snapshot().entities().at(wall.toStdString()).properties.at("height_m") == original_height,
                "keypad edit must participate in undo");
        require(window.selectEntity(wall), "reselect wall after undo clears the selection");
        const auto cancel_revision = window.document().revision();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("measurementKeypadDialog"));
            dialog->findChild<QLineEdit*>(QStringLiteral("measurementKeypadValue"))->setText(QStringLiteral("20m"));
            dialog->reject();
        });
        keypad->click();
        require(window.document().revision() == cancel_revision, "canceling keypad must not modify geometry");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("measurementKeypadDialog"));
            require(!window.createStraightWall({0, 1}, {4, 1}).isEmpty(), "stale keypad fixture must change the document");
            const auto changed_revision = window.document().revision();
            dialog->findChild<QDialogButtonBox*>(QStringLiteral("measurementKeypadButtons"))->button(QDialogButtonBox::Apply)->click();
            require(dialog->isVisible() && window.document().revision() == changed_revision &&
                    dialog->findChild<QLabel*>(QStringLiteral("measurementKeypadStatus"))->text().contains("changed"),
                    "stale keypad must not apply an expression to a changed document or selection");
            dialog->reject();
        });
        keypad->click();

        require(QFile::remove(settings_path) && QDir().mkdir(settings_path), "create blocked settings path fixture");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("keyboardShortcutDialog"));
            dialog->findChild<QComboBox*>(QStringLiteral("keyboardShortcutPreset"))->setCurrentIndex(1);
            dialog->findChild<QDialogButtonBox*>(QStringLiteral("keyboardShortcutButtons"))->button(QDialogButtonBox::Save)->click();
            require(dialog->isVisible() && define->shortcut() == QKeySequence("F4") &&
                    dialog->findChild<QLabel*>(QStringLiteral("keyboardShortcutStatus"))->text().contains("Could not save"),
                    "persistence failure must preserve active shortcuts and keep editor open");
            dialog->reject();
        });
        settings->trigger();
        require(QDir().rmdir(settings_path), "remove blocked settings path fixture");
        QFile corrupt(settings_path);
        require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate), "test settings must be writable");
        corrupt.write("{bad json");
        corrupt.close();
        sketch::desktop::MainWindow fallback;
        require(fallback.findChild<QAction*>(QStringLiteral("defineAreaShortcut"))->shortcut() == QKeySequence("Ctrl+Shift+D"),
                "corrupt settings must fail closed to the complete default preset");
    }
    require(QFile::remove(settings_path), "test shortcut settings must be removed");
    QDir().rmdir(settings_directory);
    QCoreApplication::setApplicationName(original_name);
    QStandardPaths::setTestModeEnabled(original_test_mode);
}

void test_project_subject_metadata() {
    using namespace sketch;
    desktop::MainWindow window;
    require(window.selectEntity(QStringLiteral("property-1")),
            "project property must be selectable for subject editing");
    require(window.findChild<QGroupBox*>(QStringLiteral("projectDetails")) != nullptr &&
                window.findChild<QLineEdit*>(QStringLiteral("projectSubjectName")) != nullptr &&
                window.findChild<QLineEdit*>(QStringLiteral("projectSubjectAddress")) != nullptr &&
                window.findChild<QLineEdit*>(QStringLiteral("projectSubjectReference")) != nullptr &&
                window.findChild<QPlainTextEdit*>(QStringLiteral("projectSubjectAttributes")) != nullptr &&
                window.findChild<QPushButton*>(QStringLiteral("applyProjectDetails")) != nullptr,
            "project subject editor must expose bounded fields");
    const auto before = window.document().revision();
    require(window.editProjectSubject(QStringLiteral("Maple Residence"),
                                      QStringLiteral("123 Main Street"),
                                      QStringLiteral("MLS-2048"),
                                      QStringLiteral("{\"parcel\":\"A-17\",\"zone\":\"R-2\"}")),
            "project subject edit must commit");
    const auto edited = window.document().snapshot().entities().at("property-1");
    require(window.document().revision() == before + 1 &&
                edited.properties.at("name") == "Maple Residence" &&
                edited.properties.at("subject").at("address") == "123 Main Street" &&
                edited.properties.at("subject").at("reference") == "MLS-2048" &&
                edited.properties.at("subject").at("attributes").at("parcel") == "A-17",
            "subject and attributes must be persisted in the property entity");
    const auto invalid_revision = window.document().revision();
    require(!window.editProjectSubject(QStringLiteral("Maple Residence"), {}, {},
                                       QStringLiteral("[\"not an object\"]")) &&
                window.document().revision() == invalid_revision &&
                window.lastError().contains("JSON object"),
            "invalid subject attributes must fail closed without mutation");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at("property-1").properties.at("name") ==
                    "Untitled property" &&
                window.redoCommand() &&
                window.document().snapshot().entities().at("property-1").properties.at("name") ==
                    "Maple Residence",
            "subject edit must participate in undo and redo");
    QTemporaryDir directory;
    require(directory.isValid(), "subject fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("subject.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "subject metadata must survive save and reopen");
    const auto reopened = window.document().snapshot().entities().at("property-1");
    require(reopened.properties.at("subject").at("attributes").at("zone") == "R-2" &&
                window.selectEntity(QStringLiteral("property-1")) &&
                window.findChild<QLineEdit*>(QStringLiteral("projectSubjectAddress"))->text() ==
                    QStringLiteral("123 Main Street"),
            "reopened subject metadata must repopulate the editor");
}

void test_workspace_profiles() {
    const auto original_name = QCoreApplication::applicationName();
    const auto original_test_mode = QStandardPaths::isTestModeEnabled();
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName(QStringLiteral("PropertyStudio-profile-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto settings_directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const auto settings_path = settings_directory + QStringLiteral("/workspace-profiles.json");
    QFile::remove(settings_path);
    {
        sketch::desktop::MainWindow window;
        require(window.findChild<QAction*>(QStringLiteral("workspaceProfiles")) != nullptr,
                "workspace profiles must be available from the secondary command surface");
        window.setWorkspace(sketch::desktop::Workspace::architectural);
        window.setMetricUnits(true);
        auto* grid = window.findChild<QToolButton*>(QStringLiteral("gridTool"));
        auto* snap = window.findChild<QToolButton*>(QStringLiteral("snapTool"));
        auto* overview = window.findChild<QToolButton*>(QStringLiteral("overviewMapTool"));
        auto* workspace_splitter = window.findChild<QSplitter*>(QStringLiteral("workspaceSplitter"));
        auto* architectural_splitter = window.findChild<QSplitter*>(QStringLiteral("architecturalSplitter"));
        require(grid && snap && overview && workspace_splitter && architectural_splitter,
                "profile fixture needs controls and splitter state");
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        workspace_splitter->setSizes({260, 104, 690, 320});
        architectural_splitter->setSizes({520, 300});
        QApplication::processEvents();
        const auto saved_workspace_sizes = workspace_splitter->sizes();
        const auto saved_architectural_sizes = architectural_splitter->sizes();
        grid->setChecked(false);
        snap->setChecked(false);
        overview->setChecked(false);
        require(window.setContainerVisible(QStringLiteral("floor-1"), false),
                "profile fixture should hide a floor");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("workspaceProfilesDialog"));
            require(dialog, "workspace profile editor must open");
            auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("workspaceProfileName"));
            auto* save = dialog->findChild<QPushButton*>(QStringLiteral("saveWorkspaceProfile"));
            auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("applyWorkspaceProfile"));
            auto* selector = dialog->findChild<QComboBox*>(QStringLiteral("workspaceProfileSelector"));
            auto* status = dialog->findChild<QLabel*>(QStringLiteral("workspaceProfileStatus"));
            require(name && save && apply && selector && status,
                    "workspace profile editor must expose save, apply, selection, and status");
            name->setText(QStringLiteral("Field review"));
            save->click();
            require(selector->count() == 1 && QFile::exists(settings_path),
                    "saved workspace profiles must be written locally");
            window.showAllContainers();
            window.setWorkspace(sketch::desktop::Workspace::measurement);
            window.setMetricUnits(false);
            grid->setChecked(true);
            snap->setChecked(true);
            overview->setChecked(true);
            workspace_splitter->setSizes({220, 104, 520, 220});
            architectural_splitter->setSizes({340, 480});
            apply->click();
            require(window.workspace() == sketch::desktop::Workspace::architectural &&
                        window.metricUnits() && !grid->isChecked() && !snap->isChecked() &&
                        !overview->isChecked() &&
                        !window.entityVisible(QStringLiteral("floor-1")) &&
                        workspace_splitter->sizes() == saved_workspace_sizes &&
                        architectural_splitter->sizes() == saved_architectural_sizes &&
                        status->text().contains(QStringLiteral("applied"), Qt::CaseInsensitive),
                    "applying a profile must restore workspace presentation state");
            dialog->reject();
        });
        window.showWorkspaceProfiles();
        require(QFile::exists(settings_path), "workspace profile file must remain after closing editor");
    }
    require(QFile::remove(settings_path), "workspace profile test file must be removed");
    QDir().rmdir(settings_directory);
    QCoreApplication::setApplicationName(original_name);
    QStandardPaths::setTestModeEnabled(original_test_mode);
}

void test_room_boundary_from_existing_geometry() {
    sketch::desktop::MainWindow window;
    const auto first = window.createStraightWall({0.0, 0.0}, {4.0, 0.0}, QStringLiteral("exterior"));
    const auto second = window.createStraightWall({4.0, 0.0}, {4.0, 3.0}, QStringLiteral("exterior"));
    const auto third = window.createStraightWall({0.0, 3.0}, {0.0, 0.0}, QStringLiteral("exterior"));
    const auto fourth = window.createStraightWall({4.0, 3.0}, {0.0, 3.0}, QStringLiteral("exterior"));
    require(!first.isEmpty() && !second.isEmpty() && !third.isEmpty() && !fourth.isEmpty(),
            "existing-geometry fixture must create four walls");
    require(window.selectEntity(first), "existing-geometry fixture must select a source wall");
    const auto before = window.document().revision();
    const auto room = window.createRoomBoundaryFromExistingGeometry(QStringLiteral("Living room"));
    require(!room.isEmpty() && window.document().revision() == before + 1,
            "connected existing walls must create one room boundary command");
    const auto snapshot = window.document().snapshot();
    require(snapshot.entities().at(room.toStdString()).type == "room_boundary" &&
                snapshot.entities().size() == 11 &&
                snapshot.entities().at(room.toStdString()).properties.at("name") == "Living room",
            "room creation must preserve source walls and persist its classification");
    require(window.undoCommand() && !window.document().snapshot().entities().contains(room.toStdString()) &&
                window.redoCommand() && window.document().snapshot().entities().contains(room.toStdString()),
            "room creation from existing geometry must participate in undo and redo");

    const auto branch = window.createStraightWall({4.0, 0.0}, {6.0, 0.0}, QStringLiteral("partition"));
    require(!branch.isEmpty() && window.selectEntity(first),
            "branched existing-geometry fixture must be selectable");
    const auto before_reject = window.document().revision();
    require(window.createRoomBoundaryFromExistingGeometry(QStringLiteral("Invalid" )).isEmpty() &&
                window.document().revision() == before_reject &&
                window.lastError().contains(QStringLiteral("exactly two"), Qt::CaseInsensitive),
            "branched existing walls must fail without duplicating or mutating geometry");
}

void test_selection_clipboard_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id = window.createStraightWall({0.0, 0.0}, {5.0, 0.0},
                                                   QStringLiteral("exterior"));
    require(!wall_id.isEmpty() && window.selectEntity(wall_id),
            "clipboard fixture wall must be selectable");
    const auto opening_id = window.createHostedOpening(QStringLiteral("door"),
                                                       QStringLiteral("1 ft"),
                                                       QStringLiteral("3 ft"),
                                                       QStringLiteral("0 ft"),
                                                       QStringLiteral("7 ft"));
    require(!opening_id.isEmpty() && window.selectEntity(wall_id),
            "clipboard fixture opening must be hosted by the selected wall");
    const auto source = window.document().snapshot();
    const auto source_wall = source.entities().at(wall_id.toStdString());
    const auto source_opening = source.entities().at(opening_id.toStdString());
    const auto copy_revision = window.document().revision();
    require(window.copySelection() && window.document().revision() == copy_revision,
            "copy must publish a local clipboard payload without changing the document");
    const auto clipboard_text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    require(clipboard_text.contains(QStringLiteral("sketch.document.clipboard")) &&
                clipboard_text.contains(wall_id) && clipboard_text.contains(opening_id),
            "clipboard payload must identify its format and retain the selected wall graph");

    require(window.pasteSelection(), "pasting a copied wall graph should succeed");
    const auto pasted = window.document().snapshot();
    QString pasted_wall_id;
    QString pasted_opening_id;
    for (const auto& [id, entity] : pasted.entities()) {
        if (entity.type == "wall" && id != wall_id.toStdString() &&
            entity.properties.at("baseline") == source_wall.properties.at("baseline")) {
            pasted_wall_id = QString::fromStdString(id);
        }
    }
    require(!pasted_wall_id.isEmpty(), "paste must create a fresh wall identity");
    for (const auto& [id, entity] : pasted.entities()) {
        if (entity.type == "opening" && entity.properties.value("wall_id", "") ==
                pasted_wall_id.toStdString()) {
            pasted_opening_id = QString::fromStdString(id);
        }
    }
    require(!pasted_opening_id.isEmpty() && pasted_opening_id != opening_id &&
                pasted.entities().at(pasted_opening_id.toStdString()).properties.at("width_m") ==
                    source_opening.properties.at("width_m") &&
                window.selectedEntityId() == pasted_wall_id,
            "paste must remap hosted opening links and select the new root");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(pasted_wall_id.toStdString()) &&
                window.redoCommand() &&
                window.document().snapshot().entities().contains(pasted_wall_id.toStdString()),
            "pasted graph must be one undoable command");

    require(window.selectEntity(pasted_wall_id) && window.cutSelection(),
            "cut must remove the selected wall graph");
    require(!window.document().snapshot().entities().contains(pasted_wall_id.toStdString()) &&
                !window.document().snapshot().entities().contains(pasted_opening_id.toStdString()),
            "cut must remove hosted openings with their wall");
    require(window.undoCommand() &&
                window.document().snapshot().entities().contains(pasted_wall_id.toStdString()) &&
                window.document().snapshot().entities().contains(pasted_opening_id.toStdString()),
            "cut must restore the complete graph through undo");

    const auto malformed_revision = window.document().revision();
    QGuiApplication::clipboard()->setText(QStringLiteral("{\"format\":\"wrong\"}"),
                                          QClipboard::Clipboard);
    require(!window.pasteSelection() && window.document().revision() == malformed_revision &&
                window.lastError().contains(QStringLiteral("clipboard"), Qt::CaseInsensitive),
            "malformed clipboard data must fail closed without mutation");
}

void test_delete_selection_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id = window.createStraightWall({0.0, 0.0}, {5.0, 0.0});
    require(!wall_id.isEmpty() && window.selectEntity(wall_id),
            "delete fixture wall must be selectable");
    const auto opening_id = window.createHostedOpening(QStringLiteral("window"),
                                                       QStringLiteral("1 ft"),
                                                       QStringLiteral("3 ft"),
                                                       QStringLiteral("3 ft"),
                                                       QStringLiteral("4 ft"));
    require(!opening_id.isEmpty() && window.selectEntity(wall_id),
            "delete fixture opening must be hosted by the wall");
    const auto before = window.document().revision();
    require(window.deleteSelection() && window.document().revision() == before + 1,
            "deleting a wall graph must commit one guarded command");
    require(!window.document().snapshot().entities().contains(wall_id.toStdString()) &&
                !window.document().snapshot().entities().contains(opening_id.toStdString()),
            "deleting a wall must remove its owned hosted openings");
    require(window.undoCommand() &&
                window.document().snapshot().entities().contains(wall_id.toStdString()) &&
                window.document().snapshot().entities().contains(opening_id.toStdString()) &&
                window.redoCommand() &&
                !window.document().snapshot().entities().contains(wall_id.toStdString()),
            "delete must restore and remove the complete graph through undo and redo");

    const auto boundary_id = window.createBoundary(
        Boundary{{{{0.0, 0.0}, {4.0, 0.0}, 0.0},
                  {{4.0, 0.0}, {4.0, 2.0}, 0.0},
                  {{4.0, 2.0}, {0.0, 2.0}, 0.0},
                  {{0.0, 2.0}, {0.0, 0.0}, 0.0}}}, QStringLiteral("measurement"));
    require(!boundary_id.isEmpty() && window.selectEntity(boundary_id) &&
                window.deleteSelection(),
            "a closed measurement boundary should be deletable when unreferenced");
    require(!window.document().snapshot().entities().contains(boundary_id.toStdString()),
            "deleted boundary must not remain in the document");
}

void test_boundary_vertex_insertion_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto boundary_id = window.createBoundary(
        Boundary{{{{0.0, 0.0}, {4.0, 0.0}, 0.0},
                  {{4.0, 0.0}, {4.0, 2.0}, 0.0},
                  {{4.0, 2.0}, {0.0, 2.0}, 0.0},
                  {{0.0, 2.0}, {0.0, 0.0}, 0.0}}}, QStringLiteral("measurement"));
    require(!boundary_id.isEmpty() && window.selectEntity(boundary_id),
            "vertex insertion fixture must create a selectable boundary");
    const auto original = window.document().snapshot().entities().at(boundary_id.toStdString());
    const auto identified = decode_identified_boundary_entity(original);
    const auto segment_id = QString::fromStdString(identified.segments.front().segment_id);
    const auto before = window.document().revision();
    const auto insertion_ok = window.insertSelectedBoundaryVertex(segment_id, QStringLiteral("0.5"));
    require(insertion_ok &&
                window.document().revision() == before + 1,
            "vertex insertion must commit one semantic boundary edit");
    const auto inserted_id = window.selectedEntityId();
    require(!inserted_id.isEmpty() && inserted_id != boundary_id &&
                !window.document().snapshot().entities().contains(boundary_id.toStdString()),
            "vertex insertion must replace the source with a fresh boundary identity");
    const auto inserted = decode_identified_boundary_entity(
        window.document().snapshot().entities().at(inserted_id.toStdString()));
    require(inserted.segments.size() == identified.segments.size() + 1 &&
                inserted.segments.front().segment.end.x == 2.0 &&
                validate_boundary(boundary_geometry(inserted)).empty(),
            "vertex insertion must split the analytical edge and preserve valid topology");
    require(window.undoCommand() &&
                window.document().snapshot().entities().contains(boundary_id.toStdString()) &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    boundary_id.toStdString())).segments.size() == identified.segments.size() &&
                window.redoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    inserted_id.toStdString())).segments.size() == identified.segments.size() + 1,
            "vertex insertion must be exactly undoable and redoable");
    const auto rejected_revision = window.document().revision();
    require(window.selectEntity(inserted_id) &&
                !window.insertSelectedBoundaryVertex(segment_id, QStringLiteral("1.0")) &&
                window.document().revision() == rejected_revision,
            "vertex insertion must reject an endpoint fraction without mutation");
}

void test_boundary_redefinition_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto boundary_id = window.createBoundary(
        Boundary{{{{0.0, 0.0}, {4.0, 0.0}, 0.0},
                  {{4.0, 0.0}, {4.0, 2.0}, 0.0},
                  {{4.0, 2.0}, {0.0, 2.0}, 0.0},
                  {{0.0, 2.0}, {0.0, 0.0}, 0.0}}}, QStringLiteral("measurement"));
    require(!boundary_id.isEmpty() && window.selectEntity(boundary_id),
            "redefinition fixture must create a selectable boundary");
    const auto original_model = decode_identified_boundary_entity(
        window.document().snapshot().entities().at(boundary_id.toStdString()));
    const auto before = window.document().revision();
    const auto replacement = Boundary{{{{0.0, 0.0}, {5.0, 0.0}, 0.0},
                                       {{5.0, 0.0}, {5.0, 3.0}, 0.0},
                                       {{5.0, 3.0}, {0.0, 3.0}, 0.0},
                                       {{0.0, 3.0}, {0.0, 0.0}, 0.0}}};
    require(window.redefineSelectedBoundary(replacement, QStringLiteral("living")) &&
                window.document().revision() == before + 1,
            "redefinition must commit one semantic boundary edit");
    const auto updated = window.document().snapshot().entities().at(boundary_id.toStdString());
    const auto model = decode_identified_boundary_entity(updated);
    require(model.segments.size() == 4 && model.segments.front().segment.end.x == 5.0 &&
                model.segments[1].segment.end.y == 3.0 &&
                updated.properties.at("classification") == "living",
            "redefinition must preserve identity while replacing analytical geometry and classification");
    require(window.undoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    boundary_id.toStdString())).segments.front().segment.end.x == 4.0 &&
                window.redoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    boundary_id.toStdString())).segments.front().segment.end.x == 5.0 &&
                original_model.segments.front().segment_id == model.segments.front().segment_id &&
                original_model.segments.front().start_vertex_id == model.segments.front().start_vertex_id,
            "redefinition must be exactly undoable and retain stable edge identities");
    const auto rejected_revision = window.document().revision();
    auto open = replacement;
    open.pop_back();
    require(window.selectEntity(boundary_id) && !window.redefineSelectedBoundary(open) &&
                window.document().revision() == rejected_revision,
            "redefinition must reject invalid topology without mutation");
}

void test_automatic_room_boundary_detection_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_ids = QStringList{
        window.createStraightWall({0.0, 0.0}, {2.0, 0.0}),
        window.createStraightWall({2.0, 0.0}, {4.0, 0.0}),
        window.createStraightWall({4.0, 0.0}, {4.0, 2.0}),
        window.createStraightWall({4.0, 2.0}, {2.0, 2.0}),
        window.createStraightWall({2.0, 2.0}, {0.0, 2.0}),
        window.createStraightWall({0.0, 2.0}, {0.0, 0.0}),
        window.createStraightWall({2.0, 0.0}, {2.0, 2.0}),
        window.createStraightWall({12.0, 12.0}, {13.0, 12.0})};
    require(std::all_of(wall_ids.begin(), wall_ids.end(), [](const auto& id) { return !id.isEmpty(); }),
            "automatic detection fixture must create wall graph");
    require(window.selectEntity(wall_ids.front()), "automatic detection must select a seed wall");
    const auto before = window.document().revision();
    const auto room_ids = window.detectRoomBoundariesFromExistingWalls(QStringLiteral("detected room"));
    require(room_ids.size() == 2 && window.document().revision() == before + 1,
            "automatic detection must create all bounded rooms in one command");
    for (const auto& room_id : room_ids) {
        const auto entity = window.document().snapshot().entities().at(room_id.toStdString());
        require(entity.type == "room_boundary" && entity.properties.at("classification") == "detected room",
                "detected faces must become independent classified room boundaries");
        const auto model = decode_identified_boundary_entity(entity);
        require(model.segments.size() == 4 &&
                    std::abs(signed_area(boundary_geometry(model))) == 4.0,
                "detected room must retain one valid analytical face");
    }
    require(window.undoCommand(), "automatic room detection must be undoable");
    for (const auto& room_id : room_ids) {
        require(!window.document().snapshot().entities().contains(room_id.toStdString()),
                "undo must remove every detected room from the compound command");
    }
    require(window.redoCommand(), "automatic room detection must be redoable");
    for (const auto& room_id : room_ids) {
        require(window.document().snapshot().entities().contains(room_id.toStdString()),
                "redo must restore every detected room");
    }
}

void test_named_revisions() {
    using namespace sketch;
    desktop::MainWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("revisionHistory"));
    require(action, "named revisions should be available from the secondary command surface");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("revisionHistoryDialog"));
        require(dialog, "named revision editor must open");
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("revisionList"));
        auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("revisionName"));
        auto* name_current = dialog->findChild<QPushButton*>(QStringLiteral("nameCurrentRevision"));
        auto* compare = dialog->findChild<QPushButton*>(QStringLiteral("compareRevisions"));
        auto* comparison = dialog->findChild<QPlainTextEdit*>(QStringLiteral("revisionComparison"));
        auto* status = dialog->findChild<QLabel*>(QStringLiteral("revisionStatus"));
        require(list && name && name_current && compare && comparison && status,
                "named revision editor must expose history, naming, compare, and status controls");
        name->setText(QStringLiteral("Existing conditions"));
        name_current->click();
        require(list->count() == 1 && window.document().snapshot().named_revisions().contains(
                    "Existing conditions"),
                "naming the current revision must create one persisted history marker");
        const auto named_revision = window.document().snapshot().named_revisions().at(
            "Existing conditions");
        const auto wall = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
        require(!wall.isEmpty() && window.document().revision() > named_revision,
                "later edits must remain available after naming a revision");
        compare->click();
        require(comparison->toPlainText().contains(QStringLiteral("Entities added: 1")) &&
                    status->text().contains(QStringLiteral("without changing"), Qt::CaseInsensitive),
                "comparison must report later semantic changes without mutating the document");
        dialog->reject();
    });
    action->trigger();
    const auto current_revision = window.document().revision();
    require(window.document().snapshot().named_revisions().contains("Existing conditions") &&
                window.document().revision() == current_revision,
            "closing revision history must retain the named marker and current head");

    QTemporaryDir directory;
    require(directory.isValid(), "revision restore fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("existing-conditions.bldproj"));
    const auto before_restore = window.document().snapshot();
    require(window.restoreNamedRevision(QStringLiteral("Existing conditions"), path),
            "restoring a named revision must write a portable project copy");
    const auto restored = ProjectStore::load(std::filesystem::path(path.toStdWString()));
    require(!restored.document.dirty() && restored.document.revision() ==
                before_restore.named_revisions().at("Existing conditions") &&
                restored.document.snapshot().entities().size() < before_restore.entities().size() &&
                window.document().snapshot().entities().size() == before_restore.entities().size(),
            "restoring a revision must leave later work in the current document untouched");
}

void test_boundary_transform_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto boundary_id = window.createBoundary(
        Boundary{{{{0.0, 0.0}, {4.0, 0.0}, 0.0},
                  {{4.0, 0.0}, {4.0, 2.0}, 0.0},
                  {{4.0, 2.0}, {0.0, 2.0}, 0.0},
                  {{0.0, 2.0}, {0.0, 0.0}, 0.0}}},
        QStringLiteral("measurement"));
    require(!boundary_id.isEmpty() && window.selectEntity(boundary_id),
            "transform fixture should create and select an identified boundary");
    const auto original = window.document().snapshot().entities().at(boundary_id.toStdString());
    const auto before_rotation = window.document().revision();
    const auto rotated_ok = window.transformSelectedBoundary(QStringLiteral("90"), false, false,
                                                              QStringLiteral("0"), QStringLiteral("0"), false);
    if (!rotated_ok) std::cerr << "transform error: " << window.lastError().toStdString() << '\n';
    require(rotated_ok && window.document().revision() == before_rotation + 1,
            "boundary rotation should commit one undoable document command");
    const auto rotated = window.document().snapshot().entities().at(boundary_id.toStdString());
    const auto rotated_model = decode_identified_boundary_entity(rotated);
    require(rotated_model.segments.front().segment.start.x == 3.0 &&
                rotated_model.segments.front().segment.start.y == -1.0,
            "boundary rotation should use the bounding-box center as its pivot");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(boundary_id.toStdString()) == original &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(boundary_id.toStdString()) == rotated,
            "boundary transform should restore exact geometry through undo and redo");
    const auto source_count = window.document().snapshot().entities().size();
    require(window.selectEntity(boundary_id) &&
                window.transformSelectedBoundary(QStringLiteral("0"), false, true,
                                                 QStringLiteral("4 ft"), QStringLiteral("0"), true),
            "boundary clone should accept an explicit offset and flip");
    const auto clone_id = window.selectedEntityId();
    require(!clone_id.isEmpty() && clone_id != boundary_id &&
                window.document().snapshot().entities().size() == source_count + 1 &&
                window.document().snapshot().entities().contains(boundary_id.toStdString()),
            "boundary clone should create a distinct selected entity and preserve its source");
    const auto clone = window.document().snapshot().entities().at(clone_id.toStdString());
    require(clone.properties.at("classification") == "measurement" &&
                decode_identified_boundary_entity(clone).segments.front().segment.start.x > 1.2,
            "boundary clone should retain safe context metadata while applying its transform");

    auto* action = window.findChild<QAction*>(QStringLiteral("boundaryTransform"));
    require(action, "boundary transform should be available from the secondary command surface");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("boundaryTransformDialog"));
        require(dialog, "boundary transform editor must open");
        require(dialog->findChild<QLineEdit*>(QStringLiteral("boundaryRotationDegrees")) &&
                    dialog->findChild<QLineEdit*>(QStringLiteral("boundaryOffsetX")) &&
                    dialog->findChild<QCheckBox*>(QStringLiteral("boundaryClone")) &&
                    dialog->findChild<QDialogButtonBox*>(QStringLiteral("boundaryTransformButtons")),
                "boundary transform editor must expose pivot transform controls");
        dialog->reject();
    });
    action->trigger();
}

void test_organization_context() {
    sketch::desktop::MainWindow window;
    const auto second_building = window.createBuilding("property-1", "Workshop");
    require(!second_building.isEmpty(), "create a second building");
    const auto second_floor = window.createFloor(second_building, "Upper floor");
    require(!second_floor.isEmpty(), "create a floor with a default layer atomically");
    const auto layer = window.activeLayerId();
    require(!layer.isEmpty() && layer != "layer-1", "new floor activates its own drawing layer");
    auto* drawing_context = window.findChild<QLabel*>(QStringLiteral("drawingContext"));
    require(drawing_context && drawing_context->wordWrap() &&
                drawing_context->text().contains("Workshop") && drawing_context->text().contains("Upper floor"),
            "the full active drawing context must remain readable beside a narrow layer selector");
    const auto wall = window.createStraightWall({1.0, 2.0}, {6.0, 2.0});
    require(!wall.isEmpty(), "draw on the selected floor");
    const auto snapshot = window.document().snapshot();
    require(snapshot.entities().at(wall.toStdString()).properties.at("floor_id") == second_floor.toStdString() &&
                snapshot.entities().at(wall.toStdString()).properties.at("layer_id") == layer.toStdString(),
            "new walls must use the active drawing context");
    auto* item = navigator_item(window, wall);
    require(item && item->parent() && item->parent()->data(0, Qt::UserRole).toString() == layer,
            "navigator shows the wall under its actual layer");
    require(window.renameOrganizationEntity(second_floor, "Studio"), "rename the actual floor");
    require(navigator_item(window, second_floor)->text(0) == "Studio", "navigator reflects semantic names");
    require(window.document().snapshot().entities().at(wall.toStdString()) == snapshot.entities().at(wall.toStdString()),
            "organization rename must not transform or rewrite object geometry");
    require(window.undoCommand() && window.redoCommand(), "organization rename participates in history");
    require(!window.setActiveLayer("missing-layer"), "missing layer cannot become active");
    require(window.setActiveLayer("layer-1"), "switch to original building's layer");
    const auto original_floor_wall = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
    require(window.document().snapshot().entities().at(original_floor_wall.toStdString()).properties.at("floor_id") == "floor-1",
            "context selection changes subsequent authoring only");
    const auto boundary = window.createBoundary({{{0.0, 0.0}, {4.0, 0.0}, 0.0},
        {{4.0, 0.0}, {4.0, 3.0}, 0.0}, {{4.0, 3.0}, {0.0, 3.0}, 0.0}, {{0.0, 3.0}, {0.0, 0.0}, 0.0}});
    require(!boundary.isEmpty() && window.setActiveLayer(layer), "retain selected source while changing drawing context");
    const auto mismatch_revision = window.document().revision();
    require(window.createSlabFromSelectedBoundary("150 mm", "0 m").isEmpty() &&
                window.document().revision() == mismatch_revision,
            "cross-context slab from selected boundary must fail without mutation");
    const auto preview_revision = window.document().revision();
    require(window.renameOrganizationEntity(second_building, "Workshop annex"), "intervening organization edit");
    const auto intervening_revision = window.document().revision();
    require(window.createStraightWall({0.0, 0.0}, {2.0, 0.0}, "interior", preview_revision).isEmpty() &&
                !window.renameOrganizationEntity(second_floor, "Stale rename", preview_revision) &&
                window.document().revision() == intervening_revision,
            "authoring and rename reject a preview revision invalidated by another command");
    const auto disposable_layer = window.createLayer(second_floor, "Temporary");
    require(!disposable_layer.isEmpty(), "create an independent layer");
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::erase(disposable_layer.toStdString())},
        .message = "remove empty active layer",
    });
    const auto revision = window.document().revision();
    require(window.createStraightWall({0.0, 0.0}, {1.0, 0.0}).isEmpty() &&
                window.document().revision() == revision,
            "deleted active context blocks drawing instead of choosing another floor");
    require(window.undoCommand() && window.setActiveLayer(disposable_layer), "restored layer can be explicitly reactivated");
    QTemporaryDir directory;
    require(directory.isValid(), "organization fixture needs a temporary directory");
    const auto path = directory.filePath("organization.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "multiple buildings and floors save and reopen");
    require(window.activeLayerId().isEmpty(), "multi-layer reopen requires an explicit drawing context");
    require(navigator_item(window, wall) && navigator_item(window, second_floor)->text(0) == "Studio",
            "reopened hierarchy preserves objects and names");
    require(window.selectEntity(wall) && window.activeLayerId() == layer,
            "selecting a reopened object resolves its real drawing context");
}

void test_six_form_authoring_and_quantity_history() {
    using namespace sketch;
    desktop::MainWindow window;
    const std::vector<BuildingObject> objects{
        RectangularColumn{"workflow-column", {1.0, 2.0, 0.0}, 0.4, 0.6, 3.0, 0.2},
        CircularColumn{"workflow-round", {3.0, 2.0, 0.0}, 0.25, 3.0},
        Beam{"workflow-beam", {1.0, 2.0, 3.0}, {4.0, 2.0, 3.0}, {0.0, 0.0, 1.0}, 0.2, 0.3},
        StairFlight{"workflow-stair", {5.0, 0.0, 0.0}, 0.0, 4, 0.8, 0.25, 1.0, StairLanding{0.6, 0.15}},
        SlopedRoofPanel{"workflow-shed", {0.0, 0.0, 4.0}, 0.0, 4.0, 3.0, 1.0, std::atan(0.25), 0.2, 0.1},
        GableRoof{"workflow-gable", {8.0, 0.0, 4.0}, 0.2, 5.0, 4.0, 1.0, std::atan(0.5), 0.2, 0.1},
    };
    std::vector<QString> identities;
    for (const auto& object : objects) {
        auto entity = encode_building_entity(object, {{"fixture_metadata", "retained"}});
        const auto id = window.commitBuildingObject(entity, window.document().revision());
        require(!id.isEmpty() && navigator_item(window, id), "each building form is authorable and navigable");
        identities.push_back(id);
        const auto before = window.document().snapshot().entities().at(id.toStdString());
        auto edit = before;
        const auto key = before.type == "column" ? "height_m" : before.type == "beam" ? "width_m" :
                         before.type == "stair" ? "width_m" : "thickness_m";
        edit.properties[key] = edit.properties.at(key).get<double>() * 1.1;
        require(!window.commitBuildingObject(edit, window.document().revision(), true).isEmpty(),
                "each building form supports an atomic dimension edit");
        require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before,
                "each form's undo restores exact geometry and metadata");
        require(window.redoCommand() && window.selectEntity(id), "each form's edit can be redone and selected");
    }
    desktop::BuildingObjectDialog dialog(std::nullopt, false);
    auto* width = dialog.findChild<QLineEdit*>(QStringLiteral("buildingObjectWidth"));
    require(width != nullptr, "exact-entry fixture needs the column width field");
    width->setText(QStringLiteral("1/3 ft"));
    require(dialog.submit() && dialog.candidate(), "fractional building input submits");
    const auto exact_id = window.commitBuildingObject(*dialog.candidate(), window.document().revision());
    require(!exact_id.isEmpty(), "exact entry commits through the same document command");
    auto original = window.document().snapshot().entities().at(exact_id.toStdString());
    original.properties["future_dimension"] = 42;
    original.properties["quantity_entries"]["/future_dimension"] = {{"version", 99}, {"opaque", "retain"}};
    window.document().apply(ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {EntityChange::upsert(original)},
        .message = "optional future quantity metadata fixture",
    });
    const auto receipt = original.properties.at("quantity_entries").at("/width_m");
    require(receipt.at("original_expression") == "1/3 ft" && receipt.at("exact_metres").at("numerator") == 127 &&
                receipt.at("exact_metres").at("denominator") == 1250, "exact rational and expression reach the document");
    desktop::BuildingObjectDialog edit_dialog(original, true);
    edit_dialog.findChild<QLineEdit*>(QStringLiteral("buildingObjectHeight"))->setText("3500 mm");
    require(edit_dialog.submit() && edit_dialog.candidate(), "edit another dimension through the dialog");
    require(!window.commitBuildingObject(*edit_dialog.candidate(), window.document().revision(), true).isEmpty(),
            "edited quantity records merge through the command boundary");
    require(window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").at("/width_m") == receipt,
            "editing height retains the original width expression");
    require(window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").at("/future_dimension") ==
                original.properties.at("quantity_entries").at("/future_dimension"),
            "unrelated edits preserve unknown optional quantity metadata");
    require(window.undoCommand() && window.redoCommand(), "exact entries participate in undo and redo");
    require(window.selectEntity(exact_id), "reselect exact object");
    auto programmatic = window.document().snapshot().entities().at(exact_id.toStdString());
    programmatic.properties["width_m"] = 0.2;
    require(!window.commitBuildingObject(programmatic, window.document().revision(), true).isEmpty(),
            "programmatic dimension edit remains supported");
    require(!window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").contains("/width_m"),
            "programmatic change must invalidate the stale width receipt");
    require(window.undoCommand() && window.selectEntity(exact_id), "undo restores the exact entry");
    auto forged = window.document().snapshot().entities().at(exact_id.toStdString());
    forged.properties["quantity_entries"]["/width_m"]["original_expression"] = "1/2 ft";
    const auto revision = window.document().revision();
    require(window.commitBuildingObject(forged, revision, true).isEmpty() && window.document().revision() == revision,
            "inconsistent quantity provenance rejects the entire edit");
    forged = window.document().snapshot().entities().at(exact_id.toStdString());
    forged.properties["quantity_entries"]["/version"] = {
        {"version", 1}, {"original_expression", "1 m"}, {"entered_unit", "m"},
        {"exact_metres", {{"numerator", 1}, {"denominator", 1}}}};
    require(window.commitBuildingObject(forged, revision, true).isEmpty() && window.document().revision() == revision,
            "quantity provenance cannot describe a dimensionless schema version as metres");
    forged = window.document().snapshot().entities().at(exact_id.toStdString());
    forged.properties["base_center_m"][0] = -1.0;
    forged.properties["quantity_entries"]["/base_center_m/0"] = {
        {"version", 1}, {"original_expression", "-1 m"}, {"entered_unit", "m"},
        {"exact_metres", {{"numerator", std::numeric_limits<std::uint64_t>::max()}, {"denominator", 1}}}};
    require(window.commitBuildingObject(forged, revision, true).isEmpty() && window.document().revision() == revision,
            "oversized unsigned quantity numerators cannot wrap into a valid signed measurement");
    QTemporaryDir directory;
    require(directory.isValid(), "six-form fixture needs a temporary directory");
    const auto path = directory.filePath("six-forms.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "all forms save and reopen");
    auto* plan = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(plan != nullptr, "shared plan/PDF scene is available");
    for (const auto& id : identities) {
        require(navigator_item(window, id) && window.selectEntity(id), "every reopened form remains selectable");
        require(std::any_of(plan->entities().begin(), plan->entities().end(), [&](const auto& entry) {
            return entry.id == id && !entry.segments.empty();
        }), "each reopened form appears in the shared vector output scene");
    }
    require(window.selectEntity({}), "clear selection before keyboard-style navigator selection");
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    tree->setCurrentItem(navigator_item(window, identities.front()));
    QApplication::processEvents();
    require(window.selectedEntityId() == identities.front(),
            "navigator current-item selection must update the workspace without a mouse click");
    tree->setCurrentItem(navigator_item(window, identities.back()));
    require(window.openProject(path), "open while a navigator selection callback is queued");
    QApplication::processEvents();
    require(window.selectedEntityId().isEmpty(), "queued selection from an old document must not affect a reopened document");
    require(window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").at("/width_m") == receipt,
            "fractional input survives save/reopen without losing provenance");
    require(window.exportDraftPdf(directory.filePath("six-forms.pdf")), "the six-form scene exports through the shared PDF renderer");
    const auto svg = directory.filePath("six-forms.svg");
    require(window.exportDraftSvg(svg), "the six-form scene exports through the shared SVG renderer");
    QFile svg_file(svg);
    require(svg_file.open(QIODevice::ReadOnly | QIODevice::Text), "draft SVG should be readable");
    const auto svg_text = QString::fromUtf8(svg_file.readAll());
    require(svg_text.contains("<svg") && svg_text.contains("DRAFT"),
            "draft SVG should contain vector markup and its draft stamp");
}

void test_contextual_roof_dimension_inspector() {
    using namespace sketch;
    desktop::MainWindow window;
    const SlopedRoofPanel flat_roof{
        "context-roof", {0.0, 0.0, 4.0}, 0.0, 4.0, 3.0, 0.0, 0.0, 0.2, 0.1};
    const auto source = encode_building_entity(flat_roof, {{"fixture_metadata", "retained"}});
    const auto id = window.commitBuildingObject(source, window.document().revision());
    require(!id.isEmpty() && window.selectEntity(id), "contextual roof fixture should be selectable");
    QApplication::processEvents();

    auto* group = window.findChild<QGroupBox*>(QStringLiteral("roofProperties"));
    auto* run = window.findChild<QLineEdit*>(QStringLiteral("roofRun"));
    auto* rise = window.findChild<QLineEdit*>(QStringLiteral("roofRise"));
    auto* thickness = window.findChild<QLineEdit*>(QStringLiteral("roofThickness"));
    auto* pitch = window.findChild<QLabel*>(QStringLiteral("roofPitch"));
    auto* apply = window.findChild<QPushButton*>(QStringLiteral("applyRoofProperties"));
    require(group && run && rise && thickness && pitch && apply && !group->isHidden() &&
                group->isEnabled() && !run->text().isEmpty() && !thickness->text().isEmpty() &&
                pitch->text() == QStringLiteral("0.000°"),
            "selecting a flat roof should expose compact contextual dimensions and zero pitch");

    const auto before_revision = window.document().revision();
    const auto before = window.document().snapshot().entities().at(id.toStdString());
    rise->setText(QStringLiteral("1 m"));
    apply->click();
    QApplication::processEvents();
    require(window.document().revision() == before_revision + 1,
            "applying contextual roof dimensions should create one history entry");
    const auto edited = window.document().snapshot().entities().at(id.toStdString());
    require(edited.extensions == before.extensions && edited.properties.at("run_m") == before.properties.at("run_m") &&
                edited.properties.at("thickness_m") == before.properties.at("thickness_m") &&
                std::abs(edited.properties.at("rise_m").get<double>() - 1.0) < 1e-9 &&
                std::abs(edited.properties.at("pitch_rad").get<double>() - std::atan(0.25)) < 1e-12,
            "contextual roof editing should preserve untouched dimensions and derive pitch");
    auto* plan = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(plan && std::any_of(plan->entities().begin(), plan->entities().end(), [&](const auto& entry) {
                return entry.id == id && !entry.segments.empty();
            }),
            "contextual roof editing should refresh the shared plan projection");

    require(window.undoCommand() &&
                window.document().snapshot().entities().at(id.toStdString()) == before &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(id.toStdString()).properties.at("rise_m") == 1.0,
            "contextual roof dimensions should support exact undo and redo");
    const auto invalid_revision = window.document().revision();
    rise->setText(QStringLiteral("-1 m"));
    apply->click();
    require(window.document().revision() == invalid_revision &&
                window.lastError().contains(QStringLiteral("zero or greater")),
            "negative contextual roof rise should be rejected without history mutation");

    const auto wall_id = window.createStraightWall({0.0, 0.0}, {2.0, 0.0});
    require(!wall_id.isEmpty() && window.selectEntity(wall_id) && group->isHidden(),
            "contextual roof dimensions should hide for non-roof selections");
    require(window.selectEntity(id) && !group->isHidden(),
            "contextual roof dimensions should return when the roof is reselected");
    rise->setText(QStringLiteral("2 m"));
    auto intervening = window.document().snapshot().entities().at(id.toStdString());
    intervening.properties["span_m"] = 5.0;
    window.document().apply(ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {EntityChange::upsert(intervening)},
        .message = "intervening roof edit before inspector apply",
    });
    const auto stale_revision = window.document().revision();
    apply->click();
    require(window.document().revision() == stale_revision &&
                window.document().snapshot().entities().at(id.toStdString()) == intervening,
            "roof inspector must reject fields populated before an intervening document edit");
    require(window.selectEntity(id), "refresh roof inspector after stale edit rejection");
    rise->setText(QStringLiteral("0 m"));
    apply->click();
    const auto flattened = window.document().snapshot().entities().at(id.toStdString());
    require(flattened.properties.at("rise_m") == 0.0 && flattened.properties.at("pitch_rad") == 0.0,
            "inspector must convert a sloped panel back into a flat roof");
    for (const auto& invalid : std::vector<std::pair<QLineEdit*, QString>>{
             {run, QStringLiteral("0 m")}, {thickness, QStringLiteral("0 m")},
             {rise, QStringLiteral("not a measurement")}}) {
        require(window.selectEntity(id), "refresh fields for an independent invalid-input case");
        const auto revision = window.document().revision();
        invalid.first->setText(invalid.second);
        apply->click();
        require(window.document().revision() == revision && !window.lastError().isEmpty() &&
                    window.document().snapshot().entities().at(id.toStdString()) == flattened,
                "invalid roof measurements must leave geometry and history unchanged");
    }
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("roof.bldproj")) &&
                window.openProject(directory.filePath("roof.bldproj")) &&
                window.document().snapshot().entities().at(id.toStdString()) == flattened,
            "contextual roof edits and metadata must survive save and reopen exactly");
}

void test_contextual_gable_roof_inspector(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    auto source = encode_building_entity(GableRoof{
        "context-gable", {1, 2, 4}, 0.2, 6, 4, 1, std::atan(0.5), 0.2, 0.1},
        {{"future_roof_metadata", "retain"}});
    desktop::BuildingObjectDialog exact(source, true);
    exact.findChild<QLineEdit*>("buildingObjectLength")->setText("31/3 ft");
    require(exact.submit() && exact.candidate(), "gable exact-length fixture");
    const auto id = window.commitBuildingObject(*exact.candidate(), window.document().revision());
    require(!id.isEmpty() && window.selectEntity(id), "select contextual gable fixture");
    auto* group = window.findChild<QGroupBox*>("roofProperties");
    auto* span = window.findChild<QLineEdit*>("roofSpan");
    auto* rise = window.findChild<QLineEdit*>("roofRise");
    auto* overhang = window.findChild<QLineEdit*>("roofOverhang");
    auto* apply = window.findChild<QPushButton*>("applyRoofProperties");
    require(group && !group->isHidden() && span && rise && overhang && apply,
            "gable selection must expose dimensions directly in the inspector");
    if (!capture_directory.isEmpty()) {
        window.resize(1366, 768);
        require(window.grab().save(capture_directory + QStringLiteral("/gable-inspector.png")),
                "capture contextual gable inspector for visual review");
    }
    const auto before = window.document().snapshot().entities().at(id.toStdString());
    const auto revision = window.document().revision();
    span->setText("6 m");
    rise->setText("2 m");
    overhang->setText("300 mm");
    auto* pitch_preview = window.findChild<QLabel*>("roofPitch");
    auto* preview_error = window.findChild<QLabel*>("roofPreviewError");
    require(pitch_preview && preview_error && preview_error->isHidden() &&
                pitch_preview->text() == QStringLiteral("33.690°") &&
                window.document().revision() == revision &&
                window.document().snapshot().entities().at(id.toStdString()) == before,
            "gable pitch must preview pending measurements without changing the document");
    span->setText("0 m");
    require(!preview_error->isHidden() && preview_error->text().contains("Span") &&
                pitch_preview->text() == QStringLiteral("—") && window.document().revision() == revision,
            "invalid roof input must explain the field error without displaying a stale pitch");
    if (!capture_directory.isEmpty()) {
        require(window.grab().save(capture_directory + QStringLiteral("/gable-invalid-preview.png")),
                "capture inline roof validation for visual review");
    }
    span->setText("6 m");
    require(preview_error->isHidden() && pitch_preview->text() == QStringLiteral("33.690°"),
            "correcting input must restore the pitch preview and clear its error");
    apply->click();
    const auto edited = window.document().snapshot().entities().at(id.toStdString());
    require(window.document().revision() == revision + 1 && edited.properties.at("span_m") == 6.0 &&
                edited.properties.at("rise_m") == 2.0 && edited.properties.at("overhang_m") == 0.3 &&
                std::abs(edited.properties.at("pitch_rad").get<double>() - std::atan(2.0 / 3.0)) < 1e-12,
            "gable pitch must derive from half-span in one compound dimension edit");
    require(edited.extensions == before.extensions &&
                edited.properties.at("length_m") == before.properties.at("length_m") &&
                edited.properties.at("quantity_entries").at("/length_m") ==
                    before.properties.at("quantity_entries").at("/length_m"),
            "gable inspector must retain untouched exact length and opaque metadata");
    require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before &&
                window.redoCommand() && window.document().snapshot().entities().at(id.toStdString()) == edited,
            "gable inspector dimensions must undo and redo exactly");
    rise->setText("0 m");
    const auto invalid_revision = window.document().revision();
    apply->click();
    require(window.document().revision() == invalid_revision && !window.lastError().isEmpty(),
            "gable roof must reject zero rise instead of creating an invalid ridge");
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("gable.bldproj")) &&
                window.openProject(directory.filePath("gable.bldproj")) &&
                window.document().snapshot().entities().at(id.toStdString()) == edited,
            "gable inspector geometry and quantity provenance must survive save/reopen");
}

void test_survey_calculator(const QString& capture_directory) {
    sketch::desktop::MainWindow window;
    QString survey_id;
    QTemporaryDir directory;
    require(directory.isValid(), "survey export fixture directory");
    const auto revision = window.document().revision();
    auto* action = window.findChild<QAction*>("surveyTraverse");
    require(action, "survey command must be available in the desktop workspace");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("surveyCalculator");
        require(dialog, "survey calculator must open");
        auto* source = dialog->findChild<QLineEdit*>("surveyProvenance");
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* calculate = dialog->findChild<QPushButton*>("surveyCalculate");
        auto* output = dialog->findChild<QPushButton*>("surveyExport");
        auto* result = dialog->findChild<QLabel*>("surveyResult");
        require(source && input && calculate && output && result && !output->isEnabled(),
                "survey controls and initially disabled export must exist");
        source->setText("Deed fixture");
        input->setPlainText("NE, 90:0:0, 100000 mm\nSE, 0:0:0, 100 m\nSW, 90, 100 m\nNW, 0, 100 m");
        calculate->click();
        require(output->isEnabled() && result->text().contains("10000.0000 m²") &&
                    result->text().contains("2.471054 acres"), "survey calculator must report square area and acres");
        QApplication::processEvents();
        if (!capture_directory.isEmpty())
            require(dialog->grab().save(capture_directory + "/survey-calculator.png"), "survey capture");
        const auto native_dialogs_disabled = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto path = directory.filePath("survey.json");
        QTimer::singleShot(0, dialog, [&] {
            auto* picker = dialog->findChild<QFileDialog*>();
            require(picker, "survey report destination picker");
            picker->selectFile(path);
            QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        });
        output->click();
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_dialogs_disabled);
        QFile saved(path);
        require(saved.open(QIODevice::ReadOnly), "survey report must be written locally");
        const auto report = nlohmann::json::parse(saved.readAll().toStdString());
        require(report.at("provenance") == "Deed fixture" && report.at("legs").size() == 4 &&
                    report.at("vertices").size() == 5 && report.at("diagnostics").at("area_m2") == 10000.0,
                "exported survey must preserve source, legs, vertices, and calculated area");
        require(report.contains("input_provenance"), "survey export must retain original entry provenance");
        const auto& entered = report.at("input_provenance");
        require(entered.at("version") == 1 && entered.at("default_unit") == "ft" &&
                    entered.at("legs_text") == input->toPlainText().toStdString() &&
                    entered.at("closure_tolerance_expression") == "0.001 m" &&
                    entered.at("distances").at(0).at("original_expression") == "100000 mm" &&
                    entered.at("distances").at(0).at("exact_metres").at("numerator") == 100 &&
                    entered.at("distances").at(0).at("exact_metres").at("denominator") == 1,
                "survey export must preserve entered units, expressions, and exact rational metres");
        saved.close();
        auto tampered = report;
        tampered["diagnostics"]["area_m2"] = 1.0;
        tampered["input_provenance"]["default_unit"] = "m";
        tampered["input_provenance"]["legs_text"] = "NE, 90, 100\nSE, 0, 100\nSW, 90, 100\nNW, 0, 100";
        require(saved.open(QIODevice::WriteOnly | QIODevice::Truncate), "prepare report reopen fixture");
        saved.write(QByteArray::fromStdString(tampered.dump()));
        saved.close();
        auto* open_report = dialog->findChild<QPushButton*>("surveyOpen");
        auto* input_units = dialog->findChild<QComboBox*>("surveyInputUnits");
        require(open_report && input_units, "survey reopen controls");
        const auto open_fixture = [&] {
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            QTimer::singleShot(0, dialog, [&] {
                auto* picker = dialog->findChild<QFileDialog*>();
                require(picker, "survey report open picker");
                picker->selectFile(path);
                QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
            });
            open_report->click();
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_dialogs_disabled);
        };
        open_fixture();
        require(input_units->currentData().toString() == "m" && output->isEnabled() &&
                    result->text().contains("10000.0000 m²"),
                "reopened survey must restore default units and recompute rather than trust stored area");
        auto* add = dialog->findChild<QPushButton*>("surveyAddBoundary");
        require(add && add->isEnabled(), "closed survey must offer project boundary creation");
        add->click();
        survey_id = window.selectedEntityId();
        require(!survey_id.isEmpty() && window.document().revision() == revision + 1,
                "adding survey geometry must be one document command");
        const auto entity = window.document().snapshot().entities().at(survey_id.toStdString());
        require(entity.properties.at("classification") == "survey" &&
                    entity.extensions.at("survey_source").at("report").at("diagnostics").at("area_m2") == 10000.0 &&
                    sketch::boundary_geometry(sketch::decode_identified_boundary_entity(entity)).size() == 4,
                "project survey boundary must retain its measured geometry and report provenance");
        require(window.selectEntity({}), "change selection while survey calculator remains open");
        add->click();
        require(window.document().revision() == revision + 1 && result->text().contains("context changed"),
                "survey insertion must reject a stale drawing context without duplicate geometry");
        const auto restored_input = input->toPlainText();
        tampered["input_provenance"]["version"] = 99;
        require(saved.open(QIODevice::WriteOnly | QIODevice::Truncate), "prepare unsupported report fixture");
        saved.write(QByteArray::fromStdString(tampered.dump()));
        saved.close();
        open_fixture();
        require(input->toPlainText() == restored_input && result->text().contains("Unsupported"),
                "unsupported report version must not replace current entries");
        input->setPlainText("NE, 45:30:15.5, 100 ft");
        require(!output->isEnabled() && result->text().isEmpty(), "edits must invalidate stale survey results");
        calculate->click();
        require(output->isEnabled() && result->text().contains("Open traverse") &&
                    result->text().contains("Area unavailable") && !add->isEnabled(),
                "open traverses must not display acreage or allow area creation");
        input->setPlainText("NE, 91, 100 ft");
        calculate->click();
        require(!output->isEnabled() && result->text().contains("Line 1"), "invalid survey leg must identify its line");
        dialog->reject();
    });
    action->trigger();
    require(window.document().revision() == revision + 1, "only explicit boundary creation may alter the project");
    const auto added = window.document().snapshot().entities().at(survey_id.toStdString());
    require(window.undoCommand() && !window.document().snapshot().entities().contains(survey_id.toStdString()) &&
                window.redoCommand() && window.document().snapshot().entities().at(survey_id.toStdString()) == added,
            "survey geometry and provenance must undo and redo together");
    const auto project_path = directory.filePath("survey.bldproj");
    require(window.saveProjectAs(project_path) && window.openProject(project_path) &&
                window.document().snapshot().entities().at(survey_id.toStdString()) == added,
            "survey boundary source and geometry must survive project save/reopen");
    require(window.exportDraftSvg(directory.filePath("survey.svg")), "survey boundary must export through shared vector output");
    auto* units = window.findChild<QComboBox*>("unitSystem");
    units->setCurrentIndex(units->findText("Metric"));
    const auto floor_area = window.createBoundary({{{1, -99}, {11, -99}, 0},
        {{11, -99}, {11, -89}, 0}, {{11, -89}, {1, -89}, 0}, {{1, -89}, {1, -99}, 0}}, "living");
    require(!floor_area.isEmpty() && window.selectEntity(survey_id), "select parcel surrounding a living area");
    auto* building_total = window.findChild<QLabel*>("calculationBuildingTotal");
    auto* living_total = window.findChild<QLabel*>("calculationLivingTotal");
    require(building_total && living_total && building_total->text().contains("100.00 m²") &&
                living_total->text().contains("100.00 m²"),
            "parcel enclosing a building must not block calculations or inflate building/living totals");
    const auto source_revision = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("surveyCalculator");
        require(dialog && dialog->windowTitle().contains("original source"), "selected survey must reopen its original calls");
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* source = dialog->findChild<QLineEdit*>("surveyProvenance");
        auto* result = dialog->findChild<QLabel*>("surveyResult");
        require(input && input->toPlainText().startsWith("NE, 90, 100") && source->text() == "Deed fixture" &&
                    result->text().contains("10000.0000 m²"),
                "project-backed survey source must restore calls and recompute acreage after save/reopen");
        dialog->reject();
    });
    action->trigger();
    require(window.document().revision() == source_revision,
            "inspecting original survey calls must not change the project or insert another boundary");
}

void test_design_phase_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
    require(!wall_id.isEmpty(), "phase fixture wall should be created");
    require(window.activeRemodelingAlternative().isEmpty(),
            "a new project starts on the shared existing baseline");
    auto* phase_combo = window.findChild<QComboBox*>(QStringLiteral("modelPhase"));
    require(phase_combo && phase_combo->currentText().contains(QStringLiteral("Set up")),
            "the navigator should expose the design phase setup action");

    const auto phases = ModelPhases::create(
        {wall_id.toStdString()}, {wall_id.toStdString()},
        {{"remove-wall", "Remove wall", {wall_id.toStdString()}, {}}});
    auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
    phase_entity.id = "phases-desktop";
    auto material_wall = window.document().snapshot().entities().at(wall_id.toStdString());
    material_wall.properties["material_name"] = "Timber";
    material_wall.properties["volume_m3"] = 2.0;
    window.document().apply(ApplyEntityChanges{
        window.document().revision(),
        {EntityChange::upsert(phase_entity), EntityChange::upsert(material_wall)}, {},
        "add design phase fixture"});
    require(window.selectEntity(wall_id), "phase fixture should refresh after adding its record");
    require(phase_combo->count() == 2 &&
                phase_combo->itemText(0) == QStringLiteral("Existing baseline") &&
                phase_combo->itemData(1).toString() == QStringLiteral("remove-wall"),
            "the navigator should list baseline and imported alternatives");
    require(window.entityVisible(wall_id), "baseline geometry should be visible before selection");
    const auto has_wall_material = [&] {
        const auto projection = window.scheduleSnapshot();
        return std::any_of(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& row) { return row.object_id == wall_id.toStdString() + ":material"; });
    };
    require(has_wall_material(), "baseline schedule must include the visible wall material");

    const auto baseline_revision = window.document().revision();
    require(window.selectRemodelingAlternative(QStringLiteral("remove-wall")),
            "selecting an alternative should use the typed document command");
    require(window.activeRemodelingAlternative() == QStringLiteral("remove-wall") &&
                !window.entityVisible(wall_id),
            "a demolished baseline object should be hidden in its active alternative");
    require(!has_wall_material(), "demolished wall material must be absent from the active schedule");
    auto* plan = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    bool wall_visible = false;
    if (plan != nullptr) {
        for (const auto& entity : plan->entities()) {
            if (entity.id == wall_id) wall_visible = true;
        }
    }
    require(plan && !wall_visible,
            "phase filtering should reach the shared plan canvas");
    require(window.undoCommand() && window.document().revision() > baseline_revision &&
                window.activeRemodelingAlternative().isEmpty() && window.entityVisible(wall_id),
            "phase selection should be undoable and restore the baseline view");
    require(has_wall_material(), "undoing phase selection must restore baseline material quantities");
    require(window.redoCommand() &&
                window.activeRemodelingAlternative() == QStringLiteral("remove-wall"),
            "phase selection should be redoable");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("remodelingAlternativesDialog"));
        require(dialog, "design phase manager should open from the shared workflow");
        auto* selection = dialog->findChild<QComboBox*>(QStringLiteral("remodelingPhaseSelection"));
        auto* demolition = dialog->findChild<QListWidget*>(QStringLiteral("remodelingDemolitionList"));
        require(selection && demolition && selection->count() == 2 && demolition->count() == 1,
                "design phase manager should expose typed phase and demolition controls");
        dialog->reject();
    });
    window.showRemodelingAlternatives();

    QTemporaryDir directory;
    require(directory.isValid(), "phase fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("phases.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "active design phase should save and reopen through the project store");
    require(window.activeRemodelingAlternative() == QStringLiteral("remove-wall"),
            "active design phase should persist across reopen");
}

void test_phase_authoring_ownership() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto legacy = window.createStraightWall({0, 0}, {4, 0});
    auto registry = Entity::create("model_phases", {{"model", ModelPhases::create(
        {}, {}, {{"a", "A", {}, {}}, {"b", "B", {}, {}}}).to_json()}});
    registry.id = "authored-phases";
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(registry)}, {}, "phase authoring fixture"});
    require(window.selectEntity(legacy), "refresh phase authoring fixture");
    const auto baseline = window.createStraightWall({0, 1}, {4, 1});
    require(!baseline.isEmpty(), "author baseline wall after phase setup");
    require(window.selectRemodelingAlternative("a"), "select proposal owner A");
    const auto proposed_wall = window.createStraightWall({0, 2}, {4, 2});
    require(!proposed_wall.isEmpty(), "author wall in alternative A");
    const auto opening = window.createHostedOpening("door", "1 m", "1 m", "0 m", "2 m");
    require(!opening.isEmpty(), "author hosted opening in alternative A");
    const Boundary boundary{{{{0, 3}, {4, 3}, 0}, {{4, 3}, {4, 6}, 0},
                             {{4, 6}, {0, 6}, 0}, {{0, 6}, {0, 3}, 0}}};
    const auto room = window.createRoomBoundary(boundary, "A room");
    const auto measurement = window.createBoundary(boundary, "A measurement");
    require(!room.isEmpty() && !measurement.isEmpty(), "author both boundary roles in alternative A");
    auto column = encode_building_entity(BuildingObject{
        RectangularColumn{"phase-column", {7, 2, 0}, 0.4, 0.6, 3, 0}});
    column.properties["material_name"] = "Phase material";
    column.properties["volume_m3"] = 0.72;
    const auto before_column = window.document().snapshot().entities();
    const auto column_id = window.commitBuildingObject(column, window.document().revision());
    require(!column_id.isEmpty(), "author building object in alternative A");
    const auto after_column = window.document().snapshot().entities();
    require(window.undoCommand() && window.document().snapshot().entities() == before_column,
            "one undo restores both geometry and phase registry");
    require(window.redoCommand() && window.document().snapshot().entities() == after_column,
            "one redo restores geometry identities and phase registry");
    const auto model = ModelPhases::from_json(after_column.at(registry.id).properties.at("model"));
    require(model.baseline_ids() == std::vector<std::string>{baseline.toStdString()} &&
                model.entity_ids().size() == 6 && !model.active_state().contains(legacy.toStdString()),
            "new objects receive ownership without reclassifying unregistered legacy geometry");
    QStringList proposals{proposed_wall, opening, room, measurement, column_id};
    require(window.selectEntity(proposed_wall) && window.copySelection() && window.pasteSelection(),
            "paste a wall and its hosted opening into the active alternative");
    const auto pasted = window.document().snapshot().entities();
    const auto pasted_model = ModelPhases::from_json(pasted.at(registry.id).properties.at("model"));
    for (const auto& [id, entity] : pasted) {
        if (after_column.contains(id)) continue;
        require(pasted_model.active_state().at(id) == ModelPhase::proposed,
                "compound paste registers every new geometry identity");
        proposals.push_back(QString::fromStdString(id));
    }
    require(proposals.size() == 7 && window.undoCommand() &&
                window.document().snapshot().entities() == after_column &&
                window.redoCommand() && window.document().snapshot().entities() == pasted,
            "compound paste owns wall and opening in one undoable command");
    require(window.selectRemodelingAlternative(QStringLiteral("a")) &&
                window.selectEntity(column_id),
            "select a phase-owned object before deletion");
    const auto before_delete = window.document().snapshot().entities();
    const auto delete_ok = window.deleteSelection();
    require(delete_ok,
            "deleting a phase-owned object must update the registry atomically");
    const auto after_delete = window.document().snapshot().entities();
    const auto deleted_model = ModelPhases::from_json(
        after_delete.at(registry.id).properties.at("model"));
    require(!after_delete.contains(column_id.toStdString()) &&
                std::find(deleted_model.entity_ids().begin(), deleted_model.entity_ids().end(),
                          column_id.toStdString()) == deleted_model.entity_ids().end() &&
                window.undoCommand() && window.document().snapshot().entities() == before_delete &&
                window.redoCommand() && window.document().snapshot().entities() == after_delete,
            "phase-owned deletion must remain one exact undoable command");
    proposals.removeAll(column_id);
    const auto has_material = [&] {
        const auto schedule = window.scheduleSnapshot();
        return std::any_of(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
            [&](const auto& row) { return row.object_id == column_id.toStdString() + ":material"; });
    };
    require(!has_material(), "deleting a phase-owned object removes its material schedule row");
    for (const auto& selection : QStringList{QString(), "b", "a"}) {
        require(window.selectRemodelingAlternative(selection), "switch ownership view");
        const bool active = selection == "a";
        require(window.entityVisible(baseline) && window.entityVisible(legacy),
                "baseline and legacy remain visible across alternatives");
        for (const auto& id : proposals) {
            require(window.entityVisible(id) == active, "proposal visibility follows owning alternative");
        }
    }
    QTemporaryDir directory;
    const auto path = directory.filePath("phase-authoring.bldproj");
    const auto saved = window.document().snapshot().entities();
    require(window.saveProjectAs(path) && window.openProject(path) &&
                window.document().snapshot().entities() == saved &&
                window.activeRemodelingAlternative() == "a" && !has_material(),
            "save and reopen preserve authored geometry, phase ownership and schedule");
}

void test_room_relationship_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
    const auto boundary_id = window.createBoundary({
        {{{0.0, 0.0}, {4.0, 0.0}, 0.0},
         {{4.0, 0.0}, {4.0, 3.0}, 0.0},
         {{4.0, 3.0}, {0.0, 3.0}, 0.0},
         {{0.0, 3.0}, {0.0, 0.0}, 0.0}}});
    const auto room_id = window.createRoomBoundary({
        {{{0.25, 0.25}, {3.75, 0.25}, 0.0},
         {{3.75, 0.25}, {3.75, 2.75}, 0.0},
         {{3.75, 2.75}, {0.25, 2.75}, 0.0},
         {{0.25, 2.75}, {0.25, 0.25}, 0.0}}}, QStringLiteral("living"));
    require(!wall_id.isEmpty() && !boundary_id.isEmpty() && !room_id.isEmpty(),
            "room relationship fixture should create wall, measurement and room boundaries");
    require(window.document().snapshot().entities().at(room_id.toStdString()).type == "room_boundary",
            "room authoring should retain a distinct room-boundary semantic type");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomRelationshipsDialog"));
        require(dialog, "room relationship editor should open from the shared workflow");
        auto* source = dialog->findChild<QComboBox*>(QStringLiteral("roomRelationshipSource"));
        auto* target = dialog->findChild<QComboBox*>(QStringLiteral("roomRelationshipTarget"));
        auto* kind = dialog->findChild<QComboBox*>(QStringLiteral("roomRelationshipKind"));
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("roomRelationshipList"));
        auto* add = dialog->findChild<QPushButton*>(QStringLiteral("addRoomRelationship"));
        auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("removeRoomRelationship"));
        auto* sync = dialog->findChild<QPushButton*>(QStringLiteral("syncRoomRelationships"));
        require(source && target && kind && list && add && remove && sync,
                "room relationship editor should expose typed controls");
        const auto source_index = source->findData(boundary_id);
        const auto target_index = target->findData(wall_id);
        require(source_index >= 0 && target_index >= 0,
                "room relationship editor should list live boundary and wall references");
        source->setCurrentIndex(source_index);
        target->setCurrentIndex(target_index);
        kind->setCurrentIndex(kind->findData(static_cast<int>(RoomRelationKind::follows)));
        add->click();
        require(list->count() == 1 && list->item(0)->text().contains(QStringLiteral("Follows")),
                "room relationship editor should add a typed relation");
        list->setCurrentRow(0);
        remove->click();
        require(list->count() == 0,
                "room relationship editor should remove the selected relation");
        sync->click();
        require(source->findData(room_id) >= 0,
                "room relationship editor should synchronize newly created room boundaries");
        dialog->reject();
    });
    window.showRoomRelationships();

    const auto relationship_entity = [&]() -> Entity {
        const auto snapshot = window.document().snapshot();
        const auto found = std::find_if(snapshot.entities().begin(), snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == "room_relationships"; });
        require(found != snapshot.entities().end(),
                "room relationship editor should persist its typed record");
        return found->second;
    };
    auto model = RoomRelationshipSnapshot::from_json(
        relationship_entity().properties.at("model"));
    require(model.references().size() == 3 && model.relations().empty(),
            "removing the relation should leave the measurement, room and wall references intact");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomRelationshipsDialog"));
        auto* source = dialog->findChild<QComboBox*>(QStringLiteral("roomRelationshipSource"));
        auto* target = dialog->findChild<QComboBox*>(QStringLiteral("roomRelationshipTarget"));
        auto* kind = dialog->findChild<QComboBox*>(QStringLiteral("roomRelationshipKind"));
        auto* add = dialog->findChild<QPushButton*>(QStringLiteral("addRoomRelationship"));
        source->setCurrentIndex(source->findData(boundary_id));
        target->setCurrentIndex(target->findData(wall_id));
        kind->setCurrentIndex(kind->findData(static_cast<int>(RoomRelationKind::derived_from)));
        add->click();
        dialog->reject();
    });
    window.showRoomRelationships();
    model = RoomRelationshipSnapshot::from_json(relationship_entity().properties.at("model"));
    require(model.relations().size() == 1 &&
                model.relations().front().kind == RoomRelationKind::derived_from,
            "room relationship editor should preserve the selected relation kind");
    require(window.undoCommand(), "room relationship edits should be undoable");
    model = RoomRelationshipSnapshot::from_json(relationship_entity().properties.at("model"));
    require(model.relations().empty(), "undo should restore the relation-free graph");
    require(window.redoCommand(), "room relationship edits should be redoable");

    QTemporaryDir directory;
    require(directory.isValid(), "room relationship fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("relationships.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "room relationship record should save and reopen");
    model = RoomRelationshipSnapshot::from_json(
        window.document().snapshot().entities().at(relationship_entity().id).properties.at("model"));
    require(model.relations().size() == 1,
            "room relationship relation should survive project reopen");
}

void test_assembly_catalog_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("assemblyCatalog"));
    require(action, "assembly catalog should be discoverable from the workspace actions");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("assemblyCatalogDialog"));
        require(dialog, "assembly catalog editor should open from the workspace action");
        auto* types = dialog->findChild<QListWidget*>(QStringLiteral("assemblyTypeList"));
        auto* instances = dialog->findChild<QListWidget*>(QStringLiteral("assemblyInstanceList"));
        auto* type_id = dialog->findChild<QLineEdit*>(QStringLiteral("assemblyTypeId"));
        auto* type_name = dialog->findChild<QLineEdit*>(QStringLiteral("assemblyTypeName"));
        auto* add_type = dialog->findChild<QPushButton*>(QStringLiteral("addAssemblyType"));
        auto* instance_type = dialog->findChild<QComboBox*>(QStringLiteral("assemblyInstanceType"));
        auto* add_instance = dialog->findChild<QPushButton*>(QStringLiteral("addAssemblyInstance"));
        auto* type_entry_kind = dialog->findChild<QComboBox*>(QStringLiteral("assemblyTypeEntryKind"));
        auto* type_entry_key = dialog->findChild<QLineEdit*>(QStringLiteral("assemblyTypeEntryKey"));
        auto* type_entry_value = dialog->findChild<QLineEdit*>(QStringLiteral("assemblyTypeEntryValue"));
        auto* type_entry_unit = dialog->findChild<QComboBox*>(QStringLiteral("assemblyTypeEntryUnit"));
        auto* save_type_entry = dialog->findChild<QPushButton*>(QStringLiteral("saveAssemblyTypeEntry"));
        auto* type_schema = dialog->findChild<QListWidget*>(QStringLiteral("assemblyTypeSchemaList"));
        auto* override_kind = dialog->findChild<QComboBox*>(QStringLiteral("assemblyInstanceOverrideKind"));
        auto* override_key = dialog->findChild<QLineEdit*>(QStringLiteral("assemblyInstanceOverrideKey"));
        auto* override_value = dialog->findChild<QLineEdit*>(QStringLiteral("assemblyInstanceOverrideValue"));
        auto* override_unit = dialog->findChild<QComboBox*>(QStringLiteral("assemblyInstanceOverrideUnit"));
        auto* save_override = dialog->findChild<QPushButton*>(QStringLiteral("saveAssemblyInstanceOverride"));
        auto* override_list = dialog->findChild<QListWidget*>(QStringLiteral("assemblyInstanceOverrideList"));
        require(types && instances && type_id && type_name && add_type && instance_type && add_instance,
                "assembly editor should expose catalog and instance controls");
        require(type_entry_kind && type_entry_key && type_entry_value && type_entry_unit && save_type_entry &&
                    type_schema && override_kind && override_key && override_value && override_unit &&
                    save_override && override_list,
                "assembly editor should expose typed schema and instance override controls");
        type_id->setText(QStringLiteral("wall-basic"));
        type_name->setText(QStringLiteral("Basic wall assembly"));
        add_type->click();
        require(types->count() == 1 && types->item(0)->text().contains(QStringLiteral("Basic wall assembly")),
                "assembly editor should add a reusable type through document history");
        require(instance_type->findData(QStringLiteral("wall-basic")) >= 0,
                "assembly editor should offer the new type for instance placement");
        instance_type->setCurrentIndex(instance_type->findData(QStringLiteral("wall-basic")));
        add_instance->click();
        require(instances->count() == 1, "assembly editor should add a typed instance");
        types->setCurrentRow(0);
        type_entry_kind->setCurrentIndex(type_entry_kind->findData(QStringLiteral("property")));
        type_entry_key->setText(QStringLiteral("finish"));
        type_entry_value->setText(QStringLiteral("paint"));
        save_type_entry->click();
        require(type_schema->count() == 1 && type_schema->item(0)->text().contains(QStringLiteral("finish")),
                "assembly editor should persist a typed property declaration");
        type_entry_kind->setCurrentIndex(type_entry_kind->findData(QStringLiteral("quantity")));
        type_entry_key->setText(QStringLiteral("waste"));
        type_entry_value->setText(QStringLiteral("1.5"));
        type_entry_unit->setCurrentIndex(type_entry_unit->findData(QStringLiteral("m2")));
        save_type_entry->click();
        require(type_schema->count() == 2 && type_schema->item(1)->text().contains(QStringLiteral("waste")),
                "assembly editor should persist a typed quantity declaration");
        instances->setCurrentRow(0);
        override_kind->setCurrentIndex(override_kind->findData(QStringLiteral("property")));
        override_key->setText(QStringLiteral("finish"));
        override_value->setText(QStringLiteral("wood"));
        save_override->click();
        require(override_list->count() == 1 && override_list->item(0)->text().contains(QStringLiteral("wood")),
                "assembly editor should persist a per-instance property override");
        override_kind->setCurrentIndex(override_kind->findData(QStringLiteral("quantity")));
        override_key->setText(QStringLiteral("waste"));
        override_value->setText(QStringLiteral("2.5"));
        override_unit->setCurrentIndex(override_unit->findData(QStringLiteral("m2")));
        save_override->click();
        require(override_list->count() == 2 && override_list->item(1)->text().contains(QStringLiteral("2.5")),
                "assembly editor should persist a dimensioned quantity override");
        dialog->reject();
    });
    action->trigger();

    const auto find_model = [&] {
        const auto snapshot = window.document().snapshot();
        const auto found = std::find_if(snapshot.entities().begin(), snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == "assembly_model"; });
        require(found != snapshot.entities().end(), "assembly editor should persist its typed model");
        return AssemblyModel::from_json(found->second.properties.at("model"));
    };
    auto model = find_model();
    require(model.types().size() == 1 && model.instances().size() == 1,
            "assembly catalog edits should retain type and instance records");
    require(model.types().front().properties.at("finish") == "paint" &&
                model.types().front().quantities.at("waste").value == 1.5 &&
                model.types().front().quantities.at("waste").unit == AssemblyQuantityUnit::square_metre &&
                model.instances().front().property_overrides.at("finish") == "wood" &&
                model.instances().front().quantity_overrides.at("waste").value == 2.5 &&
                model.instances().front().quantity_overrides.at("waste").unit == AssemblyQuantityUnit::square_metre &&
                model.resolve(model.instances().front().id).properties.at("finish") == "wood",
            "assembly catalog edits should retain typed declarations and resolve overrides");
    require(window.undoCommand() && window.undoCommand() && window.undoCommand() &&
                window.undoCommand() && window.undoCommand() && window.undoCommand(),
            "assembly catalog edits should participate in normal undo history");
    model = find_model();
    require(model.types().empty() && model.instances().empty(),
            "undo should remove the instance and type without leaving a partial model");
    require(window.redoCommand() && window.redoCommand() && window.redoCommand() &&
                window.redoCommand() && window.redoCommand() && window.redoCommand(),
            "assembly catalog edits should be redoable");
    model = find_model();
    require(model.types().size() == 1 && model.instances().size() == 1,
            "redo should restore the complete assembly catalog");

    QTemporaryDir directory;
    require(directory.isValid(), "assembly fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("assemblies.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "assembly model should save and reopen through the project store");
    model = find_model();
    require(model.instances().front().type_id == "wall-basic",
            "assembly instance type should survive project reopen");
}

void test_vertical_levels_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("verticalLevels"));
    require(action, "vertical levels should be discoverable from the workspace actions");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("verticalLevelsDialog"));
        require(dialog, "vertical levels editor should open from the workspace action");
        auto* levels = dialog->findChild<QListWidget*>(QStringLiteral("verticalLevelList"));
        auto* level_id = dialog->findChild<QLineEdit*>(QStringLiteral("verticalLevelId"));
        auto* elevation = dialog->findChild<QLineEdit*>(QStringLiteral("verticalLevelElevation"));
        auto* save_level = dialog->findChild<QPushButton*>(QStringLiteral("saveVerticalLevel"));
        auto* links = dialog->findChild<QListWidget*>(QStringLiteral("verticalLinkList"));
        auto* link_id = dialog->findChild<QLineEdit*>(QStringLiteral("verticalLinkId"));
        auto* lower = dialog->findChild<QComboBox*>(QStringLiteral("verticalLinkLower"));
        auto* upper = dialog->findChild<QComboBox*>(QStringLiteral("verticalLinkUpper"));
        auto* save_link = dialog->findChild<QPushButton*>(QStringLiteral("saveVerticalLink"));
        auto* freeze = dialog->findChild<QPushButton*>(QStringLiteral("freezeVerticalLink"));
        require(levels && level_id && elevation && save_level && links && link_id && lower && upper &&
                    save_link && freeze,
                "vertical levels editor should expose level and link controls");
        level_id->setText(QStringLiteral("ground"));
        elevation->setText(QStringLiteral("0"));
        save_level->click();
        level_id->setText(QStringLiteral("first"));
        elevation->setText(QStringLiteral("3"));
        save_level->click();
        require(levels->count() == 2, "vertical levels editor should create levels through document history");
        link_id->setText(QStringLiteral("ground-first"));
        lower->setCurrentIndex(lower->findData(QStringLiteral("ground")));
        upper->setCurrentIndex(upper->findData(QStringLiteral("first")));
        save_link->click();
        require(links->count() == 1, "vertical levels editor should create a floor-to-floor link");
        links->setCurrentRow(0);
        freeze->click();
        require(links->item(0)->text().contains(QStringLiteral("frozen")),
                "vertical levels editor should retain a frozen link state");
        dialog->reject();
    });
    action->trigger();

    const auto find_model = [&] {
        const auto snapshot = window.document().snapshot();
        const auto found = std::find_if(snapshot.entities().begin(), snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == "vertical_levels"; });
        require(found != snapshot.entities().end(), "vertical levels editor should persist its graph");
        return VerticalLevelGraph::from_json(found->second.properties.at("model"));
    };
    auto model = find_model();
    require(model.levels().size() == 2 && model.links().size() == 1 &&
                model.floor_to_floor_height("ground-first") == 3 &&
                model.links().front().state == RelationshipState::frozen,
            "vertical levels editor should preserve elevations and frozen link provenance");
    QTemporaryDir directory;
    require(directory.isValid(), "vertical levels fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("levels.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "vertical levels should save and reopen through the project store");
    model = find_model();
    require(model.levels().size() == 2 && model.links().front().state == RelationshipState::frozen,
            "vertical level graph should survive project reopen");
}

void test_calculation_deduction_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto outer_id = window.createRoomBoundary(
        Boundary{{{{0.0, 0.0}, {10.0, 0.0}, 0.0},
                  {{10.0, 0.0}, {10.0, 10.0}, 0.0},
                  {{10.0, 10.0}, {0.0, 10.0}, 0.0},
                  {{0.0, 10.0}, {0.0, 0.0}, 0.0}}});
    const auto hole_id = window.createRoomBoundary(
        Boundary{{{{2.0, 2.0}, {4.0, 2.0}, 0.0},
                  {{4.0, 2.0}, {4.0, 4.0}, 0.0},
                  {{4.0, 4.0}, {2.0, 4.0}, 0.0},
                  {{2.0, 4.0}, {2.0, 2.0}, 0.0}}});
    const auto outside_id = window.createRoomBoundary(
        Boundary{{{{20.0, 20.0}, {22.0, 20.0}, 0.0},
                  {{22.0, 20.0}, {22.0, 22.0}, 0.0},
                  {{22.0, 22.0}, {20.0, 22.0}, 0.0},
                  {{20.0, 22.0}, {20.0, 20.0}, 0.0}}});
    require(!outer_id.isEmpty() && !hole_id.isEmpty() && !outside_id.isEmpty() &&
                window.selectEntity(outer_id),
            "deduction fixture should create and select the base boundary");
    window.setMetricUnits(true);
    require(window.findChild<QGroupBox*>(QStringLiteral("areaAttributes")) != nullptr &&
                window.findChild<QPlainTextEdit*>(QStringLiteral("areaAttributesJson")) != nullptr &&
                window.findChild<QPushButton*>(QStringLiteral("applyAreaAttributes")) != nullptr,
            "closed-boundary inspector should expose area attributes");
    const auto attributes_revision = window.document().revision();
    require(window.editSelectedAreaAttributes(QStringLiteral(
                "{\"use\":\"conditioned\",\"finish\":\"oak\"}")),
            "area attributes must commit through the selected boundary");
    auto attributed = window.document().snapshot().entities().at(outer_id.toStdString());
    require(window.document().revision() == attributes_revision + 1 &&
                attributed.properties.at("area_attributes").at("use") == "conditioned" &&
                window.findChild<QPlainTextEdit*>(QStringLiteral("areaAttributesJson"))->toPlainText()
                    .contains(QStringLiteral("conditioned")),
            "area attributes must persist and repopulate the inspector");
    const auto invalid_attributes_revision = window.document().revision();
    require(!window.editSelectedAreaAttributes(QStringLiteral("[1,2,3]")) &&
                window.document().revision() == invalid_attributes_revision,
            "invalid area attributes must fail without mutation");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().at(outer_id.toStdString()).properties.contains(
                    "area_attributes") &&
                window.redoCommand() && window.selectEntity(outer_id),
            "area attribute edits must be undoable and redoable");
    auto* editor = window.findChild<QPushButton*>(QStringLiteral("editDeductions"));
    auto* deduction_list = window.findChild<QListWidget*>(QStringLiteral("calculationDeductions"));
    require(editor && deduction_list, "calculation inspector should expose deduction controls");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("calculationDeductionDialog"));
        require(dialog, "deduction editor should open from the calculation inspector");
        auto* source = dialog->findChild<QComboBox*>(QStringLiteral("calculationDeductionSource"));
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("calculationDeductionList"));
        auto* add = dialog->findChild<QPushButton*>(QStringLiteral("addCalculationDeduction"));
        auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("removeCalculationDeduction"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("calculationDeductionButtons"));
        auto* status = dialog->findChild<QLabel*>(QStringLiteral("calculationDeductionStatus"));
        require(source && list && add && remove && buttons && status,
                "deduction editor should expose source, list, status, and apply controls");
        const auto hole_index = source->findData(hole_id);
        const auto outside_index = source->findData(outside_id);
        require(hole_index >= 0 && outside_index >= 0,
                "deduction editor should list closed boundaries on the active floor");
        source->setCurrentIndex(hole_index);
        add->click();
        require(list->count() == 1 && list->item(0)->data(Qt::UserRole).toString() == hole_id,
                "deduction editor should stage a selected boundary");
        source->setCurrentIndex(outside_index);
        add->click();
        buttons->button(QDialogButtonBox::Apply)->click();
        require(dialog->isVisible() && status->text().contains(QStringLiteral("inside"), Qt::CaseInsensitive),
                "an outside deduction must be rejected without closing the editor");
        list->setCurrentRow(1);
        remove->click();
        buttons->button(QDialogButtonBox::Apply)->click();
    });
    editor->click();

    const auto stored = window.document().snapshot().entities().at(outer_id.toStdString());
    require(stored.properties.at("deduction_ids").is_array() &&
                stored.properties.at("deduction_ids").size() == 1 &&
                stored.properties.at("deduction_ids").at(0) == hole_id.toStdString(),
            "accepted deductions should persist as explicit entity references");
    require(deduction_list->count() == 1 &&
                deduction_list->item(0)->text().contains(QStringLiteral("4.00 m²")),
            "calculation inspector should show the applied deduction trace");
    auto* net = window.findChild<QLabel*>(QStringLiteral("calculationNetArea"));
    auto* calculation_status = window.findChild<QLabel*>(QStringLiteral("calculationStatus"));
    require(net && calculation_status && net->text().contains(QStringLiteral("96.00 m²")),
            "net area should subtract the contained deduction exactly once");
    require(window.undoCommand(), "deduction edit should be undoable");
    require(!window.document().snapshot().entities().at(outer_id.toStdString()).properties.contains("deduction_ids") &&
                window.redoCommand(),
            "undo should remove the deduction reference and redo should restore it");
    require(window.document().snapshot().entities().at(outer_id.toStdString()).properties.at("deduction_ids").at(0) ==
                hole_id.toStdString(),
            "redo should restore the explicit deduction reference");

    const auto phases = ModelPhases::create(
        {outer_id.toStdString(), hole_id.toStdString()},
        {outer_id.toStdString(), hole_id.toStdString()},
        {{"demolish-hole", "Remove deduction", {hole_id.toStdString()}, {}}});
    auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
    phase_entity.id = "calculation-phases";
    window.document().apply(ApplyEntityChanges{
        window.document().revision(), {EntityChange::upsert(phase_entity)}, {},
        "add calculation phase fixture"});
    require(window.selectEntity(outer_id),
            "calculation phase fixture should refresh the selected boundary");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                net->text().contains(QStringLiteral("96.00 m²")),
            "baseline phase should retain the selected deduction total");
    require(window.selectRemodelingAlternative(QStringLiteral("demolish-hole")),
            "calculation phase selection should use the typed document command");
    require(calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                calculation_status->text().contains(QStringLiteral("hidden by the active design phase"),
                                                    Qt::CaseInsensitive),
            "a visible area with a demolished deduction must block stale totals explicitly");
    const auto phase_undo = window.undoCommand();
    require(phase_undo && window.selectedEntityId() == outer_id &&
                !calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                net->text().contains(QStringLiteral("96.00 m²")),
            "undoing a phase selection should restore the baseline calculation");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    require(font_id >= 0, "desktop smoke must load the bundled Inter font");
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    require(!families.isEmpty(), "bundled Inter font must expose a family");
    application.setFont(QFont(families.front(), 10));
    QString field_ui_capture_directory;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--capture-field-ui") {
            field_ui_capture_directory = QString::fromLocal8Bit(argv[index + 1]);
            require(QDir().mkpath(field_ui_capture_directory), "field UI capture directory");
        }
    }
    test_shortcuts_and_measurement_keypad(field_ui_capture_directory);
    test_project_subject_metadata();
    test_workspace_profiles();
    test_room_boundary_from_existing_geometry();
    test_selection_clipboard_workflow();
    test_delete_selection_workflow();
    test_boundary_vertex_insertion_workflow();
    test_boundary_redefinition_workflow();
    test_automatic_room_boundary_detection_workflow();
    test_named_revisions();
    test_boundary_transform_workflow();
    test_design_phase_workflow();
    test_phase_authoring_ownership();
    test_room_relationship_workflow();
    test_vertical_levels_workflow();
    test_assembly_catalog_workflow();
    test_calculation_deduction_workflow();
    test_contextual_roof_dimension_inspector();
    test_contextual_gable_roof_inspector(field_ui_capture_directory);
    test_survey_calculator(field_ui_capture_directory);
    {
        QTemporaryDir stamp_directory;
        require(stamp_directory.isValid(), "stamp test directory should be available");
        const auto path = stamp_directory.filePath("native.png");
        QImage original(640, 360, QImage::Format_ARGB32);
        original.fill(QColor(17, 31, 47, 90));
        require(original.save(path), "stamp fixture should be writable");
        const auto stamp = QStringLiteral("DRAFT — internal checkpoint");
        require(sketch::desktop::stampDraftImage(path, stamp), "native image should receive a draft stamp");
        const QImage stamped(path);
        require(stamped.width() == original.width() && stamped.height() > original.height(),
                "draft footer should extend the image without covering the model");
        require(stamped.copy(original.rect()) == original,
                "draft stamp should preserve model pixels including alpha");
        bool has_text = false;
        for (int y = original.height(); y < stamped.height(); ++y) {
            for (int x = 0; x < stamped.width(); ++x) {
                const auto pixel = stamped.pixelColor(x, y);
                if (pixel.red() > pixel.green() + 30 && pixel.red() > pixel.blue() + 30) has_text = true;
            }
        }
        require(has_text, "draft footer must contain visible red text");
        const auto missing = stamp_directory.filePath("missing.png");
        require(!sketch::desktop::stampDraftImage(missing, stamp), "unreadable image must fail stamping");
        require(!QFile::exists(missing), "failed stamping must not create an image");
        const auto blocked = stamp_directory.filePath("blocked.png");
        require(original.save(blocked), "blocked stamp fixture should be writable initially");
        require(QFile::setPermissions(blocked, QFileDevice::ReadOwner), "stamp fixture should become read-only");
        require(!sketch::desktop::stampDraftImage(blocked, stamp), "unwritable image must fail stamping");
        require(QFile::setPermissions(blocked, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
                "stamp fixture permissions should be restored");
    }
    test_organization_context();
    test_six_form_authoring_and_quantity_history();
    auto document = std::make_shared<sketch::Document>(sketch::Document::create());
    sketch::desktop::MainWindow window(document);

    require(window.workspaceDocumentsShareDocument(),
            "measurement and architectural workspaces must share one document");
    require(window.document().snapshot().entities().contains("property-1"),
            "a new document must create the property scaffold before objects");
    require(window.document().snapshot().entities().contains("sheet-view-1") &&
                window.document().snapshot().entities().at("sheet-view-1").type == "sheet_view_model",
            "a new document must include a validated coordinated sheet/view model");
    require(window.document().snapshot().entities().contains("annotations-1") &&
                window.document().snapshot().entities().at("annotations-1").type == "annotation_state",
            "a new document must include a validated annotation state");
    require(window.document().snapshot().entities().contains("floor-1"),
            "a new document must create the floor scaffold before objects");
    const auto seeded_sheet = window.document().snapshot().entities().at("sheet-view-1");
    const auto& seeded_views = seeded_sheet.properties.at("model").at("views");
    const auto has_view = [&](const char* id, const char* kind) {
        return std::any_of(seeded_views.begin(), seeded_views.end(), [&](const auto& view) {
            return view.at("id") == id && view.at("kind") == kind;
        });
    };
    require(has_view("view-plan", "plan") && has_view("view-elevation", "elevation") &&
                has_view("view-section", "section"),
            "a new document must persist coordinated plan, elevation and section views");
    const auto& seeded_viewports = seeded_sheet.properties.at("model")
                                       .at("sheets").at(0).at("viewports");
    const auto has_viewport = [&](const char* id, const char* view_id) {
        return std::any_of(seeded_viewports.begin(), seeded_viewports.end(),
                           [&](const auto& viewport) {
                               return viewport.at("id") == id && viewport.at("view_id") == view_id;
                           });
    };
    require(has_viewport("viewport-plan", "view-plan") &&
                has_viewport("viewport-elevation", "view-elevation") &&
                has_viewport("viewport-section", "view-section"),
            "a new sheet must persist a viewport for each coordinated view");

    const auto label_id = window.createAnnotationLabel(
        QStringLiteral("bedroom"), QStringLiteral("Primary bedroom"), {1.0, 1.0});
    const auto symbol_id = window.createAnnotationSymbol(
        QStringLiteral("chair-w1-d1"), {2.0, 1.0});
    require(!label_id.isEmpty() && !symbol_id.isEmpty(),
            "annotation authoring should create a label and a symbol");
    auto annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.size() == 1 && annotation_state.symbols.size() == 1,
            "annotation authoring should update the typed annotation entity");
    require(window.editAnnotation(label_id, QStringLiteral("Primary bedroom suite"),
                                  QStringLiteral("3.25"), QStringLiteral("2.5"),
                                  QStringLiteral("30"), QStringLiteral("1.5"), true),
            "annotation editing should use the typed command path");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.front().content == "Primary bedroom suite" &&
                std::abs(annotation_state.labels.front().placement.position.x - 3.25) < 1e-9 &&
                std::abs(annotation_state.labels.front().placement.position.y - 2.5) < 1e-9 &&
                std::abs(annotation_state.labels.front().placement.scale - 1.5) < 1e-9 &&
                annotation_state.labels.front().visible,
            "annotation editing should persist text, position, scale, and visibility");
    require(window.selectEntity(label_id), "a persisted annotation child should be selectable");
    require(window.deleteAnnotation(label_id), "annotation deletion should be undoable");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.empty() && annotation_state.symbols.size() == 1,
            "annotation deletion should remove only the selected child");
    require(window.undoCommand() && window.redoCommand() && window.undoCommand(),
            "annotation deletion should participate in normal undo and redo");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.size() == 1 && annotation_state.symbols.size() == 1,
            "annotation undo should restore the persisted child");

    QTemporaryDir reference_directory;
    require(reference_directory.isValid(), "reference fixture needs a temporary directory");
    const auto reference_path = reference_directory.filePath("trace.png");
    QImage reference_image(40, 20, QImage::Format_ARGB32);
    reference_image.fill(QColor(220, 240, 255));
    require(reference_image.save(reference_path, "PNG"), "reference fixture should save a PNG");
    const auto reference_id = window.importReferenceImage(reference_path);
    require(!reference_id.isEmpty(), "a local raster should import into the document");
    const auto reference_snapshot = window.document().snapshot();
    const auto reference_entity = reference_snapshot.entities().find(reference_id.toStdString());
    require(reference_entity != reference_snapshot.entities().end() &&
                reference_entity->second.type == "reference_asset",
            "reference import should create a typed reference entity");
    const auto reference_asset_id = reference_entity->second.properties.at("asset_id").get<std::string>();
    require(reference_snapshot.assets().contains(reference_asset_id),
            "reference import should retain source bytes in the project asset store");
    auto* reference_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(reference_canvas != nullptr && reference_canvas->reference().has_value() &&
                !reference_canvas->reference()->image.isNull(),
            "reference import should feed the shared canvas underlay");
    require(window.calibrateReference(reference_id, QStringLiteral("0"), QStringLiteral("0"),
                                      QStringLiteral("40"), QStringLiteral("0"),
                                      QStringLiteral("2 ft")),
            "reference known-distance calibration should use typed document history");
    const auto calibrated_reference = window.document().snapshot().entities().at(reference_id.toStdString());
    require(calibrated_reference.properties.at("calibration_first_source") ==
                nlohmann::json::array({0.0, 0.0}) &&
                calibrated_reference.properties.at("calibration_second_source") ==
                nlohmann::json::array({40.0, 0.0}) &&
                calibrated_reference.properties.at("calibration_known_distance") == "2 ft" &&
                std::abs(calibrated_reference.properties.at("metres_per_source_unit").get<double>() -
                         0.6096 / 40.0) < 1e-12,
            "reference calibration should retain source points, expression, and exact scale");
    require(window.editReferenceTransform(reference_id, QStringLiteral("1.25"), QStringLiteral("-0.5"),
                                          QStringLiteral("0.02"), QStringLiteral("1.5"),
                                          QStringLiteral("15"), QStringLiteral("0.4"), true,
                                          false, true),
            "reference calibration and transform should use typed document history");
    const auto transformed_reference = window.document().snapshot().entities().at(reference_id.toStdString());
    require(transformed_reference.properties.at("position_m") ==
                nlohmann::json::array({1.25, -0.5}) &&
                transformed_reference.properties.at("metres_per_source_unit") == 0.02 &&
                transformed_reference.properties.at("rotation_degrees") == 15.0 &&
                transformed_reference.properties.at("intensity") == 0.4 &&
                transformed_reference.properties.at("flip_horizontal") == true,
            "reference transform should persist calibration, placement, and presentation");
    require(window.undoCommand() && window.redoCommand(),
            "reference transform should participate in undo and redo");
    require(window.selectEntity(reference_id) && window.beginReferenceTrace() &&
                reference_canvas->boundaryDraftPreview().has_value(),
            "a selected reference should start the normal interactive tracing workflow");
    QKeyEvent cancel_trace(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(reference_canvas, &cancel_trace);
    require(!reference_canvas->boundaryDraftPreview().has_value(),
            "cancelling a reference trace should leave no unfinished boundary draft");

    // Exercise two independent retained underlays without other scene content.
    sketch::desktop::MainWindow multi_reference_window;
    const auto second_reference_path = reference_directory.filePath("second.png");
    reference_image.fill(Qt::red);
    require(reference_image.save(reference_path), "red reference fixture should save");
    reference_image.fill(Qt::blue);
    require(reference_image.save(second_reference_path), "blue reference fixture should save");
    const auto first_underlay = multi_reference_window.importReferenceImage(reference_path);
    const auto second_underlay = multi_reference_window.importReferenceImage(second_reference_path);
    require(!first_underlay.isEmpty() && !second_underlay.isEmpty(), "two raster imports should succeed");
    const auto place_underlay = [&](const QString& id, const QString& x, bool visible) {
        return multi_reference_window.editReferenceTransform(id, x, QStringLiteral("0"),
            QStringLiteral("0.01"), QStringLiteral("1"), QStringLiteral("0"),
            QStringLiteral("1"), false, false, visible);
    };
    require(place_underlay(first_underlay, QStringLiteral("-1"), true) &&
                place_underlay(second_underlay, QStringLiteral("1"), true),
            "each underlay should support independent transforms");
    const auto verify_underlays = [&] {
        auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
            multi_reference_window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
        require(canvas && canvas->references().size() == 2, "both underlays should reach the canvas");
        canvas->setGridEnabled(false);
        QImage rendered(400, 200, QImage::Format_ARGB32);
        QPainter painter(&rendered);
        canvas->renderSceneAt(painter, QRectF(0, 0, 400, 200), 100.0, {0.0, 0.0}, Qt::white);
        painter.end();
        require(rendered.pixelColor(100, 100) == QColor(Qt::red) &&
                    rendered.pixelColor(300, 100) == QColor(Qt::blue),
                "both separately positioned raster underlays must actually render");
    };
    verify_underlays();
    require(multi_reference_window.selectEntity(first_underlay) &&
                multi_reference_window.selectEntity(second_underlay),
            "both references should be independently selectable for inspection");
    const auto multi_project_path = reference_directory.filePath("multi-reference.sketch");
    require(multi_reference_window.saveProjectAs(multi_project_path) &&
                multi_reference_window.openProject(multi_project_path),
            "two embedded references should save and reopen");
    verify_underlays();
    const auto revision_before_bad_import = multi_reference_window.document().revision();
    require(multi_reference_window.importReferenceImage(reference_directory.filePath("missing.png")).isEmpty(),
            "missing reference files must fail closed");
    QFile malformed_reference(reference_directory.filePath("malformed.png"));
    require(malformed_reference.open(QIODevice::WriteOnly), "malformed fixture should open");
    malformed_reference.write("not a raster");
    malformed_reference.close();
    require(multi_reference_window.importReferenceImage(malformed_reference.fileName()).isEmpty() &&
                multi_reference_window.document().revision() == revision_before_bad_import,
            "malformed imports must leave existing references and document history unchanged");
    verify_underlays();

    auto missing_asset_reference = multi_reference_window.document().snapshot().entities().at(
        first_underlay.toStdString());
    missing_asset_reference.properties["render_asset_id"] = "missing-render-asset";
    bool missing_asset_rejected = false;
    try {
        multi_reference_window.document().apply(sketch::ApplyEntityChanges{
            .expected_revision = multi_reference_window.document().revision(),
            .entity_changes = {sketch::EntityChange::upsert(missing_asset_reference)},
            .message = "reject missing underlay asset",
        });
    } catch (const std::exception&) { missing_asset_rejected = true; }
    require(missing_asset_rejected &&
                multi_reference_window.document().revision() == revision_before_bad_import,
            "missing embedded render assets must be rejected atomically");
    verify_underlays();

    QTemporaryDir pdf_directory;
    require(pdf_directory.isValid(), "PDF reference fixture needs a temporary directory");
    const auto reference_pdf_path = pdf_directory.filePath("trace.pdf");
    {
        QPdfWriter writer(reference_pdf_path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(144);
        QPainter painter(&writer);
        require(painter.isActive(), "PDF reference fixture should open a writer");
        painter.setPen(QPen(QColor(60, 80, 100), 8));
        painter.drawRect(QRectF(40.0, 40.0, 500.0, 320.0));
        painter.drawLine(QPointF(40.0, 200.0), QPointF(540.0, 200.0));
        painter.end();
    }
    sketch::desktop::MainWindow pdf_window;
    const auto pdf_id = pdf_window.importReferenceImage(reference_pdf_path);
    require(!pdf_id.isEmpty(), "a local PDF first page should import into the document");
    const auto pdf_snapshot = pdf_window.document().snapshot();
    const auto pdf_entity = pdf_snapshot.entities().find(pdf_id.toStdString());
    require(pdf_entity != pdf_snapshot.entities().end(), "PDF import should create a reference entity");
    const auto source_asset_id = pdf_entity->second.properties.at("asset_id").get<std::string>();
    const auto render_asset_id = pdf_entity->second.properties.at("render_asset_id").get<std::string>();
    require(pdf_snapshot.assets().at(source_asset_id).media_type == "application/pdf" &&
                source_asset_id != render_asset_id && pdf_snapshot.assets().contains(render_asset_id),
            "PDF import should retain source bytes and a separate preview asset");
    const auto& preview_asset = pdf_snapshot.assets().at(render_asset_id);
    const QByteArray preview_raw(reinterpret_cast<const char*>(preview_asset.bytes.data()),
                                 static_cast<qsizetype>(preview_asset.bytes.size()));
    require(!QImage::fromData(preview_raw).isNull(), "PDF import should decode a local first-page preview");
    auto* pdf_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        pdf_window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(pdf_canvas != nullptr && pdf_canvas->reference().has_value() &&
                !pdf_canvas->reference()->image.isNull(),
            "PDF import should feed the shared canvas underlay");
    require(pdf_window.undoCommand() && pdf_window.redoCommand(),
            "PDF import should participate in normal document history");
    require(window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.5"), QStringLiteral("80"),
                QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine")),
            "architectural view presentation must commit through typed Document history");
    const auto edited_view_entity = window.document().snapshot().entities().at("sheet-view-1");
    const auto plan_view = std::find_if(
        edited_view_entity.properties.at("model").at("views").begin(),
        edited_view_entity.properties.at("model").at("views").end(),
        [](const auto& view) { return view.at("id") == "view-plan"; });
    require(plan_view != edited_view_entity.properties.at("model").at("views").end() &&
                plan_view->at("presentation").at("cut_depth_m") == 1.5 &&
                plan_view->at("presentation").at("detail") == "fine" &&
                plan_view->at("presentation").at("hatch_scale") == 2.0,
            "architectural view presentation edit must persist typed settings");
    require(window.undoCommand() && window.redoCommand(),
            "architectural view presentation edit must participate in normal document history");

    const auto boundary_id = window.createBoundary(
        sketch::Boundary{{{{0.0, 0.0}, {3.0, 0.0}, 0.0},
                          {{3.0, 0.0}, {3.0, 2.0}, 0.0},
                          {{3.0, 2.0}, {0.0, 2.0}, 0.0},
                          {{0.0, 2.0}, {0.0, 0.0}, 0.0}}});
    require(!boundary_id.isEmpty(), "closed boundary should be accepted as a document command");

    auto* calculation_status = window.findChild<QLabel*>(QStringLiteral("calculationStatus"));
    auto* base_area = window.findChild<QLabel*>(QStringLiteral("calculationBaseArea"));
    auto* net_area = window.findChild<QLabel*>(QStringLiteral("calculationNetArea"));
    auto* factored_area = window.findChild<QLabel*>(QStringLiteral("calculationFactoredArea"));
    auto* perimeter_value = window.findChild<QLabel*>(QStringLiteral("calculationPerimeter"));
    auto* rounding_value = window.findChild<QLabel*>(QStringLiteral("calculationRounding"));
    auto* building_total = window.findChild<QLabel*>(QStringLiteral("calculationBuildingTotal"));
    auto* living_total = window.findChild<QLabel*>(QStringLiteral("calculationLivingTotal"));
    require(calculation_status != nullptr && base_area != nullptr && net_area != nullptr &&
                factored_area != nullptr && perimeter_value != nullptr && rounding_value != nullptr &&
                building_total != nullptr && living_total != nullptr,
            "calculation inspector labels should be available for a selected boundary");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive),
            "a classified boundary should have an active calculation profile rule");
    require(base_area->text().contains(QStringLiteral("64.58")) &&
                net_area->text().contains(QStringLiteral("64.58")) &&
                factored_area->text().contains(QStringLiteral("64.58")),
            "selected boundary should show base, net, and factored area in default imperial units");
    require(!perimeter_value->text().isEmpty() && perimeter_value->text() != QStringLiteral("—"),
            "selected boundary should show its perimeter");
    require(rounding_value->text().contains(QStringLiteral("unrounded"), Qt::CaseInsensitive) &&
                rounding_value->text().contains(QStringLiteral("difference"), Qt::CaseInsensitive),
            "selected boundary should show the area rounding explanation");

    require(window.editSelectedFactor(QStringLiteral("3/4")),
            "exact rational area factor should be editable from the inspector API");
    const auto factor_snapshot = window.document().snapshot();
    const auto& factor_entities = factor_snapshot.entities();
    const auto factor_entity = factor_entities.find(boundary_id.toStdString());
    require(factor_entity != factor_entities.end() &&
                factor_entity->second.properties.at("factor_expression") == "3/4" &&
                factor_entity->second.properties.at("factor_numerator").get<std::int64_t>() == 3 &&
                factor_entity->second.properties.at("factor_denominator").get<std::int64_t>() == 4 &&
                std::abs(factor_entity->second.properties.at("factor").get<double>() - 0.75) < 1e-12,
            "factor edit should preserve the expression, rational components, and numeric value");
    require(factored_area->text().contains(QStringLiteral("48.44")),
            "factored area should apply the exact rational factor");
    require(window.undoCommand(), "factor edit should be undoable");
    const auto factor_undo_snapshot = window.document().snapshot();
    const auto& factor_undo_entities = factor_undo_snapshot.entities();
    const auto factor_undo = factor_undo_entities.find(boundary_id.toStdString());
    require(factor_undo != factor_undo_entities.end() &&
                factor_undo->second.properties.at("factor_numerator").get<std::int64_t>() == 1,
            "undo should restore the original area factor");
    require(window.redoCommand(), "factor edit should be redoable");
    require(window.selectEntity(boundary_id), "boundary should be reselected after factor history");
    const auto factor_redo_snapshot = window.document().snapshot();
    const auto& factor_redo_entities = factor_redo_snapshot.entities();
    const auto factor_redo = factor_redo_entities.find(boundary_id.toStdString());
    require(factor_redo != factor_redo_entities.end() &&
                factor_redo->second.properties.at("factor_expression") == "3/4",
            "redo should restore the exact factor expression");

    require(window.editSelectedClassification(QStringLiteral("unassigned")),
            "boundary classification should be editable through the shared command path");
    require(calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                calculation_status->text().contains(QStringLiteral("no calculation profile rule"),
                                                     Qt::CaseInsensitive),
            "unknown classification should visibly block totals");
    const auto profile_before_snapshot = window.document().snapshot();
    const auto& profile_before_entities = profile_before_snapshot.entities();
    const auto profile_before = profile_before_entities.at("property-1");
    const auto profile_version_before =
        profile_before.properties.at("calculation_profile").at("version").get<unsigned>();
    require(window.setSelectedCalculationRule(true, true),
            "calculation profile rule should be assignable explicitly");
    const auto profile_after_snapshot = window.document().snapshot();
    const auto& profile_after_entities = profile_after_snapshot.entities();
    const auto profile_after = profile_after_entities.at("property-1");
    require(profile_after.properties.at("calculation_profile").at("version").get<unsigned>() ==
                profile_version_before + 1 &&
                profile_after.properties.at("calculation_profile").at("classifications")
                        .at("unassigned")
                        .at("building_total")
                        .get<bool>() &&
                profile_after.properties.at("calculation_profile").at("classifications")
                        .at("unassigned")
                        .at("living_total")
                        .get<bool>(),
            "profile edits should version and persist explicit building and living rules");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                !building_total->text().contains(QStringLiteral("—")) &&
                !living_total->text().contains(QStringLiteral("—")),
            "assigned profile rules should restore building and living totals");
    const auto overlapping_boundary_id = window.createBoundary(
        sketch::Boundary{{{{0.5, 0.5}, {1.5, 0.5}, 0.0},
                          {{1.5, 0.5}, {1.5, 1.5}, 0.0},
                          {{1.5, 1.5}, {0.5, 1.5}, 0.0},
                          {{0.5, 1.5}, {0.5, 0.5}, 0.0}}});
    require(!overlapping_boundary_id.isEmpty(), "overlapping test boundary should be created");
    require(calculation_status->text().contains(QStringLiteral("overlap"), Qt::CaseInsensitive) &&
                base_area->text() == QStringLiteral("—") && building_total->text() == QStringLiteral("—"),
            "overlapping boundaries should block totals instead of showing plausible values");
    require(window.undoCommand(), "overlapping test boundary should be undoable");
    require(window.selectEntity(boundary_id), "original boundary should be selectable after overlap undo");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive),
            "undoing the overlap should restore valid totals");
    window.setMetricUnits(true);
    require(base_area->text().contains(QStringLiteral("6.00 m²")) &&
                factored_area->text().contains(QStringLiteral("4.50 m²")),
            "calculation values should refresh in metric display units");
    window.setMetricUnits(false);

    const auto wall_id = window.createStraightWall({0.0, 0.0}, {3.0, 0.0}, "exterior");
    require(!wall_id.isEmpty(), "straight wall should be accepted as a document command");
    require(window.selectEntity(wall_id), "created wall should be selectable");
    require(window.editSelectedClassification("party"),
            "wall classification should be editable from the inspector API");
    require(window.editSelectedHeight("8 ft"),
            "wall height should parse and update through the inspector API");
    require(window.editSelectedThickness("6 in"),
            "wall thickness should parse and update through the inspector API");

    require(window.undoCommand(), "wall property edit should be undoable");
    require(window.redoCommand(), "wall property edit should be redoable");

    // Hosted openings are separate semantic entities. The wall preview is
    // passed through the architecture kernel before the atomic document
    // command is applied, so an out-of-bounds opening must leave the revision
    // untouched.
    require(window.selectEntity(wall_id), "wall should remain selected for hosted opening creation");
    const auto revision_before_invalid_opening = window.document().revision();
    require(window.createHostedOpening(QStringLiteral("door"), QStringLiteral("1 m"),
                                       QStringLiteral("20 m"), QStringLiteral("0 m"),
                                       QStringLiteral("2 m"))
                .isEmpty(),
            "an opening outside the host wall must be rejected by the kernel preview");
    require(window.document().revision() == revision_before_invalid_opening,
            "rejected opening preview must not mutate the document");

    const auto opening_id = window.createHostedOpening(
        QStringLiteral("door"), QStringLiteral("0.25 m"), QStringLiteral("0.5 m"),
        QStringLiteral("0 m"), QStringLiteral("2 m"));
    require(!opening_id.isEmpty(), "a hosted door should be accepted on the selected wall");
    require(window.selectEntity(opening_id), "created opening should be selectable");
    require(window.editSelectedLength("0.6 m"),
            "opening width should use the quantity parser and inspector command");
    require(window.editSelectedHeight("1.9 m"),
            "opening height should use the quantity parser and inspector command");
    require(window.editSelectedClassification("window"),
            "opening classification should distinguish door and window semantics");

    require(window.selectEntity(boundary_id), "closed boundary should be selectable for slab creation");
    const auto revision_before_invalid_slab = window.document().revision();
    require(window.createSlabFromSelectedBoundary("0 m", "0 m").isEmpty(),
            "a zero thickness slab must be rejected before the document command");
    require(window.document().revision() == revision_before_invalid_slab,
            "rejected slab preview must not mutate the document");
    const auto slab_id = window.createSlabFromSelectedBoundary("0.15 m", "0 m");
    require(!slab_id.isEmpty(), "a slab should be created from the selected closed boundary");
    require(window.selectEntity(slab_id), "created slab should be selectable");
    require(window.editSelectedThickness("0.2 m"),
            "slab thickness should use the quantity parser and kernel preview");
    require(window.undoCommand(), "slab thickness edit should be undoable");
    const auto slab_undo_snapshot = window.document().snapshot();
    const auto& slab_undo_entities = slab_undo_snapshot.entities();
    const auto slab_after_undo = slab_undo_entities.find(slab_id.toStdString());
    require(slab_after_undo != slab_undo_entities.end() &&
                slab_after_undo->second.properties.at("thickness_m").get<double>() < 0.16,
            "undo should restore the previous slab thickness");
    require(window.redoCommand(), "slab thickness edit should be redoable");

    const auto architectural_snapshot = window.document().snapshot();
    const auto& architectural_entities = architectural_snapshot.entities();
    const auto opening = architectural_entities.find(opening_id.toStdString());
    require(opening != architectural_entities.end() && opening->second.type == "opening",
            "opening must be persisted as a semantic opening entity");
    require(opening->second.properties.at("wall_id").get<std::string>() == wall_id.toStdString(),
            "opening must retain its host wall reference");
    require(opening->second.properties.at("width_m").get<double>() > 0.59,
            "opening width must be stored in canonical metres");
    const auto slab = architectural_entities.find(slab_id.toStdString());
    require(slab != architectural_entities.end() && slab->second.type == "slab",
            "slab must be persisted as a semantic slab entity");
    require(slab->second.properties.at("boundary").is_array() &&
                slab->second.properties.at("holes").is_array(),
            "slab must persist its boundary and holes arrays");
    require(slab->second.properties.at("thickness_m").get<double>() > 0.19,
            "slab thickness must be stored in canonical metres");
    const auto schedules = window.scheduleSnapshot();
    const auto opening_row = std::find_if(schedules.snapshot.rows.begin(),
                                          schedules.snapshot.rows.end(),
        [&](const auto& row) { return row.object_id == opening_id.toStdString(); });
    require(opening_row != schedules.snapshot.rows.end() &&
                opening_row->cells.contains("area") &&
                !opening_row->cells.at("area").editable &&
                schedules.snapshot.revision == window.document().revision(),
            "shared document schedules must include hosted openings with a read-only calculated area");
    require(window.editScheduleCell(opening_id, QStringLiteral("width"), QStringLiteral("0.7 m")),
            "editable schedule source cells must commit through the document command path");
    require(window.document().snapshot().entities().at(opening_id.toStdString()).properties.at("width_m") == 0.7,
            "schedule edit must update the canonical opening property");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(opening_id.toStdString()).properties.at("width_m") == 0.6,
            "schedule edit must be undoable from the normal workspace history");
    require(window.redoCommand() &&
                window.document().snapshot().entities().at(opening_id.toStdString()).properties.at("width_m") == 0.7,
            "schedule edit must be redoable from the normal workspace history");

    window.setMetricUnits(true);
    require(window.metricUnits(), "metric display toggle should be observable");
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    require(window.workspace() == sketch::desktop::Workspace::architectural,
            "architectural workspace should be selectable");
    window.setWorkspace(sketch::desktop::Workspace::measurement);

    QTemporaryDir temporary_directory;
    require(temporary_directory.isValid(), "smoke test needs a temporary directory");
    const auto project_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.bldproj";
    const auto project_path_qstring = QString::fromStdWString(project_path.wstring());
    const auto pdf_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.pdf";
    const auto pdf_path_qstring = QString::fromStdWString(pdf_path.wstring());
    const auto svg_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.svg";
    const auto svg_path_qstring = QString::fromStdWString(svg_path.wstring());
    auto column = sketch::encode_building_entity(
        sketch::RectangularColumn{"", {1.0, 2.0, 0.0}, 0.3, 0.4, 3.0, 0.0},
        {{"private_note", "preserve this"}});
    column.properties["future_attribute"] = "preserve this too";
    const auto column_id = window.commitBuildingObject(column, window.document().revision());
    require(!column_id.isEmpty(), "building object must commit through the workspace command");
    require(navigator_item(window, column_id) != nullptr,
            "new building object must be selectable through the project navigator");
    auto* create_object_button = window.findChild<QToolButton*>(QStringLiteral("createBuildingObject"));
    auto* edit_object_button = window.findChild<QPushButton*>(QStringLiteral("editBuildingObject"));
    require(create_object_button && create_object_button->isEnabled() &&
                edit_object_button && !edit_object_button->isHidden() && edit_object_button->isEnabled(),
            "building object tools must be available for the current editable selection");
    auto* plan_widget = window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas"));
    auto* plan = dynamic_cast<sketch::desktop::PlanCanvas*>(plan_widget);
    require(plan != nullptr, "measurement canvas must expose its derived drawing geometry");
    const auto plan_column = std::find_if(plan->entities().begin(), plan->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(plan_column != plan->entities().end() && !plan_column->segments.empty(),
            "new building object must appear in the shared plan/PDF geometry");
    require(plan_column->segments.size() == 4 &&
                std::all_of(plan_column->segments.begin(), plan_column->segments.end(),
                    [](const auto& segment) { return segment.sweep_radians == 0.0; }),
            "shared plan/PDF rectangle must have exactly four straight edges");
    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (const auto& segment : plan_column->segments) {
        for (const auto& point : {segment.start, segment.end}) {
            min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y); max_y = std::max(max_y, point.y);
        }
    }
    require(std::abs(min_x - 0.85) < 1e-7 && std::abs(max_x - 1.15) < 1e-7 &&
                std::abs(min_y - 1.8) < 1e-7 && std::abs(max_y - 2.2) < 1e-7,
            "column plan must use its real dimensions and world placement");
    auto* architectural_widget = window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas"));
    auto* architectural = dynamic_cast<sketch::desktop::PlanCanvas*>(architectural_widget);
    auto* architectural_view = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    require(architectural && architectural_view && architectural_view->count() == 3,
            "architectural view selector and canvas should be available");
    architectural_view->setCurrentText(QStringLiteral("Elevation"));
    const auto elevation_column = std::find_if(architectural->entities().begin(),
                                               architectural->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(elevation_column != architectural->entities().end() &&
                elevation_column->segments.size() == 4,
            "architectural elevation should project the same column as four edges");
    double elevation_min_y = 1e9, elevation_max_y = -1e9;
    for (const auto& segment : elevation_column->segments) {
        elevation_min_y = std::min({elevation_min_y, segment.start.y, segment.end.y});
        elevation_max_y = std::max({elevation_max_y, segment.start.y, segment.end.y});
    }
    require(std::abs(elevation_min_y) < 1e-7 && std::abs(elevation_max_y - 3.0) < 1e-7,
            "architectural elevation must preserve the column height");
    architectural_view->setCurrentText(QStringLiteral("Section @ 1.2 m"));
    const auto section_column = std::find_if(architectural->entities().begin(),
                                             architectural->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(section_column != architectural->entities().end() &&
                section_column->segments.size() == 4,
            "architectural section should intersect the same column with four edges");
    architectural_view->setCurrentText(QStringLiteral("Plan"));
    auto edited_column = column;
    edited_column.properties["height_m"] = 3.5;
    edited_column.extensions = nlohmann::json::object();
    edited_column.properties.erase("future_attribute");
    require(!window.commitBuildingObject(edited_column, window.document().revision(), true).isEmpty(),
            "building object dimensions must be editable");
    const auto edited_column_snapshot = window.document().snapshot();
    const auto& stored_column = edited_column_snapshot.entities().at(column_id.toStdString());
    require(stored_column.properties.at("height_m") == 3.5 &&
                stored_column.properties.at("future_attribute") == "preserve this too" &&
                stored_column.extensions.at("private_note") == "preserve this",
            "geometry edit must preserve unknown semantic fields and extensions");
    const auto stale_column_revision = window.document().revision();
    require(window.undoCommand(), "building object edit must be undoable");
    require(window.document().snapshot().entities().at(column_id.toStdString()).properties.at("height_m") == 3.0,
            "undo restores exact prior building dimensions");
    require(window.selectEntity(column_id), "reselect building object after undo");
    const auto before_stale = window.document().revision();
    require(window.commitBuildingObject(edited_column, stale_column_revision, true).isEmpty() &&
                window.document().revision() == before_stale,
            "stale object dialog must fail atomically");
    require(window.redoCommand(), "building object edit must be redoable");
    require(window.selectEntity(column_id), "reselect building object after redo");
    auto invalid_column = edited_column;
    invalid_column.properties["height_m"] = -1.0;
    const auto before_invalid_column = window.document().revision();
    require(window.commitBuildingObject(invalid_column, before_invalid_column, true).isEmpty() &&
                window.document().revision() == before_invalid_column,
            "invalid building geometry must not mutate the document");
    require(window.commitBuildingObject(column, before_invalid_column).isEmpty(),
            "object creation must reject an existing identity");
    require(window.editSheetMetadata(QStringLiteral("sheet-1"), QStringLiteral("A-102"),
                                     QStringLiteral("Sample property"), QStringLiteral("Issued plans"),
                                     QStringLiteral("Field designer"), QStringLiteral("2026-09-11")),
            "sheet metadata must commit through the typed Document command path");
    const auto edited_sheet_entity = window.document().snapshot().entities().at("sheet-view-1");
    require(edited_sheet_entity.properties.at("model").at("sheets").at(0).at("number") == "A-102" &&
                edited_sheet_entity.properties.at("model").at("sheets").at(0).at("title_block").at("title") == "Issued plans",
            "sheet metadata edit must persist in the canonical sheet/view entity");
    require(window.undoCommand() && window.redoCommand(),
            "sheet metadata edit must participate in normal document history");
    const auto added_sheet_id = window.createDrawingSheet(QStringLiteral("A-201"),
                                                          QStringLiteral("420"),
                                                          QStringLiteral("297"),
                                                          QStringLiteral("Details"));
    require(!added_sheet_id.isEmpty() && window.outputSheetId() == added_sheet_id,
            "creating a drawing sheet should select the new page for output");
    const auto added_sheet_model = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    const auto added_sheet = std::find_if(added_sheet_model.sheets().begin(),
                                          added_sheet_model.sheets().end(),
        [&](const auto& sheet) { return sheet.id == added_sheet_id.toStdString(); });
    require(added_sheet != added_sheet_model.sheets().end() &&
                added_sheet->number == "A-201" && added_sheet->viewports.size() == 3,
            "new drawing sheet should persist its metadata and coordinated viewports");
    require(window.exportDraftPdf(pdf_path_qstring),
            "selected new drawing sheet should render through the shared PDF path");
    QFile selected_sheet_fingerprint(QString::fromStdWString(
        std::filesystem::path(pdf_path.wstring() + L".fingerprint.json").wstring()));
    require(selected_sheet_fingerprint.open(QIODevice::ReadOnly | QIODevice::Text),
            "selected-sheet PDF fingerprint should be readable");
    const auto selected_sheet_manifest =
        nlohmann::json::parse(selected_sheet_fingerprint.readAll().toStdString());
    selected_sheet_fingerprint.close();
    bool selected_sheet_bound = false;
    for (const auto& resource : selected_sheet_manifest.at("fingerprint").at("manifest")
                                      .at("dependencies").at("views").at("resources")) {
        if (resource.at("metadata").at("sheet_id") == added_sheet_id.toStdString()) {
            selected_sheet_bound = true;
            break;
        }
    }
    require(selected_sheet_bound, "selected-sheet output fingerprint must bind the requested page");
    require(window.selectOutputSheet(QStringLiteral("sheet-1")) &&
                window.outputSheetId() == QStringLiteral("sheet-1"),
            "output sheet selection should be local presentation state");
    require(window.removeDrawingSheet(added_sheet_id),
            "drawing sheet removal should use the typed document command path");
    const auto after_sheet_remove = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    require(after_sheet_remove.sheets().size() == 1 &&
                window.outputSheetId() == QStringLiteral("sheet-1"),
            "removing a sheet should preserve the remaining output page");
    require(window.undoCommand() && window.redoCommand(),
            "drawing sheet removal must participate in normal document history");
    require(window.editSheetViewport(QStringLiteral("sheet-1"), QStringLiteral("viewport-plan"),
                                     QStringLiteral("15"), QStringLiteral("15"),
                                     QStringLiteral("390"), QStringLiteral("267"),
                                     QStringLiteral("75")),
            "sheet viewport must commit through the typed Document command path");
    const auto edited_viewport_entity = window.document().snapshot().entities().at("sheet-view-1");
    const auto& edited_viewports = edited_viewport_entity.properties.at("model")
                                       .at("sheets").at(0).at("viewports");
    const auto edited_viewport = std::find_if(
        edited_viewports.begin(), edited_viewports.end(), [](const auto& viewport) {
            return viewport.at("id") == "viewport-plan";
        });
    require(edited_viewport != edited_viewports.end() &&
                edited_viewport->at("bounds").at("width_mm") == 390.0 &&
                edited_viewport->at("scale_denominator") == 75.0,
            "sheet viewport edit must persist bounds and independent scale");
    require(window.undoCommand() && window.redoCommand(),
            "sheet viewport edit must participate in normal document history");
    require(window.editSheetSchedulePlacement(QStringLiteral("sheet-1"),
                                              QStringLiteral("schedule-doors"),
                                              QStringLiteral("235"), QStringLiteral("225"),
                                              QStringLiteral("175"), QStringLiteral("55")),
            "schedule placement must commit through the typed Document command path");
    const auto edited_schedule_entity = window.document().snapshot().entities().at("sheet-view-1");
    require(edited_schedule_entity.properties.at("model").at("sheets").at(0)
                    .at("schedules").at(0).at("bounds").at("x_mm") == 235.0 &&
                edited_schedule_entity.properties.at("model").at("sheets").at(0)
                    .at("schedules").at(0).at("bounds").at("height_mm") == 55.0,
            "schedule placement edit must persist its page bounds");
    require(window.undoCommand() && window.redoCommand(),
            "schedule placement edit must participate in normal document history");
    const auto revision_before_pdf = window.document().revision();
    require(window.exportDraftPdf(pdf_path_qstring), "draft PDF export should succeed locally");
    require(std::filesystem::file_size(pdf_path) > 0, "draft PDF should be nonempty");
    const auto pdf_fingerprint_path = std::filesystem::path(pdf_path.wstring() + L".fingerprint.json");
    require(std::filesystem::file_size(pdf_fingerprint_path) > 0,
            "draft PDF must have an adjacent output fingerprint");
    QFile pdf_fingerprint(QString::fromStdWString(pdf_fingerprint_path.wstring()));
    require(pdf_fingerprint.open(QIODevice::ReadOnly | QIODevice::Text),
            "draft PDF fingerprint should be readable");
    const auto pdf_fingerprint_json = nlohmann::json::parse(pdf_fingerprint.readAll().toStdString());
    require(pdf_fingerprint_json.at("output_kind") == "pdf" &&
                pdf_fingerprint_json.at("fingerprint").at("digest_sha256").is_string(),
            "draft PDF fingerprint must identify the output and digest");
    pdf_fingerprint.close();
    require(window.exportDraftSvg(svg_path_qstring), "draft SVG export should succeed locally");
    require(std::filesystem::file_size(svg_path) > 0, "draft SVG should be nonempty");
    QFile svg_output(svg_path_qstring);
    require(svg_output.open(QIODevice::ReadOnly | QIODevice::Text),
            "draft SVG should be readable for schedule placement verification");
    const auto svg_text = svg_output.readAll();
    require(svg_text.contains("DOORS SCHEDULE") && svg_text.contains("ELEVATION") &&
                svg_text.contains("SECTION"),
            "draft SVG should render the persisted schedule and coordinated view captions");
    svg_output.close();
    const auto svg_fingerprint_path = std::filesystem::path(svg_path.wstring() + L".fingerprint.json");
    require(std::filesystem::file_size(svg_fingerprint_path) > 0,
            "draft SVG must have an adjacent output fingerprint");
    auto* page_size = window.findChild<QComboBox*>(QStringLiteral("outputPageSize"));
    require(page_size && page_size->count() == 5, "output sheet selector should expose five page sizes");
    page_size->setCurrentText(QStringLiteral("A3"));
    require(window.exportDraftPdf(pdf_path_qstring), "A3 draft PDF export should succeed locally");
    require(std::filesystem::file_size(pdf_path) > 0, "A3 draft PDF should be nonempty");
    require(std::filesystem::file_size(pdf_fingerprint_path) > 0,
            "A3 draft PDF must refresh its adjacent output fingerprint");
    require(window.document().revision() == revision_before_pdf,
            "draft output must not mutate the semantic document");
    require(window.saveProjectAs(project_path_qstring),
            "save-as should persist the current snapshot");
    require(!window.document().dirty(), "saving the current head should clear dirty state");
    require(window.openProject(project_path_qstring),
            "saved project should reopen through the real project store");
    const auto reopened = window.document().snapshot();
    require(reopened.entities().at(column_id.toStdString()).properties.at("height_m") == 3.5 &&
                reopened.entities().at(column_id.toStdString()).extensions.at("private_note") == "preserve this",
            "save/reopen must preserve edited building object dimensions and metadata");
    require(reopened.entities().contains(wall_id.toStdString()),
            "reopened project should preserve the semantic wall entity");
    require(reopened.entities().contains(opening_id.toStdString()),
            "reopened project should preserve the hosted opening entity");
    require(reopened.entities().contains(slab_id.toStdString()),
            "reopened project should preserve the slab entity");
    require(reopened.entities().contains("sheet-view-1") &&
                reopened.entities().at("sheet-view-1").type == "sheet_view_model",
            "save/reopen must preserve the coordinated sheet/view model");
    require(reopened.entities().at(opening_id.toStdString()).properties.at("wall_id") ==
                wall_id.toStdString(),
            "reopened opening should keep the host reference");
    require(reopened.entities().at(boundary_id.toStdString()).properties.at("factor_expression") ==
                "3/4" &&
                reopened.entities().at("property-1").properties.at("calculation_profile")
                        .at("classifications")
                        .at("unassigned")
                        .at("living_total")
                        .get<bool>(),
            "save and reopen should preserve the exact factor and versioned profile rule");
    require(window.selectEntity(boundary_id), "reopened boundary should be selectable");
    require(edit_object_button->isHidden(), "building object editor must hide for a measurement boundary");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                base_area->text().contains(QStringLiteral("6.00 m²")) &&
                factored_area->text().contains(QStringLiteral("4.50 m²")),
            "calculation inspector should refresh from the reopened document");

    auto malformed = window.document().snapshot().entities().at(column_id.toStdString());
    malformed.properties["form"] = "unsupported-form";
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(malformed)},
        .message = "exercise unsupported loaded geometry",
    });
    require(window.selectEntity(column_id), "select malformed loaded building object");
    const auto protected_output =
        QString::fromStdWString((project_path.parent_path() / "must-not-exist.pdf").wstring());
    const auto protected_svg =
        QString::fromStdWString((project_path.parent_path() / "must-not-exist.svg").wstring());
    require(!window.exportDraftPdf(protected_output) && !window.exportDraftSvg(protected_svg) &&
                !window.showPrintPreview() &&
                !std::filesystem::exists(std::filesystem::path(protected_output.toStdWString())) &&
                !std::filesystem::exists(std::filesystem::path(protected_svg.toStdWString())),
            "invalid plan geometry must block output before creating a file or print dialog");
    auto* plan_error = window.findChild<QLabel*>(QStringLiteral("planGeometryError"));
    require(plan_error && !plan_error->isHidden() && !plan_error->text().isEmpty(),
            "a missing building projection must be visible in the workspace");
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--capture-invalid-plan") {
            window.setAttribute(Qt::WA_DontShowOnScreen, true);
            window.resize(1366, 768);
            window.show();
            QCoreApplication::processEvents();
            require(window.grab().save(QString::fromLocal8Bit(argv[index + 1])),
                    "invalid-plan workspace capture must be written");
            window.hide();
        }
    }
    require(window.undoCommand(), "undo restores renderable building geometry");
    require(plan_error->isHidden(), "geometry error must clear after the valid object is restored");
    return 0;
}
