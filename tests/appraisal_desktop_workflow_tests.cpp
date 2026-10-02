#include "sketch/desktop/main_window.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/project_store.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAction>
#include <QAbstractButton>
#include <QComboBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QPdfDocument>
#include <QTimer>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QLayout>
#include <QSpinBox>
#include <QTableWidget>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QDoubleSpinBox>
#include <QCheckBox>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

void auto_subtract_selected_area_workflow() {
    using sketch::desktop::MainWindow;
    MainWindow window;
    const auto parent=window.createBoundary(square(0,0,3.048));
    auto* workflow=window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedClassification(QStringLiteral("above_grade_finished")),"classify Auto-Subtract parent");
    const auto garage=window.createBoundary(square(0.5,0.5,1.524),QStringLiteral("garage"));
    require(!parent.isEmpty() && !garage.isEmpty(),"create Auto-Subtract geometry");
    auto* action=window.findChild<QAction*>(QStringLiteral("autoSubtract"));
    require(action && action->isEnabled(),"selected subtractor must expose its Auto-Subtract action");
    const auto before=window.document().snapshot();
    bool accepted=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("autoSubtractDialog"));
        require(dialog,"Auto-Subtract must open a parent chooser");
        auto* target=dialog->findChild<QComboBox*>(QStringLiteral("autoSubtractTarget"));
        auto* buttons=dialog->findChild<QDialogButtonBox*>(QStringLiteral("autoSubtractButtons"));
        require(target && buttons && target->findData(parent)>=0,"specific parent must be offered");
        target->setCurrentIndex(target->findData(parent));
        buttons->button(QDialogButtonBox::Apply)->click();
        accepted=dialog->result()==QDialog::Accepted;
        if (!accepted) dialog->reject();
    });
    action->trigger();
    const auto after=window.document().snapshot();
    require(accepted && after.revision()==before.revision()+1 &&
        after.entities().at(parent.toStdString()).properties.at("deduction_ids")==std::vector<std::string>{garage.toStdString()} &&
        after.entities().at(garage.toStdString())==before.entities().at(garage.toStdString()),
        "selected garage must subtract from its chosen parent in one command without source changes");
    require(window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"))->text().contains("75.00"),
        "Auto-Subtract must update the 100-square-foot parent to 75 square feet GLA");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),"Auto-Subtract must undo and redo atomically");
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("auto-subtract.bldproj")) &&
        window.openProject(directory.filePath("auto-subtract.bldproj")) &&
        window.document().snapshot().entities()==after.entities(),"Auto-Subtract must survive save/reopen");
    require(window.selectEntity(garage),"reselect subtractor after reopening");
    const auto unchanged=window.document().snapshot();
    require(window.applySelectedAutoSubtract(parent) && window.document().revision()==unchanged.revision() &&
        window.document().snapshot().entities()==unchanged.entities(),"adding an existing link must be idempotent");
    require(!window.applySelectedAutoSubtract(parent,false,unchanged.revision()-1) &&
        window.document().snapshot().entities()==unchanged.entities(),"stale subtraction must refuse atomically");
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("autoSubtractDialog"); require(dialog,"cancel must open target selector");
        dialog->reject();
    });
    action->trigger();
    require(window.document().revision()==unchanged.revision() && window.document().snapshot().entities()==unchanged.entities(),
        "cancelled target selector must not change links");
    require(window.editSelectedClassification("above_grade_finished"),"type-change repair fixture must edit source");
    const auto invalid_link=window.document().snapshot();
    require(!window.applySelectedAutoSubtract(parent) && window.document().snapshot().entities()==invalid_link.entities(),
        "same-type add must refuse without discarding the existing link");
    bool removed=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("autoSubtractDialog"); require(dialog,"repair removal must open selector");
        auto* target=dialog->findChild<QComboBox*>("autoSubtractTarget");
        require(target && target->findData(parent)>=0,"invalid existing target must remain listed for removal");
        target->setCurrentIndex(target->findData(parent));
        auto* remove=dialog->findChild<QPushButton*>("removeAutoSubtract"); require(remove && remove->isEnabled(),"existing invalid link must expose removal");
        remove->click(); removed=dialog->result()==QDialog::Accepted; if (!removed) dialog->reject();
    });
    action->trigger();
    require(removed,"existing invalid link must remain removable after source type changes");
    require(window.editSelectedClassification("garage") && window.applySelectedAutoSubtract(parent),"repaired link must be addable again");
    const auto linked=window.document().snapshot();
    require(window.deleteSelection() && !window.document().snapshot().entities().contains(garage.toStdString()) &&
        window.document().snapshot().entities().at(parent.toStdString()).properties.value("deduction_ids",std::vector<std::string>{}).empty() &&
        window.document().revision()==linked.revision()+1,"deleting source must clean parent links in the same command");
    require(window.undoCommand() && window.document().snapshot().entities()==linked.entities(),"delete undo must restore source and links exactly");
    const auto same=window.createBoundary(square(0.7,0.7,0.3),"above_grade_finished");
    const auto same_before=window.document().snapshot();
    require(!same.isEmpty() && !window.applySelectedAutoSubtract(parent) &&
        window.document().revision()==same_before.revision() && window.document().snapshot().entities()==same_before.entities(),
        "different owner with same area type must not subtract");
    const auto outside=window.createBoundary(square(10,10,1),"garage");
    const auto outside_before=window.document().snapshot();
    require(!outside.isEmpty() && !window.applySelectedAutoSubtract(parent) &&
        window.document().revision()==outside_before.revision() && window.document().snapshot().entities()==outside_before.entities(),
        "outside subtraction must refuse without a partial link");
}

void auto_subtract_context_repair_workflow() {
    sketch::desktop::MainWindow window;
    const auto parent = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow, "context repair fixture needs the appraisal workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "context repair parent must have an appraisal type");
    const auto unrelated = window.createBoundary(square(10, 0, 3.048), QStringLiteral("above_grade_finished"));
    const auto source = window.createBoundary(square(0.5, 0.5, 1.524), QStringLiteral("garage"));
    require(!parent.isEmpty() && !unrelated.isEmpty() && !source.isEmpty() &&
                window.applySelectedAutoSubtract(parent),
            "context repair fixture must begin with an explicit parent link");

    const auto original = window.document().snapshot();
    const auto& source_entity = original.entities().at(source.toStdString());
    const auto old_floor = source_entity.properties.at("floor_id").get<std::string>();
    const auto building = original.entities().at(old_floor).properties.at("building_id").get<std::string>();
    const auto new_floor = window.createFloor(QString::fromStdString(building), QStringLiteral("Repair floor"));
    const auto new_layer = window.activeLayerId();
    require(!new_floor.isEmpty() && new_floor.toStdString() != old_floor && !new_layer.isEmpty(),
            "context repair fixture needs another valid floor and layer");
    const auto organized = window.document().snapshot();
    require(organized.entities().at(new_layer.toStdString()).properties.at("floor_id") == new_floor.toStdString(),
            "context repair destination layer must belong to its new floor");
    auto moved_source = organized.entities().at(source.toStdString());
    moved_source.properties["floor_id"] = new_floor.toStdString();
    moved_source.properties["layer_id"] = new_layer.toStdString();
    window.document().apply(sketch::ApplyEntityChanges{
        organized.revision(), {sketch::EntityChange::upsert(moved_source)}, {},
        "Move subtractor to another floor for native link repair"});
    require(window.selectEntity(source), "moved source must remain selectable for link repair");
    const auto before_remove = window.document().snapshot();
    require(before_remove.entities().at(parent.toStdString()).properties.at("deduction_ids") ==
                std::vector<std::string>{source.toStdString()} &&
                before_remove.entities().at(parent.toStdString()).properties.at("floor_id") !=
                    moved_source.properties.at("floor_id"),
            "context repair must retain an explicit link whose floors no longer match");
    require(!window.applySelectedAutoSubtract(parent) &&
                window.document().revision() == before_remove.revision() &&
                window.document().snapshot().entities() == before_remove.entities(),
            "context mismatch must refuse a new add without discarding the existing link");

    auto* action = window.findChild<QAction*>(QStringLiteral("autoSubtract"));
    require(action && action->isEnabled(), "moved subtractor must expose its native repair action");
    bool removed = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("autoSubtractDialog"));
        require(dialog, "context repair must open the native parent selector");
        auto* target = dialog->findChild<QComboBox*>(QStringLiteral("autoSubtractTarget"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("autoSubtractButtons"));
        auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("removeAutoSubtract"));
        require(target && buttons && remove && target->findData(parent) >= 0,
                "linked parent must remain listed after the source moves to another floor");
        target->setCurrentIndex(target->findData(parent));
        require(remove->isEnabled() && !buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                "cross-floor linked parent must offer Remove while disabling Apply");
        remove->click();
        removed = dialog->result() == QDialog::Accepted;
        if (!removed) dialog->reject();
    });
    action->trigger();
    const auto after_remove = window.document().snapshot();
    require(removed && after_remove.revision() == before_remove.revision() + 1 &&
                after_remove.entities().at(parent.toStdString()).properties
                    .value("deduction_ids", std::vector<std::string>{}).empty(),
            "cross-floor repair must remove the explicit link in one command");
    for (const auto& [id, entity] : before_remove.entities()) {
        if (id != parent.toStdString()) {
            require(after_remove.entities().at(id) == entity,
                    "link repair must preserve the source, organization and unrelated parents");
        }
    }
    auto expected_parent = before_remove.entities().at(parent.toStdString());
    auto repaired_parent = after_remove.entities().at(parent.toStdString());
    expected_parent.properties.erase("deduction_ids");
    repaired_parent.properties.erase("deduction_ids");
    require(repaired_parent == expected_parent && window.undoCommand() &&
                window.document().snapshot().entities() == before_remove.entities(),
            "cross-floor repair must change only the link and undo it exactly");
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

bool set_appraisal_fact(QDialog& dialog, const char* key, const char* value) {
    auto* box = dialog.findChild<QComboBox*>(QString::fromLatin1(key));
    const auto index = box ? box->findData(QString::fromLatin1(value)) : -1;
    if (!box || index < 0) return false;
    box->setCurrentIndex(index);
    return true;
}

QComboBox* inspector_classification_control(QWidget& owner) {
    for (auto* form : owner.findChildren<QFormLayout*>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            const auto* label_item = form->itemAt(row, QFormLayout::LabelRole);
            const auto* field_item = form->itemAt(row, QFormLayout::FieldRole);
            const auto* label = label_item ? qobject_cast<QLabel*>(label_item->widget()) : nullptr;
            auto* combo = field_item ? qobject_cast<QComboBox*>(field_item->widget()) : nullptr;
            if (label && label->text() == QStringLiteral("Classification") && combo) return combo;
        }
    }
    return nullptr;
}

void edit_appraisal_facts_through_controls(sketch::desktop::MainWindow& window,
                                           const QString& boundary_id,
                                           const char* area_use,
                                           const char* finish,
                                           const std::function<void()>& open_dialog,
                                           bool save = true) {
    require(window.selectEntity(boundary_id), "select boundary before editing declared appraisal facts");
    bool completed = false;
    bool timed_out = false;
    QString callback_error;
    QTimer responder;
    responder.setSingleShot(true);
    QObject::connect(&responder, &QTimer::timeout, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("appraisalFactsDialog"));
        if (!dialog) return;
        timed_out = true;
        dialog->reject();
    });
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("appraisalFactsDialog"));
        if (!dialog) {
            callback_error = QStringLiteral("declared-facts editing did not open its native dialog");
            return;
        }
        const bool controls_ready =
            set_appraisal_fact(*dialog, "policy_kind", "residential_declared") &&
            set_appraisal_fact(*dialog, "property_kind", "detached_single_family") &&
            set_appraisal_fact(*dialog, "measurement_basis", "exterior") &&
            set_appraisal_fact(*dialog, "grade", "above") &&
            set_appraisal_fact(*dialog, "finish", finish) &&
            set_appraisal_fact(*dialog, "access", "direct_interior") &&
            set_appraisal_fact(*dialog, "ceiling_eligibility", "standard") &&
            set_appraisal_fact(*dialog, "area_use", area_use) &&
            set_appraisal_fact(*dialog, "boundary_role", "measured_area");
        if (!controls_ready) {
            callback_error = QStringLiteral("declared-facts editor is missing a required control or token");
            dialog->reject();
            return;
        }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!buttons || !buttons->button(QDialogButtonBox::Save)) {
            callback_error = QStringLiteral("declared-facts dialog is missing its Save control");
            dialog->reject();
            return;
        }
        if (save) {
            buttons->button(QDialogButtonBox::Save)->click();
            completed = dialog->result() == QDialog::Accepted;
            if (!completed) {
                callback_error = QStringLiteral("native declared-facts Save did not accept the dialog");
                dialog->reject();
            }
        } else {
            if (!buttons->button(QDialogButtonBox::Cancel)) {
                callback_error = QStringLiteral("declared-facts dialog is missing its Cancel control");
                dialog->reject();
                return;
            }
            buttons->button(QDialogButtonBox::Cancel)->click();
            completed = dialog->result() == QDialog::Rejected;
        }
    });
    responder.start(5000);
    open_dialog();
    responder.stop();
    require(completed && !timed_out && callback_error.isEmpty(),
            "native declared-facts controls must complete the requested dialog action: " +
                callback_error.toStdString());
}

void save_appraisal_facts_through_controls(sketch::desktop::MainWindow& window,
                                           const QString& boundary_id,
                                           const char* area_use,
                                           const char* finish = "finished") {
    edit_appraisal_facts_through_controls(window, boundary_id, area_use, finish,
        [&] { window.showAppraisalFacts(); });
}

void auto_subtract_uses_current_declared_facts() {
    using sketch::desktop::MainWindow;
    MainWindow window;
    window.setMetricUnits(false);
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow, "fact-driven subtraction fixture needs the appraisal workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));

    // Keep the legacy/manual categories identical while the declared facts say
    // that the inner area is a garage. The chooser and calculation must follow
    // the facts edited through the native controls, not these stale categories.
    const auto parent = window.createBoundary(square(0, 0, 3.048));
    require(!parent.isEmpty() && window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "create a manually categorized ten-foot appraisal parent");
    const auto garage = window.createBoundary(square(0.762, 0.762, 1.524));
    require(!garage.isEmpty() && window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "create a manually categorized five-foot appraisal garage");
    save_appraisal_facts_through_controls(window, parent, "dwelling");
    save_appraisal_facts_through_controls(window, garage, "garage");

    const auto before = window.document().snapshot();
    const auto& parent_before = before.entities().at(parent.toStdString());
    const auto& garage_before = before.entities().at(garage.toStdString());
    require(parent_before.properties.at("appraisal_category") == "above_grade_finished" &&
            garage_before.properties.at("appraisal_category") == "above_grade_finished" &&
            parent_before.properties.at("appraisal_facts").at("area_use") == "dwelling" &&
            garage_before.properties.at("appraisal_facts").at("area_use") == "garage",
            "fixture must preserve identical stale categories beside distinct declared uses");

    auto* action = window.findChild<QAction*>(QStringLiteral("autoSubtract"));
    require(action && action->isEnabled(), "declared garage must expose Auto-Subtract");
    bool accepted = false;
    bool chooser_opened = false;
    bool parent_present = false;
    QString chooser_error;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("autoSubtractDialog"));
        if (!dialog) {
            chooser_error = QStringLiteral("fact-driven subtraction did not open the native parent chooser");
            return;
        }
        chooser_opened = true;
        auto* target = dialog->findChild<QComboBox*>(QStringLiteral("autoSubtractTarget"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("autoSubtractButtons"));
        if (!target || !buttons || !buttons->button(QDialogButtonBox::Apply)) {
            chooser_error = QStringLiteral("parent chooser is missing its target or Apply controls");
            dialog->reject();
            return;
        }
        const auto parent_index = target->findData(parent);
        parent_present = parent_index >= 0;
        if (!parent_present) {
            chooser_error = QStringLiteral("declared garage facts did not offer the dwelling parent");
            dialog->reject();
            return;
        }
        target->setCurrentIndex(parent_index);
        buttons->button(QDialogButtonBox::Apply)->click();
        accepted = dialog->result() == QDialog::Accepted;
        if (!accepted) dialog->reject();
    });
    action->trigger();
    require(chooser_opened && parent_present,
            "declared garage facts must offer the dwelling parent even when saved manual categories match: " +
                chooser_error.toStdString());
    require(accepted, "native Auto-Subtract chooser must apply the selected parent");
    const auto after = window.document().snapshot();
    auto expected_parent = parent_before;
    expected_parent.properties["deduction_ids"] = std::vector<std::string>{garage.toStdString()};
    require(after.revision() == before.revision() + 1 &&
            after.entities().at(parent.toStdString()) == expected_parent &&
            after.entities().at(garage.toStdString()) == garage_before,
            "subtracting must add only the parent link and preserve both source geometries and facts");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    auto* garage_total = window.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"));
    require(qualification && qualification->text().startsWith(QStringLiteral("Qualified")) &&
            gla && gla->text().contains(QStringLiteral("75.00")) &&
            garage_total && garage_total->text().contains(QStringLiteral("25.00")),
            "current declared uses must yield 75 square feet of GLA and 25 square feet of garage");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
            window.redoCommand() && window.document().snapshot().entities() == after.entities(),
            "fact-driven Auto-Subtract must undo and redo its complete linked state");

    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("fact-driven-auto-subtract.bldproj"));
    require(directory.isValid() && window.saveProjectAs(path),
            "fact-driven Auto-Subtract project must save");
    MainWindow reopened;
    require(reopened.openProject(path) && reopened.selectEntity(parent),
            "fact-driven Auto-Subtract project must reopen");
    const auto persisted = reopened.document().snapshot();
    require(persisted.entities().at(parent.toStdString()).properties.at("deduction_ids") ==
                std::vector<std::string>{garage.toStdString()} &&
            persisted.entities().at(parent.toStdString()).properties.at("appraisal_facts").at("area_use") == "dwelling" &&
            persisted.entities().at(garage.toStdString()).properties.at("appraisal_facts").at("area_use") == "garage" &&
            reopened.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"))->text().contains(QStringLiteral("75.00")) &&
            reopened.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"))->text().contains(QStringLiteral("25.00")),
            "reopen must retain the relationship and both declared facts in the automatic totals");
}

