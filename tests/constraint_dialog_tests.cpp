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
#include <QPlainTextEdit>
#include <QTimer>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <numeric>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void require_near(double actual, double expected) {
    if (!std::isfinite(actual) || std::abs(actual - expected) >= 1e-7)
        throw std::runtime_error("unexpected anchored wall coordinate: expected " + std::to_string(expected) +
            ", actual " + std::to_string(actual));
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
void fresh_measured_curve_transform_preserves_source() {
    const auto read_curve = [](const Entity& entity) {
        const auto& line = entity.properties.at("baseline");
        return Segment{{line.at("start")[0].get<double>(), line.at("start")[1].get<double>()},
            {line.at("end")[0].get<double>(), line.at("end")[1].get<double>()}, line.at("sweep_radians").get<double>()};
    };
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.setWorkspace(workspace);
        const auto id = window.createCurvedWallFromConstruction({0, 0}, {4, 0}, "arc_length", "5 m");
        require(!id.isEmpty() && window.selectEntity(id), "fresh measured curve transform fixture failed");
        auto measured = window.document().snapshot().entities().at(id.toStdString());
        measured.extensions["curve_input"]["vendor_note"] = {{"exact", "original measured input"}};
        measured.properties["baseline"]["vendor_note"] = "original baseline";
        const Entity opening{"measured-curve-opening", "opening", {{"wall_id", id.toStdString()}, {"offset_m", 1.0},
            {"width_m", 0.5}, {"sill_m", 0.0}, {"height_m", 2.0}}, false, {{"vendor_note", "original host metadata"}}};
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(measured), EntityChange::upsert(opening)}, {}, "retain unknown measured curve metadata and host"});
        const auto source = window.document().snapshot();
        const auto source_input = measured.extensions.at("curve_input");
        const auto source_baseline = measured.properties.at("baseline");
        const auto unchanged_transform = [&] {
            const auto before = window.document().snapshot();
            const auto selected = window.selectedEntityId();
            for (const auto& zero : {QStringLiteral("0"), QStringLiteral("-0.0")}) {
                require(window.transformSelectedBoundary(zero, false, false, zero, zero, false),
                    "equivalent numeric zero transforms must succeed");
                const auto after = window.document().snapshot();
                require(after.revision() == before.revision() && after.entities() == before.entities() &&
                        window.selectedEntityId() == selected,
                    "no-op transform must preserve geometry, provenance, selection and history");
            }
        };
        unchanged_transform();
        const auto archive = [&](const Entity& entity) {
            require(entity.extensions.contains("curve_input_derivation"),
                "fresh measured curve reflection must archive its exact defining input");
            const auto& proof = entity.extensions.at("curve_input_derivation");
            require(proof.at("version") == 2 && proof.at("source_input") == source_input &&
                    proof.at("source_baseline") == source_baseline,
                "rigid curve transforms must retain exact original input and baseline including unknown metadata");
        };
        require(window.transformSelectedBoundary("0", true, false, "0", "0", false),
            "fresh measured curve must reflect through the actual transform action");
        const auto reflected = window.document().snapshot();
        archive(reflected.entities().at(id.toStdString()));
        require(reflected.entities().at(id.toStdString()).properties.at("baseline").at("sweep_radians") ==
                -source_baseline.at("sweep_radians").get<double>(), "one reflection must reverse signed curve sweep");
        require(window.undoCommand() && window.document().snapshot().entities() == source.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == reflected.entities(),
            "fresh measured reflection must undo and redo exactly");

        auto previous_ops = reflected.entities().at(id.toStdString()).extensions.at("curve_input_derivation").at("operations");
        const auto transformed = [&](const QString& angle, bool horizontal, bool vertical,
                                     const QString& x, const QString& y, bool clone) {
            const auto before = window.document().snapshot();
            const auto old_curve = read_curve(before.entities().at(window.selectedEntityId().toStdString()));
            const auto expected = transform_segment(old_curve, {
                {std::midpoint(old_curve.start.x, old_curve.end.x), std::midpoint(old_curve.start.y, old_curve.end.y)},
                angle.toDouble() * std::numbers::pi / 180.0, horizontal, vertical,
                {parse_quantity(x.toStdString()).metres, parse_quantity(y.toStdString()).metres}});
            if (!window.transformSelectedBoundary(angle, horizontal, vertical, x, y, clone)) {
                throw std::runtime_error(QStringLiteral(
                    "archived measured curve transform failed (workspace=%1, angle=%2, horizontal=%3, vertical=%4, x=%5, y=%6, clone=%7): %8")
                    .arg(static_cast<int>(workspace)).arg(angle).arg(horizontal).arg(vertical)
                    .arg(x).arg(y).arg(clone).arg(window.lastError()).toStdString());
            }
            const auto current = window.document().snapshot();
            const auto selected = window.selectedEntityId();
            const auto& entity = current.entities().at(selected.toStdString());
            require(current.revision() == before.revision() + 1,
                "rigid curve transform must record one atomic command");
            auto expected_opening = opening;
            expected_opening.properties["wall_id"] = selected.toStdString();
            std::size_t hosts = 0;
            for (const auto& [opening_id, candidate] : current.entities()) {
                if (candidate.type != "opening" || candidate.properties.value("wall_id", "") != selected.toStdString()) continue;
                expected_opening.id = opening_id;
                require(candidate == expected_opening, "rigid curve transforms must preserve exact hosted opening station and metadata");
                require(!clone || opening_id != opening.id, "curve clone must give its hosted opening an independent identity");
                ++hosts;
            }
            require(hosts == 1, "transformed curve must retain exactly one hosted opening");
            archive(entity);
            const auto actual = read_curve(entity);
            try {
                require_near(actual.start.x, expected.start.x); require_near(actual.start.y, expected.start.y);
                require_near(actual.end.x, expected.end.x); require_near(actual.end.y, expected.end.y);
            } catch (const std::exception& error) {
                throw std::runtime_error(QStringLiteral("measured rigid coordinates (workspace=%1, angle=%2, horizontal=%3, vertical=%4, clone=%5): %6")
                    .arg(static_cast<int>(workspace)).arg(angle).arg(horizontal).arg(vertical).arg(clone)
                    .arg(QString::fromUtf8(error.what())).toStdString());
            }
            require(actual.sweep_radians == expected.sweep_radians,
                "native wall transform must use the archived rotation, reflection and translation order");
            const auto& operations = entity.extensions.at("curve_input_derivation").at("operations");
            require(operations.size() == previous_ops.size() + 1, "rigid transform must append one provenance operation");
            for (std::size_t index = 0; index < previous_ops.size(); ++index)
                require(operations[index] == previous_ops[index], "rigid transform rewrote a retained provenance operation");
            require(operations.back().at("kind") == "rigid_transform",
                "explicit curve transform must retain typed rigid provenance");
            previous_ops = operations;
            require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                    window.redoCommand() && window.document().snapshot().entities() == current.entities() &&
                    window.selectEntity(selected),
                "rigid curve transform and clone must undo and redo exactly");
        };
        transformed("0", false, true, "0", "0", false);
        transformed("0", false, true, "0", "0", false);
        const auto before_double = window.document().snapshot().entities().at(id.toStdString()).properties.at("baseline").at("sweep_radians");
        transformed("0", true, true, "0", "0", false);
        require(window.document().snapshot().entities().at(id.toStdString()).properties.at("baseline").at("sweep_radians") == before_double,
            "two-axis reflection must preserve signed sweep");
        transformed("17", false, false, "0", "0", false);
        transformed("0", false, false, "1 m", "2 m", false);
        unchanged_transform();
        transformed("37", true, false, "1 m", "2 m", false);
        // Keep the copied curve clear of its source: the following length edit
        // exercises provenance, rather than changing a wall crossing.
        transformed("23", false, true, "20 m", "20 m", true);
        const auto clone = window.selectedEntityId();
        require(clone != id && window.document().snapshot().entities().contains(id.toStdString()),
            "transformed curve clone must retain its source owner and receive a new identity");
        const auto before_resize = window.document().snapshot();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = dynamic_cast<ConstraintDialog*>(QApplication::activeModalWidget());
            require(dialog && combo(*dialog, "constraintOperation").currentData().toInt() == 0,
                "reflected curve must keep its direct length operation");
            dialog->setLengthExpression("6 m");
            if (!dialog->previewEdit()) throw std::runtime_error(dialog->lastError().toStdString());
            require(dialog->submit(), "direct physical length after reflection must apply");
        });
        window.showConstraintEditor();
        const auto resized = window.document().snapshot();
        if (resized.revision()!=before_resize.revision()+1)
            throw std::runtime_error("physical resize after rigid clone did not commit: " + window.lastError().toStdString());
        const auto& resized_entity = resized.entities().at(clone.toStdString());
        archive(resized_entity);
        require_near(segment_length(read_curve(resized_entity)), 6);
        require(resized.entities().size() == before_resize.entities().size() &&
                resized_entity.extensions.at("constraint_authoring").at("last_length_entry").at("version") == 2,
            "direct reflected curve resize must retain physical receipt without adding a lock");
        const auto& line = resized_entity.properties.at("baseline");
        require(window.editSelectedCurvedWallFromConstruction(
            {line.at("start")[0].get<double>(), line.at("start")[1].get<double>()},
            {line.at("end")[0].get<double>(), line.at("end")[1].get<double>()}, "arc_height", "1 m"),
            "reflected measured curve must remain editable through deliberate construction");
        const auto reconstructed = window.document().snapshot();
        archive(reconstructed.entities().at(clone.toStdString()));
        require(!reconstructed.entities().at(clone.toStdString()).extensions.at("constraint_authoring").contains("last_length_entry"),
            "explicit construction must invalidate the stale known physical receipt");
        require(window.undoCommand() && window.document().snapshot().entities() == resized.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == reconstructed.entities(),
            "construction after reflected physical resize must undo and redo exactly");
        QTemporaryDir directory;
        MainWindow reopened;
        reopened.setAttribute(Qt::WA_DontShowOnScreen, true);
        require(directory.isValid() && window.saveProjectAs(directory.filePath("measured-curve-transforms.bldproj")) &&
                reopened.openProject(directory.filePath("measured-curve-transforms.bldproj")) &&
                reopened.document().snapshot().entities() == reconstructed.entities(),
            "fresh and archived measured curve transforms, clone, resize and construction must save and replay exactly");
    }
}

