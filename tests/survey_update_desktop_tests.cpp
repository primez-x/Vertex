#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "survey_update_desktop: " << message << '\n'; std::exit(1); }
}
const QString original = "NE,90,100 m\nSE,0,100 m\nSW,90,100 m\nNW,0,100 m";
const QString corrected = "NE,90,120 m\nSE,0,100 m\nSW,90,120 m\nNW,0,100 m";
using Window = sketch::desktop::MainWindow;
template<class Function> void survey_dialog(Window& window, Function run) {
    auto* action = window.findChild<QAction*>("surveyTraverse");
    require(action, "Tools survey action exists");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("surveyCalculator");
        require(dialog, "survey calculator opens");
        run(dialog);
        dialog->reject();
    });
    action->trigger();
}
QString create_survey(Window& window) {
    survey_dialog(window, [&](QDialog* dialog) {
        dialog->findChild<QLineEdit*>("surveyProvenance")->setText("Correctable deed");
        dialog->findChild<QPlainTextEdit*>("surveyLegs")->setPlainText(original);
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        auto* add = dialog->findChild<QPushButton*>("surveyAddBoundary");
        require(add && add->isEnabled(), "closed entered calls allow Add boundary");
        add->click();
    });
    require(!window.selectedEntityId().isEmpty(), "new survey is selected");
    return window.selectedEntityId();
}
sketch::Entity entity(Window& window, const QString& id) {
    return window.document().snapshot().entities().at(id.toStdString());
}
void test_update_and_history() {
    Window window;
    const auto id = create_survey(window);
    require(window.editSelectedFactor("0.75"), "survey factor can be customized");
    const auto initial_boundary = sketch::decode_identified_boundary_entity(entity(window, id));
    const auto dimension_id = window.createLengthDimension(id,
        QString::fromStdString(initial_boundary.segments.front().segment_id), {50, 5});
    require(!dimension_id.isEmpty() && window.selectEntity(id), "dimension references original survey edge");
    const auto before = entity(window, id);
    const auto revision = window.document().revision();
    const auto count = window.document().snapshot().entities().size();
    survey_dialog(window, [&](QDialog* dialog) {
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* update = dialog->findChild<QPushButton*>("surveyUpdateBoundary");
        require(input && input->toPlainText() == original && update && update->text() == "Update boundary",
                "selected survey restores original calls and offers Update boundary");
        input->setPlainText(corrected);
        require(!update->isEnabled(), "editing invalidates the previous calculation");
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        require(update->isEnabled(), "corrected closed calls allow update");
        update->click();
        require(window.selectedEntityId() == id && window.document().revision() == revision + 1,
                "Update boundary retains selection and commits exactly once");
    });
    const auto after = entity(window, id);
    auto before_metadata = before.properties;
    auto after_metadata = after.properties;
    for (const auto* key : {"segments", "boundary"}) {
        before_metadata.erase(key);
        after_metadata.erase(key);
    }
    require(window.document().snapshot().entities().size() == count && after_metadata == before_metadata,
            "update retains entity count, classification, layer, name and factor");
    require(after.extensions.at("survey_source").at("report").at("input_provenance").at("legs_text") == corrected.toStdString() &&
            std::abs(after.extensions.at("survey_source").at("report").at("diagnostics").at("area_m2").get<double>() - 12000) < 1e-6,
            "corrected calls and recomputed area are stored together");
    const auto updated_boundary = sketch::decode_identified_boundary_entity(after);
    for (std::size_t i = 0; i < initial_boundary.segments.size(); ++i)
        require(updated_boundary.segments[i].segment_id == initial_boundary.segments[i].segment_id,
                "corresponding survey edges retain identity");
    const auto dimension = sketch::decode_boundary_dimension_entity(entity(window, dimension_id));
    require(dimension.supported() && std::abs(dimension.dimension->resolve(after).segment_length() - 120) < 1e-6,
            "existing edge dimension resolves corrected length");
    require(window.undoCommand() && entity(window, id) == before && window.redoCommand() && entity(window, id) == after,
            "undo and redo restore calls and geometry atomically");
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("corrected.bldproj")) &&
            window.openProject(directory.filePath("corrected.bldproj")) && window.selectEntity(id), "corrected project saves and reopens");
    survey_dialog(window, [&](QDialog* dialog) {
        require(dialog->findChild<QPlainTextEdit*>("surveyLegs")->toPlainText() == corrected,
                "reopened survey exposes corrected calls");
    });
}
void test_invalid_stale_and_closure() {
    Window window;
    const auto id = create_survey(window);
    survey_dialog(window, [&](QDialog* dialog) {
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* update = dialog->findChild<QPushButton*>("surveyUpdateBoundary");
        require(update, "selected survey offers update");
        for (const auto& calls : {QString("NE,91,100 m"), QString("NE,90,100 m")}) {
            input->setPlainText(calls);
            dialog->findChild<QPushButton*>("surveyCalculate")->click();
            require(!update->isEnabled(), "invalid and open calls disable update");
        }
        input->setPlainText("NE,90,120 m\nSE,0,100 m\nSW,90,120 m\nNW,0,99.9995 m");
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        auto* close = dialog->findChild<QCheckBox*>("surveyCloseEndpoint");
        require(close && close->isEnabled() && !close->isChecked() && update->isEnabled(),
                "within-tolerance residual offers an explicit endpoint adjustment choice");
        close->setChecked(true);
        require(update->isEnabled(), "explicit endpoint closure enables update");
        update->click();
    });
    require(entity(window, id).extensions.at("survey_source").at("adjusted_final_endpoint") == true,
            "update records explicit closure consent");
    const auto adjusted = sketch::boundary_geometry(sketch::decode_identified_boundary_entity(entity(window, id)));
    require(adjusted.back().end.x == adjusted.front().start.x && adjusted.back().end.y == adjusted.front().start.y,
            "chosen final endpoint adjustment closes exactly at the origin");
    survey_dialog(window, [&](QDialog* dialog) {
        require(dialog->findChild<QCheckBox*>("surveyCloseEndpoint")->isChecked(),
                "reopening corrected source restores the saved endpoint adjustment choice");
        dialog->findChild<QPlainTextEdit*>("surveyLegs")->setPlainText(original);
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        require(window.editSelectedFactor("0.5"), "change revision while survey dialog is open");
        const auto before = entity(window, id);
        const auto revision = window.document().revision();
        dialog->findChild<QPushButton*>("surveyUpdateBoundary")->click();
        require(window.document().revision() == revision && entity(window, id) == before &&
                dialog->findChild<QLabel*>("surveyResult")->text().contains("context changed", Qt::CaseInsensitive),
                "stale modal revision rejects update without mutation");
    });
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("survey-update-tests-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    require(font >= 0, "bundled Inter font loads");
    app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
    test_update_and_history();
    test_invalid_stale_and_closure();
    std::cout << "Survey update desktop tests passed\n";
}