void auto_subtract_rejects_equal_declared_facts_despite_stale_categories() {
    using sketch::desktop::MainWindow;
    MainWindow window;
    window.setMetricUnits(false);
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow, "same-facts subtraction fixture needs the appraisal workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));

    const auto parent = window.createBoundary(square(0, 0, 3.048));
    require(!parent.isEmpty() && window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "create a manually dwelling-classified parent");
    const auto child = window.createBoundary(square(0.762, 0.762, 1.524));
    require(!child.isEmpty() && window.editSelectedClassification(QStringLiteral("garage")),
            "create an inner area with a deliberately different stale manual category");
    save_appraisal_facts_through_controls(window, parent, "dwelling");
    save_appraisal_facts_through_controls(window, child, "dwelling");

    const auto before = window.document().snapshot();
    const auto& parent_entity = before.entities().at(parent.toStdString());
    const auto& child_entity = before.entities().at(child.toStdString());
    require(parent_entity.properties.at("appraisal_category") == "above_grade_finished" &&
            child_entity.properties.at("appraisal_category") == "garage" &&
            parent_entity.properties.at("appraisal_facts").at("area_use") == "dwelling" &&
            child_entity.properties.at("appraisal_facts").at("area_use") == "dwelling",
            "fixture must keep different stale manual categories beside equal declared uses");
    require(window.selectEntity(child), "select equal-facts source area");
    auto* action = window.findChild<QAction*>(QStringLiteral("autoSubtract"));
    require(action, "equal-facts fixture must expose the Auto-Subtract action");
    if (action->isEnabled()) {
        bool chooser_opened = false;
        bool parent_excluded = false;
        QString chooser_error;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("autoSubtractDialog"));
            if (!dialog) {
                chooser_error = QStringLiteral("enabled Auto-Subtract did not open its parent chooser");
                return;
            }
            chooser_opened = true;
            auto* target = dialog->findChild<QComboBox*>(QStringLiteral("autoSubtractTarget"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("autoSubtractButtons"));
            if (!target || !buttons) {
                chooser_error = QStringLiteral("parent chooser is missing its target or button controls");
                dialog->reject();
                return;
            }
            parent_excluded = target->findData(parent) < 0;
            if (!parent_excluded)
                chooser_error = QStringLiteral("equal declared uses still offered the manually different parent");
            dialog->reject();
        });
        action->trigger();
        require(chooser_opened && parent_excluded,
                "equal declared uses must keep a manually different parent out of the chooser: " +
                    chooser_error.toStdString());
    }
    require(!window.applySelectedAutoSubtract(parent) &&
            window.document().revision() == before.revision() &&
            window.document().snapshot().entities() == before.entities(),
            "equal declared uses must reject the stale manual-category relationship atomically");
}

void auto_subtract_define_first_keyboard_and_recovery() {
    using sketch::desktop::MainWindow;
    using sketch::desktop::PlanCanvas;
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen,true); window.resize(1200,800); window.show();
    QApplication::setActiveWindow(&window); QCoreApplication::processEvents();
    window.setMetricUnits(false);
    const auto parent=window.createBoundary(square(0,0,3.048));
    auto* workflow=window.findChild<QComboBox*>("calculationWorkflow");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()),"declare qualified Auto-Subtract parent");
    auto* drawing=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    auto* action=window.findChild<QAction*>("drawSubtractingArea");
    require(drawing && action && action->isEnabled(),"Define First subtraction needs actual native entry");
    const auto source=window.document().snapshot();
    bool classification_seen=false,target_seen=false,expired=false;
    QTimer poll; poll.setInterval(1);
    QObject::connect(&poll,&QTimer::timeout,&window,[&] {
        auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget()); if (!modal) return;
        if (modal->objectName()=="boundaryClassificationDialog" && !classification_seen) {
            classification_seen=true;
            auto* classification=qobject_cast<QInputDialog*>(modal); require(classification,"classification must be native input dialog");
            classification->setTextValue("Open to below"); classification->accept();
        } else if (modal->objectName()=="autoSubtractDialog" && !target_seen) {
            target_seen=true;
            auto* target=modal->findChild<QComboBox*>("autoSubtractTarget");
            require(target && target->findData(parent)>=0,"pre-draw selector must offer qualified parent by stable ID");
            target->setCurrentIndex(target->findData(parent));
            const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
            if (!captures.isEmpty()) {
                if (modal->layout()) modal->layout()->activate();
                modal->adjustSize();
                QApplication::processEvents();
                require(QDir().mkpath(captures) && modal->grab().save(QDir(captures).filePath("auto-subtract-define-target.png")),
                    "target selector capture must save");
            }
            modal->findChild<QDialogButtonBox*>("autoSubtractButtons")->button(QDialogButtonBox::Apply)->click();
        }
    });
    QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&window,[&] {
        expired=true; if (auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
    });
    poll.start(); deadline.start(5000); action->trigger(); poll.stop(); deadline.stop();
    require(classification_seen && target_seen && !expired && drawing->boundaryDraftPreview(),"native Define First subtraction must start an unfinished draft");
    require(window.document().snapshot().entities()==source.entities() && window.document().revision()==source.revision(),
        "choosing subtractor type and parent must not publish model changes");
    const auto key=[&](int code) {
        QKeyEvent event(QEvent::KeyPress,code,Qt::NoModifier); QApplication::sendEvent(drawing,&event);
        QCoreApplication::processEvents(QEventLoop::AllEvents,100);
    };
    const auto precision=[&](const std::function<void(sketch::desktop::BoundaryInputDialog&)>& fill) {
        bool seen=false,timed_out=false;
        QTimer responder; responder.setInterval(1);
        QObject::connect(&responder,&QTimer::timeout,&window,[&] {
            auto* input=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(QApplication::activeModalWidget());
            if (!input || seen) return; seen=true; fill(*input);
            QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier); QApplication::sendEvent(input,&enter);
            require(input->result()==QDialog::Accepted,"valid subtractor precision form must accept Enter");
        });
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout,&QTimer::timeout,&window,[&] {
            timed_out=true; if (auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
        });
        responder.start(); timeout.start(5000); key(Qt::Key_D); responder.stop(); timeout.stop();
        require(seen && !timed_out,"D must support all subtraction drawing phases");
        require(window.focusWidget()==drawing,"precision form must restore stored canvas focus");
        // Offscreen has no window manager activation; never set canvas focus
        // for the product while simulating the parent's return to activation.
        QApplication::setActiveWindow(&window); QCoreApplication::processEvents();
    };
    const auto position=[&](const char* x,const char* y) {
        precision([&](sketch::desktop::BoundaryInputDialog& input) {
            input.findChild<QLineEdit*>("boundaryInputEndX")->setText(x);
            input.findChild<QLineEdit*>("boundaryInputEndY")->setText(y);
        });
    };
    const auto edge=[&](const char* heading) {
        precision([&](sketch::desktop::BoundaryInputDialog& input) {
            input.findChild<QLineEdit*>("boundaryInputLength")->setText("5 ft");
            input.findChild<QLineEdit*>("boundaryInputHeading")->setText(heading);
        });
    };
    position("2 ft","2 ft"); edge("0 deg"); position("4.5 ft","1 ft");
    QTemporaryDir directory; require(directory.isValid(),"subtraction recovery fixture needs directory");
    const auto path=directory.filePath("unfinished-subtraction.bldproj");
    require(window.saveProjectAs(path),"save unfinished subtracting area");
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!captures.isEmpty()) require(QFile::copy(path,QDir(captures).filePath("unfinished-subtraction.bldproj")),
        "retain unfinished subtraction fixture for previous-reader verification");
    const auto archive=sketch::ProjectStore::load_archive(std::filesystem::path(path.toStdWString()),sketch::ArchiveRole::ordinary);
    require(archive.supported() && archive.recovery.decoded && archive.recovery.decoded->active &&
        archive.recovery.decoded->active->auto_subtract_target_id==parent.toStdString(),"saved unfinished draft must retain explicit parent identity");
    require(window.createNewProject() && window.openProject(path) && drawing->boundaryDraftPreview() &&
        drawing->boundaryDraftPreview()->segments.size()==1,"reopen unfinished subtraction through ordinary workflow");
    const auto before_finish=window.document().snapshot();
    edge("90 deg"); position("8 ft","4.5 ft"); edge("180 deg"); position("4.5 ft","8 ft");
    key(Qt::Key_Return); position("1 ft","4.5 ft"); key(Qt::Key_Return);
    const auto after=window.document().snapshot();
    const auto subtractor=window.selectedEntityId();
    require(!subtractor.isEmpty() && subtractor!=parent && after.revision()==before_finish.revision()+1 &&
        after.entities().size()==before_finish.entities().size()+5 &&
        after.entities().at(parent.toStdString()).properties.at("deduction_ids")==std::vector<std::string>{subtractor.toStdString()},
        "source, manual dimensions and retained parent link must commit together once");
    const auto& child=after.entities().at(subtractor.toStdString());
    std::size_t manual_dimensions=0;
    for (const auto& [id,entity] : after.entities()) {
        (void)id;
        if (!sketch::can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto dimension=sketch::decode_boundary_dimension_entity(entity);
        if (dimension.dimension && dimension.dimension->boundary_id==subtractor.toStdString() &&
            dimension.dimension->placement==sketch::BoundaryDimensionPlacement::manual) ++manual_dimensions;
    }
    require(manual_dimensions==4,"Define First subtraction must persist all four manual dimensions");
    require(child.properties.at("classification")=="measurement" &&
        child.properties.at("appraisal_facts")==nlohmann::json{{"boundary_role","open_to_below"}} &&
        !child.properties.contains("appraisal_category"),"void drawing must record only its declared role, never fabricated qualifying facts");
    require(window.findChild<QLabel*>("appraisalQualification")->text().startsWith("Qualified") &&
        window.findChild<QLabel*>("appraisalGlaTotal")->text().contains("75.00"),"qualified parent net must decrease by the linked 25 square foot void");
    require(window.undoCommand() && window.document().snapshot().entities()==before_finish.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),"pre-draw subtraction must undo and redo complete linked state");
    require(window.saveProject() && window.openProject(path) && window.document().snapshot().entities()==after.entities(),
        "completed subtraction must save/reopen exact source, dimensions and parent link");
    if (!captures.isEmpty()) require(QFile::copy(path,QDir(captures).filePath("completed-subtraction.bldproj")),
        "retain completed subtraction fixture for previous-reader verification");
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
    require(qualification->text().startsWith("Qualified") && derived->text().contains("Above-grade finished (GLA)") && gla->text().contains("100.00"),
            "facts must qualify and derive GLA without manual category assignment");
    const auto appraisal_schedule = window.scheduleSnapshot();
    const auto schedule_gla = std::find_if(
        appraisal_schedule.snapshot.rows.begin(), appraisal_schedule.snapshot.rows.end(),
        [](const auto& row) {
            return row.kind == sketch::ScheduleRowKind::appraisal &&
                   row.object_id.ends_with(":category:above_grade_finished");
        });
    require(schedule_gla != appraisal_schedule.snapshot.rows.end() &&
                std::holds_alternative<sketch::ScheduleQuantity>(
                    schedule_gla->cells.at("area").value) &&
                std::abs(std::get<sketch::ScheduleQuantity>(
                    schedule_gla->cells.at("area").value).value - 9.290304) < 1e-8 &&
                schedule_gla->cells.at("area").sources.front().object_id == area.toStdString(),
            "the printable appraisal schedule must use automatic geometry totals with source provenance");
    require(window.setContainerVisible(QStringLiteral("floor-1"), false),
            "appraisal fixture must support hiding the floor presentation");
    const auto filtered_schedule = window.scheduleSnapshot();
    require(std::any_of(filtered_schedule.snapshot.rows.begin(),
                        filtered_schedule.snapshot.rows.end(), [](const auto& row) {
                return row.kind == sketch::ScheduleRowKind::appraisal &&
                       row.object_id.ends_with(":category:above_grade_finished") &&
                       row.cells.contains("area");
            }),
            "presentation visibility must not change printable appraisal totals");
    require(window.setContainerVisible(QStringLiteral("floor-1"), true),
            "appraisal fixture must restore the floor presentation");
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
    const auto unqualified_schedule = window.scheduleSnapshot();
    require(std::any_of(unqualified_schedule.snapshot.rows.begin(),
                        unqualified_schedule.snapshot.rows.end(), [](const auto& row) {
                if (row.kind != sketch::ScheduleRowKind::appraisal ||
                    !row.object_id.ends_with(":status")) return false;
                const auto status = row.cells.find("status");
                return status != row.cells.end() &&
                       std::holds_alternative<std::string>(status->second.value) &&
                       std::get<std::string>(status->second.value).find("totals withheld") !=
                           std::string::npos;
            }) &&
                std::none_of(unqualified_schedule.snapshot.rows.begin(),
                             unqualified_schedule.snapshot.rows.end(), [](const auto& row) {
                    return row.kind == sketch::ScheduleRowKind::appraisal &&
                           row.cells.contains("area");
                }),
            "unqualified sheet output must state that totals are withheld and print no area values");
    require(window.editSelectedAppraisalFacts(declarations("residential_declared", "dwelling", "below")) &&
            derived->text().contains("Below-grade finished") && gla->text().contains("0.00"),
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
            reopened.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"))->text().contains("Below-grade finished"),
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
    require(window.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"))->text().contains("Above-grade finished (GLA)"),
            "dialog must submit declarations through the same command");
}

void declared_appraisal_deductions_edit_without_manual_categories() {
    sketch::desktop::MainWindow window;
    const auto outer = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "declare deduction parent");
    const auto hole = window.createBoundary(square(1, 1, 1));
    require(window.editSelectedAppraisalFacts(declarations("residential_declared", "dwelling", "above", "open_to_below")),
            "declare physical void without assigning a manual category");
    require(window.selectEntity(outer), "select deduction parent");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* total = window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"));
    auto* editor = window.findChild<QPushButton*>(QStringLiteral("editDeductions"));
    require(qualification && total && editor, "declared appraisal needs deduction controls");
    const auto apply_deductions = [&](const QStringList& ids) {
        bool applied = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("calculationDeductionDialog"));
            require(dialog, "open the actual deduction editor");
            auto* source = dialog->findChild<QComboBox*>(QStringLiteral("calculationDeductionSource"));
            auto* list = dialog->findChild<QListWidget*>(QStringLiteral("calculationDeductionList"));
            auto* add = dialog->findChild<QPushButton*>(QStringLiteral("addCalculationDeduction"));
            auto* remove = dialog->findChild<QPushButton*>(QStringLiteral("removeCalculationDeduction"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("calculationDeductionButtons"));
            require(source && list && add && remove && buttons, "deduction editor exposes staged editing");
            while (list->count() > 0) {
                list->setCurrentRow(0);
                remove->click();
            }
            for (const auto& id : ids) {
                const auto index = source->findData(id);
                require(index >= 0, "deduction candidate is available on the parent floor");
                source->setCurrentIndex(index);
                add->click();
            }
            buttons->button(QDialogButtonBox::Apply)->click();
            applied = dialog->result() == QDialog::Accepted;
            if (!applied) dialog->reject();
        });
        editor->click();
        return applied;
    };
    const auto before = window.document().revision();
    require(apply_deductions({hole}), "declared appraisal must link a void without requiring a manual category");
    auto stored = window.document().snapshot().entities().at(outer.toStdString());
    require(window.document().revision() == before + 1 &&
                stored.properties.at("deduction_ids") == std::vector<std::string>{hole.toStdString()} &&
                !stored.properties.contains("appraisal_category") &&
                qualification->text().startsWith("Qualified") && total->text().contains("89.24"),
            "physical deduction must persist once and automatically qualify the net appraisal total");
    require(window.undoCommand() && qualification->text().contains("Unqualified") &&
                !window.document().snapshot().entities().at(outer.toStdString()).properties.contains("deduction_ids") &&
                window.redoCommand() && qualification->text().startsWith("Qualified") && total->text().contains("89.24"),
            "deduction editing must undo and redo both links and automatic totals");
    require(window.editSelectedFactor(QStringLiteral("0.5")) && apply_deductions({}) &&
                qualification->text().contains("Unqualified") && apply_deductions({hole}) &&
                qualification->text().contains("Unqualified") && window.editSelectedFactor(QStringLiteral("1")) &&
                qualification->text().startsWith("Qualified") && total->text().contains("89.24"),
            "unqualified factors must not prevent correcting deductions or enable adjusted automatic totals");
    require(window.editSelectedAppraisalFacts(declarations().replace("\"finish\":\"finished\",", "")) &&
                apply_deductions({}) && qualification->text().contains("Unqualified") &&
                apply_deductions({hole}) && qualification->text().contains("Unqualified") &&
                window.editSelectedAppraisalFacts(declarations()) &&
                qualification->text().startsWith("Qualified") && total->text().contains("89.24"),
            "incomplete facts must permit correcting deductions while continuing to withhold automatic totals");

    const auto outside = window.createBoundary(square(10, 10, 1));
    const auto site = window.createBoundary(square(0.25, 0.25, 0.25), QStringLiteral("survey"));
    const auto room = window.createRoomBoundary(square(0.5, 0.5, 0.25));
    require(window.selectEntity(outer), "restore parent selection for invalid deductions");
    const auto stable = window.document().snapshot();
    for (const auto& candidate : {outside, site, room}) {
        require(!apply_deductions({candidate}) && window.document().revision() == stable.revision() &&
                    window.document().snapshot().entities().at(outer.toStdString()).properties ==
                        stable.entities().at(outer.toStdString()).properties,
                "outside, site and architectural room deductions must fail without changing existing links");
    }
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
    require(qualification->text().contains("exclusion must be linked", Qt::CaseInsensitive), "unlinked exclusions must not yield plausible qualified totals");
    auto snapshot = window.document().snapshot();
    auto parent = snapshot.entities().at(outer.toStdString());
    parent.properties["deduction_ids"] = std::vector<std::string>{hole.toStdString()};
    window.document().apply(sketch::ApplyEntityChanges{snapshot.revision(), {sketch::EntityChange::upsert(parent)}, {}, "link void"});
    require(window.selectEntity(hole) && qualification->text().startsWith("Qualified") &&
            window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"))->text().contains("89.24"),
            "selected void must not contribute independently or hide valid parent totals");
    auto* exclusion_classification = inspector_classification_control(window);
    auto* exclusion_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(exclusion_classification && exclusion_canvas &&
            exclusion_classification->currentText().startsWith(QStringLiteral("Excluded")) &&
            !exclusion_classification->isEnabled() &&
            std::any_of(exclusion_canvas->entities().begin(), exclusion_canvas->entities().end(),
                [&](const auto& entity) {
                    return entity.id == hole && entity.stroke_color == QColor(137, 86, 86) &&
                           entity.filled && entity.hatch_pattern == QStringLiteral("diagonal");
                }),
            "a qualified linked void must display Excluded read-only with the deduction style, not Unqualified");
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
    require(window.findChild<QLabel*>(QStringLiteral("appraisalQualificationStatus"))->text() ==
                QStringLiteral("Excluded"),
            "site boundaries must show Excluded in the compact appraisal status");
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

void contradictory_appraisal_ownership_withholds_inspector_and_schedule_totals() {
    sketch::desktop::MainWindow window;
    const auto area = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()),
            "ownership fixture must begin with a qualified 100 square foot area");
    const auto other_building = window.createBuilding(QStringLiteral("property-1"),
                                                      QStringLiteral("Other building"));
    require(!other_building.isEmpty(), "ownership fixture needs another valid building");

    auto snapshot = window.document().snapshot();
    auto boundary = snapshot.entities().at(area.toStdString());
    boundary.properties["building_id"] = other_building.toStdString();
    window.document().apply(sketch::ApplyEntityChanges{
        snapshot.revision(), {sketch::EntityChange::upsert(boundary)}, {},
        "inject contradictory appraisal building identity"});
    const auto invalid_snapshot = window.document().snapshot();
    require(window.selectEntity(area), "contradictory area must remain inspectable");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* total = window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(qualification->text().contains("Unqualified") &&
                qualification->text().contains("building_id disagrees with its floor") &&
                total->text() == QStringLiteral("—") && gla->text() == QStringLiteral("—"),
            "contradictory ownership must withhold inspector automatic totals");
    const auto schedule = window.scheduleSnapshot();
    require(std::any_of(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
                        [](const auto& row) {
                const auto status = row.cells.find("status");
                return row.kind == sketch::ScheduleRowKind::appraisal &&
                       status != row.cells.end() &&
                       std::holds_alternative<std::string>(status->second.value) &&
                       std::get<std::string>(status->second.value).find("totals withheld") !=
                           std::string::npos;
            }) &&
                std::none_of(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
                             [](const auto& row) {
                    return row.kind == sketch::ScheduleRowKind::appraisal &&
                           row.cells.contains("area");
                }),
            "contradictory ownership must also withhold schedule automatic totals");
    require(window.document().revision() == invalid_snapshot.revision() &&
                window.document().snapshot().entities() == invalid_snapshot.entities(),
            "inspecting and projecting contradictory ownership must not repair or mutate it");

    snapshot = window.document().snapshot();
    boundary.properties["building_id"] = "building-1";
    window.document().apply(sketch::ApplyEntityChanges{
        snapshot.revision(), {sketch::EntityChange::upsert(boundary)}, {},
        "correct appraisal building identity"});
    require(window.selectEntity(area) && qualification->text().startsWith("Qualified") &&
                total->text().contains("100.00") && gla->text().contains("100.00"),
            "correcting ownership must restore qualified 100 square foot inspector totals");
    const auto corrected_schedule = window.scheduleSnapshot();
    require(std::any_of(corrected_schedule.snapshot.rows.begin(),
                        corrected_schedule.snapshot.rows.end(), [](const auto& row) {
                const auto area_cell = row.cells.find("area");
                return row.kind == sketch::ScheduleRowKind::appraisal &&
                       row.object_id.ends_with(":category:above_grade_finished") &&
                       area_cell != row.cells.end() &&
                       std::holds_alternative<sketch::ScheduleQuantity>(area_cell->second.value) &&
                       std::abs(std::get<sketch::ScheduleQuantity>(area_cell->second.value).value -
                                9.290304) < 1e-8;
            }),
            "correcting ownership must restore the same 100 square foot schedule total");
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

