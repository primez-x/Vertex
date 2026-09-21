#include "sketch/desktop/main_window.hpp"

#include "sketch/document.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/desktop/hosted_opening_dialog.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/reference_grid.hpp"
#include "sketch/project_store.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/quantity.hpp"
#include "sketch/field_adapter_contract.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/georeferencing_entity_codec.hpp"
#include "sketch/product_scope.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/trusted_reference_fixture.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/draft_image_stamp.hpp"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
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
#include <QMenu>
#include <QPainter>
#include <QPageSize>
#include <QPdfWriter>
#include <QPdfDocument>
#include <QXmlStreamReader>
#include <QPlainTextEdit>
#include <QPointingDevice>
#include <QPushButton>
#include <QGroupBox>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QStandardPaths>
#include <QSplitter>
#include <QSpinBox>
#include <QTouchEvent>
#include <QTimer>
#include <QToolBar>
#include <QTabletEvent>
#include <QUuid>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <string_view>
#include <thread>

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
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-shortcut-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto settings_directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const auto settings_path = settings_directory + QStringLiteral("/keyboard-shortcuts.json");
    {
        sketch::desktop::MainWindow window;
        auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("primaryToolbar"));
            auto* symbol_library =
                window.findChild<QListWidget*>(QStringLiteral("symbolLibraryItems"));
            require(symbol_library && symbol_library->count() > 0 &&
                        symbol_library->dragDropMode() == QAbstractItemView::DragOnly &&
                        symbol_library->dragEnabled() &&
                        symbol_library->item(0)->flags().testFlag(Qt::ItemIsDragEnabled),
                    "component library items must initiate an external drag onto the plan");
            auto* more_tools = window.findChild<QToolButton*>(QStringLiteral("moreTools"));
            auto* theme_menu = window.findChild<QToolButton*>(QStringLiteral("themeMenu"));
            auto* page_size = window.findChild<QComboBox*>(QStringLiteral("outputPageSize"));
            require(toolbar != nullptr && toolbar->minimumHeight() == 28 && toolbar->maximumHeight() == 28 &&
                    toolbar->height() == 28 &&
                    toolbar->toolButtonStyle() == Qt::ToolButtonTextBesideIcon &&
                    toolbar->iconSize() == QSize(18, 18) && more_tools && theme_menu &&
                    more_tools->toolButtonStyle() == Qt::ToolButtonTextBesideIcon &&
                    theme_menu->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                    !more_tools->accessibleName().isEmpty() && !theme_menu->accessibleName().isEmpty() &&
                    window.findChild<QToolButton*>(QStringLiteral("quickAccess")) == nullptr &&
                    page_size && !page_size->isVisible() &&
                    toolbar->widgetForAction(toolbar->actions().back()) == theme_menu &&
                    window.findChild<QWidget*>(QStringLiteral("workspaceTabs")) != nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("workspaceHeader")) == nullptr &&
                    window.findChild<QLabel*>(QStringLiteral("appMark")) == nullptr &&
                    window.findChild<QLabel*>(QStringLiteral("appTitle")) == nullptr &&
                    window.findChild<QLabel*>(QStringLiteral("projectHeader")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("checkpointBanner")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("offlineBadge")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("appSubtitle")) == nullptr,
                    "modern workspace shell must expose a compact toolbar and tabs without redundant branding or status copy");
        bool regeneration_ready = false;
        for (int attempt = 0; attempt != 200 && !regeneration_ready; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            regeneration_ready = window.regenerationReadyForCurrentRevision();
        }
        require(regeneration_ready,
                "desktop refresh must publish a source-bound regeneration result asynchronously");
        auto* measurement_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
            window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
        auto* architectural_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
            window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
        require(measurement_canvas && architectural_canvas &&
                    measurement_canvas->testAttribute(Qt::WA_AcceptTouchEvents) &&
                    architectural_canvas->testAttribute(Qt::WA_AcceptTouchEvents) &&
                    measurement_canvas->testAttribute(Qt::WA_TabletTracking) &&
                    architectural_canvas->testAttribute(Qt::WA_TabletTracking),
                "both workspace canvases must accept explicit touch and active-pen events");
        auto* sidebar_tabs = window.findChild<QTabWidget*>(QStringLiteral("sidebarTabs"));
        auto* status_controls = window.findChild<QWidget*>(QStringLiteral("canvasStatusControls"));
        auto* object_tool = window.findChild<QToolButton*>(QStringLiteral("createBuildingObject"));
        require(window.findChild<QWidget*>(QStringLiteral("toolPanel")) == nullptr &&
                    window.findChild<QToolButton*>(QStringLiteral("toggleProjectPanel")) == nullptr &&
                    sidebar_tabs && sidebar_tabs->count() == 2 &&
                    sidebar_tabs->tabText(0) == QStringLiteral("Layers") &&
                    sidebar_tabs->tabText(1) == QStringLiteral("Symbols") && status_controls &&
                    object_tool && object_tool->toolButtonStyle() == Qt::ToolButtonTextBesideIcon &&
                    object_tool->iconSize() == QSize(16, 16) &&
                    window.findChild<QToolButton*>(QStringLiteral("selectTool")) == nullptr &&
                    window.findChild<QToolButton*>(QStringLiteral("drawFirstBoundary")) == nullptr &&
                    window.findChild<QToolButton*>(QStringLiteral("defineFirstBoundary")) == nullptr,
                "the canvas must have no top tool strip, use Layers/Symbols sidebar tabs, and retain one pointer surface");
        auto* settings = window.findChild<QAction*>(QStringLiteral("keyboardShortcutSettings"));
        auto* user_guide = window.findChild<QAction*>(QStringLiteral("userGuide"));
        auto* about = window.findChild<QAction*>(QStringLiteral("aboutAction"));
        auto* assistance = window.findChild<QAction*>(QStringLiteral("assistanceAction"));
        auto* export_image = window.findChild<QAction*>(QStringLiteral("exportDraftImage"));
        auto* curved_wall_action = window.findChild<QAction*>(QStringLiteral("curvedWall"));
        auto* disto_action = window.findChild<QAction*>(QStringLiteral("distoImport"));
        require(settings && user_guide && about && about->text() == QStringLiteral("About") &&
                    assistance && assistance->text() == QStringLiteral("Assistance…") &&
                    export_image && curved_wall_action && disto_action,
                "shortcut editor, draft image export, DISTO input, curved-wall authoring, and local user guide must be discoverable");
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
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("distoImportDialog"));
            require(dialog, "DISTO import dialog must open from its discoverable action");
            require(dialog->findChild<QPlainTextEdit*>(QStringLiteral("distoPayload")) &&
                        dialog->findChild<QPushButton*>(QStringLiteral("loadDistoJson")) &&
                        dialog->findChild<QPushButton*>(QStringLiteral("applyDistoReading")),
                    "DISTO import dialog must expose local payload and apply controls");
            dialog->reject();
        });
        disto_action->trigger();
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

        const auto curved_revision = window.document().revision();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("curvedWallDialog"));
            require(dialog, "curved-wall authoring dialog must open from More");
            auto* start_x = dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallStartX"));
            auto* start_y = dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallStartY"));
            auto* end_x = dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallEndX"));
            auto* end_y = dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallEndY"));
            auto* construction = dialog->findChild<QComboBox*>(QStringLiteral("curvedWallConstruction"));
            auto* sweep = dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallSweep"));
            auto* measure_label = dialog->findChild<QLabel*>(QStringLiteral("curvedWallMeasureLabel"));
            auto* classification = dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallClassification"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("curvedWallButtons"));
            auto* status = dialog->findChild<QLabel*>(QStringLiteral("curvedWallStatus"));
            require(start_x && start_y && end_x && end_y && construction &&
                        construction->count() == 3 && construction->currentData() == QStringLiteral("angle") &&
                        sweep && measure_label && measure_label->text() == QStringLiteral("Sweep angle") &&
                        classification && buttons && status,
                    "curved-wall dialog must expose endpoint, construction, measure, classification and status controls");
            start_x->setText(QStringLiteral("20 ft"));
            start_y->setText(QStringLiteral("10 ft"));
            end_x->setText(QStringLiteral("30 ft"));
            end_y->setText(QStringLiteral("10 ft"));
            sweep->setText(QStringLiteral("0 deg"));
            buttons->button(QDialogButtonBox::Apply)->click();
            require(dialog->isVisible() && window.document().revision() == curved_revision &&
                        !status->text().isEmpty(),
                    "invalid curved-wall sweep must preserve the document and show a useful error");
            sweep->setText(QStringLiteral("90 deg"));
            classification->setText(QStringLiteral("exterior"));
            if (!capture_directory.isEmpty())
                require(dialog->grab().save(capture_directory + QStringLiteral("/curved-wall.png")),
                        "curved-wall dialog capture");
            buttons->button(QDialogButtonBox::Apply)->click();
        });
        curved_wall_action->trigger();
        const auto curved_wall_id = window.selectedEntityId();
        require(!curved_wall_id.isEmpty() && window.document().revision() == curved_revision + 1,
                "curved-wall dialog must commit one atomic document command");
        const auto curved_wall_document = window.document().snapshot();
        const auto& curved_wall = curved_wall_document.entities().at(curved_wall_id.toStdString());
        require(curved_wall.type == "wall" &&
                    std::abs(curved_wall.properties.at("baseline").at("sweep_radians").get<double>() -
                             std::numbers::pi / 2.0) < 1e-9 &&
                    curved_wall.properties.at("classification") == "exterior" &&
                    curved_wall.extensions.at("curve_input").at("sweep") == "90 deg",
                "curved-wall authoring must retain analytical sweep and source input");
        auto* edit_curve = window.findChild<QPushButton*>(QStringLiteral("editCurvedWall"));
        require(edit_curve && !edit_curve->isHidden() && edit_curve->isEnabled(),
                "selected curved walls must expose a contextual curve editor");
        const auto curved_edit_revision = window.document().revision();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("curvedWallDialog"));
            require(dialog && dialog->windowTitle() == QStringLiteral("Edit curved wall"),
                    "curve editor must open in edit mode for a selected arc");
            dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallEndX"))->setText(QStringLiteral("32 ft"));
            dialog->findChild<QLineEdit*>(QStringLiteral("curvedWallSweep"))->setText(QStringLiteral("-90 deg"));
            dialog->findChild<QDialogButtonBox*>(QStringLiteral("curvedWallButtons"))
                ->button(QDialogButtonBox::Apply)->click();
        });
        edit_curve->click();
        require(window.document().revision() == curved_edit_revision + 1,
                "curved-wall edits must commit one revision-fenced command");
        const auto edited_curved_document = window.document().snapshot();
        const auto& edited_curved_wall = edited_curved_document.entities().at(curved_wall_id.toStdString());
        require(std::abs(edited_curved_wall.properties.at("baseline").at("sweep_radians").get<double>() +
                         std::numbers::pi / 2.0) < 1e-9 &&
                    edited_curved_wall.extensions.at("curve_input").at("sweep") == "-90 deg",
                "curved-wall edits must update analytical geometry and source input");

        const auto arc_length_id = window.createCurvedWallFromConstruction(
            {40, 10}, {41, 10}, QStringLiteral("arc_length"), QStringLiteral("5 ft"),
            QStringLiteral("interior"));
        require(!arc_length_id.isEmpty(), "arc-length curved walls must be authorable through the public command seam");
        const auto arc_length_document = window.document().snapshot();
        const auto& arc_length_wall = arc_length_document.entities().at(arc_length_id.toStdString());
        require(arc_length_wall.extensions.contains("curve_input"), "arc-length wall must retain its curve receipt");
        const auto& arc_length_input = arc_length_wall.extensions.at("curve_input");
        require(arc_length_input.at("construction") == "arc_length" &&
                    arc_length_input.at("measure") == "5 ft" &&
                    arc_length_input.at("clockwise") == false &&
                    std::abs(arc_length_wall.properties.at("baseline").at("sweep_radians").get<double>()) > 1e-7 &&
                    std::abs(arc_length_wall.properties.at("baseline").at("sweep_radians").get<double>()) <
                        2.0 * std::numbers::pi,
                "arc-length construction must retain its defining measure and analytical sweep");
        const auto clockwise_arc_length_id = window.createCurvedWallFromConstruction(
            {50, 10}, {51, 10}, QStringLiteral("arc_length"), QStringLiteral("-5 ft"));
        require(!clockwise_arc_length_id.isEmpty(), "signed arc length must select clockwise construction");
        const auto clockwise_arc_length_document = window.document().snapshot();
        const auto& clockwise_arc_length_wall =
            clockwise_arc_length_document.entities().at(clockwise_arc_length_id.toStdString());
        require(clockwise_arc_length_wall.extensions.contains("curve_input"), "clockwise arc-length wall must retain its curve receipt");
        require(clockwise_arc_length_wall.extensions.at("curve_input").at("clockwise") == true &&
                    clockwise_arc_length_wall.properties.at("baseline").at("sweep_radians").get<double>() < 0.0,
                "negative arc length must retain clockwise orientation");
        const auto arc_height_id = window.createCurvedWallFromConstruction(
            {60, 10}, {64, 10}, QStringLiteral("arc_height"), QStringLiteral("1 ft"));
        require(!arc_height_id.isEmpty(), "arc-height curved walls must be authorable through the public command seam");
        const auto arc_height_document = window.document().snapshot();
        const auto& arc_height_wall = arc_height_document.entities().at(arc_height_id.toStdString());
        require(arc_height_wall.extensions.contains("curve_input"), "arc-height wall must retain its curve receipt");
        require(arc_height_wall.extensions.at("curve_input").at("construction") == "arc_height" &&
                    arc_height_wall.extensions.at("curve_input").at("measure") == "1 ft" &&
                    arc_height_wall.properties.at("baseline").at("sweep_radians").get<double>() > 0.0,
                "arc-height construction must retain its defining measure and signed sweep");

        const auto wall = window.createStraightWall({0, 0}, {4, 0});
        require(!wall.isEmpty() && window.selectEntity(wall), "keypad fixture wall must be selectable");
        auto* keypad = window.findChild<QAction*>(QStringLiteral("measurementKeypad"));
        require(keypad, "precise dimension editor must be discoverable");
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
            require(!dialog->isVisible(),
                    "valid keypad height must apply and close the temporary editor");
            require(window.document().revision() == revision + 1,
                    "valid keypad height must create exactly one document revision");
        });
        keypad->trigger();
        const auto keypad_height = window.document().snapshot().entities().at(wall.toStdString())
                                       .properties.at("height_m").get<double>();
        if (std::abs(keypad_height - 3.0) >= 1e-12)
            std::cerr << "desktop_smoke: observed keypad height " << keypad_height << '\n';
        require(std::abs(keypad_height - 3.0) < 1e-12,
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
        keypad->trigger();
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
        keypad->trigger();

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

void test_second_open_is_read_only() {
    QTemporaryDir directory;
    require(directory.isValid(), "ownership fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("owned-project.bldproj"));
    sketch::desktop::MainWindow owner;
    require(!owner.createStraightWall({0.0, 0.0}, {4.0, 0.0}, QStringLiteral("exterior")).isEmpty(),
            "ownership fixture needs source geometry");
    require(owner.saveProjectAs(path), "ownership fixture should save its source project");

    sketch::desktop::MainWindow second;
    require(second.openProject(path), "a cooperating second session should open the project");
    require(!second.document().is_editable() &&
                second.document().read_only_reason() ==
                    "Another application instance owns this project; it was opened read-only.",
            "a cooperating second session must be explicit read-only");
    require(second.createStraightWall({0.0, 1.0}, {4.0, 1.0}, QStringLiteral("interior")).isEmpty() &&
                second.lastError().contains(QStringLiteral("read-only")),
            "read-only ownership conflicts must reject edits before mutation");
}

void test_plan_canvas_native_pointer_events() {
    sketch::desktop::PlanCanvas canvas;
    canvas.resize(800, 600);
    canvas.setTool(sketch::desktop::CanvasTool::boundary);
    canvas.setSnapEnabled(false);
    int point_clicks = 0;
    canvas.setPointClicked([&](sketch::Vec2) { ++point_clicks; });

    QList<QEventPoint> touch_points;
    touch_points.append(QEventPoint(7, QEventPoint::Pressed, QPointF(120, 130),
                                    QPointF(120, 130)));
    QTouchEvent touch_begin(QEvent::TouchBegin, nullptr, Qt::NoModifier, touch_points);
    QCoreApplication::sendEvent(&canvas, &touch_begin);
    require(touch_begin.isAccepted() && point_clicks == 0,
            "a primary touch press must wait to distinguish a click from navigation");

    touch_points[0] = QEventPoint(7, QEventPoint::Updated, QPointF(120, 130),
                                  QPointF(120, 130));
    QTouchEvent touch_update(QEvent::TouchUpdate, nullptr, Qt::NoModifier, touch_points);
    QCoreApplication::sendEvent(&canvas, &touch_update);
    require(touch_update.isAccepted() && point_clicks == 0,
            "touch motion must update the canvas without creating extra geometry points");

    touch_points[0] = QEventPoint(7, QEventPoint::Released, QPointF(120, 130),
                                  QPointF(120, 130));
    QTouchEvent touch_end(QEvent::TouchEnd, nullptr, Qt::NoModifier, touch_points);
    QCoreApplication::sendEvent(&canvas, &touch_end);
    require(touch_end.isAccepted() && point_clicks == 1,
            "stationary touch release must request one precise point without a duplicate");

    QPointingDevice tablet_device(QStringLiteral("test-tablet"), 91,
                                  QInputDevice::DeviceType::Stylus,
                                  QPointingDevice::PointerType::Pen,
                                  QInputDevice::Capability::Position |
                                      QInputDevice::Capability::Pressure,
                                  1, 1);
    QTabletEvent tablet_press(QEvent::TabletPress, &tablet_device, QPointF(200, 180),
                              QPointF(200, 180), 0.35, 0.0, 0.0, 0.0, 0.0, 0.0,
                              Qt::NoModifier, Qt::LeftButton, Qt::LeftButton);
    QCoreApplication::sendEvent(&canvas, &tablet_press);
    require(tablet_press.isAccepted() && point_clicks == 1,
            "an active-pen press must wait to distinguish a click from navigation");
    QTabletEvent tablet_move(QEvent::TabletMove, &tablet_device, QPointF(200, 180),
                             QPointF(200, 180), 0.05, 0.0, 0.0, 0.0, 0.0, 0.0,
                             Qt::NoModifier, Qt::NoButton, Qt::NoButton);
    QCoreApplication::sendEvent(&canvas, &tablet_move);
    require(tablet_move.isAccepted(), "active-pen motion must be accepted by the canvas");
    QTabletEvent tablet_release(QEvent::TabletRelease, &tablet_device, QPointF(200, 180),
                                QPointF(200, 180), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                Qt::NoModifier, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::sendEvent(&canvas, &tablet_release);
    require(tablet_release.isAccepted() && point_clicks == 2,
            "stationary active-pen release must request one precise point without a duplicate");
    canvas.setTool(sketch::desktop::CanvasTool::select);
    int selection_clicks = 0;
    bool toggle_selection = false;
    canvas.setEntitySelectionClicked([&](QString, bool toggle) {
        ++selection_clicks;
        toggle_selection = toggle;
    });
    QMouseEvent control_press(QEvent::MouseButtonPress, QPointF(200, 200), QPointF(200, 200),
                              Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QMouseEvent control_release(QEvent::MouseButtonRelease, QPointF(200, 200), QPointF(200, 200),
                                Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(&canvas, &control_press);
    QCoreApplication::sendEvent(&canvas, &control_release);
    require(selection_clicks == 1 && toggle_selection,
            "Ctrl-click requests additive toggle selection");
    QMouseEvent plain_press(QEvent::MouseButtonPress, QPointF(200, 200), QPointF(200, 200),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent plain_release(QEvent::MouseButtonRelease, QPointF(200, 200), QPointF(200, 200),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &plain_press);
    QCoreApplication::sendEvent(&canvas, &plain_release);
    require(selection_clicks == 1 && point_clicks == 3,
            "plain empty click with no retained selection must request a drawing point");
}

void test_canvas_symbol_transform_persistence() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto symbol_id = window.createAnnotationSymbol(
        QStringLiteral("sofa"), {0.0, 0.0});
    require(!symbol_id.isEmpty() && window.selectEntity(symbol_id),
            "canvas transform fixture must select a symbol");
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas != nullptr, "canvas transform fixture needs the measurement canvas");
    canvas->resize(800, 600);
    canvas->fitView();
    QApplication::processEvents();
    const auto frame = canvas->selectionBounds();
    require(frame.has_value(), "selected symbol must expose its transform frame");

    const auto before = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    const auto before_symbol = std::find_if(before.symbols.begin(), before.symbols.end(),
        [&](const auto& value) { return value.id == symbol_id.toStdString(); });
    require(before_symbol != before.symbols.end(), "selected symbol must exist before resizing");
    const auto before_scale = before_symbol->placement.scale;
    const auto before_revision = window.document().revision();
    const auto start = frame->bottomRight();
    const auto end = start + QPointF(60.0, 45.0);
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);

    const auto after = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    const auto after_symbol = std::find_if(after.symbols.begin(), after.symbols.end(),
        [&](const auto& value) { return value.id == symbol_id.toStdString(); });
    require(after_symbol != after.symbols.end() &&
                after_symbol->placement.scale > before_scale * 1.05 &&
                window.document().revision() == before_revision + 1,
            "dragging a symbol resize handle must persist one model transform command");
    require(window.undoCommand(), "canvas symbol transform must be undoable");
    const auto restored = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    const auto restored_symbol = std::find_if(restored.symbols.begin(), restored.symbols.end(),
        [&](const auto& value) { return value.id == symbol_id.toStdString(); });
    require(restored_symbol != restored.symbols.end() &&
                std::abs(restored_symbol->placement.scale - before_scale) < 1e-12,
            "undo must restore the symbol scale changed from the canvas");
}

void test_external_project_change_blocks_save() {
    QTemporaryDir directory;
    require(directory.isValid(), "external-change fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("external-change.bldproj"));
    sketch::desktop::MainWindow window;
    require(!window.createStraightWall({0.0, 0.0}, {4.0, 0.0}, QStringLiteral("exterior")).isEmpty() &&
                window.saveProjectAs(path),
            "external-change fixture should save its source project");

    QFile external(path);
    require(external.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "external-change fixture should open the project for mutation");
    require(external.write("external edit") == 13, "external-change fixture should write replacement bytes");
    external.close();

    require(!window.saveProject() && !window.document().is_editable() &&
                window.lastError().contains(QStringLiteral("outside this session")),
            "an external project edit must block save and latch the document read-only");
    QFile verify(path);
    require(verify.open(QIODevice::ReadOnly) && verify.readAll() == QByteArray("external edit"),
            "a blocked save must leave externally changed bytes untouched");
}

void test_workspace_profiles() {
    const auto original_name = QCoreApplication::applicationName();
    const auto original_test_mode = QStandardPaths::isTestModeEnabled();
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-profile-test-") +
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
        auto* fit = window.findChild<QToolButton*>(QStringLiteral("fitViewTool"));
        auto* overview = window.findChild<QToolButton*>(QStringLiteral("overviewMapTool"));
        auto* status_controls = window.findChild<QWidget*>(QStringLiteral("canvasStatusControls"));
        auto* workspace_splitter = window.findChild<QSplitter*>(QStringLiteral("workspaceSplitter"));
        auto* architectural_splitter = window.findChild<QSplitter*>(QStringLiteral("architecturalSplitter"));
        require(grid && snap && fit && overview && status_controls &&
                    grid->parentWidget() == status_controls && snap->parentWidget() == status_controls &&
                    fit->parentWidget() == status_controls && overview->parentWidget() == status_controls &&
                    grid->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                    workspace_splitter && architectural_splitter,
                "profile fixture needs compact status-bar canvas controls and splitter state");
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

void test_room_volume_authoring_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const Boundary boundary = {
        {{0.0, 0.0}, {4.0, 0.0}, 0.0},
        {{4.0, 0.0}, {4.0, 3.0}, 0.0},
        {{4.0, 3.0}, {0.0, 3.0}, 0.0},
        {{0.0, 3.0}, {0.0, 0.0}, 0.0},
    };
    const auto source_id = window.createRoomBoundary(boundary, QStringLiteral("Living room"));
    require(!source_id.isEmpty() && window.selectEntity(source_id),
            "room volume fixture must create and select a closed source boundary");
    const auto before = window.document().revision();
    const auto room_id = window.createRoomVolumeFromSelectedBoundary(
        QStringLiteral("2.4 m"), QStringLiteral("0 m"));
    require(!room_id.isEmpty() && window.document().revision() == before + 1,
            "room volume authoring must publish one undoable command");

    const auto snapshot = window.document().snapshot();
    const auto room = snapshot.entities().find(room_id.toStdString());
    require(room != snapshot.entities().end() && room->second.type == "room" &&
                room->second.properties.at("height_m") == 2.4 &&
                room->second.properties.at("elevation_m") == 0.0 &&
                room->second.properties.at("holes").is_array() &&
                room->second.properties.at("holes").empty() &&
                room->second.properties.at("boundary").is_array(),
            "room volume must persist explicit height, elevation, holes, and analytical boundary");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(room_id.toStdString()) &&
                window.redoCommand() &&
                window.document().snapshot().entities().contains(room_id.toStdString()),
            "room volume authoring must participate in undo and redo");
    require(window.selectEntity(room_id) &&
                window.editSelectedRoomVolume(QStringLiteral("3 m"), QStringLiteral("0.15 m")),
            "room volume edits must validate and commit through the public workflow");
    const auto edited = window.document().snapshot().entities().at(room_id.toStdString());
    require(edited.properties.at("height_m") == 3.0 &&
                edited.properties.at("elevation_m") == 0.15,
            "room volume edits must update the canonical height and elevation fields");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("height_m") == 2.4 &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("height_m") == 3.0,
            "room volume edits must preserve undo and redo semantics");
    auto* elevation_inspector = window.findChild<QLineEdit*>(QStringLiteral("inspectorElevation"));
    require(elevation_inspector != nullptr && !elevation_inspector->isHidden() &&
                elevation_inspector->isEnabled(),
            "room selection must expose an editable base elevation in the contextual inspector");
    require(window.editSelectedHeight(QStringLiteral("3.25 m")) &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("height_m") == 3.25,
            "room height inspector editing must use the shared room-volume validator");
    require(window.editSelectedElevation(QStringLiteral("0.35 m")) &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("elevation_m") == 0.35,
            "room elevation inspector editing must use the shared room-volume validator");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("elevation_m") == 0.15 &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("elevation_m") == 0.35,
            "room elevation inspector editing must preserve undo and redo semantics");
    require(window.editSelectedElevation(QStringLiteral("-0.5 m")) &&
                window.document().snapshot().entities().at(room_id.toStdString())
                    .properties.at("elevation_m") == -0.5,
            "room elevation inspector editing must accept below-grade coordinates");

    const auto before_dimensions = window.document().snapshot();
    constexpr double resized_width = 6.0004;
    constexpr double resized_depth = 2.0007;
    constexpr double resized_area = resized_width * resized_depth;
    require(window.editSelectedRoomVolumeDimensions(
                QStringLiteral("6.0004 m"), QStringLiteral("2.0007 m"),
                QStringLiteral("4 m"), QStringLiteral("0.25 m"),
                RoomFootprintAnchor::center, before_dimensions.revision()),
            "selected room must accept one revision-fenced footprint and vertical dimension edit");
    const auto dimension_snapshot = window.document().snapshot();
    const auto dimension_entity = dimension_snapshot.entities().at(room_id.toStdString());
    RoomVolume dimension_room;
    std::string dimension_error;
    require(read_document_room(dimension_entity, dimension_room, dimension_error) &&
                std::abs(segment_length(dimension_room.boundary[0]) - resized_width) < 1e-9 &&
                std::abs(segment_length(dimension_room.boundary[1]) - resized_depth) < 1e-9 &&
                std::abs(dimension_room.height - 4.0) < 1e-9 &&
                std::abs(dimension_room.elevation - 0.25) < 1e-9 &&
                std::abs(dimension_room.boundary[0].start.x + 1.0002) < 1e-9 &&
                std::abs(dimension_room.boundary[0].start.y - 0.49965) < 1e-9,
            "room dimension edit must retain the original center and local edge directions");
    auto* plan_canvas = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    auto* architectural_canvas = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    auto* architectural_view = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    require(plan_canvas && architectural_canvas && architectural_view,
            "room dimension fixture must expose linked plan, elevation, and section views");
    const auto plan_room = std::find_if(plan_canvas->entities().begin(), plan_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == room_id; });
    require(plan_room != plan_canvas->entities().end() && plan_room->segments.size() == 4 &&
                std::abs(segment_length(plan_room->segments[0]) - resized_width) < 1e-9 &&
                std::abs(segment_length(plan_room->segments[1]) - resized_depth) < 1e-9,
            "the shared plan projection must update from the resized room semantics");
    const auto check_elevation_projection = [&] {
        architectural_view->setCurrentText(QStringLiteral("Elevation"));
        QApplication::processEvents();
        const auto projected = std::find_if(
            architectural_canvas->entities().begin(), architectural_canvas->entities().end(),
            [&](const auto& entity) { return entity.id == room_id; });
        require(projected != architectural_canvas->entities().end() && !projected->segments.empty(),
                "resized room must remain present in every linked vertical projection");
        double min_y = std::numeric_limits<double>::infinity();
        double max_y = -std::numeric_limits<double>::infinity();
        for (const auto& edge : projected->segments) {
            min_y = std::min({min_y, edge.start.y, edge.end.y});
            max_y = std::max({max_y, edge.start.y, edge.end.y});
        }
        require(std::abs(min_y - 0.25) < 1e-9 && std::abs(max_y - 4.25) < 1e-9,
                "linked vertical projections must use the edited room elevation and height");
    };
    check_elevation_projection();
    architectural_view->setCurrentText(QStringLiteral("Section · 1.2 m"));
    QApplication::processEvents();
    const auto section_room = std::find_if(
        architectural_canvas->entities().begin(), architectural_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == room_id; });
    require(section_room != architectural_canvas->entities().end() &&
                !section_room->segments.empty(),
            "the 1.2 m section plane must intersect the edited room volume");
    double section_min_x = std::numeric_limits<double>::infinity();
    double section_max_x = -std::numeric_limits<double>::infinity();
    double section_min_y = std::numeric_limits<double>::infinity();
    double section_max_y = -std::numeric_limits<double>::infinity();
    for (const auto& edge : section_room->segments) {
        section_min_x = std::min({section_min_x, edge.start.x, edge.end.x});
        section_max_x = std::max({section_max_x, edge.start.x, edge.end.x});
        section_min_y = std::min({section_min_y, edge.start.y, edge.end.y});
        section_max_y = std::max({section_max_y, edge.start.y, edge.end.y});
    }
    require(std::abs((section_max_x - section_min_x) - resized_width) < 1e-8 &&
                std::abs((section_max_y - section_min_y) - resized_depth) < 1e-8,
            "the linked section must use the edited room footprint dimensions");

    architectural_view->setCurrentText(QStringLiteral("Plan"));
    const auto schedule = window.scheduleSnapshot();
    const auto room_row = std::find_if(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
        [&](const auto& row) { return row.object_id == room_id.toStdString(); });
    require(room_row != schedule.snapshot.rows.end() &&
                std::abs(std::get<ScheduleQuantity>(
                    room_row->cells.at("gross_area").value).value - resized_area) < 1e-9 &&
                std::abs(std::get<ScheduleQuantity>(
                    room_row->cells.at("volume").value).value - resized_area * 4.0) < 1e-9,
            "room dimensions must update the shared schedule area and volume");
    require(window.undoCommand() &&
                window.document().snapshot().entities() == before_dimensions.entities() &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(room_id.toStdString()) == dimension_entity,
            "room dimension edit must undo and redo as one exact command");
    const auto stable_dimensions = window.document().snapshot();
    require(window.editSelectedRoomVolumeDimensions(
                QStringLiteral("6.0004 m"), QStringLiteral("2.0007 m"),
                QStringLiteral("4 m"), QStringLiteral("0.25 m"),
                RoomFootprintAnchor::center, stable_dimensions.revision()) &&
                window.document().snapshot().revision() == stable_dimensions.revision(),
            "an unchanged room dimension edit must not create document history");
    const bool before_stale_dialog_units = window.metricUnits();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomDimensionDialog"));
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
                                     QStringLiteral("roomDimensionButtons")) : nullptr;
        require(buttons && buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                "an unchanged room dimension dialog must have a valid preview");
        buttons->button(QDialogButtonBox::Apply)->click();
    });
    window.showRoomVolumeDimensions();
    require(window.document().snapshot().revision() == stable_dimensions.revision() &&
                window.document().snapshot().entities() == stable_dimensions.entities(),
            "applying untouched rounded display text must retain exact room geometry and history");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomDimensionDialog"));
        auto* width = dialog ? dialog->findChild<QLineEdit*>(
                                   QStringLiteral("roomDimensionWidth")) : nullptr;
        auto* status = dialog ? dialog->findChild<QLabel*>(
                                    QStringLiteral("roomDimensionStatus")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
                                     QStringLiteral("roomDimensionButtons")) : nullptr;
        require(width && status && buttons, "valid preview cancellation needs dimension controls");
        width->setText(QStringLiteral("7 m"));
        require(buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                    status->text().contains(QStringLiteral("floor area")),
                "a valid room resize must produce a detached calculated preview");
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    window.showRoomVolumeDimensions();
    require(window.document().snapshot().revision() == stable_dimensions.revision() &&
                window.document().snapshot().entities() == stable_dimensions.entities(),
            "canceling a valid live preview must restore exact authoritative room geometry");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomDimensionDialog"));
        require(dialog != nullptr, "room dimension editor must expose one named dialog");
        auto* width = dialog->findChild<QLineEdit*>(QStringLiteral("roomDimensionWidth"));
        auto* status = dialog->findChild<QLabel*>(QStringLiteral("roomDimensionStatus"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("roomDimensionButtons"));
        require(width && status && buttons, "room dimension editor controls must be discoverable");
        width->setText(QStringLiteral("invalid"));
        require(!buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                    status->text().startsWith(QStringLiteral("Preview:")),
                "invalid room dimensions must disable Apply and explain the preview failure");
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    window.showRoomVolumeDimensions();
    require(window.document().snapshot().revision() == stable_dimensions.revision() &&
                window.document().snapshot().entities() == stable_dimensions.entities(),
            "canceling an invalid live preview must leave document history and entities unchanged");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomDimensionDialog"));
        auto* height = dialog ? dialog->findChild<QLineEdit*>(
                                    QStringLiteral("roomDimensionHeight")) : nullptr;
        auto* status = dialog ? dialog->findChild<QLabel*>(
                                    QStringLiteral("roomDimensionStatus")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
                                     QStringLiteral("roomDimensionButtons")) : nullptr;
        require(height && status && buttons, "stale room preview needs dimension controls");
        window.setMetricUnits(!before_stale_dialog_units);
        height->setText(QStringLiteral("15 ft"));
        require(!buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                    status->text().contains(QStringLiteral("changed while the dialog was open")),
                "a changed unit context must stale and disable the room editor");
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    window.showRoomVolumeDimensions();
    window.setMetricUnits(before_stale_dialog_units);
    require(window.document().snapshot().revision() == stable_dimensions.revision() &&
                window.document().snapshot().entities() == stable_dimensions.entities(),
            "a stale room dialog must not mutate geometry or history");
    require(!window.editSelectedRoomVolumeDimensions(
                QStringLiteral("8 m"), QStringLiteral("5 m"),
                QStringLiteral("3 m"), QStringLiteral("0 m"),
                RoomFootprintAnchor::first_corner, before_dimensions.revision()) &&
                window.document().snapshot().revision() == stable_dimensions.revision() &&
                window.document().snapshot().entities() == stable_dimensions.entities(),
            "stale room dimension edit must reject without mutation");

    auto sheet_entity = window.document().snapshot().entities().at("sheet-view-1");
    auto sheet_model = decode_sheet_view_entity(sheet_entity);
    const auto default_section = std::find_if(sheet_model.views().begin(), sheet_model.views().end(),
        [](const auto& view) { return view.id == "view-section"; });
    require(default_section != sheet_model.views().end() &&
                std::abs(default_section->origin_m[2] - 2.4) < 1e-12 &&
                std::abs(default_section->presentation.cut_depth_m - 1.2) < 1e-12,
            "the built-in section must persist a reference origin and relative 1.2 m cut");
    auto translated_section = *default_section;
    translated_section.id = "translated-section";
    translated_section.name = "Translated section";
    translated_section.origin_m = {0.0, 0.0, 5.0};
    translated_section.presentation.cut_depth_m = 0.5;
    auto views = sheet_model.views();
    views.push_back(translated_section);
    sheet_entity.properties["model"] = SheetViewModel::create(
        std::move(views), sheet_model.sheets(), sheet_model.schedule_ids(),
        sheet_model.sheet_order()).to_json();
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(sheet_entity)}, {}, "add translated section fixture"});
    require(window.selectEntity(room_id), "translated section fixture must refresh linked views");
    QApplication::processEvents();
    const auto translated_index = architectural_view->findData(
        QStringLiteral("translated-section"), Qt::UserRole + 1);
    require(translated_index >= 0, "translated named section must be available in the view selector");
    architectural_view->setCurrentIndex(translated_index);
    QApplication::processEvents();
    require(std::none_of(architectural_canvas->entities().begin(),
                         architectural_canvas->entities().end(),
                         [&](const auto& entity) { return entity.id == room_id; }),
            "a translated section cut beyond the room must remain empty instead of moving to a global plane");

    auto holed_room = dimension_entity;
    holed_room.id = "room-with-void";
    holed_room.properties["name"] = "Room with void";
    const nlohmann::json hole = nlohmann::json::array({
        {{"start", {0.0, 1.0}}, {"end", {1.0, 1.0}}, {"sweep_radians", 0.0}},
        {{"start", {1.0, 1.0}}, {"end", {1.0, 2.0}}, {"sweep_radians", 0.0}},
        {{"start", {1.0, 2.0}}, {"end", {0.0, 2.0}}, {"sweep_radians", 0.0}},
        {{"start", {0.0, 2.0}}, {"end", {0.0, 1.0}}, {"sweep_radians", 0.0}},
    });
    holed_room.properties["holes"] = nlohmann::json::array({hole});
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(holed_room)}, {}, "add holed room fixture"});
    require(window.selectEntity(QStringLiteral("room-with-void")) &&
                window.editSelectedRoomVolume(QStringLiteral("4.5 m"), QStringLiteral("0.4 m")),
            "a holed room must allow vertical dimension edits without changing its footprint");
    const auto holed_after = window.document().snapshot().entities().at("room-with-void");
    require(holed_after.properties.at("holes") == nlohmann::json::array({hole}),
            "vertical room edits must retain exact persisted hole geometry");
    const auto holed_plan = std::find_if(plan_canvas->entities().begin(), plan_canvas->entities().end(),
        [](const auto& entity) { return entity.id == QStringLiteral("room-with-void"); });
    require(holed_plan != plan_canvas->entities().end() && holed_plan->segments.size() == 4 &&
                holed_plan->holes.size() == 1 && holed_plan->holes.front().size() == 4,
            "the conventional plan must retain both outer and inner room loops");
    QTemporaryDir room_directory;
    const auto room_path = room_directory.filePath(QStringLiteral("room-dimensions.bldproj"));
    require(room_directory.isValid() && window.saveProjectAs(room_path),
            "room dimension project must save");
    desktop::MainWindow reopened;
    require(reopened.openProject(room_path) &&
                reopened.document().snapshot().entities().at(room_id.toStdString()) == dimension_entity &&
                reopened.document().snapshot().entities().at("room-with-void") == holed_after,
            "room footprint, height, elevation, voids, and metadata must survive save and reopen");
    require(reopened.selectEntity(QStringLiteral("room-with-void")),
            "reopened holed room must remain selectable");
    auto* reopened_plan = dynamic_cast<desktop::PlanCanvas*>(
        reopened.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(reopened_plan != nullptr, "reopened project must expose its measurement plan");
    const auto reopened_holed_plan = std::find_if(
        reopened_plan->entities().begin(), reopened_plan->entities().end(),
        [](const auto& entity) { return entity.id == QStringLiteral("room-with-void"); });
    require(reopened_holed_plan != reopened_plan->entities().end() &&
                reopened_holed_plan->holes.size() == 1,
            "save and reopen must restore the room void to the plan canvas");
}

