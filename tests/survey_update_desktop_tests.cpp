#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/survey_report.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "survey_update_desktop: " << message << '\n'; std::exit(1); }
}
const QString original = "NE,90,100 m\nSE,0,100 m\nSW,90,100 m\nNW,0,100 m";
const QString corrected = "NE,90,120 m\nSE,0,100 m\nSW,90,120 m\nNW,0,100 m";
const QString curved = "NE,0,10 m\nCURVE,NE,90,20 m,-180\nSE,0,10 m\nSW,90,20 m";
const QString curved_corrected = "NE,0,10 m\nCURVE,NE,90,40 m,-180\nSE,0,10 m\nSW,90,40 m";
constexpr double pi = 3.14159265358979323846;
QString capture_directory;
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
void choose_report_file(QDialog* dialog, const char* action, const QString& path) {
    const auto native_dialogs_disabled = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QTimer::singleShot(0, dialog, [&] {
        auto* picker = dialog->findChild<QFileDialog*>();
        require(picker, "survey report file picker opens");
        picker->selectFile(path);
        QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
    });
    dialog->findChild<QPushButton*>(action)->click();
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_dialogs_disabled);
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
QString create_curved_survey(Window& window, const QString& calls = curved, const QString& capture_name = {}) {
    const auto revision = window.document().revision();
    survey_dialog(window, [&](QDialog* dialog) {
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* calculate = dialog->findChild<QPushButton*>("surveyCalculate");
        auto* result = dialog->findChild<QLabel*>("surveyResult");
        auto* add = dialog->findChild<QPushButton*>("surveyAddBoundary");
        require(input && calculate && result && add, "curved survey entry controls exist");
        dialog->findChild<QLineEdit*>("surveyProvenance")->setText("Semicircular deed");
        input->setPlainText(calls);
        calculate->click();
        // The independent fixture is a 20 x 10 rectangle plus a radius-10 upper semicircle.
        require(result->text().contains("357.0796 m²"),
                "Calculate accepts CURVE calls and reports rectangle plus semicircle area");
        require(add->isEnabled(), "closed curved calls enable Add boundary");
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(preview && !preview->entities().empty(), "curved survey has a measured preview");
        const auto& segments = preview->entities().front().segments;
        require(segments.size() == 4 && std::abs(segments[1].sweep_radians + pi) < 1e-12 &&
                    segments[0].sweep_radians == 0.0 && segments[2].sweep_radians == 0.0 &&
                    segments[3].sweep_radians == 0.0 && window.document().revision() == revision,
                "preview retains one clockwise semicircle and three lines without document mutation");
        if (!capture_name.isEmpty()) {
            QApplication::processEvents();
            const bool dark = capture_name == "dark";
            require((result->palette().color(QPalette::WindowText).lightness() > 128) == dark &&
                        (input->palette().color(QPalette::Text).lightness() > 128) == dark &&
                        (input->palette().color(QPalette::Base).lightness() < 128) == dark,
                    "survey labels and entered calls retain readable light/dark contrast");
            require(dialog->grab().save(capture_directory + "/survey-curve-" + capture_name + ".png"),
                    "curved survey dialog capture saves");
            require(preview->grab().save(capture_directory + "/survey-curve-preview-" + capture_name + ".png"),
                    "curved analytical preview capture saves");
        }
        add->click();
    });
    require(!window.selectedEntityId().isEmpty(), "new curved survey is selected");
    return window.selectedEntityId();
}
void test_curved_calculation_and_boundary() {
    Window window;
    const auto id = create_curved_survey(window);
    const auto created = entity(window, id);
    const auto segments = sketch::boundary_geometry(sketch::decode_identified_boundary_entity(created));
    require(segments.size() == 4 && std::abs(segments[1].sweep_radians + pi) < 1e-12 &&
                segments[0].sweep_radians == 0.0 && segments[2].sweep_radians == 0.0 &&
                segments[3].sweep_radians == 0.0,
            "created identified boundary retains one arc and three lines");
    const auto& report = created.extensions.at("survey_source").at("report");
    require(report.at("version") == 2 &&
                report.at("input_provenance").at("legs_text") == curved.toStdString(),
            "version-2 source retains exact entered curve parameters");
    const auto& entered = report.at("input_provenance");
    const auto& receipt = entered.at("curves").at(0);
    require(entered.at("version") == 2 && receipt.at("construction_kind") == "chord_angle" &&
                receipt.at("original_expression") == "-180" &&
                std::abs(receipt.at("sweep_radians").get<double>() + pi) < 1e-12 &&
                entered.at("distances").at(1).at("original_expression") == "20 m" &&
                entered.at("distances").at(1).at("exact_metres").at("numerator") == 20 &&
                entered.at("distances").at(1).at("exact_metres").at("denominator") == 1,
            "curve receipts retain entered signed sweep and exact chord quantity");
    const auto& diagnostics = report.at("diagnostics");
    require(std::abs(diagnostics.at("area_m2").get<double>() - (200 + 50 * pi)) < 1e-9 &&
                std::abs(diagnostics.at("perimeter_m").get<double>() - (40 + 10 * pi)) < 1e-9,
            "saved curved report uses analytical area and arc perimeter");
}
void test_curved_update_history_and_reopen() {
    Window window;
    const auto id = create_curved_survey(window);
    const auto initial = sketch::decode_identified_boundary_entity(entity(window, id));
    const auto dimension_id = window.createLengthDimension(id,
        QString::fromStdString(initial.segments[1].segment_id), {10, 25});
    require(!dimension_id.isEmpty() && window.selectEntity(id), "arc length dimension targets original survey call");
    const auto before = entity(window, id);
    const auto count = window.document().snapshot().entities().size();
    const auto revision = window.document().revision();
    survey_dialog(window, [&](QDialog* dialog) {
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* update = dialog->findChild<QPushButton*>("surveyUpdateBoundary");
        require(input->toPlainText() == curved && update->isEnabled(), "selected curve source restores editable calls");
        input->setPlainText(curved_corrected);
        require(!update->isEnabled(), "editing curved calls invalidates calculated update");
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        require(update->isEnabled() && dialog->findChild<QLabel*>("surveyResult")->text().contains("1028.3185 m²"),
                "corrected curve calculates rectangle plus radius-20 semicircle");
        update->click();
        require(window.selectedEntityId() == id && window.document().revision() == revision + 1,
                "curved update retains boundary selection and commits once");
    });
    const auto after = entity(window, id);
    const auto updated = sketch::decode_identified_boundary_entity(after);
    require(window.document().snapshot().entities().size() == count && updated.segments.size() == initial.segments.size(),
            "curved correction retains entity and edge counts");
    for (std::size_t i = 0; i < initial.segments.size(); ++i)
        require(updated.segments[i].segment_id == initial.segments[i].segment_id,
                "corresponding curved survey calls retain edge identity");
    const auto& source = after.extensions.at("survey_source");
    require(source.at("version") == 2 && source.at("report").at("input_provenance").at("legs_text") == curved_corrected.toStdString() &&
                source.at("original_report") == before.extensions.at("survey_source").at("report"),
            "corrected curve source retains original report and current entered parameters");
    const auto dimension = sketch::decode_boundary_dimension_entity(entity(window, dimension_id));
    require(dimension.supported() && std::abs(dimension.dimension->resolve(after).segment_length() - 20 * pi) < 1e-9,
            "existing arc dimension resolves corrected analytical arc length");
    require(window.undoCommand() && entity(window, id) == before && window.redoCommand() && entity(window, id) == after,
            "curved update Undo and Redo restore source and arc geometry atomically");
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("curved.bldproj")) &&
                window.openProject(directory.filePath("curved.bldproj")) && window.selectEntity(id),
            "corrected curved survey saves and reopens");
    require(entity(window, id) == after &&
                std::abs(sketch::decode_boundary_dimension_entity(entity(window, dimension_id)).dimension->resolve(entity(window, id)).segment_length() - 20 * pi) < 1e-9,
            "reopened project retains corrected curve, source, and dimension linkage");
    survey_dialog(window, [&](QDialog* dialog) {
        require(dialog->findChild<QPlainTextEdit*>("surveyLegs")->toPlainText() == curved_corrected &&
                    dialog->findChild<QPushButton*>("surveyUpdateBoundary")->isEnabled(),
                "reopened version-2 survey recalculates editable curved calls");
    });
}
void test_curved_invalid_calls_and_final_endpoint() {
    Window window;
    const auto id = create_curved_survey(window);
    const auto before = entity(window, id);
    const auto revision = window.document().revision();
    const auto count = window.document().snapshot().entities().size();
    survey_dialog(window, [&](QDialog* dialog) {
        auto* input = dialog->findChild<QPlainTextEdit*>("surveyLegs");
        auto* update = dialog->findChild<QPushButton*>("surveyUpdateBoundary");
        auto* add = dialog->findChild<QPushButton*>("surveyAddBoundary");
        for (const auto& bad : {QString("CURVE,NE,90,20 m,0"), QString("CURVE,NE,90,20 m,360"),
                QString("ARC_HEIGHT,NE,90,20 m,0 m"), QString("ARC_LENGTH,NE,90,20 m,10 m,CW"),
                QString("ARC_LENGTH,NE,90,20 m,31 m,LEFT")}) {
            input->setPlainText(bad);
            dialog->findChild<QPushButton*>("surveyCalculate")->click();
            require(!update->isEnabled() && !add->isEnabled(), "invalid curve disables boundary mutations");
            update->click();
            add->click();
            require(window.document().revision() == revision && entity(window, id) == before &&
                        window.document().snapshot().entities().size() == count,
                    "invalid curve calculations leave document unchanged");
        }
        input->setPlainText("SE,0,10 m\nSW,90,20 m\nNE,0,10 m\nCURVE,NE,90,20.0005 m,-180");
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        auto* close = dialog->findChild<QCheckBox*>("surveyCloseEndpoint");
        require(update->isEnabled() && !close->isEnabled() && !close->isChecked(),
                "within-tolerance curved final call cannot adjust its measured endpoint");
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(preview && preview->entities().size() == 2 &&
                    std::abs(preview->entities().front().segments.back().sweep_radians + pi) < 1e-12 &&
                    preview->entities().back().segments.front().sweep_radians == 0.0,
                "final curve remains analytical while a straight closing leg is previewed");
        update->click();
    });
    const auto after = entity(window, id);
    const auto segments = sketch::boundary_geometry(sketch::decode_identified_boundary_entity(after));
    require(after.extensions.at("survey_source").at("added_closing_segment") == true &&
                after.extensions.at("survey_source").at("adjusted_final_endpoint") == false &&
                segments.size() == 5 && std::abs(segments[3].sweep_radians + pi) < 1e-12 &&
                std::abs(segments[3].end.x - segments[0].start.x - 0.0005) < 1e-9 &&
                segments.back().sweep_radians == 0.0,
            "final curved call retains its measured endpoint and receives a separate straight closure");
}
void test_curve_construction_variants() {
    for (const auto& calls : {
            QString("NE,0,10 m\nARC_HEIGHT,NE,90,20 m,-10 m\nSE,0,10 m\nSW,90,20 m"),
            QString("NE,0,10 m\nARC_LENGTH,NE,90,20 m,31.41592653589793 m,CW\nSE,0,10 m\nSW,90,20 m")}) {
        Window window;
        const auto id = create_curved_survey(window, calls);
        require(entity(window, id).extensions.at("survey_source").at("report").at("input_provenance").at("legs_text") == calls.toStdString(),
                "height and arc-length constructions retain entered curve parameters");
    }
}
void test_tiny_residual_requires_explicit_straight_endpoint_adjustment() {
    Window window;
    const auto target_id = create_curved_survey(window);
    const auto before_entities = window.document().snapshot().entities();
    const auto revision = window.document().revision();
    const QString calls = "NE,0,10 m\nCURVE,NE,90,20 m,-180\nSE,0,10.00000005 m\nSW,90,20 m";
    survey_dialog(window, [&](QDialog* dialog) {
        dialog->findChild<QLineEdit*>("surveyTolerance")->setText("0.01 m");
        dialog->findChild<QPlainTextEdit*>("surveyLegs")->setPlainText(calls);
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        auto* close = dialog->findChild<QCheckBox*>("surveyCloseEndpoint");
        auto* add = dialog->findChild<QPushButton*>("surveyAddBoundary");
        auto* update = dialog->findChild<QPushButton*>("surveyUpdateBoundary");
        auto* export_report = dialog->findChild<QPushButton*>("surveyExport");
        auto* result = dialog->findChild<QLabel*>("surveyResult");
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(export_report->isEnabled() && result->text().contains("357.0796 m²"),
                "tiny straight-final residual retains calculated area and exportable measured report");
        require(close->isEnabled() && !close->isChecked() && !add->isEnabled() && !update->isEnabled(),
                "tiny residual blocks insertion until explicit supported endpoint adjustment");
        require(preview && preview->entities().size() == 1 && preview->entities().front().segments.size() == 4 &&
                    std::abs(preview->entities().front().segments.back().end.y + 0.00000005) < 1e-12,
                "uninsertable tiny residual retains the measured preview and endpoint");
        add->click();
        update->click();
        require(window.document().revision() == revision && window.document().snapshot().entities() == before_entities,
                "calculation and blocked insertion do not mutate the document");
        close->setChecked(true);
        require(add->isEnabled() && update->isEnabled() && export_report->isEnabled() &&
                    preview->entities().size() == 2,
                "explicit straight endpoint adjustment enables both boundary actions and previews the proposal");
        close->setChecked(false);
        require(!add->isEnabled() && !update->isEnabled() && export_report->isEnabled() &&
                    preview->entities().size() == 1 && result->text().contains("357.0796 m²"),
                "withdrawing closure consent blocks insertion without discarding measured calculation");
        close->setChecked(true);
        require(window.document().revision() == revision && window.document().snapshot().entities() == before_entities,
                "closure choice changes do not mutate the document");
        add->click();
    });
    const auto id = window.selectedEntityId();
    require(!id.isEmpty() && id != target_id && window.document().revision() == revision + 1 &&
                window.document().snapshot().entities().size() == before_entities.size() + 1 &&
                entity(window, target_id) == before_entities.at(target_id.toStdString()),
            "explicit tiny-residual Add creates one boundary and preserves its selected source target");
    const auto created = entity(window, id);
    const auto& source = created.extensions.at("survey_source");
    require(source.at("adjusted_final_endpoint") == true && source.at("added_closing_segment") == false &&
                source.at("report").at("input_provenance").at("legs_text") == calls.toStdString() &&
                std::abs(source.at("endpoint_adjustment_m").at("north").get<double>() - 0.00000005) < 1e-12,
            "added boundary records explicit endpoint adjustment while preserving entered measurements");
    const auto geometry = sketch::boundary_geometry(sketch::decode_identified_boundary_entity(entity(window, id)));
    require(geometry.size() == 4 && geometry.back().end.x == geometry.front().start.x &&
                geometry.back().end.y == geometry.front().start.y,
            "explicit adjustment produces a valid boundary closed exactly at its origin");
}
void test_tiny_curved_final_residual_preserves_measured_report() {
    Window window;
    create_curved_survey(window);
    const auto before_entities = window.document().snapshot().entities();
    const auto revision = window.document().revision();
    const QString calls = "SE,0,10 m\nSW,90,20 m\nNE,0,10 m\nCURVE,NE,90,20.00000005 m,-180";
    QTemporaryDir directory;
    require(directory.isValid(), "tiny curved report export directory exists");
    const auto report_path = directory.filePath("tiny-curved.json");
    const auto rebuilt = sketch::rebuild_survey_report(sketch::build_survey_report(
        {calls.toStdString(), "Tiny curved closure", "0.01 m", sketch::Unit::metre}));
    for (const auto mode : {sketch::SurveyClosureMode::retain_measured_calls, sketch::SurveyClosureMode::adjust_final_endpoint}) {
        bool rejected = false;
        try { (void)sketch::make_survey_boundary(rebuilt, mode); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "core rejects both a degenerate closing line and adjustment of a measured final curve");
    }
    survey_dialog(window, [&](QDialog* dialog) {
        dialog->findChild<QLineEdit*>("surveyTolerance")->setText("0.01 m");
        dialog->findChild<QPlainTextEdit*>("surveyLegs")->setPlainText(calls);
        dialog->findChild<QPushButton*>("surveyCalculate")->click();
        auto* close = dialog->findChild<QCheckBox*>("surveyCloseEndpoint");
        auto* add = dialog->findChild<QPushButton*>("surveyAddBoundary");
        auto* update = dialog->findChild<QPushButton*>("surveyUpdateBoundary");
        auto* result = dialog->findChild<QLabel*>("surveyResult");
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(dialog->findChild<QPushButton*>("surveyExport")->isEnabled() && result->text().contains("357.0796 m²"),
                "tiny curved-final residual remains calculable and exportable despite unavailable insertion");
        require(!close->isEnabled() && !close->isChecked() && !add->isEnabled() && !update->isEnabled() &&
                    result->text().contains("Boundary unavailable", Qt::CaseInsensitive),
                "unsupported curved-final closure gives an insertion error without offering endpoint adjustment");
        require(preview && preview->entities().size() == 1 && preview->entities().front().segments.size() == 4 &&
                    std::abs(preview->entities().front().segments.back().sweep_radians + pi) < 1e-12 &&
                    std::abs(preview->entities().front().segments.back().end.x - 0.00000005) < 1e-12,
                "uninsertable curved-final residual retains the analytical measured curve and endpoint");
        choose_report_file(dialog, "surveyExport", report_path);
        QFile saved(report_path);
        require(saved.open(QIODevice::ReadOnly), "uninsertable curved report is written through Export report");
        const auto exported = nlohmann::json::parse(saved.readAll().toStdString());
        require(exported.at("version") == 2 && exported.at("input_provenance").at("legs_text") == calls.toStdString() &&
                    std::abs(exported.at("diagnostics").at("area_m2").get<double>() - (200 + 50 * pi)) < 1e-5 &&
                    exported.at("input_provenance").at("curves").at(0).at("original_expression") == "-180",
                "exported tiny curved report preserves measured source, analytical area, and curve receipt");
        saved.close();
        dialog->findChild<QPlainTextEdit*>("surveyLegs")->setPlainText("NE,91,10 m");
        choose_report_file(dialog, "surveyOpen", report_path);
        require(dialog->findChild<QPlainTextEdit*>("surveyLegs")->toPlainText() == calls &&
                    dialog->findChild<QPushButton*>("surveyExport")->isEnabled() &&
                    result->text().contains("357.0796 m²") && !add->isEnabled() && !update->isEnabled() &&
                    preview->entities().size() == 1,
                "standalone tiny curved report reopens as measured output with insertion still blocked");
        add->click();
        update->click();
        require(window.document().revision() == revision && window.document().snapshot().entities() == before_entities,
                "unsupported curved-final closure does not mutate any document entity");
    });
}
void test_curved_standalone_report_and_vector_output() {
    QTemporaryDir directory;
    require(directory.isValid(), "curved standalone output directory exists");
    const auto report_path = directory.filePath("curved.json");
    Window window;
    const auto id = create_curved_survey(window);
    const auto before = entity(window, id);
    const auto revision = window.document().revision();
    survey_dialog(window, [&](QDialog* dialog) {
        choose_report_file(dialog, "surveyExport", report_path);
    });
    QFile saved(report_path);
    require(saved.open(QIODevice::ReadOnly), "curved standalone report is written by the calculator");
    const auto exported = nlohmann::json::parse(saved.readAll().toStdString());
    require(exported.at("version") == 2 && exported.at("input_provenance").at("legs_text") == curved.toStdString() &&
                exported.at("input_provenance").at("curves").at(0).at("construction_kind") == "chord_angle" &&
                std::abs(exported.at("diagnostics").at("area_m2").get<double>() - (200 + 50 * pi)) < 1e-9,
            "actual standalone export retains version-2 curve inputs and analytical area");
    saved.close();
    Window reopened;
    const auto reopened_revision = reopened.document().revision();
    const auto reopened_entities = reopened.document().snapshot().entities();
    survey_dialog(reopened, [&](QDialog* dialog) {
        choose_report_file(dialog, "surveyOpen", report_path);
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(dialog->findChild<QWidget*>("surveyPreview"));
        require(dialog->findChild<QPlainTextEdit*>("surveyLegs")->toPlainText() == curved &&
                    dialog->findChild<QLabel*>("surveyResult")->text().contains("357.0796 m²") &&
                    dialog->findChild<QPushButton*>("surveyExport")->isEnabled() &&
                    dialog->findChild<QPushButton*>("surveyAddBoundary")->isEnabled() &&
                    preview && preview->entities().size() == 1 &&
                    std::abs(preview->entities().front().segments.at(1).sweep_radians + pi) < 1e-12 &&
                    reopened.document().revision() == reopened_revision && reopened.document().snapshot().entities() == reopened_entities,
                "standalone curve report restores analytical preview and insertion eligibility without mutation");
    });
    const auto svg_path = directory.filePath("curved.svg");
    require(window.exportDraftSvg(svg_path), "created curved boundary exports through the shared SVG renderer");
    QFile svg(svg_path);
    require(svg.open(QIODevice::ReadOnly), "curved SVG output is readable");
    const auto svg_text = QString::fromUtf8(svg.readAll());
    require(svg_text.contains("<svg") &&
                QRegularExpression(QStringLiteral("<path\\b[^>]*\\bd=\"[^\"]*C")).match(svg_text).hasMatch() &&
                window.document().revision() == revision && entity(window, id) == before,
            "SVG contains a vector curve and exporting leaves survey source and geometry unchanged");
}
void capture_curved_themes() {
    if (capture_directory.isEmpty()) return;
    require(QDir().mkpath(capture_directory), "curved survey capture directory is available");
    for (const auto dark : {false, true}) {
        Window window;
        window.resize(1280, 900);
        window.setWorkspaceTheme(dark ? sketch::WorkspaceTheme::dark : sketch::WorkspaceTheme::light);
        window.show();
        const auto name = dark ? QString("dark") : QString("light");
        create_curved_survey(window, curved, name);
        QApplication::processEvents();
        auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
        require(canvas && canvas->grab().save(capture_directory + "/survey-curve-canvas-" + name + ".png"),
                "created curved boundary canvas capture saves");
    }
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
    for (const auto& argument : app.arguments())
        if (argument.startsWith("--capture-directory=")) capture_directory = argument.mid(20);
    if (!app.arguments().contains("--curves-only")) {
        test_update_and_history();
        test_invalid_stale_and_closure();
    }
    test_curved_calculation_and_boundary();
    test_curved_update_history_and_reopen();
    test_curved_invalid_calls_and_final_endpoint();
    test_curve_construction_variants();
    test_tiny_residual_requires_explicit_straight_endpoint_adjustment();
    test_tiny_curved_final_residual_preserves_measured_report();
    test_curved_standalone_report_and_vector_output();
    capture_curved_themes();
    std::cout << "Survey update desktop tests passed\n";
}
