#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QTemporaryDir>

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

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        appraisal_workflow_is_automatic_and_persistent();
        appraisal_redefinition_updates_the_active_category();
        std::cout << "appraisal_desktop_workflow_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "appraisal_desktop_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