void test_multiple_selection_clipboard_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto first = window.createStraightWall({0, 0}, {5, 0}, "exterior");
    const auto opening = window.createHostedOpening("door", "1 ft", "3 ft", "0 ft", "7 ft");
    const auto second = window.createStraightWall({0, 3}, {5, 3}, "interior");
    require(!first.isEmpty() && !opening.isEmpty() && !second.isEmpty(), "multi-selection fixture");
    require(window.selectEntity(first) && window.selectEntity(second, true) &&
                window.selectedEntityIds() == QStringList{first, second}, "additive selection preserves order");
    require(window.selectEntity(first, true) && window.selectedEntityIds() == QStringList{second} &&
                window.selectEntity(first, true) && window.selectedEntityIds() == QStringList{second, first},
            "Ctrl selection toggles roots without duplicates");
    require(window.selectEntity(opening, true), "explicit hosted opening joins selection");
    require(window.selectEntity({}, true) && window.selectedEntityIds().size() == 3,
            "Ctrl-click on empty canvas preserves selection");
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas && std::count_if(canvas->entities().begin(), canvas->entities().end(),
                [&](const auto& entity) { return entity.selected && (entity.id == first || entity.id == second); }) == 2,
            "both selected walls are highlighted on the canvas");
    const auto revision = window.document().revision();
    require(window.copySelection() && window.document().revision() == revision, "combined copy is read only");
    const auto payload = nlohmann::json::parse(QGuiApplication::clipboard()->text().toStdString());
    require(payload.at("root_ids").size() == 3 && payload.at("entities").size() == 3,
            "combined clipboard deduplicates explicitly selected hosted dependencies");
    require(window.pasteSelection() && window.document().revision() == revision + 1,
            "combined paste is exactly one revision");
    const auto roots = window.selectedEntityIds();
    const auto pasted = window.document().snapshot();
    require(roots.size() == 3 && roots[0] != second && roots[1] != first && roots[2] != opening &&
                pasted.entities().at(roots[2].toStdString()).properties.at("wall_id") == roots[1].toStdString(),
            "paste selects all fresh roots in order and remaps the hosted link");
    require(window.undoCommand() && !window.document().snapshot().entities().contains(roots[0].toStdString()) &&
                !window.document().snapshot().entities().contains(roots[1].toStdString()) && window.redoCommand(),
            "one undo and redo covers the complete pasted selection");
    require(window.selectEntity(roots[0]) && window.selectEntity(roots[1], true), "select both pasted walls");
    const auto cut_revision = window.document().revision();
    require(window.cutSelection() && window.document().revision() == cut_revision + 1 &&
                !window.document().snapshot().entities().contains(roots[2].toStdString()) &&
                window.selectedEntityIds().isEmpty() && window.undoCommand(),
            "cut atomically removes both walls and their opening and undo restores them");
    require(window.selectEntity(first) && window.selectEntity(second, true), "select originals for delete");
    const auto delete_revision = window.document().revision();
    require(window.deleteSelection() && window.document().revision() == delete_revision + 1 &&
                !window.document().snapshot().entities().contains(first.toStdString()) &&
                !window.document().snapshot().entities().contains(second.toStdString()) &&
                !window.document().snapshot().entities().contains(opening.toStdString()) &&
                window.undoCommand() && window.redoCommand(), "combined delete is one undoable graph command");
    require(window.selectEntity(roots[0]) && window.selectEntity("property-1", true), "mixed unsupported selection");
    const auto rejected_revision = window.document().revision();
    const auto clipboard_before = QGuiApplication::clipboard()->text();
    require(!window.cutSelection() && !window.deleteSelection() && window.document().revision() == rejected_revision &&
                QGuiApplication::clipboard()->text() == clipboard_before &&
                window.document().snapshot().entities().contains(roots[0].toStdString()),
            "unsupported mixed selections reject the complete operation without partial mutation");
    require(window.selectEntity(roots[0]) && window.selectedEntityIds() == QStringList{roots[0]} &&
                window.selectEntity({}) && window.selectedEntityIds().isEmpty(),
            "ordinary replacement and empty selection retain single-selection behavior");
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

    auto single_root_payload = nlohmann::json::parse(clipboard_text.toStdString());
    single_root_payload.erase("root_ids");
    QGuiApplication::clipboard()->setText(QString::fromStdString(single_root_payload.dump()));

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

void test_wall_transform_workflow(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id=window.createStraightWall({0,0},{4,0});
    require(window.selectEntity(wall_id),"select transform wall");
    const auto opening_id=window.createHostedOpening("door","1 m","1 m","0 m","2 m");
    auto opening=window.document().snapshot().entities().at(opening_id.toStdString());
    opening.properties["door_operation"]={{"version",1},{"hinge","start"},{"side","left"},{"angle_degrees",90}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(opening)},{},"door transform fixture"});
    require(window.selectEntity(wall_id),"reselect transform wall");
    const auto before=window.document().snapshot();
    require(window.transformSelectedBoundary("90",false,false,"1 m","3 m",false),
        "wall rotation and translation must use the selection transform command");
    const auto rotated=window.document().snapshot();
    const auto& baseline=rotated.entities().at(wall_id.toStdString()).properties.at("baseline");
    require(std::abs(baseline.at("start")[0].get<double>()-3)<1e-9 &&
        std::abs(baseline.at("start")[1].get<double>()-1)<1e-9 &&
        std::abs(baseline.at("end")[0].get<double>()-3)<1e-9 &&
        std::abs(baseline.at("end")[1].get<double>()-5)<1e-9 &&
        rotated.entities().at(opening.id)==opening,
        "rigid wall motion must preserve dimensions and hosted opening coordinates");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==rotated.entities(),
        "wall transform must undo and redo exactly");
    require(window.transformSelectedBoundary("0",true,false,"0","0",false),"mirror wall graph");
    require(window.document().snapshot().entities().at(opening.id).properties.at("door_operation").at("side")=="right",
        "reflection must mirror hosted door swing side");
    const auto mirrored=window.document().snapshot();
    require(window.transformSelectedBoundary("0",false,false,"5 m","0",true),"clone transformed wall with openings");
    const auto clone_id=window.selectedEntityId().toStdString();
    const auto cloned=window.document().snapshot();
    require(clone_id!=wall_id.toStdString() && cloned.entities().at(wall_id.toStdString())==mirrored.entities().at(wall_id.toStdString()),
        "wall clone must preserve source geometry");
    bool hosted=false;
    for(const auto& [id,entity]:cloned.entities()) if(entity.type=="opening" && entity.properties.value("wall_id","")==clone_id) {
        require(id!=opening.id && entity.properties.at("offset_m")==opening.properties.at("offset_m") &&
            entity.properties.at("door_operation").at("side")=="right","clone must preserve local opening geometry and handing");
        hosted=true;
    }
    require(hosted && window.undoCommand() && window.document().snapshot().entities()==mirrored.entities(),
        "wall clone and hosted openings must be one undoable command");
    require(window.selectEntity(wall_id),"select original after undo");
    const auto vertical=encode_constraint_entity({"transform-vertical",ConstraintRelationKind::vertical,
        {{wall_id.toStdString(),WallEndpointRole::start},{wall_id.toStdString(),WallEndpointRole::end}}});
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(vertical)},{},"lock vertical"});
    const auto constrained=window.document().snapshot();
    require(!window.transformSelectedBoundary("45",false,false,"0","0",false) &&
        window.document().snapshot().entities()==constrained.entities() && window.document().revision()==constrained.revision(),
        "incompatible rigid transform must preserve hard constraints and reject without mutation");
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
        dialog->findChild<QLineEdit*>("boundaryRotationDegrees")->setText("45");
        require(!dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply)->isEnabled() &&
            !dialog->findChild<QLabel*>("boundaryTransformStatus")->text().isEmpty() &&
            window.document().snapshot().entities()==constrained.entities(),
            "live preview must expose constraint conflicts without modifying the document");
        dialog->reject();
    });
    window.showBoundaryTransformEditor();
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::erase(vertical.id)},{},"remove test constraint"});
    auto curved=window.document().snapshot().entities().at(wall_id.toStdString());
    curved.properties["baseline"]["sweep_radians"]=0.75;
    curved.extensions["curve_input"] = {{"version", 1}, {"start", {3.0, 1.0}},
        {"end", {3.0, 5.0}}, {"sweep", "0.75"}, {"normalized_sweep", "0.75"},
        {"radians", 0.75}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(curved)},{},"curved transform fixture"});
    require(window.transformSelectedBoundary("30",true,false,"2 m","1 m",false) &&
        window.document().snapshot().entities().at(wall_id.toStdString()).properties.at("baseline").at("sweep_radians")==-0.75 &&
        window.document().snapshot().entities().at(wall_id.toStdString()).extensions.at("curve_input").at("radians")==-0.75 &&
        window.document().snapshot().entities().at(wall_id.toStdString()).extensions.at("curve_input").at("sweep")=="-0.75",
        "curved wall reflection must reverse analytical arc orientation");
    QTemporaryDir stored;
    const auto path=stored.filePath("transformed-wall.bldproj");
    require(stored.isValid() && window.saveProjectAs(path),"save transformed wall project");
    desktop::MainWindow reopened;
    require(reopened.openProject(path) &&
        reopened.document().snapshot().entities()==window.document().snapshot().entities(),
        "transformed walls and hosted openings must survive project save and reopen");
    const auto before_preview=window.document().snapshot();
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
        require(dialog && dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply)->isEnabled(),
            "transform editor must enable wall selection");
        auto* preview=dynamic_cast<desktop::PlanCanvas*>(dialog->findChild<QWidget*>("wallTransformPreview"));
        require(preview && preview->entities().size()==4,"wall preview must show original and proposed wall/opening graphs");
        auto* x=dialog->findChild<QLineEdit*>("boundaryOffsetX");
        x->setText("invalid");
        require(preview->entities().empty() &&
            !dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply)->isEnabled(),
            "invalid transform must clear stale preview and disable Apply");
        x->setText("3 m");
        dialog->findChild<QLineEdit*>("boundaryRotationDegrees")->setText("45");
        require(preview->entities().size()==4 && window.document().snapshot().entities()==before_preview.entities() &&
            window.document().revision()==before_preview.revision(),"live transform preview must not mutate document or history");
        if(!capture_directory.isEmpty()) {
            QDir().mkpath(capture_directory);
            require(dialog->grab().save(capture_directory+"/wall-transform.png"),"save wall transform editor capture");
        }
        dialog->reject();
    });
    window.showBoundaryTransformEditor();
    require(window.document().snapshot().entities()==before_preview.entities() && window.document().revision()==before_preview.revision(),
        "cancelling transform preview must leave the document unchanged");
    QString preview_root;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
        dialog->findChild<QCheckBox*>("boundaryClone")->setChecked(true);
        dialog->findChild<QLineEdit*>("boundaryOffsetX")->setText("4 m");
        auto* preview=dynamic_cast<desktop::PlanCanvas*>(dialog->findChild<QWidget*>("wallTransformPreview"));
        for(const auto& entity:preview->entities()) if(entity.type=="wall" && entity.selected) preview_root=entity.id;
        require(!preview_root.isEmpty() && preview_root!=wall_id,"copy preview must allocate its own wall identity");
        dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply)->click();
    });
    window.showBoundaryTransformEditor();
    require(window.selectedEntityId()==preview_root && window.document().revision()==before_preview.revision()+1 &&
        window.undoCommand() && window.document().snapshot().entities()==before_preview.entities(),
        "Apply must commit the exact preview identity once and remain undoable");
    require(window.selectEntity(wall_id),"select wall for stale preview test");
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
        dialog->findChild<QLineEdit*>("boundaryOffsetX")->setText("4 m");
        require(!window.createStraightWall({20,20},{21,20}).isEmpty(),"intervening edit fixture");
        const auto changed=window.document().snapshot();
        auto* apply=dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply);
        apply->click();
        auto* preview=dynamic_cast<desktop::PlanCanvas*>(dialog->findChild<QWidget*>("wallTransformPreview"));
        require(!apply->isEnabled() && preview->entities().empty() &&
            window.document().snapshot().entities()==changed.entities() && window.document().revision()==changed.revision(),
            "stale preview must clear and refuse Apply without overwriting intervening edits");
        dialog->reject();
    });
    window.showBoundaryTransformEditor();
}

void test_sloped_wall_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id = window.createSlopedWall({0.0, 0.0}, {4.0, 0.0}, "1 m");
    require(!wall_id.isEmpty(), "sloped wall creation must commit");
    auto wall = window.document().snapshot().entities().at(wall_id.toStdString());
    require(wall.properties.at("slope_rise_m") == 1.0,
            "sloped wall creation must persist the signed rise");
    require(window.selectEntity(wall_id), "select sloped wall for hosted opening");
    const auto opening_id = window.createHostedOpening(
        "window", "0.5 m", "1 m", "0 m", "2 m");
    require(!opening_id.isEmpty(), "sloped wall must host a fitting opening");
    require(window.selectEntity(wall_id) && window.editSelectedWallSlope("-0.5 m"),
            "sloped wall edit must validate hosted openings and commit");
    const auto changed = window.document().snapshot();
    require(changed.entities().at(wall_id.toStdString()).properties.at("slope_rise_m") == -0.5,
            "sloped wall edit must replace the signed rise");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(wall_id.toStdString()).properties.at("slope_rise_m") == 1.0 &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(wall_id.toStdString()).properties.at("slope_rise_m") == -0.5,
            "sloped wall slope edit must undo and redo atomically");
    QTemporaryDir stored;
    const auto path = stored.filePath("sloped-wall.bldproj");
    require(stored.isValid() && window.saveProjectAs(path),
            "sloped wall project must save");
    desktop::MainWindow reopened;
    require(reopened.openProject(path), "sloped wall project must reopen");
    require(reopened.document().snapshot().entities() == changed.entities(),
            "sloped wall and hosted opening must preserve semantics through reopen");
}

void test_material_clipboard_transfer() {
    using namespace sketch;
    desktop::MainWindow source;
    const auto wall_id=source.createStraightWall({0,0},{5,0});
    auto catalog=Entity::create("assembly_model",{{"version",1},{"model",
        AssemblyModel::create({{wall_id.toStdString(),wall_id.toStdString(),"#abcdef"},{"unused","Unused"}}, {}, {}).to_json()}});
    catalog.id="transfer-catalog";
    auto wall=source.document().snapshot().entities().at(wall_id.toStdString());
    wall.properties["name"]=wall_id.toStdString();
    wall.properties["description"]=wall_id.toStdString();
    wall.properties["custom_metadata"]={{"entity_id",wall_id.toStdString()}};
    wall.extensions={{"note",wall_id.toStdString()},{"entity_id",wall_id.toStdString()}};
    wall.properties["material_assignment"]={{"version",1},{"catalog_id",catalog.id},{"material_id",wall_id.toStdString()}};
    source.document().apply(ApplyEntityChanges{source.document().revision(),
        {EntityChange::upsert(catalog),EntityChange::upsert(wall)},{},"assign transfer material"});
    require(source.selectEntity(wall_id) && source.copySelection(),"copy assigned wall");
    catalog.properties["model"]=AssemblyModel::create({{wall_id.toStdString(),"Changed","#112233"},{"unused","Unused"}}, {}, {}).to_json();
    source.document().apply(ApplyEntityChanges{source.document().revision(),{EntityChange::upsert(catalog)},{},"change after copy"});
    desktop::MainWindow target;
    target.document().apply(ApplyEntityChanges{target.document().revision(),{EntityChange::upsert(catalog)},{},"conflicting destination catalog"});
    const auto existing=target.createStraightWall({0,2},{3,2});
    require(target.selectEntity(existing),"destination selection fixture");
    const auto before=target.document().snapshot();
    require(target.pasteSelection(),"cross-project paste must carry materials and use clipboard root despite destination selection");
    const auto pasted_id=target.selectedEntityId().toStdString();
    const auto pasted=target.document().snapshot();
    const auto& pasted_wall=pasted.entities().at(pasted_id);
    require(pasted_wall.properties.at("name")==wall.properties.at("name") &&
        pasted_wall.properties.at("description")==wall.properties.at("description") &&
        pasted_wall.properties.at("custom_metadata")==wall.properties.at("custom_metadata") &&
        pasted_wall.extensions==wall.extensions,
        "clipboard identity changes must preserve ordinary text and opaque metadata even when matching IDs");
    const auto& assignment=pasted.entities().at(pasted_id).properties.at("material_assignment");
    const auto imported_catalog=assignment.at("catalog_id").get<std::string>();
    require(imported_catalog!=catalog.id,"conflicting catalog identity must not overwrite destination materials");
    const auto imported=AssemblyModel::from_json(pasted.entities().at(imported_catalog).properties.at("model"));
    require(imported.materials().size()==1 && imported.materials()[0].id==wall_id.toStdString() &&
        imported.materials()[0].name==wall_id.toStdString() && imported.materials()[0].color_srgb=="#abcdef" &&
        assignment.at("material_id")==wall_id.toStdString(),
        "material subset must preserve captured color and internal IDs/names even when matching an entity ID");
    require(target.undoCommand() && target.document().snapshot().entities()==before.entities() && target.redoCommand(),
        "material dependency transfer must be one undoable paste");
    require(target.selectEntity(QString::fromStdString(pasted_id)) && target.copySelection(),"copy pasted object");
    auto legacy=nlohmann::json::parse(QGuiApplication::clipboard()->text().toStdString());
    legacy.erase("root_ids");
    legacy.erase("root_id");
    QGuiApplication::clipboard()->setText(QString::fromStdString(legacy.dump()));
    require(target.pasteSelection(),"legacy-root same-project material paste must remain available");
    const auto second=target.document().snapshot().entities().at(target.selectedEntityId().toStdString());
    require(second.properties.at("material_assignment").at("catalog_id")==imported_catalog,
        "same-project paste should reuse an unchanged referenced catalog");
    require(source.selectEntity(wall_id) && source.cutSelection() &&
        source.document().snapshot().entities().contains(catalog.id),
        "cutting assigned geometry must preserve its shared material catalog");

    const auto boundary_id=source.createBoundary(
        Boundary{{{{0,0},{4,0},0},{{4,0},{4,2},0},{{4,2},{0,2},0},{{0,2},{0,0},0}}},
        QStringLiteral("measurement"));
    const auto boundary=decode_identified_boundary_entity(source.document().snapshot().entities().at(boundary_id.toStdString()));
    auto dimension=encode_boundary_dimension_entity({"clipboard-dimension",boundary.id,
        boundary.segments.front().segment_id,{2,-1}});
    dimension.properties["target"]["description"]=boundary.id;
    source.document().apply(ApplyEntityChanges{source.document().revision(),{EntityChange::upsert(dimension)},{},"clipboard dimension"});
    require(source.selectEntity(boundary_id) && source.copySelection() && target.pasteSelection(),
        "identified boundary and dimension paste");
    const auto copied_snapshot=target.document().snapshot();
    const auto& copied_boundary=copied_snapshot.entities().at(target.selectedEntityId().toStdString());
    bool resolved_dimension=false;
    for(const auto& [id,entity]:copied_snapshot.entities()) {
        if(entity.type!="dimension") continue;
        const auto decoded=decode_boundary_dimension_entity(entity);
        if(!decoded.dimension || decoded.dimension->boundary_id!=copied_boundary.id) continue;
        require(decoded.dimension->segment_id!=boundary.segments.front().segment_id &&
            std::abs(decoded.dimension->resolve(copied_boundary).segment_length()-4.0)<1e-9 &&
            entity.properties.at("target").at("description")==boundary.id,
            "dimension references must follow fresh segment identities while target metadata stays unchanged");
        resolved_dimension=true;
    }
    require(resolved_dimension,"copied dimension must target copied boundary");

    AnnotationState state;
    auto label=instantiate_label(default_label_templates().front(),"clipboard-label");
    label.content=label.id;
    state.labels.push_back(label);
    auto annotation=make_annotation_entity("clipboard-annotations",state);
    annotation.extensions={{"note",label.id}};
    source.document().apply(ApplyEntityChanges{source.document().revision(),{EntityChange::upsert(annotation)},{},"clipboard annotation"});
    require(source.selectEntity(QString::fromStdString(label.id)) && source.copySelection() && target.pasteSelection(),
        "annotation clipboard transfer");
    const auto copied_annotation=target.document().snapshot().entities().at(target.selectedEntityId().toStdString());
    const auto copied_state=decode_annotation_entity(copied_annotation);
    require(copied_state.labels.front().id!=label.id && copied_state.labels.front().content==label.content &&
        copied_state.labels.front().template_id==label.template_id && copied_annotation.extensions==annotation.extensions,
        "annotation instance identity must change without rewriting label content, template or extensions");
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
    auto original = window.document().snapshot().entities().at(boundary_id.toStdString());
    const auto identified = decode_identified_boundary_entity(original);
    original.properties["name"]=original.id;
    original.extensions={{"note",identified.segments.front().segment_id}};
    original.properties["segments"][0]["edge_note"]={{"text",identified.segments.front().segment_id}};
    original.properties["segments"][1]["edge_note"]={{"text","adjacent edge"}};
    auto dimension=encode_boundary_dimension_entity({"insertion-dimension",original.id,
        identified.segments.front().segment_id,{2,-1}});
    dimension.properties["target"]["description"]=original.id;
    AnnotationState annotations;
    auto label=instantiate_label(default_label_templates().front(),"insertion-label");
    label.content=original.id;
    annotations.labels.push_back(label);
    annotations.overrides.push_back({"area",original.id,{},true});
    auto annotation=make_annotation_entity("insertion-annotations",annotations);
    auto phases=Entity::create("model_phases",{{"version",1},{"model",
        ModelPhases::create({original.id},{original.id},{{original.id,original.id,{original.id},{}}}).to_json()}});
    auto relationships=Entity::create("room_relationships",{{"version",1},{"model",
        RoomRelationshipSnapshot::create({{original.id,RoomReferenceKind::appraisal_measurement_boundary}},{}).to_json()}});
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(original),EntityChange::upsert(dimension),EntityChange::upsert(annotation),
         EntityChange::upsert(phases),EntityChange::upsert(relationships)},
        {},"insertion references"});
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
    const auto after_insertion=window.document().snapshot();
    const auto& inserted_entity=after_insertion.entities().at(inserted.id);
    const auto migrated_annotation=decode_annotation_entity(after_insertion.entities().at(annotation.id));
    const auto& migrated_dimension=after_insertion.entities().at(dimension.id);
    require(inserted_entity.properties.at("name")==original.id && inserted_entity.extensions==original.extensions &&
        migrated_annotation.labels.front().content==original.id &&
        migrated_dimension.properties.at("target").at("description")==original.id,
        "boundary insertion must preserve matching text and opaque metadata");
    require(inserted_entity.properties.at("segments").at(0).at("edge_note")==
            original.properties.at("segments").at(0).at("edge_note") &&
        !inserted_entity.properties.at("segments").at(1).contains("edge_note") &&
        inserted_entity.properties.at("segments").at(2).at("edge_note")==
            original.properties.at("segments").at(1).at("edge_note"),
        "insertion must retain metadata on the continuing first piece and unaffected edges without duplicating it");
    const auto resolved=decode_boundary_dimension_entity(migrated_dimension).dimension;
    require(migrated_annotation.overrides.front().target_id==inserted.id && resolved &&
        resolved->boundary_id==inserted.id && std::abs(resolved->resolve(inserted_entity).segment_length()-2.0)<1e-9,
        "boundary insertion must retain semantic annotation and dimension links");
    const auto migrated_phases=ModelPhases::from_json(after_insertion.entities().at(phases.id).properties.at("model"));
    const auto migrated_relationships=RoomRelationshipSnapshot::from_json(
        after_insertion.entities().at(relationships.id).properties.at("model"));
    require(migrated_phases.entity_ids()==std::vector<std::string>{inserted.id} &&
        migrated_phases.baseline_ids()==std::vector<std::string>{inserted.id} &&
        migrated_phases.alternatives().front().demolished_ids==std::vector<std::string>{inserted.id} &&
        migrated_phases.alternatives().front().id==original.id &&
        migrated_phases.alternatives().front().name==original.id &&
        migrated_relationships.references().front().id==inserted.id,
        "boundary insertion must migrate nested model memberships without changing alternative IDs or names");
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
    auto receipt_bound=window.document().snapshot().entities().at(inserted_id.toStdString());
    receipt_bound.properties["segments"][0]["receipt"]={{"version",99}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(receipt_bound)},{},"unsupported directional receipt"});
    const auto receipt_snapshot=window.document().snapshot();
    require(!window.insertSelectedBoundaryVertex(QString::fromStdString(inserted.segments.front().segment_id),
                QStringLiteral("0.5")) && window.document().snapshot().entities()==receipt_snapshot.entities() &&
        window.document().revision()==receipt_snapshot.revision(),
        "insertion must reject an unhandled edge receipt without discarding it or changing the document");
}