void malformed_appraisal_projection_prints_withheld_status() {
    sketch::desktop::MainWindow window;
    require(!window.createBoundary(square(0, 0, 3.048)).isEmpty(),
            "malformed print fixture needs an appraisal boundary");
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()),
            "malformed print fixture must begin qualified");
    auto snapshot = window.document().snapshot();
    auto property = snapshot.entities().at("property-1");
    property.properties["appraisal_policy"] = "malformed";
    window.document().apply(sketch::ApplyEntityChanges{
        snapshot.revision(), {sketch::EntityChange::upsert(std::move(property))}, {},
        "inject malformed appraisal policy"});
    const auto schedule = window.scheduleSnapshot();
    require(std::any_of(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
                        [](const auto& row) {
                const auto status = row.cells.find("status");
                return row.kind == sketch::ScheduleRowKind::appraisal &&
                       status != row.cells.end() &&
                       std::holds_alternative<std::string>(status->second.value) &&
                       std::get<std::string>(status->second.value).find("totals withheld") !=
                           std::string::npos;
            }) &&
                std::none_of(schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(),
                             [](const auto& row) {
                    return row.kind == sketch::ScheduleRowKind::appraisal &&
                           row.cells.contains("area");
                }),
            "malformed appraisal data must print an unqualified status and no area values");
}

void plan_label_placement_workflow() {
    using sketch::desktop::PlanCanvas;
    sketch::desktop::MainWindow window;
    window.setMetricUnits(true);
    window.resize(1100, 780);
    window.show();
    const auto area = window.createBoundary(square(0, 0, 0.25));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "tiny area must qualify");
    require(!window.createAnnotationSymbol(QStringLiteral("desk"),{0.125,0.125}).isEmpty() && window.selectEntity(area),
        "furnished tiny area must remain selectable with its derived label");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    const auto label = [&]() -> sketch::desktop::CanvasLabel {
        const auto found = std::find_if(canvas->labels().begin(), canvas->labels().end(),
            [&](const auto& value) { return value.id == area && value.avoid_components; });
        require(found != canvas->labels().end(), "tiny area must retain its derived label");
        return *found;
    };
    require(label().text.contains(QStringLiteral("m²")) && label().leader_start && !label().show_background,
            "no-fit area must retain readable quantity with a subordinate leader");
    auto* place = window.findChild<QPushButton*>(QStringLiteral("placePlanLabel"));
    auto* automatic = window.findChild<QPushButton*>(QStringLiteral("automaticPlanLabel"));
    require(place && automatic && place->isEnabled(), "selected area needs contextual placement controls");
    QApplication::processEvents();
    canvas->setOverviewMapEnabled(false);
    canvas->fitView();
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!captures.isEmpty()) require(QDir().mkpath(captures) &&
        canvas->grab().save(QDir(captures).filePath(QStringLiteral("tiny-area-label-leader.png"))),
        "retain actual tiny-area canvas");
    const auto click = [&](sketch::Vec2 point) {
        const auto p = QRectF(canvas->rect()).center() + QPointF(
            (point.x-canvas->viewCenter().x)*canvas->viewScale(),
            -(point.y-canvas->viewCenter().y)*canvas->viewScale());
        QMouseEvent down(QEvent::MouseButtonPress,p,canvas->mapToGlobal(p.toPoint()),
            Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease,p,canvas->mapToGlobal(p.toPoint()),
            Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(canvas,&down);
        QApplication::sendEvent(canvas,&up);
    };
    auto seeded=window.document().snapshot();
    auto annotations=seeded.entities().at("annotations-1");
    annotations.properties["state"]["vendor_layout"]={{"retain",17}};
    annotations.extensions["vendor_annotation_owner"]={{"retain",true}};
    window.document().apply(sketch::ApplyEntityChanges{seeded.revision(),{sketch::EntityChange::upsert(annotations)}, {}, "seed opaque annotation metadata"});
    require(window.selectEntity(area), "reselect tiny area after newer metadata");
    const auto before = window.document().snapshot();
    const auto area_style=std::find_if(canvas->entities().begin(),canvas->entities().end(),
        [&](const auto& value){return value.id==area;});
    require(area_style!=canvas->entities().end(), "placement fixture retains owner geometry");
    const auto old_style=*area_style;
    place->click();
    const sketch::Vec2 chosen{0.75, 0.75};
    click(chosen);
    const auto placed = window.document().snapshot();
    const auto new_style=std::find_if(canvas->entities().begin(),canvas->entities().end(),
        [&](const auto& value){return value.id==area;});
    require(new_style!=canvas->entities().end() && new_style->stroke_color==old_style.stroke_color &&
        new_style->dark_stroke_color==old_style.dark_stroke_color && new_style->fill_color==old_style.fill_color &&
        new_style->hatch_pattern==old_style.hatch_pattern && new_style->stroke_width_metres==old_style.stroke_width_metres,
        "label placement must preserve the owner's semantic appearance");
    require(placed.revision() == before.revision()+1 && placed.entities().at(area.toStdString()) == before.entities().at(area.toStdString()) &&
            std::abs(label().position.x-chosen.x)<1e-9 && std::abs(label().position.y-chosen.y)<1e-9 && label().leader_start &&
            !canvas->boundaryDraftPreview(), "placement click must change presentation once without starting geometry");
    require(placed.entities().at("annotations-1").properties.at("state").at("vendor_layout")==annotations.properties.at("state").at("vendor_layout") &&
            placed.entities().at("annotations-1").extensions==annotations.extensions &&
            placed.entities().at("annotations-1").properties.at("state").at("symbols")==annotations.properties.at("state").at("symbols"),
            "placement must preserve opaque annotation metadata and pinned furnished symbols");
    if (!captures.isEmpty()) require(canvas->grab().save(QDir(captures).filePath(QStringLiteral("manual-area-label-leader.png"))),
        "retain actual manually placed label canvas");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
            window.redoCommand() && window.document().snapshot().entities()==placed.entities(), "placement must undo/redo");
    automatic->click();
    require(label().leader_start && std::abs(label().position.x-chosen.x)>1e-6 &&
            window.undoCommand() && window.document().snapshot().entities()==placed.entities(), "Automatic must reset offset and undo restores it");
    QTemporaryDir directory;
    const auto pdf_path=directory.filePath(QStringLiteral("manual-area-label.pdf"));
    require(window.exportDraftPdf(pdf_path), "manual derived labels must render in actual PDF output");
    QPdfDocument pdf;
    require(pdf.load(pdf_path)==QPdfDocument::Error::None && pdf.getAllText(0).text().contains(label().text),
        "actual plan PDF must retain the manually placed derived quantity");
    if (!captures.isEmpty()) require(QFile::copy(pdf_path,QDir(captures).filePath(QStringLiteral("manual-area-label.pdf"))) &&
        pdf.render(0,QSize(1400,1000)).save(QDir(captures).filePath(QStringLiteral("manual-area-label-pdf.png"))),
        "retain actual exported plan label PDF");
    const auto path=directory.filePath(QStringLiteral("label-placement.bldproj"));
    require(directory.isValid() && window.saveProjectAs(path) && window.openProject(path) && window.selectEntity(area) &&
            std::abs(label().position.x-chosen.x)<1e-9, "saved placement must reopen at the derived offset");
    const auto geometry_before=window.document().snapshot();
    const auto identified=sketch::decode_identified_boundary_entity(geometry_before.entities().at(area.toStdString()));
    const auto old_text=label().text;
    const auto old_label=label().position;
    require(window.moveSelectedBoundaryVertex(QString::fromStdString(identified.segments.front().start_vertex_id),{-0.1,-0.1}),
        "placed label fixture supports ordinary corner editing");
    require(label().text!=old_text && label().plan_label_offset &&
            (std::abs(label().position.x-old_label.x)>1e-6 || std::abs(label().position.y-old_label.y)>1e-6) &&
            window.undoCommand() && std::abs(label().position.x-chosen.x)<1e-9,
        "derived quantity and attached label anchor must follow corner edits and undo");
    require(window.beginSelectedPlanLabelPlacement(), "begin cancellable placement");
    const auto cancellation=window.document().snapshot();
    const auto pan_center=canvas->viewCenter();
    const auto pan_start=QRectF(canvas->rect()).center();
    const auto pan_end=pan_start+QPointF(80,30);
    QMouseEvent pan_down(QEvent::MouseButtonPress,pan_start,canvas->mapToGlobal(pan_start.toPoint()),
        Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent pan_move(QEvent::MouseMove,pan_end,canvas->mapToGlobal(pan_end.toPoint()),
        Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent pan_up(QEvent::MouseButtonRelease,pan_end,canvas->mapToGlobal(pan_end.toPoint()),
        Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&pan_down); QApplication::sendEvent(canvas,&pan_move); QApplication::sendEvent(canvas,&pan_up);
    require(window.document().revision()==cancellation.revision() &&
                std::hypot(canvas->viewCenter().x-pan_center.x,canvas->viewCenter().y-pan_center.y)>0,
            "dragging while placement is armed must pan without placing or altering history");
    const sketch::Vec2 after_pan{0.9,0.9};
    click(after_pan);
    require(std::abs(label().position.x-after_pan.x)<1e-9 && window.undoCommand() &&
                window.document().snapshot().entities()==cancellation.entities(),
            "panning must leave placement armed for the next click and Undo restores prior placement");
    require(window.beginSelectedPlanLabelPlacement(),"re-arm placement for Escape");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(canvas,&escape);
    require(window.document().snapshot().entities()==cancellation.entities() && window.selectedEntityId()==area,
            "Escape must cancel label placement without clearing its selection or altering history");
    require(window.beginSelectedPlanLabelPlacement(), "begin fenced placement");
    auto changed=window.document().snapshot().entities().at(area.toStdString());
    changed.properties["newer_note"]="preserve";
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),{sketch::EntityChange::upsert(changed)}, {}, "newer edit"});
    const auto newer=window.document().snapshot();
    click(chosen);
    require(window.document().revision()==newer.revision() && window.document().snapshot().entities()==newer.entities(),
            "stale pending placement must preserve newer document data");
    window.document().mark_read_only("label placement read-only fixture");
    require(!window.beginSelectedPlanLabelPlacement() && !window.setSelectedPlanLabelPosition({2,2}) &&
            !window.resetSelectedPlanLabelPlacement() && window.document().revision()==newer.revision(),
            "read-only placement commands must refuse mutation");
    sketch::desktop::MainWindow room_window;
    room_window.resize(1100,780); room_window.show();
    const auto room=room_window.createRoomVolumeFromBoundary(square(0,0,2),"3 m","0 m");
    require(!room.isEmpty(),"native room placement fixture must create a real room volume");
    auto named_room=room_window.document().snapshot().entities().at(room.toStdString());
    named_room.properties["name"]="Office";
    room_window.document().apply(sketch::ApplyEntityChanges{room_window.document().revision(),
        {sketch::EntityChange::upsert(named_room)}, {}, "name native room fixture"});
    require(room_window.selectEntity(room),"native room with a derived name must select");
    auto* room_place=room_window.findChild<QPushButton*>(QStringLiteral("placePlanLabel"));
    require(room_place && room_place->isEnabled() && !room_place->parentWidget()->isHidden() &&
                room_window.setSelectedPlanLabelPosition({3,3}) &&
                room_window.document().snapshot().entities().at(room.toStdString())==named_room,
            "native room placement controls must be available and preserve physical room geometry");
}

