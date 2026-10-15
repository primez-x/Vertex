// File > Open recent, Edit > Select all and Edit > Duplicate.
#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/preferences_dialog.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QLineEdit>
#include <QDialog>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace sketch;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

QString normalized(const QString& path) {
    return QDir::toNativeSeparators(QFileInfo(path).canonicalFilePath());
}

Boundary rectangle(double left, double bottom, double right, double top) {
    return Boundary{
        {{left, bottom}, {right, bottom}, 0.0},
        {{right, bottom}, {right, top}, 0.0},
        {{right, top}, {left, top}, 0.0},
        {{left, top}, {left, bottom}, 0.0},
    };
}

// Optional captures exercise the real widgets without touching user settings.
void capture_navigation() {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "create navigation capture directory");
    desktop::MainWindow window;
    window.resize(1366, 768);
    window.show();
    QApplication::processEvents();
    for (const auto workspace : {desktop::Workspace::measurement, desktop::Workspace::architectural}) {
        window.setWorkspace(workspace);
        QApplication::processEvents();
        const auto prefix = workspace == desktop::Workspace::measurement ? QStringLiteral("measure") : QStringLiteral("design");
        require(window.grab().save(QDir(directory).filePath(prefix + QStringLiteral("-workspace.png"))), "capture workspace");
        for (auto* action : window.menuBar()->actions()) {
            auto* menu = action->menu();
            if (!menu) continue;
            menu->popup(window.menuBar()->mapToGlobal(window.menuBar()->actionGeometry(action).bottomLeft()));
            QApplication::processEvents();
            require(menu->grab().save(QDir(directory).filePath(prefix + QLatin1Char('-') + menu->objectName() + QStringLiteral(".png"))), "capture menu");
            menu->hide();
        }
    }
    window.resize(1024, 768);
    QApplication::processEvents();
    require(window.grab().save(QDir(directory).filePath(QStringLiteral("design-compact.png"))), "capture compact workspace");
    for (const auto theme : {WorkspaceTheme::dark, WorkspaceTheme::light,
                            WorkspaceTheme::high_contrast}) {
        window.setWorkspaceTheme(theme);
        for (const auto* id : {"architecturalAuthoringMenu", "buildingObjectsMenu", "viewPanelsMenu"}) {
            auto* menu = window.findChild<QMenu*>(QString::fromLatin1(id));
            if (!menu) continue;
            menu->popup(window.mapToGlobal(QPoint(40, 80)));
            QApplication::processEvents();
            require(menu->grab().save(QDir(directory).filePath(QStringLiteral("theme-%1-%2.png")
                    .arg(static_cast<int>(theme)).arg(QString::fromLatin1(id)))), "capture themed menu");
            menu->hide();
        }
    }
}

void test_menu_entries() {
    desktop::MainWindow window;
    auto* file_menu = window.findChild<QMenu*>(QStringLiteral("fileMenu"));
    auto* recent = window.findChild<QMenu*>(QStringLiteral("openRecentMenu"));
    require(file_menu && recent && recent->menuAction() &&
                file_menu->actions().contains(recent->menuAction()),
            "File menu must contain the Open recent submenu");
    auto* edit_menu = window.findChild<QMenu*>(QStringLiteral("editMenu"));
    auto* select_all = window.findChild<QAction*>(QStringLiteral("selectAll"));
    auto* duplicate = window.findChild<QAction*>(QStringLiteral("duplicateSelection"));
    require(edit_menu && select_all && duplicate && edit_menu->actions().contains(select_all) &&
                edit_menu->actions().contains(duplicate),
            "Edit menu must contain Select all and Duplicate");
    require(select_all->shortcut() == QKeySequence(QKeySequence::SelectAll), "Select all uses Ctrl+A");
    require(duplicate->shortcut() == QKeySequence(QStringLiteral("Ctrl+D")), "Duplicate uses Ctrl+D");
    // No other window action may claim either shortcut.
    for (auto* action : window.findChildren<QAction*>()) {
        if (action == select_all || action == duplicate) continue;
        require(action->shortcut() != select_all->shortcut() && action->shortcut() != duplicate->shortcut(),
                "shortcut conflict with " + action->objectName().toStdString());
    }
}

