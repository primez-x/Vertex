// File > Open recent, Edit > Select all and Edit > Duplicate.
#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/preferences_dialog.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeySequence>
#include <QMenu>
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
        test_menu_entries();
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
