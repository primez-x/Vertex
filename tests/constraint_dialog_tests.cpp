#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPushButton>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void require_near(double actual, double expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-7, "unexpected anchored wall coordinate");
}
Entity wall() {
    return {"wall-a", "wall", {{"baseline", {{"start", {0.0, 0.0}}, {"end", {3.6576, 0.0}}, {"sweep_radians", 0.0}}},
        {"thickness_m", 0.2}, {"height_m", 3.0}, {"elevation_m", 0.0}}, false, nlohmann::json::object()};
}
QComboBox& combo(ConstraintDialog& dialog, const char* name) {
    auto* field = dialog.findChild<QComboBox*>(name); require(field, "missing constraint choice"); return *field;
}
void capture(ConstraintDialog& dialog, const char* name) {
    const auto directory = qEnvironmentVariable("SKETCH_CONSTRAINT_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "cannot create capture directory");
    const bool was_visible = dialog.isVisible();
    if (!was_visible) { dialog.setAttribute(Qt::WA_DontShowOnScreen, true); dialog.show(); }
    QApplication::processEvents();
    require(dialog.grab().save(QDir(directory).filePath(QString::fromUtf8(name) + ".png")), "constraint capture failed");
    if (!was_visible) dialog.hide();
}
void resize_preview_anchor_and_invalidation() {
    for (const bool end_anchor : {false, true}) {
        auto document = Document::create({wall()});
        const auto before = document.snapshot();
        ConstraintDialog dialog(before, "wall-a");
        require(!dialog.submit(), "Apply should require a current preview");
        dialog.setLengthExpression("14 ft");
        combo(dialog, "constraintAnchor").setCurrentIndex(end_anchor ? 1 : 0);
        dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
        dialog.show();
        QApplication::setActiveWindow(&dialog);
        auto* length = dialog.findChild<QLineEdit*>("constraintLength");
        length->setFocus();
        QApplication::processEvents();
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(length, &tab);
        require(combo(dialog, "constraintAnchor").hasFocus(), "Tab from length must reach the anchor choice");
        require(dialog.previewEdit(), "12 to 14 foot resize should preview");
        require(document.snapshot().entities() == before.entities() && document.revision() == before.revision(),
                "preview mutated the document");
        dialog.setLengthExpression("13 ft");
        require(!dialog.submit() && !dialog.findChild<QPushButton*>("constraintApplyButton")->isEnabled(),
                "changing input must invalidate Apply");
        dialog.setLengthExpression("14 ft");
        require(dialog.previewEdit(), "refreshed preview should succeed");
        capture(dialog, end_anchor ? "end-anchor" : "start-anchor");
        require(dialog.submit() && dialog.acceptedPreview(), "preview Apply should return service receipt");
        apply_constraint_authoring(document, *dialog.acceptedPreview());
        const auto p = document.snapshot().entities().at("wall-a").properties.at("baseline");
        require_near(p.at("start")[0].get<double>(), end_anchor ? -0.6096 : 0.0);
        require_near(p.at("end")[0].get<double>(), end_anchor ? 3.6576 : 4.2672);
        document.undo(document.revision());
        require(document.snapshot().entities() == before.entities(), "dialog edit must undo atomically");
    }
}
void relationship_create_edit_conflict_remove() {
    auto document = Document::create({wall()});
    ConstraintDialog add(document.snapshot(), "wall-a");
    combo(add, "constraintOperation").setCurrentIndex(1);
    require(add.previewEdit() && add.submit(), "horizontal relationship should preview and submit");
    apply_constraint_authoring(document, *add.acceptedPreview());
    ConstraintDialog edit(document.snapshot(), "wall-a");
    combo(edit, "constraintOperation").setCurrentIndex(2);
    auto& relation = combo(edit, "constraintRelation");
    relation.setCurrentIndex(relation.findData(static_cast<int>(ConstraintRelationKind::fixed_length)));
    edit.setLengthExpression("14 ft");
    require(edit.previewEdit() && edit.submit(), "changing relation to a new fixed length should preview");
    apply_constraint_authoring(document, *edit.acceptedPreview());
    const auto locked = document.snapshot();
    ConstraintDialog conflict(locked, "wall-a");
    conflict.setLengthExpression("15 ft");
    require(!conflict.previewEdit() && !conflict.submit(), "locked length must reject contradictory resize");
    require(!conflict.lastError().isEmpty(), "conflict needs inline explanation");
    capture(conflict, "conflicting-length");
    require(document.snapshot().entities() == locked.entities(), "conflict mutated the document");
    ConstraintDialog remove(document.snapshot(), "wall-a");
    combo(remove, "constraintOperation").setCurrentIndex(3);
    require(remove.previewEdit() && remove.submit(), "constraint removal should preview");
    apply_constraint_authoring(document, *remove.acceptedPreview());
    require(document.snapshot().entities().size() == 1, "constraint removal left its entity behind");
    document.undo(document.revision());
    require(document.snapshot().entities() == locked.entities(), "constraint removal must undo atomically");
}
void both_workspace_entrypoints() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        MainWindow window;
        const auto id = window.createStraightWall({0, 0}, {3.6576, 0});
        require(!id.isEmpty(), "workspace wall fixture failed");
        window.setWorkspace(workspace);
        require(window.selectEntity(id), "workspace wall selection failed");
        const auto revision = window.document().revision();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = dynamic_cast<ConstraintDialog*>(QApplication::activeModalWidget());
            require(dialog, "workspace constraint action did not open the editor");
            dialog->setLengthExpression("14 ft");
            require(dialog->previewEdit(), "workspace constraint preview failed");
            capture(*dialog, workspace == Workspace::measurement ? "measurement-workspace" : "architectural-workspace");
            require(dialog->submit(), "workspace constraint dialog failed");
        });
        window.showConstraintEditor();
        require(window.document().revision() == revision + 1, "workspace Apply did not record one command");
        const auto line = window.document().snapshot().entities().at(id.toStdString()).properties.at("baseline");
        require_near(line.at("end")[0].get<double>(), 4.2672);
        require(window.undoCommand(), "workspace constraint edit cannot undo");
    }
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    app.setFont(QFont(QStringLiteral("Inter"), 10));
    try {
        resize_preview_anchor_and_invalidation();
        relationship_create_edit_conflict_remove();
        both_workspace_entrypoints();
        std::cout << "Constraint dialog workflows passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint_dialog_tests: " << error.what() << '\n';
        return 1;
    }
}
