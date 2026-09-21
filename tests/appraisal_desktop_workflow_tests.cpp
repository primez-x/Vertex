#include "sketch/desktop/main_window.hpp"
#include "sketch/model_phases.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

sketch::Boundary square(double x, double y, double size) {
    return {{{x, y}, {x + size, y}, 0.0},
            {{x + size, y}, {x + size, y + size}, 0.0},
            {{x + size, y + size}, {x, y + size}, 0.0},
            {{x, y + size}, {x, y}, 0.0}};
}

void appraisal_workflow_is_automatic_and_persistent() {
    using sketch::desktop::MainWindow;

    MainWindow window;
    const auto finished = window.createBoundary(square(0.0, 0.0, 3.048));
    require(!finished.isEmpty(), "appraisal fixture needs a finished-area boundary");

    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow != nullptr, "measurement properties must expose a first-class appraisal workflow selector");
    const auto appraisal_index = workflow->findData(QStringLiteral("appraisal"));
    require(appraisal_index >= 0, "workflow selector must offer automatic appraisal calculations");
    workflow->setCurrentIndex(appraisal_index);

    const auto activated = window.document().snapshot().entities().at("property-1");
    require(activated.properties.at("calculation_workflow") == "appraisal" &&
                activated.properties.at("calculation_profile").at("id") == "vertex-appraisal" &&
                activated.properties.at("calculation_profile").at("classifications")
                        .at("above_grade_finished")
                        .at("appraisal_category") == "above_grade_finished",
            "activating appraisal must persist the built-in versioned semantic profile");

    require(window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "selected measurement boundary must accept an appraisal category");
    const auto classified_finished =
        window.document().snapshot().entities().at(finished.toStdString());
    require(classified_finished.properties.at("classification") == "measurement" &&
                classified_finished.properties.at("measurement_classification") == "measurement" &&
                classified_finished.properties.at("appraisal_category") == "above_grade_finished",
            "appraisal edits must retain an independent measurement classification");
    auto* summary = window.findChild<QGroupBox*>(QStringLiteral("appraisalSummary"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    auto* floor = window.findChild<QLabel*>(QStringLiteral("appraisalFloorTotal"));
    auto* property = window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"));
    auto* garage_total = window.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"));
    auto* provenance = window.findChild<QLabel*>(QStringLiteral("appraisalContribution"));
    require(summary && !summary->isHidden() && gla && floor && property && garage_total && provenance,
            "appraisal mode must expose GLA, floor, property, excluded-area, and provenance results");
    require(gla->text().contains(QStringLiteral("100.00")) &&
                floor->text().contains(QStringLiteral("100.00")) &&
                property->text().contains(QStringLiteral("100.00")) &&
                provenance->text().contains(finished),
            "a ten-foot-square above-grade finished boundary must automatically report 100 square feet of GLA");

    const auto garage = window.createBoundary(square(1.0, 1.0, 1.0));
    require(!garage.isEmpty() && window.editSelectedClassification(QStringLiteral("garage")),
            "appraisal fixture needs a classified garage inside the gross footprint");
    auto deduction_snapshot = window.document().snapshot();
    auto parent = deduction_snapshot.entities().at(finished.toStdString());
    parent.properties["deduction_ids"] =
        std::vector<std::string>{garage.toStdString()};
    window.document().apply(sketch::ApplyEntityChanges{
        deduction_snapshot.revision(),
        {sketch::EntityChange::upsert(std::move(parent))},
        {},
        "link appraisal garage deduction"});
    require(window.selectEntity(finished),
            "appraisal fixture must refresh the enclosing finished boundary");
    require(gla->text().contains(QStringLiteral("89.24")) &&
                garage_total->text().contains(QStringLiteral("10.76")) &&
                property->text().contains(QStringLiteral("100.00")) &&
                provenance->text().contains(finished),
            "an internal garage must reduce GLA, remain in the garage bucket, and preserve the property total");
    require(window.selectEntity(garage) &&
                provenance->text().contains(garage) &&
                garage_total->text().contains(QStringLiteral("10.76")),
            "the independently classified garage deduction must remain selectable with provenance");

    const auto measurement_index = workflow->findData(QStringLiteral("measurement"));
    require(measurement_index >= 0, "workflow selector must retain Measurement mode");
    workflow->setCurrentIndex(measurement_index);
    auto measurement_state = window.document().snapshot();
    require(measurement_state.entities().at(finished.toStdString())
                    .properties.at("classification") == "measurement" &&
                measurement_state.entities().at(garage.toStdString())
                    .properties.at("classification") == "measurement",
            "switching back to Measurement must restore usable measurement classifications");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at("property-1")
                    .properties.at("calculation_workflow") == "appraisal" &&
                window.redoCommand() &&
                window.document().snapshot().entities().at("property-1")
                    .properties.at("calculation_workflow") == "measurement",
            "workflow and classification migration must be one undoable and redoable command");
    workflow->setCurrentIndex(appraisal_index);
    require(window.selectEntity(finished) &&
                gla->text().contains(QStringLiteral("89.24")) &&
                garage_total->text().contains(QStringLiteral("10.76")),
            "returning to Appraisal must restore explicit categories and calculated totals");

    QTemporaryDir directory;
    require(directory.isValid(), "appraisal persistence fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("appraisal.bldproj"));
    require(window.saveProjectAs(path), "appraisal project must save locally");

    MainWindow reopened;
    require(reopened.openProject(path) && reopened.selectEntity(finished),
            "saved appraisal project must reopen with the contributing boundary selectable");
    auto* reopened_workflow = reopened.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    auto* reopened_gla = reopened.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    auto* reopened_garage = reopened.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"));
    require(reopened_workflow && reopened_workflow->currentData() == QStringLiteral("appraisal") &&
                reopened_gla && reopened_gla->text().contains(QStringLiteral("89.24")) &&
                reopened_garage && reopened_garage->text().contains(QStringLiteral("10.76")),
            "save and reopen must reproduce the selected appraisal workflow, classifications, and totals");
}

void appraisal_redefinition_updates_the_active_category() {
    using sketch::desktop::MainWindow;

    MainWindow window;
    const auto area = window.createBoundary(square(0.0, 0.0, 2.0));
    require(!area.isEmpty(), "appraisal redefinition fixture needs a boundary");
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow != nullptr, "appraisal redefinition fixture needs the workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "appraisal redefinition fixture must start in GLA");
    require(window.redefineSelectedBoundary(square(0.0, 0.0, 3.048),
                                            QStringLiteral("garage")),
            "redefinition must accept a new appraisal category");
    const auto updated = window.document().snapshot().entities().at(area.toStdString());
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    auto* garage = window.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"));
    require(updated.properties.at("classification") == "measurement" &&
                updated.properties.at("measurement_classification") == "measurement" &&
                updated.properties.at("appraisal_category") == "garage" &&
                gla && gla->text().contains(QStringLiteral("0.00")) &&
                garage && garage->text().contains(QStringLiteral("100.00")),
            "redefinition must update the appraisal category used by automatic totals");
}

QString declarations(const char* kind = "residential_declared", const char* use = "dwelling",
                     const char* grade = "above", const char* role = "measured_area") {
    return QStringLiteral(R"({"appraisal_policy":{"policy_kind":"%1","version":1,"property_kind":"%2","measurement_basis":"exterior"},"grade":"%3","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"%4","boundary_role":"%5"}})")
        .arg(QString::fromLatin1(kind), QString::fromLatin1(std::string_view(kind) == "residential_declared" ? "detached_single_family" : "light_commercial"),
             QString::fromLatin1(grade), QString::fromLatin1(use), QString::fromLatin1(role));
}