void test_direct_boundary_geometry_edit_workflow() {
    using namespace sketch;
    BoundaryAuthoringOptions options;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
    (void)session.anchor({0, 0});
    (void)session.add_line_rise_run(parse_quantity("0 m", Unit::metre),
                                    parse_quantity("4 m", Unit::metre));
    (void)session.add_line_rise_run(parse_quantity("3 m", Unit::metre),
                                    parse_quantity("0 m", Unit::metre));
    (void)session.add_line_rise_run(parse_quantity("0 m", Unit::metre),
                                    parse_quantity("-4 m", Unit::metre));
    (void)session.add_line_rise_run(parse_quantity("-3 m", Unit::metre),
                                    parse_quantity("0 m", Unit::metre));
    session.classify_current_chain("living");
    const auto accepted = session.close_chain();
    auto boundary_entity = encode_identified_boundary_entity(accepted.boundary);
    boundary_entity.properties["classification"] = "living";
    boundary_entity.properties["boundary_authoring"] =
        boundary_construction_envelope(accepted, options);
    const auto original_receipt = boundary_entity.properties.at("boundary_authoring");
    const auto& first = accepted.boundary.segments[0];
    const auto& second = accepted.boundary.segments[1];
    auto length_dimension = encode_boundary_dimension_entity(BoundaryDimension{
        "direct-length", accepted.boundary.id, first.segment_id, {2, -0.5}});
    BoundaryDimension angle;
    angle.id = "direct-angle";
    angle.boundary_id = accepted.boundary.id;
    angle.segment_id = first.segment_id;
    angle.secondary_segment_id = second.segment_id;
    angle.vertex_id = first.end_vertex_id;
    angle.text_position = {4.4, 0.4};
    angle.kind = BoundaryDimensionKind::angle;
    auto angle_dimension = encode_boundary_dimension_entity(angle);

    desktop::MainWindow window;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(boundary_entity), EntityChange::upsert(length_dimension),
         EntityChange::upsert(angle_dimension)}, {}, "direct boundary fixture"});
    const auto id = QString::fromStdString(accepted.boundary.id);
    require(window.selectEntity(id), "direct-edit boundary fixture must be selectable");
    const auto before = window.document().revision();
    require(window.moveSelectedBoundaryVertex(
                QString::fromStdString(first.end_vertex_id), {5, 0.5}, before) &&
                window.document().revision() == before + 1,
            "canvas vertex edit must commit one revision through the typed command");
    auto moved_entity = window.document().snapshot().entities().at(accepted.boundary.id);
    auto moved = decode_identified_boundary_entity(moved_entity);
    require(moved.segments[0].segment.end.x == 5 && moved.segments[0].segment.end.y == 0.5 &&
                moved.segments[1].segment.start.x == 5 && moved.segments[1].segment.start.y == 0.5 &&
                moved.segments[0].segment_id == first.segment_id &&
                moved.segments[0].end_vertex_id == first.end_vertex_id,
            "vertex edit must update both incident edges while retaining stable identities");
    require(!moved_entity.properties.contains("boundary_authoring") &&
                moved_entity.extensions.at("boundary_geometry_derivation")
                    .at("source_boundary_authoring") == original_receipt &&
                moved_entity.extensions.at("boundary_geometry_derivation")
                    .at("operations").size() == 1,
            "manual editing must archive exact construction evidence and append replayable intent");
    const auto moved_snapshot = window.document().snapshot();
    require(decode_boundary_dimension_entity(moved_snapshot.entities().at("direct-length"))
                    .dimension->resolve(moved_entity).segment_length() > 5.0 &&
                std::isfinite(decode_boundary_dimension_entity(
                    moved_snapshot.entities().at("direct-angle")).dimension->resolve(moved_entity).angle()),
            "length and angle dimensions must remain resolvable through coordinate edits");

    const auto after_move = window.document().revision();
    require(window.editSelectedBoundaryEdgeLength(
                QString::fromStdString(first.segment_id), QStringLiteral("6 m"),
                BoundaryFixedEndpoint::start, false, after_move),
            "selected boundary edge must accept an explicit target length and fixed endpoint");
    const auto resized_entity = window.document().snapshot().entities().at(accepted.boundary.id);
    const auto resized = decode_identified_boundary_entity(resized_entity);
    require(std::abs(segment_length(resized.segments[0].segment) - 6.0) < 1e-9 &&
                resized_entity.extensions.at("boundary_geometry_derivation")
                    .at("operations").size() == 2,
            "edge length edit must be analytical and extend its replayable derivation");
    require(window.undoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    accepted.boundary.id)) == moved &&
                window.redoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    accepted.boundary.id)) == resized,
            "direct boundary edits must undo and redo exactly");

    const auto stable = window.document().snapshot();
    require(!window.moveSelectedBoundaryVertex(
                QString::fromStdString(first.start_vertex_id), {-1, 0}, after_move) &&
                window.document().snapshot().revision() == stable.revision() &&
                window.document().snapshot().entities() == stable.entities(),
            "stale handle release must reject without mutating the document");
    require(!window.moveSelectedBoundaryVertex(
                QString::fromStdString(first.end_vertex_id), {0, 0}) &&
                window.document().snapshot().entities() == stable.entities(),
            "degenerate vertex edits must reject atomically");

    require(window.transformSelectedBoundary(QStringLiteral("10"), false, false,
                QStringLiteral("1 m"), QStringLiteral("0 m"), false),
            "a directly edited receipt-backed boundary must remain transformable");
    const auto transformed_entity = window.document().snapshot().entities().at(
        accepted.boundary.id);
    const auto transformed = decode_identified_boundary_entity(transformed_entity);
    require(transformed != resized &&
                transformed_entity.extensions.at("boundary_geometry_derivation")
                    .at("operations").size() == 3,
            "the typed transform must append to the ordered boundary derivation");
    const auto transformed_vertex = transformed.segments[1].segment.start;
    require(window.moveSelectedBoundaryVertex(
                QString::fromStdString(first.end_vertex_id),
                {transformed_vertex.x + 0.1, transformed_vertex.y + 0.1},
                window.document().revision()),
            "a boundary must remain directly editable after a typed transform");
    const auto final_boundary = decode_identified_boundary_entity(
        window.document().snapshot().entities().at(accepted.boundary.id));
    require(window.undoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    accepted.boundary.id)) == transformed &&
                window.redoCommand() &&
                decode_identified_boundary_entity(window.document().snapshot().entities().at(
                    accepted.boundary.id)) == final_boundary,
            "edit-transform-edit history must undo and redo exactly");

    const auto source_before_clone = window.document().snapshot().entities().at(
        accepted.boundary.id);
    const auto derived_clone_ok = window.transformSelectedBoundary(
        QStringLiteral("0"), false, false,
        QStringLiteral("2 m"), QStringLiteral("1 m"), true);
    if (!derived_clone_ok)
        std::cerr << "derived boundary clone error: "
                  << window.lastError().toStdString() << '\n';
    require(derived_clone_ok,
            "a directly edited boundary must support a transformed copy");
    const auto clone_id = window.selectedEntityId().toStdString();
    const auto clone_snapshot = window.document().snapshot();
    require(clone_id != accepted.boundary.id &&
                clone_snapshot.entities().at(accepted.boundary.id) == source_before_clone,
            "derived-boundary cloning must preserve the source exactly");
    const auto& clone_entity = clone_snapshot.entities().at(clone_id);
    const auto cloned_boundary = decode_identified_boundary_entity(clone_entity);
    require(cloned_boundary.segments.size() == final_boundary.segments.size(),
            "derived-boundary clone must preserve topology");
    for (std::size_t index = 0; index < cloned_boundary.segments.size(); ++index) {
        const auto& source_edge = final_boundary.segments[index];
        const auto& cloned_edge = cloned_boundary.segments[index];
        require(cloned_edge.segment_id != source_edge.segment_id &&
                    cloned_edge.start_vertex_id != source_edge.start_vertex_id &&
                    std::abs(cloned_edge.segment.start.x - source_edge.segment.start.x - 2.0) < 1e-9 &&
                    std::abs(cloned_edge.segment.start.y - source_edge.segment.start.y - 1.0) < 1e-9 &&
                    std::abs(cloned_edge.segment.end.x - source_edge.segment.end.x - 2.0) < 1e-9 &&
                    std::abs(cloned_edge.segment.end.y - source_edge.segment.end.y - 1.0) < 1e-9,
                "derived-boundary clone must use fresh IDs and the requested transform");
    }
    const auto& clone_derivation =
        clone_entity.extensions.at("boundary_geometry_derivation");
    require(clone_derivation.at("operations").size() == 5,
            "derived-boundary clone must retain remapped edit history and append its transform");
    for (const auto& operation : clone_derivation.at("operations")) {
        require(operation.at("value").at("boundary_id") == clone_id,
                "every cloned derivation operation must target the cloned boundary");
    }
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(clone_id) &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(clone_id) == clone_entity,
            "derived-boundary cloning must undo and redo exactly");

    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("boundary-edit.bldproj"));
    require(directory.isValid() && window.saveProjectAs(path),
            "typed boundary edit history must save");
    desktop::MainWindow reopened;
    require(reopened.openProject(path), "typed boundary edit project must reopen");
    const auto reopened_snapshot = reopened.document().snapshot();
    require(decode_identified_boundary_entity(reopened_snapshot.entities().at(
                accepted.boundary.id)) == final_boundary,
            "typed boundary edit geometry must survive save and reopen");
    require(reopened_snapshot.entities().at(clone_id) == clone_entity,
            "a derived-boundary clone and its remapped proof must survive save and reopen");
    const auto persisted_edits = std::count_if(
        reopened_snapshot.history().begin(), reopened_snapshot.history().end(),
        [](const RevisionRecord& record) { return record.boundary_geometry_edit.has_value(); });
    const auto persisted_transforms = std::count_if(
        reopened_snapshot.history().begin(), reopened_snapshot.history().end(),
        [](const RevisionRecord& record) { return record.boundary_transform.has_value(); });
    require(persisted_edits == 3 && persisted_transforms == 1,
            "typed edit and transform proofs must survive save and reopen");
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

void test_explicit_boundary_geometry_operations() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto same_point = [](Vec2 left, Vec2 right) {
        return std::abs(left.x - right.x) < 1e-9 && std::abs(left.y - right.y) < 1e-9;
    };
    const Boundary open{{{0.0, 0.0}, {4.0, 0.0}, 0.0},
                        {{4.0, 0.0}, {4.0, 3.0}, 0.0},
                        {{4.0, 3.0}, {0.0, 3.0}, 0.0}};
    const auto before_close = window.document().revision();
    const auto closed_id = window.createClosedBoundaryFromOpenChain(
        open, QStringLiteral("living"));
    require(!closed_id.isEmpty() && window.document().revision() == before_close + 1,
            "automatic closure must publish one document command");
    const auto closed_snapshot = window.document().snapshot();
    require(closed_snapshot.history().back().action == "Auto close boundary",
            "automatic closure must remain visible in document history");
    const auto closed = decode_identified_boundary_entity(
        closed_snapshot.entities().at(closed_id.toStdString()));
    require(closed.segments.size() == 4 &&
                same_point(closed.segments.back().segment.end,
                           closed.segments.front().segment.start) &&
                std::abs(signed_area(boundary_geometry(closed)) - 12.0) < 1e-9,
            "automatic closure must add exactly the missing edge and preserve topology");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(closed_id.toStdString()) &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(closed_id.toStdString()) ==
                    closed_snapshot.entities().at(closed_id.toStdString()),
            "automatic closure must undo and redo as one exact command");

    const auto bay_id = window.createBayWindowBoundary(
        {0.0, 0.0}, {1.0, -1.0}, {3.0, -1.0}, {4.0, 0.0},
        QStringLiteral("bay"));
    require(!bay_id.isEmpty() &&
                window.document().snapshot().history().back().action == "Complete bay window",
            "bay-window completion must publish a named document command");
    const auto bay = decode_identified_boundary_entity(
        window.document().snapshot().entities().at(bay_id.toStdString()));
    require(bay.segments.size() == 4 &&
                same_point(bay.segments[0].segment.end, {1.0, -1.0}) &&
                same_point(bay.segments[1].segment.end, {3.0, -1.0}) &&
                same_point(bay.segments[2].segment.end, {4.0, 0.0}) &&
                same_point(bay.segments[3].segment.end, {0.0, 0.0}),
            "bay-window completion must retain the three shoulders and exact closure edge");
    const auto before_jump = window.document().revision();
    require(window.selectEntity(bay_id) &&
                window.jumpSelectedBoundaryVertex(
                    QString::fromStdString(bay.segments[1].start_vertex_id)) &&
                window.document().revision() == before_jump && window.lastError().isEmpty(),
            "point jumping must target an identified vertex without dirtying the document");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(bay_id.toStdString()) &&
                window.redoCommand() &&
                window.document().snapshot().entities().contains(bay_id.toStdString()),
            "bay-window completion must be undoable and redoable");
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
        auto* compare_from = dialog->findChild<QComboBox*>(QStringLiteral("revisionCompareFrom"));
        auto* compare_to = dialog->findChild<QComboBox*>(QStringLiteral("revisionCompareTo"));
        auto* comparison = dialog->findChild<QPlainTextEdit*>(QStringLiteral("revisionComparison"));
        auto* status = dialog->findChild<QLabel*>(QStringLiteral("revisionStatus"));
        require(list && name && name_current && compare && compare_from && compare_to &&
                    comparison && status,
                "named revision editor must expose history, naming, compare, and status controls");
        name->setText(QStringLiteral("Existing conditions"));
        name_current->click();
        require(list->count() == 1 && window.document().snapshot().named_revisions().contains(
                    "Existing conditions") &&
                    compare_from->currentData(Qt::UserRole + 1).toBool() &&
                    compare_from->currentText().startsWith(QStringLiteral("Current head")),
                "naming the current revision must create a marker without changing a current-head selection");
        const auto named_revision = window.document().snapshot().named_revisions().at(
            "Existing conditions");
        const auto wall = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
        require(!wall.isEmpty() && window.document().revision() > named_revision,
                "later edits must remain available after naming a revision");
        name->setText(QStringLiteral("Design revision"));
        name_current->click();
        const auto design_revision = window.document().snapshot().named_revisions().at(
            "Design revision");
        require(list->count() == 2 && design_revision > named_revision &&
                    compare_from->currentData(Qt::UserRole + 1).toBool() &&
                    compare_from->currentText().startsWith(QStringLiteral("Current head")),
                "a second named revision must remain independently selectable without retargeting current head");
        compare_from->setCurrentIndex(compare_from->findData(
            QVariant::fromValue<qulonglong>(named_revision)));
        compare_to->setCurrentIndex(compare_to->findData(
            QVariant::fromValue<qulonglong>(design_revision)));
        compare->click();
        require(comparison->toPlainText().contains(QStringLiteral("From: Existing conditions")) &&
                    comparison->toPlainText().contains(QStringLiteral("To: Design revision")) &&
                    comparison->toPlainText().contains(QStringLiteral("Entities added: 1")) &&
                    comparison->toPlainText().contains(QStringLiteral("Geometric changes: 1")) &&
                    comparison->toPlainText().contains(QStringLiteral("Details:")) &&
                    status->text().contains(QStringLiteral("without changing"), Qt::CaseInsensitive),
                "two named revisions must report categorized changes without mutating the document");
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

void test_boundary_transform_workflow(const QString& capture_directory) {
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
    auto decorated = rotated;
    decorated.properties["name"] = boundary_id.toStdString();
    decorated.properties["custom_metadata"] = {{"entity_id", boundary_id.toStdString()}};
    decorated.extensions["vendor_note"] = "Retain this boundary finish";
    decorated.properties["segments"][0]["finish"] = "paint";
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(decorated)},{},"clone metadata fixture"});
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
    require(clone.properties.value("name",std::string{}) == decorated.properties.at("name").get<std::string>() &&
        clone.properties.value("custom_metadata",nlohmann::json{}) == decorated.properties.at("custom_metadata") &&
        clone.extensions == decorated.extensions &&
        clone.properties.at("segments")[0].value("finish",std::string{}) == "paint",
        "boundary copy must retain names, opaque metadata and per-edge finishes");
    require(window.document().snapshot().entities().at(boundary_id.toStdString()) == decorated &&
        window.undoCommand() && !window.document().snapshot().entities().contains(clone_id.toStdString()) &&
        window.redoCommand() && window.document().snapshot().entities().at(clone_id.toStdString()) == clone,
        "boundary copy must preserve its source and restore metadata exactly through undo/redo");
    require(window.selectEntity(clone_id), "reselect restored boundary copy");
    require(clone.properties.at("classification") == "measurement" &&
                decode_identified_boundary_entity(clone).segments.front().segment.start.x > 1.2,
            "boundary clone should retain safe context metadata while applying its transform");
    const auto flipped_start=decode_identified_boundary_entity(clone).segments.front().segment.start;
    require(std::abs(flipped_start.x-(rotated_model.segments.front().segment.start.x+1.2192))<1e-9 &&
        std::abs(flipped_start.y-3.0)<1e-9,
        "vertical UI flip must reflect Y consistently for walls and boundaries");

    auto* action = window.findChild<QAction*>(QStringLiteral("boundaryTransform"));
    require(action, "boundary transform should be available from the secondary command surface");
    const auto before_preview=window.document().snapshot();
    require(window.transformSelectedBoundary("0",false,false,"0","0",false) &&
        window.document().revision()==before_preview.revision(),"unchanged boundary transform must not add history");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("boundaryTransformDialog"));
        require(dialog, "boundary transform editor must open");
        require(dialog->findChild<QLineEdit*>(QStringLiteral("boundaryRotationDegrees")) &&
                    dialog->findChild<QLineEdit*>(QStringLiteral("boundaryOffsetX")) &&
                    dialog->findChild<QCheckBox*>(QStringLiteral("boundaryClone")) &&
                    dialog->findChild<QDialogButtonBox*>(QStringLiteral("boundaryTransformButtons")),
                "boundary transform editor must expose pivot transform controls");
        auto* preview=dynamic_cast<desktop::PlanCanvas*>(dialog->findChild<QWidget*>("wallTransformPreview"));
        auto* rotation=dialog->findChild<QLineEdit*>("boundaryRotationDegrees");
        auto* apply=dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply);
        require(preview && preview->entities().size()==2,"boundary editor must preview original and proposed geometry");
        rotation->setText("invalid");
        require(preview->entities().empty() && !apply->isEnabled(),"invalid boundary angle must invalidate the candidate");
        rotation->setText("35");
        dialog->findChild<QLineEdit*>("boundaryOffsetX")->setText("2 m");
        if (!apply->isEnabled()) std::cerr << "boundary preview error: " <<
            dialog->findChild<QLabel*>("boundaryTransformStatus")->text().toStdString() << '\n';
        require(preview->entities().size()==2 && apply->isEnabled() &&
            window.document().revision()==before_preview.revision() &&
            window.document().snapshot().entities()==before_preview.entities(),
            "boundary preview must remain detached from document history");
        if(!capture_directory.isEmpty()) require(dialog->grab().save(capture_directory+"/boundary-transform.png"),
            "save boundary transform preview capture");
        dialog->reject();
    });
    action->trigger();
    require(window.document().snapshot().entities()==before_preview.entities(),"cancel boundary preview without mutation");
    QString preview_id;
    Vec2 preview_start;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
        dialog->findChild<QCheckBox*>("boundaryClone")->setChecked(true);
        dialog->findChild<QLineEdit*>("boundaryRotationDegrees")->setText("35");
        auto* preview=dynamic_cast<desktop::PlanCanvas*>(dialog->findChild<QWidget*>("wallTransformPreview"));
        for(const auto& entity:preview->entities()) if(entity.selected) {
            preview_id=entity.id;
            preview_start=entity.segments.front().start;
        }
        dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply)->click();
    });
    action->trigger();
    require(!preview_id.isEmpty() && window.selectedEntityId()==preview_id &&
        window.document().revision()==before_preview.revision()+1,"boundary Apply must commit the exact cached clone identity");
    const auto applied=decode_identified_boundary_entity(window.document().snapshot().entities().at(preview_id.toStdString()));
    require(applied.segments.front().segment.start.x==preview_start.x && applied.segments.front().segment.start.y==preview_start.y &&
        window.undoCommand() && window.document().snapshot().entities()==before_preview.entities(),
        "applied boundary geometry must equal the preview and undo exactly");
    auto guarded=window.document().snapshot().entities().at(clone_id.toStdString());
    guarded.extensions["receipt"]={{"version",99}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(guarded)},{},"opaque boundary receipt"});
    require(window.selectEntity(clone_id),"select guarded boundary");
    const auto guarded_snapshot=window.document().snapshot();
    require(!window.transformSelectedBoundary("0",false,false,"0","0",true) &&
        window.document().snapshot().entities()==guarded_snapshot.entities() &&
        window.document().revision()==guarded_snapshot.revision(),
        "copy must reject unhandled receipt identity migration without dropping data or adding history");
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("boundaryTransformDialog");
        dialog->findChild<QLineEdit*>("boundaryRotationDegrees")->setText("10");
        require(!dialog->findChild<QDialogButtonBox*>("boundaryTransformButtons")->button(QDialogButtonBox::Apply)->isEnabled() &&
            !dialog->findChild<QLabel*>("boundaryTransformStatus")->text().isEmpty() &&
            window.document().snapshot().entities()==guarded_snapshot.entities(),
            "preview must preserve opaque receipt semantics and reject unsupported geometry edits");
        dialog->reject();
    });
    action->trigger();
    desktop::MainWindow curved_window;
    const auto curved_id=curved_window.createBoundary(
        Boundary{{{-2,0},{2,0},std::acos(-1.0)},{{2,0},{-2,0},0}},"measurement");
    require(!curved_id.isEmpty() && curved_window.selectEntity(curved_id) &&
        curved_window.transformSelectedBoundary("180",false,false,"0","0",false),
        "semicircular boundary must support rotation");
    const auto curved=decode_identified_boundary_entity(curved_window.document().snapshot().entities().at(curved_id.toStdString()));
    require(std::abs(curved.segments.front().segment.start.x-2.0)<1e-9 &&
        std::abs(curved.segments.front().segment.start.y+2.0)<1e-9,
        "curved-boundary pivot must include arc extrema rather than chord endpoints alone");

    desktop::MainWindow labeled;
    const auto owner = labeled.createBoundary(
        Boundary{{{0,0},{4,0},0},{{4,0},{4,2},0},{{4,2},{0,2},0},{{0,2},{0,0},0}},"measurement");
    const auto boundary = decode_identified_boundary_entity(labeled.document().snapshot().entities().at(owner.toStdString()));
    auto dimension = encode_boundary_dimension_entity(BoundaryDimension{
        "transform-label",owner.toStdString(),boundary.segments.front().segment_id,{1,-0.5},
        BoundaryDimensionPlacement::manual,{}});
    dimension.extensions["note"] = "retain label style";
    labeled.document().apply(ApplyEntityChanges{labeled.document().revision(),
        {EntityChange::upsert(dimension)},{},"dimension transform fixture"});
    const auto labeled_before = labeled.document().snapshot();
    require(labeled.selectEntity(owner) && labeled.transformSelectedBoundary("90",true,false,"5 m","-3 m",false),
        "labeled boundary must support compound transform");
    const auto labeled_after = labeled.document().snapshot();
    const auto moved_dimension = decode_boundary_dimension_entity(labeled_after.entities().at(dimension.id));
    require(moved_dimension.supported() && std::abs(moved_dimension.dimension->text_position.x-5.5)<1e-9 &&
        std::abs(moved_dimension.dimension->text_position.y+3)<1e-9 &&
        moved_dimension.dimension->boundary_id == owner.toStdString() &&
        moved_dimension.dimension->segment_id == boundary.segments.front().segment_id &&
        labeled_after.entities().at(dimension.id).extensions == dimension.extensions,
        "in-place boundary transform must carry dimension placement, identity and metadata");
    require(labeled_after.revision() == labeled_before.revision()+1 && labeled.undoCommand() &&
        labeled.document().snapshot().entities() == labeled_before.entities() && labeled.redoCommand() &&
        labeled.document().snapshot().entities() == labeled_after.entities(),
        "boundary and label must transform in one reversible command");
    const auto before_noop = labeled.document().revision();
    require(labeled.selectEntity(owner) && labeled.transformSelectedBoundary("0",false,false,"0","0",false) &&
        labeled.document().revision() == before_noop,
        "unchanged labeled transform must not add history");
    require(!labeled.transformSelectedBoundary("0",false,false,"invalid offset","0",false) &&
        labeled.document().snapshot().entities() == labeled_after.entities(),
        "invalid transform must leave boundary and label untouched");
}

void test_organization_context() {
    sketch::desktop::MainWindow window;
    const auto second_building = window.createBuilding("property-1", "Workshop");
    require(!second_building.isEmpty(), "create a second building");
    const auto second_floor = window.createFloor(second_building, "Upper floor");
    require(!second_floor.isEmpty(), "create a floor with a default layer atomically");
    const auto layer = window.activeLayerId();
    require(!layer.isEmpty() && layer != "layer-1", "new floor activates its own drawing layer");
    auto* active_layer_item = navigator_item(window, layer);
    require(active_layer_item && active_layer_item->font(0).bold() &&
                active_layer_item->text(1) == QStringLiteral("●") &&
                active_layer_item->toolTip(0).contains(QStringLiteral("Active layer")) &&
                active_layer_item->parent() && active_layer_item->parent()->text(0) == QStringLiteral("Upper floor") &&
                active_layer_item->parent()->parent() &&
                active_layer_item->parent()->parent()->text(0) == QStringLiteral("Workshop"),
            "the project hierarchy must identify the active layer within its floor and building");
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

void test_terrain_surface_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const Boundary boundary{
        {{0.0, 0.0}, {6.0, 0.0}, 0.0},
        {{6.0, 0.0}, {6.0, 4.0}, 0.0},
        {{6.0, 4.0}, {0.0, 4.0}, 0.0},
        {{0.0, 4.0}, {0.0, 0.0}, 0.0},
    };
    const auto source = window.createRoomBoundary(boundary, QStringLiteral("Site pad"));
    require(!source.isEmpty() && window.selectEntity(source),
            "terrain fixture must create and select a source boundary");
    const auto before = window.document().revision();
    const auto terrain = window.createTerrainSurfaceFromSelectedBoundary(
        QStringLiteral("100 m, 101 m, 103 m, 102 m"));
    require(!terrain.isEmpty() && window.document().revision() == before + 1,
            "terrain authoring must commit one undoable entity command");
    const auto snapshot = window.document().snapshot();
    const auto& terrain_entity = snapshot.entities().at(terrain.toStdString());
    require(terrain_entity.type == "terrain_surface" &&
                terrain_entity.properties.at("model").at("points").size() == 5 &&
                terrain_entity.properties.at("model").at("triangles").size() == 4 &&
                terrain_entity.properties.at("source_entity_id") == source.toStdString(),
            "terrain authoring must persist a centroid fan and source metadata");
    auto* plan = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(plan != nullptr && std::any_of(plan->entities().begin(), plan->entities().end(),
                                           [&](const auto& item) {
                                               return item.id == terrain && item.segments.size() >= 5;
                                           }),
            "terrain surface must appear in the shared plan scene");
    window.setWorkspace(desktop::Workspace::architectural);
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(terrain.toStdString()) &&
                window.redoCommand() &&
                window.document().snapshot().entities().contains(terrain.toStdString()),
            "terrain creation must participate in undo and redo");
    QTemporaryDir directory;
    require(directory.isValid(), "terrain fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("terrain.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "terrain surfaces must survive save and reopen");
    require(window.document().snapshot().entities().at(terrain.toStdString()).properties
                .at("model").at("contour_interval_m") == 1.0,
            "reopened terrain must retain its model settings");
}

