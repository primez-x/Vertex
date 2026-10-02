#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QPixmap>
#include <QStandardPaths>
#include <QTabWidget>
#include <QUuid>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <vector>

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
        choose(value, "ansiCeilingKind", "flat"); child<QLineEdit>(value, "ansiMinimumHeight").setText("7 ft");
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.result() == QDialog::Accepted, "flat ANSI evidence saves");
    });
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1 && std::abs(after.entities().at(area.toStdString()).properties
        .at("appraisal_facts").at("ansi").at("ceiling").at("minimum_height_m").get<double>() - 2.1336) < 1e-12,
        "height parses explicit imperial units");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() && window.redoCommand(), "facts atomic undo redo");
    dialog(window, "appraisalFactsDialog", edit, [&](QDialog& value) {
        child<QLineEdit>(value, "ansiMinimumHeight").setText("7 ft junk");
        child<QDialogButtonBox>(value, "appraisalFactsButtons").button(QDialogButtonBox::Save)->click();
        require(value.isVisible() && window.document().snapshot().entities() == after.entities(), "invalid height refuses atomically");
    });
    QTemporaryDir directory; require(directory.isValid() && window.saveProjectAs(directory.filePath("ansi.bldproj")) &&
        window.openProject(directory.filePath("ansi.bldproj")) && window.document().snapshot().entities() == after.entities(), "ANSI evidence survives save reopen");
    require(window.selectEntity(area), "reselect complete room");
    const auto identified = sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(area.toStdString()));
    const auto dimension = window.createLengthDimension(area, QString::fromStdString(identified.segments.front().segment_id), {1.524,-1});
    require(!dimension.isEmpty(), "ANSI dimension created"); window.setMetricUnits(true);
    auto& canvas = child<sketch::desktop::PlanCanvas>(window, "measurementPlanCanvas");
    const auto dimension_label = std::find_if(canvas.labels().begin(), canvas.labels().end(), [&](const auto& label) { return label.id == dimension; });
    require(dimension_label != canvas.labels().end() && dimension_label->text == "10.0 ft (3.048 m)", "ANSI dimensions retain canonical tenth-foot value with supplementary metric");
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
        setup_and_facts(); std::cout << "ansi_appraisal_desktop_tests passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