void appraisal_qualification_transition_preview_workflow(bool named=true) {
    using namespace sketch;
    using namespace sketch::desktop;
    MainWindow window;
    window.resize(1100,780); window.show(); window.setMetricUnits(true);
    const auto parent=window.createBoundary(square(0,0,2));
    auto* workflow=window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()),"declare preview qualification parent");
    if (!named)
        require(window.setSelectedPlanLabelPosition({3,3}),
                "a qualified anonymous area must allow saved label placement");
    const Boundary outline{{{-.2,.5},{1.5,.5},0},{{1.5,.5},{1.5,1.5},0},
                           {{1.5,1.5},{.5,1.5},0},{{.5,1.5},{-.2,.5},0}};
    const auto child=window.createBoundary(outline);
    require(window.editSelectedAppraisalFacts(declarations("residential_declared","garage")),
            "declare preview qualification deduction");
    auto outer=window.document().snapshot().entities().at(parent.toStdString());
    outer.properties["name"]=named ? "Dwelling" : "measurement"; outer.properties["deduction_ids"]={child.toStdString()};
    auto inner=window.document().snapshot().entities().at(child.toStdString());
    inner.properties["name"]=named ? "Garage" : "measurement";
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(outer),EntityChange::upsert(inner)}, {}, "outside deduction qualification fixture"});
    require(window.selectEntity(child),"select outside deduction for repair");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    canvas->setSnapEnabled(false); canvas->setOverviewMapEnabled(false); canvas->fitView();
    const auto text=[&](const auto& labels,const QString& id){
        const auto found=std::find_if(labels.begin(),labels.end(),[&](const auto& value){return value.id==id;});
        return found==labels.end() ? QString{} : found->text;
    };
    const auto old_parent=named ? QStringLiteral("Dwelling") : QString{};
    const auto old_child=named ? QStringLiteral("Garage") : QString{};
    const auto new_parent=(named ? QStringLiteral("Dwelling\n") : QString{})+QStringLiteral("3.00 m²");
    const auto new_child=(named ? QStringLiteral("Garage\n") : QString{})+QStringLiteral("1.00 m²");
    require(text(canvas->labels(),parent)==old_parent && text(canvas->labels(),child)==old_child,
            "outside deduction must start with withheld appraisal quantities");
    const auto before=window.document().snapshot();
    const auto pixel=[&](Vec2 point){return QRectF(canvas->rect()).center()+QPointF(
        (point.x-canvas->viewCenter().x)*canvas->viewScale(),-(point.y-canvas->viewCenter().y)*canvas->viewScale());};
    const auto start=pixel({-.2,.5}), end=pixel({.5,.5});
    QMouseEvent down(QEvent::MouseButtonPress,start,canvas->mapToGlobal(start.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove,end,canvas->mapToGlobal(end.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&down); QApplication::sendEvent(canvas,&move);
    QElapsedTimer timer; timer.start();
    while(!canvas->boundaryVertexPreviewMetrics() && timer.elapsed()<3000)
        QApplication::processEvents(QEventLoop::AllEvents,30);
    if (!canvas->boundaryVertexPreviewMetrics() ||
        text(canvas->boundaryVertexPreviewLabels(),parent)!=new_parent ||
        text(canvas->boundaryVertexPreviewLabels(),child)!=new_child) {
        std::cerr<<"qualification preview named="<<named<<" metrics="
                 <<canvas->boundaryVertexPreviewMetrics().has_value()<<" parent="
                 <<text(canvas->boundaryVertexPreviewLabels(),parent).toStdString()<<" child="
                 <<text(canvas->boundaryVertexPreviewLabels(),child).toStdString()<<'\n';
    }
    require(canvas->boundaryVertexPreviewMetrics() &&
                text(canvas->boundaryVertexPreviewLabels(),parent)==new_parent &&
                text(canvas->boundaryVertexPreviewLabels(),child)==new_child &&
                window.document().snapshot().entities()==before.entities(),
            "repair preview must gain qualified quantities and footprints before any commit");
    const auto proposed_labels=canvas->boundaryVertexPreviewLabels();
    QMouseEvent up(QEvent::MouseButtonRelease,end,canvas->mapToGlobal(end.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&up); QApplication::processEvents();
    require(text(canvas->labels(),parent)==new_parent && text(canvas->labels(),child)==new_child,
            "committed qualification must agree with candidate label values");
    for (const auto& proposed:proposed_labels) {
        if (proposed.id!=parent && proposed.id!=child) continue;
        const auto committed=std::find_if(canvas->labels().begin(),canvas->labels().end(),
            [&](const auto& label){return label.id==proposed.id;});
        require(committed!=canvas->labels().end() &&
                    std::hypot(committed->position.x-proposed.position.x,
                               committed->position.y-proposed.position.y)<1e-9,
                "newly qualified label placement must remain stable across commit");
        if (!named && proposed.id==parent)
            require(std::hypot(proposed.position.x-3,proposed.position.y-3)<1e-9,
                    "newly qualified anonymous label must honor its persisted position");
    }
    require(window.undoCommand() && text(canvas->labels(),parent)==old_parent,
            "committed qualification and Undo must agree with candidate label values");
}

void appraisal_plan_area_labels_workflow() {
    using sketch::desktop::PlanCanvas;
    sketch::desktop::MainWindow window;
    window.setMetricUnits(false);
    const auto outer = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow, "plan labels need the actual Appraisal workflow control");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "declare the enclosing living area");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "plan labels need the actual retained canvas");
    const auto label_text = [&](const QString& id) {
        const auto found = std::find_if(canvas->labels().begin(), canvas->labels().end(),
            [&](const auto& label) { return label.id == id && label.plan_only && label.avoid_components; });
        return found == canvas->labels().end() ? QString{} : found->text;
    };
    require(label_text(outer) == QStringLiteral("100.00 ft²"),
            "generic area names must still show authoritative Appraisal area on the plan");
    const auto garage = window.createBoundary(square(0.5, 0.5, 1.524));
    require(!garage.isEmpty() && !label_text(outer).contains(QStringLiteral("ft²")),
            "a newly unqualified participating area must withhold existing numerical plan labels");
    require(window.editSelectedAppraisalFacts(declarations("residential_declared", "garage")) &&
            window.applySelectedAutoSubtract(outer), "declare and subtract an internal garage");
    const auto edit_name = [&](const QString& id, const QString& value) {
        require(window.selectEntity(id), "select an area for its actual Name control");
        auto* name = window.findChild<QLineEdit*>(QStringLiteral("areaName"));
        require(name && name->isEnabled(), "area properties must offer a direct editable Name field");
        const auto before = window.document().snapshot();
        name->setText(value);
        name->setModified(true);
        require(QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection),
                "commit the actual area Name control");
        auto expected = before.entities();
        if (value.trimmed().isEmpty()) expected.at(id.toStdString()).properties.erase("name");
        else expected.at(id.toStdString()).properties["name"] = value.trimmed().toStdString();
        require(window.document().revision() == before.revision() + 1 &&
                window.document().snapshot().entities() == expected,
                "area name editing must change only its owner's name in one history command");
        const auto saved = window.document().snapshot();
        name->setModified(true);
        QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
        require(window.document().revision() == saved.revision(), "unchanged area name must be a no-op");
    };
    edit_name(outer, QStringLiteral(" First floor "));
    edit_name(garage, QStringLiteral("Garage"));
    const auto named = window.document().snapshot();
    require(window.undoCommand() && window.redoCommand() &&
            window.document().snapshot().entities() == named.entities(),
            "area Name edits must undo/redo without changing appraisal geometry or facts");
    edit_name(garage, QStringLiteral("   "));
    require(label_text(garage) == QStringLiteral("25.00 ft²") && window.undoCommand() &&
            window.document().snapshot().entities() == named.entities(),
            "clearing an area name removes only the name and undo restores it");
    require(window.selectEntity(garage), "select the area before testing a stale Name edit");
    auto* stale_name = window.findChild<QLineEdit*>(QStringLiteral("areaName"));
    stale_name->setText(QStringLiteral("Stale name"));
    stale_name->setModified(true);
    auto external_area = window.document().snapshot().entities().at(garage.toStdString());
    external_area.properties["external_note"] = "preserve newer data";
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
        {sketch::EntityChange::upsert(external_area)}, {}, "External area edit"});
    const auto external = window.document().snapshot();
    QMetaObject::invokeMethod(stale_name, "editingFinished", Qt::DirectConnection);
    require(window.document().revision() == external.revision() &&
            window.document().snapshot().entities() == external.entities() && window.undoCommand() &&
            window.document().snapshot().entities() == named.entities(),
            "stale Name edits must preserve newer area data without another command");
    {
        sketch::desktop::MainWindow read_only_window;
        const auto read_only_area = read_only_window.createBoundary(square(0, 0, 3.048));
        read_only_window.document().mark_read_only("Name field read-only fixture");
        require(read_only_window.selectEntity(read_only_area), "select the read-only area");
        auto* read_only_name = read_only_window.findChild<QLineEdit*>(QStringLiteral("areaName"));
        require(read_only_name && !read_only_name->isEnabled(), "read-only areas must disable Name editing");
        const auto before = read_only_window.document().snapshot();
        read_only_name->setText(QStringLiteral("Cannot save"));
        read_only_name->setModified(true);
        QMetaObject::invokeMethod(read_only_name, "editingFinished", Qt::DirectConnection);
        require(read_only_window.document().revision() == before.revision() &&
                read_only_window.document().snapshot().entities() == before.entities(),
                "programmatic Name signals must not mutate a read-only document");
    }
    require(window.selectEntity(outer) && label_text(outer) == QStringLiteral("First Floor\n75.00 ft²") &&
            label_text(garage) == QStringLiteral("Garage\n25.00 ft²"),
            "plan labels must show net parent area and the separate garage contribution exactly once");
    const auto parent_label = std::find_if(canvas->labels().begin(), canvas->labels().end(),
        [&](const auto& label) { return label.id == outer && label.avoid_components; });
    require(parent_label != canvas->labels().end() &&
            !(parent_label->position.x > 0.5 && parent_label->position.x < 2.024 &&
              parent_label->position.y > 0.5 && parent_label->position.y < 2.024),
            "the parent's net-area name/value must be placed outside its deducted garage");
    const auto void_area = window.createBoundary(square(0.1, 2.2, 0.3048));
    require(!void_area.isEmpty() &&
            window.editSelectedAppraisalFacts(declarations("residential_declared", "dwelling", "above", "other_void")) &&
            window.applySelectedAutoSubtract(outer) && !label_text(void_area).contains(QStringLiteral("ft²")) &&
            label_text(outer) == QStringLiteral("First Floor\n74.00 ft²"),
            "linked voids must reduce the parent without acquiring a standalone Appraisal area label");
    require(window.deleteSelection() && label_text(outer) == QStringLiteral("First Floor\n75.00 ft²"),
            "deleting a void must rebuild its remaining parent's exact net label");
    const auto site = window.createBoundary(square(20, 20, 10), QStringLiteral("survey"));
    require(!site.isEmpty() && !label_text(site).contains(QStringLiteral("ft²")) &&
            label_text(outer) == QStringLiteral("First Floor\n75.00 ft²") && window.deleteSelection(),
            "independent site outlines must not acquire numerical building Appraisal labels or change GLA");
    const auto separate_layer = window.createLayer(QStringLiteral("floor-1"), QStringLiteral("Garage presentation"));
    require(!separate_layer.isEmpty(), "presentation filter fixture needs a distinct layer");
    auto source = window.document().snapshot();
    auto relayered = source.entities().at(garage.toStdString());
    relayered.properties["layer_id"] = separate_layer.toStdString();
    window.document().apply(sketch::ApplyEntityChanges{source.revision(),
        {sketch::EntityChange::upsert(relayered)}, {}, "Move garage to a presentation layer"});
    require(window.setContainerVisible(separate_layer, false) && window.selectEntity(outer) &&
            label_text(outer) == QStringLiteral("First Floor\n75.00 ft²") && label_text(garage).isEmpty(),
            "hiding a deduction's layer must not alter its visible parent's numerical area");
    require(window.setContainerVisible(separate_layer, true), "restore the garage presentation");
    window.setMetricUnits(true);
    require(label_text(outer) == QStringLiteral("First Floor\n6.97 m²") &&
            label_text(garage) == QStringLiteral("Garage\n2.32 m²"),
            "automatic labels must convert workspace units without altering geometry");
    window.setMetricUnits(false);
    require(window.selectEntity(garage), "select the deduction for an ordinary size edit");
    window.resize(1100, 780);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(garage), "select the displayed deduction before dragging its vertex");
    canvas->setSnapEnabled(false);
    canvas->setOverviewMapEnabled(false);
    canvas->fitView();
    const auto before_drag = window.document().snapshot();
    const auto screen = [&](sketch::Vec2 point) {
        return QRectF(canvas->rect()).center() + QPointF(
            (point.x - canvas->viewCenter().x) * canvas->viewScale(),
            -(point.y - canvas->viewCenter().y) * canvas->viewScale());
    };
    const auto press = screen({0.5, 0.5});
    const auto move = screen({0.7, 0.7});
    QMouseEvent down(QEvent::MouseButtonPress, press, canvas->mapToGlobal(press.toPoint()),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &down);
    QMouseEvent drag(QEvent::MouseMove, move, canvas->mapToGlobal(move.toPoint()),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &drag);
    QElapsedTimer preview_wait;
    preview_wait.start();
    while (!canvas->boundaryVertexPreviewMetrics() && preview_wait.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 30);
    if (!canvas->boundaryVertexPreviewMetrics()) {
        const auto retained = std::find_if(canvas->entities().begin(), canvas->entities().end(),
            [&](const auto& item) { return item.id == garage; });
        throw std::runtime_error((QStringLiteral("Real vertex preview missing: serial %1, handles %2, selected %3, scale %4, point %5,%6; %7")
            .arg(canvas->boundaryVertexPreviewSerial())
            .arg(retained == canvas->entities().end() ? 0 : retained->vertex_handles.size())
            .arg(retained != canvas->entities().end() && retained->selected)
            .arg(canvas->viewScale()).arg(press.x()).arg(press.y()).arg(window.lastError())).toStdString());
    }
    // Moving this corner by 0.2 m on each axis removes 1.524 * 0.2 m²
    // from the five-foot square. The unchanged parent gains that deduction.
    const std::map<QString, QString> preview_values{
        {outer, QStringLiteral("First Floor\n78.28 ft²")},
        {garage, QStringLiteral("Garage\n21.72 ft²")}};
    for (const auto& [id, expected] : preview_values) {
        const auto& labels = canvas->boundaryVertexPreviewLabels();
        const auto proposed = std::find_if(labels.begin(), labels.end(), [&](const auto& label) { return label.id == id; });
        require(proposed != labels.end() && proposed->text == expected,
                "vertex previews must recalculate net numbers for both the moved deduction and unchanged parent");
    }
    require(window.document().revision() == before_drag.revision() &&
                window.document().snapshot().entities() == before_drag.entities(),
            "live area labels must not commit preview geometry or history");
    if (const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); !captures.isEmpty()) {
        require(QDir().mkpath(captures) &&
                    canvas->grab().save(QDir(captures).filePath(QStringLiteral("live-net-area-preview.png"))),
                "retain the actual canvas with live parent and deduction values");
    }
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    require(window.document().snapshot().entities() == before_drag.entities() &&
            label_text(outer) == QStringLiteral("First Floor\n75.00 ft²"),
            "cancelling a vertex preview must restore committed area labels without changing geometry");
    QApplication::sendEvent(canvas, &down);
    QApplication::sendEvent(canvas, &drag);
    preview_wait.restart();
    while (!canvas->boundaryVertexPreviewMetrics() && preview_wait.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 30);
    require(canvas->boundaryVertexPreviewMetrics().has_value(),
            "repeat the same accepted corner preview before committing");
    QMouseEvent up(QEvent::MouseButtonRelease, move, canvas->mapToGlobal(move.toPoint()),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &up);
    QApplication::processEvents();
    require(window.document().revision() == before_drag.revision() + 1 &&
                label_text(outer) == preview_values.at(outer) &&
                label_text(garage) == preview_values.at(garage),
            "committing the corner must match the previewed parent and deduction net values");
    require(window.undoCommand() &&
                window.document().snapshot().entities() == before_drag.entities() &&
                label_text(outer) == QStringLiteral("First Floor\n75.00 ft²"),
            "undo must restore the geometry and the committed area values after the live preview");
    // Connected mode preserves the complementary 5-ft edge while changing
    // the anchored edge to 4 ft: (4 + 5) / 2 * 5 = 22.5 ft².
    const auto before_size = window.document().snapshot();
    const auto boundary = sketch::decode_identified_boundary_entity(before_size.entities().at(garage.toStdString()));
    require(window.editSelectedBoundaryEdgeLength(QString::fromStdString(boundary.segments.front().segment_id),
        QStringLiteral("4 ft"), sketch::BoundaryFixedEndpoint::start, true),
        "Appraisal plan labels must follow a real measured-edge edit");
    if (label_text(outer) != QStringLiteral("First Floor\n77.50 ft²") ||
        label_text(garage) != QStringLiteral("Garage\n22.50 ft²"))
        throw std::runtime_error((QStringLiteral("Actual labels after measured edit: parent [%1], garage [%2]")
            .arg(label_text(outer), label_text(garage))).toStdString());
    require(window.undoCommand() && label_text(outer) == QStringLiteral("First Floor\n75.00 ft²") &&
            window.redoCommand() && label_text(outer) == QStringLiteral("First Floor\n77.50 ft²"),
            "ordinary undo/redo must rebuild derived area labels");
    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath(QStringLiteral("plan-labels.bldproj"))) &&
            window.openProject(directory.filePath(QStringLiteral("plan-labels.bldproj"))) &&
            label_text(outer) == QStringLiteral("First Floor\n77.50 ft²"),
            "ordinary save/reopen must rebuild exact numerical plan labels");
    const auto pdf_path = directory.filePath(QStringLiteral("appraisal-plan-labels.pdf"));
    require(window.exportDraftPdf(pdf_path), "the ordinary draft plan output must include calculated area labels");
    QPdfDocument pdf;
    require(pdf.load(pdf_path) == QPdfDocument::Error::None && pdf.pageCount() > 0,
            "the actual Appraisal plan PDF must reopen");
    const auto text = pdf.getAllText(0).text().simplified();
    require(text.contains(QStringLiteral("First Floor")) && text.contains(QStringLiteral("77.50 ft²")) &&
            text.contains(QStringLiteral("Garage")) && text.contains(QStringLiteral("22.50 ft²")),
            "plan PDF must retain complete names and numerical labels without clipping a second line");
    require(text.count(QStringLiteral("77.50 ft²")) == 1 && text.count(QStringLiteral("22.50 ft²")) == 1,
            "plan-only area values must not leak into elevation or section sheet viewports");
    const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!captures.isEmpty()) {
        require(QFile::copy(pdf_path, QDir(captures).filePath(QStringLiteral("appraisal-plan-labels.pdf"))) &&
                pdf.render(0, QSize(1400, 1000)).save(QDir(captures).filePath(QStringLiteral("appraisal-plan-labels.png"))),
                "retain the actual Appraisal plan output for visual review");
    }
    require(window.selectEntity(garage) &&
            window.editSelectedAppraisalFacts(declarations("residential_declared", "garage").replace("\"finish\":\"finished\",", "")) &&
            !label_text(outer).contains(QStringLiteral("ft²")) &&
            label_text(outer) == QStringLiteral("First Floor"),
            "unqualified plans retain their names and withhold numerical Appraisal assertions");
}

