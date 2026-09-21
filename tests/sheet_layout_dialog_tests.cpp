#include "sketch/desktop/sheet_layout_dialog.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
sketch::SheetViewModel fixture() {
    sketch::DrawingSheet a;
    a.id = "sheet-a"; a.number = "A101";
    a.viewports = {{"viewport-a", "plan", {10, 10, 100, 100}, 100},
                   {"viewport-b", "plan", {120, 10, 100, 100}, 100}};
    a.schedules = {{"schedule-a", "rooms", {10, 120, 100, 50}},
                   {"schedule-b", "rooms", {120, 120, 100, 50}}};
    auto b = a; b.id = "sheet-b"; b.number = "A102";
    return sketch::SheetViewModel::create({{"plan", "Floor plan"}, {"section", "Section"}},
                                          {a, b}, {"rooms", "doors"});
}
void field(sketch::desktop::SheetLayoutDialog& dialog, const char* name, const char* text) {
    auto* edit = dialog.findChild<QLineEdit*>(name);
    require(edit, "missing layout field"); edit->setText(text);
}

void click(sketch::desktop::SheetLayoutDialog& dialog, const char* name) {
    auto* button = dialog.findChild<QPushButton*>(name);
    require(button && button->isEnabled(), "placement lifecycle control missing or disabled");
    button->click();
}

void testPlacementLifecycle() {
    auto source = fixture();
    auto page = source.sheets()[1];
    page.viewports.clear(); page.schedules.clear(); page.width_mm = 12; page.height_mm = 8;
    source = source.with_sheet(page);
    const auto saved = source.to_json();
    sketch::desktop::SheetLayoutDialog dialog(source, "sheet-b");
    auto* choices = dialog.findChild<QComboBox*>("sheetLayoutNewView");
    require(choices, "view source chooser missing");
    choices->setCurrentIndex(choices->findData("section"));
    click(dialog, "sheetLayoutAddViewport");
    const auto first = dialog.workingModel().sheets()[1].viewports.front();
    require(first.view_id == "section" && first.bounds.width_mm <= 12 && first.bounds.height_mm <= 8,
            "new viewport did not use selected shared view or fit page");
    field(dialog, "sheetLayoutWidth", "999");
    const auto before_invalid = dialog.workingModel().to_json();
    click(dialog, "sheetLayoutAddSchedule");
    require(dialog.workingModel().to_json() == before_invalid, "add discarded invalid pending edit");
    field(dialog, "sheetLayoutWidth", "10");
    click(dialog, "sheetLayoutAddViewport");
    const auto viewports = dialog.workingModel().sheets()[1].viewports;
    require(viewports.size() == 2 && viewports[0].id != viewports[1].id, "viewport identities collide");
    auto* schedules = dialog.findChild<QComboBox*>("sheetLayoutNewSchedule");
    require(schedules && schedules->findData("rooms") >= 0 && schedules->findData("doors") >= 0,
            "schedule registry chooser missing unplaced schedule");
    schedules->setCurrentIndex(schedules->findData("doors"));
    click(dialog, "sheetLayoutAddSchedule");
    const auto schedule = dialog.workingModel().sheets()[1].schedules.front();
    require(schedule.schedule_id == "doors" && schedule.bounds.width_mm <= 12 && schedule.bounds.height_mm <= 8,
            "schedule defaults do not fit page");
    click(dialog, "sheetLayoutAddSchedule");
    const auto repeated_schedules = dialog.workingModel().sheets()[1].schedules;
    require(repeated_schedules.size() == 2 && repeated_schedules[0].id != repeated_schedules[1].id,
            "schedule identities collide");
    click(dialog, "sheetLayoutRemoveSelected");
    require(dialog.selectSheet("sheet-a") && dialog.selectSheet("sheet-b"), "target switching failed");
    require(dialog.selectSchedulePlacement(QString::fromStdString(schedule.id)), "new schedule lost stable identity");
    click(dialog, "sheetLayoutRemoveSelected");
    for (const auto& viewport : viewports) {
        require(dialog.selectViewport(QString::fromStdString(viewport.id)), "new viewport lost stable identity");
        click(dialog, "sheetLayoutRemoveSelected");
    }
    require(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled(),
            "empty layout cannot be saved after deletion");
    dialog.accept();
    require(dialog.acceptedModel() && dialog.acceptedModel()->sheets()[1].viewports.empty() &&
            dialog.acceptedModel()->sheets()[1].schedules.empty(), "last removal not accepted");
    sketch::desktop::SheetLayoutDialog cancelled(source, "sheet-a");
    require(cancelled.selectViewport("viewport-a"), "cancel removal selection failed");
    click(cancelled, "sheetLayoutRemoveSelected");
    click(cancelled, "sheetLayoutAddSchedule");
    cancelled.reject();
    require(!cancelled.acceptedModel() && source.to_json() == saved, "cancel leaked add/remove edits");
    auto linked = source.sheets()[1];
    linked.callouts = {{"callout", "See plan", "sheet-a", "viewport-a", 0, 0}};
    sketch::desktop::SheetLayoutDialog protected_dialog(source.with_sheet(linked), "sheet-a");
    require(protected_dialog.selectViewport("viewport-a"), "protected viewport selection failed");
    const auto protected_before = protected_dialog.workingModel().to_json();
    click(protected_dialog, "sheetLayoutRemoveSelected");
    require(protected_dialog.workingModel().to_json() == protected_before &&
            !protected_dialog.findChild<QLabel*>("sheetLayoutError")->text().isEmpty(), "callout protection failed");
}