void connected_curve_rigid_transforms_preserve_supported_motion() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        for (const bool measured : {false, true}) {
            for (const bool rotate : {false, true}) {
                MainWindow window;
                window.setAttribute(Qt::WA_DontShowOnScreen, true);
                window.setWorkspace(workspace);
                QString id = "wall-a";
                if (measured) {
                    id = window.createCurvedWallFromConstruction({0, 0}, {4, 0}, "arc_length", "5 m");
                    require(!id.isEmpty(), "connected measured curve fixture failed");
                } else {
                    auto curve = wall();
                    curve.properties["baseline"] = {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
                        {"sweep_radians", std::numbers::pi / 2}, {"vendor_note", "connected generic curve"}};
                    window.document().apply(ApplyEntityChanges{window.document().revision(),
                        {EntityChange::upsert(curve)}, {}, "connected generic curve fixture"});
                }
                auto neighbor = wall(); neighbor.id = "connected-neighbor";
                neighbor.properties["baseline"]["start"] = {4.0, 0.0};
                neighbor.properties["baseline"]["end"] = {7.0, 0.0};
                const PersistentConstraint join{"connected-rigid-join", ConstraintRelationKind::coincident,
                    {{id.toStdString(), WallEndpointRole::end}, {neighbor.id, WallEndpointRole::start}}};
                const Entity opening{"connected-rigid-opening", "opening", {{"wall_id", id.toStdString()},
                    {"offset_m", 1.0}, {"width_m", 0.5}, {"sill_m", 0.0}, {"height_m", 2.0}}, false,
                    {{"vendor_note", "retain exact host"}}};
                window.document().apply(ApplyEntityChanges{window.document().revision(),
                    {EntityChange::upsert(neighbor), EntityChange::upsert(encode_constraint_entity(join)),
                     EntityChange::upsert(opening)}, {}, "persist supported curve connection"});
                require(window.selectEntity(id), "connected curve selection failed");
                const auto before = window.document().snapshot();
                const auto& line = before.entities().at(id.toStdString()).properties.at("baseline");
                const auto diagonal=std::sqrt(2.0);
                const Segment expected{rotate ? Vec2{2-diagonal,-diagonal} : Vec2{1,2},
                    rotate ? Vec2{2+diagonal,diagonal} : Vec2{5,2},line.at("sweep_radians").get<double>()};
                if (!window.transformSelectedBoundary(rotate ? "45" : "0", false, false,
                        rotate ? "0" : "1 m", rotate ? "0" : "2 m", false))
                    throw std::runtime_error("supported connected curve rigid transform refused: " + window.lastError().toStdString());
                const auto after = window.document().snapshot();
                const auto& selected = after.entities().at(id.toStdString()).properties.at("baseline");
                const auto& linked = after.entities().at(neighbor.id).properties.at("baseline");
                require_near(selected.at("start")[0].get<double>(), expected.start.x);
                require_near(selected.at("start")[1].get<double>(), expected.start.y);
                require_near(selected.at("end")[0].get<double>(), expected.end.x);
                require_near(selected.at("end")[1].get<double>(), expected.end.y);
                require_near(linked.at("start")[0].get<double>(), expected.end.x);
                require_near(linked.at("start")[1].get<double>(), expected.end.y);
                require(after.entities().at(opening.id) == opening && after.revision() == before.revision() + 1,
                    "connected rigid transform must retain host and use one command");
                require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                        window.redoCommand() && window.document().snapshot().entities() == after.entities(),
                    "connected rigid transform must undo and redo every owner exactly");
                QTemporaryDir directory;
                MainWindow reopened; reopened.setAttribute(Qt::WA_DontShowOnScreen,true);
                require(directory.isValid() && window.saveProjectAs(directory.filePath("connected-rigid.bldproj")) &&
                        reopened.openProject(directory.filePath("connected-rigid.bldproj")) &&
                        reopened.document().snapshot().entities()==after.entities(),
                    "connected rigid geometry and history must save and reopen exactly");
            }
        }
    }
}