void test_building_form_authoring_and_quantity_history() {
    using namespace sketch;
    desktop::MainWindow window;
    const std::vector<BuildingObject> objects{
        RectangularColumn{"workflow-column", {1.0, 2.0, 0.0}, 0.4, 0.6, 3.0, 0.2},
        CircularColumn{"workflow-round", {3.0, 2.0, 0.0}, 0.25, 3.0},
        Beam{"workflow-beam", {1.0, 2.0, 3.0}, {4.0, 2.0, 3.0}, {0.0, 0.0, 1.0}, 0.2, 0.3},
        StairFlight{"workflow-stair", {5.0, 0.0, 0.0}, 0.0, 4, 0.8, 0.25, 1.0, StairLanding{0.6, 0.15}},
        Railing{"workflow-railing", {5.0, 2.0, 0.0}, 0.0, 3.0, 1.1, 0.08, 0.9},
        SlopedRoofPanel{"workflow-shed", {0.0, 0.0, 4.0}, 0.0, 4.0, 3.0, 1.0, std::atan(0.25), 0.2, 0.1},
        GableRoof{"workflow-gable", {8.0, 0.0, 4.0}, 0.2, 5.0, 4.0, 1.0, std::atan(0.5), 0.2, 0.1},
        HipRoof{"workflow-hip", {16.0, 0.0, 4.0}, 0.2, 6.0, 4.0, 1.0, std::atan(0.5), 0.2, 0.1},
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
    require(directory.isValid(), "architectural-form fixture needs a temporary directory");
    const auto path = directory.filePath("architectural-forms.bldproj");
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
    if (!window.exportDraftPdf(directory.filePath("architectural-forms.pdf"))) {
        throw std::runtime_error(
            "the architectural-form scene exports through the shared PDF renderer: " +
            window.lastError().toStdString());
    }
    const auto svg = directory.filePath("architectural-forms.svg");
    require(window.exportDraftSvg(svg), "the architectural-form scene exports through the shared SVG renderer");
    QFile svg_file(svg);
    require(svg_file.open(QIODevice::ReadOnly | QIODevice::Text), "draft SVG should be readable");
    const auto svg_text = QString::fromUtf8(svg_file.readAll());
    require(svg_text.contains("<svg") && svg_text.contains("DRAFT"),
            "draft SVG should contain vector markup and its draft stamp");
}

void test_contextual_building_dimension_inspector(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    window.setMetricUnits(true);
    window.setWorkspace(desktop::Workspace::architectural);
    const std::vector<BuildingObject> objects{
        RectangularColumn{"inspector-column", {1, 2.123456789012345, 0}, 0.4, 0.6, 3.0, 0.23456789012345},
        CircularColumn{"inspector-round", {3, 2, 0}, 0.25, 3.0},
        Beam{"inspector-beam", {1, 2, 3}, {4, 2, 3}, {0, 0, 1}, 0.2, 0.3},
        StairFlight{"inspector-stair", {5, 0, 0}, 0.0, 4, 0.8, 0.25, 1.0, StairLanding{0.6, 0.15}},
        Railing{"inspector-railing", {5, 2, 0}, 0.0, 3.0, 1.1, 0.08, 0.9},
    };
    auto* group = window.findChild<QGroupBox*>("buildingDimensions");
    auto* apply = window.findChild<QPushButton*>("applyBuildingDimensions");
    auto* error = window.findChild<QLabel*>("buildingDimensionsError");
    auto* placement_toggle = window.findChild<QToolButton*>("buildingPlacementToggle");
    auto* placement_body = window.findChild<QWidget*>("buildingPlacement");
    require(group && apply && error && placement_toggle && placement_body && placement_body->isHidden(),
            "building inspector has a collapsed placement section");
    for (const auto& object : objects) {
        const auto id = window.commitBuildingObject(encode_building_entity(object,
            {{"future_metadata", "retained"}}), window.document().revision());
        require(!id.isEmpty() && window.selectEntity(id) && !group->isHidden(),
                "each column, beam, stair and railing exposes contextual dimensions");
        if (!placement_toggle->isChecked()) placement_toggle->click();
        require(!placement_body->isHidden(), "selected object placement section expands");
        const auto before = window.document().snapshot().entities().at(id.toStdString());
        const auto suffix = before.type == "column" ? "Height" :
            before.type == "railing" ? "Length" : "Width";
        const auto key = before.type == "column" ? "height_m" :
            before.type == "railing" ? "length_m" : "width_m";
        auto* edit = window.findChild<QLineEdit*>(QStringLiteral("contextBuilding") + suffix);
        require(edit && !edit->isHidden() &&
                    std::abs(parse_quantity(edit->text().toStdString(), Unit::metre).metres -
                             before.properties.at(key).get<double>()) < 1e-6,
                "family dimensions populate editable fields");
        const auto revision = window.document().revision();
        apply->click();
        require(window.document().revision() == revision, "unchanged dimensions must not add history");
        edit->setText("1.5 m");
        apply->click();
        const auto edited = window.document().snapshot().entities().at(id.toStdString());
        require(window.document().revision() == revision + 1 && edited.properties.at(key) == 1.5 &&
                    edited.extensions == before.extensions,
                "dimension edit validates and commits exactly one edit to selected object");
        require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before &&
                    window.redoCommand() && window.document().snapshot().entities().at(id.toStdString()) == edited,
                "building dimension edits undo and redo exactly");
        const auto position_key = before.type == "beam" ? "start_m" :
            (before.type == "stair" || before.type == "railing") ? "base_position_m" : "base_center_m";
        require(edited.properties.at(position_key) == before.properties.at(position_key),
                "dimension-only edits preserve exact untouched placement precision");
        if (id == "inspector-column")
            require(edited.properties.at("rotation_rad") == before.properties.at("rotation_rad"),
                    "dimension-only edits preserve exact untouched angle precision");
        edit->setText("-1 m");
        const auto invalid_revision = window.document().revision();
        apply->click();
        require(window.document().revision() == invalid_revision &&
                    window.document().snapshot().entities().at(id.toStdString()) == edited &&
                    !error->isHidden() && !error->text().isEmpty(),
                "invalid dimension leaves document unchanged and shows inline validation");
        require(window.selectEntity(id), "refresh contextual building fields");
        const auto set_placement = [&](const char* suffix, const char* text) {
            auto* field = window.findChild<QLineEdit*>(QStringLiteral("contextBuilding") + suffix);
            require(field && !field->isHidden(), "selected object exposes relevant placement field");
            field->setText(QString::fromLatin1(text));
        };
        const bool beam = before.type == "beam";
        set_placement(beam ? "StartX" : "BaseX", "1 1/2 ft");
        set_placement(beam ? "StartY" : "BaseY", "-2 m");
        set_placement(beam ? "StartZ" : "BaseZ", "0.75 m");
        if (beam) {
            set_placement("EndX", "4 m");
            set_placement("EndY", "-1 m");
            set_placement("EndZ", "1.25 m");
        } else if (id != "inspector-round") {
            set_placement("OrientationDegrees", "45");
        }
        const auto placement_revision = window.document().revision();
        apply->click();
        const auto placed = window.document().snapshot().entities().at(id.toStdString());
        require(window.document().revision() == placement_revision + 1 &&
                    std::abs(placed.properties.at(position_key).at(0).get<double>() - 0.4572) < 1e-12 &&
                    placed.properties.at(position_key).at(1) == -2.0 &&
                    placed.properties.at(position_key).at(2) == 0.75 && placed.extensions == edited.extensions,
                "placement coordinates commit together using quantity parsing");
        require(placed.properties.at("quantity_entries").contains(std::string("/") + position_key + "/0"),
                "placement quantity input retains its receipt");
        if (beam) {
            require(placed.properties.at("end_m") == nlohmann::json::array({4.0, -1.0, 1.25}),
                    "beam end coordinates are editable");
        } else if (id != "inspector-round") {
            require(std::abs(placed.properties.at(before.type == "column" ? "rotation_rad" : "orientation_rad")
                                 .get<double>() - std::acos(-1.0) / 4.0) < 1e-12,
                    "placement orientation accepts degrees and stores radians");
        }
        require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == edited &&
                    window.redoCommand() && window.document().snapshot().entities().at(id.toStdString()) == placed,
                "placement edits undo and redo exactly");
        if (beam) {
            set_placement("StartX", "4 m");
            set_placement("StartY", "-1 m");
            set_placement("StartZ", "1.25 m");
            edit->setText("2 m");
            const auto degenerate_revision = window.document().revision();
            apply->click();
            require(window.document().revision() == degenerate_revision &&
                        window.document().snapshot().entities().at(id.toStdString()) == placed && !error->isHidden(),
                    "degenerate beam endpoints reject dimensions and placement atomically");
            require(window.selectEntity(id), "reset rejected beam placement fields");
        }
        set_placement(beam ? "StartX" : "BaseX", "7 m");
        edit->setText("2 m");
        auto intervening = placed;
        intervening.properties["external_metadata"] = "intervening";
        window.document().apply(ApplyEntityChanges{
            .expected_revision = window.document().revision(),
            .entity_changes = {EntityChange::upsert(intervening)},
            .message = "intervening building edit",
        });
        const auto stale_revision = window.document().revision();
        apply->click();
        require(window.document().revision() == stale_revision &&
                    window.document().snapshot().entities().at(id.toStdString()) == intervening && !error->isHidden(),
                "stale building dimension context cannot overwrite intervening edits");
    }
    require(window.selectEntity("inspector-round") &&
                !window.findChild<QLineEdit*>("contextBuildingRadius")->isHidden() &&
                window.findChild<QLineEdit*>("contextBuildingWidth")->isHidden() &&
                window.findChild<QLineEdit*>("contextBuildingOrientationDegrees")->isHidden() &&
                window.findChild<QLineEdit*>("contextBuildingStartX")->isHidden(),
            "round columns show radius instead of rectangular section dimensions");
    require(window.selectEntity("inspector-stair") &&
                !window.findChild<QLineEdit*>("contextBuildingRiserCount")->isHidden() &&
                window.findChild<QLineEdit*>("contextBuildingRadius")->isHidden(),
            "stairs expose risers and hide column radius");
    auto* risers = window.findChild<QLineEdit*>("contextBuildingRiserCount");
    require(risers->text() == "4", "stair riser count reflects the selected stair");
    const auto stair_revision = window.document().revision();
    risers->setText("2.5");
    apply->click();
    require(window.document().revision() == stair_revision && !error->isHidden(),
            "fractional riser count is rejected without a document mutation");
    risers->setText("6");
    apply->click();
    require(window.document().revision() == stair_revision + 1 &&
                window.document().snapshot().entities().at("inspector-stair").properties.at("riser_count") == 6,
            "contextual stair risers use normal integer validation");
    if (!capture_directory.isEmpty()) {
        window.resize(1200, 850);
        window.show();
        window.fitView();
        QApplication::processEvents();
        require(window.grab().save(capture_directory + "/stair-inspector.png"),
                "capture contextual stair inspector for visual review");
        require(window.grab().save(capture_directory + "/placement-inspector.png"),
                "capture expanded placement inspector for visual review");
    }
    const auto final_entities = window.document().snapshot().entities();
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("building-inspector.bldproj")) &&
                window.openProject(directory.filePath("building-inspector.bldproj")),
            "contextual building dimensions save and reopen");
    auto* plan = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    for (const auto& id : {"inspector-column", "inspector-round", "inspector-beam", "inspector-stair", "inspector-railing"}) {
        require(window.document().snapshot().entities().at(id) == final_entities.at(id),
                "edited building geometry and metadata persist exactly");
        require(plan && std::any_of(plan->entities().begin(), plan->entities().end(), [&](const auto& entry) {
                    return entry.id == id && !entry.segments.empty();
                }), "edited objects remain in the shared plan and export projection");
    }
    require(window.selectEntity({}) && group->isHidden(), "dimension inspector hides without an object selection");
}

void test_material_assignment_inspector(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    window.setWorkspace(desktop::Workspace::architectural);
    auto catalog = Entity::create("assembly_model", {{"version", 1},
        {"model", AssemblyModel::create({{"timber", "Timber"}, {"steel", "Steel"}}, {}, {}).to_json()}});
    window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(catalog)}, {}, "create materials"});
    const auto id = window.commitBuildingObject(encode_building_entity(RectangularColumn{
        "assigned-column", {0,0,0}, 0.4,0.4,3,0}), window.document().revision());
    require(!id.isEmpty() && window.selectEntity(id), "select material-bearing object");
    auto* choices = window.findChild<QComboBox*>("materialAssignment");
    auto* apply = window.findChild<QPushButton*>("assignMaterial");
    require(choices && choices->count() == 3 && !choices->isHidden(), "inspector exposes local catalog materials");
    const auto before = window.document().snapshot().entities().at(id.toStdString());
    choices->setCurrentIndex(choices->findText("Timber"));
    const auto revision = window.document().revision();
    apply->click();
    const auto assigned = window.document().snapshot().entities().at(id.toStdString());
    require(window.document().revision() == revision + 1 &&
                assigned.properties.at("material_assignment").at("material_id") == "timber" &&
                assigned.properties.at("material_assignment").at("catalog_id") == catalog.id,
            "material selection commits the catalog reference");
    require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before &&
                window.redoCommand(), "material assignment undo/redo");
    QTemporaryDir directory;
    require(window.saveProjectAs(directory.filePath("materials.bldproj")) &&
                window.openProject(directory.filePath("materials.bldproj")) &&
                window.document().snapshot().entities().at(id.toStdString()) == assigned && window.selectEntity(id),
            "material assignment persists with geometry");
    require(choices->currentText() == "Timber", "reopened inspector resolves material name");
    const auto schedule = window.scheduleSnapshot();
    const auto material_row = std::find_if(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
        [&](const auto& row) { return row.object_id == id.toStdString() + ":material"; });
    require(material_row != schedule.snapshot.rows.end() &&
        std::abs(std::get<ScheduleQuantity>(material_row->cells.at("volume").value).value - 0.48) < 1e-8 &&
        !material_row->cells.at("volume").editable, "desktop schedule measures assigned solid volume");
    if (!capture_directory.isEmpty()) {
        window.resize(1200, 850); window.show(); QApplication::processEvents(); window.fitView();
        require(window.grab().save(capture_directory + "/material-inspector.png"), "material inspector capture");
    }
    choices->setCurrentIndex(choices->findText("Steel"));
    auto changed = assigned;
    changed.properties["intervening_note"] = true;
    window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(changed)}, {}, "intervening edit"});
    const auto stale_revision = window.document().revision();
    apply->click();
    require(window.document().revision() == stale_revision &&
                !window.findChild<QLabel*>("materialAssignmentError")->isHidden(), "stale material editor rejects overwrite");
    require(window.selectEntity(id), "refresh material editor");
    choices->setCurrentIndex(0); apply->click();
    require(!window.document().snapshot().entities().at(id.toStdString()).properties.contains("material_assignment"),
            "None explicitly removes material assignment");
}

void test_roof_opening_authoring(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    window.setMetricUnits(true);
    const auto id = window.commitBuildingObject(encode_building_entity(HipRoof{
        "opening-ui", {0,0,3}, 0, 8,6,1.5,std::atan(0.5),0.2,0.15}), window.document().revision());
    require(!id.isEmpty() && window.selectEntity(id), "select roof for opening authoring");
    const auto before = window.document().snapshot().entities().at(id.toStdString());
    const auto revision = window.document().revision();
    auto* command = window.findChild<QPushButton*>("editRoofOpenings");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("roofOpeningsDialog");
        require(dialog, "opening editor opens");
        auto* table = dialog->findChild<QTableWidget*>("roofOpeningsTable");
        dialog->findChild<QPushButton*>("addRoofOpening")->click();
        table->item(0, 0)->setText("50 m");
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        save->click();
        require(window.document().revision() == revision && !dialog->findChild<QLabel*>("roofOpeningsError")->text().isEmpty(),
            "outside opening rejected without history");
        table->item(0, 0)->setText("invalid");
        save->click();
        require(dialog->findChild<QLabel*>("roofOpeningsError")->text().contains("Opening 1, X") &&
                    window.document().revision() == revision, "quantity error identifies opening and column");
        table->item(0, 0)->setText("-1/2");
        table->item(0, 1)->setText("-0.5 m");
        save->click();
    });
    command->click();
    const auto opened = window.document().snapshot().entities().at(id.toStdString());
    require(window.document().revision() == revision + 1 && opened.properties.at("version") == 2 &&
                opened.properties.at("roof_openings").size() == 1, "opening commits as one semantic edit");
    const auto opening_id = opened.properties.at("roof_openings").at(0).at("id").get<std::string>();
    require(opened.extensions.at("roof_opening_input").at("entries").at(opening_id).at("x_m").at("original_expression") == "-1/2",
            "opening coordinate receipt reaches the document");
    require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before &&
                window.redoCommand(), "opening undo restores the uncut roof");
    desktop::BuildingObjectDialog dimensions(opened, true);
    dimensions.findChild<QLineEdit*>("buildingObjectThickness")->setText("0.2 m");
    require(dimensions.submit() && dimensions.candidate()->properties.at("roof_openings") == opened.properties.at("roof_openings"),
            "dimension editing preserves roof openings");
    QTemporaryDir directory;
    require(window.saveProjectAs(directory.filePath("roof-openings.bldproj")) &&
                window.openProject(directory.filePath("roof-openings.bldproj")) &&
                window.document().snapshot().entities().at(id.toStdString()) == opened,
            "opening geometry and input metadata save/reopen exactly");
    require(window.selectEntity(id), "reselect opened roof");
    window.setMetricUnits(false);
    const auto expression_revision = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("roofOpeningsDialog");
        auto* table = dialog->findChild<QTableWidget*>("roofOpeningsTable");
        require(table->item(0, 0)->text() == "-1/2 m", "saved suffixless metric expression stays unambiguous in imperial workspace");
        if (!capture_directory.isEmpty()) {
            QApplication::processEvents();
            require(dialog->grab().save(capture_directory + "/opening-expressions.png"), "opening expressions capture");
        }
        table->item(0, 0)->setText("-500 mm");
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    command->click();
    const auto reexpressed = window.document().snapshot().entities().at(id.toStdString());
    require(window.document().revision() == expression_revision + 1 &&
                reexpressed.properties == opened.properties &&
                reexpressed.extensions.at("roof_opening_input").at("entries").at(opening_id).at("x_m").at("original_expression") == "-500 mm",
            "equivalent entered expression commits its receipt without changing geometry");
    auto tampered = reexpressed;
    tampered.extensions["roof_opening_input"]["entries"][opening_id]["x_m"]["exact_metres"]["numerator"] = 777;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(tampered)}, {}, "tampered input metadata fixture"});
    require(window.selectEntity(id), "refresh tampered receipt fixture");
    const auto viewing_revision = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("roofOpeningsDialog");
        require(dialog->findChild<QTableWidget*>("roofOpeningsTable")->item(0, 0)->text() == "-0.5 m",
            "invalid receipt falls back to canonical geometry rather than its expression");
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    command->click();
    require(window.document().revision() == viewing_revision, "viewing an invalid receipt does not rewrite history");
    if (!capture_directory.isEmpty()) {
        window.resize(1200, 850); window.show(); QApplication::processEvents(); window.fitView();
        require(window.grab().save(capture_directory + "/roof-opening.png"), "roof opening plan capture");
    }
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("roofOpeningsDialog");
        dialog->findChild<QTableWidget*>("roofOpeningsTable")->item(0, 0)->setText("0 m");
        auto intervening = window.document().snapshot().entities().at(id.toStdString());
        intervening.properties["intervening_metadata"] = true;
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(intervening)}, {}, "intervening roof edit"});
        const auto stale_revision = window.document().revision();
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        require(window.document().revision() == stale_revision &&
                    !dialog->findChild<QLabel*>("roofOpeningsError")->text().isEmpty(),
                "stale opening editor cannot overwrite an intervening edit");
        dialog->reject();
    });
    command->click();
    require(window.selectEntity(id), "refresh after stale opening edit");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("roofOpeningsDialog");
        dialog->findChild<QTableWidget*>("roofOpeningsTable")->selectRow(0);
        dialog->findChild<QPushButton*>("removeRoofOpening")->click();
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    command->click();
    const auto closed = window.document().snapshot().entities().at(id.toStdString());
    require(closed.properties.at("version") == 1 && !closed.properties.contains("roof_openings"),
            "removing the final opening restores an uncut version-one roof");
}

void test_hip_roof_authoring(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    window.setWorkspace(desktop::Workspace::architectural);
    desktop::BuildingObjectDialog dialog(std::nullopt, true);
    auto* type = dialog.findChild<QComboBox*>("buildingObjectType");
    type->setCurrentIndex(type->findData("roof"));
    auto* form = dialog.findChild<QComboBox*>("buildingObjectForm");
    form->setCurrentIndex(form->findData("hip_roof"));
    require(dialog.submit() && dialog.candidate() &&
                dialog.candidate()->properties.at("form") == "hip_roof", "hip roof authoring form submits");
    const auto id = window.commitBuildingObject(*dialog.candidate(), window.document().revision());
    require(!id.isEmpty() && window.selectEntity(id), "hip roof creates a document object");
    auto* length = window.findChild<QLineEdit*>("roofRun");
    auto* span = window.findChild<QLineEdit*>("roofSpan");
    auto* apply = window.findChild<QPushButton*>("applyRoofProperties");
    require(!window.findChild<QGroupBox*>("roofProperties")->isHidden(), "hip dimensions visible");
    const auto before = window.document().snapshot().entities().at(id.toStdString());
    const auto revision = window.document().revision();
    length->setText("3 m");
    apply->click();
    require(window.document().revision() == revision &&
                window.document().snapshot().entities().at(id.toStdString()) == before,
            "hip length smaller than span is rejected atomically");
    length->setText("6 m");
    span->setText("6 m");
    apply->click();
    const auto edited = window.document().snapshot().entities().at(id.toStdString());
    require(window.document().revision() == revision + 1 && edited.properties.at("length_m") == 6.0 &&
                edited.properties.at("span_m") == 6.0 && edited.properties.at("form") == "hip_roof",
            "hip inspector can create the square pyramid case");
    require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before &&
                window.redoCommand(), "hip dimensions undo and redo");
    QTemporaryDir directory;
    require(window.saveProjectAs(directory.filePath("hip.bldproj")) &&
                window.openProject(directory.filePath("hip.bldproj")) &&
                window.document().snapshot().entities().at(id.toStdString()) == edited,
            "hip geometry and receipts persist exactly");
    require(window.selectEntity(id), "reselect reopened hip");
    if (!capture_directory.isEmpty()) {
        window.resize(1200, 850);
        window.show();
        QApplication::processEvents();
        window.fitView();
        require(window.grab().save(capture_directory + "/hip-inspector.png"), "hip inspector capture");
    }
}

void test_contextual_roof_dimension_inspector() {
    using namespace sketch;
    desktop::MainWindow window;
    window.setWorkspace(desktop::Workspace::architectural);
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
    window.setWorkspace(desktop::Workspace::architectural);
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

void test_georeferencing_workflow(const QString& capture_directory) {
    sketch::desktop::MainWindow window;
    QTemporaryDir directory;
    require(directory.isValid(), "georeferencing project directory");
    const auto revision = window.document().revision();
    auto* action = window.findChild<QAction*>(QStringLiteral("georeferencingWorkflow"));
    require(action, "georeferencing command must be available in the desktop workspace");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("georeferencingDialog"));
        require(dialog, "georeferencing editor must open");
        auto* identifier = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingCrsIdentifier"));
        auto* definition = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingCrsDefinition"));
        auto* resource_root = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingResourceRoot"));
        auto* a = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingA"));
        auto* b = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingB"));
        auto* tx = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingTx"));
        auto* c = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingC"));
        auto* d = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingD"));
        auto* ty = dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingTy"));
        auto* points = dialog->findChild<QPlainTextEdit*>(QStringLiteral("georeferencingControlPoints"));
        auto* resources = dialog->findChild<QPlainTextEdit*>(QStringLiteral("georeferencingOfflineResources"));
        auto* validate = dialog->findChild<QPushButton*>(QStringLiteral("georeferencingValidate"));
        auto* verify_runtime = dialog->findChild<QPushButton*>(QStringLiteral("georeferencingVerifyRuntime"));
        auto* save = dialog->findChild<QPushButton*>(QStringLiteral("georeferencingSave"));
        auto* summary = dialog->findChild<QLabel*>(QStringLiteral("georeferencingSummary"));
        auto* residuals = dialog->findChild<QPlainTextEdit*>(QStringLiteral("georeferencingResiduals"));
        auto* sample_result = dialog->findChild<QLabel*>(QStringLiteral("georeferencingSampleResult"));
        require(identifier && definition && resource_root && a && b && tx && c && d && ty && points && resources &&
                    validate && verify_runtime && save && summary && residuals && sample_result && !save->isEnabled(),
                "georeferencing controls must exist and start unsaved");
        identifier->setText(QStringLiteral("EPSG:32613"));
        definition->setText(QStringLiteral("fixture projected metre CRS"));
        a->setText(QStringLiteral("2"));
        b->setText(QStringLiteral("0"));
        tx->setText(QStringLiteral("10"));
        c->setText(QStringLiteral("0"));
        d->setText(QStringLiteral("3"));
        ty->setText(QStringLiteral("20"));
        points->setPlainText(QStringLiteral(
            "b,1,0,12,20\norigin,0,0,10,20\nnorth,0,1,10,26"));
        resources->setPlainText(QStringLiteral("proj/proj.db,%1").arg(QString(64, QLatin1Char('a'))));
        validate->click();
        require(save->isEnabled() && summary->text().contains(QStringLiteral("Validated 3 control points")) &&
                    residuals->toPlainText().contains(QStringLiteral("origin")) &&
                    sample_result->text() == QStringLiteral("(10, 20) m"),
                "georeferencing editor must validate and display residuals and transform output");
        if (!capture_directory.isEmpty())
            require(dialog->grab().save(capture_directory + QStringLiteral("/georeferencing.png")),
                    "georeferencing capture");
        save->click();
    });
    action->trigger();
    require(window.document().revision() == revision + 1,
            "saving georeferencing must be one document command");
    std::string georeferencing_id;
    const auto snapshot = window.document().snapshot();
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type == sketch::kGeoreferencingEntityType) {
            require(georeferencing_id.empty(), "georeferencing workflow must keep one record");
            georeferencing_id = id;
            const auto contract = sketch::decode_georeferencing_entity(entity);
            require(contract.apply(1, 1).x == 12.0 && contract.apply(1, 1).y == 23.0,
                    "saved georeferencing must apply its affine transform");
        }
    }
    require(!georeferencing_id.empty(), "georeferencing entity must be saved");
    const auto path = directory.filePath(QStringLiteral("georeferencing.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "georeferencing project must save and reopen");
    require(window.document().snapshot().entities().contains(georeferencing_id),
            "reopened project must retain georeferencing entity");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("georeferencingDialog"));
        auto* identifier = dialog ? dialog->findChild<QLineEdit*>(QStringLiteral("georeferencingCrsIdentifier")) : nullptr;
        auto* points = dialog ? dialog->findChild<QPlainTextEdit*>(QStringLiteral("georeferencingControlPoints")) : nullptr;
        require(dialog && identifier && points && identifier->text() == QStringLiteral("EPSG:32613") &&
                    points->toPlainText().contains(QStringLiteral("origin,0,0,10,20")),
                "reopened georeferencing editor must restore the persisted contract");
        dialog->reject();
    });
    action->trigger();
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
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(preview && preview->entities().size() == 1 && preview->entities().front().segments.size() == 4 &&
                    window.document().revision() == revision,
                "closed survey must preview measured legs without creating document geometry");
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
        require(preview->entities().empty(), "editing calls must clear stale preview geometry");
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

void test_survey_explicit_endpoint_closure() {
    sketch::desktop::MainWindow window;
    auto* action = window.findChild<QAction*>("surveyTraverse");
    const auto revision = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("surveyCalculator");
        dialog->findChild<QLineEdit*>("surveyProvenance")->setText("Closure fixture");
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        input->setPlainText("NE,90,100 m\nSE,0,100 m\nSW,90,100 m\nNW,0,99.9995 m");
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        auto* close = dialog->findChild<QCheckBox*>("surveyCloseEndpoint");
        require(close && close->isEnabled() && !close->isChecked(),
                "closing an endpoint must require an explicit choice for a within-tolerance residual");
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(preview && preview->entities().size() == 2, "residual must have a separate proposed closure preview");
        close->setChecked(true);
        require(preview->entities().front().segments.back().end.y != 0.0 &&
                    preview->entities().back().segments.front().start.y == -100.0 &&
                    preview->entities().back().segments.front().end.y == 0.0 && window.document().revision() == revision,
                "endpoint adjustment must preview the proposed final leg while retaining measured geometry");
        dialog->findChild<QPushButton*>("surveyAddBoundary")->click();
        require(window.document().revision() == revision + 1, "explicit endpoint closure must commit once");
        const auto entity = window.document().snapshot().entities().at(window.selectedEntityId().toStdString());
        const auto boundary = sketch::boundary_geometry(sketch::decode_identified_boundary_entity(entity));
        const auto& source = entity.extensions.at("survey_source");
        require(boundary.size() == 4 && boundary.back().end.x == 0.0 && boundary.back().end.y == 0.0 &&
                    source.at("adjusted_final_endpoint") == true && source.at("added_closing_segment") == false &&
                    std::abs(source.at("endpoint_adjustment_m").at("north").get<double>() - 0.0005) < 1e-10 &&
                    source.at("report").at("vertices").back().at("north_m").get<double>() != 0.0,
                "adjusted boundary must retain the original residual and record the exact correction");
        input->setPlainText("NE,90,100 m");
        require(!close->isChecked() && !close->isEnabled(), "new calls must clear previous endpoint-adjustment consent");
        input->setPlainText("NE,90,100 m\nSE,0,100 m\nSW,90,100 m\nNW,0,99.9999999999999 m");
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        require(close->isEnabled(), "floating-point-sized residual must permit explicit closure");
        const auto before_small_residual = window.document().revision();
        close->setChecked(true);
        dialog->findChild<QPushButton*>("surveyAddBoundary")->click();
        require(window.document().revision() == before_small_residual + 1,
                "explicit closure must avoid creating a degenerate segment for tiny residuals");
        dialog->reject();
    });
    action->trigger();
    require(window.undoCommand(), "explicit survey closure must remain undoable");
}

void test_design_phase_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto wall_id = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
    require(!wall_id.isEmpty(), "phase fixture wall should be created");
    const auto proposed_wall_id = window.createStraightWall({6.0, 2.0}, {10.0, 2.0});
    require(!proposed_wall_id.isEmpty(), "phase fixture proposed wall should be created");
    const auto anchor_wall_id = window.createStraightWall({-10.0, -3.0}, {-8.0, -3.0});
    require(!anchor_wall_id.isEmpty(), "phase fixture anchor wall should be created");
    require(window.activeRemodelingAlternative().isEmpty(),
            "a new project starts on the shared existing baseline");
    auto* phase_combo = window.findChild<QComboBox*>(QStringLiteral("modelPhase"));
    require(phase_combo && phase_combo->currentText().contains(QStringLiteral("Set up")),
            "the navigator should expose the design phase setup action");

    const auto phases = ModelPhases::create(
        {wall_id.toStdString(), proposed_wall_id.toStdString()}, {wall_id.toStdString()},
        {{"remove-wall", "Remove wall", {wall_id.toStdString()},
          {proposed_wall_id.toStdString()}}});
    auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
    phase_entity.id = "phases-desktop";
    auto material_wall = window.document().snapshot().entities().at(wall_id.toStdString());
    material_wall.properties["material_name"] = "Timber";
    material_wall.properties["volume_m3"] = 2.0;
    auto proposed_material = window.document().snapshot().entities().at(proposed_wall_id.toStdString());
    proposed_material.properties["material_name"] = "Masonry";
    proposed_material.properties["volume_m3"] = 3.0;
    window.document().apply(ApplyEntityChanges{
        window.document().revision(), {EntityChange::upsert(phase_entity),
                                        EntityChange::upsert(material_wall),
                                        EntityChange::upsert(proposed_material)}, {},
        "add design phase fixture"});
    require(window.selectEntity(wall_id), "phase fixture should refresh after adding its record");
    require(phase_combo->count() == 2 &&
                phase_combo->itemText(0) == QStringLiteral("Existing baseline") &&
                phase_combo->itemData(1).toString() == QStringLiteral("remove-wall"),
            "the navigator should list baseline and imported alternatives");
    require(window.entityVisible(wall_id), "baseline geometry should be visible before selection");
    require(!window.entityVisible(proposed_wall_id),
            "a proposed object must stay out of the shared baseline view");
    require(window.entityVisible(anchor_wall_id),
            "objects outside the phase registry must remain visible in every phase");
    const auto has_wall_material = [&] {
        const auto projection = window.scheduleSnapshot();
        return std::any_of(projection.snapshot.rows.begin(), projection.snapshot.rows.end(),
            [&](const auto& row) { return row.object_id == wall_id.toStdString() + ":material"; });
    };
    require(has_wall_material(), "baseline schedule must include the visible wall material");

    window.setWorkspace(desktop::Workspace::architectural);
    auto* architectural = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(architectural != nullptr, "phase workflow must expose the architectural canvas");
    const auto canvas_has = [&](const desktop::PlanCanvas* canvas, const QString& id) {
        return canvas != nullptr && std::any_of(canvas->entities().begin(), canvas->entities().end(),
            [&](const auto& entity) { return entity.id == id; });
    };
    require(canvas_has(architectural, wall_id) && !canvas_has(architectural, proposed_wall_id),
            "baseline architectural geometry must exclude proposed objects");

    QTemporaryDir directory;
    require(directory.isValid(), "phase fixture needs a temporary directory");
    const auto render_pdf = [&](const QString& path) {
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
                "phase output PDF must load as one page");
        const auto rendered = pdf.render(0, QSize(1680, 1188));
        require(!rendered.isNull(), "phase output PDF must render");
        return rendered;
    };
    const auto fingerprint_digest = [](const QString& path) {
        QFile file(path + QStringLiteral(".fingerprint.json"));
        require(file.open(QIODevice::ReadOnly | QIODevice::Text),
                "phase output fingerprint must be readable");
        return nlohmann::json::parse(file.readAll()).at("fingerprint").at("digest_sha256")
            .get<std::string>();
    };
    const auto baseline_pdf = directory.filePath(QStringLiteral("baseline.pdf"));
    require(window.exportDraftPdf(baseline_pdf),
            "existing baseline must export through coordinated sheet output");
    const auto baseline_render = render_pdf(baseline_pdf);
    const auto baseline_digest = fingerprint_digest(baseline_pdf);

    const auto baseline_revision = window.document().revision();
    require(window.selectRemodelingAlternative(QStringLiteral("remove-wall")),
            "selecting an alternative should use the typed document command");
    require(window.activeRemodelingAlternative() == QStringLiteral("remove-wall") &&
                !window.entityVisible(wall_id),
            "a demolished baseline object should be hidden in its active alternative");
    require(window.entityVisible(proposed_wall_id),
            "a proposed object should appear in its active alternative");
    require(!has_wall_material(), "demolished wall material must be absent from the active schedule");
    require(canvas_has(architectural, proposed_wall_id) && !canvas_has(architectural, wall_id),
            "alternative architectural geometry must replace the baseline object");
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
    require(!window.entityVisible(proposed_wall_id) && canvas_has(architectural, wall_id) &&
                !canvas_has(architectural, proposed_wall_id),
            "undoing phase selection must restore baseline coordinated geometry");
    require(has_wall_material(), "undoing phase selection must restore baseline material quantities");
    require(window.redoCommand() &&
                window.activeRemodelingAlternative() == QStringLiteral("remove-wall"),
            "phase selection should be redoable");
    require(canvas_has(architectural, proposed_wall_id) && !canvas_has(architectural, wall_id),
            "redoing phase selection must restore alternative architectural geometry");

    const auto alternative_pdf = directory.filePath(QStringLiteral("alternative.pdf"));
    require(window.exportDraftPdf(alternative_pdf),
            "the active alternative must export through coordinated sheet output");
    const auto alternative_render = render_pdf(alternative_pdf);
    require(baseline_render != alternative_render,
            "switching phase must change the coordinated output image");
    require(baseline_digest != fingerprint_digest(alternative_pdf),
            "switching phase must change the coordinated output fingerprint");
    require(baseline_render.copy(QRect(50, 50, 1580, 360)) !=
                alternative_render.copy(QRect(50, 50, 1580, 360)),
            "phase selection must change the coordinated plan viewport");
    require(baseline_render.copy(QRect(50, 500, 740, 320)) !=
                alternative_render.copy(QRect(50, 500, 740, 320)),
            "phase selection must change the coordinated elevation viewport");
    require(baseline_render.copy(QRect(870, 500, 740, 320)) !=
                alternative_render.copy(QRect(870, 500, 740, 320)),
            "phase selection must change the coordinated section viewport");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("remodelingAlternativesDialog"));
        require(dialog, "design phase manager should open from the shared workflow");
        auto* selection = dialog->findChild<QComboBox*>(QStringLiteral("remodelingPhaseSelection"));
        auto* demolition = dialog->findChild<QListWidget*>(QStringLiteral("remodelingDemolitionList"));
        auto* compare_left = dialog->findChild<QComboBox*>(QStringLiteral("remodelingCompareLeft"));
        auto* compare_right = dialog->findChild<QComboBox*>(QStringLiteral("remodelingCompareRight"));
        auto* comparison = dialog->findChild<QListWidget*>(QStringLiteral("remodelingComparisonList"));
        require(selection && demolition && selection->count() == 2 && demolition->count() == 1,
                "design phase manager should expose typed phase and demolition controls");
        bool has_demolished = false;
        bool has_proposed = false;
        if (comparison != nullptr) {
            for (int index = 0; index < comparison->count(); ++index) {
                const auto text = comparison->item(index)->text();
                has_demolished = has_demolished || text.contains(QStringLiteral("demolished"));
                has_proposed = has_proposed || text.contains(QStringLiteral("proposed"));
            }
        }
        require(compare_left && compare_right && comparison && compare_left->count() == 2 &&
                    compare_right->count() == 2 && comparison->count() == 2 &&
                    has_demolished && has_proposed,
                "design phase manager should expose baseline-versus-alternative comparison");
        dialog->reject();
    });
    window.showRemodelingAlternatives();

    const auto path = directory.filePath(QStringLiteral("phases.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "active design phase should save and reopen through the project store");
    require(window.activeRemodelingAlternative() == QStringLiteral("remove-wall"),
            "active design phase should persist across reopen");
    const auto reopened_pdf = directory.filePath(QStringLiteral("reopened.pdf"));
    require(window.exportDraftPdf(reopened_pdf),
            "a reopened alternative must export through the same coordinated sheet output");
    require(fingerprint_digest(reopened_pdf) == fingerprint_digest(alternative_pdf),
            "save and reopen must preserve the alternative output fingerprint");
    require(render_pdf(reopened_pdf) == alternative_render,
            "save and reopen must preserve coordinated plan/elevation/section output");
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
        auto* retarget = dialog->findChild<QPushButton*>(QStringLiteral("retargetRoomRelationship"));
        auto* sync = dialog->findChild<QPushButton*>(QStringLiteral("syncRoomRelationships"));
        require(source && target && kind && list && add && remove && retarget && sync,
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

    const auto boundary_before_retarget = window.document().snapshot().entities().at(boundary_id.toStdString());
    const auto revision_before_retarget = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomRelationshipsDialog"));
        require(dialog, "room relationship editor should reopen for controlled retargeting");
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("roomRelationshipList"));
        auto* retarget = dialog->findChild<QPushButton*>(QStringLiteral("retargetRoomRelationship"));
        require(list && retarget, "relationship editor should expose the retarget action");
        list->setCurrentRow(0);
        require(retarget->isEnabled(), "retarget action should enable for a selected relation");
        QTimer::singleShot(0, &window, [&] {
            auto* retarget_dialog = window.findChild<QDialog*>(
                QStringLiteral("roomRelationshipRetargetDialog"));
            require(retarget_dialog, "retarget action should open an explicit target dialog");
            auto* replacement = retarget_dialog->findChild<QComboBox*>(
                QStringLiteral("roomRelationshipRetargetTarget"));
            auto* buttons = retarget_dialog->findChild<QDialogButtonBox*>(
                QStringLiteral("roomRelationshipRetargetButtons"));
            require(replacement && buttons, "retarget dialog should expose target and confirmation controls");
            const auto source_option_index = replacement->findData(boundary_id);
            require(source_option_index < 0,
                    "retarget dialog should not offer the current source as a replacement target");
            const auto replacement_index = replacement->findData(room_id);
            require(replacement_index >= 0,
                    "retarget dialog should list the live room boundary target");
            replacement->setCurrentIndex(replacement_index);
            buttons->button(QDialogButtonBox::Ok)->click();
            dialog->reject();
        });
        retarget->click();
    });
    window.showRoomRelationships();
    model = RoomRelationshipSnapshot::from_json(relationship_entity().properties.at("model"));
    require(model.relations().size() == 1 &&
                model.relations().front().target_id == room_id.toStdString(),
            "controlled retarget should update the dependency target in one operation");
    require(window.document().revision() == revision_before_retarget + 1 &&
                window.document().snapshot().entities().at(boundary_id.toStdString()) == boundary_before_retarget,
            "controlled retarget should preserve geometry and commit exactly one document revision");
    require(window.undoCommand(), "controlled retarget should be undoable");
    model = RoomRelationshipSnapshot::from_json(relationship_entity().properties.at("model"));
    require(model.relations().size() == 1 &&
                model.relations().front().target_id == wall_id.toStdString(),
            "undo should restore the original relationship target");
    require(window.redoCommand(), "controlled retarget should be redoable");
    model = RoomRelationshipSnapshot::from_json(relationship_entity().properties.at("model"));
    require(model.relations().size() == 1 &&
                model.relations().front().target_id == room_id.toStdString(),
            "redo should restore the retargeted relationship target");

    const auto before_propagation = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomRelationshipsDialog"));
        require(dialog, "room relationship editor should reopen for geometry propagation");
        auto* propagation = dialog->findChild<QPushButton*>(
            QStringLiteral("previewRoomRelationshipPropagation"));
        require(propagation, "relationship editor should expose geometry propagation preview");
        QTimer::singleShot(0, &window, [&] {
            auto* preview = window.findChild<QDialog*>(
                QStringLiteral("roomRelationshipPropagationDialog"));
            require(preview, "geometry propagation should open a visible preview dialog");
            auto* driver = preview->findChild<QComboBox*>(
                QStringLiteral("roomRelationshipPropagationDriver"));
            auto* offset_x = preview->findChild<QLineEdit*>(
                QStringLiteral("roomRelationshipPropagationOffsetX"));
            auto* canvas_widget = preview->findChild<QWidget*>(
                QStringLiteral("roomRelationshipPropagationPreview"));
            auto* canvas = static_cast<desktop::PlanCanvas*>(canvas_widget);
            auto* buttons = preview->findChild<QDialogButtonBox*>(
                QStringLiteral("roomRelationshipPropagationButtons"));
            require(driver && offset_x && canvas_widget && canvas && buttons,
                    "propagation preview should expose typed controls and a canvas");
            const auto driver_index = driver->findData(room_id);
            require(driver_index >= 0, "propagation preview should list the retargeted room driver");
            driver->setCurrentIndex(driver_index);
            offset_x->setText(QStringLiteral("1 m"));
            require(!canvas->entities().empty() &&
                        buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                    "valid propagation should render proposed geometry and enable apply");
            buttons->button(QDialogButtonBox::Apply)->click();
            const auto moved = decode_identified_boundary_entity(
                window.document().snapshot().entities().at(boundary_id.toStdString()));
            require(moved.segments.front().segment.start.x > 0.9,
                    "applied relationship propagation should move the dependent boundary");
            require(window.document().revision() == before_propagation + 1,
                    "relationship propagation should commit one document revision");
            dialog->reject();
        });
        propagation->click();
    });
    window.showRoomRelationships();

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