void test_navigation_access() {
    desktop::MainWindow window;
    window.resize(1366, 768);
    window.show();
    QApplication::processEvents();
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("primaryToolbar"));
    require(toolbar != nullptr, "drawing toolbar exists");
    for (const auto* id : {"drawModeWall", "drawModeArea", "drawModeMeasuredLines"}) {
        auto* action = window.findChild<QAction*>(QString::fromLatin1(id));
        auto* button = action ? qobject_cast<QToolButton*>(toolbar->widgetForAction(action)) : nullptr;
        require(button && button->toolButtonStyle() == Qt::ToolButtonTextBesideIcon &&
                    button->width() >= button->sizeHint().width(),
                std::string("drawing tool must show its full label: ") + id);
    }
    auto* layout = window.findChild<QAction*>(QStringLiteral("viewportEditor"));
    // Preserve the established identity if the label changes.
    if (!layout) layout = window.findChild<QAction*>(QStringLiteral("sheetLayoutManager"));
    require(layout && layout->isVisible(), "sheet layout is available in Measure");
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("sidebarTabs"));
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("workspaceSplitter"));
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(tabs && splitter, "navigation panel exists");
    require(tree && !tree->accessibleName().isEmpty(), "navigator has an accessible name");
    const auto revision = window.document().revision();
    for (int index = 0; index < 3; ++index) {
        splitter->setSizes({0, 1000});
        const auto id = QStringList{QStringLiteral("showLayersPanel"), QStringLiteral("showLibraryPanel"), QStringLiteral("showDetailsPanel")}.at(index);
        auto* action = window.findChild<QAction*>(id);
        require(action && action->isVisible() && action->isEnabled(), "View can reopen every navigation tab");
        action->trigger();
        QApplication::processEvents();
        require(tabs->currentIndex() == index && splitter->sizes().front() > 0,
                "View command selects the requested tab and restores a collapsed panel");
    }
    require(window.document().revision() == revision, "navigation does not edit the project");
    window.setWorkspace(desktop::Workspace::architectural);
    QApplication::processEvents();
    auto* view_choice = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    require(view_choice && view_choice->isVisible() &&
                toolbar->rect().contains(QRect(view_choice->mapTo(toolbar, QPoint()), view_choice->size())),
            "Design view selector stays visible in the laptop header");
    auto* draw = window.findChild<QMenu*>(QStringLiteral("architecturalAuthoringMenu"));
    require(draw, "Draw menu exists");
    draw->ensurePolished();
    draw->adjustSize();
    require(draw->sizeHint().height() <= 600 && draw->sizeHint().width() <= 480,
            "Draw fits in one readable column on an ordinary laptop");
    require(window.findChild<QMenu*>(QStringLiteral("buildingObjectsMenu")), "building objects have a focused submenu");
}

void test_focused_text_commands() {
    desktop::MainWindow window;
    window.show();
    auto* select_all = window.findChild<QAction*>(QStringLiteral("selectAll"));
    auto* copy = window.findChild<QAction*>(QStringLiteral("copySelection"));
    auto* cut = window.findChild<QAction*>(QStringLiteral("cutSelection"));
    auto* paste = window.findChild<QAction*>(QStringLiteral("pasteSelection"));
    auto* erase = window.findChild<QAction*>(QStringLiteral("deleteSelection"));
    require(select_all && copy && cut && paste && erase, "Edit text commands exist");
    const auto revision = window.document().revision();
    QLineEdit single(&window);
    QPlainTextEdit plain(&window);
    QTextEdit rich(&window);
    for (auto* editor : {static_cast<QWidget*>(&single), static_cast<QWidget*>(&plain), static_cast<QWidget*>(&rich)}) {
        editor->setGeometry(400, 120, 240, 100);
        editor->show();
        editor->raise();
        editor->setFocus();
        QApplication::processEvents();
        const auto set_text = [&](const QString& text) {
            if (editor == &single) single.setText(text);
            else if (editor == &plain) plain.setPlainText(text);
            else rich.setPlainText(text);
        };
        const auto text = [&] {
            return editor == &single ? single.text() : editor == &plain ? plain.toPlainText() : rich.toPlainText();
        };
        set_text(QStringLiteral("Example"));
        QApplication::clipboard()->setText(QStringLiteral("sentinel"));
        select_all->trigger();
        copy->trigger();
        require(QApplication::clipboard()->text() == QStringLiteral("Example"), "Edit > Select all and Copy operate on focused text");
        cut->trigger();
        require(text().isEmpty(), "Edit > Cut operates on focused text");
        paste->trigger();
        require(text() == QStringLiteral("Example"), "Edit > Paste operates on focused text");
        select_all->trigger();
        erase->trigger();
        require(text().isEmpty(), "Edit > Delete operates on focused text");
        set_text(QStringLiteral("Read only"));
        if (editor == &single) single.setReadOnly(true);
        else if (editor == &plain) plain.setReadOnly(true);
        else rich.setReadOnly(true);
        select_all->trigger();
        QApplication::clipboard()->setText(QStringLiteral("sentinel"));
        copy->trigger();
        require(QApplication::clipboard()->text() == QStringLiteral("Read only"), "read-only text can still be copied");
        cut->trigger();
        erase->trigger();
        QApplication::clipboard()->setText(QStringLiteral("Replacement"));
        paste->trigger();
        require(text() == QStringLiteral("Read only"), "Edit commands preserve read-only text");
        editor->hide();
    }
    require(window.document().revision() == revision, "text editing does not alter project history");
}