void generic_curve_transform_and_connected_refusal() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.setWorkspace(workspace);
        auto curve = wall();
        curve.properties["baseline"] = {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
            {"sweep_radians", std::numbers::pi / 2}, {"vendor_note", "generic analytical curve"}};
        window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(curve)}, {},
            "generic analytical curve without construction receipt"});
        require(window.selectEntity("wall-a"), "generic curve selection failed");
        const auto transform = [&](QString angle, bool horizontal, bool vertical, QString x, QString y) {
            const auto before = window.document().snapshot();
            const auto& line = before.entities().at("wall-a").properties.at("baseline");
            const Segment old{{line.at("start")[0].get<double>(), line.at("start")[1].get<double>()},
                {line.at("end")[0].get<double>(), line.at("end")[1].get<double>()}, line.at("sweep_radians").get<double>()};
            const auto expected = transform_segment(old, {
                {std::midpoint(old.start.x, old.end.x), std::midpoint(old.start.y, old.end.y)},
                angle.toDouble() * std::numbers::pi / 180.0, horizontal, vertical,
                {parse_quantity(x.toStdString()).metres, parse_quantity(y.toStdString()).metres}});
            if (!window.transformSelectedBoundary(angle, horizontal, vertical, x, y, false))
                throw std::runtime_error("generic curved wall rigid transform failed: " + window.lastError().toStdString());
            const auto after = window.document().snapshot();
            const auto& result = after.entities().at("wall-a");
            const auto& actual = result.properties.at("baseline");
            require_near(actual.at("start")[0].get<double>(), expected.start.x);
            require_near(actual.at("start")[1].get<double>(), expected.start.y);
            require_near(actual.at("end")[0].get<double>(), expected.end.x);
            require_near(actual.at("end")[1].get<double>(), expected.end.y);
            require(actual.at("sweep_radians") == expected.sweep_radians &&
                    actual.at("vendor_note") == line.at("vendor_note") && result.extensions == curve.extensions,
                "generic rigid curve transform must preserve analytical sweep and opaque metadata without inventing receipts");
            require(after.revision() == before.revision() + 1 && window.undoCommand() &&
                    window.document().snapshot().entities() == before.entities() && window.redoCommand() &&
                    window.document().snapshot().entities() == after.entities(),
                "generic rigid curve transforms must undo and redo atomically");
        };
        transform("0", true, true, "0", "0");
        transform("131", false, false, "0", "0");
        transform("0", false, false, "1 m", "2 m");
        transform("17", true, false, "0", "0");

        const auto before_join = window.document().snapshot();
        const auto& line = before_join.entities().at("wall-a").properties.at("baseline");
        auto neighbor = wall(); neighbor.id = "connected-neighbor";
        neighbor.properties["baseline"]["start"] = line.at("end");
        neighbor.properties["baseline"]["end"] = {line.at("end")[0].get<double>() + 3, line.at("end")[1].get<double>()};
        const PersistentConstraint join{"curve-transform-join", ConstraintRelationKind::coincident,
            {{"wall-a", WallEndpointRole::end}, {neighbor.id, WallEndpointRole::start}}};
        const Entity opening{"joined-curve-opening", "opening", {{"wall_id", "wall-a"}, {"offset_m", 1.0},
            {"width_m", 0.5}, {"sill_m", 0.0}, {"height_m", 2.0}}, false, nlohmann::json::object()};
        window.document().apply(ApplyEntityChanges{before_join.revision(), {EntityChange::upsert(neighbor),
            EntityChange::upsert(encode_constraint_entity(join)), EntityChange::upsert(opening)}, {}, "join generic curved wall"});
        const auto joined = window.document().snapshot();
        require(!window.transformSelectedBoundary("0", true, true, "0", "0", false) && !window.lastError().isEmpty(),
            "rigid curve transform must explain an incompatible persisted endpoint join");
        require(window.document().revision() == joined.revision() && window.document().snapshot().entities() == joined.entities() &&
                window.selectedEntityId() == "wall-a",
            "rejected linked curve transform must retain connected walls, host, relation, selection and history atomically");
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
            require(combo(*dialog, "constraintOperation").itemText(combo(*dialog, "constraintOperation").findData(0)) == "Change curve length",
                "curved wall must offer a clearly named direct physical length operation");
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
        {
            const auto unlock_source = window.document().snapshot();
            for (const auto& [constraint_id, entity] : unlock_source.entities())
                if (entity.type == "constraint") unlock.push_back(EntityChange::erase(constraint_id));
        }
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
    auto& operation = combo(dialog, "constraintOperation");
    if (operation.currentData().toInt() == 0) operation.setCurrentIndex(operation.findData(1));
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

Entity physical_curve_wall() {
    auto value = wall();
    const auto arc = arc_from_chord_arc_length({0, 0}, {4, 0}, 5, false);
    value.properties["baseline"] = {{"start", {0, 0}}, {"end", {4, 0}}, {"sweep_radians", arc.sweep_radians}};
    value.properties["name"] = "Measured curve";
    return value;
}
Segment stored_wall_baseline(const Entity& value) {
    const auto& line = value.properties.at("baseline");
    return {{line.at("start")[0].get<double>(), line.at("start")[1].get<double>()},
            {line.at("end")[0].get<double>(), line.at("end")[1].get<double>()},
            line.at("sweep_radians").get<double>()};
}