void test_hosted_opening_editor(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    window.setMetricUnits(true);
    const auto wall = window.createStraightWall({0,0},{6,0},"exterior");
    require(!wall.isEmpty() && window.selectEntity(wall), "opening editor needs selected host");
    const auto before = window.document().snapshot();
    QTimer::singleShot(0,&window,[&] {
        auto* palette = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        require(palette, "opening command palette opens");
        auto* search = palette->findChild<QLineEdit*>();
        search->setText("Create door opening");
        QTimer::singleShot(0,&window,[&] {
            auto* dialog = dynamic_cast<desktop::HostedOpeningDialog*>(window.findChild<QDialog*>("hostedOpeningDialog"));
            require(dialog, "single opening editor replaces chained prompts");
            auto* offset = dialog->findChild<QLineEdit*>("openingOffset");
            auto* width = dialog->findChild<QLineEdit*>("openingWidth");
            auto* button = dialog->findChild<QPushButton*>("createOpening");
            width->setText("10 m");
            require(!button->isEnabled() && !dialog->submit() && window.document().revision()==before.revision(),
                "oversized opening preview must reject without history mutation");
            offset->setText("1/2 m"); width->setText("1 m");
            dialog->findChild<QCheckBox*>("showDoorSwing")->setChecked(true);
            dialog->findChild<QComboBox*>("doorHinge")->setCurrentIndex(1);
            require(button->isEnabled(), "corrected input enables creation");
            if(!capture_directory.isEmpty()) {
                QApplication::processEvents();
                require(dialog->grab().save(capture_directory+"/hosted-opening-editor.png"),"capture hosted opening editor");
            }
            require(dialog->submit(), "valid opening submits");
        });
        QMetaObject::invokeMethod(search,"returnPressed",Qt::DirectConnection);
    });
    window.showCommandPalette();
    const auto after=window.document().snapshot();
    const auto opening=std::find_if(after.entities().begin(),after.entities().end(),
        [](const auto& entry){return entry.second.type=="opening";});
    require(opening!=after.entities().end() && opening->second.properties.at("offset_m")==0.5 &&
        opening->second.properties.at("width_m")==1 && after.revision()==before.revision()+1,
        "opening form must create one exact hosted object in one command");
    require(opening->second.properties.at("door_operation").at("hinge")=="end",
        "door creation must retain explicit handing");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand(), "single form creation participates in undo and redo");
    require(window.selectEntity(QString::fromStdString(opening->first)),"select door for operation editing");
    QTimer::singleShot(0,&window,[&]{
        auto* dialog=window.findChild<QDialog*>("doorSwingDialog");
        require(dialog,"door operation inspector opens");
        dialog->findChild<QComboBox*>("editDoorHinge")->setCurrentIndex(1);
        dialog->findChild<QComboBox*>("editDoorSide")->setCurrentIndex(1);
        dialog->accept();
    });
    window.findChild<QPushButton*>("editDoorSwing")->click();
    require(window.document().snapshot().entities().at(opening->first).properties.at("door_operation").at("side")=="right",
        "door operation edits persist through inspector command");
    require(window.findChild<QPushButton*>("editOpeningAssembly") != nullptr,
        "opening inspector exposes typed assembly editing");
    const auto assembly_before = window.document().revision();
    const auto assembly_edit_ok = window.editSelectedOpeningAssembly(
        "100 mm", "90 mm", "35 mm", "0 mm", "-20 mm");
    if (!assembly_edit_ok) std::cerr << "opening assembly edit error: "
                                    << window.lastError().toStdString() << '\n';
    require(assembly_edit_ok,
        "door assembly dimensions edit through the shared command");
    const auto edited_assembly = parse_opening_assembly(
        window.document().snapshot().entities().at(opening->first).properties.at("opening_assembly"));
    require(std::abs(edited_assembly.frame_width_m - 0.1) < 1e-9 &&
                std::abs(edited_assembly.frame_depth_m - 0.09) < 1e-9 &&
                std::abs(edited_assembly.panel_thickness_m - 0.035) < 1e-9 &&
                edited_assembly.glazing_thickness_m == 0.0 &&
                std::abs(edited_assembly.inset_m + 0.02) < 1e-9 &&
                window.document().revision() == assembly_before + 1,
        "opening assembly edit must retain exact dimensional and signed inset values");
    const auto rejected_assembly_revision = window.document().revision();
    require(!window.editSelectedOpeningAssembly("600 mm", "140 mm", "35 mm", "0 mm", "0 mm") &&
                window.document().revision() == rejected_assembly_revision,
        "opening assembly preview must reject a frame that removes the clear opening");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(opening->first).properties.at("opening_assembly") ==
                    opening->second.properties.at("opening_assembly") &&
                window.redoCommand(),
        "opening assembly dimensions participate in undo and redo");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("openingAssemblyDialog");
        require(dialog, "opening assembly inspector dialog opens");
        dialog->findChild<QLineEdit*>("openingFrameWidth")->setText("120 mm");
        dialog->findChild<QLineEdit*>("openingFrameDepth")->setText("120 mm");
        dialog->findChild<QLineEdit*>("openingPanelThickness")->setText("40 mm");
        dialog->findChild<QLineEdit*>("openingGlazingThickness")->setText("0 mm");
        dialog->findChild<QLineEdit*>("openingInset")->setText("0 mm");
        dialog->findChild<QPushButton*>("saveOpeningAssembly")->click();
    });
    window.findChild<QPushButton*>("editOpeningAssembly")->click();
    const auto ui_assembly = parse_opening_assembly(
        window.document().snapshot().entities().at(opening->first).properties.at("opening_assembly"));
    require(std::abs(ui_assembly.frame_width_m - 0.12) < 1e-9 &&
                std::abs(ui_assembly.frame_depth_m - 0.12) < 1e-9 &&
                std::abs(ui_assembly.panel_thickness_m - 0.04) < 1e-9 &&
                ui_assembly.inset_m == 0.0,
        "opening assembly inspector must commit edited profile fields");
    QTemporaryDir project;
    require(window.saveProjectAs(project.filePath("door.bldproj")) && window.openProject(project.filePath("door.bldproj")) &&
        window.document().snapshot().entities().at(opening->first).properties.at("door_operation").at("side")=="right" &&
        parse_opening_assembly(window.document().snapshot().entities().at(opening->first)
                                   .properties.at("opening_assembly")).frame_width_m == 0.12,
        "door handing and typed assembly dimensions survive project reopen");
    if(!capture_directory.isEmpty()) {
        window.resize(1200,850); window.show(); QApplication::processEvents(); window.fitView();
        require(window.grab().save(capture_directory+"/door-swing-plan.png"),"capture analytic door swing on plan");
    }
    Wall curved{"curved",{{0,0},{4,0},0.5},0.2,3,0,{{"existing",1,1,0,2}}};
    desktop::HostedOpeningDialog overlap(curved,Unit::metre,false);
    overlap.findChild<QLineEdit*>("openingOffset")->setText("1.5 m");
    require(!overlap.submit(), "editor rejects overlapping existing openings on a curved host");
    overlap.findChild<QLineEdit*>("openingOffset")->setText("2.5 m");
    require(overlap.submit(), "curved host preview uses along-wall distance");
}

void test_material_color_catalog(const QString& capture_directory) {
    using namespace sketch;
    desktop::MainWindow window;
    const auto model = [&] {
        const auto snapshot = window.document().snapshot();
        for (const auto& [id, entity] : snapshot.entities())
            if (entity.type == "assembly_model") return AssemblyModel::from_json(entity.properties.at("model"));
        throw std::runtime_error("missing material catalog");
    };
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("assemblyCatalogDialog");
        require(dialog, "material catalog dialog opens");
        auto* id = dialog->findChild<QLineEdit*>("assemblyMaterialId");
        auto* name = dialog->findChild<QLineEdit*>("assemblyMaterialName");
        auto* color = dialog->findChild<QLineEdit*>("assemblyMaterialColor");
        auto* save = dialog->findChild<QPushButton*>("saveAssemblyMaterial");
        require(id && name && color && save, "material catalog exposes appearance authoring");
        id->setText("paint"); name->setText("Paint"); color->setText("#d08030"); save->click();
        require(model().materials().size() == 1 && model().materials()[0].color_srgb == "#d08030",
            "material appearance is saved");
        name->setText("Ochre paint"); color->setText("#c07020"); save->click();
        require(model().materials().size() == 1 && model().materials()[0].name == "Ochre paint" &&
            model().materials()[0].color_srgb == "#c07020", "saving an existing material updates its stable identity");
        if (!capture_directory.isEmpty()) {
            for (auto* tabs : dialog->findChildren<QTabWidget*>())
                if (tabs->indexOf(color->parentWidget()) >= 0) tabs->setCurrentWidget(color->parentWidget());
            QApplication::processEvents();
            require(dialog->grab().save(capture_directory + "/material-color-catalog.png"), "capture material appearance editor");
        }
        const auto revision = window.document().revision();
        color->setText("bad-color"); save->click();
        require(window.document().revision() == revision, "malformed color must not mutate the catalog");
        dialog->accept();
    });
    window.findChild<QAction*>("assemblyCatalog")->trigger();
    require(window.undoCommand() && model().materials()[0].color_srgb == "#d08030" &&
        window.redoCommand() && model().materials()[0].color_srgb == "#c07020", "material appearance undo/redo");
    QTemporaryDir directory;
    require(window.saveProjectAs(directory.filePath("color.bldproj")) && window.openProject(directory.filePath("color.bldproj")) &&
        model().materials()[0].color_srgb == "#c07020", "material appearance survives project save/reopen");
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

void test_assembly_placement_plan_preview() {
    using namespace sketch;
    auto catalog = Entity::create("assembly_model", {
        {"model", AssemblyModel::create(
            {{"steel", "Steel", "#d08030"}},
            {AssemblyType{"lintel", "Lintel", {}, {{"finish", "steel"}}, {}}},
            {AssemblyInstance{"lintel-1", "lintel", {}, {}, {},
                AssemblyPlacement{"host-wall", {1.0, 2.0}, std::numbers::pi / 2.0, 1.0}}}).to_json()}});
    catalog.id = "assembly-catalog";
    auto host = Entity::create("wall", {
        {"baseline", {{"start", {0.0, 0.0}}, {"end", {10.0, 0.0}}, {"sweep_radians", 0.0}}},
        {"height_m", 3.0}, {"thickness_m", 0.2}, {"elevation_m", 0.0}});
    host.id = "host-wall";
    auto document = std::make_shared<Document>(Document::create({catalog, host}));
    desktop::MainWindow window(document);
    QApplication::processEvents();
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "assembly placement fixture should expose the measurement canvas");
    const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [](const auto& entity) { return entity.type == QStringLiteral("assembly_instance"); });
    require(found != canvas->entities().end(), "placed assembly should render a retained plan preview");
    require(found->id == QStringLiteral("assembly-catalog:instance:lintel-1") &&
                std::abs(found->segments.front().start.x - 1.0) < 1e-8 &&
                std::abs(found->segments.front().start.y - 2.0) < 1e-8 &&
                std::abs(found->segments.front().end.x - 1.0) < 1e-8 &&
                std::abs(found->segments.front().end.y - 12.0) < 1e-8 &&
                found->filled && found->fill_color == QColor("#d08030"),
            "assembly placement should apply its transform and material appearance");

    auto* architectural_view = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    auto* architectural_canvas = dynamic_cast<desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(architectural_view && architectural_canvas,
            "assembly placement fixture should expose coordinated architectural views");
    const auto has_assembly_instance = [&] {
        return std::find_if(architectural_canvas->entities().begin(),
                            architectural_canvas->entities().end(),
                            [](const auto& entity) {
                                return entity.id == QStringLiteral("assembly-catalog:instance:lintel-1");
                            }) != architectural_canvas->entities().end();
    };
    architectural_view->setCurrentText(QStringLiteral("Elevation"));
    QApplication::processEvents();
    require(has_assembly_instance(),
            "placed assembly should project through the coordinated elevation view");
    architectural_view->setCurrentIndex(2);
    QApplication::processEvents();
    require(has_assembly_instance(),
            "placed assembly should project through the coordinated section view");
    architectural_view->setCurrentIndex(0);
    require(window.selectEntity(QStringLiteral("assembly-catalog:instance:lintel-1")) &&
                window.selectedEntityId() == QStringLiteral("host-wall"),
            "native or canvas assembly selection should resolve to its persisted host");
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
        auto* binding_floor = dialog->findChild<QComboBox*>(QStringLiteral("verticalFloorBindingFloor"));
        auto* binding_level = dialog->findChild<QComboBox*>(QStringLiteral("verticalFloorBindingLevel"));
        auto* save_binding = dialog->findChild<QPushButton*>(QStringLiteral("saveVerticalFloorBinding"));
        auto* clear_binding = dialog->findChild<QPushButton*>(QStringLiteral("clearVerticalFloorBinding"));
        require(levels && level_id && elevation && save_level && links && link_id && lower && upper &&
                    save_link && freeze && binding_floor && binding_level && save_binding && clear_binding,
                "vertical levels editor should expose level, link and floor binding controls");
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
        binding_floor->setCurrentIndex(binding_floor->findData(QStringLiteral("floor-1")));
        binding_level->setCurrentIndex(binding_level->findData(QStringLiteral("ground")));
        save_binding->click();
        const auto bound_floor = window.document().snapshot().entities().at("floor-1");
        const auto graph_id = bound_floor.properties.at("vertical_level_binding").at("graph_id");
        require(graph_id.is_string() && !graph_id.get<std::string>().empty() &&
                    window.document().snapshot().entities().contains(graph_id.get<std::string>()) &&
                    window.document().snapshot().entities().at(graph_id.get<std::string>()).type ==
                        "vertical_levels",
                "vertical levels editor should persist the selected graph binding");
        require(bound_floor.properties.at("vertical_level_binding").at("level_id") == "ground",
                "vertical levels editor should persist the selected floor level");
        dialog->reject();
    });
    action->trigger();

    const auto bound_wall = window.createStraightWall({0.0, 0.0}, {2.0, 0.0});
    require(!bound_wall.isEmpty(), "a bound floor should continue to author walls");
    const auto bound_wall_entity = window.document().snapshot().entities().at(bound_wall.toStdString());
    require(bound_wall_entity.properties.at("vertical_placement") ==
                nlohmann::json{{"version", 1}, {"mode", "level"}, {"offset_m", 0.0}},
            "new walls on a bound floor should persist level-driven placement");
    const auto bound_column = window.commitBuildingObject(
        encode_building_entity(RectangularColumn{"bound-column", {1.0, 1.0, 0.0}, 0.3, 0.3, 2.5, 0.0}),
        window.document().revision());
    require(!bound_column.isEmpty(), "a bound floor should continue to author architectural objects");
    const auto bound_column_entity = window.document().snapshot().entities().at(bound_column.toStdString());
    require(bound_column_entity.properties.at("vertical_placement") ==
                nlohmann::json{{"version", 1}, {"mode", "level"}, {"offset_m", 0.0}},
            "new architectural objects on a bound floor should persist level-driven placement");

    const Boundary room_boundary{{{{0.0, 0.0}, {4.0004, 0.0}, 0.0},
                                  {{4.0004, 0.0}, {4.0004, 2.0007}, 0.0},
                                  {{4.0004, 2.0007}, {0.0, 2.0007}, 0.0},
                                  {{0.0, 2.0007}, {0.0, 0.0}, 0.0}}};
    const auto bound_room = window.createRoomVolumeFromBoundary(
        room_boundary, QStringLiteral("2.4004 m"), QStringLiteral("0.1234 m"));
    require(!bound_room.isEmpty(), "a bound floor should author a level-relative room volume");
    auto placement_snapshot = window.document().snapshot();
    auto placed_floor = placement_snapshot.entities().at("floor-1");
    auto placed_room = placement_snapshot.entities().at(bound_room.toStdString());
    placed_floor.properties["vertical_level_binding"]["level_id"] = "first";
    placed_room.properties["vertical_placement"]["offset_m"] = 0.4;
    window.document().apply(ApplyEntityChanges{
        .expected_revision = placement_snapshot.revision(),
        .entity_changes = {EntityChange::upsert(std::move(placed_floor)),
                           EntityChange::upsert(std::move(placed_room))},
        .message = "configure level-relative room fixture",
    });
    window.setMetricUnits(true);
    require(window.selectEntity(bound_room), "the level-relative room must remain selectable");
    const auto before_room_dialog = window.document().snapshot();
    const auto before_room_boundary = before_room_dialog.entities()
        .at(bound_room.toStdString()).properties.at("boundary");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("roomDimensionDialog"));
        auto* elevation_field = dialog ? dialog->findChild<QLineEdit*>(
                                             QStringLiteral("roomDimensionElevation")) : nullptr;
        auto* placement_note = dialog ? dialog->findChild<QLabel*>(
                                            QStringLiteral("roomDimensionPlacement")) : nullptr;
        auto* status = dialog ? dialog->findChild<QLabel*>(
                                    QStringLiteral("roomDimensionStatus")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
                                     QStringLiteral("roomDimensionButtons")) : nullptr;
        require(elevation_field && placement_note && status && buttons &&
                    elevation_field->accessibleName().contains(QStringLiteral("bound level")) &&
                    placement_note->text().contains(QStringLiteral("placement offset")) &&
                    status->text().contains(QStringLiteral("project base")),
                "a level-relative room editor must explain local and resolved elevation");
        elevation_field->setText(QStringLiteral("0.2 m"));
        require(buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                    status->text().contains(QStringLiteral("3.600 m project base")),
                "a local elevation preview must include level elevation and placement offset");
        buttons->button(QDialogButtonBox::Apply)->click();
    });
    window.showRoomVolumeDimensions();
    const auto edited_room = window.document().snapshot().entities().at(bound_room.toStdString());
    const auto resolved_room = resolve_vertical_placement(window.document().snapshot(), edited_room);
    require(window.document().revision() == before_room_dialog.revision() + 1 &&
                edited_room.properties.at("boundary") == before_room_boundary &&
                std::abs(edited_room.properties.at("elevation_m").get<double>() - 0.2) < 1e-9 &&
                std::abs(resolved_room.properties.at("elevation_m").get<double>() - 3.6) < 1e-9,
            "valid modal room editing must preserve untouched geometry and commit local/resolved elevation once");

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
    const auto reopened_floor = window.document().snapshot().entities().at("floor-1");
    require(reopened_floor.properties.at("vertical_level_binding").at("level_id") == "first",
            "floor vertical level binding should survive project reopen");
}

void test_reference_grid_workflow() {
    using namespace sketch;
    desktop::MainWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("referenceGrids"));
    require(action, "reference grids should be discoverable from the workspace actions");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("referenceGridDialog"));
        require(dialog, "reference grid editor should open from the workspace action");
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("referenceGridList"));
        auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridName"));
        auto* origin_x = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridOriginX"));
        auto* origin_y = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridOriginY"));
        auto* rotation = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridRotation"));
        auto* spacing_x = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridSpacingX"));
        auto* spacing_y = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridSpacingY"));
        auto* count_x = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridCountX"));
        auto* count_y = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridCountY"));
        auto* major_every = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridMajorEvery"));
        auto* x_label = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridXLabel"));
        auto* y_label = dialog->findChild<QLineEdit*>(QStringLiteral("referenceGridYLabel"));
        auto* visible = dialog->findChild<QCheckBox*>(QStringLiteral("referenceGridVisible"));
        auto* new_grid = dialog->findChild<QPushButton*>(QStringLiteral("newReferenceGrid"));
        auto* save_grid = dialog->findChild<QPushButton*>(QStringLiteral("saveReferenceGrid"));
        auto* remove_grid = dialog->findChild<QPushButton*>(QStringLiteral("removeReferenceGrid"));
        require(list && name && origin_x && origin_y && rotation && spacing_x && spacing_y &&
                    count_x && count_y && major_every && x_label && y_label && visible &&
                    new_grid && save_grid && remove_grid,
                "reference grid editor should expose persisted geometry controls");
        new_grid->click();
        name->setText(QStringLiteral("Structural grid"));
        origin_x->setText(QStringLiteral("10"));
        origin_y->setText(QStringLiteral("-4"));
        rotation->setText(QStringLiteral("0.25"));
        spacing_x->setText(QStringLiteral("2"));
        spacing_y->setText(QStringLiteral("3"));
        count_x->setText(QStringLiteral("2"));
        count_y->setText(QStringLiteral("1"));
        major_every->setText(QStringLiteral("2"));
        x_label->setText(QStringLiteral("Grid X"));
        y_label->setText(QStringLiteral("Grid Y"));
        visible->setChecked(true);
        save_grid->click();
        require(list->count() == 1, "reference grid editor should create one persisted grid");
        const auto snapshot = window.document().snapshot();
        const auto found = std::find_if(snapshot.entities().begin(), snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == "reference_grid"; });
        require(found != snapshot.entities().end(), "reference grid should be a typed document entity");
        const auto model = ReferenceGridModel::from_json(found->second.properties.at("model"));
        require(model.origin_m.x == 10.0 && model.origin_m.y == -4.0 &&
                    model.rotation_radians == 0.25 && model.spacing_x_m == 2.0 &&
                    model.spacing_y_m == 3.0 && model.count_x == 2 && model.count_y == 1 &&
                    model.major_every == 2 && model.visible && model.lines().size() == 8,
                "reference grid editor should persist the exact validated geometry");
        auto* canvas_widget = window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas"));
        auto* canvas = dynamic_cast<desktop::PlanCanvas*>(canvas_widget);
        require(canvas && canvas->referenceGrids().size() == 1 &&
                    canvas->referenceGrids().front().lines == model.lines(),
                "reference grid should render through the shared measurement canvas model");
        rotation->setText(QStringLiteral("0.5"));
        save_grid->click();
        const auto edited_snapshot = window.document().snapshot();
        const auto edited = std::find_if(edited_snapshot.entities().begin(), edited_snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == "reference_grid"; });
        require(edited != edited_snapshot.entities().end() &&
                    ReferenceGridModel::from_json(edited->second.properties.at("model"))
                        .rotation_radians == 0.5,
                "reference grid edits should update one atomic document revision");
        require(window.document().revision() >= 3,
                "reference grid creation and edit should use normal document history");
        dialog->reject();
    });
    action->trigger();

    const auto find_model = [&] {
        const auto snapshot = window.document().snapshot();
        const auto found = std::find_if(snapshot.entities().begin(), snapshot.entities().end(),
            [](const auto& entry) { return entry.second.type == "reference_grid"; });
        require(found != snapshot.entities().end(), "reference grid should remain in the document");
        return ReferenceGridModel::from_json(found->second.properties.at("model"));
    };
    const auto model = find_model();
    require(model.rotation_radians == 0.5 && model.lines().size() == 8,
            "reference grid edit should survive dialog close");
    QTemporaryDir directory;
    require(directory.isValid(), "reference grid fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("reference-grid.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "reference grid should save and reopen through the project store");
    require(find_model() == model, "reference grid geometry should survive project reopen");
    auto* canvas_widget = window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas"));
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(canvas_widget);
    require(canvas && canvas->referenceGrids().size() == 1 &&
                canvas->referenceGrids().front().lines == model.lines(),
            "reopened reference grid should remain bound to the canvas renderer");
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