void selection_filter_dropdown_workflow() {
    using namespace sketch::desktop;
    MainWindow window;
    const auto area = window.createBoundary(square(0, 0, 3.048));
    require(!area.isEmpty() && window.selectEntity(area), "dimension filter fixture needs a closed boundary");
    if (sketch::inspect_boundary_entity_version(window.document().snapshot().entities().at(area.toStdString())).format ==
            sketch::BoundaryEntityFormat::anonymous_legacy)
        require(window.upgradeSelectedBoundaryIdentities(), "measured dimensions need stable boundary identities");
    const auto boundary = sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(area.toStdString()));
    const auto length_dimension = window.createLengthDimension(area,
        QString::fromStdString(boundary.segments.front().segment_id), {1.5, -2});
    const auto area_dimension = window.createAreaDimension(area, {-2, 1.5});
    require(!length_dimension.isEmpty() && !area_dimension.isEmpty(), "persist both length and label-only area dimensions");
    const auto symbol = window.createAnnotationSymbol("svg-v2-04_living-sectional-left", {5, 5});
    require(!area.isEmpty() && !symbol.isEmpty() && window.selectEntity(area, true),
            "selection filter fixture must retain a mixed area and furniture selection");
    auto* filter = window.findChild<QComboBox*>(QStringLiteral("selectionFilter"));
    auto* measurement = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    auto* architectural = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas")));
    require(filter && measurement && architectural, "selection filter must be a real dropdown shared by both workspaces");
    const auto before = window.document().snapshot();
    const auto selected = window.selectedEntityIds();
    require(selected.contains(area) && selected.contains(symbol), "capture both existing selected owners");
    for (const auto value : {CanvasSelectionFilter::all, CanvasSelectionFilter::areas,
             CanvasSelectionFilter::objects, CanvasSelectionFilter::dimensions, CanvasSelectionFilter::labels,
             CanvasSelectionFilter::symbols, CanvasSelectionFilter::references}) {
        const auto index = filter->findData(static_cast<int>(value));
        require(index >= 0, "selection dropdown must expose every supported category");
        filter->setCurrentIndex(index);
        const auto after = window.document().snapshot();
        require(measurement->selectionFilter() == value && architectural->selectionFilter() == value,
                "dropdown changes must reach both real workspace canvases");
        require(after.revision() == before.revision() && after.entities() == before.entities() &&
                    after.history().size() == before.history().size() && window.selectedEntityIds() == selected,
                "selection filters must preserve the document and existing mixed selection");
    }
    filter->setCurrentIndex(filter->findData(static_cast<int>(CanvasSelectionFilter::dimensions)));
    require(std::any_of(measurement->entities().begin(), measurement->entities().end(),
                [&](const auto& entity) { return entity.id == length_dimension && entity.type == "dimension_line"; }) &&
            std::any_of(measurement->labels().begin(), measurement->labels().end(),
                [&](const auto& label) { return label.id == area_dimension && label.selection_type == "dimension"; }),
            "Dimensions must include real persisted line dimensions and label-only area dimensions");
    window.show();
    QApplication::processEvents();
    measurement->setOverviewMapEnabled(false);
    measurement->fitView();
    for (const auto& id : {length_dimension, area_dimension}) {
        const auto label = std::find_if(measurement->labels().begin(), measurement->labels().end(),
            [&](const auto& item) { return item.id == id; });
        require(label != measurement->labels().end(), "measured dimension retains its real selectable label");
        const auto point = QRectF(measurement->rect()).center() + QPointF(
            (label->position.x - measurement->viewCenter().x) * measurement->viewScale(),
            -(label->position.y - measurement->viewCenter().y) * measurement->viewScale());
        QMouseEvent down(QEvent::MouseButtonPress, point, measurement->mapToGlobal(point.toPoint()),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(measurement, &down);
        QMouseEvent up(QEvent::MouseButtonRelease, point, measurement->mapToGlobal(point.toPoint()),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(measurement, &up);
        require(window.selectedEntityId() == id, "Dimensions filter must allow clicking each measured dimension owner");
    }
    require(window.document().revision() == before.revision() && window.document().snapshot().entities() == before.entities(),
            "dimension picking must not change persisted measurements");
}

void area_appearance_workflow() {
    using namespace sketch;
    sketch::desktop::MainWindow window;
    window.setMetricUnits(false);
    const auto area = window.createBoundary(square(0, 0, 3.048));
    require(!area.isEmpty() && window.selectEntity(area), "area appearance needs a selected closed area");
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "appearance fixture qualifies 100 square feet GLA");
    const auto symbol = window.createAnnotationSymbol("svg-v2-04_living-sectional-left", {5, 5});
    require(!symbol.isEmpty() && window.selectEntity(area), "appearance fixture retains unrelated furniture");
    auto* appearance = window.findChild<QPushButton*>(QStringLiteral("areaAppearanceButton"));
    require(appearance && appearance->isEnabled(), "selected areas must expose an editable Area appearance control");
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "appearance checks use the actual retained canvas");
    const auto initial = window.document().snapshot();
    const auto gla = [&] { return window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"))->text(); };
    const auto initial_gla = gla();
    const auto open = [&](const std::function<void(QDialog*)>& edit) {
        require(window.selectEntity(area), "select area before editing appearance");
        std::exception_ptr callback_failure;
        QTimer::singleShot(0, &window, [&] {
            try {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                require(dialog && dialog->objectName() == QStringLiteral("areaAppearanceDialog"), "actual appearance dialog");
                edit(dialog);
            } catch (...) {
                callback_failure = std::current_exception();
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
            }
        });
        appearance->click();
        if (callback_failure) std::rethrow_exception(callback_failure);
    };
    const auto buttons = [](QDialog* dialog) {
        auto* value = dialog->findChild<QDialogButtonBox*>();
        require(value && value->button(QDialogButtonBox::Apply), "appearance must have Apply and Cancel");
        return value;
    };
    open([&](QDialog* dialog) { buttons(dialog)->button(QDialogButtonBox::Apply)->click(); });
    require(window.document().revision() == initial.revision() &&
                window.document().snapshot().history().size() == initial.history().size() &&
                window.document().snapshot().entities() == initial.entities(),
            "untouched default appearance Apply must not create an override or history entry");
    open([&](QDialog* dialog) {
        require(dialog->findChild<QLineEdit*>("areaOutlineColor") && dialog->findChild<QLineEdit*>("areaFillColor") &&
                dialog->findChild<QComboBox*>("areaFillPattern") && dialog->findChild<QDoubleSpinBox*>("areaHatchScale") &&
                dialog->findChild<QDoubleSpinBox*>("areaLineWidthMm") && dialog->findChild<QCheckBox*>("areaVisible"),
                "area editor must provide the usable outline/fill/hatch/width/visibility controls");
        dialog->findChild<QLineEdit*>("areaOutlineColor")->setText("#253545");
        dialog->findChild<QLineEdit*>("areaFillColor")->setText("#A4D9B0");
        auto* pattern = dialog->findChild<QComboBox*>("areaFillPattern");
        pattern->setCurrentIndex(pattern->findData("solid"));
        dialog->findChild<QDoubleSpinBox*>("areaHatchScale")->setValue(2.5);
        dialog->findChild<QDoubleSpinBox*>("areaLineWidthMm")->setValue(0.75);
        const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!captures.isEmpty())
            require(QDir().mkpath(captures) && dialog->grab().save(QDir(captures).filePath("area-appearance-dialog.png")),
                    "retain the actual configured area appearance dialog for visual review");
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    auto styled = window.document().snapshot();
    require(styled.revision() == initial.revision() + 1 && gla() == initial_gla &&
            styled.entities().at(area.toStdString()) == initial.entities().at(area.toStdString()),
            "appearance is one history command and preserves geometry, appraisal facts and totals");
    const auto provider = [&](const DocumentSnapshot& snapshot) {
        for (const auto& [id, entity] : snapshot.entities()) {
            if (entity.type != kAnnotationEntityType) continue;
            for (const auto& record : entity.properties.at("state").at("overrides"))
                if (record.at("target_kind") == "area" && record.at("target_id") == area.toStdString()) return id;
        }
        return std::string{};
    };
    const auto provider_id = provider(styled);
    require(!provider_id.empty(), "area appearance must be persisted as a presentation override");
    for (const auto& [id, entity] : initial.entities())
        if (id != provider_id) require(styled.entities().at(id) == entity, "appearance must preserve unrelated source entities");
    const auto canvas_area = [&]() -> const desktop::CanvasEntity& {
        const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
            [&](const auto& item) { return item.id == area; });
        require(found != canvas->entities().end(), "styled area must remain in the actual canvas");
        return *found;
    };
    require(canvas_area().stroke_color == QColor("#253545") && canvas_area().fill_color == QColor("#A4D9B0") &&
            canvas_area().filled && std::abs(canvas_area().hatch_scale - 2.5) < 1e-12 &&
            std::abs(canvas_area().output_stroke_width_mm - 0.75) < 1e-12 &&
            !canvas_area().dark_stroke_color.isValid(), "actual canvas must retain the explicit style and paper width");
    require(window.undoCommand() && window.document().snapshot().entities() == initial.entities() &&
            window.redoCommand() && window.document().snapshot().entities() == styled.entities(),
            "one-step history restores the exact area style");
    const auto before_noop = window.document().snapshot();
    open([&](QDialog* dialog) { buttons(dialog)->button(QDialogButtonBox::Apply)->click(); });
    const auto after_noop = window.document().snapshot();
    require(after_noop.revision() == before_noop.revision() &&
                after_noop.history().size() == before_noop.history().size() &&
                after_noop.entities() == before_noop.entities(),
            "applying unchanged appearance must not add a history command or normalize saved data");
    const auto before_visibility = window.document().snapshot();
    open([&](QDialog* dialog) {
        dialog->findChild<QCheckBox*>("areaVisible")->setChecked(false);
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    auto hidden_expected = before_visibility.entities();
    hidden_expected.at(provider_id).properties["state"]["overrides"][0]["visible"] = false;
    require(window.document().revision() == before_visibility.revision() + 1 &&
                window.document().snapshot().entities() == hidden_expected && gla() == initial_gla &&
                std::none_of(canvas->entities().begin(), canvas->entities().end(),
                    [&](const auto& item) { return item.id == area; }) &&
                std::none_of(canvas->labels().begin(), canvas->labels().end(),
                    [&](const auto& item) { return item.id == area; }),
            "hiding an area must change only its presentation, remove its canvas shape/name and preserve GLA");
    require(window.undoCommand() && window.document().snapshot().entities() == before_visibility.entities() &&
                gla() == initial_gla && canvas_area().filled,
            "one Undo must restore exact visible appearance and physical appraisal truth");
    auto annotated = styled.entities().at(provider_id);
    annotated.required = true;
    annotated.extensions["vendor"] = "retained";
    annotated.properties["state"]["opaque"] = "retained";
    auto& custom = annotated.properties["state"]["overrides"][0];
    custom["opaque"] = "retained";
    custom["style"]["opaque"] = "retained";
    auto unrelated = custom;
    unrelated["target_kind"] = "object";
    unrelated["target_id"] = symbol.toStdString();
    annotated.properties["state"]["overrides"].push_back(unrelated);
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(annotated)}, {}, "Retain appearance metadata"});
    require(window.selectEntity(area), "refresh area appearance after metadata seed");
    const auto before_edit = window.document().snapshot();
    open([&](QDialog* dialog) {
        auto* pattern = dialog->findChild<QComboBox*>("areaFillPattern");
        pattern->setCurrentIndex(pattern->findData("hatch"));
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    auto expected = before_edit.entities();
    expected.at(provider_id).properties["state"]["overrides"][0]["style"]["fill_pattern"] = "hatch";
    require(window.document().revision() == before_edit.revision() + 1 &&
            window.document().snapshot().entities() == expected && gla() == initial_gla,
            "editing one known style field must preserve required owners, unknown keys and unrelated overrides");
    const auto hatched = window.document().snapshot();
    require(canvas_area().hatch_pattern == "hatch", "hatch edit must reach the retained scene");
    QTemporaryDir directory;
    require(window.saveProjectAs(directory.filePath("area-style.bldproj")) &&
            window.openProject(directory.filePath("area-style.bldproj")) &&
            window.document().snapshot().entities() == hatched.entities(), "area style survives ordinary save/reopen");
    require(window.exportDraftPdf(directory.filePath("area-style.pdf")), "styled plan exports through ordinary PDF command");
    QPdfDocument pdf;
    require(pdf.load(directory.filePath("area-style.pdf")) == QPdfDocument::Error::None && pdf.pageCount() > 0,
            "styled plan PDF reopens");
    const auto page = pdf.render(0, QSize(1400, 1000));
    require(!page.isNull(), "styled plan PDF renders");
    int outline_pixels = 0;
    int fill_pixels = 0;
    for (int y = 0; y < page.height(); ++y) {
        for (int x = 0; x < page.width(); ++x) {
            const auto pixel = page.pixelColor(x, y);
            if (std::abs(pixel.red() - 37) <= 6 && std::abs(pixel.green() - 53) <= 6 &&
                std::abs(pixel.blue() - 69) <= 6) ++outline_pixels;
            // Hatch rasterization varies coverage, so check the chosen
            // #A4D9B0 hue composited onto white across nontrivial coverage.
            // A fixed fully-covered RGB misses correctly antialiased stripes.
            const auto coverage = (255.0-pixel.red())/(255.0-164.0);
            if (coverage >= .04 && coverage <= .30 &&
                std::abs(pixel.green()-(255.0-(255.0-217.0)*coverage)) <= 3 &&
                std::abs(pixel.blue()-(255.0-(255.0-176.0)*coverage)) <= 3) ++fill_pixels;
        }
    }
    const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!captures.isEmpty()) {
        require(QDir().mkpath(captures) && QFile::copy(directory.filePath("area-style.pdf"),
            QDir(captures).filePath("area-style.pdf")), "retain actual area style PDF");
        require(page.save(QDir(captures).filePath("area-style.png")), "retain actual rendered area style page");
    }
    if (outline_pixels <= 50 || fill_pixels <= 50)
        std::cerr << "area style PDF outline pixels=" << outline_pixels << ", fill pixels=" << fill_pixels << '\n';
    require(outline_pixels > 50 && fill_pixels > 50,
            "actual PDF pixels must contain the chosen area outline and fill colors");
    const auto before_invalid = window.document().snapshot();
    open([&](QDialog* dialog) {
        dialog->findChild<QLineEdit*>("areaFillColor")->setText("garbage");
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        require(window.document().revision() == before_invalid.revision() &&
                window.document().snapshot().entities() == before_invalid.entities(), "invalid color must refuse without mutation");
        buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    });
    open([&](QDialog* dialog) {
        const auto current = window.document().snapshot();
        auto changed = current.entities().at(area.toStdString());
        changed.properties["external_note"] = "newer data";
        window.document().apply(ApplyEntityChanges{current.revision(), {EntityChange::upsert(changed)}, {}, "External area edit"});
        const auto newer = window.document().snapshot();
        dialog->findChild<QDoubleSpinBox*>("areaLineWidthMm")->setValue(1.5);
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        require(window.document().revision() == newer.revision() &&
                window.document().snapshot().entities() == newer.entities(), "stale appearance must preserve newer area data");
        buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    });
    require(window.undoCommand() && window.document().snapshot().entities() == hatched.entities(), "restore external edit fixture");
    const auto before_reset = window.document().snapshot();
    open([&](QDialog* dialog) {
        auto* reset = dialog->findChild<QPushButton*>("resetAreaAppearance");
        require(reset, "area appearance must offer Reset to classification defaults");
        reset->click();
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    auto reset_expected = before_reset.entities();
    reset_expected.at(provider_id).properties["state"]["overrides"].erase(
        reset_expected.at(provider_id).properties["state"]["overrides"].begin());
    require(window.document().revision() == before_reset.revision() + 1 &&
            window.document().snapshot().entities() == reset_expected && gla() == initial_gla &&
            window.undoCommand() && window.document().snapshot().entities() == before_reset.entities(),
            "Reset removes only the selected override and one Undo restores all its metadata");
    auto legacy_owner = window.document().snapshot().entities().at(provider_id);
    auto& legacy_override = legacy_owner.properties["state"]["overrides"][0];
    legacy_override.erase("paper_line_width_mm");
    legacy_override.erase("hatch_scale");
    legacy_override["style"]["stroke_width_metres"] = 0.003;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(legacy_owner)}, {}, "Retain legacy area appearance"});
    require(window.selectEntity(area), "refresh the optional-free legacy appearance provider");
    const auto legacy = window.document().snapshot();
    open([&](QDialog* dialog) { buttons(dialog)->button(QDialogButtonBox::Apply)->click(); });
    require(window.document().revision() == legacy.revision() && window.document().snapshot().entities() == legacy.entities(),
            "unchanged legacy appearance must retain absent optional paper width and hatch scale");
    open([&](QDialog* dialog) {
        dialog->findChild<QLineEdit*>("areaOutlineColor")->setText("#456789");
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    auto legacy_expected = legacy.entities();
    legacy_expected.at(provider_id).properties["state"]["overrides"][0]["style"]["stroke_color"] = "#456789";
    require(window.document().revision() == legacy.revision() + 1 &&
                window.document().snapshot().entities() == legacy_expected && gla() == initial_gla &&
                window.undoCommand() && window.document().snapshot().entities() == legacy.entities(),
            "color-only legacy edits preserve absent optional fields, model stroke width and opaque owner data");
    auto duplicate = legacy.entities().at(provider_id);
    duplicate.id = "annotations-duplicate-area-style";
    duplicate.required = false;
    duplicate.properties["state"]["labels"] = nlohmann::json::array();
    duplicate.properties["state"]["symbols"] = nlohmann::json::array();
    duplicate.properties["state"]["overrides"] = nlohmann::json::array({
        legacy.entities().at(provider_id).properties.at("state").at("overrides").at(0)});
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(duplicate)}, {}, "Duplicate provider refusal fixture"});
    require(window.selectEntity(area), "refresh duplicate area presentation providers");
    const auto duplicated = window.document().snapshot();
    appearance->click();
    require(window.lastError().contains("multiple annotation groups") &&
                window.document().revision() == duplicated.revision() &&
                window.document().snapshot().entities() == duplicated.entities() &&
                window.undoCommand() && window.document().snapshot().entities() == legacy.entities(),
            "ambiguous providers must report refusal without choosing or altering either record");
    window.document().mark_read_only("Area style read-only fixture");
    require(window.selectEntity(area) && !appearance->isEnabled(), "read-only areas must disable appearance editing");
}