sketch::SheetViewModel windowSheetModel(const sketch::desktop::MainWindow& window) {
    std::string types;
    const auto snapshot = window.document().snapshot();
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type == sketch::kSheetViewEntityType) {
            return sketch::decode_sheet_view_entity(entity);
        }
        if (!types.empty()) types += ", ";
        types += entity.type;
    }
    throw std::runtime_error("window sheet model is missing; entities: " + types);
}

void testMainWindowCommitsSelectedSheetPlacement() {
    sketch::desktop::MainWindow window;
    const auto first_sheet = windowSheetModel(window).sheets().front();
    const auto second_id = window.createDrawingSheet(
        QStringLiteral("A-102"), QStringLiteral("420"), QStringLiteral("297"),
        QStringLiteral("Second sheet"));
    require(!second_id.isEmpty() && window.selectOutputSheet(second_id),
            "second output sheet could not be selected");
    const auto before = windowSheetModel(window);
    const auto second = std::find_if(before.sheets().begin(), before.sheets().end(),
        [&](const auto& sheet) { return sheet.id == second_id.toStdString(); });
    require(second != before.sheets().end() && second->viewports.size() >= 2,
            "second sheet must contain multiple editable viewports");
    const auto viewport_id = second->viewports[1].id;
    const auto original_x = second->viewports[1].bounds.x_mm;
    const auto revision = window.document().revision();
    auto* action = window.findChild<QAction*>(QStringLiteral("sheetLayoutManager"));
    require(action, "sheet layout action is not exposed by MainWindow");
    QString callback_error;
    QTimer::singleShot(0, [&] {
        try {
            auto* modal = dynamic_cast<sketch::desktop::SheetLayoutDialog*>(
                QApplication::activeModalWidget());
            require(modal && modal->selectedSheetId() == second_id,
                    "layout manager did not open on selected output sheet");
            require(modal->selectViewport(QString::fromStdString(viewport_id)),
                    "layout manager could not select second viewport");
            field(*modal, "sheetLayoutX", QString::number(original_x + 1.0, 'g', 17).toUtf8().constData());
            modal->accept();
        } catch (const std::exception& error) {
            callback_error = QString::fromUtf8(error.what());
            if (auto* modal = QApplication::activeModalWidget()) modal->close();
        }
    });
    action->trigger();
    require(callback_error.isEmpty(), callback_error.toUtf8().constData());
    require(window.document().revision() == revision + 1,
            "layout manager must commit all staged edits as one document command");
    const auto after = windowSheetModel(window);
    // Sheets are sorted by ID, so the new sheet's random ID can sort first.
    const auto unchanged_sheet = std::find_if(after.sheets().begin(), after.sheets().end(),
        [&](const auto& sheet) { return sheet.id == first_sheet.id; });
    require(unchanged_sheet != after.sheets().end() && *unchanged_sheet == first_sheet,
            "editing selected output sheet changed the first sheet");
    const auto edited_sheet = std::find_if(after.sheets().begin(), after.sheets().end(),
        [&](const auto& sheet) { return sheet.id == second_id.toStdString(); });
    require(edited_sheet != after.sheets().end(), "edited output sheet is missing");
    const auto edited_viewport = std::find_if(
        edited_sheet->viewports.begin(), edited_sheet->viewports.end(),
        [&](const auto& viewport) { return viewport.id == viewport_id; });
    require(edited_viewport != edited_sheet->viewports.end() &&
                edited_viewport->bounds.x_mm == original_x + 1.0,
            "layout manager edited the wrong sheet or viewport");
    require(window.undoCommand() && windowSheetModel(window).to_json() == before.to_json(),
            "layout manager command must undo atomically");
}