void test_command_search_discovery() {
    desktop::MainWindow window;
    window.show();
    for (const auto& query : {QStringLiteral("Appraisal report PDF"), QStringLiteral("Site placement"),
                              QStringLiteral("georeferencing"), QStringLiteral("Edit dimensions")}) {
        bool found = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* search = dialog->findChild<QLineEdit*>();
            auto* list = dialog->findChild<QListWidget*>();
            if (search && list) {
                search->setText(query);
                found = list->count() == 1 && list->item(0)->text().contains(query, Qt::CaseInsensitive) &&
                    list->item(0)->text().contains(QStringLiteral(" > "));
            }
            dialog->reject();
        });
        window.showCommandPalette();
        require(found, "command search finds the existing menu label once: " + query.toStdString());
    }
    bool search_hygiene = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* search = dialog->findChild<QLineEdit*>();
        auto* list = dialog->findChild<QListWidget*>();
        if (search && list) {
            search->setText(QStringLiteral("Command search"));
            const auto no_recursion = list->count() == 0;
            search->setText(QStringLiteral("Draw curved wall"));
            const auto no_hidden_design_command = list->count() == 0;
            search->setText(QStringLiteral("View > Panels >"));
            search_hygiene = no_recursion && no_hidden_design_command && list->count() == 3;
        }
        dialog->reject();
    });
    window.showCommandPalette();
    require(search_hygiene, "search supports menu paths, omits itself and omits hidden workspace actions");
}

void test_command_search_execution() {
    desktop::MainWindow window;
    window.show();
    const auto first = window.createBoundary(rectangle(0, 0, 4, 3), QStringLiteral("living_area"));
    const auto second = window.createBoundary(rectangle(10, 0, 14, 3), QStringLiteral("living_area"));
    require(!first.isEmpty() && !second.isEmpty() && window.selectEntity(first), "command search selection fixture");
    auto* canvas = window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas"));
    require(canvas, "plan canvas exists");
    const auto invoke = [&](const QString& query, bool from_list = false) {
        bool invoked = false;
        canvas->setFocus();
        QApplication::processEvents();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* search = dialog->findChild<QLineEdit*>();
            auto* list = dialog->findChild<QListWidget*>();
            if (search && list) {
                search->setText(query);
                if (list->count() == 1 && list->currentItem()) {
                    invoked = true;
                    if (from_list) {
                        list->setFocus();
                        Q_EMIT list->itemActivated(list->currentItem());
                    } else QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
                }
            }
            if (!invoked) dialog->reject();
        });
        window.showCommandPalette();
        QApplication::processEvents();
        require(invoked, "selection command resolves once: " + query.toStdString());
    };
    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    invoke(QStringLiteral("Copy selection"));
    require(QApplication::clipboard()->text() != QStringLiteral("Copy selection") &&
                QApplication::clipboard()->text() != QStringLiteral("sentinel"),
            "command search copies the drawing rather than its query");
    const auto revision = window.document().revision();
    invoke(QStringLiteral("Duplicate selection"));
    require(window.document().revision() == revision + 1, "command search Duplicate edits the drawing once");
    require(window.selectEntity(QString{}), "clear selection before command search Select all");
    invoke(QStringLiteral("Select all"), true);
    require(window.selectedEntityIds().contains(first) && window.selectedEntityIds().contains(second),
            "list-activated Select all selects the drawing rather than search results");
}