void appraisal_derived_category_presentation_workflow() {
    using namespace sketch;
    using desktop::MainWindow;
    using desktop::PlanCanvas;

    MainWindow window;
    window.setMetricUnits(false);
    const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture_directory.isEmpty()) {
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.resize(1280, 840);
        window.show();
        QApplication::setActiveWindow(&window);
        QCoreApplication::processEvents();
    }
    const auto area = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(!area.isEmpty() && workflow, "derived presentation fixture needs a boundary and workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "fixture must retain its explicitly saved manual appraisal category");
    save_appraisal_facts_through_controls(window, area, "garage");

    auto* classification = inspector_classification_control(window);
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* derived = window.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"));
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(classification && qualification && derived && canvas,
            "derived presentation fixture needs the inspector, qualification, and retained canvas controls");
    const auto current_canvas_area = [](PlanCanvas* target_canvas, const QString& id) {
        const auto found = std::find_if(target_canvas->entities().begin(), target_canvas->entities().end(),
            [&](const auto& entity) { return entity.id == id; });
        require(found != target_canvas->entities().end(), "derived area must remain in the actual plan canvas");
        return *found;
    };
    const auto garage_style = current_canvas_area(canvas, area);
    const auto garage_facts_state = window.document().snapshot();
    const auto garage_entity = garage_facts_state.entities().at(area.toStdString());
    const auto geometry = garage_entity.properties.at("segments");
    require(garage_entity.properties.at("appraisal_category") == "above_grade_finished" &&
            garage_entity.properties.at("classification") == "measurement" &&
            garage_entity.properties.at("appraisal_facts").at("area_use") == "garage",
            "declared Garage facts must coexist with retained manual Above-grade finished metadata");
    require(qualification->text().startsWith(QStringLiteral("Qualified")) &&
            derived->text().startsWith(QStringLiteral("Garage")),
            "the automatic report must show the facts-derived Garage category");
    // Intended first RED: the report already derives Garage, but the current
    // inspector still exposes the stale manual category as an editable value.
    require(classification->currentText() == QStringLiteral("Garage") && !classification->isEnabled(),
            "a qualified declared area must show its facts-derived category read-only in the inspector");
    const auto before_manual_edit = window.document().snapshot();
    require(!window.editSelectedClassification(QStringLiteral("porch")) &&
            window.document().revision() == before_manual_edit.revision() &&
            window.document().snapshot().entities() == before_manual_edit.entities(),
            "declared facts own the derived category and reject a conflicting manual-category edit");
    require(garage_style.stroke_color == QColor(194, 116, 24) &&
            garage_style.fill_color == QColor(237, 197, 132) &&
            garage_style.filled && garage_style.hatch_pattern == QStringLiteral("forward_diagonal") &&
            std::abs(garage_style.hatch_scale - 0.7) < 1e-12,
            "facts-derived Garage must use the orange hatched plan default");
    if (!capture_directory.isEmpty()) {
        QCoreApplication::processEvents();
        auto* inspector = classification->parentWidget();
        require(inspector && QDir().mkpath(capture_directory) &&
                canvas->grab().save(QDir(capture_directory).filePath("appraisal-derived-garage-canvas.png")) &&
                inspector->grab().save(QDir(capture_directory).filePath("appraisal-derived-garage-inspector.png")),
                "retain the actual Garage plan canvas and selected inspector controls");
    }

    const auto open_appearance = [](MainWindow& host, const QString& boundary_id,
                                    const std::function<void(QDialog*)>& edit) {
        require(host.selectEntity(boundary_id), "select boundary before opening its appearance dialog");
        auto* button = host.findChild<QPushButton*>(QStringLiteral("areaAppearanceButton"));
        if (!button || !button->isEnabled()) {
            std::cerr << "Appearance disabled: selected=" << host.selectedEntityId().toStdString()
                      << " count=" << host.selectedEntityIds().size()
                      << " editable=" << host.document().is_editable()
                      << " reason=" << host.document().read_only_reason()
                      << " error=" << host.lastError().toStdString() << '\n';
        }
        require(button && button->isEnabled(), "editable boundary must expose Area appearance");
        bool seen = false;
        std::exception_ptr callback_failure;
        QTimer::singleShot(0, &host, [&] {
            try {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                require(dialog && dialog->objectName() == QStringLiteral("areaAppearanceDialog"),
                        "appearance action must open the native area dialog");
                seen = true;
                edit(dialog);
            } catch (...) {
                callback_failure = std::current_exception();
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
            }
        });
        button->click();
        if (callback_failure) std::rethrow_exception(callback_failure);
        require(seen, "native appearance dialog callback must run");
    };
    const auto appearance_buttons = [](QDialog* dialog) {
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        require(buttons && buttons->button(QDialogButtonBox::Apply) &&
                    buttons->button(QDialogButtonBox::Cancel),
                "appearance dialog must provide Apply and Cancel");
        return buttons;
    };
    const auto verify_dialog_matches_canvas = [&](MainWindow& host, const QString& boundary_id,
                                                   const desktop::CanvasEntity& expected,
                                                   bool check_garage_palette) {
        open_appearance(host, boundary_id, [&](QDialog* dialog) {
            auto* outline = dialog->findChild<QLineEdit*>(QStringLiteral("areaOutlineColor"));
            auto* fill = dialog->findChild<QLineEdit*>(QStringLiteral("areaFillColor"));
            auto* pattern = dialog->findChild<QComboBox*>(QStringLiteral("areaFillPattern"));
            auto* hatch = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("areaHatchScale"));
            require(outline && fill && pattern && hatch,
                    "appearance defaults must populate outline, fill, pattern, and hatch scale controls");
            const auto expected_pattern = !expected.filled ? QStringLiteral("none")
                : expected.hatch_pattern == QStringLiteral("solid") ? QStringLiteral("solid")
                                                                     : QStringLiteral("hatch");
            require(QColor(outline->text()).rgb() == expected.stroke_color.rgb() &&
                    QColor(fill->text()).rgb() == expected.fill_color.rgb() &&
                    pattern->currentData().toString() == expected_pattern &&
                    std::abs(hatch->value() - (expected.hatch_pattern.contains(QStringLiteral("diagonal")) ? 0.7 : 1.0)) < 1e-12,
                    "appearance dialog defaults must match the facts-derived canvas style");
            if (check_garage_palette)
                require(QColor(outline->text()).rgb() == QColor(194, 116, 24).rgb() &&
                        QColor(fill->text()).rgb() == QColor(237, 197, 132).rgb() &&
                        pattern->currentData().toString() == QStringLiteral("hatch") &&
                        std::abs(hatch->value() - 0.7) < 1e-12,
                        "Garage appearance defaults must be orange and hatched");
            if (check_garage_palette && !capture_directory.isEmpty()) {
                if (dialog->layout()) dialog->layout()->activate();
                dialog->adjustSize();
                QCoreApplication::processEvents();
                require(dialog->grab().save(QDir(capture_directory).filePath("appraisal-derived-garage-appearance.png")),
                        "retain the actual Garage appearance defaults dialog");
            }
            appearance_buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
        });
    };
    verify_dialog_matches_canvas(window, area, garage_style, true);
    require(window.document().snapshot().entities() == garage_facts_state.entities(),
            "reading Garage classification and appearance defaults must not mutate project state");

    save_appraisal_facts_through_controls(window, area, "dwelling");
    const auto dwelling_state = window.document().snapshot();
    const auto dwelling_entity = dwelling_state.entities().at(area.toStdString());
    const auto dwelling_style = current_canvas_area(canvas, area);
    require(dwelling_entity.properties.at("appraisal_category") == "above_grade_finished" &&
            dwelling_entity.properties.at("classification") == "measurement" &&
            dwelling_entity.properties.at("appraisal_facts").at("area_use") == "dwelling" &&
            dwelling_entity.properties.at("segments") == geometry &&
            derived->text().startsWith(QStringLiteral("Above-grade finished (GLA)")) &&
            classification->currentText() == QStringLiteral("Above-grade finished (GLA)") &&
            !classification->isEnabled(),
            "editing facts back to Dwelling must update the derived inspector without rewriting geometry or manual metadata");
    require(dwelling_style.stroke_color != garage_style.stroke_color ||
            dwelling_style.fill_color != garage_style.fill_color ||
            dwelling_style.hatch_pattern != garage_style.hatch_pattern ||
            dwelling_style.filled != garage_style.filled,
            "facts-derived Dwelling must select a different plan default from Garage");
    verify_dialog_matches_canvas(window, area, dwelling_style, false);
    require(window.document().snapshot().entities() == dwelling_state.entities(),
            "reading Dwelling appearance defaults must not mutate project state");

    // A user override remains authoritative in the canvas while the displayed
    // semantic category changes; Reset must then use the latest declared facts.
    open_appearance(window, area, [&](QDialog* dialog) {
        dialog->findChild<QLineEdit*>(QStringLiteral("areaOutlineColor"))->setText(QStringLiteral("#5B2C83"));
        dialog->findChild<QLineEdit*>(QStringLiteral("areaFillColor"))->setText(QStringLiteral("#88CC44"));
        auto* pattern = dialog->findChild<QComboBox*>(QStringLiteral("areaFillPattern"));
        pattern->setCurrentIndex(pattern->findData(QStringLiteral("solid")));
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("areaHatchScale"))->setValue(2.0);
        appearance_buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    const auto explicit_style_state = window.document().snapshot();
    const auto explicit_area = explicit_style_state.entities().at(area.toStdString());
    const auto explicit_style = current_canvas_area(canvas, area);
    require(explicit_area == dwelling_entity &&
            explicit_style.stroke_color == QColor(QStringLiteral("#5B2C83")) &&
            explicit_style.fill_color == QColor(QStringLiteral("#88CC44")) &&
            explicit_style.filled && explicit_style.hatch_pattern == QStringLiteral("solid"),
            "explicit appearance must alter only presentation and remain visible on the canvas");
    require(window.undoCommand() && window.document().snapshot().entities() == dwelling_state.entities() &&
            window.redoCommand() && window.document().snapshot().entities() == explicit_style_state.entities(),
            "explicit area appearance must undo and redo without changing source facts or geometry");

    save_appraisal_facts_through_controls(window, area, "garage");
    const auto garage_override_state = window.document().snapshot();
    require(garage_override_state.entities().at(area.toStdString()).properties.at("appraisal_category") ==
                "above_grade_finished" &&
            garage_override_state.entities().at(area.toStdString()).properties.at("appraisal_facts").at("area_use") == "garage" &&
            garage_override_state.entities().at(area.toStdString()).properties.at("segments") == geometry &&
            current_canvas_area(canvas, area).stroke_color == QColor(QStringLiteral("#5B2C83")) &&
            classification->currentText() == QStringLiteral("Garage") && !classification->isEnabled(),
            "changing facts must update the read-only derived category while preserving explicit appearance and source metadata");
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("facts-derived-area-appearance.bldproj"));
    require(directory.isValid() && window.saveProjectAs(path),
            "facts-derived appearance fixture must save with its explicit override");
    require(window.createNewProject(), "release the saved project's ownership before reopening it for editing");
    MainWindow reopened;
    require(reopened.openProject(path) && reopened.selectEntity(area),
            "facts-derived appearance fixture must reopen");
    auto* reopened_classification = inspector_classification_control(reopened);
    auto* reopened_derived = reopened.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"));
    auto* reopened_canvas = dynamic_cast<PlanCanvas*>(reopened.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(reopened_classification && reopened_derived && reopened_canvas &&
            reopened_classification->currentText() == QStringLiteral("Garage") &&
            !reopened_classification->isEnabled() &&
            reopened_derived->text().startsWith(QStringLiteral("Garage")) &&
            current_canvas_area(reopened_canvas, area).stroke_color == QColor(QStringLiteral("#5B2C83")) &&
            reopened.document().snapshot().entities().at(area.toStdString()).properties.at("appraisal_category") ==
                "above_grade_finished",
            "reopen must preserve explicit presentation and the distinct facts-derived and manual categories");

    const auto before_reset = reopened.document().snapshot();
    open_appearance(reopened, area, [&](QDialog* dialog) {
        auto* outline = dialog->findChild<QLineEdit*>(QStringLiteral("areaOutlineColor"));
        auto* fill = dialog->findChild<QLineEdit*>(QStringLiteral("areaFillColor"));
        auto* pattern = dialog->findChild<QComboBox*>(QStringLiteral("areaFillPattern"));
        auto* reset = dialog->findChild<QPushButton*>(QStringLiteral("resetAreaAppearance"));
        require(outline && fill && pattern && reset,
                "explicit appearance editor must expose its current fields and Reset control");
        require(QColor(outline->text()) == QColor(QStringLiteral("#5B2C83")) &&
                QColor(fill->text()) == QColor(QStringLiteral("#88CC44")),
                "appearance editor must reopen with the persisted explicit override");
        reset->click();
        require(QColor(outline->text()).rgb() == QColor(194, 116, 24).rgb() &&
                QColor(fill->text()).rgb() == QColor(237, 197, 132).rgb() &&
                pattern->currentData().toString() == QStringLiteral("hatch"),
                "Reset must load the current facts-derived Garage defaults, not the stale manual category");
        appearance_buttons(dialog)->button(QDialogButtonBox::Apply)->click();
    });
    const auto reset_state = reopened.document().snapshot();
    require(reset_state.entities().at(area.toStdString()) == before_reset.entities().at(area.toStdString()) &&
            current_canvas_area(reopened_canvas, area).stroke_color == QColor(194, 116, 24) &&
            current_canvas_area(reopened_canvas, area).fill_color == QColor(237, 197, 132) &&
            current_canvas_area(reopened_canvas, area).hatch_pattern == QStringLiteral("forward_diagonal"),
            "Reset must remove only the explicit style and restore the orange Garage default");
    require(reopened.undoCommand() && reopened.document().snapshot().entities() == before_reset.entities() &&
            reopened.redoCommand() && reopened.document().snapshot().entities() == reset_state.entities(),
            "undo and redo around Reset must restore the exact override and facts-derived default states");
    require(reopened.saveProject(), "facts-derived default presentation must save after Reset");
    require(reopened.createNewProject(), "release the reset project's ownership before opening the next editor");
    MainWindow final_reopen;
    require(final_reopen.openProject(path) && final_reopen.selectEntity(area),
            "reset facts-derived presentation must survive another save/reopen");
    auto* final_classification = inspector_classification_control(final_reopen);
    auto* final_derived = final_reopen.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"));
    auto* final_canvas = dynamic_cast<PlanCanvas*>(final_reopen.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(final_classification && final_derived && final_canvas &&
            final_classification->currentText() == QStringLiteral("Garage") && !final_classification->isEnabled() &&
            final_derived->text().startsWith(QStringLiteral("Garage")) &&
            current_canvas_area(final_canvas, area).stroke_color == QColor(194, 116, 24) &&
            current_canvas_area(final_canvas, area).fill_color == QColor(237, 197, 132) &&
            current_canvas_area(final_canvas, area).hatch_pattern == QStringLiteral("forward_diagonal") &&
            final_reopen.document().snapshot().entities().at(area.toStdString()).properties.at("appraisal_category") ==
                "above_grade_finished" &&
            final_reopen.document().snapshot().entities().at(area.toStdString()).properties.at("segments") == geometry,
            "saved reset state must retain Garage-derived presentation, geometry, and manual appraisal metadata");

    // Another incomplete participant withholds aggregate totals, but must not
    // discard this boundary's independently qualified category and defaults.
    const auto incomplete = final_reopen.createBoundary(square(5, 0, 3.048));
    require(!incomplete.isEmpty() && final_reopen.selectEntity(area),
            "partial report fixture needs a second boundary and the qualified Garage selected");
    require(final_reopen.findChild<QLabel*>(QStringLiteral("appraisalQualification"))->text()
                .startsWith(QStringLiteral("Unqualified")) &&
            final_classification->currentText() == QStringLiteral("Garage") &&
            !final_classification->isEnabled() &&
            current_canvas_area(final_canvas, area).stroke_color == QColor(194, 116, 24),
            "aggregate refusal must preserve an individually qualified Garage's derived display");
    require(final_reopen.selectEntity(incomplete) &&
            final_classification->currentText() == QStringLiteral("Unqualified") &&
            !final_classification->isEnabled(),
            "a declared property boundary missing its own facts must show Unqualified read-only");

    MainWindow unqualified;
    const auto unqualified_area = unqualified.createBoundary(square(0, 0, 3.048));
    auto* unqualified_workflow = unqualified.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(!unqualified_area.isEmpty() && unqualified_workflow,
            "unqualified display fixture needs a boundary and workflow selector");
    unqualified_workflow->setCurrentIndex(unqualified_workflow->findData(QStringLiteral("appraisal")));
    require(unqualified.editSelectedClassification(QStringLiteral("above_grade_finished")),
            "unqualified fixture must retain a manual category for comparison");
    save_appraisal_facts_through_controls(unqualified, unqualified_area, "garage", "");
    auto* unqualified_label = unqualified.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* unqualified_derived = unqualified.findChild<QLabel*>(QStringLiteral("appraisalDerivedCategory"));
    auto* unqualified_classification = inspector_classification_control(unqualified);
    require(unqualified_label && unqualified_derived && unqualified_classification &&
            unqualified_label->text().startsWith(QStringLiteral("Unqualified")) &&
            unqualified_derived->text().startsWith(QStringLiteral("Unqualified")) &&
            unqualified_classification->currentText() == QStringLiteral("Unqualified") &&
            !unqualified_classification->isEnabled() &&
            unqualified.document().snapshot().entities().at(unqualified_area.toStdString()).properties.at("appraisal_category") ==
                "above_grade_finished",
            "incomplete declared facts must display Unqualified instead of falling back to a saved category");

    MainWindow legacy;
    const auto legacy_area = legacy.createBoundary(square(0, 0, 3.048));
    require(!legacy_area.isEmpty() && legacy.editSelectedClassification(QStringLiteral("garage")),
            "legacy compatibility fixture must store its old classification before the Appraisal workflow");
    auto* legacy_workflow = legacy.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(legacy_workflow, "legacy compatibility fixture needs the workflow selector");
    legacy_workflow->setCurrentIndex(legacy_workflow->findData(QStringLiteral("appraisal")));
    require(legacy.editSelectedClassification(QStringLiteral("porch")) &&
            legacy.editSelectedClassification(QStringLiteral("garage")),
            "undeclared legacy Appraisal categories must remain manually editable");
    auto* legacy_classification = inspector_classification_control(legacy);
    require(legacy_classification && legacy_classification->isEnabled() && legacy_classification->isEditable() &&
            legacy_classification->currentText() == QStringLiteral("garage") &&
            legacy.document().snapshot().entities().at(legacy_area.toStdString()).properties.at("classification") == "garage" &&
            legacy.document().snapshot().entities().at(legacy_area.toStdString()).properties.at("appraisal_category") == "garage",
            "undeclared legacy classifications must retain the editable manual category migrated by workflow activation");
    save_appraisal_facts_through_controls(legacy, legacy_area, "dwelling");
    require(legacy_classification->currentText() == QStringLiteral("Above-grade finished (GLA)") &&
            !legacy_classification->isEnabled() &&
            legacy.document().snapshot().entities().at(legacy_area.toStdString()).properties.at("classification") == "garage" &&
            legacy.document().snapshot().entities().at(legacy_area.toStdString()).properties.at("appraisal_category") == "garage" &&
            legacy.document().snapshot().entities().at(legacy_area.toStdString()).properties.at("appraisal_facts").at("area_use") == "dwelling",
            "declared Dwelling facts must override a legacy raw Garage label only in derived presentation");
    require(legacy.setSelectedPlanLabelPosition({-1.0, 4.0}),
            "automatic style inheritance fixture needs a manually placed area label");
    const auto inherited_state = legacy.document().snapshot();
    const auto inherited_override = [&] {
        for (const auto& [id, entity] : inherited_state.entities()) {
            if (entity.type != kAnnotationEntityType) continue;
            for (const auto& record : entity.properties.at("state").at("overrides"))
                if (record.at("target_kind") == "area" && record.at("target_id") == legacy_area.toStdString())
                    return record;
        }
        return nlohmann::json{};
    }();
    require(inherited_override.value("inherit_appearance", false) &&
            inherited_override.contains("plan_label_offset_m"),
            "manual label placement must retain automatic category appearance inheritance");
    save_appraisal_facts_through_controls(legacy, legacy_area, "garage");
    auto* legacy_canvas = dynamic_cast<PlanCanvas*>(legacy.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(legacy_canvas && current_canvas_area(legacy_canvas, legacy_area).stroke_color == QColor(194, 116, 24),
            "an inherited label override must still follow the latest Garage default");
    for (const auto& [id, entity] : inherited_state.entities())
        if (entity.type == kAnnotationEntityType)
            require(legacy.document().snapshot().entities().at(id) == entity,
                    "facts-only restyling must preserve inherited annotation state and label placement exactly");
}

void appraisal_area_properties_entry_points_are_visible() {
    using sketch::desktop::MainWindow;
    using sketch::desktop::PlanCanvas;
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1280, 840);
    window.setMetricUnits(false);
    const auto area = window.createBoundary(square(0, 0, 3.048));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(!area.isEmpty() && workflow, "area Properties fixture needs a boundary and workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "area Properties entry points require the actual plan canvas");
    canvas->setSnapEnabled(false);
    canvas->setOverviewMapEnabled(false);
    window.show();
    QApplication::setActiveWindow(&window);
    QCoreApplication::processEvents();
    canvas->fitView();
    require(window.selectEntity(area), "select area before opening its actual Properties quick panel");

    const auto trigger_canvas_context_action = [&](const QString& object_name,
                                                   const QString& label,
                                                   const std::function<void()>& respond_to_modal) {
        bool menu_seen = false;
        bool action_triggered = false;
        std::exception_ptr callback_error;
        QTimer::singleShot(0, &window, [&] {
            try {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                require(menu, "canvas right-click must expose its actual popup menu");
                menu_seen = true;
                const auto actions = menu->actions();
                const auto found = std::find_if(actions.begin(), actions.end(), [&](const QAction* action) {
                    return (!object_name.isEmpty() && action->objectName() == object_name) ||
                           (!label.isEmpty() && action->text() == label);
                });
                require(found != actions.end(), "requested QAction is missing from the area context menu");
                if (respond_to_modal) QTimer::singleShot(0, &window, [&] {
                    try {
                        respond_to_modal();
                    } catch (...) {
                        callback_error = std::current_exception();
                        if (auto* modal = QApplication::activeModalWidget())
                            if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
                    }
                });
                (*found)->trigger();
                action_triggered = true;
                menu->close();
            } catch (...) {
                callback_error = std::current_exception();
                if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) menu->close();
            }
        });
        const QPointF point = QRectF(canvas->rect()).center() + QPointF(
            (1.524 - canvas->viewCenter().x) * canvas->viewScale(),
            -(1.524 - canvas->viewCenter().y) * canvas->viewScale());
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas->mapToGlobal(point.toPoint()),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, canvas->mapToGlobal(point.toPoint()),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
        if (callback_error) std::rethrow_exception(callback_error);
        require(menu_seen && action_triggered,
                "actual area context action must be found and triggered");
    };
    trigger_canvas_context_action({}, QStringLiteral("Properties"), {});
    QCoreApplication::processEvents();

    auto* editor = window.findChild<QScrollArea*>(QStringLiteral("contextEditor"));
    require(editor && editor->isVisible(), "Properties action must show the contextual quick panel");
    const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!captures.isEmpty())
        require(QDir().mkpath(captures) &&
                    editor->grab().save(QDir(captures).filePath(QStringLiteral("appraisal-area-properties-initial.png"))),
                "retain the actual initial contextual Properties panel");
    auto* attributes = window.findChild<QPushButton*>(QStringLiteral("editAreaAttributes"));
    auto* attributes_summary = window.findChild<QLabel*>(QStringLiteral("areaAttributesSummary"));
    require(attributes && attributes->isEnabled() && attributes_summary && attributes_summary->isVisible(),
            "selected area Properties must expose its custom-details summary and structured Name/Value editor");

    auto* facts = window.findChild<QPushButton*>(QStringLiteral("editAppraisalFacts"));
    require(facts && facts->isVisible() && editor->verticalScrollBar()->value() == 0,
            "Appraisal facts action must be visible at the initial Properties scroll position");
    const QRect facts_in_viewport(facts->mapTo(editor->viewport(), QPoint(0, 0)), facts->size());
    if (!editor->viewport()->rect().contains(facts_in_viewport)) {
        std::cerr << "Facts rectangle " << facts_in_viewport.x() << ',' << facts_in_viewport.y()
                  << ' ' << facts_in_viewport.width() << 'x' << facts_in_viewport.height()
                  << "; viewport " << editor->viewport()->width() << 'x' << editor->viewport()->height() << '\n';
        for (auto* widget : editor->widget()->findChildren<QWidget*>())
            if (widget->isVisible() && widget->minimumSizeHint().width() > 280)
                std::cerr << widget->metaObject()->className() << ' '
                          << widget->objectName().toStdString() << " minhint="
                          << widget->minimumSizeHint().width() << " min=" << widget->minimumWidth() << '\n';
        const auto diagnostic_dir = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!diagnostic_dir.isEmpty()) editor->grab().save(QDir(diagnostic_dir).filePath(
            QStringLiteral("appraisal-properties-facts-outside.png")));
    }
    require(editor->viewport()->rect().contains(facts_in_viewport),
            "Edit appraisal facts must appear directly in the unscrolled contextual Properties viewport");

    const auto before_cancel = window.document().snapshot();
    edit_appraisal_facts_through_controls(window, area, "dwelling", "finished",
        [&] { facts->click(); }, false);
    require(window.document().snapshot().revision() == before_cancel.revision() &&
                window.document().snapshot().history().size() == before_cancel.history().size() &&
                window.document().snapshot().entities() == before_cancel.entities(),
            "Cancel in the real Properties facts action must preserve facts and command history");

    // The selected-area right-click menu must reuse the same QAction as the
    // Properties button. Cancel it after the nested dialog actually opens.
    bool menu_facts_opened = false;
    trigger_canvas_context_action(QStringLiteral("editAppraisalFactsAction"), {}, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("appraisalFactsDialog"));
        require(dialog, "area right-click facts action must open the native facts dialog");
        auto* box = dialog->findChild<QDialogButtonBox*>();
        require(box && box->button(QDialogButtonBox::Cancel), "native facts dialog must expose Cancel");
        menu_facts_opened = true;
        box->button(QDialogButtonBox::Cancel)->click();
    });
    require(menu_facts_opened && window.document().snapshot().entities() == before_cancel.entities(),
            "right-click Edit appraisal facts must open the same cancelable native dialog without mutation");

    edit_appraisal_facts_through_controls(window, area, "dwelling", "finished",
        [&] { facts->click(); });
    const auto after_facts = window.document().snapshot();
    auto* declared_category = inspector_classification_control(window);
    require(declared_category && declared_category->currentText() == QStringLiteral("Above-grade finished (GLA)") &&
                declared_category->currentData().toString() == QStringLiteral("above_grade_finished"),
            "the declared category must use a readable label while retaining its semantic key");
    QCoreApplication::processEvents();
    if (!captures.isEmpty())
        require(editor->grab().save(QDir(captures).filePath(QStringLiteral("appraisal-area-properties-qualified.png"))),
                "retain the qualified compact appraisal summary");
    for (const auto* name : {"appraisalGlaTotal", "appraisalPropertyTotal"}) {
        auto* value = window.findChild<QLabel*>(QString::fromLatin1(name));
        if (value && !editor->viewport()->rect().contains(
                QRect(value->mapTo(editor->viewport(), QPoint{}), value->size()))) {
            const auto bounds = QRect(value->mapTo(editor->viewport(), QPoint{}), value->size());
            std::cerr << name << " rect " << bounds.x() << ',' << bounds.y() << ' '
                      << bounds.width() << 'x' << bounds.height() << '\n';
            for (auto* widget : editor->widget()->findChildren<QWidget*>())
                if (widget->isVisible() && widget->minimumSizeHint().width() > 260)
                    std::cerr << widget->metaObject()->className() << ' '
                              << widget->objectName().toStdString() << " minhint="
                              << widget->minimumSizeHint().width() << " title="
                              << widget->property("title").toString().toStdString() << '\n';
        }
        require(value && editor->viewport()->rect().contains(
                    QRect(value->mapTo(editor->viewport(), QPoint{}), value->size())),
                "qualified primary appraisal totals must fit the unscrolled Properties viewport");
    }
    require(window.findChild<QLabel*>(QStringLiteral("appraisalQualification"))->text().startsWith(
                QStringLiteral("Qualified")) &&
            window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"))->text().contains(
                QStringLiteral("100.00")) &&
            after_facts.entities().at(area.toStdString()).properties.at("appraisal_facts").at("area_use") == "dwelling",
            "the quick-panel action must open the native fact editor and save the declared Dwelling facts");
    require(window.undoCommand() && window.document().snapshot().entities() == before_cancel.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == after_facts.entities(),
            "one Undo and Redo must restore the exact facts saved from Properties");

    auto* detail_toggle = window.findChild<QAbstractButton*>(QStringLiteral("appraisalDetailsToggle"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    auto* property = window.findChild<QLabel*>(QStringLiteral("appraisalPropertyTotal"));
    auto* garage = window.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"));
    require(detail_toggle && detail_toggle->isCheckable() && gla && property && garage &&
                !detail_toggle->isChecked() && !garage->isVisible(),
            "Properties must begin with a compact appraisal summary and collapsed extended totals");
    const auto before_details = window.document().snapshot();
    const auto summary_values = std::pair{gla->text(), property->text()};
    detail_toggle->click();
    require(detail_toggle->isChecked() && garage->isVisible() && gla->text() == summary_values.first &&
                property->text() == summary_values.second &&
                window.document().snapshot().entities() == before_details.entities() &&
                window.document().snapshot().revision() == before_details.revision(),
            "expanding appraisal Details must reveal retained totals without changing summary values or project state");
    detail_toggle->click();
    require(!detail_toggle->isChecked() && !garage->isVisible(),
            "the compact appraisal summary toggle must collapse extended totals again");

    auto* details_button = window.findChild<QPushButton*>(QStringLiteral("editAreaAttributes"));
    require(details_button && details_button->isEnabled(), "selected area must retain its structured Details action");
    const auto with_attributes_dialog = [&](const std::function<void(
        QDialog&, QTableWidget&, QLabel&, QDialogButtonBox&)>& interact) {
        require(window.selectEntity(area), "select area before opening structured Details");
        trigger_canvas_context_action({}, QStringLiteral("Properties"), {});
        details_button = window.findChild<QPushButton*>(QStringLiteral("editAreaAttributes"));
        require(details_button && details_button->isEnabled(), "area Details action must be enabled");
        bool completed = false;
        std::exception_ptr callback_error;
        QTimer::singleShot(0, &window, [&] {
            try {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                require(dialog && dialog->objectName() == QStringLiteral("areaAttributesDialog"),
                        "Details action must open the native structured-attributes dialog");
                auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("areaAttributesTable"));
                auto* validation = dialog->findChild<QLabel*>(QStringLiteral("areaAttributesValidation"));
                auto* box = dialog->findChild<QDialogButtonBox*>();
                require(table && table->columnCount() == 2 && table->horizontalHeaderItem(0) &&
                            table->horizontalHeaderItem(1) &&
                            table->horizontalHeaderItem(0)->text() == QStringLiteral("Name") &&
                            table->horizontalHeaderItem(1)->text() == QStringLiteral("Value") &&
                            validation && box && box->button(QDialogButtonBox::Save) &&
                            box->button(QDialogButtonBox::Cancel) &&
                            dialog->findChild<QPushButton*>(QStringLiteral("areaAttributeAdd")) &&
                            dialog->findChild<QPushButton*>(QStringLiteral("areaAttributeRemove")),
                        "Details editor must expose its Name/Value table, Add/Remove, validation, Save and Cancel controls");
                interact(*dialog, *table, *validation, *box);
                completed = true;
            } catch (...) {
                callback_error = std::current_exception();
                if (auto* modal = QApplication::activeModalWidget())
                    if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
            }
        });
        details_button->click();
        if (callback_error) std::rethrow_exception(callback_error);
        require(completed, "structured Details dialog callback must complete");
    };
    const auto set_cell = [](QTableWidget& table, int row, int column, const QString& value) {
        auto* item = table.item(row, column);
        require(item, "Add must create editable Name and Value cells");
        // Add begins editing Name; move focus out before changing the backing
        // item so the delegate cannot overwrite the test's value on close.
        table.setCurrentCell(row, column == 0 ? 1 : 0);
        item->setText(value);
    };
    const auto append_attribute = [&](QTableWidget& table, QPushButton& add,
                                      const QString& name, const QString& value) {
        const int before = table.rowCount();
        add.click();
        require(table.rowCount() == before + 1 && table.currentRow() == before && table.currentColumn() == 0,
                "Add must append one row and focus its Name cell");
        set_cell(table, before, 0, name);
        set_cell(table, before, 1, value);
    };
    const auto attributes_before = window.document().snapshot();
    const auto gla_before_attributes = gla->text();
    with_attributes_dialog([&](QDialog&, QTableWidget&, QLabel&, QDialogButtonBox& box) {
        require(box.button(QDialogButtonBox::Save)->isEnabled(), "unchanged structured details may be saved as a no-op");
        box.button(QDialogButtonBox::Save)->click();
    });
    require(window.document().snapshot().revision() == attributes_before.revision() &&
                window.document().snapshot().history().size() == attributes_before.history().size() &&
                window.document().snapshot().entities() == attributes_before.entities(),
            "saving unchanged structured attributes must not add a revision, history entry, or normalized metadata");
    with_attributes_dialog([&](QDialog& dialog, QTableWidget& table, QLabel&, QDialogButtonBox& box) {
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("areaAttributeAdd"));
        append_attribute(table, *add, QStringLiteral("discard"), QStringLiteral("cancelled"));
        box.button(QDialogButtonBox::Cancel)->click();
    });
    require(window.document().snapshot().entities() == attributes_before.entities() &&
                window.document().snapshot().revision() == attributes_before.revision(),
            "Cancel after editing a real Name/Value row must leave the document untouched");

    with_attributes_dialog([&](QDialog& dialog, QTableWidget& table, QLabel& validation,
                               QDialogButtonBox& box) {
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("areaAttributeAdd"));
        append_attribute(table, *add, QStringLiteral("stale"), QStringLiteral("must not commit"));
        const auto current = window.document().snapshot();
        auto external = current.entities().at(area.toStdString());
        external.properties["external_note"] = "newer project revision";
        window.document().apply(sketch::ApplyEntityChanges{current.revision(),
            {sketch::EntityChange::upsert(external)}, {}, "external edit during area details"});
        const auto newer = window.document().snapshot();
        box.button(QDialogButtonBox::Save)->click();
        require(!validation.text().trimmed().isEmpty() &&
                    window.document().snapshot().revision() == newer.revision() &&
                    window.document().snapshot().entities() == newer.entities(),
                "a stale Details dialog must explain refusal and preserve the newer project revision");
        box.button(QDialogButtonBox::Cancel)->click();
    });
    require(window.undoCommand() && window.document().snapshot().entities() == attributes_before.entities(),
            "Undo must restore the intentional external edit after stale Details refuses to overwrite it");

    const auto& selection_fixture = attributes_before.entities().at(area.toStdString());
    const auto floor_id = selection_fixture.properties.at("floor_id").get<std::string>();
    const auto building_id = attributes_before.entities().at(floor_id).properties.at("building_id").get<std::string>();
    const auto property_id = attributes_before.entities().at(building_id).properties.at("property_id").get<std::string>();
    with_attributes_dialog([&](QDialog& dialog, QTableWidget& table, QLabel& validation,
                               QDialogButtonBox& box) {
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("areaAttributeAdd"));
        append_attribute(table, *add, QStringLiteral("wrong-target"), QStringLiteral("must not commit"));
        require(window.selectEntity(QString::fromStdString(property_id)),
                "selection-refusal fixture must change the selected entity while Details is open");
        box.button(QDialogButtonBox::Save)->click();
        require(!validation.text().trimmed().isEmpty() &&
                    window.document().snapshot().entities() == attributes_before.entities(),
                "a Details dialog opened for another selection must refuse without changing the project");
        box.button(QDialogButtonBox::Cancel)->click();
    });

    const auto attributes_save_base = window.document().snapshot();
    with_attributes_dialog([&](QDialog& dialog, QTableWidget& table, QLabel& validation,
                               QDialogButtonBox& box) {
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("areaAttributeAdd"));
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("areaAttributeRemove"));
        auto* save = box.button(QDialogButtonBox::Save);
        append_attribute(table, *add, {}, QStringLiteral("value without a name"));
        QCoreApplication::processEvents();
        require(!save->isEnabled() && !validation.text().trimmed().isEmpty(),
                "an empty Name must show inline validation and disable Save");
        require(table.item(0, 1)->text() == QStringLiteral("value without a name"),
                "empty-Name validation must preserve the user's entered Value");
        set_cell(table, 0, 0, QStringLiteral("zone"));
        require(save->isEnabled(), "a nonempty Name and bounded Value must restore Save");
        append_attribute(table, *add, QStringLiteral("zone"), QStringLiteral("duplicate"));
        QCoreApplication::processEvents();
        require(!save->isEnabled() && !validation.text().trimmed().isEmpty(),
                "duplicate Names must show validation and disable Save");
        if (!captures.isEmpty())
            require(QDir().mkpath(captures) &&
                        dialog.grab().save(QDir(captures).filePath(QStringLiteral("appraisal-area-attributes-invalid.png"))),
                    "retain the actual populated duplicate Name/Value dialog");
        set_cell(table, 1, 0, QString(129, QChar(0x00e9)));
        require(!save->isEnabled() && !validation.text().trimmed().isEmpty(),
                "UTF-8 Name length must be bounded in bytes, not code points");
        set_cell(table, 1, 0, QStringLiteral("note"));
        set_cell(table, 1, 1, QString(16385, QLatin1Char('v')));
        require(!save->isEnabled() && !validation.text().trimmed().isEmpty(),
                "UTF-8 Value length above 16,384 bytes must disable Save");
        set_cell(table, 1, 1, QStringLiteral("retained"));
        require(save->isEnabled(), "correcting invalid fields must make the dialog saveable");
        append_attribute(table, *add, QStringLiteral("discard"), QStringLiteral("remove me"));
        table.setCurrentCell(2, 0);
        remove->click();
        require(table.rowCount() == 2 && table.item(0, 0)->text() == QStringLiteral("zone") &&
                    table.item(1, 0)->text() == QStringLiteral("note"),
                "Remove must delete only the selected row while preserving edited rows");
        set_cell(table, 0, 1, QStringLiteral("north wing"));
        box.button(QDialogButtonBox::Save)->click();
    });
    auto attributes_saved = window.document().snapshot();
    auto expected_entities = attributes_save_base.entities();
    expected_entities.at(area.toStdString()).properties["area_attributes"] =
        nlohmann::json{{"zone", "north wing"}, {"note", "retained"}};
    require(attributes_saved.revision() == attributes_save_base.revision() + 1 &&
                attributes_saved.entities() == expected_entities &&
                attributes_saved.entities().at(area.toStdString()).properties.at("segments") ==
                    attributes_before.entities().at(area.toStdString()).properties.at("segments") &&
                attributes_saved.entities().at(area.toStdString()).properties.at("appraisal_facts") ==
                    attributes_before.entities().at(area.toStdString()).properties.at("appraisal_facts") &&
                window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"))->text() == gla_before_attributes,
            "structured Save must update only area_attributes and preserve geometry, appraisal facts and totals");
    require(window.undoCommand() && window.document().snapshot().entities() == attributes_before.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == attributes_saved.entities(),
            "one Undo/Redo must remove and restore the exact structured attribute map");

    // Exercise the existing editor limit through actual Add clicks without
    // writing the oversized map to the document.
    with_attributes_dialog([&](QDialog& dialog, QTableWidget& table, QLabel&, QDialogButtonBox& box) {
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("areaAttributeAdd"));
        while (table.rowCount() < 256 && add->isEnabled()) {
            const auto row = table.rowCount();
            append_attribute(table, *add, QStringLiteral("entry-%1").arg(row), QStringLiteral("v"));
        }
        require(table.rowCount() == 256, "Details editor must permit exactly 256 rows");
        if (add->isEnabled()) add->click();
        require(table.rowCount() == 256 || (!box.button(QDialogButtonBox::Save)->isEnabled() &&
                    table.rowCount() == 257),
                "attempting to add a 257th row must be refused or remain unsaveable");
        box.button(QDialogButtonBox::Cancel)->click();
    });
    require(window.document().snapshot().entities() == attributes_saved.entities(),
            "row-limit validation must not change persisted area metadata");

    QTemporaryDir directory;
    const auto project_path = directory.filePath(QStringLiteral("area-properties.bldproj"));
    require(directory.isValid() && window.saveProjectAs(project_path) && window.createNewProject(),
            "structured area metadata fixture must save and release its project before reopening");
    MainWindow reopened;
    require(reopened.openProject(project_path) && reopened.document().snapshot().entities() == attributes_saved.entities() &&
                reopened.selectEntity(area) && reopened.document().snapshot().entities().at(area.toStdString())
                    .properties.at("area_attributes") == nlohmann::json{{"zone", "north wing"}, {"note", "retained"}},
            "native save/reopen must preserve the exact Name/Value metadata and source entities");
}

