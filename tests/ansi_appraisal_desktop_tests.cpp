#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPixmap>
#include <QPdfDocument>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QUuid>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <vector>
#include <tuple>

namespace {
using sketch::desktop::MainWindow;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T& child(QWidget& parent, const char* name) {
    auto* value = dynamic_cast<T*>(parent.findChild<QWidget*>(QString::fromLatin1(name)));
    require(value, "expected native ANSI control"); return *value;
}
void choose(QWidget& parent, const char* name, const char* value) {
    auto& box = child<QComboBox>(parent, name); const auto index = box.findData(QString::fromLatin1(value));
    require(index >= 0, "expected ANSI option"); box.setCurrentIndex(index);
}
void dialog(MainWindow& window, const char* name, const std::function<void()>& open,
            const std::function<void(QDialog&)>& inspect) {
    std::exception_ptr failure; bool found = false;
    QTimer::singleShot(0, &window, [&] {
        auto* value = window.findChild<QDialog*>(QString::fromLatin1(name));
        try { require(value, "expected native dialog"); found = true; inspect(*value); }
        catch (...) { failure = std::current_exception(); }
        if (value && value->isVisible()) value->reject();
    });
    open(); if (failure) std::rethrow_exception(failure); require(found, "dialog opened");
}
void boundary_review_precision(bool metric, bool ansi) {
    QTemporaryDir fixture; require(fixture.isValid(), "isolated boundary review fixture");
    MainWindow window({}, nullptr, fixture.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(metric); window.resize(1280, 900); window.show();
    QApplication::processEvents();
    if (ansi) {
        child<QTabWidget>(window, "sidebarTabs").setCurrentIndex(2);
        dialog(window, "appraisalSetupDialog",
            [&] { child<QPushButton>(window, "appraisalDetailsSetup").click(); },
            [&](QDialog& value) {
                value.setAttribute(Qt::WA_DontShowOnScreen);
                choose(value, "appraisalSetupPolicy", "ansi_z765_2021");
                choose(value, "appraisalSetupPropertyKind", "detached_single_family");
                choose(value, "appraisalSetupMeasurementBasis", "exterior");
                choose(value, "ansiInteriorInspected", "yes");
                choose(value, "ansiDirectMeasurement", "yes");
                choose(value, "ansiAcquisitionIncrement", "tenth_foot");
                child<QDialogButtonBox>(value, "appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
                require(value.result() == QDialog::Accepted, "boundary review ANSI setup saves");
            });
    }
    const sketch::Boundary square{{{0,0},{3.048,0},0},{{3.048,0},{3.048,3.048},0},
        {{3.048,3.048},{0,3.048},0},{{0,3.048},{0,0},0}};
    const auto area = window.createBoundary(square);
    require(!area.isEmpty(), "review uses an actual authored boundary");
    const auto boundary = sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(area.toStdString()));
    const auto length = window.createLengthDimension(area,
        QString::fromStdString(boundary.segments.front().segment_id), {1.524,-1});
    const auto area_dimension = window.createAreaDimension(area, {1.524,1.524});
    require(!length.isEmpty() && !area_dimension.isEmpty(), "review uses authored dependent dimensions");
    require(window.selectEntity(area), "select measured boundary for the actual geometry editor");
    const auto before = window.document().snapshot();
    const auto before_length = ansi ? (metric ? QStringLiteral("10.0 ft (3.048 m)") : QStringLiteral("10.0 ft"))
                                   : QStringLiteral("3.048 m");
    const auto after_length = ansi ? (metric ? QStringLiteral("11.0 ft (3.353 m)") : QStringLiteral("11.0 ft"))
                                  : QStringLiteral("3.353 m");
    const auto before_area = ansi ? (metric ? QStringLiteral("100 sq ft (9.29 m²)") : QStringLiteral("100 sq ft"))
                                 : QStringLiteral("9.29 m²");
    const auto after_area = ansi ? (metric ? QStringLiteral("105 sq ft (9.75 m²)") : QStringLiteral("105 sq ft"))
                                : QStringLiteral("9.75 m²");
    dialog(window, "boundaryGeometryDialog",
        [&] { child<QPushButton>(window, "editBoundaryGeometry").click(); }, [&](QDialog& value) {
            value.setAttribute(Qt::WA_DontShowOnScreen);
            require(child<QComboBox>(value, "boundaryEditOperation").currentData().toString() == "length",
                "boundary review opens the real default edge-length operation");
            auto& edge = child<QComboBox>(value, "boundaryEdge");
            const auto index = edge.findData(QString::fromStdString(boundary.segments.front().segment_id));
            require(index >= 0, "review retains the authored edge identity"); edge.setCurrentIndex(index);
            child<QLineEdit>(value, "boundaryEdgeLength").setText(QStringLiteral("11 ft"));
            QApplication::processEvents();
            auto& buttons = child<QDialogButtonBox>(value, "boundaryGeometryButtons");
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(), "exact edge input produces a valid review");
            const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
            if (!capture.isEmpty()) {
                require(QDir().mkpath(capture) && value.grab().save(QDir(capture).filePath(
                    QStringLiteral("boundary-review-%1-%2.png").arg(ansi ? "ansi" : "generic", metric ? "metric" : "imperial"))),
                    "capture the actual boundary review dialog");
            }
            auto& changes = child<QTableWidget>(value, "boundaryGeometryChanges");
            const auto check_row = [&](const QString& name, const QString& original, const QString& proposed) {
                for (int row = 0; row < changes.rowCount(); ++row) {
                    if (changes.item(row, 0)->text() != name) continue;
                    const auto actual_before = changes.item(row, 1)->text();
                    const auto actual_after = changes.item(row, 2)->text();
                    if (!actual_before.startsWith(original + QStringLiteral("  @ ")) ||
                        !actual_after.startsWith(proposed + QStringLiteral("  @ ")))
                        throw std::runtime_error(QStringLiteral("%1 %2 review %3 must use canonical dimension text; before='%4', after='%5'")
                            .arg(ansi ? "ANSI" : "generic", metric ? "Metric" : "Imperial", name, actual_before, actual_after).toStdString());
                    return;
                }
                throw std::runtime_error("authored dimension is missing from the real review table");
            };
            check_row(QStringLiteral("Edge 1 measurement"), before_length, after_length);
            check_row(QStringLiteral("Area measurement"), before_area, after_area);
            const auto& labels = child<sketch::desktop::PlanCanvas>(value, "boundaryGeometryPreview").labels();
            const auto check_labels = [&](const QString& id, const QString& original, const QString& proposed) {
                const auto original_label = std::find_if(labels.begin(), labels.end(),
                    [&](const auto& label) { return label.id == id + QStringLiteral("-original"); });
                const auto proposed_label = std::find_if(labels.begin(), labels.end(),
                    [&](const auto& label) { return label.id == id + QStringLiteral("-proposed"); });
                require(proposed_label != labels.end(), "real preview retains the proposed authored dimension label");
                // The review combines both values at a shared text position to avoid overprinting.
                require(original_label == labels.end()
                    ? proposed_label->text == original + QStringLiteral(" → ") + proposed
                    : original_label->text == original && proposed_label->text == proposed,
                    "original and proposed preview dimensions use the same presentation as their review table");
            };
            check_labels(length, before_length, after_length);
            check_labels(area_dimension, before_area, after_area);
            const auto before_perimeter = ansi ? (metric ? QStringLiteral("40.0 ft (12.192 m)") : QStringLiteral("40.0 ft"))
                                              : QStringLiteral("12.192 m");
            const auto after_perimeter = ansi ? (metric ? QStringLiteral("41.0 ft (12.512 m)") : QStringLiteral("41.0 ft"))
                                             : QStringLiteral("12.512 m");
            require(child<QLabel>(value, "boundaryGeometrySummary").text() ==
                QStringLiteral("Edge: %1 → %2\nAnalytical boundary area: %3 → %4    Perimeter: %5 → %6")
                    .arg(before_length, after_length, before_area, after_area, before_perimeter, after_perimeter),
                "review summary uses the owning property's dimension precision for edge, area and perimeter");
            require(window.document().snapshot().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(), "review preserves exact live geometry and dimensions");
            buttons.button(QDialogButtonBox::Cancel)->click();
        });
    require(window.document().snapshot().revision() == before.revision() &&
        window.document().snapshot().entities() == before.entities(), "Cancel preserves boundary dimensions and history exactly");
}
void boundary_review_precision() {
    boundary_review_precision(true, true);
    boundary_review_precision(false, true);
    boundary_review_precision(true, false);
}
void check_saved_appraisal_sheet(MainWindow& window, const QString& area, const QString& directory) {
    auto source = window.document().snapshot();
    auto sheet_entity = source.entities().at("sheet-view-1");
    const auto model = sketch::decode_sheet_view_entity(sheet_entity);
    auto sheet = model.sheets().front();
    sheet.schedules = {{"ansi-area-summary", "appraisal-areas", {270, 10, 140, 250}}};
    require(!sheet.viewports.empty(), "saved appraisal sheet has a plan viewport");
    sheet.viewports.front().bounds = {10, 10, 250, 250};
    auto schedules = model.schedule_ids();
    if (std::find(schedules.begin(), schedules.end(), "appraisal-areas") == schedules.end())
        schedules.push_back("appraisal-areas");
    sheet_entity.properties["model"] = sketch::SheetViewModel::create(model.views(), {sheet}, schedules).to_json();
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),
        {sketch::EntityChange::upsert(sheet_entity)}, {}, "appraisal sheet precision fixture"});
    require(window.selectEntity(area) && window.selectOutputSheet(QString::fromStdString(sheet.id)),
        "refresh persisted appraisal sheet from the current document");
    const auto project_path = QDir(directory).filePath("ansi-sheet.bldproj");
    require(window.saveProjectAs(project_path) && window.openProject(project_path),
        "ANSI sheet and policy survive native reopening");
    for (const bool metric : {false, true}) {
        window.setMetricUnits(metric);
        require(window.selectEntity(area), "refresh ANSI sheet unit presentation");
        const auto projection = window.scheduleSnapshot();
        bool policy_found = false;
        for (const auto& row : projection.snapshot.rows) {
            if (row.kind != sketch::ScheduleRowKind::appraisal) continue;
            const auto policy = row.cells.find("policy_kind");
            require(policy != row.cells.end() && std::holds_alternative<std::string>(policy->second.value) &&
                std::get<std::string>(policy->second.value) == "ansi_z765_2021",
                "appraisal sheet rows retain their actual measurement policy");
            policy_found = true;
        }
        require(policy_found, "ANSI sheet has actual appraisal rows");
        auto* schedules_action = window.findChild<QAction*>(QStringLiteral("openSchedules"));
        require(schedules_action, "ordinary Schedules action exists");
        dialog(window, "scheduleDialog", [&] { schedules_action->trigger(); }, [&](QDialog& value) {
            auto& table = child<QTableWidget>(value, "scheduleTable");
            int label_column = -1, area_column = -1;
            for (int i = 0; i < table.columnCount(); ++i) {
                if (table.horizontalHeaderItem(i)->text() == "label") label_column = i;
                if (table.horizontalHeaderItem(i)->text() == "area") area_column = i;
            }
            require(label_column >= 0 && area_column >= 0, "schedule table has area and label columns");
            bool found = false;
            for (int i = 0; i < table.rowCount(); ++i) {
                if (table.item(i, label_column)->text() != "Above-grade finished (GLA)") continue;
                const auto displayed = table.item(i, area_column)->text();
                require(displayed.startsWith("100 ft²") &&
                    displayed.contains("supplemental: 9.29 m²") == metric,
                    "actual schedule table matches canonical GLA and supplementary metric output");
                found = true;
            }
            require(found, "ordinary schedule table contains the actual GLA category");
            value.reject();
        });
        const auto path = QDir(directory).filePath(metric ? "ansi-sheet-metric.pdf" : "ansi-sheet-imperial.pdf");
        require(window.exportDraftPdf(path), "ANSI persisted plan sheet exports through the ordinary PDF path");
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
            "actual appraisal sheet PDF loads");
        const auto text = pdf.getAllText(0).text().simplified();
        require(text.contains("Above-grade finished (GLA) 100 ft²") ||
                text.contains("Above-grade finished (GLA) 100 sq ft"),
            "actual ANSI schedule GLA row reports whole square feet in either workspace");
        require(!text.contains("100.00"),
            "canonical ANSI sheet totals do not fall back to workspace rounding");
        require(text.contains("ANSI") && text.contains("validation pending"),
            "saved sheet identifies its policy and pending standards validation");
        require(text.contains(metric ? "10.0 ft (3.048 m)" : "10.0 ft"),
            "plan dimensions and schedule retain the canonical measurement presentation");
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            require(QDir().mkpath(capture), "create isolated saved-sheet capture directory");
            const auto image = pdf.render(0, QSize(1680, 1188));
            require(!image.isNull() && image.save(QDir(capture).filePath(
                metric ? "ansi-sheet-metric.png" : "ansi-sheet-imperial.png")), "render actual ANSI plan sheet");
            QFile original(path), saved(QDir(capture).filePath(
                metric ? "ansi-sheet-metric.pdf" : "ansi-sheet-imperial.pdf"));
            require(original.open(QIODevice::ReadOnly) && saved.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "open isolated ANSI plan PDF capture");
            const auto bytes = original.readAll();
            require(!bytes.isEmpty() && saved.write(bytes) == bytes.size(), "retain actual ANSI plan PDF");
        }
    }
}
void setup_and_facts() {
    QTemporaryDir fixture; require(fixture.isValid(), "isolated fixture");
    MainWindow window({}, nullptr, fixture.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1280, 900); window.show(); QApplication::processEvents();
    child<QTabWidget>(window, "sidebarTabs").setCurrentIndex(2);
    const auto initial = window.document().snapshot();
    const auto open = [&] { child<QPushButton>(window, "appraisalDetailsSetup").click(); };
    dialog(window, "appraisalSetupDialog", open, [&](QDialog& value) {
        choose(value, "appraisalSetupPolicy", "ansi_z765_2021"); value.reject();
    });
    require(window.document().snapshot().entities() == initial.entities(), "cancel preserves setup");
    dialog(window, "appraisalSetupDialog", open, [&](QDialog& value) {
        choose(value, "appraisalSetupPolicy", "ansi_z765_2021");
        choose(value, "appraisalSetupPropertyKind", "light_commercial");
        choose(value, "appraisalSetupMeasurementBasis", "exterior");
        child<QDialogButtonBox>(value, "appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(value.isVisible() && window.document().revision() == initial.revision(), "commercial ANSI setup refuses atomically");
        choose(value, "appraisalSetupPropertyKind", "detached_single_family");
        require(child<QComboBox>(value, "ansiInteriorInspected").currentData().toString().isEmpty(), "inspection starts undeclared");
        choose(value, "ansiInteriorInspected", "yes"); choose(value, "ansiDirectMeasurement", "yes");
        choose(value, "ansiAcquisitionIncrement", "tenth_foot");
        child<QDialogButtonBox>(value, "appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "ANSI setup saves");
    });
    const auto configured = window.document().snapshot();
    require(configured.revision() == initial.revision() + 1 && window.undoCommand() &&
        window.document().snapshot().entities() == initial.entities() && window.redoCommand(), "setup atomic undo redo");
    sketch::Boundary shape{{{0,0},{3.048,0},0},{{3.048,0},{3.048,3.048},0},{{3.048,3.048},{0,3.048},0},{{0,3.048},{0,0},0}};
    const auto area = window.createBoundary(shape); require(!area.isEmpty(), "area authored");
    const auto before = window.document().snapshot();
    const auto edit = [&] { window.showAppraisalFacts(); };
    dialog(window, "appraisalFactsDialog", edit, [&](QDialog& value) {
        choose(value, "ansiAnyPartBelowGrade", "no"); choose(value, "finish", "finished");
        choose(value, "access", "direct_interior"); choose(value, "area_use", "dwelling");
        choose(value, "boundary_role", "measured_area"); choose(value, "ansiYearRoundSuitable", "yes");
        choose(value, "ansiFinishMatchesDwelling", "yes"); choose(value, "ansiDwellingIdentity", "primary");
        choose(value, "ansiCeilingKind", "flat"); child<QLineEdit>(value, "ansiMinimumHeight").setText("6.96 ft");
        require(child<QLabel>(value,"ansiRoundedMinimumHeight").text().contains("7.0 ft (nearest tenth foot)"),
            "facts previews the rounded classification height without replacing the entered observation");
        const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if(!capture.isEmpty()) {
            auto* viewport=value.findChild<QScrollArea*>();require(viewport,"Facts has a scrollable evidence viewport");
            QApplication::processEvents();
            viewport->verticalScrollBar()->setValue(viewport->verticalScrollBar()->maximum());
            QApplication::processEvents();require(QDir().mkpath(capture) &&
                value.grab().save(QDir(capture).filePath("ceiling-height-facts.png")),"capture actual observed and rounded height controls");
        }
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "flat ANSI evidence saves");
    });
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1 && std::abs(after.entities().at(area.toStdString()).properties
        .at("appraisal_facts").at("ansi").at("ceiling").at("minimum_height_m").get<double>() - 6.96*.3048) < 1e-12,
        "height retains explicit observed imperial quantity before acquisition rounding");
    auto& height_details=child<sketch::desktop::AppraisalDetailsPanel>(window,"appraisalDetailsPanel");
    require(child<QLabel>(height_details,"appraisalDetailsGla").text()=="100 sq ft",
        "actual Facts authoring counts the source-backed rounded ceiling observation");
    for(const auto& [height,increment,rounded,gla] : std::vector<std::tuple<const char*,const char*,const char*,const char*>>{
        {"6.85 ft","tenth_foot","6.9 ft (nearest tenth foot)","0 sq ft"},
        {"6.951 ft","tenth_foot","7.0 ft (nearest tenth foot)","100 sq ft"},
        {"6.951 ft","inch","6 ft 11 in (nearest inch)","0 sq ft"}}) {
        dialog(window,"appraisalFactsDialog",edit,[&](QDialog& value) {
            choose(value,"ansiAcquisitionIncrement",increment);
            child<QLineEdit>(value,"ansiMinimumHeight").setText(QString::fromLatin1(height));
            require(child<QLabel>(value,"ansiRoundedMinimumHeight").text().contains(QString::fromLatin1(rounded)),
                "live height preview follows entered observation");
            child<QDialogButtonBox>(value,"appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
            require(value.result()==QDialog::Accepted,"rounded height facts save atomically");
        });
        require(child<QLabel>(height_details,"appraisalDetailsGla").text()==QString::fromLatin1(gla),
            "GLA refreshes to the actual rounded flat-ceiling classification");
        require(window.undoCommand() && window.document().snapshot().entities()==after.entities(),
            "undo preserves the original observed height and classification");
    }
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() && window.redoCommand(), "facts atomic undo redo");
    dialog(window, "appraisalFactsDialog", edit, [&](QDialog& value) {
        child<QLineEdit>(value, "ansiMinimumHeight").setText("7 ft junk");
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.isVisible() && window.document().snapshot().entities() == after.entities(), "invalid height refuses atomically");
    });
    QTemporaryDir directory; require(directory.isValid() && window.saveProjectAs(directory.filePath("ansi.bldproj")) &&
        window.openProject(directory.filePath("ansi.bldproj")) && window.document().snapshot().entities() == after.entities(), "ANSI evidence survives save reopen");
    require(child<QLabel>(height_details,"appraisalDetailsGla").text()=="100 sq ft",
        "reopened observed height uses the same acquisition rounding");
    const auto height_report=sketch::build_appraisal_document_report(window.document().snapshot(),"property-1");
    const auto height_html=sketch::desktop::appraisal_report_html(window.document().snapshot(),height_report,true,true);
    require(height_html.contains("Recorded minimum ceiling height") && height_html.contains("6.96 ft") &&
        height_html.contains("7.0 ft (nearest tenth foot)"),"printable report preserves recorded and rounded ceiling height");
    QString height_error;
    const auto height_pdf_path=directory.filePath("ceiling-height-report.pdf");
    require(sketch::desktop::write_appraisal_report_pdf(window.document().snapshot(),height_report,true,height_pdf_path,height_error),
        "export actual report for recorded and rounded ceiling observations");
    QPdfDocument height_pdf;require(height_pdf.load(height_pdf_path)==QPdfDocument::Error::None,"reopen actual ceiling observation PDF");
    QString height_pdf_text;
    for(int page=0;page<height_pdf.pageCount();++page) {
        const auto page_text=height_pdf.getAllText(page).text().simplified();height_pdf_text+=page_text+QLatin1Char(' ');
        const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if(!capture.isEmpty() && page_text.contains("Recorded minimum ceiling height")) {
            const auto rendered=height_pdf.render(page,QSize(1000,1400));
            require(!rendered.isNull() && rendered.save(QDir(capture).filePath("ceiling-height-report.png")),
                "capture actual exported ceiling-height report page");
        }
    }
    require(height_pdf_text.contains("Recorded minimum ceiling height") && height_pdf_text.contains("6.96 ft") &&
        height_pdf_text.contains("7.0 ft (nearest tenth foot)"),"actual PDF preserves rounded classification and original observation");
    require(window.selectEntity(area), "reselect complete room");
    const auto identified = sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(area.toStdString()));
    const auto dimension = window.createLengthDimension(area, QString::fromStdString(identified.segments.front().segment_id), {1.524,-1});
    require(!dimension.isEmpty(), "ANSI dimension created"); window.setMetricUnits(true);
    auto& canvas = child<sketch::desktop::PlanCanvas>(window, "measurementPlanCanvas");
    const auto dimension_label = std::find_if(canvas.labels().begin(), canvas.labels().end(), [&](const auto& label) { return label.id == dimension; });
    require(dimension_label != canvas.labels().end() && dimension_label->text == "10.0 ft (3.048 m)", "ANSI dimensions retain canonical tenth-foot value with supplementary metric");
    check_saved_appraisal_sheet(window, area, directory.path());
    sketch::Boundary low_shape{{{0.2,0.2},{1.2,0.2},0},{{1.2,0.2},{1.2,1.2},0},{{1.2,1.2},{0.2,1.2},0},{{0.2,1.2},{0.2,0.2},0}};
    const auto low_area = window.createBoundary(low_shape); require(!low_area.isEmpty(), "actual low-height geometry");
    const auto low_source = window.document().snapshot();
    const auto& area_entity = low_source.entities().at(area.toStdString());
    const auto floor_id = area_entity.properties.at("floor_id").get<std::string>();
    const auto building_id = low_source.entities().at(floor_id).properties.at("building_id").get<std::string>();
    const auto property_id = low_source.entities().at(building_id).properties.at("property_id").get<std::string>();
    nlohmann::json exclusion{{"appraisal_policy",low_source.entities().at(property_id).properties.at("appraisal_policy")},
        {"floor_appraisal_facts",low_source.entities().at(floor_id).properties.at("appraisal_facts")},
        {"appraisal_facts",{{"boundary_role","other_void"}}}};
    require(window.editSelectedAppraisalFacts(QString::fromStdString(exclusion.dump())) && window.selectEntity(area), "declare real exclusion and select whole room");
    dialog(window, "appraisalFactsDialog", edit, [&](QDialog& value) {
        choose(value, "ansiCeilingKind", "sloped");
        auto& list = child<QListWidget>(value, "ansiBelow5ftDeductions");
        bool found = false;
        for (int i=0; i<list.count(); ++i) if (list.item(i)->data(Qt::UserRole).toString() == low_area) { list.item(i)->setCheckState(Qt::Checked); found = true; }
        require(found, "real low-height boundary offered");
        child<QLineEdit>(value, "ansiAtLeast7ftArea").clear();
        child<QCheckBox>(value, "ansiConfirmRoomGeometry").setChecked(true);
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "incomplete observations may be recorded");
    });
    const auto incomplete = window.document().snapshot();
    const auto report = sketch::build_appraisal_document_report(incomplete, property_id);
    require(!report.qualified && !report.calculation && incomplete.entities().at(area.toStdString()).properties.at("deduction_ids") == std::vector<std::string>{low_area.toStdString()},
        "missing high-ceiling observation withholds totals while real deduction persists");
    dialog(window, "appraisalFactsDialog", edit, [&](QDialog& value) {
        child<QLineEdit>(value, "ansiAtLeast7ftArea").setText("80");
        child<QCheckBox>(value, "ansiConfirmRoomGeometry").setChecked(true);
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "geometry-bound sloped observation saves");
    });
    const auto sloped = window.document().snapshot();
    const auto& ceiling = sloped.entities().at(area.toStdString()).properties.at("appraisal_facts").at("ansi").at("ceiling");
    require(ceiling.at("room_boundary_id") == area.toStdString() && ceiling.at("source_geometry_sha256") ==
        sketch::appraisal_ceiling_geometry_digest(shape, {{low_area.toStdString(),low_shape}}), "room observation binds exact current geometry and deductions");
    require(window.undoCommand() && window.document().snapshot().entities() == incomplete.entities() && window.redoCommand(), "sloped observation atomic undo redo");
    sketch::Boundary footprint{{{-1,-1},{5,-1},0},{{5,-1},{5,5},0},{{5,5},{-1,5},0},{{-1,5},{-1,-1},0}};
    const auto parent = window.createBoundary(footprint);require(!parent.isEmpty(),"parent footprint authored");
    auto parent_source=window.document().snapshot();
    auto parent_facts=parent_source.entities().at(area.toStdString()).properties.at("appraisal_facts");
    parent_facts["ansi"]["ceiling"]={{"kind","flat"},{"minimum_height_m",2.1336}};
    nlohmann::json parent_declaration={{"appraisal_policy",parent_source.entities().at(property_id).properties.at("appraisal_policy")},
        {"floor_appraisal_facts",parent_source.entities().at(floor_id).properties.at("appraisal_facts")},{"appraisal_facts",parent_facts}};
    require(window.editSelectedAppraisalFacts(QString::fromStdString(parent_declaration.dump())),"parent facts declared");
    dialog(window,"calculationDeductionDialog",[&] { child<QPushButton>(window,"editDeductions").click(); },[&](QDialog& value) {
        auto& source=child<QComboBox>(value,"calculationDeductionSource");
        const auto index=source.findData(area);require(index>=0,"whole room offered as parent deduction");source.setCurrentIndex(index);
        child<QPushButton>(value,"addCalculationDeduction").click();
        child<QDialogButtonBox>(value,"calculationDeductionButtons").button(QDialogButtonBox::Apply)->click();
        require(value.result()==QDialog::Accepted,"native deduction editor permits ANSI footprint room low-height nesting");
    });
    const auto nested=window.document().snapshot();
    const auto nested_report=sketch::build_appraisal_document_report(nested,property_id);
    require(nested_report.qualified && nested_report.calculation,"nested room measurements qualify under current Vertex rules");
    const auto expected_gla=QString::fromStdString(nested_report.calculation->property.gla().total.display.text)+" sq ft";
    auto& details=child<sketch::desktop::AppraisalDetailsPanel>(window,"appraisalDetailsPanel");
    require(child<QLabel>(window,"appraisalGlaTotal").text()==
        QString::fromStdString(nested_report.calculation->property.gla().total.display.text)+QStringLiteral(" ft²"),
        "Properties GLA agrees with authoritative nested appraisal total");
    require(child<QLabel>(details,"appraisalDetailsGla").text()==expected_gla,
        "Details agrees with authoritative nested appraisal total");
    require(child<QLabel>(window,"calculationNetArea").text().contains(QString::fromStdString(
        sketch::display_area(36.0-9.290304,sketch::ansi_appraisal_profile()).text)),
        "Properties exposes parent net after complete room deduction");
    require(sketch::desktop::appraisal_report_html(nested,nested_report,true,true).contains(expected_gla),
        "Printable summary agrees with authoritative nested appraisal total");
    require(child<QLabel>(details,"appraisalDetailsStatus").text().contains("provisional"),
        "live GLA exposes the unresolved sloped denominator even with no room selected");
    require(window.selectEntity(area),"restore room selection for evidence captures");
    const auto capture = [&](const QString& name, QWidget& widget) {
        const auto path = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); if (path.isEmpty()) return;
        QApplication::processEvents(); require(QDir().mkpath(path) && widget.grab().save(QDir(path).filePath(name)), "capture native ANSI UI");
    };
    for (const auto theme : {sketch::WorkspaceTheme::light, sketch::WorkspaceTheme::dark}) {
        window.setWorkspaceTheme(theme); const auto suffix = theme == sketch::WorkspaceTheme::light ? "light" : "dark";
        capture(QStringLiteral("ansi-details-%1.png").arg(suffix), window);
        dialog(window,"appraisalSetupDialog",open,[&](QDialog& value) { capture(QStringLiteral("ansi-setup-%1.png").arg(suffix),value); value.reject(); });
        dialog(window,"appraisalFactsDialog",edit,[&](QDialog& value) { capture(QStringLiteral("ansi-facts-%1.png").arg(suffix),value); value.reject(); });
    }
}
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true); QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-ansi-appraisal-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0, "Inter font loads");
        app.setFont(QFont(QStringLiteral("Inter"), 10));
        if (QCoreApplication::arguments().contains(QStringLiteral("--ansi-boundary-review-only"))) {
            boundary_review_precision();
            std::cout << "ansi_appraisal_desktop_tests boundary review passed\n"; return 0;
        }
        boundary_review_precision(); setup_and_facts(); std::cout << "ansi_appraisal_desktop_tests passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