void test_recent_projects(const QTemporaryDir& files) {
    {
        // Test mode keeps recording off unless a test opts in.
        QSettings settings;
        settings.setValue(QStringLiteral("recentProjects"), QStringList{files.filePath(QStringLiteral("seed.bldproj"))});
    }
    desktop::MainWindow window;
    require(window.recentProjects().isEmpty(), "recent projects are disabled by default in test mode");
    window.setRecentProjectsEnabled(true);
    window.clearRecentProjects();
    require(window.recentProjects().isEmpty(), "clear empties the list");
    require(!window.createBoundary(rectangle(0, 0, 4, 3), QStringLiteral("living_area")).isEmpty(),
            "fixture boundary: " + window.lastError().toStdString());

    const auto a = files.filePath(QStringLiteral("alpha.bldproj"));
    const auto b = files.filePath(QStringLiteral("beta & co.bldproj"));
    require(window.saveProjectAs(a), "save alpha: " + window.lastError().toStdString());
    require(window.saveProjectAs(b), "save beta: " + window.lastError().toStdString());
    require(window.recentProjects() == QStringList({normalized(b), normalized(a)}),
            "Save as records most recent first");
    require(window.openProject(a), "open alpha: " + window.lastError().toStdString());
    require(window.recentProjects() == QStringList({normalized(a), normalized(b)}),
            "open moves the project to the front without duplicating it");
    // A differently spelled path to the same file is one entry.
    require(window.openProject(QDir(files.path()).filePath(QStringLiteral("./alpha.bldproj"))), "reopen alpha");
    require(window.recentProjects().size() == 2, "normalized paths do not duplicate entries");

    // Menu presentation: file names, full-path tooltips and Clear recent.
    auto* menu = window.findChild<QMenu*>(QStringLiteral("openRecentMenu"));
    require(menu != nullptr, "Open recent menu exists");
    Q_EMIT menu->aboutToShow();
    auto* first = menu->findChild<QAction*>(QStringLiteral("recentProject1"));
    auto* second = menu->findChild<QAction*>(QStringLiteral("recentProject2"));
    auto* clear = menu->findChild<QAction*>(QStringLiteral("clearRecentProjects"));
    require(first && second && clear && clear->isEnabled(), "menu lists both projects and Clear recent");
    require(first->text().contains(QStringLiteral("alpha.bldproj")) && first->toolTip() == normalized(a),
            "entry shows the file name with the full path as tooltip");
    require(second->text().contains(QStringLiteral("beta && co.bldproj")), "ampersands are escaped");

    // Capped at ten.
    for (int index = 0; index < 11; ++index)
        require(window.saveProjectAs(files.filePath(QStringLiteral("cap-%1.bldproj").arg(index))),
                "save cap file: " + window.lastError().toStdString());
    const auto capped = window.recentProjects();
    require(capped.size() == 10 && capped.front() == normalized(files.filePath(QStringLiteral("cap-10.bldproj"))),
            "list keeps the ten most recent projects");

    // Missing file: removed with a fact-then-fix status message.
    const auto missing = normalized(files.filePath(QStringLiteral("cap-9.bldproj")));
    require(capped.contains(missing) && QFile::remove(missing), "remove a listed project from disk");
    require(!window.openRecentProject(missing), "a missing recent project does not open");
    require(!window.recentProjects().contains(missing), "missing project is removed from the list");
    require(!QSettings().value(QStringLiteral("recentProjects")).toStringList().contains(missing),
            "missing project is removed from settings");
    const auto message = window.statusBar()->currentMessage();
    require(message.contains(QStringLiteral("cap-9.bldproj")) && message.contains(QStringLiteral("removed from Open recent")),
            "status explains what happened: " + message.toStdString());

    // Stored entries for files that no longer exist are skipped on read.
    QSettings().setValue(QStringLiteral("recentProjects"),
                         QStringList{files.filePath(QStringLiteral("never.bldproj")), a});
    require(window.recentProjects() == QStringList{normalized(a)}, "only existing files are listed");

    Q_EMIT menu->aboutToShow();
    clear = menu->findChild<QAction*>(QStringLiteral("clearRecentProjects"));
    require(clear && clear->isEnabled(), "Clear recent is enabled with entries");
    clear->trigger();
    require(window.recentProjects().isEmpty() && !QSettings().contains(QStringLiteral("recentProjects")),
            "Clear recent empties the list and settings");
}