void direct_curve_length_resize_workflow() {
    for (const bool end_anchor : {false, true}) {
        auto document = Document::create({physical_curve_wall()});
        const auto original = document.snapshot();
        const auto old_curve = stored_wall_baseline(original.entities().at("wall-a"));
        ConstraintDialog dialog(original, "wall-a", true);
        require(combo(dialog, "constraintOperation").currentData().toInt() == 0 &&
                combo(dialog, "constraintOperation").currentText() == "Change curve length",
                "curved wall must default to the direct physical length operation");
        require_near(parse_quantity(dialog.findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 5);
        combo(dialog, "constraintAnchor").setCurrentIndex(end_anchor ? 1 : 0);
        dialog.setLengthExpression("6 m");
        require(dialog.previewEdit(), "direct curve length must preview a changed physical target");
        require(document.snapshot().entities() == original.entities(), "direct curve length preview mutated source");
        capture(dialog, end_anchor ? "direct-curve-end-anchor" : "direct-curve-start-anchor");
        require(dialog.submit() && dialog.acceptedPreview(), "direct curve length Apply must return accepted intent");
        (void)apply_constraint_authoring(document, *dialog.acceptedPreview());
        const auto resized = document.snapshot();
        const auto& wall_entity = resized.entities().at("wall-a");
        const auto curve = stored_wall_baseline(wall_entity);
        require_near(segment_length(curve), 6);
        require(curve.sweep_radians == old_curve.sweep_radians && curve.end.x > curve.start.x,
                "direct curve length must preserve signed sweep and direction");
        require_near(curve.end.y - curve.start.y, old_curve.end.y - old_curve.start.y);
        require_near(end_anchor ? curve.end.x : curve.start.x, end_anchor ? old_curve.end.x : old_curve.start.x);
        require_near(end_anchor ? curve.end.y : curve.start.y, end_anchor ? old_curve.end.y : old_curve.start.y);
        const auto& receipt = wall_entity.extensions.at("constraint_authoring").at("last_length_entry");
        require(receipt.size() == 5 && receipt.at("version") == 2 && receipt.at("original_expression") == "6 m" &&
                receipt.at("entered_unit") == "m" && receipt.at("baseline") == wall_entity.properties.at("baseline") &&
                receipt.at("exact_metres").at("numerator") == 6 && receipt.at("exact_metres").at("denominator") == 1,
                "direct curve length must preserve the entered physical quantity exactly");
        require(resized.entities().size() == original.entities().size(),
                "direct curve length must not silently add a persistent length constraint");
        ConstraintDialog repeat(resized, "wall-a", true);
        require_near(parse_quantity(repeat.findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 6);
        combo(repeat, "constraintAnchor").setCurrentIndex(end_anchor ? 1 : 0);
        repeat.setLengthExpression("7 m");
        require(repeat.previewEdit() && repeat.submit(), "direct physical curve length must remain editable without adding a lock");
        (void)apply_constraint_authoring(document, *repeat.acceptedPreview());
        const auto repeated = document.snapshot();
        const auto repeated_curve = stored_wall_baseline(repeated.entities().at("wall-a"));
        require_near(segment_length(repeated_curve), 7);
        require(repeated_curve.sweep_radians == old_curve.sweep_radians,
                "repeated direct curve edit must preserve its signed sweep");
        require_near(end_anchor ? repeated_curve.end.x : repeated_curve.start.x, end_anchor ? old_curve.end.x : old_curve.start.x);
        require_near(end_anchor ? repeated_curve.end.y : repeated_curve.start.y, end_anchor ? old_curve.end.y : old_curve.start.y);
        require(repeated.entities().size() == original.entities().size() &&
                repeated.entities().at("wall-a").extensions.at("constraint_authoring").at("last_length_entry").at("original_expression") == "7 m",
                "repeated direct curve edit must update exact entry without creating a constraint");
        document.undo(document.revision());
        require(document.snapshot().entities() == resized.entities(), "repeated direct curve edit must undo exactly");
        document.undo(document.revision());
        require(document.snapshot().entities() == original.entities(), "direct curve edit must undo to exact source");
    }

    auto unlocked = Document::create({physical_curve_wall()});
    const auto before_cancel = unlocked.snapshot();
    ConstraintDialog cancel(before_cancel, "wall-a", true); cancel.setLengthExpression("6 m");
    require(cancel.previewEdit(), "direct physical curve edit must preview before cancel");
    cancel.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    require(!cancel.acceptedPreview() && unlocked.snapshot().entities() == before_cancel.entities(),
            "Cancel must discard direct physical curve edit");
    PersistentConstraint lock{"physical-lock", ConstraintRelationKind::fixed_arc_length,
        {{"wall-a", WallEndpointRole::start}, {"wall-a", WallEndpointRole::end}}, parse_quantity("5 m")};
    auto locked = Document::create({physical_curve_wall(), encode_constraint_entity(lock)});
    const auto locked_source = locked.snapshot();
    ConstraintDialog conflict(locked_source, "wall-a", true); conflict.setLengthExpression("6 m");
    require(!conflict.previewEdit() && !conflict.submit() && !conflict.lastError().isEmpty() &&
            locked.snapshot().entities() == locked_source.entities(),
            "direct curve edit must explain and preserve an incompatible existing physical length lock");
    capture(conflict, "direct-curve-locked-conflict");
}

void direct_curve_length_connected_boundary_and_host() {
    auto region = boundary("region", 4, 0); region.properties["name"] = "Joined boundary";
    const PersistentConstraint join{"curve-join", ConstraintRelationKind::coincident,
        {{"wall-a", WallEndpointRole::end}, {"region", WallEndpointRole::start, "ab", "a"}}};
    const Entity opening{"curve-opening", "opening", {{"wall_id", "wall-a"}, {"offset_m", 4.7},
        {"width_m", 0.2}, {"sill_m", 0.0}, {"height_m", 2.0}}, false, nlohmann::json::object()};
    auto document = Document::create({physical_curve_wall(), region, opening, encode_constraint_entity(join)});
    const auto original = document.snapshot();
    ConstraintDialog frozen(original, "wall-a", true); frozen.setLengthExpression("6 m");
    frozen.findChild<QCheckBox*>("constraintMoveConnected")->setChecked(false);
    require(!frozen.previewEdit() && !frozen.submit() && !frozen.lastError().isEmpty() &&
            document.snapshot().entities() == original.entities(),
            "freezing a joined boundary must reject incompatible direct curve length");
    ConstraintDialog move(original, "wall-a", true); move.setLengthExpression("6 m");
    require(move.previewEdit() && move.submit(), "direct physical curve length must move its explicitly joined boundary");
    require(move.acceptedPreview()->changed_boundaries().size() == 1,
            "direct physical length preview omitted its moved boundary neighbor");
    capture(move, "direct-curve-connected-boundary");
    (void)apply_constraint_authoring(document, *move.acceptedPreview());
    const auto changed = document.snapshot();
    const auto curve = stored_wall_baseline(changed.entities().at("wall-a"));
    require_near(segment_length(curve), 6);
    const auto moved_boundary = decode_identified_boundary_entity(changed.entities().at("region"));
    require_near(moved_boundary.segments.front().segment.start.x, curve.end.x);
    require_near(moved_boundary.segments.front().segment.start.y, curve.end.y);
    require(changed.entities().at("curve-opening") == opening && changed.entities().at("curve-join") == original.entities().at("curve-join") &&
            changed.entities().size() == original.entities().size(),
            "direct curve edit must preserve physical opening offsets and existing relation without adding a lock");
    document.undo(document.revision());
    require(document.snapshot().entities() == original.entities(), "direct curve edit with boundary and host must undo atomically");
}

void direct_curve_length_workspace_entrypoints() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        MainWindow window;
        const auto id = window.createCurvedWallFromConstruction({0, 0}, {4, 0}, "arc_length", "5 m");
        require(!id.isEmpty(), "direct curve workspace fixture failed");
        window.setWorkspace(workspace);
        require(window.selectEntity(id), "direct curve workspace selection failed");
        const auto original = window.document().snapshot();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = dynamic_cast<ConstraintDialog*>(QApplication::activeModalWidget());
            require(dialog && combo(*dialog, "constraintOperation").currentData().toInt() == 0 &&
                    combo(*dialog, "constraintOperation").currentText() == "Change curve length",
                    "both workspaces must open the actual direct curve operation");
            require_near(parse_quantity(dialog->findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 5);
            dialog->setLengthExpression("6 m");
            if (!dialog->previewEdit()) throw std::runtime_error(dialog->lastError().toStdString());
            capture(*dialog, workspace == Workspace::measurement ? "direct-curve-measurement-workspace"
                                                                 : "direct-curve-architectural-workspace");
            require(dialog->submit(), "workspace direct curve length Apply failed");
        });
        window.showConstraintEditor();
        const auto resized = window.document().snapshot();
        require(resized.revision() == original.revision() + 1 && resized.entities().size() == original.entities().size(),
                "workspace direct curve edit must record one change without a persistent lock");
        const auto curve = stored_wall_baseline(resized.entities().at(id.toStdString()));
        require_near(segment_length(curve), 6);
        require(curve.sweep_radians == stored_wall_baseline(original.entities().at(id.toStdString())).sweep_radians,
                "workspace direct physical curve edit lost its sweep");
        require(window.undoCommand() && window.document().snapshot().entities() == original.entities(),
                "workspace direct curve length must undo exactly");
    }
}
void physical_curve_length_wall_workflow() {
    const auto curve_length = ConstraintRelationKind::fixed_arc_length;
    for (const bool end_anchor : {false, true}) {
        auto document = Document::create({physical_curve_wall()});
        const auto original = document.snapshot();
        const auto before = stored_wall_baseline(original.entities().at("wall-a"));
        ConstraintDialog add(original, "wall-a", true);
        require(combo(add, "constraintRelation").findText("Curve length") >= 0,
                "curved owner must expose the physical Curve length relationship");
        require(combo(add, "constraintOperation").itemText(combo(add, "constraintOperation").findData(0)) == "Change curve length",
                "physical curve length must retain the separate direct curve operation");
        select_relation(add, curve_length);
        auto* length = add.findChild<QLineEdit*>("constraintLength");
        require(length && parse_quantity(length->text().toStdString()).metres > 4.9,
                "Curve length must prefill physical arc length rather than endpoint distance");
        require_near(parse_quantity(length->text().toStdString()).metres, 5);
        select_relation(add, ConstraintRelationKind::fixed_length);
        require_near(parse_quantity(length->text().toStdString()).metres, 4);
        select_relation(add, curve_length);
        require_near(parse_quantity(length->text().toStdString()).metres, 5);
        require(combo(add, "constraintBinding0").currentText().endsWith("start") &&
                combo(add, "constraintBinding1").currentText().endsWith("end"),
                "curve length must default to opposite endpoints of its selected owner");
        choose_endpoint(add, "constraintAnchor", "Measured curve", end_anchor ? "end fixed" : "start fixed");
        add.setLengthExpression("6 m");
        require(add.previewEdit(), "physical curve length must preview a changed anchored target");
        require(document.snapshot().entities() == original.entities(), "curve length preview mutated source");
        auto* table = add.findChild<QTableWidget*>("constraintChanges");
        require(table && table->rowCount() == 1 && table->item(0, 1)->text() == "5 m" &&
                table->item(0, 2)->text() == "6 m", "curve length comparison must show physical lengths");
        capture(add, end_anchor ? "physical-curve-end-anchor" : "physical-curve-start-anchor");
        require(add.submit() && add.acceptedPreview(), "physical curve length Apply must return accepted intent");
        (void)apply_constraint_authoring(document, *add.acceptedPreview());
        const auto after = document.snapshot();
        const auto curve = stored_wall_baseline(after.entities().at("wall-a"));
        require_near(segment_length(curve), 6);
        require(curve.sweep_radians == before.sweep_radians, "curve length must preserve signed sweep");
        require_near(end_anchor ? curve.end.x : curve.start.x, end_anchor ? before.end.x : before.start.x);
        require_near(end_anchor ? curve.end.y : curve.start.y, end_anchor ? before.end.y : before.start.y);
        std::string relation_id;
        for (const auto& [id, entity] : after.entities()) if (entity.type == "constraint") {
            const auto saved = *decode_constraint_entity(entity).constraint;
            require(saved.relation == curve_length && saved.length && saved.length->original_expression == "6 m",
                    "curve length dialog must persist its physical target and relation");
            relation_id = id;
        }
        require(!relation_id.empty(), "curve length dialog did not persist a relation");

        ConstraintDialog edit(after, "wall-a", true);
        auto& operation = combo(edit, "constraintOperation"); operation.setCurrentIndex(operation.findData(2));
        require(combo(edit, "constraintRelation").currentText() == "Curve length" &&
                edit.findChild<QLineEdit*>("constraintLength")->text() == "6 m" &&
                combo(edit, "existingConstraint").currentText().startsWith("Curve length"),
                "existing physical curve target must reload with its exact expression");
        choose_endpoint(edit, "constraintAnchor", "Measured curve", "end fixed");
        edit.setLengthExpression("7 m");
        require(edit.previewEdit() && edit.submit(), "existing physical curve target must remain editable");
        (void)apply_constraint_authoring(document, *edit.acceptedPreview());
        require_near(segment_length(stored_wall_baseline(document.snapshot().entities().at("wall-a"))), 7);
        require(document.snapshot().entities().contains(relation_id), "editing curve target must retain relation identity");
        const auto edited = document.snapshot();
        ConstraintDialog conflict(edited, "wall-a", true);
        select_relation(conflict, curve_length); conflict.setLengthExpression("8 m");
        require(!conflict.previewEdit() && !conflict.submit() && !conflict.lastError().isEmpty(),
                "a second incompatible physical curve target must reject with an explanation");
        require(!conflict.lastError().contains("fixed_arc_length") &&
                !conflict.findChild<QPlainTextEdit*>("constraintStatus")->toPlainText().contains("fixed_arc_length"),
                "curve length diagnostics must not expose persistence spellings");
        require(document.snapshot().entities() == edited.entities(), "curve length conflict mutated source");
        ConstraintDialog cancel(edited, "wall-a", true);
        combo(cancel, "constraintOperation").setCurrentIndex(combo(cancel, "constraintOperation").findData(2));
        cancel.setLengthExpression("8 m");
        require(cancel.previewEdit(), "changed curve target must preview before cancel");
        cancel.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        require(!cancel.acceptedPreview() && document.snapshot().entities() == edited.entities(),
                "Cancel must discard a physical curve target preview");
        ConstraintDialog remove(edited, "wall-a", true);
        combo(remove, "constraintOperation").setCurrentIndex(combo(remove, "constraintOperation").findData(3));
        require(remove.previewEdit() && remove.submit(), "physical curve relation must remain removable");
        (void)apply_constraint_authoring(document, *remove.acceptedPreview());
        require(!document.snapshot().entities().contains(relation_id), "curve target removal retained the relation");
        auto without_relation = edited.entities(); without_relation.erase(relation_id);
        require(document.snapshot().entities() == without_relation, "removing curve target must preserve exact geometry");
        document.undo(document.revision());
        require(document.snapshot().entities() == edited.entities(), "curve length removal must undo atomically");
    }
}