void test_market_scoped_architectural_workflow() {
    using namespace sketch;
    const auto scope = ProductScopeProfile::production_scope();
    scope.validate();
    for (const auto market : {ScopeMarket::residential, ScopeMarket::light_commercial}) {
        const auto market_name = QString::fromStdString(scope_market_name(market));
        QTemporaryDir directory;
        require(directory.isValid(), "market workflow fixture needs a temporary directory");
        desktop::MainWindow window;
        require(window.selectEntity(QStringLiteral("property-1")) &&
                    window.editProjectSubject(
                        market == ScopeMarket::residential ? QStringLiteral("Residential remodel")
                                                           : QStringLiteral("Light-commercial remodel"),
                        QStringLiteral("1 Example Way"),
                        QStringLiteral("fixture-%1").arg(market_name),
                        QStringLiteral("{\"market\":\"%1\",\"fixture\":\"integrated-architecture-v1\"}")
                            .arg(market_name)),
                "market fixture must persist its scoped subject metadata");

        // Keep the measurement path in the same project as the architectural
        // path.  This is the end-to-end production fixture boundary: a field
        // sketch is created, inspected, and then carried into the richer
        // model workspace without changing its semantic role.
        window.setWorkspace(desktop::Workspace::measurement);
        const Boundary measured_boundary{{{{0.0, 0.0}, {8.0, 0.0}, 0.0},
                                          {{8.0, 0.0}, {8.0, 6.0}, 0.0},
                                          {{8.0, 6.0}, {0.0, 6.0}, 0.0},
                                          {{0.0, 6.0}, {0.0, 0.0}, 0.0}}};
        const auto measured_id = window.createBoundary(measured_boundary,
                                                        QStringLiteral("living"));
        require(!measured_id.isEmpty() && window.selectEntity(measured_id),
                "production fixture must create a classified measurement boundary");
        QApplication::processEvents();
        auto* measured_area = window.findChild<QLabel*>(QStringLiteral("calculationBaseArea"));
        require(measured_area != nullptr && !measured_area->text().contains(QStringLiteral("—")),
                "production fixture must expose a calculated measurement result");
        window.setWorkspace(desktop::Workspace::architectural);

        const auto baseline_wall = window.createStraightWall(
            {0.0, 0.0}, {8.0, 0.0}, QStringLiteral("exterior"));
        const auto proposed_wall = window.createStraightWall(
            {0.0, 3.0}, {8.0, 3.0}, QStringLiteral("interior"));
        const auto room = window.createRoomBoundary(
            {{{{0.25, 0.25}, {7.75, 0.25}, 0.0},
             {{7.75, 0.25}, {7.75, 2.75}, 0.0},
             {{7.75, 2.75}, {0.25, 2.75}, 0.0},
             {{0.25, 2.75}, {0.25, 0.25}, 0.0}}},
            market_name + QStringLiteral(" room"));
        require(!baseline_wall.isEmpty() && !proposed_wall.isEmpty() && !room.isEmpty(),
                "market fixture must author baseline, proposed, and room geometry");

        const Boundary volume_boundary{{{{0.5, 0.5}, {3.5, 0.5}, 0.0},
                                        {{3.5, 0.5}, {3.5, 2.5}, 0.0},
                                        {{3.5, 2.5}, {0.5, 2.5}, 0.0},
                                        {{0.5, 2.5}, {0.5, 0.5}, 0.0}}};
        const auto room_volume = window.createRoomVolumeFromBoundary(
            volume_boundary, QStringLiteral("2.8 m"), QStringLiteral("0 m"));
        require(!room_volume.isEmpty(),
                "production fixture must author an editable architectural room volume");

        auto column = encode_building_entity(
            RectangularColumn{"market-column", {2.0, 1.5, 0.0}, 0.3, 0.3, 3.0, 0.0},
            {{"market_fixture", market_name.toStdString()}});
        column.properties["material_name"] = "Concrete";
        column.properties["volume_m3"] = 0.27;
        const auto column_id = window.commitBuildingObject(column, window.document().revision());
        require(!column_id.isEmpty(), "market fixture must author a semantic building object");

        const auto beam_id = window.commitBuildingObject(
            encode_building_entity(
                Beam{"market-beam", {0.5, 4.0, 2.4}, {6.5, 4.0, 2.4},
                     {0.0, 0.0, 1.0}, 0.2, 0.3}),
            window.document().revision());
        const auto stair_id = window.commitBuildingObject(
            encode_building_entity(
                StairFlight{"market-stair", {4.5, 0.5, 0.0}, 0.0, 4, 0.8, 0.25,
                             1.0, StairLanding{0.6, 0.15}}),
            window.document().revision());
        require(!beam_id.isEmpty() && !stair_id.isEmpty(),
                "production fixture must author structural and circulation objects");
        for (const auto& id : {column_id, beam_id, stair_id}) {
            const auto entity = window.document().snapshot().entities().at(id.toStdString());
            require(!make_building_shape(decode_building_entity(entity)).IsNull(),
                    "production fixture architectural objects must produce native solids");
        }
        require(window.selectEntity(column_id) &&
                    window.transformSelectedArchitecturalObject(QStringLiteral("15"),
                        QStringLiteral("1 ft"), QStringLiteral("0 ft"), QStringLiteral("0 ft"),
                        QStringLiteral("1.1"), false),
                "production fixture must edit an architectural object through the 3D transform path");
        require(window.undoCommand() && window.redoCommand(),
                "production fixture architectural edits must recover through undo and redo");

        const auto toilet_id = window.createAnnotationSymbol(QStringLiteral("toilet"), {1.0, 1.0});
        const auto bed_id = window.createAnnotationSymbol(QStringLiteral("double-bed"), {2.5, 1.0});
        const auto sofa_id = window.createAnnotationSymbol(QStringLiteral("sofa"), {5.0, 1.0});
        const auto commercial_id = window.createAnnotationSymbol(QStringLiteral("checkout-counter"), {6.5, 1.0});
        require(!toilet_id.isEmpty() && !bed_id.isEmpty() && !sofa_id.isEmpty() &&
                    !commercial_id.isEmpty(),
                "production fixture must resolve the residential and commercial symbol families");
        require(window.editAnnotation(toilet_id, {}, QStringLiteral("1.25"), QStringLiteral("1.1"),
                                      QStringLiteral("10"), QStringLiteral("1.8"), true),
                "production fixture must resize and rotate a catalog symbol");
        const auto annotation_state = decode_annotation_entity(
            window.document().snapshot().entities().at("annotations-1"));
        require(annotation_state.symbols.size() >= 4 &&
                    std::any_of(annotation_state.symbols.begin(), annotation_state.symbols.end(),
                        [&](const auto& symbol) {
                            return symbol.id == toilet_id.toStdString() &&
                                std::abs(symbol.placement.scale - 1.8) < 1e-9;
                        }),
                "production fixture must persist symbol catalog placement and scale");

        const auto phases = ModelPhases::create(
            {baseline_wall.toStdString(), proposed_wall.toStdString(), room.toStdString(),
             room_volume.toStdString(), column_id.toStdString(), beam_id.toStdString(),
             stair_id.toStdString()},
            {baseline_wall.toStdString(), room.toStdString(), room_volume.toStdString(),
             column_id.toStdString(), beam_id.toStdString(), stair_id.toStdString()},
            {{"option-" + market_name.toStdString(), "Open plan option", {},
              {proposed_wall.toStdString()}}});
        auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
        phase_entity.id = "market-phases-" + market_name.toStdString();
        window.document().apply(ApplyEntityChanges{
            .expected_revision = window.document().revision(),
            .entity_changes = {EntityChange::upsert(std::move(phase_entity))},
            .message = "add market workflow alternative",
        });
        require(window.selectEntity(column_id) &&
                    window.selectRemodelingAlternative(
                        QStringLiteral("option-%1").arg(market_name)),
                "market fixture must select its remodeling alternative");
        require(window.activeRemodelingAlternative() ==
                    QStringLiteral("option-%1").arg(market_name) &&
                    window.entityVisible(proposed_wall),
                "market fixture alternative must expose proposed geometry");
        const auto view_sources = measured_id + QStringLiteral(",") + baseline_wall +
            QStringLiteral(",") + proposed_wall + QStringLiteral(",") + room_volume +
            QStringLiteral(",") + column_id + QStringLiteral(",") + beam_id +
            QStringLiteral(",") + stair_id;
        for (const auto& view_id : {QStringLiteral("view-plan"), QStringLiteral("view-elevation"),
                                    QStringLiteral("view-section")}) {
            require(window.editArchitecturalViewPresentation(
                        view_id, QStringLiteral("1.5"), QStringLiteral("80"),
                        QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                        QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine"),
                        view_sources),
                    "production fixture must bind plan, elevation, and section sources");
        }
        require(window.editSheetMetadata(
                    QStringLiteral("sheet-1"),
                    market == ScopeMarket::residential ? QStringLiteral("R-101")
                                                       : QStringLiteral("C-101"),
                    market_name + QStringLiteral(" remodel"), QStringLiteral("Coordination set"),
                    QStringLiteral("Field designer"), QStringLiteral("2026-09-13")),
                "market fixture must issue a coordinated drawing sheet");
        const auto revision_id = window.addSheetRevision(
            QStringLiteral("sheet-1"), QStringLiteral("2026-09-13"),
            QStringLiteral("Internal market fixture"));
        require(!revision_id.isEmpty(), "market fixture must retain an issued sheet revision");
        window.document().apply(NameRevision{window.document().revision(),
                                             "Production fixture checkpoint"});
        require(window.document().snapshot().named_revisions().contains(
                    "Production fixture checkpoint"),
                "production fixture must retain a named recoverable revision");
        const auto schedule = window.scheduleSnapshot();
        require(std::any_of(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
                            [&](const auto& row) {
                                return row.object_id == column_id.toStdString();
                            }),
                "market fixture must expose its building object in the shared schedule");

        const auto stem = market_name + QStringLiteral("-workflow");
        const auto project = directory.filePath(stem + QStringLiteral(".bldproj"));
        const auto pdf = directory.filePath(stem + QStringLiteral(".pdf"));
        const auto svg = directory.filePath(stem + QStringLiteral(".svg"));
        const auto png = directory.filePath(stem + QStringLiteral(".png"));
        require(window.saveProjectAs(project) && window.exportDraftPdf(pdf) &&
                    window.exportDraftSvg(svg) && window.exportDraftImage(png),
                "market fixture must save and export its coordinated deliverables");
        const auto project_hash = ProjectStore::file_sha256(
            std::filesystem::path(project.toStdWString()));
        require(std::filesystem::file_size(project.toStdWString()) > 0 &&
                    std::filesystem::file_size(pdf.toStdWString()) > 0 &&
                    std::filesystem::file_size(svg.toStdWString()) > 0 &&
                    std::filesystem::file_size(png.toStdWString()) > 0,
                "market fixture deliverables must be nonempty");
        require(window.openProject(project), "market fixture must reopen through ProjectStore");
        const auto reopened = window.document().snapshot();
        require(ProjectStore::file_sha256(std::filesystem::path(project.toStdWString())) ==
                    project_hash,
                "production fixture reopen must preserve the saved project bytes");
        require(reopened.entities().at("property-1").properties.at("subject")
                        .at("attributes").at("market") == market_name.toStdString() &&
                    reopened.entities().contains(baseline_wall.toStdString()) &&
                    reopened.entities().contains(proposed_wall.toStdString()) &&
                    reopened.entities().contains(room_volume.toStdString()) &&
                    reopened.entities().contains(column_id.toStdString()) &&
                    reopened.entities().contains(beam_id.toStdString()) &&
                    reopened.entities().contains(stair_id.toStdString()) &&
                    reopened.entities().at("annotations-1").properties.at("state")
                        .at("symbols").size() >= 4 &&
                    window.activeRemodelingAlternative() ==
                        QStringLiteral("option-%1").arg(market_name),
                "market fixture must preserve scope, alternatives, and semantic objects after reopen");
    }
}

void test_pdf_export_atomicity() {
    QTemporaryDir directory;
    require(directory.isValid(), "PDF atomicity fixture needs a temporary directory");
    sketch::desktop::MainWindow window;
    const auto path = directory.filePath(QStringLiteral("drawing.pdf"));
    const auto read = [](const QString& name) {
        QFile file(name);
        require(file.open(QIODevice::ReadOnly), "PDF atomicity fixture must be readable");
        return file.readAll();
    };
    if (!window.exportDraftPdf(path)) {
        throw std::runtime_error("PDF atomicity fixture must export initially: " +
                                 window.lastError().toStdString());
    }
    const auto original = read(path);
    require(original.startsWith("%PDF-") && original.contains("%%EOF"),
            "successful PDF must be finalized before export returns");
    const auto fingerprint_path = path + QStringLiteral(".fingerprint.json");
    require(QFile::remove(fingerprint_path) && QDir().mkdir(fingerprint_path),
            "fingerprint destination must be blocked deterministically");
    require(window.editSheetMetadata(QStringLiteral("sheet-1"), QStringLiteral("CHANGED"),
                QStringLiteral("Replacement drawing"), QStringLiteral("Atomic output"),
                QStringLiteral("Test"), QStringLiteral("2026-09-13")),
            "replacement PDF must have changed drawing content");
    require(!window.exportDraftPdf(path), "blocked fingerprint must fail the export");
    require(read(path) == original, "fingerprint failure must preserve the existing PDF bytes");
    const auto absent_path = directory.filePath(QStringLiteral("absent.pdf"));
    require(QDir().mkdir(absent_path + QStringLiteral(".fingerprint.json")) &&
                !window.exportDraftPdf(absent_path) && !QFileInfo::exists(absent_path),
            "failed export must not publish a new PDF");
    require(QDir().rmdir(fingerprint_path), "fingerprint fixture must unblock cleanly");

    const auto valid_sheet = window.document().snapshot().entities().at("sheet-view-1");
    auto unrenderable_sheet = valid_sheet;
    auto& tiny_page = unrenderable_sheet.properties["model"]["sheets"][0];
    tiny_page["width_mm"] = 0.001;
    tiny_page["height_mm"] = 0.001;
    for (const auto* placements : {"viewports", "schedules", "revisions", "callouts"}) {
        tiny_page[placements] = nlohmann::json::array();
    }
    window.document().apply(sketch::ApplyEntityChanges{
        window.document().revision(), {sketch::EntityChange::upsert(unrenderable_sheet)}, {},
        "exercise sheet below PDF device resolution"});
    require(!window.exportDraftPdf(path) && read(path) == original,
            "unrenderable sheet output must preserve the existing PDF bytes");
    window.document().apply(sketch::ApplyEntityChanges{
        window.document().revision(), {sketch::EntityChange::upsert(valid_sheet)}, {},
        "restore valid sheet output"});
    require(window.exportDraftPdf(path) && read(path) != original,
            "successful export must atomically replace the previous PDF");
    {
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
                "replacement PDF must be readable after writer finalization");
        pdf.close();
    }
    const auto fingerprint_digest = [&] {
        QFile file(path + QStringLiteral(".fingerprint.json"));
        require(file.open(QIODevice::ReadOnly | QIODevice::Text),
                "PDF fingerprint must remain readable");
        return nlohmann::json::parse(file.readAll()).at("fingerprint").at("digest_sha256")
            .get<std::string>();
    };
    const auto visible_digest = fingerprint_digest();
    require(window.setContainerVisible(QStringLiteral("floor-1"), false),
            "visibility fixture must hide the persisted floor");
    if (!window.exportDraftPdf(path)) {
        throw std::runtime_error(
            "PDF export must remain available with an effective visibility mask: " +
            window.lastError().toStdString());
    }
    require(fingerprint_digest() != visible_digest,
            "effective visibility masks must change the output fingerprint");
    window.showAllContainers();
    const auto inspect_pdf_without_explicit_close = [&] {
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
                "PDF atomicity fixture must inspect the committed page");
        (void)pdf.pagePointSize(0);
    };
    inspect_pdf_without_explicit_close();
    auto* page_size = window.findChild<QComboBox*>(QStringLiteral("outputPageSize"));
    require(page_size != nullptr, "PDF atomicity fixture must expose the page-size selector");
    page_size->setCurrentText(QStringLiteral("A3"));
    if (!window.exportDraftPdf(path)) {
        throw std::runtime_error("repeated A3 PDF export must remain available: " +
                                 window.lastError().toStdString());
    }
    require(QFileInfo(path).size() > 0, "repeated A3 PDF export must publish bytes");
    require(!window.exportDraftPdf(directory.filePath(QStringLiteral("missing/drawing.pdf"))),
            "unavailable PDF destination must fail without publishing output");
    const auto blocked_path = directory.filePath(QStringLiteral("directory.pdf"));
    require(QDir().mkdir(blocked_path), "PDF destination fixture must block replacement with a directory");
    require(!window.exportDraftPdf(blocked_path) && QFileInfo(blocked_path).isDir(),
            "blocked PDF destination must preserve the existing directory");
}

void test_drawing_set_pdf_and_ordering() {
    QTemporaryDir directory;
    require(directory.isValid(), "drawing-set fixture needs a temporary directory");
    sketch::desktop::MainWindow window;
    const auto landscape = window.createDrawingSheet(
        QStringLiteral("A-201"), QStringLiteral("420"), QStringLiteral("297"),
        QStringLiteral("Landscape details"));
    const auto portrait = window.createDrawingSheet(
        QStringLiteral("A-202"), QStringLiteral("215.5"), QStringLiteral("330.2"),
        QStringLiteral("Portrait details"));
    require(!landscape.isEmpty() && !portrait.isEmpty(),
            "drawing-set fixture must create two additional sheets");
    const auto sheet_order = [&](const sketch::desktop::MainWindow& source) {
        const auto model = sketch::decode_sheet_view_entity(
            source.document().snapshot().entities().at("sheet-view-1"));
        return model.sheet_order();
    };
    require(sheet_order(window) == std::vector<std::string>{
                "sheet-1", landscape.toStdString(), portrait.toStdString()},
            "new drawing sheets must append to the persisted page order");
    require(window.moveDrawingSheet(portrait, -1) && window.moveDrawingSheet(portrait, -1) &&
                sheet_order(window) == std::vector<std::string>{
                    portrait.toStdString(), "sheet-1", landscape.toStdString()},
            "sheet order must support stable one-step moves");
    const auto reordered = sheet_order(window);
    require(window.undoCommand() && sheet_order(window) != reordered &&
                window.redoCommand() && sheet_order(window) == reordered,
            "sheet ordering must participate in normal undo and redo");
    require(!window.moveDrawingSheet(portrait, -1) && sheet_order(window) == reordered,
            "moving the first sheet above the set must be a no-op without history");

    const auto project = directory.filePath(QStringLiteral("ordered-set.bldproj"));
    require(window.saveProjectAs(project), "ordered drawing set must save");
    require(window.createNewProject(),
            "drawing-set fixture must release the saved project before independent reopen");
    sketch::desktop::MainWindow reopened;
    require(reopened.openProject(project) && sheet_order(reopened) == reordered,
            "drawing-sheet order must survive save and reopen");

    const auto set_pdf = directory.filePath(QStringLiteral("drawing-set.pdf"));
    require(reopened.exportDrawingSetPdf(set_pdf), "ordered drawing set must export as one PDF");
    {
        QPdfDocument set_document;
        require(set_document.load(set_pdf) == QPdfDocument::Error::None &&
                    set_document.pageCount() == 3,
                "drawing-set PDF must contain every ordered sheet exactly once");
        const auto require_page_mm = [&](int index, double width, double height) {
            const auto points = set_document.pagePointSize(index);
            require(std::abs(points.width() * 25.4 / 72.0 - width) < 0.4 &&
                        std::abs(points.height() * 25.4 / 72.0 - height) < 0.4,
                    "drawing-set PDF page dimensions or order are incorrect");
        };
        require_page_mm(0, 215.5, 330.2);
        require_page_mm(1, 420.0, 297.0);
        require_page_mm(2, 420.0, 297.0);
    }

    const auto sidecar = set_pdf + QStringLiteral(".fingerprint.json");
    const auto read_manifest = [&](const QString& path) {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly | QIODevice::Text),
                "drawing-set fingerprint must be readable");
        return nlohmann::json::parse(file.readAll().toStdString());
    };
    const auto first_manifest = read_manifest(sidecar);
    require(first_manifest.at("output_kind") == "pdf-set" &&
                first_manifest.at("sheet_order") == reordered &&
                first_manifest.at("fingerprint").at("digest_sha256").is_string(),
            "drawing-set fingerprint must bind kind, complete order, and digest");
    const auto read_bytes = [](const QString& path) {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "drawing-set rollback fixture must read output");
        return file.readAll();
    };
    const auto committed_pdf = read_bytes(set_pdf);
    const auto committed_sidecar = read_bytes(sidecar);
    qApp->setProperty("vertex.testFailDrawingSetSidecarCommit", true);
    require(!reopened.exportDrawingSetPdf(set_pdf),
            "injected drawing-set sidecar commit failure must fail publication");
    qApp->setProperty("vertex.testFailDrawingSetSidecarCommit", {});
    require(read_bytes(set_pdf) == committed_pdf && read_bytes(sidecar) == committed_sidecar,
            "sidecar commit failure must restore the previous PDF and fingerprint pair");
    require(reopened.selectOutputSheet(landscape),
            "drawing-set fixture must select another page without editing the document");
    if (!reopened.exportDrawingSetPdf(set_pdf)) {
        throw std::runtime_error(
            "drawing-set export must remain available after changing selected sheet: " +
            reopened.lastError().toStdString());
    }
    const auto second_manifest = read_manifest(sidecar);
    const auto first_set_digest =
        first_manifest.at("fingerprint").at("digest_sha256").get<std::string>();
    const auto second_set_digest =
        second_manifest.at("fingerprint").at("digest_sha256").get<std::string>();
    if (second_set_digest != first_set_digest) {
        throw std::runtime_error(
            "drawing-set fingerprint must not depend on the selected single-sheet page: " +
            first_set_digest + " != " + second_set_digest);
    }
    const auto valid_set_pdf = read_bytes(set_pdf);
    const auto valid_set_sidecar = read_bytes(sidecar);
    auto unrenderable_set = reopened.document().snapshot().entities().at("sheet-view-1");
    auto& encoded_sheets = unrenderable_set.properties["model"]["sheets"];
    const auto tiny = std::find_if(encoded_sheets.begin(), encoded_sheets.end(),
        [&](const auto& sheet) { return sheet.at("id") == landscape.toStdString(); });
    require(tiny != encoded_sheets.end(), "drawing-set fixture must resolve its final sheet");
    (*tiny)["width_mm"] = 0.001;
    (*tiny)["height_mm"] = 0.001;
    for (const auto* placements : {"viewports", "schedules", "callouts"})
        (*tiny)[placements] = nlohmann::json::array();
    reopened.document().apply(sketch::ApplyEntityChanges{
        reopened.document().revision(), {sketch::EntityChange::upsert(unrenderable_set)}, {},
        "make later drawing-set page unrenderable"});
    require(!reopened.exportDrawingSetPdf(set_pdf) &&
                read_bytes(set_pdf) == valid_set_pdf &&
                read_bytes(sidecar) == valid_set_sidecar,
            "a valid first page and unrenderable later page must preserve the published set pair");
    require(reopened.undoCommand(), "drawing-set fixture must restore the renderable sheet");
    require(reopened.showDrawingSetPrintPreview(),
            "drawing-set fixture must open the PDF-backed mixed-size preview");
    QApplication::processEvents();
    auto* preview = reopened.findChild<QDialog*>(QStringLiteral("drawingSetPrintPreview"));
    require(preview != nullptr, "drawing-set preview must be discoverable");
    const std::array<QSizeF, 3> preview_sizes{
        QSizeF(215.5, 330.2), QSizeF(420.0, 297.0), QSizeF(420.0, 297.0)};
    for (int index = 0; index < 3; ++index) {
        const auto* page = preview->findChild<QLabel*>(
            QStringLiteral("drawingSetPreviewPage%1").arg(index + 1));
        require(page != nullptr && !page->pixmap().isNull(),
                "drawing-set preview must render every staged PDF page");
        require(std::abs(page->property("physicalWidthMm").toDouble() -
                             preview_sizes[static_cast<std::size_t>(index)].width()) < 0.4 &&
                    std::abs(page->property("physicalHeightMm").toDouble() -
                             preview_sizes[static_cast<std::size_t>(index)].height()) < 0.4,
                "drawing-set preview must retain each page's own physical geometry");
    }
    preview->close();
    QApplication::processEvents();

    const auto selected_pdf = directory.filePath(QStringLiteral("selected-sheet.pdf"));
    require(reopened.exportDraftPdf(selected_pdf),
            "selected-sheet PDF must remain available beside drawing-set output");
    QPdfDocument selected_document;
    require(selected_document.load(selected_pdf) == QPdfDocument::Error::None &&
                selected_document.pageCount() == 1,
            "selected-sheet PDF must remain a one-page export");
    const auto selected_points = selected_document.pagePointSize(0);
    require(std::abs(selected_points.width() * 25.4 / 72.0 - 420.0) < 0.4 &&
                std::abs(selected_points.height() * 25.4 / 72.0 - 297.0) < 0.4,
            "selected-sheet PDF must retain its selected physical dimensions");
    selected_document.close();
}

void test_coordinated_view_output_identity() {
    QTemporaryDir directory;
    require(directory.isValid(), "view output fixture directory");
    sketch::desktop::MainWindow window;
    const auto horizontal = window.createStraightWall({0, 0}, {8, 0});
    const auto vertical = window.createStraightWall({0, 0}, {0, 4});
    require(!horizontal.isEmpty() && !vertical.isEmpty(), "view output fixture walls");
    const auto initial = window.document().snapshot().entities().at("sheet-view-1");
    const auto model = sketch::decode_sheet_view_entity(initial);
    auto sheet = model.sheets().front();
    sheet.schedules.clear();
    sheet.viewports = {{"left", "first", {10, 10, 190, 190}, 100},
                       {"right", "second", {220, 10, 190, 190}, 100}};
    const auto render = [&](sketch::CoordinatedViewKind kind, bool second_vertical,
                            bool rotate_second, const QString& name) {
        sketch::CoordinatedView first;
        first.id = "first";
        first.name = "First";
        first.kind = kind;
        first.object_ids = {horizontal.toStdString()};
        if (kind == sketch::CoordinatedViewKind::elevation) {
            first.direction = {0, -1, 0};
            first.up = {0, 0, 1};
        }
        auto second = first;
        second.id = "second";
        second.name = "Second";
        if (second_vertical) second.object_ids = {vertical.toStdString()};
        if (rotate_second) {
            second.direction = {1, 0, 0};
            second.up = {0, 0, 1};
        }
        auto entity = initial;
        entity.properties["model"] = sketch::SheetViewModel::create({first, second}, {sheet}).to_json();
        window.document().apply(sketch::ApplyEntityChanges{
            window.document().revision(), {sketch::EntityChange::upsert(entity)}, {},
            "exercise independently coordinated viewport output"});
        require(window.selectEntity(horizontal), "refresh view output fixture");
        const auto path = directory.filePath(name + QStringLiteral(".pdf"));
        require(window.exportDraftPdf(path), "coordinated viewport PDF must export");
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None, "coordinated viewport PDF must load");
        const auto image = pdf.render(0, QSize(1680, 1188));
        require(!image.isNull(), "coordinated viewport PDF must render");
        return image;
    };
    const auto left = QRect(50, 120, 730, 650);
    const auto right = QRect(890, 120, 730, 650);
    for (const auto kind : {sketch::CoordinatedViewKind::plan,
                            sketch::CoordinatedViewKind::elevation}) {
        const auto original = render(kind, false, false, QStringLiteral("original"));
        const auto changed = render(kind, true, false, QStringLiteral("filtered"));
        require(original.copy(left) == changed.copy(left),
                "changing a second same-kind view must preserve the first viewport");
        require(original.copy(right) != changed.copy(right),
                "each same-kind viewport must render its own object_ids, including plan");
    }
    for (const auto kind : {sketch::CoordinatedViewKind::plan,
                            sketch::CoordinatedViewKind::elevation}) {
        const auto original = render(kind, false, false, QStringLiteral("frame-original"));
        const auto rotated = render(kind, false, true, QStringLiteral("frame-rotated"));
        require(original.copy(left) == rotated.copy(left) && original.copy(right) != rotated.copy(right),
                "each same-kind viewport must render its own persisted frame");
    }

    sketch::desktop::MainWindow crop_window;
    const auto crop_wall = crop_window.createStraightWall({0, 0}, {8, 0});
    require(!crop_wall.isEmpty() && crop_window.selectEntity(crop_wall),
            "cropped plan wall fixture");
    const auto door = crop_window.createHostedOpening(
        QStringLiteral("door"), QStringLiteral("0.5 m"), QStringLiteral("1 m"),
        QStringLiteral("0 m"), QStringLiteral("2.1 m"), std::nullopt,
        sketch::DoorOperation{});
    require(!door.isEmpty() && crop_window.selectEntity(crop_wall),
            "cropped plan door fixture");
    const auto window_id = crop_window.createHostedOpening(
        QStringLiteral("window"), QStringLiteral("5.5 m"), QStringLiteral("1 m"),
        QStringLiteral("1 m"), QStringLiteral("1 m"));
    const sketch::Boundary measurement{{{-1, 1}, {6, 1}, 0}, {{6, 1}, {6, 4}, 0},
                                        {{6, 4}, {-1, 4}, 0}, {{-1, 4}, {-1, 1}, 0}};
    const auto boundary_id = crop_window.createBoundary(measurement);
    const sketch::Boundary room_outline{{{-5, -5}, {10, -5}, 0},
                                         {{10, -5}, {10, 10}, 0},
                                         {{10, 10}, {-5, 10}, 0},
                                         {{-5, 10}, {-5, -5}, 0}};
    const sketch::Boundary room_hole{{{1, 1}, {2, 1}, 0}, {{2, 1}, {2, 2}, 0},
                                     {{2, 2}, {1, 2}, 0}, {{1, 2}, {1, 1}, 0}};
    const auto room_id = crop_window.createRoomVolumeFromBoundary(
        room_outline, QStringLiteral("2.4 m"), QStringLiteral("0 m"), {room_hole});
    require(!window_id.isEmpty() && !boundary_id.isEmpty(),
            "cropped plan window and boundary fixtures");
    require(!room_id.isEmpty(), "cropped plan room-with-hole fixture");
    const auto identified = sketch::decode_identified_boundary_entity(
        crop_window.document().snapshot().entities().at(boundary_id.toStdString()));
    const auto dimension_id = crop_window.createLengthDimension(
        boundary_id, QString::fromStdString(identified.segments.front().segment_id), {2.5, 0.5});
    const auto symbol_id = crop_window.createAnnotationSymbol(
        QStringLiteral("svg-v2-04_living-sofa-three-seat"), {10, 6});
    require(!dimension_id.isEmpty() && !symbol_id.isEmpty(),
            "cropped plan dimension and annotation fixtures");
    auto* crop_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        crop_window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(crop_canvas != nullptr, "cropped plan architectural canvas");
    const auto canvas_has = [&](const QString& id, const QString& type) {
        return std::any_of(crop_canvas->entities().begin(), crop_canvas->entities().end(),
            [&](const auto& entity) { return entity.id == id && entity.type == type; });
    };
    const auto require_complete_plan_content = [&] {
        require(canvas_has(door, QStringLiteral("opening")) &&
                    canvas_has(window_id, QStringLiteral("window")) &&
                    canvas_has(boundary_id, QStringLiteral("measurement_boundary")) &&
                    canvas_has(room_id, QStringLiteral("room")) &&
                    canvas_has(dimension_id, QStringLiteral("dimension_line")) &&
                    canvas_has(symbol_id, QStringLiteral("symbol")),
                "plan views must retain openings, boundaries, dimensions and annotation symbols");
    };
    require_complete_plan_content();
    QTemporaryDir crop_output;
    require(crop_output.isValid(), "cropped plan output directory");
    const auto render_plan_sheet = [&](const QString& name) {
        const auto path = crop_output.filePath(name + QStringLiteral(".pdf"));
        require(crop_window.exportDraftPdf(path), "cropped plan sheet must export");
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None,
                "cropped plan sheet PDF must load");
        const auto image = pdf.render(0, QSize(1680, 1188));
        require(!image.isNull(), "cropped plan sheet PDF must render");
        return image;
    };
    const auto uncropped_sheet = render_plan_sheet(QStringLiteral("uncropped"));
    const auto view_revision = crop_window.document().revision();
    require(crop_window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.2"), QStringLiteral("100"),
                QStringLiteral("0.5"), QStringLiteral("0.18"), true,
                QStringLiteral("solid"), QStringLiteral("1"), QStringLiteral("medium"), {},
                QStringLiteral("-20, 20, -20, 20")),
            "oversized plan crop must commit");
    require(crop_window.document().revision() == view_revision + 1,
            "plan crop must be one undoable command");
    require_complete_plan_content();
    const auto oversized_sheet = render_plan_sheet(QStringLiteral("oversized"));
    require(uncropped_sheet == oversized_sheet,
            "an oversized plan crop must preserve sheet content exactly");

    require(crop_window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.2"), QStringLiteral("100"),
                QStringLiteral("0.5"), QStringLiteral("0.18"), true,
                QStringLiteral("solid"), QStringLiteral("1"), QStringLiteral("medium"), {},
                QStringLiteral("0, 3, -1, 3")),
            "smaller plan crop must commit");
    require(canvas_has(door, QStringLiteral("opening")) &&
                !canvas_has(window_id, QStringLiteral("window")) &&
                canvas_has(boundary_id, QStringLiteral("measurement_boundary")) &&
                canvas_has(room_id, QStringLiteral("room")) &&
                canvas_has(dimension_id, QStringLiteral("dimension_line")) &&
                canvas_has(symbol_id, QStringLiteral("symbol")),
            "smaller crop must clip model linework while retaining annotations");
    const auto boundary_projection = std::find_if(
        crop_canvas->entities().begin(), crop_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == boundary_id; });
    require(boundary_projection != crop_canvas->entities().end() &&
                !boundary_projection->filled && boundary_projection->holes.empty() &&
                boundary_projection->vertex_handles.empty(),
            "cropped area outlines must remain derived unfilled fragments");
    const auto room_projection = std::find_if(
        crop_canvas->entities().begin(), crop_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == room_id; });
    require(room_projection != crop_canvas->entities().end() &&
                room_projection->segments.empty() && room_projection->holes.size() == 1 &&
                !room_projection->holes.front().empty() && !room_projection->filled &&
                room_projection->vertex_handles.empty(),
            "a crop containing only room-hole edges must retain those interior strokes");
    const auto cropped_sheet = render_plan_sheet(QStringLiteral("cropped"));
    require(cropped_sheet != uncropped_sheet,
            "a smaller model crop must visibly change coordinated sheet output");

    require(crop_window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.2"), QStringLiteral("100"),
                QStringLiteral("0.5"), QStringLiteral("0.18"), true,
                QStringLiteral("solid"), QStringLiteral("1"), QStringLiteral("medium"), {},
                QStringLiteral("")),
            "blank crop bounds must clear the plan crop");
    auto crop_model = sketch::decode_sheet_view_entity(
        crop_window.document().snapshot().entities().at("sheet-view-1"));
    auto crop_view = std::find_if(crop_model.views().begin(), crop_model.views().end(),
        [](const auto& view) { return view.id == "view-plan"; });
    require(crop_view != crop_model.views().end() && !crop_view->presentation.crop,
            "clearing crop bounds must persist a null crop");
    require(crop_window.undoCommand(), "clearing a crop must undo");
    crop_model = sketch::decode_sheet_view_entity(
        crop_window.document().snapshot().entities().at("sheet-view-1"));
    crop_view = std::find_if(crop_model.views().begin(), crop_model.views().end(),
        [](const auto& view) { return view.id == "view-plan"; });
    require(crop_view != crop_model.views().end() && crop_view->presentation.crop ==
                std::optional(sketch::ViewCrop{0, 3, -1, 3}),
            "crop clearing undo must restore exact model-space extents");
    const auto project_path = crop_output.filePath(QStringLiteral("crop-roundtrip.bldproj"));
    require(crop_window.saveProjectAs(project_path) && crop_window.openProject(project_path),
            "cropped coordinated view must save and reopen");
    crop_model = sketch::decode_sheet_view_entity(
        crop_window.document().snapshot().entities().at("sheet-view-1"));
    crop_view = std::find_if(crop_model.views().begin(), crop_model.views().end(),
        [](const auto& view) { return view.id == "view-plan"; });
    require(crop_view != crop_model.views().end() && crop_view->presentation.crop ==
                std::optional(sketch::ViewCrop{0, 3, -1, 3}),
            "save/reopen must preserve exact coordinated-view crop extents");
}