void declared_appraisal_qualifies_without_manual_categories() {
    using sketch::desktop::MainWindow;
    MainWindow window;
    const auto area = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* derived = window.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(qualification && qualification->text().contains("Unqualified"), "undeclared geometry must never be qualified");
    const auto before = window.document().snapshot().revision();
    require(window.editSelectedAppraisalFacts(declarations()), "valid declarations must commit atomically");
    require(qualification->text().startsWith("Qualified") && derived->text().contains("above_grade_finished") && gla->text().contains("100.00"),
            "facts must qualify and derive GLA without manual category assignment");
    const auto stored = window.document().snapshot();
    require(stored.entities().at("property-1").properties.at("appraisal_policy").at("version") == 1 &&
            stored.entities().at(area.toStdString()).properties.at("appraisal_facts").at("finish") == "finished",
            "typed declaration fields must persist on property and boundary");
    require(window.undoCommand() && qualification->text().contains("Unqualified") && window.redoCommand() && qualification->text().startsWith("Qualified"),
            "all three declaration scopes must undo and redo together");
    const auto stable = window.document().snapshot().revision();
    require(!window.editSelectedAppraisalFacts(declarations().replace("\"version\":1", "\"version\":2")) &&
            window.document().snapshot().revision() == stable, "unsupported versions must fail without mutation");
    require(!window.editSelectedAppraisalFacts(declarations().replace("\"finished\"", "\"typo\"")) &&
            !window.editSelectedAppraisalFacts(declarations().replace("\"policy_kind\"", "\"kind\"")) &&
            !window.editSelectedAppraisalFacts(declarations(), before) &&
            window.document().snapshot().revision() == stable, "unknown tokens and stale edits must fail atomically");
    require(window.editSelectedAppraisalFacts(declarations().replace("\"finish\":\"finished\",", "")) &&
            qualification->text().contains("Unqualified") && qualification->text().contains("Declare finish") &&
            !gla->text().contains("100.00"), "missing declarations must withhold automatic totals instead of inferring facts");
    require(window.editSelectedAppraisalFacts(declarations("residential_declared", "dwelling", "below")) &&
            derived->text().contains("below_grade_finished") && gla->text().contains("0.00"),
            "declared floor grade must override neither name nor elevation, and exclude below grade from GLA");
    require(window.editSelectedFactor(QStringLiteral("0.5")) && qualification->text().contains("Unqualified") &&
            derived->text().contains("Physical: 100.00") && derived->text().contains("Adjusted: 50.00") &&
            !gla->text().contains("50.00"), "nonunity factors must show physical and adjusted values without qualifying adjusted GLA");
    require(window.editSelectedFactor(QStringLiteral("1")), "restore physical factor");
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("declared.bldproj"));
    require(window.saveProjectAs(path), "declared project saves");
    MainWindow reopened;
    require(reopened.openProject(path) && reopened.selectEntity(area), "declared project reopens");
    require(reopened.findChild<QLabel*>(QStringLiteral("appraisalQualification"))->text().startsWith("Qualified") &&
            reopened.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"))->text().contains("below_grade_finished"),
            "reopen must recalculate from persisted facts");

    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("appraisalFactsDialog"));
        require(dialog != nullptr, "facts dialog must be reachable");
        require(dialog->findChild<QComboBox*>(QStringLiteral("property_kind"))->currentText() == QStringLiteral("Detached single-family"),
                "dialog must display human labels while retaining stable fact tokens");
        auto* grade = dialog->findChild<QComboBox*>(QStringLiteral("grade"));
        grade->setCurrentIndex(grade->findData(QStringLiteral("above")));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    QTimer::singleShot(3000, &window, [&] {
        if (auto* dialog = window.findChild<QDialog*>(QStringLiteral("appraisalFactsDialog"))) {
            dialog->reject();
        }
    });
    window.showAppraisalFacts();
    require(window.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"))->text().contains("above_grade_finished"),
            "dialog must submit declarations through the same command");
}