void test_reopen_last_project(const QTemporaryDir& files) {
    QString path;
    std::size_t entity_count{};
    {
        desktop::MainWindow source;
        source.setRecentProjectsEnabled(true);
        source.clearRecentProjects();
        require(!source.createBoundary(rectangle(0, 0, 5, 5), QStringLiteral("living_area")).isEmpty(), "fixture");
        path = files.filePath(QStringLiteral("reopen.bldproj"));
        require(source.saveProjectAs(path), "save reopen fixture: " + source.lastError().toStdString());
        entity_count = source.document().snapshot().entities().size();
    }
    {
        desktop::MainWindow window;
        window.setRecentProjectsEnabled(true);
        require(window.recentProjects().value(0) == normalized(path), "fixture is the most recent project");
        require(!window.reopenLastProject(), "reopen is off by default");
        require(window.document().snapshot().entities().size() != entity_count,
                "window remains untitled when reopen is off");
    }
    {
        QSettings settings;
        auto preferences = desktop::Preferences::defaults();
        preferences.reopen_last_project = true;
        preferences.save(settings);
        settings.sync();
    }
    {
        desktop::MainWindow window;
        require(!window.reopenLastProject(), "test-mode windows never reopen unless recording is enabled");
        window.setRecentProjectsEnabled(true);
        require(window.reopenLastProject(), "reopen opens the most recent project: " + window.lastError().toStdString());
        require(window.document().snapshot().entities().size() == entity_count, "the saved project was opened");
        require(!window.reopenLastProject(), "reopen never replaces an open project");
    }
    {
        QSettings settings;
        settings.remove(QStringLiteral("preferences"));
        settings.remove(QStringLiteral("recentProjects"));
    }
}

