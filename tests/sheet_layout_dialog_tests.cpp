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
#include <QPdfDocument>
#include <QSize>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
sketch::Boundary square(double x, double y, double side) {
    return {{{x,y},{x+side,y},0},{{x+side,y},{x+side,y+side},0},
            {{x+side,y+side},{x,y+side},0},{{x,y+side},{x,y},0}};
}
sketch::SheetViewModel fixture() {
    sketch::DrawingSheet a;
    a.id = "sheet-a"; a.number = "A101";
    a.viewports = {{"viewport-a", "plan", {10, 10, 100, 100}, 100},
                   {"viewport-b", "plan", {120, 10, 100, 100}, 100}};
    a.schedules = {{"schedule-a", "rooms", {10, 120, 100, 50}},
                   {"schedule-b", "rooms", {120, 120, 100, 50}}};
    auto b = a; b.id = "sheet-b"; b.number = "A102";
    sketch::CoordinatedView plan;
    plan.id = "plan"; plan.name = "Floor plan";
    sketch::CoordinatedView section;
    section.id = "section"; section.name = "Section";
    section.kind = sketch::CoordinatedViewKind::section;
    return sketch::SheetViewModel::create({plan, section},
                                          {a, b}, {"rooms", "doors", "appraisal-areas"});
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
    const auto appraisal_index = schedules ? schedules->findData("appraisal-areas") : -1;
    require(schedules && schedules->findData("rooms") >= 0 && schedules->findData("doors") >= 0 &&
                appraisal_index >= 0 &&
                schedules->itemText(appraisal_index) == QStringLiteral("Appraisal area summary"),
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

void testAppraisalSheetPreset() {
    auto source = fixture();
    auto occupied_number = source.sheets().front();
    occupied_number.number = "AP-101";
    source = source.with_sheet(occupied_number);
    sketch::CoordinatedView tilted;
    tilted.id = "tilted-plan"; tilted.name = "Tilted plan";
    tilted.kind = sketch::CoordinatedViewKind::plan;
    tilted.direction = {1, 0, 0}; tilted.up = {0, 0, 1};
    auto views = source.views();views.push_back(tilted);
    source = sketch::SheetViewModel::create(std::move(views),source.sheets(),source.schedule_ids(),source.sheet_order());
    const auto source_json = source.to_json();

    sketch::desktop::SheetLayoutDialog dialog(source, "sheet-a");
    auto* chooser = dialog.findChild<QComboBox*>("appraisalSheetPlanView");
    auto* button = dialog.findChild<QPushButton*>("addAppraisalSheet");
    require(chooser && button && chooser->count() == 1 &&
            chooser->currentData().toString() == "plan" && button->isEnabled(),
            "appraisal preset must offer only horizontal plan views");
    button->click();
    auto find_sheet = [&](const std::string& number) {
        return std::find_if(dialog.workingModel().sheets().begin(), dialog.workingModel().sheets().end(),
            [&](const auto& sheet) { return sheet.number == number; });
    };
    auto first = find_sheet("AP-102");
    require(first != dialog.workingModel().sheets().end(), "preset did not skip the occupied AP-101 number");
    require(first->width_mm == 420 && first->height_mm == 297 &&
            first->title_block.title == "Appraisal plan and area summary" &&
            first->viewports.size() == 1 && first->schedules.size() == 1,
            "preset page metadata or placement count is wrong");
    require(first->viewports[0].view_id == "plan" && first->viewports[0].bounds == sketch::SheetRect{10,10,250,250} &&
            first->viewports[0].scale_denominator == 100 &&
            first->schedules[0].schedule_id == "appraisal-areas" &&
            first->schedules[0].bounds == sketch::SheetRect{270,10,140,250},
            "preset plan or appraisal placement geometry is wrong");
    require(first->viewports[0].bounds.x_mm + first->viewports[0].bounds.width_mm <
                first->schedules[0].bounds.x_mm &&
            first->schedules[0].bounds.x_mm + first->schedules[0].bounds.width_mm <= first->width_mm &&
            first->viewports[0].bounds.y_mm + first->viewports[0].bounds.height_mm <= first->height_mm &&
            first->schedules[0].bounds.y_mm + first->schedules[0].bounds.height_mm <= first->height_mm,
            "preset placements overlap or leave the page");
    require(dialog.workingModel().schedule_ids().end() !=
                std::find(dialog.workingModel().schedule_ids().begin(), dialog.workingModel().schedule_ids().end(),
                          "appraisal-areas"),
            "preset failed to register the appraisal schedule");
    for (const auto& original : source.sheets()) {
        const auto kept = std::find_if(dialog.workingModel().sheets().begin(), dialog.workingModel().sheets().end(),
            [&](const auto& sheet) { return sheet.id == original.id; });
        require(kept != dialog.workingModel().sheets().end() && *kept == original,
                "preset changed an existing sheet");
    }
    require(dialog.workingModel().views() == source.views(), "preset changed shared views");
    auto expected_schedule_ids = source.schedule_ids();
    require(dialog.workingModel().schedule_ids() == expected_schedule_ids,
            "preset changed a schedule registry that already contained appraisal");
    const auto first_id = first->id;
    const auto first_viewport_id = first->viewports[0].id;
    const auto first_schedule_id = first->schedules[0].id;
    button->click();
    require(find_sheet("AP-103") != dialog.workingModel().sheets().end(),
            "repeated preset did not allocate the next available number");
    const auto second = find_sheet("AP-103");
    require(second->id != first_id && second->viewports[0].id != first_viewport_id &&
            second->schedules[0].id != first_schedule_id,
            "repeated preset reused a sheet or placement identity");
    dialog.reject();
    require(!dialog.acceptedModel() && source.to_json() == source_json,
            "cancel published the staged appraisal sheets or changed source");

    const auto missing_schedule = sketch::SheetViewModel::create(
        source.views(), source.sheets(), {"rooms", "doors"}, source.sheet_order());
    sketch::desktop::SheetLayoutDialog accepted(missing_schedule, "sheet-a");
    require(accepted.addAppraisalSheet("plan"), "preset could not add its missing appraisal schedule");
    accepted.accept();
    require(accepted.acceptedModel() &&
            std::find(accepted.acceptedModel()->schedule_ids().begin(),
                      accepted.acceptedModel()->schedule_ids().end(), "appraisal-areas") !=
                accepted.acceptedModel()->schedule_ids().end() &&
            sketch::SheetViewModel::from_json(accepted.acceptedModel()->to_json()).to_json() ==
                accepted.acceptedModel()->to_json(),
            "accepted appraisal preset did not register and round-trip its model");

    sketch::CoordinatedView elevation;
    elevation.id = "elevation"; elevation.name = "Elevation";
    elevation.kind = sketch::CoordinatedViewKind::elevation;
    auto empty_page = source.sheets().front();empty_page.viewports.clear();empty_page.schedules.clear();empty_page.callouts.clear();
    const auto no_plan = sketch::SheetViewModel::create({elevation, tilted}, {empty_page}, {"rooms"});
    sketch::desktop::SheetLayoutDialog unavailable(no_plan, "sheet-a");
    auto* unavailable_button = unavailable.findChild<QPushButton*>("addAppraisalSheet");
    require(unavailable_button && !unavailable_button->isEnabled() &&
            !unavailable.addAppraisalSheet("tilted-plan") &&
            unavailable.workingModel().to_json() == no_plan.to_json(),
            "preset accepted an elevation/tilted plan or changed a model without a horizontal plan");
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
    const auto initial_model = windowSheetModel(window);
    require(std::find(initial_model.schedule_ids().begin(), initial_model.schedule_ids().end(),
                      "appraisal-areas") != initial_model.schedule_ids().end(),
            "new projects must register the appraisal area summary for sheet placement");
    const auto first_sheet = initial_model.sheets().front();
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

void testMainWindowAppraisalSheetPresetLifecycleAndPdf() {
    QTemporaryDir directory;
    require(directory.isValid(), "appraisal sheet fixture needs an isolated directory");
    sketch::desktop::MainWindow window;
    const auto area_id = window.createBoundary(square(0, 0, 3.048));
    require(!area_id.isEmpty(), "appraisal sheet fixture needs an actual plan boundary");
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow && workflow->findData(QStringLiteral("appraisal")) >= 0,
            "appraisal sheet fixture needs the appraisal workflow control");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(QStringLiteral(
        R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})")),
        "appraisal sheet fixture must use a declared qualified area");

    const auto before_model = windowSheetModel(window);
    const auto source_sheet = before_model.sheets().front();
    const auto revision = window.document().revision();
    auto* action = window.findChild<QAction*>(QStringLiteral("sheetLayoutManager"));
    require(action, "appraisal sheet preset must be reachable through the real layout command");
    QString callback_error;
    bool accepted = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = dynamic_cast<sketch::desktop::SheetLayoutDialog*>(QApplication::activeModalWidget());
        try {
            require(dialog && dialog->selectedSheetId() == QString::fromStdString(source_sheet.id),
                    "layout command must open the actual dialog on the selected source sheet");
            auto* button = dialog->findChild<QPushButton*>(QStringLiteral("addAppraisalSheet"));
            require(button && button->isEnabled(), "actual dialog must expose its enabled appraisal preset");
            button->click();
            require(dialog->selectedSheetId() != QString::fromStdString(source_sheet.id),
                    "preset must stage and select a new sheet in the dialog");
            dialog->accept();
            accepted = dialog->acceptedModel().has_value();
        } catch (const std::exception& error) {
            callback_error = QString::fromUtf8(error.what());
            if (dialog && dialog->isVisible()) dialog->reject();
        }
    });
    action->trigger();
    require(callback_error.isEmpty(), callback_error.toUtf8().constData());
    require(accepted && window.document().revision() == revision + 1,
            "accepted preset must commit one normal sheet-model command");
    const auto after_model = windowSheetModel(window);
    const auto created = std::find_if(after_model.sheets().begin(), after_model.sheets().end(),
        [](const auto& sheet) { return sheet.number == "AP-101"; });
    require(created != after_model.sheets().end() && created->viewports.size() == 1 &&
            created->viewports[0].view_id == "view-plan" && created->schedules.size() == 1 &&
            created->schedules[0].schedule_id == "appraisal-areas",
            "MainWindow did not commit the actual plan and appraisal summary placements");
    const auto appraisal_sheet_id = QString::fromStdString(created->id);
    require(window.undoCommand() && windowSheetModel(window).to_json() == before_model.to_json(),
            "preset must undo as one command without changing the original sheet graph");
    require(window.redoCommand() && windowSheetModel(window).to_json() == after_model.to_json(),
            "preset must redo the same complete sheet graph");

    const auto project = directory.filePath(QStringLiteral("appraisal-sheet-preset.bldproj"));
    require(window.saveProjectAs(project), "preset project must save");
    sketch::desktop::MainWindow reopened;
    require(reopened.openProject(project), "preset project must reopen in a new MainWindow");
    const auto reopened_model = windowSheetModel(reopened);
    require(reopened_model.to_json() == after_model.to_json(),
            "save/reopen must retain the preset view and appraisal schedule graph");
    require(reopened.selectOutputSheet(appraisal_sheet_id), "appraisal output sheet must remain selectable after reopen");
    const auto pdf_path = directory.filePath(QStringLiteral("appraisal-sheet-preset.pdf"));
    require(reopened.exportDrawingSetPdf(pdf_path), "preset drawing set must export as an actual PDF");
    QPdfDocument pdf;
    require(pdf.load(pdf_path) == QPdfDocument::Error::None && pdf.pageCount() == 2,
            "drawing-set PDF must include the original and appraisal sheets");
    const auto page_text = pdf.getAllText(1).text();
    require(page_text.contains(QStringLiteral("APPRAISAL AREA SUMMARY")) &&
            page_text.contains(QStringLiteral("Appraisal plan and area summary")) &&
            !pdf.render(1, QSize(1000, 707)).isNull(),
            "appraisal sheet PDF must expose its real schedule, title, and renderable plan page");
    const auto capture_dir = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture_dir.isEmpty()) {
        require(QDir().mkpath(capture_dir) &&
                pdf.render(1, QSize(1200, 848)).save(QDir(capture_dir).filePath(
                    QStringLiteral("appraisal-sheet-preset.png"))),
                "optional appraisal sheet PDF page capture must save");
    }
    require(window.document().revision() >= revision + 1 && window.document().snapshot().entities().contains(area_id.toStdString()),
            "sheet preset output must retain its source project geometry");
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
        testAppraisalSheetPreset();
        testPlacementLifecycle();
        testMainWindowCommitsPlacementLifecycle();
        testMainWindowAppraisalSheetPresetLifecycleAndPdf();
        std::cout << "sheet layout dialog checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