void declared_exclusions_and_commercial_totals() {
    sketch::desktop::MainWindow window;
    const auto outer = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "declare outer dwelling");
    const auto hole = window.createBoundary(square(1, 1, 1));
    require(window.editSelectedAppraisalFacts(declarations("residential_declared", "dwelling", "above", "open_to_below")), "declare void");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    require(qualification->text().contains("Exclusion must be linked"), "unlinked exclusions must not yield plausible qualified totals");
    auto snapshot = window.document().snapshot();
    auto parent = snapshot.entities().at(outer.toStdString());
    parent.properties["deduction_ids"] = std::vector<std::string>{hole.toStdString()};
    window.document().apply(sketch::ApplyEntityChanges{snapshot.revision(), {sketch::EntityChange::upsert(parent)}, {}, "link void"});
    require(window.selectEntity(hole) && qualification->text().startsWith("Qualified") &&
            window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"))->text().contains("89.24"),
            "selected void must not contribute independently or hide valid parent totals");
    require(window.selectEntity(outer) && window.editSelectedAppraisalFacts(declarations("light_commercial_declared", "commercial_occupiable")), "declare commercial area");
    require(window.selectEntity(hole) && window.editSelectedAppraisalFacts(declarations("light_commercial_declared", "commercial_service")), "declare service area");
    require(qualification->text().startsWith("Qualified") &&
            window.findChild<QLabel*>(QStringLiteral("appraisalCommercialTotals"))->text().contains("89.24") &&
            window.findChild<QLabel*>(QStringLiteral("appraisalCommercialTotals"))->text().contains("10.76") &&
            window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"))->text().contains("100.00"),
            "occupiable and service categories must partition commercial gross area without double counting");
    require(window.editSelectedAppraisalFacts(declarations("light_commercial_declared", "commercial_common")) &&
            window.findChild<QLabel*>(QStringLiteral("appraisalCommercialTotals"))->text().contains(QStringLiteral("89.24 ft² / 10.76 ft² / 0.00 ft²")),
            "commercial common area must have its own derived bucket");
}

