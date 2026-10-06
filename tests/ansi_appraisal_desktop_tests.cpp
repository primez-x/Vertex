#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_digest.hpp"
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
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
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
                                  : QStringLiteral("≈ 3.353 m");
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
                                             : QStringLiteral("≈ 12.512 m");
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
void reporting_and_declaration_lifecycle() {
    QTemporaryDir fixture; require(fixture.isValid(), "isolated reporting workflow");
    MainWindow window({}, nullptr, fixture.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1280, 900); window.show();
    QApplication::processEvents(); child<QTabWidget>(window, "sidebarTabs").setCurrentIndex(2);
    const auto setup = [&] { child<QPushButton>(window, "appraisalDetailsSetup").click(); };
    dialog(window, "appraisalSetupDialog", setup, [&](QDialog& value) {
        choose(value, "appraisalSetupPolicy", "ansi_z765_2021");
        choose(value, "appraisalSetupPropertyKind", "detached_single_family");
        choose(value, "appraisalSetupMeasurementBasis", "exterior");
        choose(value, "ansiInteriorInspected", "yes"); choose(value, "ansiDirectMeasurement", "yes");
        choose(value, "ansiAcquisitionIncrement", "inch");
        require(child<QPlainTextEdit>(value, "ansiPlansDeclaration").isHidden(),
            "ordinary inspected exterior setup has no irrelevant declaration field");
        child<QDialogButtonBox>(value, "appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "ordinary ANSI setup saves");
    });
    const sketch::Boundary shape{{{0,0},{3.048,0},0},{{3.048,0},{3.048,3.048},0},
        {{3.048,3.048},{0,3.048},0},{{0,3.048},{0,0},0}};
    const auto area = window.createBoundary(shape); require(!area.isEmpty(), "reporting area authored");
    dialog(window, "appraisalFactsDialog", [&] { window.showAppraisalFacts(); }, [&](QDialog& value) {
        choose(value, "ansiAnyPartBelowGrade", "no"); choose(value, "finish", "finished");
        choose(value, "access", "direct_interior"); choose(value, "area_use", "dwelling");
        choose(value, "boundary_role", "measured_area"); choose(value, "ansiYearRoundSuitable", "yes");
        choose(value, "ansiFinishMatchesDwelling", "yes"); choose(value, "ansiDwellingIdentity", "primary");
        choose(value, "ansiCeilingKind", "flat"); child<QLineEdit>(value, "ansiMinimumHeight").setText("8 ft");
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "observed primary area facts save");
    });
    auto& details = child<sketch::desktop::AppraisalDetailsPanel>(window, "appraisalDetailsPanel");
    const auto property_id = details.propertyId().toStdString();
    const auto before_reporting = window.document().snapshot();
    const auto reporting = [&] { child<QPushButton>(window, "appraisalDetailsReporting").click(); };
    QString primary_unit_id;
    dialog(window, "appraisalReportingDialog", reporting, [&](QDialog& value) {
        auto& units = child<QTableWidget>(value, "appraisalReportingUnits");
        require(units.rowCount() == 0, "legacy reporting starts without inferred living units");
        child<QPushButton>(value, "appraisalReportingAddUnit").click();
        require(units.rowCount() == 1, "Add unit creates an editable registry row");
        units.item(0, 0)->setText("Main dwelling");
        primary_unit_id = units.item(0, 0)->data(Qt::UserRole).toString();
        require(!primary_unit_id.isEmpty(), "unit identifier has a separate stable technical identity");
        auto* primary_role = qobject_cast<QComboBox*>(units.cellWidget(0, 1));
        require(primary_role && primary_role->currentData().toInt() == static_cast<int>(sketch::DwellingIdentity::primary),
            "first unit defaults to the primary dwelling role");
        child<QPushButton>(value, "appraisalReportingAddUnit").click();
        units.item(1, 0)->setText("Unused ADU");
        auto* adu_role = qobject_cast<QComboBox*>(units.cellWidget(1, 1));
        require(adu_role && adu_role->currentData().toInt() == static_cast<int>(sketch::DwellingIdentity::attached_adu),
            "subsequent units default to attached ADU");
        adu_role->setCurrentIndex(adu_role->findData(static_cast<int>(sketch::DwellingIdentity::detached_adu)));
        require(adu_role->currentData().toInt() == static_cast<int>(sketch::DwellingIdentity::detached_adu),
            "unit dwelling role is editable");
        units.setCurrentCell(1, 0);
        child<QPushButton>(value, "appraisalReportingRemoveUnit").click();
        require(units.rowCount() == 1 && units.item(0, 0)->data(Qt::UserRole).toString() == primary_unit_id,
            "Remove unit preserves the remaining unit's technical identity");
        auto& unit = child<QComboBox>(value, "appraisalReportingUnit");
        const auto unit_index = unit.findData(primary_unit_id);
        require(unit_index >= 0 && unit.itemText(unit_index) == "Main dwelling", "area assignment offers the named unit");
        unit.setCurrentIndex(unit_index);
        child<QSpinBox>(value, "appraisalReportingLevelNumber").setValue(1);
        choose(value, "appraisalReportingLevelGrade", "above_grade");
        child<QCheckBox>(value, "appraisalReportingComplete").setChecked(true);
        child<QCheckBox>(value, "appraisalReportingReconfirm").setChecked(true);
        auto& rooms = child<QTableWidget>(value, "appraisalReportingRooms");
        for (const auto& [id, use] : std::vector<std::pair<const char*, sketch::AppraisalRoomUse>>{
            {"bedroom-1", sketch::AppraisalRoomUse::bedroom}, {"bathroom-1", sketch::AppraisalRoomUse::bathroom_full},
            {"kitchen-1", sketch::AppraisalRoomUse::kitchen}, {"music-room-1", sketch::AppraisalRoomUse::other}}) {
            child<QPushButton>(value, "appraisalReportingAddRoom").click();
            const auto row = rooms.rowCount() - 1; rooms.item(row, 0)->setText(QString::fromLatin1(id));
            auto* kind = qobject_cast<QComboBox*>(rooms.cellWidget(row, 1)); require(kind, "room use is editable");
            require(kind->count() == 19, "actual V2 room menu exposes all nineteen original room types");
            kind->setCurrentIndex(kind->findData(static_cast<int>(use)));
            auto* total = qobject_cast<QComboBox*>(rooms.cellWidget(row, 2)); require(total, "legacy room membership is editable");
            total->setCurrentIndex(total->findData(use == sketch::AppraisalRoomUse::bedroom ? 1 : 0));
            auto* description = qobject_cast<QLineEdit*>(rooms.cellWidget(row, 3));
            require(description && description->isEnabled() == (use == sketch::AppraisalRoomUse::other),
                "Other description control is enabled only for the Other original room type");
            if (use == sketch::AppraisalRoomUse::other) description->setText("Music room");
        }
        require(rooms.columnCount() == 4 && rooms.isColumnHidden(2) && !rooms.isColumnHidden(3),
            "UAD 3.6 hides legacy membership while exposing the explicit Other description column");
        const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QApplication::processEvents();
            require(QDir().mkpath(captures) && value.grab().save(QDir(captures).filePath("appraisal-reporting-editor.png")),
                "retain actual source-bound room reporting editor");
        }
        child<QPushButton>(value, "appraisalReportingSave").click();
        require(value.result() == QDialog::Accepted, "reporting commits through actual MainWindow callback");
    });
    const auto reported = window.document().snapshot();
    require(reported.revision() == before_reporting.revision() + 1 && reported.history().size() == before_reporting.history().size() + 1,
        "property and room declarations are one history command");
    const auto report = sketch::build_appraisal_document_report(reported, property_id);
    require(report.qualified && report.reporting && report.reporting->room_counts_available &&
        report.reporting->primary_counts.bedrooms == 1 && report.reporting->primary_counts.bathrooms_full == 1,
        "actual reporting editor produces current explicit primary counts");
    require(report.reporting->individual_units_available && report.reporting->living_units.size() == 1,
        "confirmed assignment produces one independently available named unit");
    const auto& primary_unit = report.reporting->living_units.front();
    require(primary_unit.living_unit.unit_id == primary_unit_id.toStdString() &&
        primary_unit.living_unit.identifier == "Main dwelling" && primary_unit.area_fields_available &&
        primary_unit.room_counts_available && primary_unit.counts.bedrooms == 1 && primary_unit.counts.bathrooms_full == 1 &&
        primary_unit.levels.size() == 1 && primary_unit.levels.front().form_fields_available &&
        primary_unit.levels.front().declaration && primary_unit.levels.front().declaration->level_number == 1 &&
        primary_unit.levels.front().declaration->grade_level_type == "above_grade",
        "saved unit projection retains its declared counts and source-bound Level 1 fields");
    const auto area_reporting = sketch::parse_appraisal_area_reporting_facts(
        reported.entities().at(area.toStdString()).properties.at("appraisal_reporting"));
    require(area_reporting.version == 2 && area_reporting.living_unit_id == primary_unit_id.toStdString(),
        "measured area stores the V2 assignment by technical identity");
    require(child<QLabel>(details, "appraisalDetailsTotals").text().contains("1 bedrooms; 1 full / 0 half bathrooms"),
        "Details shows the same entered room fields");
    const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!captures.isEmpty()) {
        QApplication::processEvents();
        require(QDir().mkpath(captures) && window.grab().save(QDir(captures).filePath("appraisal-reporting-details.png")),
            "retain actual current room fields in the Details workspace");
    }
    require(window.undoCommand() && window.document().snapshot().entities() == before_reporting.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == reported.entities(), "reporting transaction is reversibly atomic");
    dialog(window, "appraisalReportingDialog", reporting, [&](QDialog& value) {
        auto& unit = child<QComboBox>(value, "appraisalReportingUnit");
        require(unit.currentData().toString() == primary_unit_id &&
            child<QSpinBox>(value, "appraisalReportingLevelNumber").value() == 1 &&
            child<QComboBox>(value, "appraisalReportingLevelGrade").currentData().toString() == "above_grade",
            "reopening the editor restores the saved unit assignment and explicit level");
        unit.setCurrentIndex(0);
        child<QCheckBox>(value, "appraisalReportingReconfirm").setChecked(true);
        child<QPushButton>(value, "appraisalReportingSave").click();
        require(value.result() == QDialog::Accepted, "unknown area assignment may be recorded explicitly");
    });
    const auto unassigned_report = sketch::build_appraisal_document_report(window.document().snapshot(), property_id);
    require(unassigned_report.qualified && unassigned_report.calculation && unassigned_report.reporting &&
        unassigned_report.reporting->room_counts_available && unassigned_report.reporting->primary_counts.bedrooms == 1 &&
        !unassigned_report.reporting->individual_units_available,
        "unknown unit assignment withholds individual fields while preserving qualified legacy totals");
    require(window.undoCommand() && window.document().snapshot().entities() == reported.entities(),
        "undo of unknown unit assignment restores the complete named-unit declaration");
    const auto before_cancel = window.document().snapshot();
    dialog(window, "appraisalReportingDialog", reporting, [&](QDialog& value) {
        auto& units = child<QTableWidget>(value, "appraisalReportingUnits");
        child<QPushButton>(value, "appraisalReportingAddUnit").click();
        units.item(1, 0)->setText("Main dwelling");
        child<QPushButton>(value, "appraisalReportingSave").click();
        require(value.result() != QDialog::Accepted && !child<QLabel>(value, "appraisalReportingError").text().isEmpty() &&
            window.document().snapshot().entities() == before_cancel.entities() &&
            window.document().revision() == before_cancel.revision(),
            "duplicate unit identifier refuses Save without changing authoritative entities or history");
    });
    dialog(window, "appraisalReportingDialog", reporting, [&](QDialog& value) {
        auto& contract = child<QComboBox>(value, "appraisalReportingContract");
        contract.setCurrentIndex(contract.findData(static_cast<int>(sketch::AppraisalReportingContract::legacy_uad_2_6)));
        child<QTableWidget>(value, "appraisalReportingUnits").item(0, 0)->setText("Cancelled rename");
        child<QSpinBox>(value, "appraisalReportingLevelNumber").setValue(2);
        value.reject();
    });
    require(window.document().snapshot().entities() == before_cancel.entities() && window.document().revision() == before_cancel.revision(),
        "cancelled report contract does not change facts or history");
    const auto create_adu = [&](double x, const char* identity) {
        const sketch::Boundary boundary{{{x,0},{x+3.048,0},0},{{x+3.048,0},{x+3.048,3.048},0},
            {{x+3.048,3.048},{x,3.048},0},{{x,3.048},{x,0},0}};
        const auto id = window.createBoundary(boundary);
        require(!id.isEmpty() && window.selectEntity(id), "independent nonoverlapping ADU measurement is authored and selected");
        dialog(window, "appraisalFactsDialog", [&] { window.showAppraisalFacts(); }, [&](QDialog& value) {
            choose(value, "ansiAnyPartBelowGrade", "no"); choose(value, "finish", "finished");
            choose(value, "access", "direct_interior"); choose(value, "area_use", "dwelling");
            choose(value, "boundary_role", "measured_area"); choose(value, "ansiYearRoundSuitable", "yes");
            choose(value, "ansiFinishMatchesDwelling", "yes"); choose(value, "ansiDwellingIdentity", identity);
            choose(value, "ansiCeilingKind", "flat"); child<QLineEdit>(value, "ansiMinimumHeight").setText("8 ft");
            child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
            require(value.result() == QDialog::Accepted, "actual Facts dialog saves each ADU dwelling identity");
        });
        return id;
    };
    const auto attached_area = create_adu(5, "attached_adu");
    const auto detached_area = create_adu(10, "detached_adu");
    const auto before_multiunit = window.document().snapshot();
    QString attached_unit_id, detached_unit_id;
    dialog(window, "appraisalReportingDialog", reporting, [&](QDialog& value) {
        auto& units = child<QTableWidget>(value, "appraisalReportingUnits");
        const auto add_unit = [&](const char* identifier, sketch::DwellingIdentity role) {
            child<QPushButton>(value, "appraisalReportingAddUnit").click();
            const auto row = units.rowCount() - 1;
            units.item(row, 0)->setText(QString::fromLatin1(identifier));
            auto* roles = qobject_cast<QComboBox*>(units.cellWidget(row, 1));
            require(roles, "each named ADU has an editable dwelling role");
            roles->setCurrentIndex(roles->findData(static_cast<int>(role)));
            return units.item(row, 0)->data(Qt::UserRole).toString();
        };
        attached_unit_id = add_unit("Garden suite", sketch::DwellingIdentity::attached_adu);
        detached_unit_id = add_unit("Coach house", sketch::DwellingIdentity::detached_adu);
        require(primary_unit_id != attached_unit_id && attached_unit_id != detached_unit_id &&
            primary_unit_id != detached_unit_id, "three named units retain distinct technical identities");
        auto& areas = child<QComboBox>(value, "appraisalReportingArea");
        auto& assignments = child<QComboBox>(value, "appraisalReportingUnit");
        const auto assign = [&](const QString& boundary_id, const QString& unit_id) {
            const auto area_index = areas.findData(boundary_id);
            require(area_index >= 0, "reporting editor offers each actual measured area by stable identity");
            areas.setCurrentIndex(area_index);
            const auto unit_index = assignments.findData(unit_id);
            require(unit_index >= 0, "reporting editor offers each named unit by technical identity");
            assignments.setCurrentIndex(unit_index);
            child<QSpinBox>(value, "appraisalReportingLevelNumber").setValue(1);
            choose(value, "appraisalReportingLevelGrade", "above_grade");
            child<QCheckBox>(value, "appraisalReportingReconfirm").setChecked(true);
        };
        const auto add_room = [&](const char* identifier, sketch::AppraisalRoomUse use) {
            auto& rooms = child<QTableWidget>(value, "appraisalReportingRooms");
            child<QPushButton>(value, "appraisalReportingAddRoom").click();
            const auto row = rooms.rowCount() - 1;
            rooms.item(row, 0)->setText(QString::fromLatin1(identifier));
            auto* types = qobject_cast<QComboBox*>(rooms.cellWidget(row, 1));
            require(types, "ADU original room type is editable");
            types->setCurrentIndex(types->findData(static_cast<int>(use)));
        };
        // Confirm before changing area: each switch retains the current declaration.
        assign(area, primary_unit_id);
        assign(attached_area, attached_unit_id);
        add_room("garden-bedroom-1", sketch::AppraisalRoomUse::bedroom);
        add_room("garden-bedroom-2", sketch::AppraisalRoomUse::bedroom);
        add_room("garden-half-bath", sketch::AppraisalRoomUse::bathroom_half);
        assign(detached_area, detached_unit_id);
        add_room("coach-full-bath-1", sketch::AppraisalRoomUse::bathroom_full);
        add_room("coach-full-bath-2", sketch::AppraisalRoomUse::bathroom_full);
        child<QCheckBox>(value, "appraisalReportingComplete").setChecked(true);
        child<QPushButton>(value, "appraisalReportingSave").click();
        require(value.result() == QDialog::Accepted, "one actual reporting Save commits three independent unit declarations");
    });
    const auto multiunit = window.document().snapshot();
    require(multiunit.revision() == before_multiunit.revision() + 1 &&
        multiunit.history().size() == before_multiunit.history().size() + 1,
        "three-unit registry and area declarations commit as one history command");
    const auto multiunit_report = sketch::build_appraisal_document_report(multiunit, property_id);
    require(multiunit_report.qualified && multiunit_report.reporting &&
        multiunit_report.reporting->individual_units_available && multiunit_report.reporting->living_units.size() == 3,
        "three measured dwelling units produce independently available projections");
    const auto check_unit = [&](const QString& id, const char* identifier, sketch::DwellingIdentity role,
                                const QString& boundary_id, unsigned bedrooms, unsigned full, unsigned half) {
        const auto& projected = multiunit_report.reporting->living_units;
        const auto found = std::find_if(projected.begin(), projected.end(),
            [&](const auto& unit) { return unit.living_unit.unit_id == id.toStdString(); });
        require(found != projected.end() && found->living_unit.identifier == identifier && found->living_unit.role == role &&
            found->area_fields_available && found->room_counts_available && found->counts.bedrooms == bedrooms &&
            found->counts.bathrooms_full == full && found->counts.bathrooms_half == half &&
            found->boundary_ids == std::vector<std::string>{boundary_id.toStdString()} && found->levels.size() == 1 &&
            found->levels.front().form_fields_available && found->levels.front().declaration &&
            found->levels.front().declaration->level_number == 1,
            "named unit retains only its assigned measurements, room counts, role and confirmed Level 1");
    };
    check_unit(primary_unit_id, "Main dwelling", sketch::DwellingIdentity::primary, area, 1, 1, 0);
    check_unit(attached_unit_id, "Garden suite", sketch::DwellingIdentity::attached_adu, attached_area, 2, 0, 1);
    check_unit(detached_unit_id, "Coach house", sketch::DwellingIdentity::detached_adu, detached_area, 0, 2, 0);
    require(window.undoCommand() && window.document().snapshot().entities() == before_multiunit.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == multiunit.entities(),
        "three-unit reporting Save is atomically reversible");
    const auto project = fixture.filePath("reporting.bldproj");
    require(window.saveProjectAs(project) && window.openProject(project) && window.document().snapshot().entities() == multiunit.entities(),
        "actual native save/reopen preserves form contract and source-bound room facts");
    const auto pdf = fixture.filePath("reporting.pdf");
    require(window.exportAppraisalReportPdf(pdf), "actual reopened report exports to PDF");
    QPdfDocument document; require(document.load(pdf) == QPdfDocument::Error::None, "report PDF opens independently");
    QString contents; for (int page = 0; page < document.pageCount(); ++page) contents += document.getAllText(page).text();
    require(contents.contains("1 bedrooms") && contents.contains("1 full") && contents.contains("UAD 3.6"),
        "reopened printed report includes entered contract and room counts");
    require(contents.contains("Main dwelling") && contents.contains("Level 1") &&
        contents.contains("Unit bedroom / bathroom counts") && contents.contains("Measured level finished"),
        "reopened PDF includes the named unit and explicit level fields");
    require(contents.contains("Kitchen") && contents.contains("Music room"),
        "reopened PDF preserves actual menu-selected Kitchen and explicitly described Other room");
    const auto printed = contents.simplified();
    const auto garden_start = printed.indexOf("Garden suite");
    const auto coach_start = printed.indexOf("Coach house", garden_start + 1);
    require(garden_start >= 0 && coach_start > garden_start &&
        printed.mid(garden_start, coach_start - garden_start).contains("2 bedrooms; 0 full / 1 half bathrooms") &&
        printed.mid(coach_start).contains("0 bedrooms; 2 full / 0 half bathrooms"),
        "reopened PDF reports each ADU's independent entered bedroom and bathroom counts under its name");
    dialog(window, "appraisalSetupDialog", setup, [&](QDialog& value) {
        choose(value, "appraisalSetupMeasurementBasis", "plans");
        require(!child<QPlainTextEdit>(value, "ansiPlansDeclaration").isHidden(), "plans condition exposes its declaration field");
        child<QLineEdit>(value, "ansiLimitationsStatement").setText("General measurement notes only.");
        child<QDialogButtonBox>(value, "appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "incomplete observations are saved without being qualified");
    });
    require(!sketch::build_appraisal_document_report(window.document().snapshot(), property_id).qualified,
        "generic notes do not satisfy the required plans declaration");
    dialog(window, "appraisalSetupDialog", setup, [&](QDialog& value) {
        child<QPlainTextEdit>(value, "ansiPlansDeclaration").setPlainText("Measurement based on the supplied building plans.");
        child<QDialogButtonBox>(value, "appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "explicit plans declaration is recorded");
    });
    const auto declared = window.document().snapshot();
    const auto declared_report = sketch::build_appraisal_document_report(declared, property_id);
    require(declared_report.qualified && declared_report.ansi_measurement &&
        declared_report.ansi_measurement->limitation_declarations.size() == 1 &&
        sketch::desktop::appraisal_report_html(declared, declared_report, false).contains("supplied building plans"),
        "typed declaration reaches the current calculation and report");
    dialog(window, "appraisalReportingDialog", reporting, [&](QDialog& value) {
        auto replacement = window.document().snapshot();
        const auto captured_digest = sketch::document_snapshot_digest(replacement);
        auto& retained = const_cast<std::vector<sketch::RevisionRecord>&>(replacement.history());
        const auto property_record = std::find_if(retained.begin(), retained.end(), [&](const auto& record) {
            return record.revision < replacement.revision() && record.entities.contains(property_id);
        });
        require(property_record != retained.end(), "fixture contains a retained property snapshot before the current head");
        property_record->entities.at(property_id).properties["name"] = "Altered retained property history";
        window.document() = sketch::Document::fork(replacement);
        const auto replaced = window.document().snapshot();
        require(replaced.revision() == replacement.revision() && replaced.document_id() == replacement.document_id() &&
            sketch::document_snapshot_digest(replaced) != captured_digest, "retained source changes while identity and revision stay the same");
        child<QPushButton>(value, "appraisalReportingSave").click();
        require(value.result() != QDialog::Accepted && !child<QLabel>(value, "appraisalReportingError").text().isEmpty() &&
            sketch::document_snapshot_digest(window.document().snapshot()) == sketch::document_snapshot_digest(replaced),
            "full-source fence refuses retained-history replacement without another command");
    });
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
        auto& rule=child<QComboBox>(value,"appraisalSetupRuleVersion");
        require(rule.currentData().toInt()==2,"new ANSI setup offers the finished-room rule by default");
        rule.setCurrentIndex(rule.findData(1));
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
    const auto legacy=window.document().snapshot();
    dialog(window,"appraisalSetupDialog",open,[&](QDialog& value) {
        auto& rule=child<QComboBox>(value,"appraisalSetupRuleVersion");
        require(rule.currentData().toInt()==1,"opening existing V1 setup retains its recorded interpretation");
        rule.setCurrentIndex(rule.findData(2)); value.reject();
    });
    require(window.document().snapshot().entities()==legacy.entities(),"cancelled rule migration preserves every observation");
    dialog(window,"appraisalSetupDialog",open,[&](QDialog& value) {
        auto& rule=child<QComboBox>(value,"appraisalSetupRuleVersion");rule.setCurrentIndex(rule.findData(2));
        child<QDialogButtonBox>(value,"appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(value.result()==QDialog::Accepted,"explicit V2 migration saves through the ordinary command");
    });
    const auto migrated=window.document().snapshot();
    const auto unconfirmed=sketch::build_appraisal_document_report(migrated,property_id);
    require(migrated.entities().at(property_id).properties.at("appraisal_policy").at("version")==2 &&
        !unconfirmed.qualified && !unconfirmed.calculation &&
        migrated.entities().at(area.toStdString()).properties==legacy.entities().at(area.toStdString()).properties,
        "migration retains raw observations and withholds sloped totals until new complete-room confirmation");
    require(window.undoCommand() && window.document().snapshot().entities()==legacy.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==migrated.entities(),"rule migration is atomically reversible");
    require(window.selectEntity(area),"select complete room for V2 reconfirmation");
    dialog(window,"appraisalFactsDialog",edit,[&](QDialog& value) {
        child<QLineEdit>(value,"ansiAtLeast7ftArea").setText("47");
        child<QCheckBox>(value,"ansiConfirmRoomGeometry").setChecked(true);
        child<QDialogButtonBox>(value,"appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.result()==QDialog::Accepted,"V2 records candidate-scoped observed height area");
    });
    const auto confirmed=window.document().snapshot();
    const auto v2=sketch::build_appraisal_document_report(confirmed,property_id);
    require(v2.qualified && v2.calculation &&
        confirmed.entities().at(area.toStdString()).properties.at("appraisal_facts").at("ansi").at("ceiling").at("complete_room_observed")==true,
        "47 high square feet qualify against the room's 89 countable square feet");
    details.setSelectedBoundary(area);
    const auto detail_trace=child<QLabel>(details,"appraisalDetailsTrace").text();
    const auto v2_html=sketch::desktop::appraisal_report_html(confirmed,v2,true,true);
    for(const auto& label:{"V2: countable finished room after exclusions","Ceiling threshold denominator","Seven-foot share (50% required)"})
        require(detail_trace.contains(label) && v2_html.contains(label),"Details and PDF share the explicit unrounded ceiling calculation basis");
    require(v2_html.contains("vertex-ansi-z765-2021-v2") &&
        child<QLabel>(details,"appraisalDetailsStatus").text().contains("V2 sloped rooms"),"visible report provenance identifies V2 without claiming certification");
    QTemporaryDir v2_directory;require(v2_directory.isValid(),"V2 save fixture has a unique local folder");
    require(window.saveProjectAs(v2_directory.filePath("ansi-v2.bldproj")) && window.openProject(v2_directory.filePath("ansi-v2.bldproj")) &&
        window.document().snapshot().entities()==confirmed.entities(),"V2 native save/reopen preserves rule and complete-room evidence");
    require(window.selectEntity(area),"reopened V2 room can be selected for the current Details trace");
    for(const auto theme:{sketch::WorkspaceTheme::light,sketch::WorkspaceTheme::dark}) {
        window.setWorkspaceTheme(theme); const auto suffix=theme==sketch::WorkspaceTheme::light?"light":"dark";
        capture(QStringLiteral("ansi-v2-details-%1.png").arg(suffix),window);
        capture(QStringLiteral("ansi-v2-ceiling-trace-%1.png").arg(suffix),child<QLabel>(details,"appraisalDetailsTrace"));
        dialog(window,"appraisalSetupDialog",open,[&](QDialog& value){capture(QStringLiteral("ansi-v2-setup-%1.png").arg(suffix),value);value.reject();});
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
        std::cout << "Boundary review precision\n" << std::flush; boundary_review_precision();
        std::cout << "Setup and observation lifecycle\n" << std::flush; setup_and_facts();
        std::cout << "Reporting and declaration lifecycle\n" << std::flush; reporting_and_declaration_lifecycle();
        std::cout << "ansi_appraisal_desktop_tests passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