void physical_curve_chain_total_editing() {
    for (const int fixture : {0, 1, 2}) {
        const std::size_t pieces = fixture == 0 ? 2 : 3;
        PersistentConstraint chain{"arc-chain-total", ConstraintRelationKind::fixed_arc_length, {},
            parse_quantity(pieces == 2 ? "2.0858296429334882 m" : "3.1287444644002323 m")};
        std::vector<Entity> entities;
        std::string selected;
        if (fixture == 2) {
            auto shape = encode_identified_boundary_entity(IdentifiedBoundary{"chain-boundary", "measurement_boundary", {
                {"ab", "a", "b", {{0, 0}, {1, 0}, 1}},
                {"bc", "b", "c", {{1, 0}, {2, 0}, 1}},
                {"cd", "c", "d", {{2, 0}, {3, 0}, 1}},
                {"de", "d", "e", {{3, 0}, {3, 2}, 0}},
                {"ef", "e", "f", {{3, 2}, {0, 2}, 0}},
                {"fa", "f", "a", {{0, 2}, {0, 0}, 0}}}});
            shape.properties["name"] = "Chain boundary";
            selected = shape.id;
            entities.push_back(shape);
            // Reverse traversal must retain its ordered stable segment and vertex identities.
            chain.bindings = {{selected, WallEndpointRole::end, "cd", "d"},
                {selected, WallEndpointRole::start, "cd", "c"},
                {selected, WallEndpointRole::end, "bc", "c"},
                {selected, WallEndpointRole::start, "bc", "b"},
                {selected, WallEndpointRole::end, "ab", "b"},
                {selected, WallEndpointRole::start, "ab", "a"}};
        } else {
            for (std::size_t i = 0; i < pieces; ++i) {
                auto piece = wall(); piece.id = "chain-wall-" + std::to_string(i);
                piece.properties["name"] = "Chain curve " + std::to_string(i + 1);
                piece.properties["baseline"] = {{"start", {static_cast<double>(i), 0}},
                    {"end", {static_cast<double>(i + 1), 0}}, {"sweep_radians", 1.0}};
                piece.properties["thickness_m"] = 0.1;
                if (i == 0) selected = piece.id;
                entities.push_back(piece);
                chain.bindings.push_back({piece.id, WallEndpointRole::start});
                chain.bindings.push_back({piece.id, WallEndpointRole::end});
                if (i > 0) {
                    const PersistentConstraint seam{"seam-" + std::to_string(i), ConstraintRelationKind::coincident,
                        {{"chain-wall-" + std::to_string(i - 1), WallEndpointRole::end},
                            {piece.id, WallEndpointRole::start}}};
                    entities.push_back(encode_constraint_entity(seam));
                }
            }
        }
        auto encoded = encode_constraint_entity(chain);
        encoded.properties["vendor_property"] = "retain envelope";
        encoded.properties["bindings"].back()["vendor_binding"] = "retain last member";
        encoded.properties["quantity_entries"]["/length_m"]["vendor_receipt"] = "retain receipt";
        encoded.properties["quantity_entries"]["/future_dimension"] = {{"version", 99}, {"opaque", "retain unrelated quantity"}};
        encoded.extensions["vendor_extension"] = "retain extension";
        entities.push_back(encoded);
        auto document = Document::create(entities);
        const auto original = document.snapshot();
        ConstraintDialog edit(original, QString::fromStdString(selected), true);
        auto& operation = combo(edit, "constraintOperation");
        operation.setCurrentIndex(operation.findData(2));
        auto& existing = combo(edit, "existingConstraint");
        existing.setCurrentIndex(existing.findData(QString::fromStdString(chain.id)));
        auto* length = edit.findChild<QLineEdit*>("constraintLength");
        require(length && length->text().toStdString() == chain.length->original_expression,
            "chain editor must load the original exact total expression");
        // Disabled controls must not be able to reinterpret a chain through
        // programmatic selection changes or their ordinary prefill callbacks.
        auto& relation = combo(edit, "constraintRelation");
        relation.setCurrentIndex(relation.findData(static_cast<int>(ConstraintRelationKind::fixed_length)));
        auto& first_binding = combo(edit, "constraintBinding0");
        first_binding.setCurrentIndex((first_binding.currentIndex() + 1) % first_binding.count());
        require(relation.currentData().toInt() == static_cast<int>(ConstraintRelationKind::fixed_arc_length) &&
                length->text().toStdString() == chain.length->original_expression,
            "pair-only controls must not change saved chain type or prefill only its first curve");
        edit.setLengthExpression("12 ft");
        if (!edit.previewEdit()) throw std::runtime_error(edit.lastError().toStdString());
        require(edit.submit() && edit.acceptedPreview(), "chain total edit must Preview and Apply");
        const auto preview = *edit.acceptedPreview();
        const auto& candidate = preview.candidate_entities();
        const auto& saved_entity = candidate.at(chain.id);
        const auto saved = *decode_constraint_entity(saved_entity).constraint;
        require(saved.bindings == chain.bindings && saved.relation == ConstraintRelationKind::fixed_arc_length &&
                saved_entity.properties.at("version") == 4,
            "editing the total must preserve the complete ordered arc chain instead of its first curve");
        require(saved.length && encode_constraint_quantity_receipt(*saved.length) ==
                encode_constraint_quantity_receipt(parse_quantity("12 ft")),
            "chain total edit must persist the entered exact quantity");
        require(saved_entity.properties.at("bindings") == encoded.properties.at("bindings") &&
                saved_entity.properties.at("vendor_property") == encoded.properties.at("vendor_property") &&
                saved_entity.extensions == encoded.extensions &&
                saved_entity.properties.at("quantity_entries").at("/future_dimension") ==
                    encoded.properties.at("quantity_entries").at("/future_dimension"),
            "chain total edit must preserve opaque envelope, binding and unrelated quantity metadata");
        // Receipt metadata belongs to the previous quantity. The codec
        // intentionally replaces it when the exact physical total changes.
        require(saved_entity.properties.at("quantity_entries").at("/length_m") ==
                encode_constraint_quantity_receipt(parse_quantity("12 ft")),
            "changed chain total must replace stale receipt metadata with the fresh exact quantity");
        require_near(resolve_constraint_arc_length(saved, candidate), 3.6576);
        for (const auto& [id, entity] : original.entities()) if (entity.type == "constraint" && id != chain.id)
            require(candidate.at(id) == entity, "chain total edit must preserve every relation in the existing graph");
        require(document.snapshot().entities() == original.entities(), "chain Preview and submission mutated the source");
        auto* membership = edit.findChild<QLabel*>("constraintArcChain");
        require(membership && !membership->isHidden() && membership->text().contains(QString::number(pieces)) &&
                membership->text().contains("total", Qt::CaseInsensitive),
            "chain editor must explain segment membership and total-length semantics");
        require(!combo(edit, "constraintRelation").isEnabled(), "chain editing must lock its relationship type");
        for (const auto* field : {"constraintBinding0", "constraintBinding1", "constraintBinding2", "constraintBinding3"})
            require(combo(edit, field).isHidden() && !combo(edit, field).isEnabled(),
                "chain editor must hide and disable pair-only endpoint controls");
        capture(edit, fixture == 0 ? "physical-curve-chain-total" : fixture == 1
            ? "physical-three-curve-chain-total" : "physical-boundary-chain-total");
        apply_constraint_authoring(document, *edit.acceptedPreview());
        const auto applied = document.snapshot();
        require(applied.entities().at(chain.id) == saved_entity, "Apply must persist the complete chain preview");
        operation.setCurrentIndex(operation.findData(1));
        require(combo(edit, "constraintRelation").isEnabled() && !combo(edit, "constraintBinding0").isHidden() &&
                combo(edit, "constraintBinding0").isEnabled() && membership->isHidden(),
            "leaving chain editing must restore ordinary relationship authoring controls");
        document.undo(document.revision());
        require(document.snapshot().entities() == original.entities(), "chain total edit must undo atomically");
        document.redo(document.revision());
        require(document.snapshot().entities() == applied.entities(), "chain total edit must redo atomically");
        const auto restored = document.snapshot();
        ConstraintDialog cancel(restored, QString::fromStdString(selected), true);
        auto& cancel_operation = combo(cancel, "constraintOperation");
        cancel_operation.setCurrentIndex(cancel_operation.findData(2));
        combo(cancel, "existingConstraint").setCurrentIndex(combo(cancel, "existingConstraint").findData(QString::fromStdString(chain.id)));
        require(!cancel.previewEdit() && cancel.lastError().contains("makes no document change"),
            "unchanged chain total must retain the no-op rejection contract");
        cancel.setLengthExpression("bad total");
        require(!cancel.previewEdit() && !cancel.submit(), "invalid chain total must reject Preview and Apply");
        cancel.setLengthExpression("4 m");
        require(cancel.previewEdit(), "chain target must preview before Cancel");
        cancel.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        require(!cancel.acceptedPreview() && document.snapshot().entities() == applied.entities(),
            "Cancel must discard the complete chain edit");
        ConstraintDialog remove(restored, QString::fromStdString(selected), true);
        auto& remove_operation = combo(remove, "constraintOperation");
        remove_operation.setCurrentIndex(remove_operation.findData(3));
        combo(remove, "existingConstraint").setCurrentIndex(combo(remove, "existingConstraint").findData(QString::fromStdString(chain.id)));
        require(remove.previewEdit() && remove.submit(), "chain removal must remain an explicit operation");
        apply_constraint_authoring(document, *remove.acceptedPreview());
        auto without_chain = applied.entities(); without_chain.erase(chain.id);
        require(document.snapshot().entities() == without_chain, "chain removal must preserve geometry and other graph relations");
    }
}