void declared_appraisal_excludes_site_boundaries() {
    sketch::desktop::MainWindow window;
    const auto dwelling = window.createBoundary(square(0, 0, 3.048));
    const auto explicit_site = window.createBoundary(square(20, 20, 10), QStringLiteral("survey"));
    const auto legacy_site = window.createBoundary(square(22, 22, std::sqrt(10.0)), QStringLiteral("survey"));
    require(!dwelling.isEmpty() && !explicit_site.isEmpty() && !legacy_site.isEmpty(),
            "mixed appraisal fixture needs one building and two site boundaries");

    auto snapshot = window.document().snapshot();
    auto legacy = snapshot.entities().at(legacy_site.toStdString());
    legacy.properties.erase("calculation_scope");
    auto site_parent = snapshot.entities().at(explicit_site.toStdString());
    site_parent.properties["deduction_ids"] = std::vector<std::string>{legacy_site.toStdString()};
    window.document().apply(sketch::ApplyEntityChanges{
        snapshot.revision(), {sketch::EntityChange::upsert(legacy),
                              sketch::EntityChange::upsert(site_parent)}, {},
        "make site deduction and implicit legacy survey fixture"});

    require(window.selectEntity(explicit_site), "site parent must be selectable in Measurement mode");
    auto* net = window.findChild<QLabel*>(QStringLiteral("calculationNetArea"));
    auto* deductions = window.findChild<QListWidget*>(QStringLiteral("calculationDeductions"));
    require(net->text().contains("968.75") && deductions->count() == 1 &&
                deductions->item(0)->text().contains(legacy_site),
            "Measurement mode must retain site-to-site deduction arithmetic and trace");

    require(window.selectEntity(dwelling), "dwelling must be selected before appraisal activation");
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "dwelling declarations must commit");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    auto* property = window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"));
    require(qualification->text().startsWith("Qualified") && gla->text().contains("100.00") &&
                property->text().contains("100.00"),
            "explicit and legacy site outlines must not require building appraisal facts or alter totals");

    require(window.selectEntity(explicit_site), "site boundary must remain selectable");
    auto* status = window.findChild<QLabel*>(QStringLiteral("calculationStatus"));
    require(status->text().contains("excluded from building appraisal") &&
                property->text().contains("100.00") && net->text().contains("968.75") &&
                deductions->count() == 1 && deductions->item(0)->text().contains(legacy_site),
            "selected site area must explain its exclusion, preserve its deduction trace and leave building totals unchanged");

    const auto phases = sketch::ModelPhases::create(
        {dwelling.toStdString(), explicit_site.toStdString(), legacy_site.toStdString()},
        {dwelling.toStdString(), explicit_site.toStdString(), legacy_site.toStdString()},
        {{"hide-site-deduction", "Hide site deduction", {legacy_site.toStdString()}, {}}});
    auto phase_entity = sketch::Entity::create("model_phases", {{"model", phases.to_json()}});
    phase_entity.id = "appraisal-site-phases";
    window.document().apply(sketch::ApplyEntityChanges{
        window.document().revision(), {sketch::EntityChange::upsert(phase_entity)}, {},
        "add appraisal site phase fixture"});
    require(window.selectEntity(dwelling) &&
                window.selectRemodelingAlternative(QStringLiteral("hide-site-deduction")) &&
                qualification->text().startsWith("Qualified") && property->text().contains("100.00"),
            "a hidden site-only deduction must not block independent building appraisal totals");
    require(window.selectEntity(explicit_site) &&
                status->text().contains("hidden by the active design phase"),
            "selecting the site must explicitly block its own stale hidden-deduction calculation");
    require(window.selectRemodelingAlternative(QString{}),
            "site fixture must return to the existing baseline");

    snapshot = window.document().snapshot();
    auto parent = snapshot.entities().at(dwelling.toStdString());
    parent.properties["deduction_ids"] = std::vector<std::string>{explicit_site.toStdString()};
    window.document().apply(sketch::ApplyEntityChanges{
        snapshot.revision(), {sketch::EntityChange::upsert(parent)}, {},
        "reject site as building deduction fixture"});
    require(window.selectEntity(dwelling) && status->text().contains("cannot be used as a building-area deduction"),
            "site geometry must never be accepted as a building-area deduction");
}