void test_select_all_and_duplicate() {
    desktop::MainWindow window;
    const auto first = window.createBoundary(rectangle(0, 0, 4, 3), QStringLiteral("living_area"));
    const auto second = window.createBoundary(rectangle(10, 0, 14, 3), QStringLiteral("living_area"));
    require(!first.isEmpty() && !second.isEmpty(), "fixture boundaries: " + window.lastError().toStdString());
    require(window.selectEntity(QString{}), "clear selection");

    require(window.selectAll(), "select all: " + window.lastError().toStdString());
    const auto all = window.selectedEntityIds();
    require(all.contains(first) && all.contains(second), "select all includes both visible areas");
    const auto count = all.size();
    require(window.selectAll() && window.selectedEntityIds().size() == count,
            "select all is idempotent and replaces rather than extends");

    // Duplicate one area: one revision, the copy becomes the selection.
    require(window.selectEntity(first), "select first area");
    const auto before = window.document().snapshot();
    require(window.duplicateSelection(), "duplicate: " + window.lastError().toStdString());
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1, "duplicate commits exactly one revision");
    require(after.entities().size() > before.entities().size(), "duplicate adds the copy");
    const auto copies = window.selectedEntityIds();
    require(copies.size() == 1 && copies.front() != first &&
                after.entities().contains(copies.front().toStdString()) &&
                !before.entities().contains(copies.front().toStdString()),
            "the new copy is selected");
    for (const auto& [id, entity] : before.entities())
        require(after.entities().contains(id) && after.entities().at(id) == entity, "duplicate leaves sources unchanged");
    require(window.undoCommand(), "undo duplicate");
    require(window.document().snapshot().entities() == before.entities(), "one undo removes the copy");

    // Duplicate both areas at once.
    require(window.selectAll(), "select all again");
    const auto group_before = window.document().snapshot();
    require(window.duplicateSelection(), "duplicate group: " + window.lastError().toStdString());
    require(window.document().snapshot().revision() == group_before.revision() + 1, "group duplicate is one revision");
    const auto group = window.selectedEntityIds();
    require(group.size() >= 2 && !group.contains(first) && !group.contains(second), "copies replace the selection");
    require(window.undoCommand() && window.document().snapshot().entities() == group_before.entities(),
            "one undo removes the group copy");

    require(window.selectEntity(QString{}), "clear selection");
    require(!window.duplicateSelection() && !window.lastError().isEmpty(), "duplicate needs a selection");
}
// File > Export: sketch and appraisal report first; sheet output says why it
// is unavailable when no drawing sheet exists.
void test_export_menu() {
    desktop::MainWindow window;
    auto* export_menu = window.findChild<QMenu*>(QStringLiteral("exportMenu"));
    auto* sheet = window.findChild<QAction*>(QStringLiteral("exportSheetPdf"));
    auto* sketch_pdf = window.findChild<QAction*>(QStringLiteral("exportSketchPdf"));
    auto* report = window.findChild<QAction*>(QStringLiteral("exportAppraisalReportPdf"));
    require(export_menu && sheet && sketch_pdf && report, "Export menu has sketch, report and sheet actions");
    const auto actions = export_menu->actions();
    require(actions.indexOf(sketch_pdf) == 0 && actions.indexOf(report) == 1 && actions.contains(sheet),
            "Export lists Export sketch PDF, then Appraisal report PDF");
    QMetaObject::invokeMethod(export_menu, "aboutToShow");
    require(sheet->isEnabled() && report->isEnabled(),
            "the default project has a drawing sheet and a property to report on");

    // A project without drawing sheets (for example an imported sketch).
    QTemporaryDir directory;
    auto document = std::make_shared<Document>(Document::create({
        {"p", "property", {{"name", "No sheets"}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}, {"name", "Plan"}}, false}}));
    desktop::MainWindow bare(document, nullptr, directory.filePath(QStringLiteral("text-library.json")));
    auto* bare_menu = bare.findChild<QMenu*>(QStringLiteral("exportMenu"));
    auto* bare_sheet = bare.findChild<QAction*>(QStringLiteral("exportSheetPdf"));
    auto* bare_set = bare.findChild<QAction*>(QStringLiteral("exportDrawingSetPdf"));
    require(bare_menu && bare_sheet && bare_set, "Export menu exists without sheets");
    QMetaObject::invokeMethod(bare_menu, "aboutToShow");
    require(!bare_sheet->isEnabled() && bare_sheet->toolTip().contains(QStringLiteral("No drawing sheets")) &&
                !bare_set->isEnabled(),
            "sheet output is disabled with a reason when no sheet exists");
    require(bare_sheet->toolTip().contains(QStringLiteral("Output > Drawing sheets")),
            "no-sheet hint points to the first-sheet creation path");
    bool guarded_export = false;
    QTimer::singleShot(0, &bare, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* search = dialog->findChild<QLineEdit*>();
        auto* list = dialog->findChild<QListWidget*>();
        if (search && list) {
            search->setText(QStringLiteral("Export selected sheet PDF"));
            guarded_export = list->count() == 1 && !(list->item(0)->flags() & Qt::ItemIsEnabled);
        }
        dialog->reject();
    });
    bare.showCommandPalette();
    require(guarded_export, "command search exposes the same disabled sheet-export action");
    bool enabled_result = false;
    QTimer::singleShot(0, &bare, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* search = dialog->findChild<QLineEdit*>();
        auto* list = dialog->findChild<QListWidget*>();
        if (search && list) {
            search->setText(QStringLiteral("sheet"));
            enabled_result = list->currentRow() > 0 && list->currentItem() &&
                (list->currentItem()->flags() & Qt::ItemIsEnabled);
        }
        dialog->reject();
    });
    bare.showCommandPalette();
    require(enabled_result, "search initially selects an available result after disabled sheet actions");
    const auto revision = bare.document().revision();
    require(!bare.createDrawingSheet(QStringLiteral("A1"), QStringLiteral("215.9"),
                                    QStringLiteral("279.4"), QStringLiteral("Sketch")).isEmpty(),
            "a project without sheets can create its first sheet: " + bare.lastError().toStdString());
    require(bare.document().revision() == revision + 1, "first sheet creates one atomic edit");
    QMetaObject::invokeMethod(bare_menu, "aboutToShow");
    require(bare_sheet->isEnabled() && bare_set->isEnabled(), "first sheet enables PDF output");
    require(bare.undoCommand(), "undo first sheet");
    QMetaObject::invokeMethod(bare_menu, "aboutToShow");
    require(!bare_sheet->isEnabled(), "one Undo removes the first sheet");
    require(bare.redoCommand(), "redo first sheet");
    QMetaObject::invokeMethod(bare_menu, "aboutToShow");
    require(bare_sheet->isEnabled(), "Redo restores sheet output");
    require(bare.exportDraftPdf(directory.filePath(QStringLiteral("first-sheet.pdf"))),
            "the first sheet produces a usable PDF: " + bare.lastError().toStdString());
    require(bare.saveProjectAs(directory.filePath(QStringLiteral("first-sheet.bldproj"))), "save first sheet");
    desktop::MainWindow reopened;
    require(reopened.openProject(directory.filePath(QStringLiteral("first-sheet.bldproj"))), "reopen first sheet");
    auto* reopened_menu = reopened.findChild<QMenu*>(QStringLiteral("exportMenu"));
    QMetaObject::invokeMethod(reopened_menu, "aboutToShow");
    require(reopened.findChild<QAction*>(QStringLiteral("exportSheetPdf"))->isEnabled(), "first sheet survives editable save/Open");

    // Follow the advertised menu path on another project with no output metadata.
    auto fresh_document = std::make_shared<Document>(Document::create({
        {"p", "property", {{"name", "No sheets"}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}, {"name", "Plan"}}, false}}));
    desktop::MainWindow fresh(fresh_document);
    auto* output = fresh.findChild<QMenu*>(QStringLiteral("outputMenu"));
    require(output && !output->actions().isEmpty() && output->actions().front()->text().contains(QStringLiteral("Drawing sheets")),
            "Output names the same Drawing sheets destination as the recovery hint");
    bool opened = false;
    bool created = false;
    const auto fresh_revision = fresh.document().revision();
    QTimer::singleShot(0, &fresh, [&] {
        auto* dialog = fresh.findChild<QDialog*>(QStringLiteral("sheetSettingsDialog"));
        if (!dialog) return;
        opened = true;
        auto* add = dialog->findChild<QPushButton*>(QStringLiteral("addSheet"));
        if (add) {
            QTimer::singleShot(0, dialog, [&] {
                auto* form = fresh.findChild<QDialog*>(QStringLiteral("addDrawingSheetForm"));
                if (!form) return;
                if (auto* number = form->findChild<QLineEdit*>(QStringLiteral("newSheetNumber")))
                    number->setText(QStringLiteral("A1"));
                form->accept();
            });
            add->click();
            auto* selector = dialog->findChild<QComboBox*>(QStringLiteral("sheetSelector"));
            auto* project = dialog->findChild<QLineEdit*>(QStringLiteral("sheetProject"));
            created = selector && selector->count() == 1 && fresh.document().revision() == fresh_revision + 1 &&
                project && project->text() == QStringLiteral("No sheets");
        }
        dialog->reject();
    });
    output->actions().front()->trigger();
    require(opened && created, "Drawing sheets > Add creates the first sheet in one edit");
    require(fresh.undoCommand() && fresh.document().snapshot().entities().size() == 4,
            "one Undo restores the original project without hidden output entities");
}
}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir settings_directory;
    QTemporaryDir files;
    QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QCoreApplication::setApplicationName(QStringLiteral("EditMenuDesktop"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory.path());
    try {
        require(settings_directory.isValid() && files.isValid(), "temporary directories");
        const auto font = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font >= 0, "bundled Inter font required");
        app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
        capture_navigation();
        test_menu_entries();
        test_command_search_execution();
        test_focused_text_commands();
        test_navigation_access();
        test_command_search_discovery();
        test_export_menu();
        test_recent_projects(files);
        test_reopen_last_project(files);
        test_select_all_and_duplicate();
        std::cout << "Edit menu desktop tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edit_menu_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