void test_architectural_authoring_commands() {
    sketch::desktop::MainWindow window;
    auto* menu = window.findChild<QMenu*>(QStringLiteral("architecturalAuthoringMenu"));
    require(menu != nullptr, "architectural authoring must have a discoverable menu");
    for (const auto* name : {"createWall", "createDoor", "createWindow", "createOpening",
                             "createRoom", "createSlab", "createFloor", "createRoof",
                             "createStair", "createColumn", "createBeam", "joinWalls",
                             "unjoinWalls", "joinRoofs", "unjoinRoofs"}) {
        auto* action = window.findChild<QAction*>(QString::fromLatin1(name));
        require(action && menu->actions().contains(action), "architectural command must be discoverable");
    }
    const auto first = window.createStraightWall({0, 0}, {4, 0}, QStringLiteral("living"));
    const auto second = window.createStraightWall({4, 0}, {4, 3}, QStringLiteral("living"));
    require(!first.isEmpty() && !second.isEmpty(), "wall fixture must be valid");
    require(window.selectEntity(first) && window.selectEntity(second, true), "select wall pair");
    const auto before = window.document().revision();
    const auto created_join = window.joinSelectedWalls(before);
    auto snapshot = window.document().snapshot();
    const auto join_id = created_join.toStdString();
    for (const auto& [id, entity] : snapshot.entities()) {
        if (entity.type == "wall_join") {
            require(id == join_id, "join API must return the committed wall join identity");
            require(entity.properties.at("wall_ids").size() == 2, "join must retain both source walls");
        }
    }
    require(!join_id.empty() && window.document().revision() == before + 1,
            "join action must atomically create semantic wall join");
    require(window.joinSelectedWalls().isEmpty(), "duplicate wall join must be rejected");
    require(window.document().revision() == before + 1, "duplicate join membership must be rejected atomically");
    require(window.selectEntity(created_join) && window.unjoinSelectedWalls(),
            "a selected join identity must remove the derived wall join");
    snapshot = window.document().snapshot();
    require(!snapshot.entities().contains(join_id) && snapshot.entities().contains(first.toStdString()) &&
                snapshot.entities().contains(second.toStdString()), "unjoin must preserve source walls");
    require(window.undoCommand() && window.document().snapshot().entities().contains(join_id),
            "unjoin must undo as one operation");
    require(window.redoCommand(), "unjoin must redo");
    require(window.selectEntity(first), "select lone wall");
    const auto invalid_revision = window.document().revision();
    require(window.joinSelectedWalls().isEmpty(), "a lone wall cannot form a join");
    require(window.document().revision() == invalid_revision && !window.lastError().isEmpty(),
            "invalid join selection must leave document unchanged");
    const auto far_first = window.createStraightWall({20, 0}, {24, 0}, QStringLiteral("living"));
    const auto far_second = window.createStraightWall({24, 0}, {24, 3}, QStringLiteral("living"));
    require(window.selectEntity(first) && window.selectEntity(second, true) &&
                window.selectEntity(far_first, true) && window.selectEntity(far_second, true),
            "select two disconnected wall pairs");
    const auto disconnected_revision = window.document().revision();
    require(window.joinSelectedWalls(disconnected_revision).isEmpty() &&
                window.document().revision() == disconnected_revision,
            "disconnected wall clusters must be rejected before document mutation");
    require(window.selectEntity(first) && window.selectEntity(second, true) &&
                window.joinSelectedWalls(disconnected_revision - 1).isEmpty() &&
                window.document().revision() == disconnected_revision,
            "stale wall join requests must not change the document");
    const auto roof_a = window.commitBuildingObject(sketch::encode_building_entity(sketch::SlopedRoofPanel{
        "command-roof-a", {0, 0, 3}, 0, 4, 3, 0, 0, 0.2, 0.1}), window.document().revision());
    const auto roof_b = window.commitBuildingObject(sketch::encode_building_entity(sketch::SlopedRoofPanel{
        "command-roof-b", {3, 0, 3}, 0, 4, 3, 0, 0, 0.2, 0.1}), window.document().revision());
    require(!roof_a.isEmpty() && !roof_b.isEmpty() && window.selectEntity(roof_a) && window.selectEntity(roof_b, true),
            "select roof pair");
    const auto roof_join_id = window.joinSelectedRoofs();
    const auto joined_roofs = window.document().snapshot();
    const auto roof_join = std::find_if(joined_roofs.entities().begin(), joined_roofs.entities().end(),
        [](const auto& entry) { return entry.second.type == "roof_join"; });
    require(!roof_join_id.isEmpty() && roof_join != joined_roofs.entities().end() &&
                roof_join->first == roof_join_id.toStdString() &&
                roof_join->second.properties.at("roof_ids").size() == 2,
            "roof join must retain both semantic sources");
    require(window.unjoinSelectedRoofs(), "selected roof members must remove their derived join");
    require(!window.document().snapshot().entities().contains(roof_join->first), "roof unjoin removes the join");
    for (const auto* type : {"Roof", "Stair", "Column", "Beam"}) {
        QTimer::singleShot(0, &window, [&window, type] {
            auto* dialog = dynamic_cast<sketch::desktop::BuildingObjectDialog*>(QApplication::activeModalWidget());
            require(dialog && dialog->findChild<QComboBox*>("buildingObjectType")->currentText() == type,
                    "direct architectural command must preselect its object type");
            dialog->reject();
        });
        window.findChild<QAction*>(QStringLiteral("create") + QString::fromLatin1(type))->trigger();
    }
    auto* views = window.findChild<QAction*>(QStringLiteral("manageNamedViews"));
    require(views, "named elevation and section views must have a discoverable manager");
    const auto before_view = window.document().revision();
    QTimer::singleShot(0, &window, [&window] {
        auto* dialog = QApplication::activeModalWidget();
        require(dialog, "named view editor opens");
        auto* selector = dialog->findChild<QComboBox*>("namedViewSelection");
        selector->setCurrentIndex(0);
        dialog->findChild<QLineEdit*>("namedViewName")->setText("East elevation");
        dialog->findChild<QComboBox*>("namedViewKind")->setCurrentIndex(0);
        dialog->findChild<QLineEdit*>("namedViewDirection")->setText("0, 0, 0");
        const auto revision = window.document().revision();
        dialog->findChild<QPushButton*>("saveNamedView")->click();
        require(window.document().revision() == revision && !dialog->findChild<QLabel*>("namedViewError")->text().isEmpty(),
                "invalid view frame must remain editable without document mutation");
        dialog->findChild<QLineEdit*>("namedViewDirection")->setText("1, 0, 0");
        dialog->findChild<QCheckBox*>("namedViewCropEnabled")->setChecked(true);
        dialog->findChild<QLineEdit*>("namedViewCropBounds")->setText("2, 1, -1, 4");
        dialog->findChild<QPushButton*>("saveNamedView")->click();
        require(window.document().revision() == revision &&
                    !dialog->findChild<QLabel*>("namedViewError")->text().isEmpty(),
                "invalid named-view crop must remain editable without document mutation");
        dialog->findChild<QLineEdit*>("namedViewCropBounds")->setText("-1, 5, -0.5, 4");
        dialog->findChild<QPushButton*>("saveNamedView")->click();
    });
    views->trigger();
    require(window.document().revision() == before_view + 1, "named view saves as one command");
    const auto view_snapshot = window.document().snapshot();
    bool found_view = false;
    for (const auto& [id, entity] : view_snapshot.entities()) {
        if (entity.type != sketch::kSheetViewEntityType) continue;
        const auto model = sketch::decode_sheet_view_entity(entity);
        for (const auto& view : model.views()) {
            if (view.name == "East elevation") {
                found_view = view.kind == sketch::CoordinatedViewKind::elevation &&
                    view.direction[0] == 1 && view.presentation.crop ==
                        std::optional(sketch::ViewCrop{-1, 5, -0.5, 4});
            }
        }
    }
    require(found_view, "named view persists its direction and kind");
    require(window.findChild<QComboBox*>("architecturalView")->currentText() == "East elevation",
            "saved named view becomes the selected architectural projection");

    sketch::desktop::MainWindow action_window;
    const auto action_first = action_window.createStraightWall({0, 0}, {4, 0}, QStringLiteral("living"));
    const auto action_second = action_window.createStraightWall({4, 0}, {4, 3}, QStringLiteral("living"));
    require(action_window.selectEntity(action_first) && action_window.selectEntity(action_second, true),
            "join action fixture selection");
    const auto action_revision = action_window.document().revision();
    action_window.findChild<QAction*>(QStringLiteral("joinWalls"))->trigger();
    const auto action_snapshot = action_window.document().snapshot();
    require(action_window.document().revision() == action_revision + 1 &&
                std::any_of(action_snapshot.entities().begin(), action_snapshot.entities().end(),
                    [](const auto& entry) { return entry.second.type == "wall_join"; }),
            "wall join QAction must route through the geometry-admitted command");

    sketch::desktop::MainWindow read_only;
    const auto read_only_first = read_only.createStraightWall({0, 0}, {4, 0}, QStringLiteral("living"));
    const auto read_only_second = read_only.createStraightWall({4, 0}, {4, 3}, QStringLiteral("living"));
    require(read_only.selectEntity(read_only_first) && read_only.selectEntity(read_only_second, true),
            "read-only join fixture selection");
    const auto read_only_revision = read_only.document().revision();
    read_only.document().mark_read_only("join fixture");
    require(read_only.joinSelectedWalls().isEmpty() &&
                read_only.document().revision() == read_only_revision &&
                read_only.lastError().contains(QStringLiteral("read-only")),
            "read-only projects must reject wall joins without mutation");
}

void test_cross_view_source_editing() {
    using namespace sketch;
    desktop::MainWindow window;
    window.setMetricUnits(true);
    window.setWorkspace(desktop::Workspace::architectural);
    const auto wall = window.createStraightWall({0, 0}, {6, 0}, "exterior");
    require(!wall.isEmpty() && window.selectEntity(wall), "cross-view host wall");
    const auto opening = window.createHostedOpening("window", "2 m", "1 m", "0.5 m", "1.5 m");
    require(!opening.isEmpty(), "cross-view hosted window");
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    auto* views = window.findChild<QComboBox*>("architecturalView");
    require(canvas && views, "cross-view canvas and named view selector");
    canvas->setFixedSize(900, 650);
    canvas->setTool(desktop::CanvasTool::select);
    const auto select_view = [&](const char* id) {
        const auto index = views->findData(QString::fromLatin1(id), Qt::UserRole + 1);
        require(index >= 0, "named coordinated view exists");
        views->setCurrentIndex(index);
        canvas->fitView();
    };
    const auto projection = [&](const QString& id) {
        const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
            [&](const auto& entity) { return entity.id == id; });
        require(found != canvas->entities().end() && !found->segments.empty(),
                "non-plan projection must retain an independently selectable source entity");
        return *found;
    };
    const auto bounds = [](const desktop::CanvasEntity& entity) {
        double left = std::numeric_limits<double>::infinity(), right = -left;
        double bottom = left, top = right;
        for (const auto& edge : entity.segments) {
            left = std::min({left, edge.start.x, edge.end.x});
            right = std::max({right, edge.start.x, edge.end.x});
            bottom = std::min({bottom, edge.start.y, edge.end.y});
            top = std::max({top, edge.start.y, edge.end.y});
        }
        return QRectF(QPointF(left, bottom), QPointF(right, top));
    };
    const auto double_click_source = [&](const QString& id) {
        const auto entity = projection(id);
        const auto model = bounds(entity);
        // Measure the actual retained selection frame without changing the
        // window selection; the following mouse event must resolve the source.
        canvas->setSelectedId(id);
        const auto frame = canvas->selectionBounds();
        require(frame.has_value(), "source projection has screen bounds");
        const auto scale = entity.type == "wall"
            ? frame->width() / (model.width() + entity.thickness_metres)
            : (frame->width() - 3.0) / model.width();
        require(window.selectEntity({}), "clear selection before projected hit");
        for (const auto& edge : entity.segments) {
            const QPointF midpoint((edge.start.x + edge.end.x) * 0.5,
                                   (edge.start.y + edge.end.y) * 0.5);
            const QPointF point(frame->center().x() + (midpoint.x() - model.center().x()) * scale,
                                frame->center().y() - (midpoint.y() - model.center().y()) * scale);
            QMouseEvent press(QEvent::MouseButtonPress, point, point,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &press);
            QApplication::sendEvent(canvas, &release);
            if (window.selectedEntityId() != id) continue;
            QMouseEvent event(QEvent::MouseButtonDblClick, point, point,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &event);
            if (window.selectedEntityId() == id) break;
        }
        require(window.selectedEntityId() == id, "projected double-click resolves semantic source ID");
    };
    select_view("view-elevation");
    const auto original_wall = bounds(projection(wall));
    (void)projection(opening);
    double_click_source(wall);
    const auto before_wall = window.document().snapshot();
    auto* height = window.findChild<QLineEdit*>("inspectorHeight");
    require(height && height->isEnabled() && !height->isHidden(), "projected wall exposes typed height");
    height->setText("4 m");
    QMetaObject::invokeMethod(height, "editingFinished", Qt::DirectConnection);
    require(window.document().revision() == before_wall.revision() + 1 &&
            window.document().snapshot().entities().at(wall.toStdString()).properties.at("height_m") == 4.0 &&
            bounds(projection(wall)).height() > original_wall.height(),
            "wall quick edit updates the same source and elevation in one command");
    require(window.undoCommand() && window.document().snapshot().entities() == before_wall.entities() &&
            window.redoCommand(), "projected wall edit participates in undo and redo");
    select_view("view-section");
    const auto original_opening = bounds(projection(opening));
    double_click_source(opening);
    const auto before_opening = window.document().snapshot();
    auto* width = window.findChild<QLineEdit*>("inspectorLength");
    require(width && width->isEnabled() && !width->isHidden(), "projected opening exposes typed width");
    width->setText("20 m");
    width->setModified(true);
    QMetaObject::invokeMethod(width, "editingFinished", Qt::DirectConnection);
    require(window.document().revision() == before_opening.revision() &&
            window.document().snapshot().entities() == before_opening.entities(),
            "invalid projected opening width rejects atomically without changing source history");
    width->setText("1.5 m");
    width->setModified(true);
    QMetaObject::invokeMethod(width, "editingFinished", Qt::DirectConnection);
    const auto edited = window.document().snapshot();
    require(edited.revision() == before_opening.revision() + 1 &&
            edited.entities().at(opening.toStdString()).properties.at("width_m") == 1.5 &&
            edited.entities().at(opening.toStdString()).properties.at("wall_id") == wall.toStdString() &&
            edited.entities().size() == before_opening.entities().size() &&
            bounds(projection(opening)).width() > original_opening.width(),
            "section opening edit changes the shared hosted source without derived duplicates");
    require(window.undoCommand() && window.document().snapshot().entities() == before_opening.entities() &&
            window.redoCommand(), "projected opening edit participates in undo and redo");
    for (const auto* view : {"view-plan", "view-elevation", "view-section"}) {
        select_view(view);
        require(std::abs(bounds(projection(opening)).width() - 1.5) < 1e-7,
                "all coordinated views refresh the edited opening width");
    }
    const auto schedule = window.scheduleSnapshot();
    require(schedule.snapshot.revision == window.document().revision(), "schedule follows projected edit revision");
    const auto row = std::find_if(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
        [&](const auto& item) { return item.object_id == opening.toStdString(); });
    require(row != schedule.snapshot.rows.end() &&
            std::get<ScheduleQuantity>(row->cells.at("width").value).value == 1.5,
            "edited opening retains its source schedule row and updated width");
    QTemporaryDir directory;
    const auto path = directory.filePath("cross-view.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path) &&
            window.document().snapshot().entities() == edited.entities(),
            "cross-view edits preserve exact semantic entities through save and reopen");
    select_view("view-elevation");
    require(std::abs(bounds(projection(opening)).width() - 1.5) < 1e-7,
            "reopened elevation projects the edited source");
}