void declared_appraisal_reports_selected_building_floor_and_property() {
    sketch::desktop::MainWindow window;
    const auto first = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "first building declarations must commit");

    const auto second_building = window.createBuilding(QStringLiteral("property-1"),
                                                       QStringLiteral("Building 2"));
    const auto second_floor = window.createFloor(second_building, QStringLiteral("Floor 1"));
    const auto second = window.createBoundary(square(20, 0, 6.096));
    require(!second_building.isEmpty() && !second_floor.isEmpty() && !second.isEmpty() &&
                window.editSelectedAppraisalFacts(declarations()),
            "second building must accept its own floor and appraisal area");

    auto* building = window.findChild<QLabel*>(QStringLiteral("calculationBuildingTotal"));
    auto* floor = window.findChild<QLabel*>(QStringLiteral("appraisalFloorTotal"));
    auto* property = window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"));
    require(building->text().contains("400.00") && floor->text().contains("400.00") &&
                property->text().contains("500.00"),
            "selecting building 2 must show its 400 square feet separately from the 500 square foot property");
    require(window.selectEntity(first) && building->text().contains("100.00") &&
                floor->text().contains("100.00") && property->text().contains("500.00"),
            "selecting building 1 must change building and floor totals without changing the property total");
}

void appraisal_declarations_reject_read_only_documents() {
    sketch::desktop::MainWindow window;
    require(!window.createBoundary(square(0, 0, 3.048)).isEmpty(),
            "read-only fixture needs a selected boundary");
    const auto revision = window.document().revision();
    window.document().mark_read_only("appraisal read-only fixture");
    require(!window.editSelectedAppraisalFacts(declarations()) &&
                window.document().revision() == revision &&
                window.lastError().contains("read-only"),
            "appraisal declarations must reject a read-only document without mutation");
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        appraisal_workflow_is_automatic_and_persistent();
        appraisal_redefinition_updates_the_active_category();
        declared_appraisal_qualifies_without_manual_categories();
        declared_exclusions_and_commercial_totals();
        declared_appraisal_excludes_site_boundaries();
        declared_appraisal_reports_selected_building_floor_and_property();
        appraisal_declarations_reject_read_only_documents();
        std::cout << "appraisal_desktop_workflow_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "appraisal_desktop_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