void malformed_appraisal_boundary_has_no_stale_presentation() {
    using namespace sketch;
    desktop::MainWindow window;
    const auto area = window.createBoundary(square(0, 0, 3.048), QStringLiteral("garage"));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(!area.isEmpty() && workflow, "malformed presentation fixture needs its legacy Garage");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()), "declare facts before damaging hierarchy");
    const auto before = window.document().snapshot();
    auto malformed = before.entities().at(area.toStdString());
    const auto floor = before.entities().at(malformed.properties.at("floor_id").get<std::string>());
    const auto building = before.entities().at(floor.properties.at("building_id").get<std::string>());
    const auto property_id = building.properties.at("property_id").get<std::string>();
    malformed.properties["property_id"] = property_id;
    malformed.properties.erase("floor_id");
    malformed.properties.erase("building_id");
    window.document().apply(ApplyEntityChanges{before.revision(),
        {EntityChange::upsert(malformed)}, {}, "malformed ownership presentation fixture"});
    const auto report = build_appraisal_document_report(window.document().snapshot(), property_id);
    require(!report.qualified && std::any_of(report.boundaries.begin(), report.boundaries.end(),
                [&](const auto& row) {
                    return row.boundary_id == area.toStdString() && !row.qualification.qualified && !row.measurement;
                }), "malformed known-property boundary must remain a diagnostic report row");
    require(window.selectEntity(area), "malformed boundary must remain selectable for repair");
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "malformed presentation must use the actual retained canvas");
    const auto item = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [&](const auto& value) { return value.id == area; });
    require(item != canvas->entities().end(), "malformed boundary geometry must remain visible for repair");
    require(item->stroke_color != QColor(194, 116, 24) && !item->filled,
            "no-floor/no-building declared boundary must not recover stale raw Garage styling");
    require(window.document().snapshot().entities().at(area.toStdString()) == malformed,
            "diagnostic presentation must not repair or rewrite source metadata");
}

