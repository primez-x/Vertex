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
Entity boundary(std::string id, double x = 0, double tilt = 1) {
    return encode_identified_boundary_entity(IdentifiedBoundary{std::move(id), "measurement_boundary", {
        {"ab", "a", "b", {{x, 0}, {x + 4, tilt}, 0}},
        {"bc", "b", "c", {{x + 4, tilt}, {x + 4, 4}, 0}},
        {"cd", "c", "d", {{x + 4, 4}, {x, 4}, 0}},
        {"da", "d", "a", {{x, 4}, {x, 0}, 0}}}});
}
void select_relation(ConstraintDialog& dialog, ConstraintRelationKind kind) {
    auto& field = combo(dialog, "constraintRelation");
    field.setCurrentIndex(field.findData(static_cast<int>(kind)));
}
void boundary_relationship_workflows() {
    auto document = Document::create({boundary("first"), boundary("second", 10, 0), wall()});
    const auto original = document.snapshot();
    ConstraintDialog add(original, "first", true);
    require(ConstraintDialog::supportsEntity(original.entities().at("first")), "straight boundary unavailable");
    require(combo(add, "constraintOperation").count() == 3 && combo(add, "constraintOperation").currentData().toInt() == 1,
        "boundary must default to Add and omit wall resize");
    require(combo(add, "constraintBinding0").count() == 16, "boundary endpoints include walls or miss stable edges");
    require(add.previewEdit() && add.submit(), "boundary horizontal preview failed");
    const auto preview = *add.acceptedPreview();
    require(preview.changed_boundaries().size() == 1 && document.snapshot().entities() == original.entities(), "boundary preview mutated state or omitted geometry");
    capture(add, "boundary-horizontal");
    apply_constraint_authoring(document, preview);
    const auto solved = decode_identified_boundary_entity(document.snapshot().entities().at("first"));
    require_near(solved.segments[0].segment.end.y, 0);
    bool stale_rejected = false;
    try { apply_constraint_authoring(document, preview); } catch (const std::exception&) { stale_rejected = true; }
    require(stale_rejected, "stale boundary preview applied twice");
    const auto horizontal = document.snapshot();
    document.undo(document.revision());
    require(document.snapshot().entities() == original.entities(), "boundary dialog undo was incomplete");
    document.redo(document.revision());
    require(document.snapshot().entities() == horizontal.entities(), "boundary dialog redo was incomplete");
    ConstraintDialog edit(document.snapshot(), "first", true);
    combo(edit, "constraintOperation").setCurrentIndex(1);
    select_relation(edit, ConstraintRelationKind::fixed_length);
    edit.setLengthExpression("5 m");
    require(edit.previewEdit() && edit.submit(), "boundary constraint edit failed");
    apply_constraint_authoring(document, *edit.acceptedPreview());
    const auto locked = document.snapshot();
    ConstraintDialog conflict(locked, "first", true);
    select_relation(conflict, ConstraintRelationKind::fixed_length);
    conflict.setLengthExpression("6 m");
    require(!conflict.previewEdit() && !conflict.submit() && !conflict.lastError().isEmpty(), "boundary conflict was not explained and rejected");
    require(document.snapshot().entities() == locked.entities(), "boundary conflict changed document");
    ConstraintDialog remove(locked, "first", true);
    combo(remove, "constraintOperation").setCurrentIndex(2);
    require(remove.previewEdit() && remove.submit(), "boundary removal preview failed");
    require(remove.acceptedPreview()->changed_boundaries().empty(), "removing constraint moved geometry");
    apply_constraint_authoring(document, *remove.acceptedPreview());
    document.undo(document.revision());
    require(document.snapshot().entities() == locked.entities(), "boundary removal undo failed");
    for (const auto kind : {ConstraintRelationKind::parallel, ConstraintRelationKind::perpendicular,
                           ConstraintRelationKind::coincident, ConstraintRelationKind::vertical,
                           ConstraintRelationKind::fixed_anchor}) {
        auto fixture = Document::create({boundary("first", 0, 0.5), boundary("second", 10, 0)});
        ConstraintDialog relation(fixture.snapshot(), "first", true);
        select_relation(relation, kind);
        if (kind == ConstraintRelationKind::perpendicular) {
            combo(relation, "constraintBinding2").setCurrentIndex(10);
            combo(relation, "constraintBinding3").setCurrentIndex(11);
        } else if (kind == ConstraintRelationKind::coincident) {
            combo(relation, "constraintBinding0").setCurrentIndex(1);
            combo(relation, "constraintBinding1").setCurrentIndex(8);
        } else if (kind == ConstraintRelationKind::vertical) {
            combo(relation, "constraintBinding0").setCurrentIndex(2);
            combo(relation, "constraintBinding1").setCurrentIndex(3);
        }
        if (!relation.previewEdit()) throw std::runtime_error(relation.lastError().toStdString());
        require(relation.submit(), "boundary relation submit failed");
        apply_constraint_authoring(fixture, *relation.acceptedPreview());
        bool found = false;
        const auto committed = fixture.snapshot();
        for (const auto& [id, entity] : committed.entities()) if (entity.type == "constraint") {
            const auto constraint = decode_constraint_entity(entity);
            require(constraint.constraint->relation == kind, "dialog saved wrong relation");
            for (const auto& binding : constraint.constraint->bindings)
                require(!binding.segment_id.empty() && !binding.vertex_id.empty(), "dialog dropped stable boundary identity");
            found = true;
        }
        require(found, "boundary relationship was not persisted");
    }
    auto curved = decode_identified_boundary_entity(boundary("curve"));
    curved.segments[0].segment.sweep_radians = 0.2;
    require(!ConstraintDialog::supportsEntity(encode_identified_boundary_entity(curved)), "curved boundary offered unsupported constraints");
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
        boundary_relationship_workflows();
        both_workspace_entrypoints();
        std::cout << "Constraint dialog workflows passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint_dialog_tests: " << error.what() << '\n';
        return 1;
    }
}