void testMainWindowCommitsPlacementLifecycle() {
    sketch::desktop::MainWindow window;
    const auto before = windowSheetModel(window);
    const auto sheet_id = before.sheets().front().id;
    require(window.selectOutputSheet(QString::fromStdString(sheet_id)), "integration sheet selection failed");
    const auto revision = window.document().revision();
    auto* action = window.findChild<QAction*>(QStringLiteral("sheetLayoutManager"));
    require(action, "layout manager action missing");
    QString callback_error;
    std::optional<sketch::SheetViewModel> expected;
    QTimer::singleShot(0, [&] {
        try {
            auto* modal = dynamic_cast<sketch::desktop::SheetLayoutDialog*>(QApplication::activeModalWidget());
            require(modal, "lifecycle dialog did not open");
            click(*modal, "sheetLayoutAddViewport");
            click(*modal, "sheetLayoutRemoveSelected");
            click(*modal, "sheetLayoutAddViewport");
            click(*modal, "sheetLayoutAddSchedule");
            modal->accept();
            expected = modal->acceptedModel();
        } catch (const std::exception& error) {
            callback_error = QString::fromUtf8(error.what());
            if (auto* modal = QApplication::activeModalWidget()) modal->close();
        }
    });
    action->trigger();
    require(callback_error.isEmpty(), callback_error.toUtf8().constData());
    require(expected && window.document().revision() == revision + 1 &&
            windowSheetModel(window).to_json() == expected->to_json(), "lifecycle did not commit in one command");
    require(window.undoCommand() && windowSheetModel(window).to_json() == before.to_json(),
            "lifecycle undo did not restore full snapshot");
    require(window.redoCommand() && windowSheetModel(window).to_json() == expected->to_json(),
            "lifecycle redo did not restore the complete placement set");
    QTemporaryDir directory;
    require(directory.isValid(), "lifecycle save fixture directory failed");
    const auto project = directory.filePath(QStringLiteral("sheet-placement-lifecycle.bldproj"));
    require(window.saveProjectAs(project), "lifecycle project did not save");
    sketch::desktop::MainWindow reopened;
    require(reopened.openProject(project) &&
                windowSheetModel(reopened).to_json() == expected->to_json(),
            "lifecycle placement set did not survive save and reopen");
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        const int font_id = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "test font failed to load");
        app.setFont(QFont(families.front(), 10));
        const auto original = fixture();
        const auto original_json = original.to_json();
        sketch::desktop::SheetLayoutDialog dialog(original, "sheet-b");
        require(dialog.selectedSheetId() == "sheet-b", "selected output sheet was ignored");
        require(dialog.selectViewport("viewport-b"), "cannot select second viewport");
        field(dialog, "sheetLayoutX", "140");
        field(dialog, "sheetLayoutScale", "50");
        auto* view = dialog.findChild<QComboBox*>("sheetLayoutView");
        require(view, "missing view assignment");
        view->setCurrentIndex(view->findData("section"));
        require(dialog.applyCurrentEdit(), "valid edit rejected");
        require(dialog.workingModel().sheets()[0] == original.sheets()[0], "first sheet changed");
        require(dialog.workingModel().sheets()[1].viewports[0] == original.sheets()[1].viewports[0],
                "first viewport changed");
        const auto edited = dialog.workingModel().sheets()[1].viewports[1];
        require(edited.bounds.x_mm == 140 && edited.scale_denominator == 50 && edited.view_id == "section",
                "second viewport edit missing");
        field(dialog, "sheetLayoutWidth", "9999");
        require(!dialog.applyCurrentEdit(), "out of page bounds accepted");
        require(!dialog.selectSheet("sheet-a"), "invalid edit silently discarded by sheet switch");
        require(dialog.selectedSheetId() == "sheet-b", "invalid switch changed target");
        require(dialog.workingModel().sheets()[1].viewports[1] == edited, "invalid edit altered draft");
        auto* sheet_choice = dialog.findChild<QComboBox*>("sheetLayoutSheet");
        auto* placement_choice = dialog.findChild<QComboBox*>("sheetLayoutPlacement");
        require(sheet_choice && placement_choice, "missing target selectors");
        sheet_choice->setCurrentIndex(sheet_choice->findData("sheet-a"));
        require(sheet_choice->currentData().toString() == "sheet-b", "UI switch did not restore invalid sheet selection");
        placement_choice->setCurrentIndex(0);
        require(placement_choice->currentData().toString() == "viewport-b", "UI switch discarded invalid placement edit");
        require(!dialog.findChild<QLabel*>("sheetLayoutError")->text().isEmpty(), "validation error not displayed");
        field(dialog, "sheetLayoutWidth", "100");
        field(dialog, "sheetLayoutScale", "0");
        require(!dialog.applyCurrentEdit(), "zero scale accepted");
        field(dialog, "sheetLayoutScale", "nan");
        require(!dialog.applyCurrentEdit(), "nonfinite scale accepted");
        field(dialog, "sheetLayoutScale", "50");
        const auto capture_dir = qEnvironmentVariable("SKETCH_SHEET_LAYOUT_CAPTURE_DIR");
        if (!capture_dir.isEmpty()) {
            require(QDir().mkpath(capture_dir), "capture directory failed");
            dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
            dialog.show(); QApplication::processEvents();
            require(dialog.grab().save(QDir(capture_dir).filePath("sheet-layout.png")), "layout capture failed");
            dialog.hide();
        }
        require(dialog.selectSchedulePlacement("schedule-b"), "cannot select second schedule");
        field(dialog, "sheetLayoutX", "155");
        require(dialog.applyCurrentEdit(), "schedule edit rejected");
        require(dialog.workingModel().sheets()[1].schedules[1].bounds.x_mm == 155, "wrong schedule edited");
        require(dialog.workingModel().sheets()[1].schedules[0] == original.sheets()[1].schedules[0],
                "first schedule changed");
        require(!dialog.selectViewport("missing"), "unknown placement accepted");
        require(!dialog.selectSheet("missing"), "unknown sheet accepted");
        dialog.accept();
        require(dialog.acceptedModel().has_value(), "accepted snapshot missing");
        require(original.to_json() == original_json, "source mutated");
        sketch::desktop::SheetLayoutDialog cancelled(original, "sheet-b");
        require(cancelled.selectViewport("viewport-b"), "cancel fixture selection failed");
        field(cancelled, "sheetLayoutScale", "25");
        require(cancelled.applyCurrentEdit(), "cancel fixture edit failed");
        cancelled.reject();
        require(!cancelled.acceptedModel(), "cancel returned edits");
        sketch::desktop::SheetLayoutDialog unknown(original, "missing");
        require(unknown.selectedSheetId().isEmpty(), "unknown output sheet fell back to first");
        require(!unknown.applyCurrentEdit(), "missing target accepted");
        sketch::DrawingSheet empty; empty.id = "empty"; empty.number = "E101";
        sketch::desktop::SheetLayoutDialog empty_dialog(sketch::SheetViewModel::create({}, {empty}), "empty");
        require(!empty_dialog.selectViewport("viewport-a") && !empty_dialog.applyCurrentEdit(),
                "empty sheet supplied a synthetic placement");
        require(!empty_dialog.findChild<QLineEdit*>("sheetLayoutX")->isEnabled(), "empty sheet fields enabled");
        require(!empty_dialog.findChild<QPushButton*>("sheetLayoutAddViewport")->isEnabled() &&
                !empty_dialog.findChild<QPushButton*>("sheetLayoutAddSchedule")->isEnabled(),
                "add controls enabled without source registry entries");
        testMainWindowCommitsSelectedSheetPlacement();
        testPlacementLifecycle();
        testMainWindowCommitsPlacementLifecycle();
        std::cout << "sheet layout dialog checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
