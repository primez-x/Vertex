#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/project_store.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void exercise() {
    desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1400,1000);
    window.show();
    QApplication::processEvents();
    const auto id = window.createBoundary({{{0,0},{4,0},0},{{4,0},{4,3},0},
        {{4,3},{0,3},0},{{0,3},{0,0},0}}, "living_area");
    require(!id.isEmpty() && window.selectEntity(id), "source room must be selected");
    const auto source = decode_identified_boundary_entity(window.document().snapshot().entities().at(id.toStdString()));
    const auto& first = source.segments[0];
    const auto& second = source.segments[1];
    BoundaryDimension angle{"redraw-auto-angle",id.toStdString(),first.segment_id,{4.5,0.5}};
    angle.kind = BoundaryDimensionKind::angle;
    angle.placement = BoundaryDimensionPlacement::automatic;
    angle.automatic_placement_version = 2;
    angle.secondary_segment_id = second.segment_id;
    angle.vertex_id = first.end_vertex_id;
    BoundaryDimension manual{"redraw-manual-length",id.toStdString(),first.segment_id,{2,-0.5}};
    BoundaryDimension area{"redraw-area",id.toStdString(),{}, {2,1.5}};
    area.kind = BoundaryDimensionKind::area;
    const auto unrelated = window.createBoundary({{{8,0},{10,0},0},{{10,0},{10,2},0},
        {{10,2},{8,2},0},{{8,2},{8,0},0}}, "living_area");
    const auto other = decode_identified_boundary_entity(window.document().snapshot().entities().at(unrelated.toStdString()));
    BoundaryDimension other_length{"redraw-unrelated-length",other.id,other.segments[0].segment_id,{9,-0.5}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(encode_boundary_dimension_entity(angle)),
         EntityChange::upsert(encode_boundary_dimension_entity(manual)),
         EntityChange::upsert(encode_boundary_dimension_entity(area)),
         EntityChange::upsert(encode_boundary_dimension_entity(other_length))},{},"redraw references"});
    require(window.selectEntity(id), "source room must be reselected");
    const auto before = window.document().snapshot();
    const Boundary triangle{{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,0},0}};
    bool drawn_redraw = false;
    const auto review = [&](bool remove, bool cancel, const QString& capture) {
        QTimer::singleShot(0,&window,[&,remove,cancel,capture] {
            auto* dialog = window.findChild<QDialog*>("boundaryReferenceReview");
            require(dialog, "real redraw review dialog must open");
            dialog->setAttribute(Qt::WA_DontShowOnScreen);
            auto* choices = dialog->findChild<QTableWidget*>("boundaryReferenceChoices");
            auto* mappings = dialog->findChild<QTableWidget*>("boundaryReferenceMappings");
            auto* buttons = dialog->findChild<QDialogButtonBox*>("boundaryReferenceButtons");
            require(choices && mappings && buttons, "review controls must exist");
            bool found_angle = false;
            for (int row=0;row<choices->rowCount();++row) {
                const auto ref = choices->item(row,0)->data(Qt::UserRole).toString();
                auto* decision = qobject_cast<QComboBox*>(choices->cellWidget(row,1));
                require(decision, "reference decision must be a combo");
                if (ref == QString::fromStdString(angle.id)) {
                    found_angle = true;
                    require(decision->findText("Remove") == 2, "automatic angle must offer explicit Remove");
                    // Exercise retry from Remove to Keep before the final decision.
                    decision->setCurrentIndex(2);
                    decision->setCurrentIndex(1);
                    decision->setCurrentIndex(remove ? 2 : 1);
                } else {
                    require(ref == QString::fromStdString(manual.id), "automatic edge lengths and unrelated refs must not be choices");
                    decision->setCurrentIndex(1);
                }
            }
            require(found_angle, "automatic angle must appear in review");
            for (int row=0;row<mappings->rowCount();++row) {
                const auto old = mappings->item(row,0)->data(Qt::UserRole).toString().toStdString();
                qobject_cast<QComboBox*>(mappings->cellWidget(row,1))->setCurrentIndex(
                    old == second.segment_id || old == first.end_vertex_id ? 2 : 1);
            }
            require(buttons->button(QDialogButtonBox::Apply)->isEnabled(), "valid reference choices must preview atomically");
            const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
            if (!directory.isEmpty() && !capture.isEmpty()) require(QDir().mkpath(directory) &&
                dialog->grab().save(QDir(directory).filePath(capture)), "actual review capture must save");
            buttons->button(cancel ? QDialogButtonBox::Cancel : QDialogButtonBox::Apply)->click();
        });
        if (!drawn_redraw) return window.redefineSelectedBoundary(triangle);
        auto* canvas = window.findChild<QWidget*>("measurementPlanCanvas");
        require(canvas, "drawn redraw canvas must exist");
        QKeyEvent finish(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas, &finish);
        return !dynamic_cast<desktop::PlanCanvas*>(canvas)->boundaryDraftPreview().has_value();
    };
    require(!review(true,true,{}) && window.document().revision() == before.revision() &&
        window.document().snapshot().entities() == before.entities(), "cancel must preserve revision and all entities");
    require(review(false,false,"redraw-auto-angle-keep.png"), "mapped automatic angle Keep must apply");
    const auto kept = window.document().snapshot();
    const auto kept_geometry = decode_identified_boundary_entity(kept.entities().at(id.toStdString()));
    const auto kept_angle = *decode_boundary_dimension_entity(kept.entities().at(angle.id)).dimension;
    require(kept_angle.segment_id == kept_geometry.segments[0].segment_id &&
        kept_angle.secondary_segment_id == kept_geometry.segments[1].segment_id &&
        std::isfinite(kept_angle.resolve(kept.entities().at(id.toStdString())).angle()), "kept angle must resolve on mapped replacement children");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(), "mapped Keep must undo in one step");
    QTemporaryDir project;
    require(project.isValid() && window.selectEntity(id), "drawn redraw fixture must have a selected target");
    auto* redraw = window.findChild<QAction*>("boundaryRedefinition");
    auto* canvas = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(redraw && canvas, "real redraw action and canvas must exist");
    window.setMetricUnits(true);
    window.fitView();
    QApplication::processEvents();
    redraw->trigger();
    const QRectF viewport(canvas->rect());
    for (const Vec2 model : {Vec2{0,0}, Vec2{4,0}, Vec2{4,3}}) {
        const QPointF point(viewport.center().x() + (model.x-canvas->viewCenter().x)*canvas->viewScale(),
            viewport.center().y() - (model.y-canvas->viewCenter().y)*canvas->viewScale());
        require(viewport.adjusted(1,1,-1,-1).contains(point), "redraw click must be inside the laid out canvas");
        QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
    }
    require(canvas->boundaryDraftPreview().has_value(), "canvas clicks must create an actual redraw draft");
    const auto draft_path = project.filePath("redraw-draft.bldproj");
    require(window.saveProjectAs(draft_path) && window.openProject(draft_path) && canvas->boundaryDraftPreview(),
        "unfinished redraw checkpoint must save and recover into the canvas");
    drawn_redraw = true;
    const auto cancel_revision = window.document().revision();
    require(!review(true,true,{}) && window.document().revision() == cancel_revision &&
        window.document().snapshot().entities() == before.entities() &&
        canvas->boundaryDraftPreview(), "Finish review Cancel must preserve source and recovered redraw draft for retry");
    require(review(true,false,"redraw-auto-angle-remove.png"), "retried drawn Finish must apply automatic angle Remove");
    const auto after = window.document().snapshot();
    require(!after.entities().contains(angle.id) && after.entities().contains(manual.id) &&
        after.entities().at(area.id) == before.entities().at(area.id) &&
        after.entities().at(other_length.id) == before.entities().at(other_length.id) &&
        after.entities().at(other.id) == before.entities().at(other.id), "Remove must preserve manual and unrelated references");
    for (const auto& [ref,entity] : after.entities()) if (entity.type == "dimension") {
        const auto dimension = *decode_boundary_dimension_entity(entity).dimension;
        if (dimension.boundary_id != id.toStdString()) continue;
        const auto resolved = dimension.resolve(after.entities().at(id.toStdString()));
        if (dimension.kind == BoundaryDimensionKind::segment_length)
            require(std::isfinite(resolved.segment_length()), "replacement edge length calculation must be current");
        if (dimension.kind == BoundaryDimensionKind::area)
            require(std::abs(resolved.area()-6.0)<1e-9, "retained area dimension must calculate replacement area");
    }
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == after.entities(), "Remove must undo and redo atomically");
    const auto& edit = after.history().back().boundary_geometry_edit;
    require(edit && edit->allow_automatic_angle_removal && encode_boundary_geometry_edit(*edit).at("version") == 5,
        "drawn Finish must retain the strict version-five automatic-angle removal proof");
    require(window.undoCommand(), "Undo Finish must retain its reviewed checkpoint");
    require(!canvas->boundaryDraftPreview(), "Undo Finish must retain completed input without restarting drawing");
    if (!window.saveProjectAs(project.filePath("redraw-undone.bldproj")))
        throw std::runtime_error("Save undone Finish: " + window.lastError().toStdString());
    const auto undone = ProjectStore::load_archive(std::filesystem::path(project.filePath("redraw-undone.bldproj").toStdWString()), ArchiveRole::ordinary);
    require(undone.supported() && !undone.recovery.decoded->active &&
        undone.recovery.decoded->history && undone.recovery.decoded->history->retired.size() == 1,
        "saved Undo Finish must expose the archived retired checkpoint");
    const auto& operation = undone.recovery.decoded->history->retired.begin()->second.value
        ->extensions.at("desktop_operation");
    require(operation.at("version") == 3 && operation.at("allow_automatic_angle_removal") == true &&
        operation.at("replacement_removed_reference_ids") == nlohmann::json::array({angle.id}),
        "archived desktop_operation v3 must retain the explicit automatic-angle removal decision");
    require(window.openProject(project.filePath("redraw-undone.bldproj")) && window.redoCommand() &&
        window.document().snapshot().entities() == after.entities(), "recovered reviewed checkpoint must redo Finish exactly");
    desktop::MainWindow reopened;
    reopened.setAttribute(Qt::WA_DontShowOnScreen);
    require(project.isValid() && window.saveProjectAs(project.filePath("redraw-angle.bldproj")) &&
        window.createNewProject() && reopened.openProject(project.filePath("redraw-angle.bldproj")) &&
        reopened.document().snapshot().is_editable() &&
        reopened.document().snapshot().entities() == after.entities(), "reviewed removal must save and reopen exactly");
    require(reopened.undoCommand() && reopened.document().snapshot().entities() == before.entities() &&
        reopened.redoCommand() && reopened.document().snapshot().entities() == after.entities(),
        "saved drawn Finish must retain atomic Undo and Redo after reopen");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc,argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("RedrawAutomaticAngle");
    const auto font = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    if (font >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(),10));
    try { exercise(); std::cout << "redraw automatic angle desktop checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