void appraisal_area_display_precision_workflow() {
    sketch::desktop::MainWindow window;
    window.setMetricUnits(false);
    const auto side = std::sqrt(151.51 * 0.09290304);
    const auto first = window.createBoundary(square(0, 0, side));
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow, "area display fixture needs the appraisal workflow selector");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(!first.isEmpty() && window.editSelectedAppraisalFacts(declarations()),
            "area display fixture must qualify its first 151.51 square foot room");
    const auto second = window.createBoundary(square(6, 0, side));
    require(!second.isEmpty() && window.editSelectedAppraisalFacts(declarations()),
            "area display fixture must qualify its second nonoverlapping room");
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(gla && gla->text().contains(QStringLiteral("303.02")),
            "area display fixture must start with the exact aggregate shown at two decimals");
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "automatic area labels need the actual measurement canvas");
    const auto area_label = [&](const QString& id) {
        const auto label = std::find_if(canvas->labels().begin(), canvas->labels().end(),
            [&](const auto& item) { return item.id == id && item.plan_only && item.avoid_components; });
        return label == canvas->labels().end() ? QString{} : label->text;
    };
    require(area_label(first).contains(QStringLiteral("151.51 ft²")) &&
            area_label(second).contains(QStringLiteral("151.51 ft²")),
            "qualified areas must automatically show their calculated square footage on the plan");
    auto* action = window.findChild<QAction*>(QStringLiteral("calculationProfile"));
    require(action && action->isEnabled(),
            "qualified Appraisal must offer its Area display dialog");

    // Opaque profile and entity metadata must survive a display-only edit.
    auto seeded = window.document().snapshot();
    auto property = seeded.entities().at("property-1");
    property.properties["calculation_profile"]["vendor_rounding_note"] = {{"original", "retain verbatim"}};
    property.properties["calculation_profile"]["classifications"]["above_grade_finished"]["vendor_rule"] = "retain";
    // Hidden legacy Measurement fields cannot override or block Appraisal policy.
    property.properties["calculation_profile"]["id"] = nullptr;
    property.properties["calculation_profile"]["display_unit"] = "unknown-legacy-unit";
    property.properties["calculation_profile"]["classifications"]["garage"] = "opaque legacy rule";
    property.extensions["vendor_area_display"] = {{"value", 17}};
    window.document().apply(sketch::ApplyEntityChanges{
        seeded.revision(), {sketch::EntityChange::upsert(property)}, {}, "Seed opaque area display metadata"});
    require(window.selectEntity(second) && gla->text() == QStringLiteral("303.02 ft²") && action->isEnabled(),
            "hidden legacy profile metadata must not block qualified Appraisal totals or display editing");
    const auto original = window.document().snapshot();
    const auto physical_report = sketch::build_appraisal_document_report(original, "property-1");
    require(physical_report.qualified && physical_report.calculation &&
                std::abs(physical_report.calculation->property.gla().total.square_metres - 303.02 * 0.09290304) < 1e-12,
            "area display source must independently contain the unrounded aggregate in SI");
    const auto quantities = [&](const sketch::DocumentScheduleProjection& projection) {
        std::map<std::string, sketch::ScheduleQuantity> result;
        for (const auto& row : projection.snapshot.rows) {
            const auto cell = row.cells.find("area");
            if (row.kind == sketch::ScheduleRowKind::appraisal && cell != row.cells.end()) {
                require(std::holds_alternative<sketch::ScheduleQuantity>(cell->second.value),
                        "appraisal area schedule cells must retain typed quantities");
                result.emplace(row.object_id, std::get<sketch::ScheduleQuantity>(cell->second.value));
            }
        }
        require(!result.empty(), "area display fixture needs generated appraisal quantities");
        return result;
    };
    const auto original_quantities = quantities(window.scheduleSnapshot());
    const auto edit_precision = [&](int precision, bool save, bool tamper_policy = false,
                                    const std::function<void()>& while_open = {}) {
        bool seen = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("calculationProfileDialog"));
            require(dialog && dialog->windowTitle() == QStringLiteral("Area display"),
                    "Appraisal must open its phase-specific Area display dialog");
            seen = true;
            auto* decimals = dialog->findChild<QSpinBox*>(QStringLiteral("calculationProfileDecimals"));
            require(decimals && decimals->minimum() == 0 && decimals->maximum() == 6 && decimals->isVisibleTo(dialog),
                    "Area display must expose only a bounded decimal precision input");
            for (const auto* name : {"calculationProfileId", "calculationProfileDisplayUnit",
                                    "calculationProfileClassifications", "addCalculationClassification",
                                    "removeCalculationClassification"}) {
                auto* control = dialog->findChild<QWidget*>(QString::fromLatin1(name));
                require(!control || !control->isVisibleTo(dialog),
                        "built-in appraisal identity, units and classification rules must stay hidden");
            }
            if (tamper_policy) {
                if (auto* id = dialog->findChild<QLineEdit*>(QStringLiteral("calculationProfileId")))
                    id->setText(QStringLiteral("must-not-replace-appraisal-policy"));
                if (auto* unit = dialog->findChild<QComboBox*>(QStringLiteral("calculationProfileDisplayUnit")))
                    unit->setCurrentIndex(unit->findData(QStringLiteral("acre")));
                if (auto* classes = dialog->findChild<QTableWidget*>(QStringLiteral("calculationProfileClassifications"));
                    classes && classes->rowCount() > 0 && classes->item(0, 0))
                    classes->item(0, 0)->setText(QStringLiteral("must-not-replace-fixed-rule"));
            }
            decimals->setValue(precision);
            if (while_open) while_open();
            const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
            if (tamper_policy && !captures.isEmpty()) {
                if (dialog->layout()) dialog->layout()->activate();
                dialog->adjustSize();
                QApplication::processEvents();
                require(QDir().mkpath(captures) && dialog->grab().save(QDir(captures).filePath("appraisal-area-display.png")),
                        "compact Area display capture must save");
            }
            auto* button = dialog->findChild<QPushButton*>(save ? QStringLiteral("saveCalculationProfile")
                                                               : QStringLiteral("cancelCalculationProfile"));
            require(button && button->isEnabled(), "Area display must expose its enabled native action");
            button->click();
            // The custom Measurement editor keeps Save open; either modal
            // closing policy is compatible with a successful display edit.
            if (dialog->isVisible()) dialog->reject();
        });
        action->trigger();
        require(seen, "Area display action must invoke the actual modal settings workflow");
    };
    const auto check_precision = [&](unsigned precision, const QString& displayed) {
        require(gla->text() == displayed + QStringLiteral(" ft²"),
                "inspector must round the unrounded aggregate once using selected decimal places");
        const auto report = sketch::build_appraisal_document_report(window.document().snapshot(), "property-1");
        require(report.qualified && report.calculation && report.display_decimal_places == precision &&
                    report.calculation->property.gla().total.square_metres ==
                        physical_report.calculation->property.gla().total.square_metres,
                "display settings must change report precision without changing physical totals");
        const auto expected_area = QString::fromStdString(
            report.calculation->calculation.areas.front().display.text) + QStringLiteral(" ft²");
        require(area_label(first).contains(expected_area) && area_label(second).contains(expected_area),
                "automatic canvas area labels must follow the same persisted precision as the report");
        const auto schedule = window.scheduleSnapshot();
        require(quantities(schedule) == original_quantities,
                "display settings must leave every canonical appraisal schedule quantity unchanged");
        for (const auto& row : schedule.snapshot.rows) {
            if (row.kind == sketch::ScheduleRowKind::appraisal && row.cells.contains("area"))
                require(row.cells.at("area").display_decimal_places == precision,
                        "generated appraisal area cells must carry selected display precision");
        }
    };

    edit_precision(0, true, true);
    const auto zero = window.document().snapshot();
    auto expected_property = original.entities().at("property-1");
    auto& expected_profile = expected_property.properties["calculation_profile"];
    expected_profile["decimal_places"] = 0U;
    expected_profile["version"] = expected_profile.at("version").get<unsigned>() + 1U;
    require(zero.revision() == original.revision() + 1 && zero.entities().size() == original.entities().size() &&
                zero.entities().at("property-1") == expected_property,
            "precision save must change only decimals/version and preserve exact fixed policy and opaque metadata");
    for (const auto& [id, entity] : original.entities()) {
        if (id != "property-1") require(zero.entities().at(id) == entity,
                                      "Area display must never rewrite geometry, annotations or organization");
    }
    check_precision(0, QStringLiteral("303"));
    require(window.undoCommand() && window.document().snapshot().entities() == original.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == zero.entities(),
            "Area display must undo and redo its one atomic settings command");

    const auto show_schedule = [&](const QString& displayed) {
        QAction* schedules = nullptr;
        for (auto* candidate : window.findChildren<QAction*>())
            if (candidate->text() == QStringLiteral("Schedules")) schedules = candidate;
        require(schedules && schedules->isEnabled(), "native schedules action must be available");
        const auto projection = window.scheduleSnapshot();
        const auto row = std::find_if(projection.snapshot.rows.begin(), projection.snapshot.rows.end(), [](const auto& item) {
            return item.kind == sketch::ScheduleRowKind::appraisal && item.object_id.ends_with(":category:above_grade_finished");
        });
        require(row != projection.snapshot.rows.end(), "native schedule fixture needs its GLA row");
        bool seen = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* table = dialog ? dialog->findChild<QTableWidget*>(QStringLiteral("scheduleTable")) : nullptr;
            require(table, "Schedules action must open the actual native table");
            int area_column = -1;
            for (int column = 0; column < table->columnCount(); ++column)
                if (table->horizontalHeaderItem(column)->text() == QStringLiteral("area")) area_column = column;
            const auto row_index = static_cast<int>(std::distance(projection.snapshot.rows.begin(), row));
            require(area_column >= 0 && table->item(row_index, area_column) &&
                        table->item(row_index, area_column)->text() == displayed + QStringLiteral(" ft²"),
                    "native schedule table must honor workspace units and selected area precision");
            seen = true;
            dialog->reject();
        });
        schedules->trigger();
        require(seen, "native schedule precision must be observed through its actual dialog");
    };
    show_schedule(QStringLiteral("303"));
    edit_precision(1, true);
    check_precision(1, QStringLiteral("303.0"));
    show_schedule(QStringLiteral("303.0"));
    edit_precision(6, true);
    check_precision(6, QStringLiteral("303.020000"));
    show_schedule(QStringLiteral("303.020000"));
    const auto six = window.document().snapshot();
    edit_precision(0, false);
    edit_precision(6, true);
    require(window.document().revision() == six.revision() && window.document().snapshot().entities() == six.entities(),
            "Cancel and no-op Save must preserve display configuration and command history");

    QTemporaryDir directory;
    require(directory.isValid() && window.saveProjectAs(directory.filePath(QStringLiteral("area-display.bldproj"))) &&
                window.openProject(directory.filePath(QStringLiteral("area-display.bldproj"))) &&
                window.document().snapshot().entities() == six.entities() && window.selectEntity(second),
            "Area display configuration and exact quantities must survive ordinary save/reopen");
    check_precision(6, QStringLiteral("303.020000"));

    auto sheet_snapshot = window.document().snapshot();
    const auto sheet_entity = std::find_if(sheet_snapshot.entities().begin(), sheet_snapshot.entities().end(), [](const auto& item) {
        return item.second.type == sketch::kSheetViewEntityType;
    });
    require(sheet_entity != sheet_snapshot.entities().end(), "Area display export needs a sheet model");
    auto model = sketch::decode_sheet_view_entity(sheet_entity->second);
    sketch::DrawingSheet report_sheet;
    report_sheet.id = "sheet-area-display";
    report_sheet.number = "A-901";
    report_sheet.width_mm = 420;
    report_sheet.height_mm = 297;
    report_sheet.title_block = {"Area display fixture", "Appraisal area summary", "", ""};
    report_sheet.schedules.push_back({"area-display-summary", "appraisal-areas", {10, 10, 400, 250}});
    const auto old_sheet = model.sheets().front().id;
    model = model.with_added_sheet(std::move(report_sheet)).with_removed_sheet(old_sheet);
    window.document().apply(sketch::ApplyEntityChanges{sheet_snapshot.revision(),
        {sketch::EntityChange::upsert(sketch::make_sheet_view_entity(sheet_entity->first, model))}, {},
        "Add native Area display export sheet"});
    const auto export_precision = [&](const QString& expected, const QString& file) {
        const auto path = directory.filePath(file);
        require(window.exportDrawingSetPdf(path), "Area display appraisal sheet must export through the normal PDF workflow");
        const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!captures.isEmpty()) require(QFile::copy(path, QDir(captures).filePath(file)),
            "retain the actual area display PDF for inspection");
        QPdfDocument pdf;
        require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
                "Area display PDF must reopen as its single report sheet");
        const auto raw_text = pdf.getAllText(0).text();
        const auto text = raw_text.simplified();
        if (!text.contains(QStringLiteral("Above-grade finished (GLA)")) ||
            !text.contains(expected + QStringLiteral(" ft²")))
            throw std::runtime_error((QStringLiteral("Actual PDF must show %1 ft²; extracted text: %2")
                .arg(expected, text)).toStdString());
        require(text.contains(QStringLiteral("A-901")) &&
                !text.contains(QRegularExpression(QStringLiteral("[\\x{E000}-\\x{F8FF}]"))),
                "sheet PDF punctuation must copy as authored Unicode rather than private-use glyphs");
        const auto amount_index = raw_text.indexOf(expected);
        const auto amount = pdf.getSelectionAtIndex(0, amount_index, expected.size());
        require(amount_index >= 0 && amount.isValid() && amount.boundingRectangle().height() > 0 &&
                amount.boundingRectangle().height() <= 12.0,
                "sheet body text must remain near its 8pt physical size at export resolution");
        return text;
    };
    export_precision(QStringLiteral("303.020000"), QStringLiteral("area-display-six.pdf"));
    edit_precision(0, true);
    const auto whole_text = export_precision(QStringLiteral("303"), QStringLiteral("area-display-zero.pdf"));
    require(!whole_text.contains(QStringLiteral("303.02")) && !whole_text.contains(QStringLiteral("304 ft²")),
            "whole-number PDF totals must sum exact room quantities before rounding once");
    check_precision(0, QStringLiteral("303"));

    sketch::DocumentSnapshot externally_changed = window.document().snapshot();
    edit_precision(1, true, false, [&] {
        const auto source = window.document().snapshot();
        auto changed = source.entities().at("property-1");
        changed.properties["name"] = "Changed while Area display was open";
        window.document().apply(sketch::ApplyEntityChanges{source.revision(), {sketch::EntityChange::upsert(changed)}, {},
            "External modal revision fixture"});
        externally_changed = window.document().snapshot();
    });
    require(window.document().revision() == externally_changed.revision() &&
                window.document().snapshot().entities() == externally_changed.entities(),
            "stale modal Save must not overwrite a newer project revision");

    auto valid = window.document().snapshot();
    auto malformed_property = valid.entities().at("property-1");
    malformed_property.properties["calculation_profile"]["decimal_places"] = 7U;
    window.document().apply(sketch::ApplyEntityChanges{valid.revision(), {sketch::EntityChange::upsert(malformed_property)}, {},
        "Malformed display precision fixture"});
    const auto malformed = window.document().snapshot();
    require(window.selectEntity(second), "malformed display fixture must refresh the selected area");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    const auto malformed_report = sketch::build_appraisal_document_report(malformed, "property-1");
    const auto malformed_schedule = window.scheduleSnapshot();
    require(qualification && qualification->text().contains(QStringLiteral("Unqualified")) &&
                !gla->text().contains(QRegularExpression(QStringLiteral("\\d"))) &&
                !malformed_report.qualified && !malformed_report.calculation &&
                std::none_of(malformed_schedule.snapshot.rows.begin(), malformed_schedule.snapshot.rows.end(), [](const auto& row) {
                    return row.kind == sketch::ScheduleRowKind::appraisal && row.cells.contains("area");
                }) && window.document().revision() == malformed.revision(),
            "invalid display precision must withhold inspector/report/schedule totals without repairing the source");
    require(!area_label(first).contains(QStringLiteral("ft²")) && !area_label(second).contains(QStringLiteral("ft²")),
            "malformed appraisal settings must not leave stale automatic square footage on the plan");
    window.document().apply(sketch::ApplyEntityChanges{malformed.revision(),
        {sketch::EntityChange::upsert(valid.entities().at("property-1"))}, {}, "Restore valid display fixture"});
    window.document().mark_read_only("Area display read-only fixture");
    require(window.selectEntity(second) && !action->isEnabled(),
            "read-only Appraisal must disable the Area display editing action");
    const auto read_only = window.document().snapshot();
    action->trigger();
    require(window.document().revision() == read_only.revision() && window.document().snapshot().entities() == read_only.entities() &&
                !window.findChild<QDialog*>(QStringLiteral("calculationProfileDialog")),
            "read-only Area display invocation must not open a writable dialog or mutate the project");
}

void appraisal_summary_prints_from_the_automatic_report() {
    sketch::desktop::MainWindow window;
    require(!window.createBoundary(square(0, 0, 3.048)).isEmpty(),
            "print fixture needs an appraisal boundary");
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(declarations()),
            "print fixture declarations must qualify");
    const auto schedule = window.scheduleSnapshot();
    const auto gla_row = std::find_if(
        schedule.snapshot.rows.begin(), schedule.snapshot.rows.end(), [](const auto& row) {
            return row.kind == sketch::ScheduleRowKind::appraisal &&
                   row.object_id.ends_with(":category:above_grade_finished");
        });
    require(gla_row != schedule.snapshot.rows.end() &&
                std::holds_alternative<sketch::ScheduleQuantity>(gla_row->cells.at("area").value) &&
                std::abs(std::get<sketch::ScheduleQuantity>(gla_row->cells.at("area").value).value -
                         9.290304) < 1e-8,
            "print fixture must project its automatic 100 square foot GLA row before export");

    auto snapshot = window.document().snapshot();
    const auto sheet_entity = std::find_if(
        snapshot.entities().begin(), snapshot.entities().end(), [](const auto& item) {
            return item.second.type == sketch::kSheetViewEntityType;
        });
    require(sheet_entity != snapshot.entities().end(), "print fixture needs a sheet model");
    auto model = sketch::decode_sheet_view_entity(sheet_entity->second);
    require(std::find(model.schedule_ids().begin(), model.schedule_ids().end(),
                      "appraisal-areas") != model.schedule_ids().end(),
            "appraisal schedule must be registered in a new project");
    sketch::DrawingSheet report_sheet;
    report_sheet.id = "sheet-appraisal";
    report_sheet.number = "A-900";
    report_sheet.width_mm = 420;
    report_sheet.height_mm = 297;
    report_sheet.title_block = {"Appraisal fixture", "Appraisal area summary", "", ""};
    report_sheet.schedules.push_back(
        {"appraisal-summary-placement", "appraisal-areas", {10, 10, 400, 250}});
    const auto original_sheet_id = model.sheets().front().id;
    model = model.with_added_sheet(std::move(report_sheet))
                 .with_removed_sheet(original_sheet_id);
    window.document().apply(sketch::ApplyEntityChanges{
        snapshot.revision(),
        {sketch::EntityChange::upsert(
            sketch::make_sheet_view_entity(sheet_entity->first, model))}, {},
        "add appraisal report sheet"});

    QTemporaryDir directory;
    require(directory.isValid(), "print fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("appraisal-set.pdf"));
    if (!window.exportDrawingSetPdf(path)) {
        throw std::runtime_error(
            "qualified appraisal summary must export in the drawing set: " +
            window.lastError().toStdString());
    }
    QPdfDocument pdf;
    require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
            "appraisal drawing set must contain the added report sheet");
    const auto page = pdf.render(0, QSize(840, 594));
    require(!page.isNull(), "appraisal drawing set page must render back from the exported PDF");
    int schedule_header_pixels = 0;
    for (int y = 20; y < 52; ++y) {
        for (int x = 20; x < 820; ++x) {
            const auto color = page.pixelColor(x, y);
            if (color.red() < 245 || color.green() < 245 || color.blue() < 245)
                ++schedule_header_pixels;
        }
    }
    require(schedule_header_pixels > 15000,
            "exported appraisal sheet must visibly render the automatic schedule header and rows");
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font_id >= 0, "bundled appraisal workflow font must load");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "bundled appraisal workflow font must expose its family");
        app.setFont(QFont(families.front(), 10));
        if (app.arguments().contains(QStringLiteral("--derived-presentation-only"))) {
            appraisal_derived_category_presentation_workflow();
            malformed_appraisal_boundary_has_no_stale_presentation();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--malformed-presentation-only"))) {
            malformed_appraisal_boundary_has_no_stale_presentation();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--area-properties-only"))) {
            appraisal_area_properties_entry_points_are_visible();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--area-appearance-only"))) {
            selection_filter_dropdown_workflow();
            area_appearance_workflow();
            appraisal_derived_category_presentation_workflow();
            malformed_appraisal_boundary_has_no_stale_presentation();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--area-display-only"))) {
            appraisal_area_display_precision_workflow();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--plan-area-labels-only"))) {
            appraisal_plan_area_labels_workflow();
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--plan-label-placement-only"))) {
            plan_label_placement_workflow();
            appraisal_qualification_transition_preview_workflow();
            appraisal_qualification_transition_preview_workflow(false);
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--auto-subtract-only"))) {
            auto_subtract_selected_area_workflow();
            auto_subtract_context_repair_workflow();
            auto_subtract_uses_current_declared_facts();
            auto_subtract_rejects_equal_declared_facts_despite_stale_categories();
            auto_subtract_define_first_keyboard_and_recovery();
            return 0;
        }
        auto_subtract_selected_area_workflow();
        auto_subtract_context_repair_workflow();
        auto_subtract_uses_current_declared_facts();
        auto_subtract_rejects_equal_declared_facts_despite_stale_categories();
        auto_subtract_define_first_keyboard_and_recovery();
        appraisal_workflow_is_automatic_and_persistent();
        appraisal_redefinition_updates_the_active_category();
        declared_appraisal_qualifies_without_manual_categories();
        declared_appraisal_deductions_edit_without_manual_categories();
        declared_exclusions_and_commercial_totals();
        declared_appraisal_excludes_site_boundaries();
        declared_appraisal_reports_selected_building_floor_and_property();
        contradictory_appraisal_ownership_withholds_inspector_and_schedule_totals();
        appraisal_declarations_reject_read_only_documents();
        appraisal_area_display_precision_workflow();
        appraisal_plan_area_labels_workflow();
        plan_label_placement_workflow();
        appraisal_qualification_transition_preview_workflow();
        appraisal_qualification_transition_preview_workflow(false);
        selection_filter_dropdown_workflow();
        appraisal_area_properties_entry_points_are_visible();
        appraisal_derived_category_presentation_workflow();
        malformed_appraisal_boundary_has_no_stale_presentation();
        area_appearance_workflow();
        malformed_appraisal_projection_prints_withheld_status();
        appraisal_summary_prints_from_the_automatic_report();
        std::cout << "appraisal_desktop_workflow_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "appraisal_desktop_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