void physical_curve_length_boundary_workflow_and_invalid_bindings() {
    auto shape = decode_identified_boundary_entity(boundary("region", 0, 0));
    const auto physical = arc_from_chord_arc_length({4, 0}, {4, 4}, 5, false);
    shape.segments[1].segment = physical;
    auto owner = encode_identified_boundary_entity(shape); owner.properties["name"] = "Measured boundary";
    auto document = Document::create({owner});
    const auto original = document.snapshot();
    ConstraintDialog add(original, "region", true);
    select_relation(add, ConstraintRelationKind::fixed_arc_length);
    require(combo(add, "constraintBinding0").currentText().contains("Edge 2 · start") &&
            combo(add, "constraintBinding1").currentText().contains("Edge 2 · end") &&
            combo(add, "constraintAnchor").currentText().contains("Edge 2 · start fixed"),
            "curve length must select a curved edge and matching fixed anchor even when the first edge is straight");
    require_near(parse_quantity(add.findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 5);
    add.setLengthExpression("6 m");
    require(add.previewEdit() && add.submit(), "physical curved boundary edge must preview and apply");
    const auto* table = add.findChild<QTableWidget*>("constraintChanges");
    require(table && table->rowCount() == 4 && table->item(1, 1)->text() == "5 m" &&
            table->item(1, 2)->text() == "6 m", "curved boundary comparison must show physical edge lengths");
    capture(add, "physical-boundary-curve-length");
    (void)apply_constraint_authoring(document, *add.acceptedPreview());
    const auto applied = document.snapshot();
    const auto updated = decode_identified_boundary_entity(applied.entities().at("region"));
    require_near(segment_length(updated.segments[1].segment), 6);
    require(updated.segments[1].segment.sweep_radians == physical.sweep_radians,
            "physical edge length must preserve signed sweep");
    require_near(updated.segments[1].segment.start.x, physical.start.x);
    require_near(updated.segments[1].segment.start.y, physical.start.y);
    std::string relation_id;
    for (const auto& [id, entity] : applied.entities()) if (entity.type == "constraint") {
        const auto saved = *decode_constraint_entity(entity).constraint;
        require(saved.relation == ConstraintRelationKind::fixed_arc_length &&
                saved.bindings == std::vector<WallEndpointBinding>{{"region", WallEndpointRole::start, "bc", "b"},
                    {"region", WallEndpointRole::end, "bc", "c"}},
                "physical boundary relation must persist exact same-edge and stable vertex bindings");
        relation_id = id;
    }
    require(!relation_id.empty(), "physical boundary relation was not persisted");
    ConstraintDialog edit(applied, "region", true);
    combo(edit, "constraintOperation").setCurrentIndex(combo(edit, "constraintOperation").findData(2));
    require(combo(edit, "constraintRelation").currentText() == "Curve length" &&
            edit.findChild<QLineEdit*>("constraintLength")->text() == "6 m" &&
            combo(edit, "constraintBinding0").currentText().contains("Edge 2 · start") &&
            combo(edit, "constraintAnchor").currentText().contains("Edge 2 · start fixed"),
            "existing physical edge target must reload correct bindings, exact expression, and matching anchor");
    choose_endpoint(edit, "constraintAnchor", "Measured boundary", "Edge 2 · end fixed");
    edit.setLengthExpression("5.5 m");
    require(edit.previewEdit() && edit.submit(), "existing physical boundary target must remain editable with another anchor");
    (void)apply_constraint_authoring(document, *edit.acceptedPreview());
    const auto edited = document.snapshot();
    const auto resized = decode_identified_boundary_entity(edited.entities().at("region"));
    require_near(segment_length(resized.segments[1].segment), 5.5);
    require_near(resized.segments[1].segment.end.x, updated.segments[1].segment.end.x);
    require_near(resized.segments[1].segment.end.y, updated.segments[1].segment.end.y);
    ConstraintDialog remove(edited, "region", true);
    combo(remove, "constraintOperation").setCurrentIndex(combo(remove, "constraintOperation").findData(3));
    require(remove.previewEdit() && remove.submit(), "physical boundary target must remain removable");
    (void)apply_constraint_authoring(document, *remove.acceptedPreview());
    auto without_relation = edited.entities(); without_relation.erase(relation_id);
    require(document.snapshot().entities() == without_relation, "removing physical edge target must preserve exact boundary geometry");
    document.undo(document.revision());
    require(document.snapshot().entities() == edited.entities(), "physical boundary target removal must undo atomically");

    for (const auto mismatch : {QStringLiteral("Edge 1 · end"), QStringLiteral("Edge 2 · start")}) {
        ConstraintDialog invalid(original, "region", true);
        select_relation(invalid, ConstraintRelationKind::fixed_arc_length);
        choose_endpoint(invalid, "constraintBinding1", "Measured boundary", mismatch);
        invalid.setLengthExpression("6 m");
        require(!invalid.previewEdit() && !invalid.submit() && invalid.lastError().contains("same curved"),
                "mismatched edge or same endpoint roles must produce a clear curve-length diagnostic");
        require(document.snapshot().entities() == edited.entities(), "invalid physical boundary binding mutated project");
    }
    auto straight = Document::create({wall()});
    const auto straight_original = straight.snapshot();
    ConstraintDialog straight_dialog(straight_original, "wall-a", true);
    auto& operation = combo(straight_dialog, "constraintOperation"); operation.setCurrentIndex(operation.findData(1));
    select_relation(straight_dialog, ConstraintRelationKind::fixed_arc_length);
    straight_dialog.setLengthExpression("5 m");
    require(!straight_dialog.previewEdit() && !straight_dialog.submit() && straight_dialog.lastError().contains("curved"),
            "a straight owner must reject physical curve length with an explanation");
    require(straight.snapshot().entities() == straight_original.entities(), "invalid straight curve target mutated source");
    auto other = physical_curve_wall(); other.id = "wall-b"; other.properties["name"] = "Other curve";
    other.properties["baseline"]["start"] = {10, 0}; other.properties["baseline"]["end"] = {14, 0};
    auto mismatched = Document::create({physical_curve_wall(), other});
    const auto mismatch_source = mismatched.snapshot();
    ConstraintDialog mismatched_dialog(mismatch_source, "wall-a", true);
    select_relation(mismatched_dialog, ConstraintRelationKind::fixed_arc_length);
    choose_endpoint(mismatched_dialog, "constraintBinding1", "Other curve", "end");
    mismatched_dialog.setLengthExpression("6 m");
    require(!mismatched_dialog.previewEdit() && !mismatched_dialog.submit() &&
            mismatched_dialog.lastError().contains("same curved") && mismatched.snapshot().entities() == mismatch_source.entities(),
            "different curved owners must not be accepted as one physical curve-length target");

    auto two_curves = shape;
    two_curves.segments[2].segment = arc_from_chord_arc_length({4, 4}, {0, 4}, 6, false);
    auto alternate = encode_identified_boundary_entity(two_curves); alternate.properties["name"] = "Measured boundary";
    auto alternate_source = Document::create({alternate});
    ConstraintDialog edge_choice(alternate_source.snapshot(), "region", true);
    select_relation(edge_choice, ConstraintRelationKind::fixed_arc_length);
    choose_endpoint(edge_choice, "constraintBinding0", "Measured boundary", "Edge 3 · start");
    require(combo(edge_choice, "constraintBinding1").currentText().contains("Edge 3 · end") &&
            combo(edge_choice, "constraintAnchor").currentText().contains("Edge 3 · start fixed"),
            "changing the physical target edge must choose its opposite endpoint and matching anchor");
    require_near(parse_quantity(edge_choice.findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 6);
    select_relation(edge_choice, ConstraintRelationKind::fixed_length);
    require_near(parse_quantity(edge_choice.findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 4);
    select_relation(edge_choice, ConstraintRelationKind::fixed_arc_length);
    require(combo(edge_choice, "constraintBinding0").currentText().contains("Edge 3 · start"),
            "Curve length must preserve a meaningful currently selected curved edge");
    require_near(parse_quantity(edge_choice.findChild<QLineEdit*>("constraintLength")->text().toStdString()).metres, 6);
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
        if (app.arguments().contains(QStringLiteral("--connected-rigid-only"))) {
            connected_curve_rigid_transforms_preserve_supported_motion();
            std::cout << "Connected curve rigid workflows passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--arc-chain-edit-only"))) {
            physical_curve_chain_total_editing();
            std::cout << "Physical curve chain editing passed\n";
            return 0;
        }
        fresh_measured_curve_transform_preserves_source();
        connected_curve_rigid_transforms_preserve_supported_motion();
        generic_curve_transform_and_connected_refusal();
        direct_curve_length_resize_workflow();
        direct_curve_length_connected_boundary_and_host();
        direct_curve_length_workspace_entrypoints();
        physical_curve_length_wall_workflow();
        physical_curve_chain_total_editing();
        physical_curve_length_boundary_workflow_and_invalid_bindings();
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