void test_section_overlay_workflow() {
    sketch::desktop::MainWindow window;
    auto* action = window.findChild<QAction*>("manageNamedViews");
    require(action, "section overlay editor action");
    const auto before = window.document().revision();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = QApplication::activeModalWidget();
        auto* selection = dialog->findChild<QComboBox*>("namedViewSelection");
        selection->setCurrentIndex(selection->findData(QStringLiteral("view-section")));
        auto* table = dialog->findChild<QTableWidget*>("sectionOverlays");
        auto* add = dialog->findChild<QPushButton*>("addSectionOverlay");
        add->click(); table->item(0, 6)->setText("Section overlay proof");
        add->click(); table->item(1, 1)->setText("dimension"); table->item(1, 4)->setText("2");
        add->click(); table->item(2, 1)->setText("detail_line"); table->item(2, 7)->setText("fine");
        dialog->findChild<QPushButton*>("saveNamedView")->click();
    });
    action->trigger();
    require(window.document().revision() == before + 1, "overlay authoring is one history command");
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    const auto count_lines = [&] { return std::count_if(canvas->entities().begin(), canvas->entities().end(),
        [](const auto& entity) { return entity.type == "section_overlay"; }); };
    require(count_lines() == 1, "medium includes dimension but excludes fine detail line");
    require(std::any_of(canvas->labels().begin(), canvas->labels().end(),
        [](const auto& label) { return label.text == "Section overlay proof"; }), "section note retained on canvas");
    require(std::any_of(canvas->labels().begin(), canvas->labels().end(),
        [](const auto& label) { return label.text.contains('\''); }),
        "section dimensions follow the active Imperial display units");
    require(window.undoCommand() && count_lines() == 0 && window.redoCommand() && count_lines() == 1,
        "overlay undo and redo refresh canvas");
    const auto detail = [&](const QString& level) {
        require(window.editArchitecturalViewPresentation("view-section", "1.2", "100", "0.5", "0.18",
            true, "solid", "1", level, ""), "section detail edit");
    };
    detail("fine"); require(count_lines() == 2, "fine includes detail overlay");
    detail("coarse"); require(count_lines() == 0, "coarse excludes medium and fine overlays");
    detail("fine");
    QTemporaryDir directory;
    const auto svg = directory.filePath("section.svg");
    require(window.exportDraftSvg(svg) && window.exportDraftPdf(directory.filePath("section.pdf")),
        "section overlays export through shared scene");
    QFile output(svg); require(output.open(QIODevice::ReadOnly) && output.readAll().contains("Section overlay proof"),
        "SVG contains section annotation text");
    const auto path = directory.filePath("section.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "section overlay save reopen");
    const auto model = sketch::decode_sheet_view_entity(window.document().snapshot().entities().at("sheet-view-1"));
    const auto view = std::find_if(model.views().begin(), model.views().end(), [](const auto& v) { return v.id == "view-section"; });
    require(view != model.views().end() && view->overlays.size() == 3, "overlays survive desktop save reopen");
    const auto stable_id = view->overlays.front().id;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = QApplication::activeModalWidget();
        auto* selection = dialog->findChild<QComboBox*>("namedViewSelection");
        selection->setCurrentIndex(selection->findData(QStringLiteral("view-section")));
        auto* table = dialog->findChild<QTableWidget*>("sectionOverlays");
        table->item(0, 1)->setText("invalid");
        const auto revision = window.document().revision();
        dialog->findChild<QPushButton*>("saveNamedView")->click();
        require(window.document().revision() == revision, "invalid overlay kind cannot mutate document");
        table->item(0, 1)->setText("text"); table->item(0, 6)->setText("Edited annotation");
        table->setCurrentCell(2, 0);
        dialog->findChild<QPushButton*>("removeSectionOverlay")->click();
        dialog->findChild<QPushButton*>("saveNamedView")->click();
    });
    action->trigger();
    const auto edited = sketch::decode_sheet_view_entity(window.document().snapshot().entities().at("sheet-view-1"));
    const auto section = std::find_if(edited.views().begin(), edited.views().end(), [](const auto& v) { return v.id == "view-section"; });
    require(section->overlays.size() == 2 && section->overlays.front().id == stable_id &&
        section->overlays.front().text == "Edited annotation", "edit and remove preserve remaining overlay identity");
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
    if (argc == 2 && std::string_view(argv[1]) == "--named-revisions-only") {
        test_named_revisions();
        std::cout << "Named revision comparison tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--drawing-set-output-only") {
        test_drawing_set_pdf_and_ordering();
        std::cout << "Drawing-set ordering and PDF tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--section-overlays-only") {
        test_section_overlay_workflow();
        std::cout << "Section overlay workflow tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--room-volume-only") {
        test_room_volume_authoring_workflow();
        std::cout << "Room volume workflow tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--cross-view-editing-only") {
        test_cross_view_source_editing();
        std::cout << "Cross-view source editing tests passed\n";
        return 0;
    }
    test_cross_view_source_editing();
    test_architectural_authoring_commands();
    test_section_overlay_workflow();
    if (argc == 2 && std::string_view(argv[1]) == "--architectural-authoring-only") return 0;
    if (argc == 2 && std::string_view(argv[1]) == "--coordinated-view-output-only") {
        test_coordinated_view_output_identity();
        std::cout << "Coordinated view output tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--pdf-export-atomicity-only") {
        test_pdf_export_atomicity();
        std::cout << "PDF export atomicity tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--building-form-only") {
        test_building_form_authoring_and_quantity_history();
        std::cout << "Building form workflow tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--design-phase-only") {
        test_design_phase_workflow();
        std::cout << "Design phase workflow tests passed\n";
        return 0;
    }
    test_plan_canvas_native_pointer_events();
    test_canvas_symbol_transform_persistence();
    if (argc == 2 && std::string_view(argv[1]) == "--canvas-transform-only") {
        std::cout << "Canvas transform workflow tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--selection-clipboard-only") {
        test_multiple_selection_clipboard_workflow();
        test_selection_clipboard_workflow();
        test_material_clipboard_transfer();
        test_delete_selection_workflow();
        std::cout << "Selection clipboard workflow tests passed\n";
        return 0;
    }
    QString field_ui_capture_directory;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--capture-field-ui") {
            field_ui_capture_directory = QString::fromLocal8Bit(argv[index + 1]);
            require(QDir().mkpath(field_ui_capture_directory), "field UI capture directory");
        }
    }
    test_shortcuts_and_measurement_keypad(field_ui_capture_directory);
    test_project_subject_metadata();
    test_second_open_is_read_only();
    test_external_project_change_blocks_save();
    test_workspace_profiles();
    test_room_boundary_from_existing_geometry();
    test_room_volume_authoring_workflow();
    test_multiple_selection_clipboard_workflow();
    test_selection_clipboard_workflow();
    test_wall_transform_workflow(field_ui_capture_directory);
    test_sloped_wall_workflow();
    test_material_clipboard_transfer();
    test_delete_selection_workflow();
    test_boundary_vertex_insertion_workflow();
    test_direct_boundary_geometry_edit_workflow();
    test_boundary_redefinition_workflow();
    test_automatic_room_boundary_detection_workflow();
    test_explicit_boundary_geometry_operations();
    test_named_revisions();
    test_boundary_transform_workflow(field_ui_capture_directory);
    test_design_phase_workflow();
    test_phase_authoring_ownership();
    test_room_relationship_workflow();
    test_vertical_levels_workflow();
    test_reference_grid_workflow();
    test_assembly_catalog_workflow();
    test_assembly_placement_plan_preview();
    test_material_color_catalog(field_ui_capture_directory);
    test_hosted_opening_editor(field_ui_capture_directory);
    test_calculation_deduction_workflow();
    test_market_scoped_architectural_workflow();
    test_coordinated_view_output_identity();
    test_pdf_export_atomicity();
    test_contextual_building_dimension_inspector(field_ui_capture_directory);
    test_contextual_roof_dimension_inspector();
    test_hip_roof_authoring(field_ui_capture_directory);
    test_roof_opening_authoring(field_ui_capture_directory);
    test_material_assignment_inspector(field_ui_capture_directory);
    test_contextual_gable_roof_inspector(field_ui_capture_directory);
    test_georeferencing_workflow(field_ui_capture_directory);
    test_survey_calculator(field_ui_capture_directory);
    test_survey_explicit_endpoint_closure();
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
    test_terrain_surface_workflow();
    test_building_form_authoring_and_quantity_history();
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
    const auto toilet_id = window.createAnnotationSymbol(
        QStringLiteral("toilet"), {6.0, 1.0});
    const auto bed_id = window.createAnnotationSymbol(
        QStringLiteral("double-bed"), {8.0, 1.0});
    const auto sofa_id = window.createAnnotationSymbol(
        QStringLiteral("sofa"), {10.0, 1.0});
    const auto commercial_id = window.createAnnotationSymbol(
        QStringLiteral("checkout-counter"), {12.0, 1.0});
    require(!toilet_id.isEmpty() && !bed_id.isEmpty() && !sofa_id.isEmpty() &&
                !commercial_id.isEmpty(),
            "symbol family aliases should place canonical residential and commercial variants");
    auto annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.size() == 1 && annotation_state.symbols.size() == 5,
            "annotation authoring should update the typed annotation entity");
    const auto annotation_layer = annotation_state.labels.front().placement.layer_id;
    require(!annotation_layer.empty() &&
                std::all_of(annotation_state.symbols.begin(), annotation_state.symbols.end(),
                    [&](const auto& symbol) {
                        return symbol.placement.layer_id == annotation_layer;
                    }),
            "new labels and symbols must belong to the active drawing layer");
    const auto* symbol_item = navigator_item(window, symbol_id);
    require(symbol_item && symbol_item->parent() && symbol_item->parent()->parent() &&
                symbol_item->parent()->text(0) == QStringLiteral("Symbols & labels") &&
                symbol_item->parent()->parent()->data(0, Qt::UserRole).toString() ==
                    QString::fromStdString(annotation_layer),
            "the navigator must nest symbols and labels beneath their owning layer");
    auto* component_heading =
        window.findChild<QLabel*>(QStringLiteral("componentLibraryHeading"));
    require(component_heading &&
                component_heading->text().contains(QStringLiteral("Default layer")) &&
                component_heading->text().contains(QStringLiteral("Symbols & labels")),
            "the component library must identify the active layer it will place into");
    const auto alias_symbol = [&](const QString& id, const char* family) {
        const auto found = std::find_if(annotation_state.symbols.begin(), annotation_state.symbols.end(),
            [&](const auto& value) { return value.id == id.toStdString(); });
        require(found != annotation_state.symbols.end() && found->symbol_id.find(family) == 0 &&
                    found->symbol_id.find("-w2-d2") != std::string::npos,
                "symbol family aliases should resolve the canonical dimension variant");
    };
    alias_symbol(toilet_id, "toilet");
    alias_symbol(bed_id, "double-bed");
    alias_symbol(sofa_id, "sofa");
    alias_symbol(commercial_id, "checkout-counter");
    require(window.editAnnotation(label_id, QStringLiteral("Primary bedroom suite"),
                                  QStringLiteral("3.25"), QStringLiteral("2.5"),
                                  QStringLiteral("30"), QStringLiteral("1.5"), true,
                                  QStringLiteral("Inter"), QStringLiteral("6"),
                                  QStringLiteral("#112233"), QStringLiteral("#445566"), true, true, true),
            "annotation editing should use the typed command path");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.front().content == "Primary bedroom suite" &&
                std::abs(annotation_state.labels.front().placement.position.x - 3.25) < 1e-9 &&
                std::abs(annotation_state.labels.front().placement.position.y - 2.5) < 1e-9 &&
                std::abs(annotation_state.labels.front().placement.scale - 1.5) < 1e-9 &&
                annotation_state.labels.front().style.font_family == "Inter" &&
                std::abs(annotation_state.labels.front().style.text_height_metres - 0.006) < 1e-12 &&
                annotation_state.labels.front().style.stroke_color == "#112233" &&
                annotation_state.labels.front().style.fill_color == "#445566" &&
                annotation_state.labels.front().style.bold && annotation_state.labels.front().style.italic &&
                annotation_state.labels.front().visible,
            "annotation editing should persist text, style, position, scale, and visibility");
    auto* annotation_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(annotation_canvas != nullptr, "annotation style smoke needs the measurement canvas");
    const auto rendered_label = std::find_if(
        annotation_canvas->labels().begin(), annotation_canvas->labels().end(),
        [&](const auto& value) { return value.id == label_id; });
    require(rendered_label != annotation_canvas->labels().end() &&
                rendered_label->color == QColor(QStringLiteral("#112233")) &&
                rendered_label->fill_color == QColor(QStringLiteral("#445566")) &&
                rendered_label->bold && rendered_label->italic &&
                std::abs(rendered_label->text_height_metres - 0.006) < 1e-12,
            "annotation style must reach the shared canvas renderer");
    require(window.editAnnotation(symbol_id, QString(), QStringLiteral("4.25"),
                                  QStringLiteral("2.75"), QStringLiteral("30"),
                                  QStringLiteral("2.4"), true),
            "symbol resizing and placement should use the typed command path");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    const auto edited_symbol = std::find_if(
        annotation_state.symbols.begin(), annotation_state.symbols.end(),
        [&](const auto& value) { return value.id == symbol_id.toStdString(); });
    require(edited_symbol != annotation_state.symbols.end() &&
                std::abs(edited_symbol->placement.position.x - 4.25) < 1e-9 &&
                std::abs(edited_symbol->placement.position.y - 2.75) < 1e-9 &&
                std::abs(edited_symbol->placement.rotation_radians -
                         (30.0 * std::numbers::pi / 180.0)) < 1e-9 &&
                std::abs(edited_symbol->placement.scale - 2.4) < 1e-9,
            "symbol resizing must persist physical placement metadata");
    const auto rendered_symbol = std::find_if(
        annotation_canvas->entities().begin(), annotation_canvas->entities().end(),
        [&](const auto& value) { return value.id == symbol_id && value.type == QStringLiteral("symbol"); });
    require(rendered_symbol != annotation_canvas->entities().end() &&
                !rendered_symbol->segments.empty(),
            "resized symbols must reach the shared canvas renderer");
    const auto symbol_catalog = sketch::default_symbol_catalog();
    for (const auto& instance : annotation_state.symbols) {
        const auto definition = std::find_if(
            symbol_catalog.begin(), symbol_catalog.end(), [&](const auto& value) {
                return value.id == instance.symbol_id;
            });
        require(definition != symbol_catalog.end(),
                "every desktop symbol fixture must resolve to a catalog definition");
        const auto expected = placed_symbol_preview(*definition, instance.placement);
        const auto rendered = std::find_if(
            annotation_canvas->entities().begin(), annotation_canvas->entities().end(),
            [&](const auto& value) {
                return value.id == QString::fromStdString(instance.id) &&
                       value.type == QStringLiteral("symbol");
            });
        require(rendered != annotation_canvas->entities().end() &&
                    rendered->segments.size() == expected.size() &&
                    !expected.empty(),
                "every residential and commercial symbol must retain all vector strokes on canvas");
        require(std::abs(rendered->segments.front().start.x - expected.front().start.x) < 1e-9 &&
                    std::abs(rendered->segments.front().start.y - expected.front().start.y) < 1e-9 &&
                    std::abs(rendered->segments.front().end.x - expected.front().end.x) < 1e-9 &&
                    std::abs(rendered->segments.front().end.y - expected.front().end.y) < 1e-9,
                "canvas symbol coordinates must preserve the catalog resize and rotation transform");
    }
    QTemporaryDir symbol_output_directory;
    require(symbol_output_directory.isValid(), "symbol output fixture needs a temporary directory");
    const auto symbol_pdf = symbol_output_directory.filePath(QStringLiteral("symbols.pdf"));
    const auto symbol_svg = symbol_output_directory.filePath(QStringLiteral("symbols.svg"));
    const auto symbol_png = symbol_output_directory.filePath(QStringLiteral("symbols.png"));
    require(window.exportDraftPdf(symbol_pdf) && window.exportDraftSvg(symbol_svg) &&
                window.exportDraftImage(symbol_png),
            "residential symbol instances must use all shared output paths");
    QFile symbol_svg_file(symbol_svg);
    require(symbol_svg_file.open(QIODevice::ReadOnly | QIODevice::Text),
            "symbol SVG output should be readable");
    const auto symbol_svg_text = QString::fromUtf8(symbol_svg_file.readAll());
    require(symbol_svg_text.contains(QStringLiteral("stroke")) &&
                QFileInfo(symbol_pdf).size() > 0 && QFileInfo(symbol_png).size() > 0 &&
                QFileInfo::exists(symbol_pdf + QStringLiteral(".fingerprint.json")) &&
                QFileInfo::exists(symbol_svg + QStringLiteral(".fingerprint.json")) &&
                QFileInfo::exists(symbol_png + QStringLiteral(".fingerprint.json")),
            "resized symbols must survive PDF, SVG, and image output with fingerprints");
    require(window.selectEntity(label_id), "a persisted annotation child should be selectable");
    require(window.deleteAnnotation(label_id), "annotation deletion should be undoable");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.empty() && annotation_state.symbols.size() == 5,
            "annotation deletion should remove only the selected child");
    require(window.undoCommand() && window.redoCommand() && window.undoCommand(),
            "annotation deletion should participate in normal undo and redo");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.size() == 1 && annotation_state.symbols.size() == 5,
            "annotation undo should restore the persisted child");

    QTemporaryDir reference_directory;
    require(reference_directory.isValid(), "reference fixture needs a temporary directory");
    const auto reference_path = reference_directory.filePath("trace.png");
    QImage reference_image(40, 20, QImage::Format_ARGB32);
    reference_image.fill(QColor(220, 240, 255));
    require(reference_image.save(reference_path, "PNG"), "reference fixture should save a PNG");
    const auto reference_id = sketch::testing::importOrSeedTrustedReferenceFixture(window, reference_path, reference_image);
    require(!reference_id.isEmpty(), "a local raster should import into the document");
    const auto reference_snapshot = window.document().snapshot();
    const auto reference_entity = reference_snapshot.entities().find(reference_id.toStdString());
    require(reference_entity != reference_snapshot.entities().end() &&
                reference_entity->second.type == "reference_asset",
            "reference import should create a typed reference entity");
    require(reference_entity->second.properties.at("content_mode") == "traceable-reference" &&
                reference_entity->second.properties.at("editable_extraction") == false &&
                reference_entity->second.properties.at("source_preserved") == true &&
                reference_entity->second.properties.at("page_index") == 0 &&
                reference_entity->second.properties.at("page_count") == 1 &&
                reference_entity->second.properties.at("fidelity_mode") == "decoded-raster",
            "reference import should declare traceable content and decoded fidelity explicitly");
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
    reference_image.fill(Qt::red);
    const auto first_underlay = sketch::testing::importOrSeedTrustedReferenceFixture(
        multi_reference_window, reference_path, reference_image);
    reference_image.fill(Qt::blue);
    const auto second_underlay = sketch::testing::importOrSeedTrustedReferenceFixture(
        multi_reference_window, second_reference_path, reference_image);
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
    const auto pdf_id = sketch::testing::importOrSeedTrustedReferenceFixture(pdf_window, reference_pdf_path, reference_image);
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

    auto* profile_action = window.findChild<QAction*>(QStringLiteral("calculationProfile"));
    require(profile_action, "calculation profile editor should be available as a command");
    const auto profile_editor_revision = window.document().revision();
    const auto profile_editor_before = window.document().snapshot().entities().at("property-1")
                                           .properties.at("calculation_profile");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("calculationProfileDialog"));
        QLineEdit* profile_id = nullptr;
        QSpinBox* decimals = nullptr;
        QTableWidget* classifications = nullptr;
        QPushButton* save = nullptr;
        if (dialog != nullptr) {
            profile_id = dialog->findChild<QLineEdit*>(QStringLiteral("calculationProfileId"));
            decimals = dialog->findChild<QSpinBox*>(QStringLiteral("calculationProfileDecimals"));
            classifications = dialog->findChild<QTableWidget*>(
                QStringLiteral("calculationProfileClassifications"));
            save = dialog->findChild<QPushButton*>(QStringLiteral("saveCalculationProfile"));
        }
        require(dialog && profile_id && decimals && classifications && save,
                "calculation profile editor should expose version, precision, rules, and save controls");
        require(profile_id->text() == QStringLiteral("vertex-default") &&
                    classifications->rowCount() >= 1,
                "calculation profile editor should load the persisted profile");
        decimals->setValue(3);
        save->click();
        require(dialog->isVisible() &&
                    dialog->findChild<QLabel*>(QStringLiteral("calculationProfileEditorStatus"))
                        ->text()
                        .contains(QStringLiteral("saved"), Qt::CaseInsensitive),
                "calculation profile editor should report a successful versioned save");
        dialog->reject();
    });
    profile_action->trigger();
    const auto profile_editor_after = window.document().snapshot().entities().at("property-1")
                                          .properties.at("calculation_profile");
    require(window.document().revision() == profile_editor_revision + 1 &&
                profile_editor_after.at("version").get<unsigned>() ==
                    profile_editor_before.at("version").get<unsigned>() + 1 &&
                profile_editor_after.at("decimal_places").get<unsigned>() == 3,
            "calculation profile editor should persist precision as one undoable versioned command");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at("property-1")
                        .properties.at("calculation_profile")
                        .at("decimal_places")
                        .get<unsigned>() == profile_editor_before.at("decimal_places").get<unsigned>() &&
                window.redoCommand() &&
                window.document().snapshot().entities().at("property-1")
                        .properties.at("calculation_profile")
                        .at("decimal_places")
                        .get<unsigned>() == 3,
            "calculation profile editor changes should be undoable and redoable");

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
    require(base_area->text().contains(QStringLiteral("6.000 m²")) &&
                factored_area->text().contains(QStringLiteral("4.500 m²")),
            "calculation values should refresh in metric display units");
    window.setMetricUnits(false);

    const auto wall_id = window.createStraightWall({0.0, 0.0}, {3.0, 0.0}, "exterior");
    require(!wall_id.isEmpty(), "straight wall should be accepted as a document command");
    const auto curved_wall_id = window.createCurvedWall(
        {8.0, 2.0}, {11.0, 2.0}, QStringLiteral("pi/2"), QStringLiteral("exterior"));
    require(!curved_wall_id.isEmpty(), "curved wall should be accepted as an analytical document command");
    const auto curved_wall_snapshot = window.document().snapshot().entities().at(curved_wall_id.toStdString());
    require(std::abs(curved_wall_snapshot.properties.at("baseline").at("sweep_radians").get<double>() -
                     std::numbers::pi / 2.0) < 1e-9 &&
                curved_wall_snapshot.extensions.at("curve_input").at("sweep") == "pi/2",
            "curved wall should retain its analytical sweep and source expression");
    require(window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.5"), QStringLiteral("80"),
                QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine"),
                curved_wall_id + QStringLiteral(", ") + wall_id),
            "architectural view source IDs should commit through typed Document history");
    const auto referenced_view_model = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    const auto referenced_view = std::find_if(
        referenced_view_model.views().begin(), referenced_view_model.views().end(),
        [](const auto& view) { return view.id == "view-plan"; });
    std::vector<std::string> expected_view_object_ids{wall_id.toStdString(),
                                                       curved_wall_id.toStdString()};
    std::sort(expected_view_object_ids.begin(), expected_view_object_ids.end());
    require(referenced_view != referenced_view_model.views().end() &&
                referenced_view->object_ids == expected_view_object_ids,
            "architectural view source IDs should persist in canonical sorted order");
    auto* referenced_architectural_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(referenced_architectural_canvas != nullptr,
            "architectural source filtering should expose the coordinated canvas");
    const auto has_architectural_entity = [&](const QString& id) {
        return std::find_if(referenced_architectural_canvas->entities().begin(),
                            referenced_architectural_canvas->entities().end(),
                            [&](const auto& entity) { return entity.id == id; }) !=
               referenced_architectural_canvas->entities().end();
    };
    require(has_architectural_entity(wall_id) && has_architectural_entity(curved_wall_id) &&
                !has_architectural_entity(boundary_id),
            "architectural plan should render only the explicitly referenced source objects");
    const auto revision_before_duplicate_view_ids = window.document().revision();
    require(!window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.5"), QStringLiteral("80"),
                QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine"),
                wall_id + QStringLiteral(", ") + wall_id) &&
                window.document().revision() == revision_before_duplicate_view_ids &&
                window.lastError().contains(QStringLiteral("unique"), Qt::CaseInsensitive),
            "duplicate architectural view source IDs should fail without mutation");
    require(window.selectEntity(wall_id), "created wall should be selectable");
    const sketch::DistoMeasurementRecord disto_height{
        {1, 0}, "desktop-reading-1", "wall.height", 2.8, "m",
        "2026-09-13T12:34:56Z", "synthetic-disto", "fixture-1",
        "offline-json", "desktop smoke fixture"};
    const auto disto_revision = window.document().revision();
    require(window.importDistoMeasurement(
                QString::fromStdString(sketch::disto_measurement_json(disto_height))) &&
                window.document().revision() == disto_revision + 2,
            "a local DISTO reading should update the selected field and retain a separate provenance command");
    const auto disto_snapshot = window.document().snapshot();
    const auto& disto_wall = disto_snapshot.entities().at(wall_id.toStdString());
    require(disto_wall.properties.at("height_m") == 2.8 &&
                disto_wall.extensions.at("disto_measurements").at("version") == 1 &&
                disto_wall.extensions.at("disto_measurements").at("fields").at("wall.height")
                        .at("reading_id") == "desktop-reading-1" &&
                disto_wall.extensions.at("disto_measurements").at("fields").at("wall.height")
                        .at("unit") == "m",
            "DISTO import should retain the selected value, original unit, reading identity, and provenance");
    const auto disto_duplicate_revision = window.document().revision();
    require(!window.importDistoMeasurement(
                QString::fromStdString(sketch::disto_measurement_json(disto_height))) &&
                window.document().revision() == disto_duplicate_revision &&
                window.lastError().contains(QStringLiteral("already has"), Qt::CaseInsensitive),
            "a second DISTO reading for an occupied field must fail without mutation");
    const sketch::DistoMeasurementRecord wrong_target{
        {1, 0}, "desktop-reading-2", "slab.thickness", 0.1, "m",
        "2026-09-13T12:35:56Z", "synthetic-disto", "fixture-1",
        "offline-json", "desktop smoke fixture"};
    const auto wrong_target_revision = window.document().revision();
    require(!window.importDistoMeasurement(
                QString::fromStdString(sketch::disto_measurement_json(wrong_target))) &&
                window.document().revision() == wrong_target_revision,
            "a DISTO target for another entity type must fail without mutation");
    require(window.editSelectedClassification("party"),
            "wall classification should be editable from the inspector API");
    require(window.editSelectedHeight("8 ft"),
            "wall height should parse and update through the inspector API");
    require(window.editSelectedThickness("6 in"),
            "wall thickness should parse and update through the inspector API");

    require(window.undoCommand(), "wall property edit should be undoable");
    require(window.redoCommand(), "wall property edit should be redoable");
    const auto layered_wall_json = QStringLiteral(
        "[{\"id\":\"outer\",\"thickness_m\":0.02},"
        "{\"id\":\"core\",\"thickness_m\":0.1124},"
        "{\"id\":\"inner\",\"thickness_m\":0.02}]");
    require(window.editSelectedWallLayers(layered_wall_json),
            "wall assembly layers should be editable through the document command seam");
    const auto layered_wall = window.document().snapshot().entities().at(wall_id.toStdString());
    require(layered_wall.properties.at("layers").is_array() &&
                layered_wall.properties.at("layers").size() == 3,
            "wall assembly layers should persist in canonical project JSON");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().at(wall_id.toStdString()).properties.contains("layers"),
            "wall assembly layer edit should be undoable");
    require(window.redoCommand() &&
                window.document().snapshot().entities().at(wall_id.toStdString()).properties.contains("layers"),
            "wall assembly layer edit should be redoable");

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
    require(window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.5"), QStringLiteral("80"),
                QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine"),
                opening_id),
            "architectural opening references should commit through typed history");
    auto* opening_reference_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(opening_reference_canvas != nullptr,
            "opening reference filtering should expose the coordinated canvas");
    const auto has_opening_reference_entity = [&](const QString& id) {
        return std::find_if(opening_reference_canvas->entities().begin(),
                            opening_reference_canvas->entities().end(),
                            [&](const auto& entity) { return entity.id == id; }) !=
               opening_reference_canvas->entities().end();
    };
    require(has_opening_reference_entity(wall_id) &&
                !has_opening_reference_entity(curved_wall_id),
            "opening-only architectural references should project their host wall dependency");
    require(window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.5"), QStringLiteral("80"),
                QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine")),
            "architectural view references should be clearable through typed history");

    require(window.selectEntity(boundary_id), "closed boundary should be selectable for slab creation");
    const auto revision_before_invalid_slab = window.document().revision();
    require(window.createSlabFromSelectedBoundary("0 m", "0 m").isEmpty(),
            "a zero thickness slab must be rejected before the document command");
    require(window.document().revision() == revision_before_invalid_slab,
            "rejected slab preview must not mutate the document");
    require(window.createSurfaceFromSelectedBoundary("roof", "0.15 m", "0 m").isEmpty(),
            "an unsupported horizontal assembly kind must be rejected before mutation");
    const auto floor_id = window.createSurfaceFromSelectedBoundary(
        "floor", "0.15 m", "0 m");
    require(!floor_id.isEmpty(), "a semantic floor should be created from the selected boundary");
    const auto floor_snapshot = window.document().snapshot();
    const auto floor_entity = floor_snapshot.entities().find(floor_id.toStdString());
    require(floor_entity != floor_snapshot.entities().end() &&
                floor_entity->second.type == "slab" &&
                floor_entity->second.properties.at("element_kind") == "floor",
            "semantic floor creation must persist a slab-compatible floor element kind");
    require(window.selectEntity(boundary_id), "the source boundary should remain selectable after floor creation");
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
    const auto layered_slab_json = QStringLiteral(
        "[{\"id\":\"structure\",\"thickness_m\":0.15},"
        "{\"id\":\"finish\",\"thickness_m\":0.05}]");
    require(window.editSelectedSlabLayers(layered_slab_json),
            "slab assembly layers should be editable through the document command seam");
    const auto layered_slab_entity = window.document().snapshot().entities().at(slab_id.toStdString());
    require(layered_slab_entity.properties.at("layers").is_array() &&
                layered_slab_entity.properties.at("layers").size() == 2,
            "slab assembly layers should persist in canonical project JSON");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().at(slab_id.toStdString()).properties.contains("layers"),
            "slab assembly layer edit should be undoable");
    require(window.redoCommand() &&
                window.document().snapshot().entities().at(slab_id.toStdString()).properties.contains("layers"),
            "slab assembly layer edit should be redoable");

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
    const auto image_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.png";
    const auto image_path_qstring = QString::fromStdWString(image_path.wstring());
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
    require(create_object_button && create_object_button->isHidden() &&
                edit_object_button && edit_object_button->isHidden(),
            "the 2D workspace must hide architectural object authoring controls");
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    require(!create_object_button->isHidden() && create_object_button->isEnabled() &&
                !edit_object_button->isHidden() && edit_object_button->isEnabled(),
            "building object tools must be available in the architectural workspace");
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
    require(architectural && architectural_view && architectural_view->count() >= 3 &&
                architectural_view->itemText(0) == QStringLiteral("Plan") &&
                architectural_view->itemText(1) == QStringLiteral("Elevation") &&
                architectural_view->itemText(2) == QStringLiteral("Section · 1.2 m"),
            "architectural view selector must retain standard views alongside named views");
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
    architectural_view->setCurrentIndex(2);
    const auto section_column = std::find_if(architectural->entities().begin(),
                                             architectural->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(section_column != architectural->entities().end() &&
                section_column->segments.size() == 4,
            "architectural section should intersect the same column with four edges");
    require(section_column->filled && section_column->hatch_pattern == QStringLiteral("solid") &&
                std::abs(section_column->hatch_scale - 1.0) < 1e-12,
            "architectural section projections should carry persisted material hatching metadata");
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
    const auto require_pdf_page_mm = [&](double width, double height) {
        QPdfDocument pdf;
        require(pdf.load(pdf_path_qstring) == QPdfDocument::Error::None,
                "sheet PDF should load for physical page measurement");
        const auto points = pdf.pagePointSize(0);
        require(std::abs(points.width() * 25.4 / 72.0 - width) < 0.4 &&
                    std::abs(points.height() * 25.4 / 72.0 - height) < 0.4,
                "PDF physical page must match persisted selected sheet millimetres");
        pdf.close();
    };
    require_pdf_page_mm(420.0, 297.0);
    auto* physical_page_preset = window.findChild<QComboBox*>(QStringLiteral("outputPageSize"));
    require(physical_page_preset != nullptr, "output page preset should exist");
    physical_page_preset->setCurrentText(QStringLiteral("Letter"));
    require(window.exportDraftPdf(pdf_path_qstring), "sheet PDF should export after preset change");
    require_pdf_page_mm(420.0, 297.0);
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
        const auto& metadata = resource.at("metadata");
        if (metadata.is_object() && metadata.contains("sheet_id") &&
            metadata.at("sheet_id") == added_sheet_id.toStdString()) {
            selected_sheet_bound = true;
            break;
        }
    }
    require(selected_sheet_bound, "selected-sheet output fingerprint must bind the requested page");
    const auto portrait_sheet = window.createDrawingSheet(QStringLiteral("A-202"),
        QStringLiteral("215.5"), QStringLiteral("330.2"), QStringLiteral("Custom portrait"));
    require(!portrait_sheet.isEmpty() && window.exportDraftPdf(pdf_path_qstring),
            "custom portrait sheet should export a PDF");
    require_pdf_page_mm(215.5, 330.2);
    require(window.exportDraftSvg(svg_path_qstring), "custom portrait sheet should export SVG");
    QFile physical_svg(svg_path_qstring);
    require(physical_svg.open(QIODevice::ReadOnly), "physical SVG should be readable");
    QXmlStreamReader physical_svg_xml(&physical_svg);
    require(physical_svg_xml.readNextStartElement() && physical_svg_xml.name() == QStringLiteral("svg"),
            "physical SVG must have a root element");
    const auto svg_attributes = physical_svg_xml.attributes();
    require(svg_attributes.value(QStringLiteral("width")) == QStringLiteral("215.5mm") &&
                svg_attributes.value(QStringLiteral("height")) == QStringLiteral("330.2mm"),
            "SVG physical dimensions must match the selected persisted portrait sheet");
    const auto view_box = svg_attributes.value(QStringLiteral("viewBox")).toString().split(' ');
    require(view_box.size() == 4 &&
                std::abs(view_box[2].toDouble() / view_box[3].toDouble() - 215.5 / 330.2) < 0.00001,
            "SVG viewBox aspect must match its persisted paper dimensions");
    physical_svg.close();
    require(window.exportDraftImage(image_path_qstring),
            "custom portrait sheet should export a PNG");
    QImage portrait_image(image_path_qstring);
    require(!portrait_image.isNull() &&
                portrait_image.size() == QSize(qRound(215.5 * 144.0 / 25.4),
                                               qRound(330.2 * 144.0 / 25.4)) &&
                std::abs(portrait_image.dotsPerMeterX() - qRound(144.0 / 0.0254)) <= 1 &&
                std::abs(portrait_image.dotsPerMeterY() - qRound(144.0 / 0.0254)) <= 1,
            "PNG pixels and metadata must preserve the persisted portrait sheet at 144 DPI");
    QFile preserved_png(image_path_qstring);
    require(preserved_png.open(QIODevice::ReadOnly), "portrait PNG should be readable before cap rejection");
    const auto preserved_png_bytes = preserved_png.readAll();
    preserved_png.close();
    const auto oversized_sheet = window.createDrawingSheet(QStringLiteral("A-203"),
        QStringLiteral("2000"), QStringLiteral("2000"), QStringLiteral("Raster cap fixture"));
    require(!oversized_sheet.isEmpty() && !window.exportDraftImage(image_path_qstring) &&
                window.lastError().contains(QStringLiteral("100 megapixel"), Qt::CaseInsensitive),
            "PNG export must reject a selected sheet above the raster allocation limit");
    require(preserved_png.open(QIODevice::ReadOnly) && preserved_png.readAll() == preserved_png_bytes,
            "rejected oversized PNG export must preserve the existing destination bytes");
    preserved_png.close();
    require(window.removeDrawingSheet(oversized_sheet), "oversized PNG fixture sheet should be removable");
    require(window.removeDrawingSheet(portrait_sheet), "custom portrait test sheet should be removable");
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
    require(window.editSheetSchedulePlacement(QStringLiteral("sheet-1"),
                                              QStringLiteral("schedule-objects"),
                                              QStringLiteral("215"), QStringLiteral("220"),
                                              QStringLiteral("195"), QStringLiteral("20")),
            "undersized schedule placement must remain editable for output overflow coverage");
    require(window.exportDraftSvg(svg_path_qstring),
            "undersized schedule placement should still produce an inspectable SVG");
    QFile undersized_schedule_svg(svg_path_qstring);
    require(undersized_schedule_svg.open(QIODevice::ReadOnly | QIODevice::Text),
            "undersized schedule SVG should be readable");
    const auto undersized_schedule_text = undersized_schedule_svg.readAll();
    require(undersized_schedule_text.contains("additional rows - enlarge schedule"),
            "undersized schedule output must identify omitted rows and request a larger placement");
    undersized_schedule_svg.close();
    require(window.editSheetSchedulePlacement(QStringLiteral("sheet-1"),
                                              QStringLiteral("schedule-objects"),
                                              QStringLiteral("215"), QStringLiteral("220"),
                                              QStringLiteral("195"), QStringLiteral("70")),
            "schedule placement should accept a height large enough for all fixture rows");
    require(window.exportDraftSvg(svg_path_qstring),
            "adequately sized schedule placement should export through the shared SVG path");
    QFile adequate_schedule_svg(svg_path_qstring);
    require(adequate_schedule_svg.open(QIODevice::ReadOnly | QIODevice::Text),
            "adequately sized schedule SVG should be readable");
    const auto adequate_schedule_text = adequate_schedule_svg.readAll();
    require(!adequate_schedule_text.contains("additional rows - enlarge schedule"),
            "adequately sized schedule output must not report a false overflow");
    adequate_schedule_svg.close();
    const auto revision_id = window.addSheetRevision(QStringLiteral("sheet-1"),
                                                     QStringLiteral("2026-09-12"),
                                                     QStringLiteral("Permit set"));
    require(!revision_id.isEmpty(), "sheet revision should be created through Document history");
    auto sheet_with_revision = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    require(sheet_with_revision.sheets().front().revisions.size() == 1 &&
                sheet_with_revision.sheets().front().revisions.front().id == revision_id.toStdString(),
            "sheet revision should persist in the canonical sheet/view entity");
    require(window.editSheetRevision(QStringLiteral("sheet-1"), revision_id,
                                     QStringLiteral("2026-09-13"), QStringLiteral("Issued set")),
            "sheet revision edit should use the typed command path");
    require(window.undoCommand() && window.redoCommand(),
            "sheet revision edit must participate in normal document history");
    const auto callout_id = window.addSheetCallout(QStringLiteral("sheet-1"),
                                                   QStringLiteral("Section A"),
                                                   QStringLiteral("sheet-1"),
                                                   QStringLiteral("viewport-section"),
                                                   QStringLiteral("30"), QStringLiteral("30"));
    require(!callout_id.isEmpty(), "sheet callout should be created through Document history");
    auto sheet_with_callout = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    require(sheet_with_callout.sheets().front().callouts.size() == 1 &&
                sheet_with_callout.sheets().front().callouts.front().id == callout_id.toStdString(),
            "sheet callout should persist in the canonical sheet/view entity");
    require(window.editSheetCallout(QStringLiteral("sheet-1"), callout_id,
                                    QStringLiteral("Section A / A101"), QStringLiteral("sheet-1"),
                                    QStringLiteral("viewport-section"), QStringLiteral("35"),
                                    QStringLiteral("35")),
            "sheet callout edit should use the typed command path");
    require(window.undoCommand() && window.redoCommand(),
            "sheet callout edit must participate in normal document history");
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
                pdf_fingerprint_json.at("fingerprint").at("digest_sha256").is_string() &&
                pdf_fingerprint_json.at("output_sha256").is_string(),
            "draft PDF fingerprint must identify the output and digest");
    QFile pdf_bytes(pdf_path_qstring);
    require(pdf_bytes.open(QIODevice::ReadOnly), "draft PDF bytes should be readable for evidence");
    const auto pdf_digest = QCryptographicHash::hash(pdf_bytes.readAll(), QCryptographicHash::Sha256)
                                .toHex().toStdString();
    pdf_bytes.close();
    require(pdf_fingerprint_json.at("output_sha256") == pdf_digest,
            "draft PDF fingerprint must bind the committed PDF bytes");
    const auto& processing_roles = pdf_fingerprint_json.at("fingerprint").at("manifest")
                                       .at("dependencies").at("processing_components").at("roles");
    bool runtime_dependency_bound = false;
    for (const auto& [role_name, role] : processing_roles.items()) {
        (void)role_name;
        for (const auto& resource : role.at("resources")) {
            if (resource.at("metadata").value("identity", "") == "verified-runtime-binary" ||
                resource.at("metadata").value("identity", "") == "developer-runtime-binary") {
                runtime_dependency_bound = true;
            }
            require(resource.at("metadata").value("identity", "") !=
                        "statically-linked-into-application",
                    "processing fingerprints must not mislabel dynamic runtime DLLs as static");
        }
    }
    require(runtime_dependency_bound,
            "processing fingerprints must bind an observed runtime dependency set");
    pdf_fingerprint.close();
    require(window.exportDraftSvg(svg_path_qstring), "draft SVG export should succeed locally");
    require(std::filesystem::file_size(svg_path) > 0, "draft SVG should be nonempty");
    QFile svg_output(svg_path_qstring);
    require(svg_output.open(QIODevice::ReadOnly | QIODevice::Text),
            "draft SVG should be readable for schedule placement verification");
    const auto svg_text = svg_output.readAll();
    require(svg_text.contains("DOORS SCHEDULE") &&
                svg_text.contains("BUILDING-OBJECTS SCHEDULE") &&
                svg_text.contains("ELEVATION") &&
                svg_text.contains("SECTION") && svg_text.contains("Issued set") &&
                svg_text.contains("Section A / A101"),
            "draft SVG should render building schedules, coordinated captions, revisions and callouts");
    svg_output.close();
    const auto svg_fingerprint_path = std::filesystem::path(svg_path.wstring() + L".fingerprint.json");
    require(std::filesystem::file_size(svg_fingerprint_path) > 0,
            "draft SVG must have an adjacent output fingerprint");
    require(window.exportDraftImage(image_path_qstring), "draft PNG export should succeed locally");
    require(std::filesystem::file_size(image_path) > 0, "draft PNG should be nonempty");
    QImage draft_image(image_path_qstring);
    const auto raster_sheet_model = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    const auto raster_sheet = std::find_if(raster_sheet_model.sheets().begin(),
        raster_sheet_model.sheets().end(), [&](const auto& sheet) {
            return sheet.id == window.outputSheetId().toStdString();
        });
    require(raster_sheet != raster_sheet_model.sheets().end(),
            "draft PNG must resolve the selected persisted drawing sheet");
    const QSize expected_raster_size(qRound(raster_sheet->width_mm * 144.0 / 25.4),
                                     qRound(raster_sheet->height_mm * 144.0 / 25.4));
    require(!draft_image.isNull() && draft_image.size() == expected_raster_size &&
                std::abs(draft_image.dotsPerMeterX() - qRound(144.0 / 0.0254)) <= 1 &&
                std::abs(draft_image.dotsPerMeterY() - qRound(144.0 / 0.0254)) <= 1,
            "draft PNG must preserve the selected sheet aspect and physical size at 144 DPI");
    const auto image_fingerprint_path = std::filesystem::path(image_path.wstring() + L".fingerprint.json");
    QFile image_fingerprint(QString::fromStdWString(image_fingerprint_path.wstring()));
    require(image_fingerprint.open(QIODevice::ReadOnly | QIODevice::Text),
            "draft PNG fingerprint should be readable");
    const auto image_fingerprint_json = nlohmann::json::parse(image_fingerprint.readAll().toStdString());
    require(image_fingerprint_json.at("output_kind") == "png" &&
                image_fingerprint_json.at("fingerprint").at("digest_sha256").is_string(),
            "draft PNG fingerprint must identify the output and digest");
    image_fingerprint.close();
    auto* page_size = window.findChild<QComboBox*>(QStringLiteral("outputPageSize"));
    require(page_size && page_size->count() == 5, "output sheet selector should expose five page sizes");
    page_size->setCurrentText(QStringLiteral("A3"));
    if (!window.exportDraftPdf(pdf_path_qstring)) {
        throw std::runtime_error("A3 draft PDF export should succeed locally: " +
                                 window.lastError().toStdString());
    }
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
    require(reopened.entities().contains(curved_wall_id.toStdString()) &&
                std::abs(reopened.entities().at(curved_wall_id.toStdString()).properties
                             .at("baseline").at("sweep_radians").get<double>() -
                         std::numbers::pi / 2.0) < 1e-9 &&
                reopened.entities().at(curved_wall_id.toStdString()).extensions
                             .at("curve_input").at("sweep") == "pi/2",
            "save/reopen must preserve the analytical curved wall and its source expression");
    require(reopened.entities().contains(opening_id.toStdString()),
            "reopened project should preserve the hosted opening entity");
    require(reopened.entities().contains(slab_id.toStdString()),
            "reopened project should preserve the slab entity");
    require(reopened.entities().contains(floor_id.toStdString()) &&
                reopened.entities().at(floor_id.toStdString()).properties.at("element_kind") == "floor",
            "reopened project should preserve the semantic floor element kind");
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
    require(window.selectEntity(column_id), "building object should be selected for its transform editor");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("architecturalTransformDialog"));
        require(dialog, "architectural object selection should open its semantic transform editor");
        require(dialog->findChild<QLineEdit*>(QStringLiteral("architecturalTransformScale")) != nullptr &&
                    dialog->findChild<QLineEdit*>(QStringLiteral("architecturalTransformOffsetZ")) != nullptr,
                "architectural transform editor must expose scale and vertical translation");
        dialog->reject();
    });
    window.showBoundaryTransformEditor();
    require(window.selectEntity(column_id) &&
                window.transformSelectedArchitecturalObject(QStringLiteral("90"),
                    QStringLiteral("1 m"), QStringLiteral("2 m"), QStringLiteral("0.5 m"),
                    QStringLiteral("2"), false),
            "selected architectural objects must support semantic transforms through the desktop command path");
    const auto transformed_column = sketch::decode_building_entity(
        window.document().snapshot().entities().at(column_id.toStdString()));
    const auto& transformed_rectangular = std::get<sketch::RectangularColumn>(transformed_column);
    require(std::abs(transformed_rectangular.base_center.x + 3.0) < 1e-7 &&
                std::abs(transformed_rectangular.base_center.y - 4.0) < 1e-7 &&
                std::abs(transformed_rectangular.base_center.z - 0.5) < 1e-7 &&
                std::abs(transformed_rectangular.width - 0.6) < 1e-7 &&
                std::abs(transformed_rectangular.height - 7.0) < 1e-7 &&
                window.document().snapshot().entities().at(column_id.toStdString()).extensions.at("private_note") ==
                    "preserve this",
            "architectural object transforms must update canonical dimensions and preserve metadata");
    require(window.undoCommand() && window.redoCommand(),
            "architectural object transforms must participate in normal undo and redo");
    require(window.selectEntity(column_id) &&
                window.transformSelectedArchitecturalObject(QStringLiteral("0"),
                    QStringLiteral("1 m"), QStringLiteral("0 m"), QStringLiteral("0 m"),
                    QStringLiteral("1"), true),
            "architectural object transforms must support creating a selected copy");
    const auto copied_column_id = window.selectedEntityId();
    require(!copied_column_id.isEmpty() && copied_column_id != column_id &&
                window.document().snapshot().entities().contains(copied_column_id.toStdString()) &&
                window.document().snapshot().entities().contains(column_id.toStdString()),
            "architectural object clone must preserve the source and select the copy");
    require(window.selectEntity(boundary_id), "reopened boundary should be selectable");
    require(edit_object_button->isHidden(), "building object editor must hide for a measurement boundary");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                base_area->text().contains(QStringLiteral("6.000 m²")) &&
                factored_area->text().contains(QStringLiteral("4.500 m²")),
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
    const auto protected_png =
        QString::fromStdWString((project_path.parent_path() / "must-not-exist.png").wstring());
    require(!window.exportDraftPdf(protected_output) && !window.exportDraftSvg(protected_svg) &&
                !window.exportDraftImage(protected_png) &&
                !window.showPrintPreview() &&
                !std::filesystem::exists(std::filesystem::path(protected_output.toStdWString())) &&
                !std::filesystem::exists(std::filesystem::path(protected_svg.toStdWString())) &&
                !std::filesystem::exists(std::filesystem::path(protected_png.toStdWString())),
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
    // The malformed entity above is injected directly through the public
    // document fixture API, outside the workspace command ledger. Reverse it
    // through that same test-only path; desktop commands deliberately reject
    // out-of-band divergence instead of replacing recovery history.
    window.document().undo(window.document().revision());
    require(window.selectEntity(column_id), "restored building geometry should be selectable");
    require(plan_error->isHidden(), "geometry error must clear after the valid object is restored");
    return 0;
}
