#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/constraint_preview_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLineEdit>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QPushButton>
#include <QTimer>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <numbers>
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
void select_relation(ConstraintDialog& dialog, ConstraintRelationKind kind);
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
void analytical_curve_preview() {
    for (const bool light : {false, true}) for (const double sign : {-1.0, 1.0}) {
        ConstraintPreviewCanvas canvas;
        canvas.resize(420, 250);
        auto palette = canvas.palette();
        palette.setColor(QPalette::Base, light ? Qt::white : QColor("#17212d"));
        palette.setColor(QPalette::Text, light ? Qt::black : Qt::white);
        canvas.setPalette(palette);
        const Segment arc{{0, 0}, {4, 0}, sign * std::numbers::pi};
        canvas.setWalls({{QStringLiteral("Semicircle"), arc, arc}});
        const auto image = canvas.grab().toImage();
        const auto blue_near = [&](int x, int y) {
            for (int dx = -5; dx <= 5; ++dx) for (int dy = -5; dy <= 5; ++dy) {
                const auto color = image.pixelColor(x + dx, y + dy);
                if (color.blue() > color.red() + 70 && color.blue() > color.green() + 25) return true;
            }
            return false;
        };
        require(blue_near(210, sign > 0 ? 220 : 62), "preview must draw the signed semicircle at its analytical extremum");
        require(!blue_near(210, sign > 0 ? 62 : 220), "preview incorrectly draws the arc as its chord");
    }
}
void curved_wall_workspace_entrypoints() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        MainWindow window;
        const auto id = window.createCurvedWallFromConstruction({0, 0}, {4, 0}, "arc_length", "5 m");
        require(!id.isEmpty(), "curved workspace fixture failed");
        window.setWorkspace(workspace);
        require(window.selectEntity(id), "curved wall selection failed");
        const auto original = window.document().snapshot();
        const auto sweep = original.entities().at(id.toStdString()).properties.at("baseline").at("sweep_radians");
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = dynamic_cast<ConstraintDialog*>(QApplication::activeModalWidget());
            require(dialog, "curved constraint action did not open editor");
            require(combo(*dialog, "constraintOperation").findData(0) == -1,
                "curved wall must not offer the straight-wall resize operation");
            select_relation(*dialog, ConstraintRelationKind::fixed_length);
            require(combo(*dialog, "constraintRelation").currentText() == "Endpoint distance",
                "curved relationship length must explain its endpoint-distance meaning");
            dialog->setLengthExpression("4.5 m");
            if (!dialog->previewEdit()) throw std::runtime_error(dialog->lastError().toStdString());
            capture(*dialog, workspace == Workspace::measurement ? "curved-wall-measurement" : "curved-wall-architectural");
            require(dialog->submit(), "curved wall constraint submission failed");
        });
        window.showConstraintEditor();
        const auto edited = window.document().snapshot();
        require(edited.revision() == original.revision() + 1, "curved wall edit must record one command");
        const auto& baseline = edited.entities().at(id.toStdString()).properties.at("baseline");
        require(baseline.at("sweep_radians") == sweep, "curved wall constraint lost its signed sweep");
        require_near(std::hypot(baseline.at("end")[0].get<double>() - baseline.at("start")[0].get<double>(),
            baseline.at("end")[1].get<double>() - baseline.at("start")[1].get<double>()), 4.5);
        require(window.undoCommand(), "curved wall edit cannot undo");
        require(window.document().snapshot().entities() == original.entities(), "curved wall undo failed to restore original construction evidence");
        require(window.redoCommand() && window.document().snapshot().entities() == edited.entities(), "curved wall redo changed the result");
        require(window.selectEntity(id), "derived curved wall selection failed");
        const Vec2 start{baseline.at("start")[0].get<double>(), baseline.at("start")[1].get<double>()};
        const Vec2 end{baseline.at("end")[0].get<double>(), baseline.at("end")[1].get<double>()};
        require(window.editSelectedCurvedWallFromConstruction(start, end, "arc_height", "1 m"),
            "explicit curve construction must remain editable after connected endpoint derivation");
        const auto reconstructed = window.document().snapshot();
        require(reconstructed.entities().at(id.toStdString()).extensions.at("curve_input_derivation").at("source_input").at("measure") == "5 m",
            "later curve construction discarded the original measured input");
        QTemporaryDir directory;
        MainWindow reopened;
        require(directory.isValid() && window.saveProjectAs(directory.filePath("curved-constraints.bldproj")) &&
            reopened.openProject(directory.filePath("curved-constraints.bldproj")) &&
            reopened.document().snapshot().entities() == reconstructed.entities(),
            "curved endpoint derivation and subsequent construction must save and reopen exactly");
        require(window.undoCommand() && window.document().snapshot().entities() == edited.entities(),
            "explicit curve reconstruction must undo without losing the constraint edit");
        require(window.saveProjectAs(directory.filePath("curved-undone-construction.bldproj")) &&
            reopened.openProject(directory.filePath("curved-undone-construction.bldproj")) &&
            reopened.document().snapshot().entities() == edited.entities(),
            "save/reopen must restore a shorter construction derivation after undo");
        require(window.undoCommand() && window.document().snapshot().entities() == original.entities() &&
            window.saveProjectAs(directory.filePath("curved-undone-endpoint.bldproj")) &&
            reopened.openProject(directory.filePath("curved-undone-endpoint.bldproj")) &&
            reopened.document().snapshot().entities() == original.entities(),
            "save/reopen must restore the original curve without derivation after undo");
        require(window.redoCommand() && window.redoCommand(), "derived curve fixture must redo both edits");
        std::vector<EntityChange> unlock;
        for (const auto& [constraint_id, entity] : window.document().snapshot().entities())
            if (entity.type == "constraint") unlock.push_back(EntityChange::erase(constraint_id));
        window.document().apply(ApplyEntityChanges{window.document().revision(),unlock,{},"remove locks before rigid transforms"});
        require(window.selectEntity(id), "derived curve transform selection failed");
        const auto transform_source=window.document().snapshot();
        require(window.transformSelectedBoundary("90",false,false,"1 m","2 m",false),
            "derived curve must rotate and translate through the existing transform editor");
        const auto rotated=window.document().snapshot();
        require(window.undoCommand() && window.document().snapshot().entities()==transform_source.entities() &&
            window.redoCommand() && window.document().snapshot().entities()==rotated.entities(),
            "derived curve rigid transform must undo and redo exactly");
        const auto before_flip=rotated.entities().at(id.toStdString()).properties.at("baseline").at("sweep_radians").get<double>();
        require(window.selectEntity(id) && window.transformSelectedBoundary("0",true,false,"0","0",false),
            "derived curve must reflect while retaining its original construction archive");
        require(window.document().snapshot().entities().at(id.toStdString()).properties.at("baseline").at("sweep_radians").get<double>() == -before_flip,
            "derived curve reflection must reverse the signed sweep");
        require(window.transformSelectedBoundary("45",false,false,"2 m","0",true),
            "transformed cloning of a derived curve must succeed");
        const auto clone=window.selectedEntityId();
        const auto transformed=window.document().snapshot();
        require(clone!=id && transformed.entities().at(clone.toStdString()).extensions.at("curve_input_derivation").at("source_input").at("measure")=="5 m",
            "derived clone must retain the exact original measured input");
        require(window.saveProjectAs(directory.filePath("curved-transformed-clone.bldproj")) &&
            reopened.openProject(directory.filePath("curved-transformed-clone.bldproj")) &&
            reopened.document().snapshot().entities()==transformed.entities(),
            "derived curve rotation, reflection and clone must independently replay after reopening");
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
    require(combo(add, "constraintBinding0").count() == 18, "boundary editor must include wall and stable boundary endpoints");
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
    require(ConstraintDialog::supportsEntity(encode_identified_boundary_entity(curved)), "curved boundary must offer endpoint constraints");
    auto curved_document = Document::create({encode_identified_boundary_entity(curved)});
    const auto curved_original = curved_document.snapshot();
    ConstraintDialog curve_dialog(curved_original, "curve", true);
    select_relation(curve_dialog, ConstraintRelationKind::fixed_length);
    curve_dialog.setLengthExpression("5 m");
    require(curve_dialog.previewEdit() && curve_dialog.submit(), "curved boundary endpoint-distance preview failed");
    capture(curve_dialog, "curved-boundary-endpoint-distance");
    apply_constraint_authoring(curved_document, *curve_dialog.acceptedPreview());
    const auto curve_after = decode_identified_boundary_entity(curved_document.snapshot().entities().at("curve"));
    require(curve_after.segments[0].segment.sweep_radians == 0.2, "constraint flattened the boundary arc");
    require_near(std::hypot(curve_after.segments[0].segment.end.x - curve_after.segments[0].segment.start.x,
        curve_after.segments[0].segment.end.y - curve_after.segments[0].segment.start.y), 5);
    require(segment_length(curve_after.segments[0].segment) > 5, "endpoint-distance relation became an arc-length lock");
    curved_document.undo(curved_document.revision());
    require(curved_document.snapshot().entities() == curved_original.entities(), "curved relation did not undo atomically");
}
int endpoint_choice(QComboBox& field, const QString& owner, const QString& endpoint) {
    for (int i = 0; i < field.count(); ++i)
        if (field.itemText(i).contains(owner) && field.itemText(i).endsWith(endpoint)) return i;
    throw std::runtime_error("missing mixed owner endpoint choice");
}
void choose_endpoint(ConstraintDialog& dialog, const char* field, const QString& owner, const QString& endpoint) {
    auto& choice = combo(dialog, field);
    choice.setCurrentIndex(endpoint_choice(choice, owner, endpoint));
}
Document mixed_fixture() {
    auto line = wall(); line.properties["name"] = "Shared wall";
    auto region = boundary("region", 3.6576, 0); region.properties["name"] = "Shared boundary";
    return Document::create({line, region});
}
void mixed_owner_choices_and_relation_editing() {
    for (const auto& selected : {QStringLiteral("wall-a"), QStringLiteral("region")}) {
        auto document = mixed_fixture();
        const auto original = document.snapshot();
        ConstraintDialog add(original, selected, true);
        auto& operations = combo(add, "constraintOperation");
        require((operations.findData(0) >= 0) == (selected == "wall-a"), "only walls may offer length resize");
        operations.setCurrentIndex(operations.findData(1));
        auto& endpoints = combo(add, "constraintBinding0");
        require(endpoints.count() == 10, "both selected owner types must expose the same mixed endpoint universe");
        require(endpoints.itemText(endpoint_choice(endpoints, "Shared wall", "end")).contains("Wall") &&
            endpoints.itemText(endpoint_choice(endpoints, "Shared boundary", "Edge 1 · start")).contains("Boundary"),
            "endpoint labels must distinguish named wall and numbered boundary edge");
        auto* connected = add.findChild<QCheckBox*>("constraintMoveConnected");
        require(connected && connected->text() == "Allow connected objects to move", "mixed relation movement wording is wall/boundary specific");
        require(combo(add, "constraintAnchor").count() == 10 &&
            endpoint_choice(combo(add, "constraintAnchor"), "Shared wall", "start fixed") >= 0 &&
            endpoint_choice(combo(add, "constraintAnchor"), "Shared boundary", "Edge 1 · end fixed") >= 0,
            "relation anchors must expose either supported owner type");
        select_relation(add, ConstraintRelationKind::coincident);
        choose_endpoint(add, "constraintBinding0", "Shared wall", "end");
        choose_endpoint(add, "constraintBinding1", "Shared boundary", "Edge 1 · start");
        require(add.previewEdit() && add.submit(), "mixed coincidence must preview and save from either selected owner");
        require(document.snapshot().entities() == original.entities(), "mixed relation preview mutated document");
        apply_constraint_authoring(document, *add.acceptedPreview());
        const auto joined = document.snapshot();
        std::string relation_id;
        for (const auto& [id, entity] : joined.entities()) if (entity.type == "constraint") {
            const auto saved = *decode_constraint_entity(entity).constraint;
            require(saved.bindings == std::vector<WallEndpointBinding>{{"wall-a", WallEndpointRole::end},
                {"region", WallEndpointRole::start, "ab", "a"}}, "mixed dialog dropped stable owner/edge/vertex bindings");
            relation_id = id;
        }
        require(!relation_id.empty(), "mixed relation entity was not saved");
        // A reopened editor must load and effectively edit the same mixed relation.
        ConstraintDialog edit(joined, selected, true);
        auto& edit_mode = combo(edit, "constraintOperation"); edit_mode.setCurrentIndex(edit_mode.findData(2));
        require(combo(edit, "existingConstraint").currentData().toString().toStdString() == relation_id,
            "reopened mixed owner editor did not find its persisted relation");
        require(combo(edit, "constraintBinding0").currentText().contains("Shared wall") &&
            combo(edit, "constraintBinding1").currentText().contains("Shared boundary"), "reopened mixed bindings not restored");
        choose_endpoint(edit, "constraintBinding1", "Shared boundary", "Edge 1 · end");
        choose_endpoint(edit, "constraintAnchor", "Shared boundary", "Edge 1 · end fixed");
        require(edit.previewEdit() && edit.submit(), "mixed relation edit with boundary anchor must succeed");
        require(!edit.acceptedPreview()->changed_walls().empty(), "mixed relation edit must actually move the wall");
        capture(edit, selected == "wall-a" ? "mixed-wall-editor" : "mixed-boundary-editor");
        apply_constraint_authoring(document, *edit.acceptedPreview());
        const auto edited = document.snapshot();
        const auto saved = *decode_constraint_entity(edited.entities().at(relation_id)).constraint;
        require(saved.bindings[1].segment_id == "ab" && saved.bindings[1].vertex_id == "b", "mixed relation edit did not save new stable endpoint");
        require_near(edited.entities().at("wall-a").properties.at("baseline").at("end")[0].get<double>(), 7.6576);
        document.undo(document.revision()); require(document.snapshot().entities() == joined.entities(), "mixed edit undo was incomplete");
        document.redo(document.revision()); require(document.snapshot().entities() == edited.entities(), "mixed edit redo was incomplete");
        ConstraintDialog cancel(edited, selected, true);
        auto& cancel_mode = combo(cancel, "constraintOperation"); cancel_mode.setCurrentIndex(cancel_mode.findData(3));
        require(cancel.previewEdit(), "mixed removal must preview");
        cancel.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        require(!cancel.acceptedPreview() && document.snapshot().entities() == edited.entities(), "Cancel applied a mixed preview");
    }
}
void mixed_connected_movement_and_frozen_conflict() {
    auto document = mixed_fixture();
    ConstraintDialog join(document.snapshot(), "region", true);
    select_relation(join, ConstraintRelationKind::coincident);
    choose_endpoint(join, "constraintBinding0", "Shared wall", "end");
    choose_endpoint(join, "constraintBinding1", "Shared boundary", "Edge 1 · start");
    require(join.previewEdit() && join.submit(), "mixed movement fixture relation failed");
    apply_constraint_authoring(document, *join.acceptedPreview());
    const auto before = document.snapshot();
    ConstraintDialog frozen(before, "wall-a", true);
    frozen.setLengthExpression("4.2672 m");
    frozen.findChild<QCheckBox*>("constraintMoveConnected")->setChecked(false);
    require(!frozen.previewEdit() && !frozen.submit() && !frozen.lastError().isEmpty(), "freezing mixed partner must reject incompatible resize");
    require(document.snapshot().entities() == before.entities(), "frozen conflict mutated mixed geometry");
    ConstraintDialog move(before, "wall-a", true); move.setLengthExpression("4.2672 m");
    require(move.previewEdit() && move.submit(), "connected mixed partner must move with resized wall");
    const auto preview = *move.acceptedPreview();
    require(preview.changed_walls().size() == 1 && preview.changed_boundaries().size() == 1, "mixed preview omitted changed wall or boundary");
    auto* table = move.findChild<QTableWidget*>("constraintChanges");
    require(table && table->rowCount() == 5, "mixed preview table must show wall and all boundary edges");
    require(table->item(0, 0)->text().contains("Shared wall") && table->item(1, 0)->text().contains("Shared boundary"),
        "mixed preview table must identify both named owners");
    capture(move, "mixed-connected-movement");
    apply_constraint_authoring(document, preview);
    const auto committed = document.snapshot();
    require_near(decode_identified_boundary_entity(committed.entities().at("region")).segments.front().segment.start.x, 4.2672);
    document.undo(document.revision()); require(document.snapshot().entities() == before.entities(), "mixed movement undo was incomplete");
    document.redo(document.revision()); require(document.snapshot().entities() == committed.entities(), "mixed movement redo was incomplete");
}
void room_boundary_mixed_choices_and_reopen() {
    auto room = decode_identified_boundary_entity(boundary("room", 3.6576, 0));
    room.type = "room_boundary";
    auto region = encode_identified_boundary_entity(room); region.properties["name"] = "Shared room";
    auto line = wall(); line.properties["name"] = "Shared wall";
    require(ConstraintDialog::supportsEntity(region), "straight identified room boundary must be supported");
    for (const auto& selected : {QStringLiteral("wall-a"), QStringLiteral("room")}) {
        auto document = Document::create({line, region});
        const auto before = document.snapshot();
        ConstraintDialog dialog(before, selected, true);
        auto& operation = combo(dialog, "constraintOperation"); operation.setCurrentIndex(operation.findData(1));
        require((operation.findData(0) >= 0) == (selected == "wall-a"), "room boundary must not offer wall resize");
        auto& choices = combo(dialog, "constraintBinding0");
        require(choices.count() == 10 &&
            choices.itemText(endpoint_choice(choices, "Shared room", "Edge 1 · start")).startsWith("Room boundary"),
            "wall and room editors must expose type-aware room edge choices");
        select_relation(dialog, ConstraintRelationKind::coincident);
        choose_endpoint(dialog, "constraintBinding0", "Shared wall", "end");
        choose_endpoint(dialog, "constraintBinding1", "Shared room", "Edge 1 · start");
        require(dialog.previewEdit() && dialog.submit(), "wall/room mixed relation must preview from either owner");
        require(document.snapshot().entities() == before.entities(), "room mixed preview mutated source");
        apply_constraint_authoring(document, *dialog.acceptedPreview());
        const auto joined = document.snapshot();
        ConstraintDialog reopened(joined, selected, true);
        auto& reopen_mode = combo(reopened, "constraintOperation"); reopen_mode.setCurrentIndex(reopen_mode.findData(2));
        require(combo(reopened, "existingConstraint").count() == 1 &&
            combo(reopened, "constraintBinding0").currentText().contains("Shared wall") &&
            combo(reopened, "constraintBinding1").currentText().contains("Shared room"),
            "reopened wall/room editor must restore the mixed relation bindings");
        require(!reopened.previewEdit() && reopened.lastError().contains("makes no document change"),
            "unchanged reopened relation must preserve the no-op rejection contract");
        choose_endpoint(reopened, "constraintBinding1", "Shared room", "Edge 1 · end");
        choose_endpoint(reopened, "constraintAnchor", "Shared room", "Edge 1 · end fixed");
        require(reopened.previewEdit() && reopened.submit(), "reopened room mixed relation must remain editable");
        require(document.snapshot().entities() == joined.entities(), "room mixed edit preview mutated source");
        require(!reopened.acceptedPreview()->changed_walls().empty(), "room mixed endpoint edit must move the wall");
        apply_constraint_authoring(document, *reopened.acceptedPreview());
        const auto edited = document.snapshot();
        const auto relation_id = combo(reopened, "existingConstraint").currentData().toString().toStdString();
        const auto saved = *decode_constraint_entity(edited.entities().at(relation_id)).constraint;
        require(saved.bindings[1] == WallEndpointBinding{"room", WallEndpointRole::end, "ab", "b"},
            "room mixed edit must persist the new stable endpoint binding");
        require_near(edited.entities().at("wall-a").properties.at("baseline").at("end")[0].get<double>(), 7.6576);
        document.undo(document.revision()); require(document.snapshot().entities() == joined.entities(), "room mixed edit undo was incomplete");
        document.redo(document.revision()); require(document.snapshot().entities() == edited.entities(), "room mixed edit redo was incomplete");
    }
    room.segments.front().segment.sweep_radians = 0.2;
    require(ConstraintDialog::supportsEntity(encode_identified_boundary_entity(room)), "curved room boundary must offer endpoint constraints");
}
QLabel& persistent_freedom(ConstraintDialog& dialog) {
    auto* label = dialog.findChild<QLabel*>("constraintPersistentFreedom");
    require(label, "missing persisted component coordinate freedom label");
    return *label;
}
PersistentConstraint horizontal_boundary_relation(std::string id, std::string owner) {
    return {std::move(id), ConstraintRelationKind::horizontal,
        {{owner, WallEndpointRole::start, "ab", "a"}, {owner, WallEndpointRole::end, "ab", "b"}}};
}
void persisted_coordinate_freedom_before_after() {
    const auto level = horizontal_boundary_relation("stored-level", "first");
    auto document = Document::create({boundary("first", 0, 0), encode_constraint_entity(level)});
    const auto original = document.snapshot();
    ConstraintDialog dialog(original, "first", true);
    auto& label = persistent_freedom(dialog);
    require(label.text().contains("7") && !label.text().contains(QStringLiteral("→")),
        "source must show seven stored coordinate freedoms without preview counts");
    require(label.toolTip().contains("X/Y") && label.toolTip().contains("translation") &&
        label.toolTip().contains("rotation") && label.toolTip().contains("architectural"),
        "coordinate freedom tooltip must define its scope and rigid movement freedoms");
    // Temporary dialog edit anchors remove two further coordinates. They must
    // never replace the separate persisted component value in the label.
    ConstraintAuthoringIntent anchored;
    auto duplicate = level; duplicate.id = "duplicate-level";
    anchored.relation_mutations = {ConstraintRelationMutation::upsert(duplicate)};
    anchored.relation_anchor = level.bindings.front();
    const auto anchored_preview = preview_constraint_authoring(original, anchored);
    require(anchored_preview.accepted() && anchored_preview.degrees_of_freedom() == 5,
        "fixture must distinguish five edit freedoms from seven stored freedoms");
    select_relation(dialog, ConstraintRelationKind::fixed_length);
    dialog.setLengthExpression("4 m");
    require(dialog.previewEdit(), "fixed length freedom preview must remain accepted");
    require(label.text().contains(QStringLiteral("7 → 6")) && label.text().contains("-1") &&
        label.text().contains("1 object"),
        "stored fixed length must show a one-freedom reduction for the same owner component");
    require(document.snapshot().entities() == original.entities(), "freedom analysis must not mutate the source");
    capture(dialog, "persistent-freedom-fixed-length");
    dialog.setLengthExpression("5 m");
    require(label.text().contains("7") && !label.text().contains(QStringLiteral("→")) &&
        !dialog.findChild<QPushButton*>("constraintApplyButton")->isEnabled(),
        "input invalidation must restore source-only freedom and invalidate Apply");
    dialog.setLengthExpression("not a length");
    require(!dialog.previewEdit() && !label.text().contains(QStringLiteral("→")) && label.text().contains("7"),
        "rejected preview must not retain an after freedom count");
}
void removal_preserves_comparison_owner_universe() {
    const PersistentConstraint join{"stored-bridge", ConstraintRelationKind::coincident,
        {{"first", WallEndpointRole::end, "ab", "b"}, {"second", WallEndpointRole::start, "ab", "a"}}};
    auto document = Document::create({boundary("first", 0, 0), boundary("second", 4, 0), encode_constraint_entity(join)});
    ConstraintDialog dialog(document.snapshot(), "first", true);
    auto& label = persistent_freedom(dialog);
    require(label.text().contains("14") && label.text().contains("2 objects"),
        "connected source must report both owners and fourteen freedoms");
    combo(dialog, "constraintOperation").setCurrentIndex(2);
    require(dialog.previewEdit(), "bridge removal preview must stay accepted");
    require(label.text().contains(QStringLiteral("14 → 16")) && label.text().contains("+2") &&
        label.text().contains("2 objects"),
        "removal must compare both owners even when the proposed graph disconnects");
    capture(dialog, "persistent-freedom-bridge-removal");
    combo(dialog, "constraintOperation").setCurrentIndex(0);
    require(label.text().contains("14") && label.text().contains("2 objects") &&
        !label.text().contains(QStringLiteral("→")),
        "changing operation must restore the original connected source value");
}
void unavailable_persistent_freedom_is_explicit() {
    auto unknown = encode_constraint_entity(horizontal_boundary_relation("future-level", "first"));
    unknown.properties["version"] = 99;
    auto document = Document::create({boundary("first", 0, 0), unknown});
    ConstraintDialog dialog(document.snapshot(), "first", true);
    auto& label = persistent_freedom(dialog);
    require(label.text().contains("unavailable", Qt::CaseInsensitive) && !label.text().contains("0 freedoms") &&
        !label.text().contains(QStringLiteral("→")),
        "unsupported persistent relations must show unavailable, never an invented zero");
    const auto before = document.snapshot();
    require(!dialog.submit(), "freedom analysis must not independently authorize Apply");
    require(document.snapshot().entities() == before.entities(), "unavailable diagnostic must preserve unknown source entities");
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
        mixed_owner_choices_and_relation_editing();
        mixed_connected_movement_and_frozen_conflict();
        room_boundary_mixed_choices_and_reopen();
        boundary_relationship_workflows();
        persisted_coordinate_freedom_before_after();
        removal_preserves_comparison_owner_universe();
        unavailable_persistent_freedom_is_explicit();
        both_workspace_entrypoints();
        analytical_curve_preview();
        curved_wall_workspace_entrypoints();
        std::cout << "Constraint dialog workflows passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint_dialog_tests: " << error.what() << '\n';
        return 1;
    }
}
