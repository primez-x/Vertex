#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/geometry_operations.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using sketch::desktop::MainWindow;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

sketch::Boundary tall_rectangle() {
    return {{{0, 0}, {4, 0}, 0}, {{4, 0}, {4, 12}, 0},
            {{4, 12}, {0, 12}, 0}, {{0, 12}, {0, 0}, 0}};
}

template <class Widget>
Widget& child(QWidget& owner, const char* name) {
    auto* value = owner.findChild<Widget*>(QString::fromLatin1(name));
    require(value != nullptr, "actual boundary editing control must exist");
    return *value;
}

void choose_data(QComboBox& combo, const QString& value) {
    const auto index = combo.findData(value);
    require(index >= 0, "actual boundary editor must offer the requested stable target");
    combo.setCurrentIndex(index);
}

void capture(QWidget& widget, const QString& filename) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) &&
                widget.grab().save(QDir(directory).filePath(filename)),
            "actual boundary editing dialog capture must save");
}

void modal_interaction(QWidget& owner, const char* object_name,
                       const std::function<void()>& launch,
                       const std::function<void(QDialog&)>& interact) {
    std::exception_ptr failure;
    bool opened = false;
    QTimer::singleShot(0, &owner, [&] {
        auto* dialog = owner.findChild<QDialog*>(QString::fromLatin1(object_name));
        try {
            require(dialog != nullptr, "actual Tools control must open its named dialog");
            opened = true;
            interact(*dialog);
        } catch (...) {
            failure = std::current_exception();
        }
        if (dialog && dialog->isVisible()) dialog->reject();
    });
    launch();
    if (failure) std::rethrow_exception(failure);
    require(opened, "actual Tools control must execute its modal dialog");
}

struct DimensionEntry {
    std::string id;
    sketch::BoundaryDimension dimension;
};

std::optional<DimensionEntry> length_dimension(
    const sketch::DocumentSnapshot& snapshot, const QString& boundary_id,
    const std::string& segment_id) {
    for (const auto& [id, entity] : snapshot.entities()) {
        if (!sketch::can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded = sketch::decode_boundary_dimension_entity(entity);
        if (decoded.dimension &&
            decoded.dimension->kind == sketch::BoundaryDimensionKind::segment_length &&
            decoded.dimension->boundary_id == boundary_id.toStdString() &&
            decoded.dimension->segment_id == segment_id) {
            return DimensionEntry{id, *decoded.dimension};
        }
    }
    return std::nullopt;
}

DimensionEntry require_length_dimension(
    const sketch::DocumentSnapshot& snapshot, const QString& boundary_id,
    const std::string& segment_id) {
    const auto found = length_dimension(snapshot, boundary_id, segment_id);
    require(found.has_value(), "source edge must have its saved length dimension");
    return *found;
}

bool same_point(sketch::Vec2 left, sketch::Vec2 right) {
    return left.x == right.x && left.y == right.y;
}

void add_automatic_length_dimension(MainWindow& window,
                                   const QString& boundary_id,
                                   const QString& segment_id,
                                   const QString& capture_name) {
    auto* action = window.findChild<QAction*>(QStringLiteral("dimensionCreator"));
    require(action && action->isEnabled(), "Tools must expose the actual dimension creator action");
    modal_interaction(window, "dimensionCreatorDialog",
        [&] { action->trigger(); }, [&](QDialog& dialog) {
            auto& boundary = child<QComboBox>(dialog, "dimensionSourceBoundary");
            choose_data(boundary, boundary_id);
            QApplication::processEvents();
            auto& segment = child<QComboBox>(dialog, "dimensionFirstSegment");
            choose_data(segment, segment_id);
            auto& automatic = child<QCheckBox>(dialog, "dimensionAutomaticPlacement");
            require(automatic.isChecked(), "length dimensions must default to automatic exterior placement");
            capture(dialog, capture_name);

            const auto before = window.document().snapshot();
            child<QPushButton>(dialog, "addLengthDimension").click();
            QApplication::processEvents();
            const auto after = window.document().snapshot();
            require(after.revision() == before.revision() + 1,
                    "adding a length dimension must create one real document revision");
            const auto entry = require_length_dimension(after, boundary_id,
                                                        segment_id.toStdString());
            const auto owner = after.entities().at(boundary_id.toStdString());
            require(entry.dimension.placement == sketch::BoundaryDimensionPlacement::automatic &&
                        entry.dimension.automatic_placement_version.has_value() &&
                        entry.dimension.text_position.y < 0 &&
                        std::abs(entry.dimension.resolve(owner).segment_length_metres - 4.0) < 1e-9,
                    "automatic length dimension must retain its stable edge and sit outside the tall boundary");
            require(!child<QLabel>(dialog, "dimensionCreatorStatus").text().isEmpty(),
                    "actual dimension creator must report the committed result");
        });
}

void length_dimension_delete_recreate_and_restore() {
    QTemporaryDir directory;
    require(directory.isValid(), "length dimension fixture needs a temporary project directory");
    MainWindow window({}, nullptr, directory.filePath(QStringLiteral("text-library.json")));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    const auto boundary_id = window.createBoundary(tall_rectangle());
    require(!boundary_id.isEmpty(), "length dimension fixture needs a real boundary");
    const auto source = sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(boundary_id.toStdString()));
    const auto segment_id = QString::fromStdString(source.segments.front().segment_id);
    window.resize(1100, 780);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(boundary_id), "select boundary before creating its length dimension");

    add_automatic_length_dimension(window, boundary_id, segment_id,
                                   QStringLiteral("boundary-dimension-creator.png"));
    const auto first = require_length_dimension(window.document().snapshot(), boundary_id,
                                                segment_id.toStdString());
    const auto before_delete = window.document().snapshot();
    require(window.selectEntity(QString::fromStdString(first.id)), "select the edge dimension to remove");
    auto& deletion = child<QAction>(window, "deleteSelection");
    require(deletion.isEnabled(), "actual Delete action must be available for a dimension");
    deletion.trigger();
    const auto deleted = window.document().snapshot();
    require(deleted.revision() == before_delete.revision() + 1 &&
                !length_dimension(deleted, boundary_id, segment_id.toStdString()),
            "deleting a selected edge dimension must remove only that dimension");
    require(window.undoCommand() &&
                window.document().snapshot().entities() == before_delete.entities(),
            "undo must restore the deleted dimension and its stable edge target");
    require(window.redoCommand() &&
                !length_dimension(window.document().snapshot(), boundary_id,
                                  segment_id.toStdString()),
            "redo must remove the same dimension again");

    add_automatic_length_dimension(window, boundary_id, segment_id,
                                   QStringLiteral("boundary-dimension-recreated.png"));
    const auto restored = window.document().snapshot();
    const auto restored_dimension = require_length_dimension(restored, boundary_id,
                                                              segment_id.toStdString());
    require(restored_dimension.dimension.placement == sketch::BoundaryDimensionPlacement::automatic &&
                restored_dimension.dimension.text_position.y < 0,
            "recreated length dimension must retain automatic exterior placement");

    const auto project_path = directory.filePath(QStringLiteral("dimension-restoration.bldproj"));
    require(window.saveProjectAs(project_path), "dimension project must save through MainWindow");
    MainWindow reopened({}, nullptr, directory.filePath(QStringLiteral("missing-library.json")));
    const auto opened = reopened.openProject(project_path);
    if (!opened)
        std::cerr << "Dimension reopen failed: " << reopened.lastError().toStdString() << '\n';
    require(opened && reopened.document().snapshot().entities() == restored.entities(),
            "saved length dimension project must reopen with its original entities");
}

void boundary_point_removal_workflow() {
    QTemporaryDir directory;
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("library.json")));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.setMetricUnits(true);
    const auto id=window.createBoundary(tall_rectangle());
    require(!id.isEmpty(),"point removal needs a real identified boundary");
    window.resize(1100,780);window.show();QApplication::processEvents();
    require(window.selectEntity(id),"select removal source");
    auto& workflow=child<QComboBox>(window,"calculationWorkflow");
    choose_data(workflow,QStringLiteral("appraisal"));
    require(window.editSelectedAppraisalFacts(QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})")),
        "declare source appraisal facts");
    const auto original=window.document().snapshot();
    const auto original_report=sketch::build_appraisal_document_report(original,"property-1",sketch::AreaUnit::square_metre);
    require(original_report.qualified && original_report.calculation &&
        std::abs(original_report.calculation->property.gla().total.square_metres-48)<1e-7,
        "source appraisal must calculate 48 square metres");
    const auto boundary=sketch::decode_identified_boundary_entity(original.entities().at(id.toStdString()));
    auto& action=child<QAction>(window,"removeBoundaryVertex");
    modal_interaction(window,"boundaryVertexRemovalDialog",[&]{action.trigger();},[&](QDialog& dialog){
        auto& points=child<QComboBox>(dialog,"boundaryRemovalPoint");
        choose_data(points,QString::fromStdString(boundary.segments[1].start_vertex_id));
        require(child<QDialogButtonBox>(dialog,"boundaryRemovalButtons").button(QDialogButtonBox::Apply)->isEnabled(),
            "valid removal preview must enable Apply before Cancel");
    });
    require(window.document().snapshot().entities()==original.entities(),"Cancel must preserve source entities");
    modal_interaction(window,"boundaryVertexRemovalDialog",[&]{action.trigger();},[&](QDialog& dialog){
        choose_data(child<QComboBox>(dialog,"boundaryRemovalPoint"),QString::fromStdString(boundary.segments[1].start_vertex_id));
        auto& buttons=child<QDialogButtonBox>(dialog,"boundaryRemovalButtons");
        require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),"valid removed point preview must enable Apply");
        const auto* scene=dynamic_cast<sketch::desktop::PlanCanvas*>(&child<QWidget>(dialog,"boundaryRemovalPreview"));
        require(scene && scene->entities().size()==2 && scene->entities().back().segments.size()==3,
            "actual removal preview must show original and triangle replacement");
        capture(dialog,QStringLiteral("boundary-point-removal-preview.png"));
        buttons.button(QDialogButtonBox::Apply)->click();
    });
    const auto changed=window.document().snapshot();
    require(changed.revision()==original.revision()+1,"removal must commit once");
    const auto result=sketch::decode_identified_boundary_entity(changed.entities().at(id.toStdString()));
    require(result.id==boundary.id && result.segments.size()==3 &&
        std::abs(sketch::signed_area(sketch::boundary_geometry(result))-24)<1e-7,
        "removal must retain boundary with expected triangle area");
    const auto changed_report=sketch::build_appraisal_document_report(changed,"property-1",sketch::AreaUnit::square_metre);
    require(changed_report.qualified && changed_report.calculation &&
        std::abs(changed_report.calculation->property.gla().total.square_metres-24)<1e-7 &&
        changed.entities().at(id.toStdString()).properties.at("appraisal_facts")==original.entities().at(id.toStdString()).properties.at("appraisal_facts"),
        "removal must update declared GLA and preserve facts");
    require(window.undoCommand() && window.document().snapshot().entities()==original.entities(),"removal Undo must be exact");
    require(window.redoCommand() && window.document().snapshot().entities()==changed.entities(),"removal Redo must be exact");
    const auto path=directory.filePath(QStringLiteral("point-removal.bldproj"));
    require(window.saveProjectAs(path),"removed point project must save");
    {
        MainWindow read_only({},nullptr,directory.filePath(QStringLiteral("readonly-library.json")));
        require(read_only.openProject(path) && !read_only.document().is_editable() && read_only.selectEntity(id),
            "second open must retain the writer's ownership and be read-only");
        const auto owned=read_only.document().snapshot();
        const auto current=sketch::decode_identified_boundary_entity(owned.entities().at(id.toStdString()));
        require(!read_only.removeSelectedBoundaryVertex(QString::fromStdString(current.segments.front().start_vertex_id)) &&
            !read_only.undoCommand() && read_only.document().snapshot().entities()==owned.entities(),
            "read-only point editing and history must refuse unchanged");
    }
    require(window.createNewProject(),"release writer ownership before editable reopening");
    MainWindow reopened({},nullptr,directory.filePath(QStringLiteral("other-library.json")));
    require(reopened.openProject(path) && reopened.document().is_editable() && reopened.document().snapshot().entities()==changed.entities(),
        "point removal must reopen with exact entities");
    require(reopened.undoCommand() && reopened.document().snapshot().entities()==original.entities(),
        "reopened history must restore removed point");
    require(reopened.selectEntity(id),"reselect restored boundary");
    const auto restored=reopened.document().snapshot();
    require(!reopened.removeSelectedBoundaryVertex(QStringLiteral("missing")) &&
        reopened.document().snapshot().entities()==restored.entities(),"unknown point must refuse unchanged");

    require(reopened.redoCommand() && reopened.selectEntity(id),"restore triangle for invalid removal");
    const auto triangle=reopened.document().snapshot();
    auto& reopened_action=child<QAction>(reopened,"removeBoundaryVertex");
    modal_interaction(reopened,"boundaryVertexRemovalDialog",[&]{reopened_action.trigger();},[&](QDialog& dialog){
        require(!child<QDialogButtonBox>(dialog,"boundaryRemovalButtons").button(QDialogButtonBox::Apply)->isEnabled(),
            "triangle cannot lose a corner into a line");
        require(!child<QLabel>(dialog,"boundaryRemovalStatus").text().isEmpty(),"invalid removal must explain refusal");
    });
    require(reopened.document().snapshot().entities()==triangle.entities(),"invalid removal must preserve source");
}

void boundary_point_removal_reference_review() {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    const auto id=window.createBoundary(tall_rectangle());
    const auto boundary=sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(id.toStdString()));
    const auto kept=window.createLengthDimension(id,QString::fromStdString(boundary.segments[0].segment_id),{2,-1});
    const auto retired=window.createLengthDimension(id,QString::fromStdString(boundary.segments[1].segment_id),{5,6});
    require(!kept.isEmpty() && !retired.isEmpty() && window.selectEntity(id),"removal needs retained and retired manual references");
    const auto before=window.document().snapshot();
    bool removed=false;
    modal_interaction(window,"boundaryReferenceReview",[&]{
        removed=window.removeSelectedBoundaryVertex(QString::fromStdString(boundary.segments[1].start_vertex_id));
    },[&](QDialog& dialog){
        auto& choices=child<QTableWidget>(dialog,"boundaryReferenceChoices");
        auto& buttons=child<QDialogButtonBox>(dialog,"boundaryReferenceButtons");
        require(!buttons.button(QDialogButtonBox::Apply)->isEnabled(),"retired reference must require explicit review");
        bool found_kept=false,found_retired=false;
        for(int row=0;row<choices.rowCount();++row) {
            const auto ref=choices.item(row,0)->data(Qt::UserRole).toString();
            auto* decision=qobject_cast<QComboBox*>(choices.cellWidget(row,1));
            require(decision!=nullptr,"reference must have a decision");
            if(ref==kept) {found_kept=true;require(decision->currentIndex()==1,"surviving stable edge must default to Keep");}
            if(ref==retired) {found_retired=true;require(decision->currentIndex()==0,"retired edge must await explicit decision");decision->setCurrentIndex(2);}
        }
        require(found_kept && found_retired && buttons.button(QDialogButtonBox::Apply)->isEnabled(),
            "explicit removal with surviving identity must preview successfully");
        for(const auto* name:{"boundaryReferenceOriginal","boundaryReferenceProposed"}) {
            const auto* scene=dynamic_cast<sketch::desktop::PlanCanvas*>(&child<QWidget>(dialog,name));
            require(scene!=nullptr,"reference review needs a plan canvas");
            const auto center=scene->viewCenter();
            for(const auto& entity:scene->entities()) for(const auto& segment:entity.segments) {
                const QPointF position(scene->rect().center().x()+(segment.start.x-center.x)*scene->viewScale(),
                    scene->rect().center().y()-(segment.start.y-center.y)*scene->viewScale());
                require(QRectF(scene->rect()).adjusted(10,10,-10,-10).contains(position),
                    "every source and replacement corner must fit inside the laid-out review canvas");
            }
        }
        capture(dialog,QStringLiteral("boundary-point-removal-references.png"));
        buttons.button(QDialogButtonBox::Apply)->click();
    });
    const auto after=window.document().snapshot();
    require(removed && after.revision()==before.revision()+1 && after.entities().contains(kept.toStdString()) &&
        !after.entities().contains(retired.toStdString()),"reference review and geometry must commit together");
    const auto retained=*sketch::decode_boundary_dimension_entity(after.entities().at(kept.toStdString())).dimension;
    const auto replacement=sketch::decode_identified_boundary_entity(after.entities().at(id.toStdString()));
    require(retained.segment_id==replacement.segments[0].segment_id &&
        std::abs(retained.resolve(after.entities().at(id.toStdString())).segment_length()-std::sqrt(160.0))<1e-7,
        "kept manual dimension must resolve the merged diagonal");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"Undo must restore retired reference exactly");

    const auto other=window.createBoundary({{{20,0},{22,0},0},{{22,0},{22,2},0},{{22,2},{20,2},0},{{20,2},{20,0},0}});
    require(window.selectEntity(id) && window.selectEntity(other,true),"select two boundaries");
    const auto multi=window.document().snapshot();
    require(!window.removeSelectedBoundaryVertex(QString::fromStdString(boundary.segments[1].start_vertex_id)) &&
        window.document().snapshot().entities()==multi.entities(),"multiple selection must refuse point removal");
    require(window.selectEntity(id),"select stale-preview source");
    auto& action=child<QAction>(window,"removeBoundaryVertex");
    modal_interaction(window,"boundaryVertexRemovalDialog",[&]{action.trigger();},[&](QDialog& dialog){
        require(window.selectEntity(other),"change selection while point preview is open");
        auto& buttons=child<QDialogButtonBox>(dialog,"boundaryRemovalButtons");
        buttons.button(QDialogButtonBox::Apply)->click();
        require(!buttons.button(QDialogButtonBox::Apply)->isEnabled(),"stale selection must disable Apply");
    });
    require(window.document().snapshot().entities()==multi.entities(),"stale preview must preserve both objects");
}

void boundary_point_removal_restores_split_arc() {
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    const auto id=window.createBoundary({{{0,0},{4,0},std::numbers::pi},{{4,0},{0,0},std::numbers::pi}});
    require(!id.isEmpty(),"split arc fixture must create a circle");
    const auto original=sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(id.toStdString()));
    require(window.selectEntity(id) && window.insertSelectedBoundaryVertex(
        QString::fromStdString(original.segments.front().segment_id),QStringLiteral("0.4")),"insert an arc point through desktop command");
    const auto split=window.document().snapshot();
    const auto split_boundary=sketch::decode_identified_boundary_entity(split.entities().at(id.toStdString()));
    require(window.removeSelectedBoundaryVertex(QString::fromStdString(split_boundary.segments[1].start_vertex_id)),
        "remove inserted arc point through desktop command");
    const auto restored=sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(id.toStdString()));
    require(restored.id==original.id && restored.segments.size()==2 &&
        std::abs(restored.segments[0].segment.sweep_radians-std::numbers::pi)<1e-9 &&
        std::abs(sketch::perimeter(sketch::boundary_geometry(restored))-4*std::numbers::pi)<1e-9,
        "removal must merge the split semicircle analytically without a chord shortcut");
    require(window.undoCommand() && window.document().snapshot().entities()==split.entities(),"arc removal must undo exactly");
}

void default_length_operation_remains_available() {
    QTemporaryDir directory;
    require(directory.isValid(), "length editor fixture needs a temporary project directory");
    MainWindow window({}, nullptr, directory.filePath(QStringLiteral("text-library.json")));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    const auto boundary_id = window.createBoundary(tall_rectangle());
    require(!boundary_id.isEmpty(), "length editor fixture needs a real boundary");
    const auto before = window.document().snapshot();
    const auto source = sketch::decode_identified_boundary_entity(
        before.entities().at(boundary_id.toStdString()));
    const auto original_edge = source.segments.front();
    window.resize(1100, 780);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(boundary_id), "select boundary before opening geometry editor");
    auto& button = child<QPushButton>(window, "editBoundaryGeometry");
    require(button.isEnabled(), "selected boundary must expose the actual geometry editor button");
    modal_interaction(window, "boundaryGeometryDialog",
        [&] { button.click(); }, [&](QDialog& dialog) {
            auto& operation = child<QComboBox>(dialog, "boundaryEditOperation");
            require(operation.currentData().toString() == QStringLiteral("length"),
                    "existing boundary length editing must remain the default operation");
            auto& edge = child<QComboBox>(dialog, "boundaryEdge");
            choose_data(edge, QString::fromStdString(original_edge.segment_id));
            auto& fixed = child<QComboBox>(dialog, "boundaryFixedEndpoint");
            require(fixed.currentData().toString() == QStringLiteral("start"),
                    "legacy length editing must keep its start-anchor default");
            auto& connected = child<QCheckBox>(dialog, "boundaryMoveConnected");
            require(!connected.isChecked(), "legacy length editing must keep its disconnected default");
            auto& length = child<QLineEdit>(dialog, "boundaryEdgeLength");
            length.setText(QStringLiteral("5 m"));
            QApplication::processEvents();
            require(window.document().snapshot().revision() == before.revision() &&
                        window.document().snapshot().entities() == before.entities(),
                    "length preview must not mutate the live boundary");
            auto& summary = child<QLabel>(dialog, "boundaryGeometrySummary");
            require(!summary.text().isEmpty(), "length editor must produce a visible analytical preview");
            auto& buttons = child<QDialogButtonBox>(dialog, "boundaryGeometryButtons");
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),
                    "valid length preview must enable the actual Apply control");
            capture(dialog, QStringLiteral("boundary-length-preview.png"));
            buttons.button(QDialogButtonBox::Apply)->click();
        });

    const auto resized = window.document().snapshot();
    const auto updated = sketch::decode_identified_boundary_entity(
        resized.entities().at(boundary_id.toStdString()));
    require(resized.revision() == before.revision() + 1 &&
                updated.segments.front().segment_id == original_edge.segment_id &&
                updated.segments.front().start_vertex_id == original_edge.start_vertex_id &&
                updated.segments.front().end_vertex_id == original_edge.end_vertex_id &&
                same_point(updated.segments.front().segment.start, original_edge.segment.start) &&
                std::abs(sketch::segment_length(updated.segments.front().segment) - 5.0) < 1e-9,
            "default Length Apply must preserve the original edge identity and fixed start point");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == resized.entities(),
            "default Length Apply must remain one undoable and redoable geometry edit");
}

void exact_vertex_editor(bool metric) {
    QTemporaryDir directory;
    require(directory.isValid(), "exact corner fixture needs a project directory");
    MainWindow window({}, nullptr, directory.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(metric);
    const auto area_id = window.createBoundary(tall_rectangle(), "finished");
    const auto wall_id = window.createStraightWall({4,0}, {7,0});
    const auto neighbor_id = window.createBoundary({{{4,0},{7,0},0},{{7,0},{7,-3},0},
        {{7,-3},{4,-3},0},{{4,-3},{4,0},0}}, "garage");
    const auto unrelated_id = window.createStraightWall({20,0}, {23,0});
    require(!area_id.isEmpty() && !wall_id.isEmpty() && !neighbor_id.isEmpty() && !unrelated_id.isEmpty(),
        "exact corner fixture creates related owners and unrelated work");
    const auto initial = window.document().snapshot();
    const auto original = sketch::decode_identified_boundary_entity(initial.entities().at(area_id.toStdString()));
    const auto neighbor = sketch::decode_identified_boundary_entity(initial.entities().at(neighbor_id.toStdString()));
    const auto& target_edge = original.segments[1];
    const sketch::WallEndpointBinding corner{area_id.toStdString(), sketch::WallEndpointRole::start,
        target_edge.segment_id, target_edge.start_vertex_id};
    const sketch::PersistentConstraint wall_join{"exact-corner-wall-joint", sketch::ConstraintRelationKind::coincident,
        {corner, {wall_id.toStdString(), sketch::WallEndpointRole::start}}, std::nullopt, std::nullopt};
    const sketch::PersistentConstraint area_join{"exact-corner-area-joint", sketch::ConstraintRelationKind::coincident,
        {corner, {neighbor_id.toStdString(), sketch::WallEndpointRole::start,
            neighbor.segments.front().segment_id, neighbor.segments.front().start_vertex_id}}, std::nullopt, std::nullopt};
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
        {sketch::EntityChange::upsert(sketch::encode_constraint_entity(wall_join)),
         sketch::EntityChange::upsert(sketch::encode_constraint_entity(area_join))}, {}, "Exact corner relationships fixture"});
    require(!window.createLengthDimension(area_id, QString::fromStdString(original.segments.front().segment_id), {2,-1}).isEmpty(),
        "exact corner fixture retains a dependent dimension");
    window.resize(1100,780);
    window.show();
    QApplication::processEvents();
    const auto before = window.document().snapshot();
    const sketch::Vec2 expected = metric ? sketch::Vec2{5.125,-0.5} : sketch::Vec2{16.5*0.3048,-1.5*0.3048};
    const auto edit = [&](bool apply) {
        window.setWorkspaceTheme(apply ? sketch::WorkspaceTheme::dark : sketch::WorkspaceTheme::light);
        require(window.selectEntity(area_id), "select completed boundary for exact corner editing");
        auto& button = child<QPushButton>(window,"editBoundaryGeometry");
        modal_interaction(window,"boundaryGeometryDialog",[&] { button.click(); },[&](QDialog& dialog) {
            choose_data(child<QComboBox>(dialog,"boundaryEditOperation"), QStringLiteral("vertex"));
            auto& selector = child<QComboBox>(dialog,"boundaryVertex");
            choose_data(selector, QString::fromStdString(target_edge.start_vertex_id));
            auto& x = child<QLineEdit>(dialog,"boundaryVertexX");
            auto& y = child<QLineEdit>(dialog,"boundaryVertexY");
            auto& buttons = child<QDialogButtonBox>(dialog,"boundaryGeometryButtons");
            require(selector.isVisible() && x.isVisible() && y.isVisible() &&
                !child<QComboBox>(dialog,"boundaryEdge").isVisible() &&
                !child<QLineEdit>(dialog,"boundaryEdgeLength").isVisible(),
                "Vertex position must expose corner and accessible X/Y controls");
            require(!x.accessibleName().isEmpty() && !y.accessibleName().isEmpty() &&
                std::abs(sketch::parse_quantity(x.text().toStdString()).metres-4)<1e-12 &&
                !buttons.button(QDialogButtonBox::Apply)->isEnabled(),
                "current exact coordinate defaults must preserve an unchanged corner as a no-op");
            x.setText(QStringLiteral("not a coordinate"));
            require(!buttons.button(QDialogButtonBox::Apply)->isEnabled() &&
                !child<QLabel>(dialog,"boundaryGeometryStatus").text().isEmpty(),
                "invalid exact coordinates must refuse Apply with a visible explanation");
            // Bare expressions deliberately test the active metric/imperial input default.
            x.setText(metric ? QStringLiteral("5.125") : QStringLiteral("16.5"));
            y.setText(metric ? QStringLiteral("-0.5") : QStringLiteral("-1.5"));
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled() &&
                child<QLabel>(dialog,"boundaryGeometrySummary").text().contains(QStringLiteral("Corner 2:")) &&
                child<QTableWidget>(dialog,"boundaryGeometryChanges").rowCount() >= 4,
                "exact corner preview must show before/after coordinates and related movement");
            auto& related = child<QCheckBox>(dialog,"boundaryMoveRelatedObjects");
            require(related.isVisible() && related.isEnabled() && related.isChecked(),
                "vertex movement must offer saved related-object propagation");
            related.setChecked(false);
            require(!buttons.button(QDialogButtonBox::Apply)->isEnabled() &&
                !child<QLabel>(dialog,"boundaryGeometryStatus").text().isEmpty(),
                "a retained shared corner must refuse a conflicting exact move");
            related.setChecked(true);
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled() &&
                window.document().snapshot().entities()==before.entities() &&
                window.document().snapshot().revision()==before.revision(),
                "restored exact preview must keep live geometry and history unchanged");
            capture(dialog, QStringLiteral("exact-vertex-%1-%2.png").arg(metric ? "metric" : "imperial",
                apply ? "dark" : "light"));
            buttons.button(apply ? QDialogButtonBox::Apply : QDialogButtonBox::Cancel)->click();
        });
    };
    edit(false);
    require(window.document().snapshot().entities()==before.entities() &&
        window.document().revision()==before.revision(), "exact corner Cancel must preserve all owners and history");
    edit(true);
    const auto after = window.document().snapshot();
    const auto edited = sketch::decode_identified_boundary_entity(after.entities().at(area_id.toStdString()));
    require(after.revision()==before.revision()+1 &&
        std::hypot(edited.segments[1].segment.start.x-expected.x,edited.segments[1].segment.start.y-expected.y)<1e-12 &&
        same_point(edited.segments.front().segment.end,edited.segments[1].segment.start),
        "one exact corner Apply must set both incident edges to the requested coordinates");
    for (std::size_t i=0; i<original.segments.size(); ++i)
        require(edited.segments[i].segment_id==original.segments[i].segment_id &&
            edited.segments[i].start_vertex_id==original.segments[i].start_vertex_id &&
            edited.segments[i].end_vertex_id==original.segments[i].end_vertex_id &&
            edited.segments[i].segment.sweep_radians==original.segments[i].segment.sweep_radians,
            "exact movement must retain stable edge/corner identities and signed sweeps");
    const auto moved_neighbor = sketch::decode_identified_boundary_entity(after.entities().at(neighbor_id.toStdString()));
    const auto& moved_wall = after.entities().at(wall_id.toStdString()).properties.at("baseline").at("start");
    require(std::hypot(moved_neighbor.segments.front().segment.start.x-expected.x,
                       moved_neighbor.segments.front().segment.start.y-expected.y)<1e-7 &&
        std::hypot(moved_wall[0].get<double>()-expected.x,moved_wall[1].get<double>()-expected.y)<1e-7 &&
        after.entities().at(wall_join.id)==before.entities().at(wall_join.id) &&
        after.entities().at(area_join.id)==before.entities().at(area_join.id) &&
        after.entities().at(unrelated_id.toStdString())==before.entities().at(unrelated_id.toStdString()),
        "exact move must propagate saved shared corners while preserving relationships and unrelated work");
    require(after.history().back().boundary_constraint_changes.has_value() &&
        window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),
        "exact corner and all related movement must use one typed atomic Undo/Redo command");
    const auto project = directory.filePath(metric ? "exact-vertex-metric.bldproj" : "exact-vertex-imperial.bldproj");
    require(window.saveProjectAs(project), "exact corner project must persist its typed edit and history");
    // A second MainWindow intentionally opens a currently owned project read-only.
    // Release the writer's lease before exercising restored editing/history.
    require(window.createNewProject(), "exact corner persistence fixture must release project ownership before reopening");
    MainWindow reopened({}, nullptr, directory.filePath("missing-library.json"));
    require(reopened.openProject(project) && reopened.document().is_editable() &&
        reopened.document().snapshot().entities()==after.entities(),
        "exact coordinates and dependencies must survive persisted reopening with editable ownership");
    require(reopened.undoCommand() && reopened.document().snapshot().entities()==before.entities(),
        "one restored Undo must atomically restore exact corner and all related owners");
    require(reopened.redoCommand() && reopened.document().snapshot().entities()==after.entities(),
        "one restored Redo must atomically restore persisted exact corner and all related owners");
}

void source_corner_editor(bool metric) {
    QTemporaryDir directory;
    require(directory.isValid(), "source corner test has an isolated project directory");
    MainWindow window({}, nullptr, directory.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(metric);
    window.resize(1100,780);window.show();QApplication::processEvents();
    const QStringList walls{window.createStraightWall({0,0},{6,0},"exterior"),
        window.createStraightWall({6,0},{6,4},"exterior"),window.createStraightWall({6,4},{0,4},"exterior"),
        window.createStraightWall({0,4},{0,0},"exterior")};
    for (qsizetype i=0;i<walls.size();++i) {
        require(!walls[i].isEmpty() && window.selectEntity(walls[i]) &&
            window.editSelectedThickness(i==0 ? "200 mm" : "150 mm"),
            "physical perimeter retains differing wall thickness");
    }
    for (qsizetype i=0;i<walls.size();++i)
        require(window.selectEntity(walls[i],i!=0),"select exterior source walls");
    const auto area=window.createMeasurementBoundaryFromSelectedWalls();
    require(!area.isEmpty(),"derive current measured exterior");
    const auto declaration=QStringLiteral(R"({"appraisal_policy":{"policy_kind":"ansi_z765_2021","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior","ansi":{"interior_inspected":true,"direct_measurement":true,"acquisition_increment":"tenth_foot","limitations_statement":"No inaccessible portions in this fixture."}},"floor_appraisal_facts":{"grade":"above","ansi":{"any_part_below_grade":false}},"appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area","ansi":{"year_round_suitable":true,"finish_matches_dwelling":true,"dwelling_identity":"primary","ceiling":{"kind":"flat","minimum_height_m":2.4384}}}})");
    require(window.editSelectedAppraisalFacts(declaration),"declare measured area for automatic GLA");
    modal_interaction(window,"appraisalSetupDialog",[&]{child<QPushButton>(window,"appraisalDetailsSetup").click();},[&](QDialog& dialog){
        child<QCheckBox>(dialog,"appraisalSetupEnabled").setChecked(true);
        child<QDialogButtonBox>(dialog,"appraisalSetupButtons").button(QDialogButtonBox::Save)->click();
        require(dialog.result()==QDialog::Accepted,"enable appraisal through native Setup using retained ANSI declarations");
    });
    const auto before=window.document().snapshot();
    const auto original=sketch::decode_identified_boundary_entity(before.entities().at(area.toStdString()));
    const auto vertex=original.segments.front().start_vertex_id;
    const auto point=original.segments.front().segment.start;
    const sketch::Vec2 target{point.x+0.25,point.y-0.125};
    sketch::ConstraintAuthoringIntent intent;
    intent.boundary_vertex_move=sketch::BoundaryVertexMoveIntent{
        {area.toStdString(),sketch::BoundaryGeometryEditKind::move_vertex,vertex,target},true};
    const auto preview=sketch::preview_constraint_authoring(before,intent);
    require(preview.accepted(),"coordinated source corner has a valid core candidate");
    const auto candidate=sketch::preview_constraint_authoring_snapshot(before,preview);
    require(sketch::wall_measurement_source_current(candidate,candidate.entities().at(area.toStdString())),
        "source corner preview retains exact physical correspondence");
    const auto property=before.entities().at(area.toStdString()).properties.at("property_id").get<std::string>();
    const auto original_report=sketch::build_appraisal_document_report(before,property);
    const auto proposed_report=sketch::build_appraisal_document_report(candidate,property);
    require(original_report.qualified && original_report.calculation && proposed_report.qualified && proposed_report.calculation &&
        original_report.calculation->property.gla().total.display.text!=proposed_report.calculation->property.gla().total.display.text,
        "source corner changes authoritative GLA in the proposed document");
    const auto edit=[&](bool apply) {
        window.setWorkspaceTheme(apply ? sketch::WorkspaceTheme::dark : sketch::WorkspaceTheme::light);
        require(window.selectEntity(area),"select sourced area for exact exterior corner editing");
        modal_interaction(window,"boundaryGeometryDialog",[&] { child<QPushButton>(window,"editBoundaryGeometry").click(); },[&](QDialog& dialog) {
            choose_data(child<QComboBox>(dialog,"boundaryEditOperation"),"vertex");
            choose_data(child<QComboBox>(dialog,"boundaryVertex"),QString::fromStdString(vertex));
            child<QLineEdit>(dialog,"boundaryVertexX").setText(QString::number(target.x,'g',17)+" m");
            child<QLineEdit>(dialog,"boundaryVertexY").setText(QString::number(target.y,'g',17)+" m");
            auto& buttons=child<QDialogButtonBox>(dialog,"boundaryGeometryButtons");
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),
                QStringLiteral("Valid source corner Apply refused: %1").arg(child<QLabel>(dialog,"boundaryGeometryStatus").text()).toStdString());
            require(child<QTableWidget>(dialog,"boundaryGeometryChanges").rowCount()>=5 &&
                window.document().snapshot().entities()==before.entities(),
                "source corner preview exposes wall consequences without committing");
            capture(dialog,QStringLiteral("source-corner-%1-%2.png").arg(metric ? "metric" : "imperial",apply ? "dark" : "light"));
            buttons.button(apply ? QDialogButtonBox::Apply : QDialogButtonBox::Cancel)->click();
        });
    };
    edit(false);
    require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),
        "source corner Cancel is mutation free");
    edit(true);
    const auto after=window.document().snapshot();
    require(after.entities()==candidate.entities() && after.revision()==before.revision()+1,
        "source corner commits exactly the previewed complete document in one revision");
    const auto result=sketch::decode_identified_boundary_entity(after.entities().at(area.toStdString()));
    require(std::hypot(result.segments.front().segment.start.x-target.x,result.segments.front().segment.start.y-target.y)<=1e-7,
        "committed forward-derived corner matches entered model coordinates");
    for (std::size_t i=0;i<original.segments.size();++i) {
        require(result.segments[i].segment_id==original.segments[i].segment_id &&
            result.segments[i].start_vertex_id==original.segments[i].start_vertex_id,
            "source correction preserves stable edges and vertices");
        if(i!=0)require(std::hypot(result.segments[i].segment.start.x-original.segments[i].segment.start.x,
            result.segments[i].segment.start.y-original.segments[i].segment.start.y)<=1e-7,
            "other exterior corners remain anchored");
    }
    bool changed=false;
    for(const auto& wall:walls) {
        changed|=after.entities().at(wall.toStdString())!=before.entities().at(wall.toStdString());
        require(after.entities().at(wall.toStdString()).properties.at("thickness_m")==
            before.entities().at(wall.toStdString()).properties.at("thickness_m"),"source edit preserves physical thickness");
    }
    require(changed && sketch::wall_measurement_source_current(after,after.entities().at(area.toStdString())),
        "source correction changes physical walls and retains a current measured exterior");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
        window.redoCommand() && window.document().snapshot().entities()==after.entities(),"source corner Undo/Redo is atomic");
    const auto project=directory.filePath(metric ? "source-corner-metric.bldproj" : "source-corner-imperial.bldproj");
    require(window.saveProjectAs(project) && window.createNewProject(),"persist coordinated source edit and release project lease");
    MainWindow reopened({},nullptr,directory.filePath("other-text-library.json"));
    require(reopened.openProject(project) && reopened.document().is_editable() && reopened.document().snapshot().entities()==after.entities(),
        "source proof and physical correspondence survive editable reopen");
    require(reopened.undoCommand() && reopened.document().snapshot().entities()==before.entities(),"restored source corner Undo is atomic");
    reopened.setAttribute(Qt::WA_DontShowOnScreen);reopened.resize(1100,780);reopened.show();
    reopened.setWorkspace(sketch::desktop::Workspace::measurement);
    require(reopened.selectEntity(area),"select restored sourced exterior for actual handle drag");
    auto* canvas=dynamic_cast<sketch::desktop::PlanCanvas*>(reopened.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas!=nullptr,"measurement canvas exists for source corner drag");
      canvas->setSnapEnabled(false);canvas->setOverviewMapEnabled(false);canvas->fitView();canvas->zoomBy(.8);
      QApplication::processEvents();
      // Settle the newly shown window's initial focus/navigation events before
      // beginning a pointer gesture, as an interactive user necessarily does.
      canvas->setFocus();QApplication::processEvents();QApplication::processEvents();
      require(reopened.selectEntity(area),"retain measured owner after initial window focus settles");
      canvas->setSnapEnabled(false);
      const auto drag_before=reopened.document().snapshot();
    const auto mouse=[&](QEvent::Type type,sketch::Vec2 world) {
        const auto center=QRectF(canvas->rect()).center();const auto view=canvas->viewCenter();
        const QPointF pixel{center.x()+(world.x-view.x)*canvas->viewScale(),center.y()-(world.y-view.y)*canvas->viewScale()};
        QMouseEvent event(type,pixel,canvas->mapToGlobal(pixel.toPoint()),
            type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(canvas,&event);
    };
    mouse(QEvent::MouseButtonPress,point);mouse(QEvent::MouseMove,target);
    auto* poll=reopened.findChild<QTimer*>("boundaryVertexPreviewPoll");
    require(poll && poll->isActive(),"actual source corner drag requests deferred preview");
    QEventLoop loop;QTimer observe,deadline;observe.setInterval(10);deadline.setSingleShot(true);
    QObject::connect(&observe,&QTimer::timeout,&loop,[&]{if(!poll->isActive())loop.quit();});
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
      observe.start();deadline.start(15000);loop.exec();
      if (poll->isActive() || !canvas->boundaryVertexPreviewMetrics()) {
          capture(*canvas,QStringLiteral("source-corner-drag-failure-%1.png").arg(metric ? "metric" : "imperial"));
          std::cerr << "source drag: pending=" << poll->isActive()
                    << " metrics=" << canvas->boundaryVertexPreviewMetrics().has_value()
                    << " projected=" << canvas->boundaryVertexPreviewEntities().size()
                    << " error=" << reopened.lastError().toStdString() << '\n';
      }
      require(!poll->isActive() && canvas->boundaryVertexPreviewMetrics() &&
          reopened.document().snapshot().entities()==drag_before.entities(),"actual source drag preview completes without mutation");
    const auto projected=canvas->boundaryVertexPreviewEntities();
    require(std::any_of(projected.begin(),projected.end(),[&](const auto& value){return value.id==area;}) &&
        std::any_of(projected.begin(),projected.end(),[&](const auto& value){return walls.contains(value.id);}),
        "actual source drag previews both measured exterior and physical walls");
    capture(*canvas,QStringLiteral("source-corner-drag-%1.png").arg(metric ? "metric" : "imperial"));
      mouse(QEvent::MouseButtonRelease,target);
      const auto dragged=reopened.document().snapshot();
      if (!reopened.lastError().isEmpty() || dragged.revision()!=drag_before.revision()+1) {
          std::cerr << "source drag release: revision=" << dragged.revision()
                    << " expected=" << drag_before.revision()+1
                    << " error=" << reopened.lastError().toStdString() << '\n';
          capture(*canvas,QStringLiteral("source-corner-release-failure-%1.png").arg(metric ? "metric" : "imperial"));
      }
      require(reopened.lastError().isEmpty() && dragged.revision()==drag_before.revision()+1 &&
        sketch::wall_measurement_source_current(dragged,dragged.entities().at(area.toStdString())),
        "actual canvas-handle release commits one current coordinated source corner document");
    for(const auto& proposed:projected) {
        const auto committed=std::find_if(canvas->entities().begin(),canvas->entities().end(),[&](const auto& value){return value.id==proposed.id;});
        require(committed!=canvas->entities().end() && proposed.segments.size()==committed->segments.size() &&
            std::equal(proposed.segments.begin(),proposed.segments.end(),committed->segments.begin(),[](const auto& a,const auto& b){
                return std::hypot(a.start.x-b.start.x,a.start.y-b.start.y)<=1e-7 &&
                    std::hypot(a.end.x-b.end.x,a.end.y-b.end.y)<=1e-7 && std::abs(a.sweep_radians-b.sweep_radians)<=1e-8;
            }),"source corner release matches every previewed physical and measured object");
    }
    const auto dragged_report=sketch::build_appraisal_document_report(dragged,property);
    require(dragged_report.qualified && dragged_report.calculation &&
        dragged_report.calculation->property.gla().total.display.text==proposed_report.calculation->property.gla().total.display.text,
        "actual canvas edit updates GLA consistently with numeric exterior correction");
}

void exact_vertex_refuses_locked_and_stale_context() {
    QTemporaryDir directory;
    MainWindow window({}, nullptr, directory.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    window.resize(1100,780);
    window.show();
    QApplication::processEvents();
    const auto area = window.createBoundary(tall_rectangle());
    const auto original = sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(area.toStdString()));
    const auto& edge = original.segments[1];
    sketch::PersistentConstraint lock{"exact-corner-fixed", sketch::ConstraintRelationKind::fixed_anchor,
        {{area.toStdString(),sketch::WallEndpointRole::start,edge.segment_id,edge.start_vertex_id}}, std::nullopt, sketch::Vec2{4,0}};
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
        {sketch::EntityChange::upsert(sketch::encode_constraint_entity(lock))},{},"Exact corner locked fixture"});
    const auto refuses = [&](const QString& owner, const std::string& vertex) {
        require(window.selectEntity(owner), "select corner for refusal validation");
        const auto before = window.document().snapshot();
        const auto model = sketch::decode_identified_boundary_entity(before.entities().at(owner.toStdString()));
        const auto target = std::find_if(model.segments.begin(),model.segments.end(),
            [&](const auto& item) { return item.start_vertex_id==vertex; });
        require(target!=model.segments.end(), "refusal fixture must resolve its exact stable corner");
        const sketch::Vec2 proposed_point{5.0,target->segment.start.y};
        auto& button = child<QPushButton>(window,"editBoundaryGeometry");
        modal_interaction(window,"boundaryGeometryDialog",[&] { button.click(); },[&](QDialog& dialog) {
            choose_data(child<QComboBox>(dialog,"boundaryEditOperation"),QStringLiteral("vertex"));
            choose_data(child<QComboBox>(dialog,"boundaryVertex"),QString::fromStdString(vertex));
            child<QLineEdit>(dialog,"boundaryVertexX").setText(QString::number(proposed_point.x,'g',17)+QStringLiteral(" m"));
            auto& status = child<QLabel>(dialog,"boundaryGeometryStatus");
            const auto enabled = child<QDialogButtonBox>(dialog,"boundaryGeometryButtons").button(QDialogButtonBox::Apply)->isEnabled();
            require(!enabled && !status.text().isEmpty(),
                QStringLiteral("Locked corner refusal failed: owner=%1 vertex=%2 Apply=%3 X=%4 status=%5")
                    .arg(owner,QString::fromStdString(vertex)).arg(enabled)
                    .arg(child<QLineEdit>(dialog,"boundaryVertexX").text(),status.text()).toStdString());
        });
        require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),
            "refused exact movement must preserve source geometry, constraints and history");
    };
    refuses(area, edge.start_vertex_id);
    const QStringList walls{window.createStraightWall({10,0},{14,0},"exterior"),
        window.createStraightWall({14,0},{14,3},"exterior"),window.createStraightWall({14,3},{10,3},"exterior"),
        window.createStraightWall({10,3},{10,0},"exterior")};
    for (qsizetype i=0; i<walls.size(); ++i)
        require(!walls[i].isEmpty() && window.selectEntity(walls[i],i!=0), "select complete physical source wall loop");
    const auto sourced = window.createMeasurementBoundaryFromSelectedWalls();
    require(!sourced.isEmpty(), "source refusal fixture creates a live measured exterior");
    const auto source_snapshot = window.document().snapshot();
    const auto source_boundary = sketch::decode_identified_boundary_entity(source_snapshot.entities().at(sourced.toStdString()));
    require(sketch::wall_measurement_source_current(source_snapshot,source_snapshot.entities().at(sourced.toStdString())),
        "source refusal fixture begins with a current physical source");
    const auto& source_edge = source_boundary.segments.front();
    const auto shared_point = source_edge.segment.start;
    const sketch::Vec2 triangle_second{shared_point.x-1,shared_point.y-2};
    const sketch::Vec2 triangle_third{shared_point.x-3,shared_point.y};
    const auto joined_free = window.createBoundary({{shared_point,triangle_second,0},
        {triangle_second,triangle_third,0},{triangle_third,shared_point,0}});
    require(!joined_free.isEmpty(), "indirect source fixture creates a free triangle sharing the exterior corner");
    const auto free_triangle = sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(joined_free.toStdString()));
    const auto& triangle_edge = free_triangle.segments.front();
    const sketch::PersistentConstraint indirect_join{"exact-corner-source-neighbor-joint",
        sketch::ConstraintRelationKind::coincident,
        {{joined_free.toStdString(),sketch::WallEndpointRole::start,triangle_edge.segment_id,triangle_edge.start_vertex_id},
         {sourced.toStdString(),sketch::WallEndpointRole::start,source_edge.segment_id,source_edge.start_vertex_id}},
        std::nullopt,std::nullopt};
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
        {sketch::EntityChange::upsert(sketch::encode_constraint_entity(indirect_join))},{},"Indirect source corner fixture"});
    const auto indirect_before = window.document().snapshot();
    const sketch::Vec2 indirect_target{shared_point.x+0.2,shared_point.y};
    sketch::ConstraintAuthoringIntent indirect_intent;
    indirect_intent.boundary_vertex_move = sketch::BoundaryVertexMoveIntent{
        {joined_free.toStdString(),sketch::BoundaryGeometryEditKind::move_vertex,
            triangle_edge.start_vertex_id,indirect_target},true};
    const auto unconstrained_source_preview = sketch::preview_constraint_authoring(indirect_before,indirect_intent);
    require(!unconstrained_source_preview.accepted(),
        "core and canvas preview must refuse an indirect edit that would stale a current measured source");
    require(window.selectEntity(joined_free), "select the free triangle rather than its source-derived neighbor");
    auto& indirect_button = child<QPushButton>(window,"editBoundaryGeometry");
    modal_interaction(window,"boundaryGeometryDialog",[&] { indirect_button.click(); },[&](QDialog& dialog) {
        choose_data(child<QComboBox>(dialog,"boundaryEditOperation"),QStringLiteral("vertex"));
        choose_data(child<QComboBox>(dialog,"boundaryVertex"),QString::fromStdString(triangle_edge.start_vertex_id));
        require(child<QCheckBox>(dialog,"boundaryMoveRelatedObjects").isChecked(),
            "indirect source regression must attempt saved related-corner propagation");
        child<QLineEdit>(dialog,"boundaryVertexX").setText(QString::number(indirect_target.x,'g',17)+QStringLiteral(" m"));
        require(!child<QDialogButtonBox>(dialog,"boundaryGeometryButtons").button(QDialogButtonBox::Apply)->isEnabled() &&
            child<QLabel>(dialog,"boundaryGeometryStatus").text().contains(QStringLiteral("source walls"),Qt::CaseInsensitive),
            "free corner edit must explain and refuse a stale source-derived related boundary");
        child<QDialogButtonBox>(dialog,"boundaryGeometryButtons").button(QDialogButtonBox::Cancel)->click();
    });
    const auto indirect_after = window.document().snapshot();
    require(indirect_after.entities()==indirect_before.entities() && indirect_after.revision()==indirect_before.revision() &&
        indirect_after.history().size()==indirect_before.history().size() &&
        sketch::wall_measurement_source_current(indirect_after,indirect_after.entities().at(sourced.toStdString())),
        "indirect source refusal must preserve both boundaries, physical walls, relationships, source validity and history");
    const auto free_area = window.createBoundary(tall_rectangle());
    require(!free_area.isEmpty() && window.selectEntity(free_area), "stale fixture selects a free boundary");
    auto& button = child<QPushButton>(window,"editBoundaryGeometry");
    const auto before_stale = window.document().snapshot();
    modal_interaction(window,"boundaryGeometryDialog",[&] { button.click(); },[&](QDialog& dialog) {
        choose_data(child<QComboBox>(dialog,"boundaryEditOperation"),QStringLiteral("vertex"));
        child<QLineEdit>(dialog,"boundaryVertexX").setText("-0.5 m");
        auto& apply = *child<QDialogButtonBox>(dialog,"boundaryGeometryButtons").button(QDialogButtonBox::Apply);
        require(apply.isEnabled(), "exact move must have a valid candidate before source revision changes");
        auto unrelated = before_stale.entities().at(walls.front().toStdString());
        unrelated.properties["name"]="Concurrent unrelated edit";
        window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
            {sketch::EntityChange::upsert(unrelated)},{},"Stale modal fixture"});
        const auto concurrent = window.document().snapshot();
        apply.click();
        require(dialog.isVisible() && !apply.isEnabled() &&
            !child<QLabel>(dialog,"boundaryGeometryStatus").text().isEmpty() &&
            window.document().snapshot().entities()==concurrent.entities() &&
            window.document().revision()==concurrent.revision(),
            "stale exact candidate must refuse Apply and preserve the newer work");
    });
    require(window.document().snapshot().entities().at(free_area.toStdString())==before_stale.entities().at(free_area.toStdString()),
        "stale exact corner refusal must leave the selected boundary untouched");
}

void curved_edge_resize_offers_related_object_choice(bool metric) {
    QTemporaryDir directory;
    require(directory.isValid(), "curved linked editor fixture needs a project directory");
    MainWindow window({}, nullptr, directory.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(metric);
    const sketch::Boundary shape{{{0,0},{4,0},0.6},{{4,0},{4,3},0},
        {{4,3},{0,3},0},{{0,3},{0,0},0}};
    const auto area_id = window.createBoundary(shape);
    const auto wall_id = window.createStraightWall({4,0},{7,0});
    require(!area_id.isEmpty() && !wall_id.isEmpty(), "curved linked fixture creates measured area and wall");
    const auto original = window.document().snapshot();
    const auto boundary = sketch::decode_identified_boundary_entity(original.entities().at(area_id.toStdString()));
    const auto& edge = boundary.segments.front();
    const sketch::PersistentConstraint relation{"curved-edge-wall-joint",
        sketch::ConstraintRelationKind::coincident,
        {{area_id.toStdString(),sketch::WallEndpointRole::end,edge.segment_id,edge.end_vertex_id},
         {wall_id.toStdString(),sketch::WallEndpointRole::start}},std::nullopt,std::nullopt};
    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
        {sketch::EntityChange::upsert(sketch::encode_constraint_entity(relation))},{},"Fixture explicit endpoint joint"});
    window.resize(1100,780);
    window.show();
    QApplication::processEvents();
    const auto before = window.document().snapshot();
    const auto edit = [&](bool apply) {
        require(window.selectEntity(area_id), "select curved boundary before resizing");
        auto& button = child<QPushButton>(window,"editBoundaryGeometry");
        button.setFocus();
        modal_interaction(window,"boundaryGeometryDialog",[&] { button.click(); },[&](QDialog& dialog) {
            auto& related = child<QCheckBox>(dialog,"boundaryMoveRelatedObjects");
            require(related.isEnabled() && related.isChecked(),
                "curved edge length editing must offer an enabled related-object choice");
            choose_data(child<QComboBox>(dialog,"boundaryEdge"),QString::fromStdString(edge.segment_id));
            auto& length = child<QLineEdit>(dialog,"boundaryEdgeLength");
            length.setText("5 m"); // An explicit unit must work in either display system.
            auto& buttons = child<QDialogButtonBox>(dialog,"boundaryGeometryButtons");
            require(buttons.button(QDialogButtonBox::Apply)->isEnabled(),
                "curved physical length resize must preview its related wall movement");
            related.click();
            require(!related.isChecked() && !buttons.button(QDialogButtonBox::Apply)->isEnabled() &&
                        !child<QLabel>(dialog,"boundaryGeometryStatus").text().isEmpty(),
                "retaining a joined wall must explain and refuse the conflicting curved edge resize");
            require(window.document().snapshot().entities()==before.entities(),
                "conflicting preview must preserve both owners and their saved relationship");
            related.click();
            require(related.isChecked() && buttons.button(QDialogButtonBox::Apply)->isEnabled() &&
                        child<QTableWidget>(dialog,"boundaryGeometryChanges").rowCount()>=2,
                "allowing related movement must restore the valid analytical proposal");
            capture(dialog,metric ? "curved-related-metric.png" : "curved-related-imperial.png");
            buttons.button(apply ? QDialogButtonBox::Apply : QDialogButtonBox::Cancel)->click();
        });
    };
    edit(false);
    require(window.document().snapshot().entities()==before.entities() &&
                window.document().revision()==before.revision(),
        "Cancel must retain the original curved area, related wall and history");
    edit(true);
    const auto after = window.document().snapshot();
    const auto resized = sketch::decode_identified_boundary_entity(after.entities().at(area_id.toStdString()));
    const auto& segment = resized.segments.front().segment;
    const auto& wall = after.entities().at(wall_id.toStdString()).properties.at("baseline");
    require(after.revision()==before.revision()+1 && segment.sweep_radians==edge.segment.sweep_radians &&
                same_point(segment.start,edge.segment.start) && std::abs(sketch::segment_length(segment)-5)<1e-8 &&
                std::hypot(wall.at("start").at(0).get<double>()-segment.end.x,
                           wall.at("start").at(1).get<double>()-segment.end.y)<1e-7,
        "one Apply must retain sweep and anchored endpoint while moving the joined wall to the resized arc");
    require(after.entities().at(relation.id)==before.entities().at(relation.id),
        "physical curved editing must retain the explicit endpoint relationship");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==after.entities(),
        "related curved editing must undo and redo both owners together");
    const auto project = directory.filePath("curved-related.bldproj");
    MainWindow reopened({},nullptr,directory.filePath("missing-library.json"));
    require(window.saveProjectAs(project) && reopened.openProject(project) &&
                reopened.document().snapshot().entities()==after.entities(),
        "related curved edit, dimensions and joint must survive save/reopen");
}

struct ArcCase {
    const char* name;
    const char* operation;
    const char* expression;
    sketch::BoundaryConstructionKind construction;
    double expected_input;
    double expected_dimension_length;
    bool outward;
    bool clockwise;
};

void assert_arc_receipt(const sketch::DocumentSnapshot& snapshot,
                        const ArcCase& test_case,
                        const std::string& segment_id) {
    const auto record = std::find_if(snapshot.history().rbegin(), snapshot.history().rend(),
        [&](const auto& item) { return item.boundary_geometry_edit && item.boundary_geometry_edit->target_id == segment_id; });
    require(record != snapshot.history().rend(),
            "arc edit must persist its semantic intent in revision history");
    const auto& edit = *record->boundary_geometry_edit;
    require(edit.kind == sketch::BoundaryGeometryEditKind::reconstruct_arc &&
                edit.target_id == segment_id && edit.arc_construction.has_value() &&
                edit.arc_construction->kind == test_case.construction,
            "arc history must retain the selected stable segment and construction kind");
    const auto& input = *edit.arc_construction;
    if (test_case.construction == sketch::BoundaryConstructionKind::arc_chord_angle) {
        require(input.angle &&
                    std::abs(input.angle->radians - test_case.expected_input) < 1e-12 &&
                    *input.angle == sketch::parse_angle(test_case.expression),
                "angle construction must retain the exact entered expression and signed angle intent");
    } else if (test_case.construction == sketch::BoundaryConstructionKind::arc_chord_height) {
        require(input.height && std::abs(input.height->metres - test_case.expected_input) < 1e-12,
                "height construction must retain the entered signed height intent");
    } else {
        require(input.arc_length &&
                    std::abs(input.arc_length->metres - test_case.expected_input) < 1e-12 &&
                    input.clockwise == test_case.clockwise,
                "arc-length construction must retain its entered length and selected side");
    }
}

void fixed_endpoint_arc_edit(const ArcCase& test_case) {
    QTemporaryDir directory;
    require(directory.isValid(), "arc fixture needs a temporary project directory");
    MainWindow window({}, nullptr, directory.filePath(QStringLiteral("text-library.json")));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    const auto boundary_id = window.createBoundary(tall_rectangle());
    require(!boundary_id.isEmpty(), "arc fixture needs a real tall rectangle");
    const auto owner_id = boundary_id.toStdString();
    const auto original = sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(owner_id));
    const auto original_edge = original.segments.front();
    const auto segment_id = original_edge.segment_id;
    require(std::abs(sketch::segment_length(original_edge.segment) - 4.0) < 1e-12,
            "arc fixture must present a 4 metre chord on the tall rectangle");
    require(window.selectEntity(boundary_id), "select arc source before adding its dependent dimension");
    const auto length_id = window.createLengthDimension(
        boundary_id, QString::fromStdString(segment_id), {2.0, -1.0});
    require(!length_id.isEmpty(), "arc fixture needs a real dependent length dimension");
    const auto before = window.document().snapshot();
    const auto before_dimension = require_length_dimension(before, boundary_id, segment_id);
    const auto before_owner = before.entities().at(owner_id);
    require(std::abs(before_dimension.dimension.resolve(before_owner).segment_length_metres - 4.0) < 1e-12,
            "dependent dimension must start at the straight 4 metre chord length");
    window.resize(1100, 780);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(boundary_id), "select boundary before opening arc editor");
    auto& edit_button = child<QPushButton>(window, "editBoundaryGeometry");
    require(edit_button.isEnabled(), "selected boundary must expose the actual geometry editor button");
    modal_interaction(window, "boundaryGeometryDialog",
        [&] { edit_button.click(); }, [&](QDialog& dialog) {
            auto& operation = child<QComboBox>(dialog, "boundaryEditOperation");
            choose_data(operation, QString::fromLatin1(test_case.operation));
            auto& edge = child<QComboBox>(dialog, "boundaryEdge");
            choose_data(edge, QString::fromStdString(segment_id));
            auto& expression = child<QLineEdit>(dialog, "boundaryEdgeLength");
            expression.setText(QString::fromLatin1(test_case.expression));
            if (std::string_view(test_case.operation) == "arc_length") {
                auto& clockwise = child<QCheckBox>(dialog, "boundaryCurveClockwise");
                clockwise.setChecked(test_case.clockwise);
            }
            QApplication::processEvents();
            const auto previewed = window.document().snapshot();
            require(previewed.revision() == before.revision() &&
                        previewed.entities() == before.entities(),
                    "arc preview must leave the live boundary and dependent dimension untouched");
            require(!child<QLabel>(dialog, "boundaryGeometrySummary").text().isEmpty() &&
                        child<QDialogButtonBox>(dialog, "boundaryGeometryButtons")
                            .button(QDialogButtonBox::Apply)->isEnabled(),
                    "valid fixed-endpoint arc must appear in the real preview before Apply");
            capture(dialog, QStringLiteral("boundary-arc-%1-preview.png")
                                .arg(QString::fromLatin1(test_case.name)));
            child<QDialogButtonBox>(dialog, "boundaryGeometryButtons")
                .button(QDialogButtonBox::Apply)->click();
        });

    const auto edited = window.document().snapshot();
    require(edited.revision() == before.revision() + 1,
            "fixed-endpoint arc Apply must create one real document revision");
    const auto updated = sketch::decode_identified_boundary_entity(
        edited.entities().at(owner_id));
    require(updated.id == original.id && updated.segments.size() == original.segments.size(),
            "arc reconstruction must preserve boundary identity and child count");
    for (std::size_t index = 0; index < original.segments.size(); ++index) {
        const auto& old_edge = original.segments[index];
        const auto& new_edge = updated.segments[index];
        require(new_edge.segment_id == old_edge.segment_id &&
                    new_edge.start_vertex_id == old_edge.start_vertex_id &&
                    new_edge.end_vertex_id == old_edge.end_vertex_id,
                "arc reconstruction must preserve every stable segment and vertex ID");
        if (old_edge.segment_id == segment_id) {
            require(same_point(new_edge.segment.start, old_edge.segment.start) &&
                        same_point(new_edge.segment.end, old_edge.segment.end) &&
                        std::abs(new_edge.segment.sweep_radians) > 1e-8,
                    "reconstructed arc must keep both chord endpoints fixed");
            if (test_case.construction == sketch::BoundaryConstructionKind::arc_chord_angle)
                require(std::abs(new_edge.segment.sweep_radians - test_case.expected_input) < 1e-12,
                        "explicit angle input must construct the requested signed analytical sweep");
            const auto bounds = sketch::segment_bounds(new_edge.segment);
            require(test_case.outward ? bounds.minimum.y < -0.01 : bounds.maximum.y > 0.01,
                    "signed construction input must select the expected side of the chord");
        } else {
            require(same_point(new_edge.segment.start, old_edge.segment.start) &&
                        same_point(new_edge.segment.end, old_edge.segment.end) &&
                        new_edge.segment.sweep_radians == old_edge.segment.sweep_radians,
                    "arc reconstruction must leave the other boundary edges unchanged");
        }
    }
    const auto dependent = require_length_dimension(edited, boundary_id, segment_id);
    require(dependent.id == before_dimension.id &&
                std::abs(dependent.dimension.resolve(edited.entities().at(owner_id))
                             .segment_length_metres - test_case.expected_dimension_length) < 1e-8,
            "existing stable length dimension must resolve the new arc length");
    assert_arc_receipt(edited, test_case, segment_id);

    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == edited.entities(),
            "arc reconstruction must undo and redo without losing its dependent dimension");
    const auto project_path = directory.filePath(QStringLiteral("boundary-arc-%1.bldproj")
                                                     .arg(QString::fromLatin1(test_case.name)));
    require(window.saveProjectAs(project_path), "arc project must save through MainWindow");
    MainWindow reopened({}, nullptr, directory.filePath(QStringLiteral("missing-library.json")));
    const auto opened = reopened.openProject(project_path);
    if (!opened)
        std::cerr << "Arc reopen failed: " << reopened.lastError().toStdString() << '\n';
    require(opened && reopened.document().snapshot().entities() == edited.entities(),
            "reopened project must retain fixed endpoints, stable children and dependent dimension");
    const auto reopened_snapshot = reopened.document().snapshot();
    const auto reopened_boundary = sketch::decode_identified_boundary_entity(
        reopened_snapshot.entities().at(owner_id));
    require(reopened_boundary.segments.front().segment_id == segment_id &&
                reopened_boundary.segments.front().segment.start.x == original_edge.segment.start.x &&
                reopened_boundary.segments.front().segment.start.y == original_edge.segment.start.y &&
                reopened_boundary.segments.front().segment.end.x == original_edge.segment.end.x &&
                reopened_boundary.segments.front().segment.end.y == original_edge.segment.end.y,
            "native reopen must preserve the original arc chord and stable segment identity");
    require(std::abs(require_length_dimension(reopened_snapshot, boundary_id, segment_id)
                         .dimension.resolve(reopened_snapshot.entities().at(owner_id))
                         .segment_length_metres - test_case.expected_dimension_length) < 1e-8,
            "native reopen must restore the dependent arc-length measurement");
    assert_arc_receipt(reopened_snapshot, test_case, segment_id);
}

void curve_angle_requires_explicit_units() {
    // Generic core parsing deliberately retains its bare-radian contract;
    // the actual authoring editor must require users to state their intent.
    require(sketch::parse_angle("1").radians == 1.0,
            "core angle parsing must preserve generic bare-radian semantics");
    QTemporaryDir directory;
    require(directory.isValid(), "curve angle unit fixture needs a temporary directory");
    MainWindow window({}, nullptr, directory.filePath(QStringLiteral("text-library.json")));
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setMetricUnits(true);
    const auto boundary_id = window.createBoundary(tall_rectangle());
    require(!boundary_id.isEmpty(), "curve angle unit fixture needs a real boundary");
    const auto before = window.document().snapshot();
    const auto source = sketch::decode_identified_boundary_entity(
        before.entities().at(boundary_id.toStdString()));
    window.resize(1100, 780);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(boundary_id), "select boundary before checking curve angle units");
    auto& button = child<QPushButton>(window, "editBoundaryGeometry");
    require(button.isEnabled(), "selected boundary must expose actual geometry editing");
    modal_interaction(window, "boundaryGeometryDialog", [&] { button.click(); },
        [&](QDialog& dialog) {
            choose_data(child<QComboBox>(dialog, "boundaryEditOperation"), QStringLiteral("angle"));
            choose_data(child<QComboBox>(dialog, "boundaryEdge"),
                        QString::fromStdString(source.segments.front().segment_id));
            child<QLineEdit>(dialog, "boundaryEdgeLength").setText(QStringLiteral("1"));
            QApplication::processEvents();
            const auto unchanged = window.document().snapshot();
            require(unchanged.revision() == before.revision() &&
                        unchanged.entities() == before.entities() &&
                        unchanged.history().size() == before.history().size(),
                    "ambiguous curve-angle preview must leave geometry and history unchanged");
            require(!child<QDialogButtonBox>(dialog, "boundaryGeometryButtons")
                         .button(QDialogButtonBox::Apply)->isEnabled(),
                    "Curve angle bare '1' must disable Apply despite being geometrically valid radians");
            const auto status = child<QLabel>(dialog, "boundaryGeometryStatus").text();
            require(status.contains(QStringLiteral("explicit"), Qt::CaseInsensitive) &&
                        status.contains(QStringLiteral("deg")) &&
                        status.contains(QStringLiteral("rad")) &&
                        status.contains(QStringLiteral("pi")),
                    "ambiguous curve angle must show actionable explicit deg/rad/pi validation");
            capture(dialog, QStringLiteral("boundary-curve-angle-explicit-unit-validation.png"));
        });
    require(window.document().snapshot().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "cancelling invalid curve angle must preserve the complete boundary");
}

void explicit_curve_angle_constructions() {
    const auto quarter_turn = std::numbers::pi / 2.0;
    const auto quarter_arc_length = std::sqrt(2.0) * std::numbers::pi;
    const ArcCase cases[]{
        {"explicit-degrees", "angle", "90 deg", sketch::BoundaryConstructionKind::arc_chord_angle,
         quarter_turn, quarter_arc_length, true, false},
        {"explicit-pi", "angle", "pi/2", sketch::BoundaryConstructionKind::arc_chord_angle,
         quarter_turn, quarter_arc_length, true, false},
        {"explicit-negative-pi", "angle", "-pi/2", sketch::BoundaryConstructionKind::arc_chord_angle,
         -quarter_turn, quarter_arc_length, false, false}};
    for (const auto& test_case : cases) fixed_endpoint_arc_edit(test_case);
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Vertex-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-boundary-editing-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled font must load for actual editor captures");
        app.setFont(QFont(QStringLiteral("Inter"), 10));
        if (app.arguments().contains(QStringLiteral("--exact-vertex-only"))) {
            exact_vertex_editor(false);
            exact_vertex_editor(true);
            source_corner_editor(false);
            source_corner_editor(true);
            exact_vertex_refuses_locked_and_stale_context();
            std::cout << "exact completed-boundary vertex editor passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--curved-related-only"))) {
            curved_edge_resize_offers_related_object_choice(false);
            curved_edge_resize_offers_related_object_choice(true);
            std::cout << "curved related-object editor passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--remove-point-only"))) {
            boundary_point_removal_workflow();
            boundary_point_removal_reference_review();
            boundary_point_removal_restores_split_arc();
            std::cout << "boundary point removal passed\n";
            return 0;
        }
        curve_angle_requires_explicit_units();
        explicit_curve_angle_constructions();
        const bool curve_units_only = std::any_of(argv + 1, argv + argc, [](const char* arg) {
            return std::string_view(arg) == "--curve-angle-units-only";
        });
        if (curve_units_only) {
            std::cout << "boundary_editing_desktop_tests curve angle units passed\n";
            return 0;
        }
        boundary_point_removal_workflow();
        boundary_point_removal_reference_review();
        boundary_point_removal_restores_split_arc();
        length_dimension_delete_recreate_and_restore();
        default_length_operation_remains_available();
        exact_vertex_editor(false);
        exact_vertex_editor(true);
        source_corner_editor(false);
        source_corner_editor(true);
        exact_vertex_refuses_locked_and_stale_context();
        curved_edge_resize_offers_related_object_choice(false);
        curved_edge_resize_offers_related_object_choice(true);
        const ArcCase cases[]{
            {"angle-outward", "angle", "60 deg",
             sketch::BoundaryConstructionKind::arc_chord_angle,
             std::numbers::pi / 3.0, 4.0 * std::numbers::pi / 3.0, true, false},
            {"angle-inward", "angle", "-60 deg",
             sketch::BoundaryConstructionKind::arc_chord_angle,
             -std::numbers::pi / 3.0, 4.0 * std::numbers::pi / 3.0, false, false},
            {"height-outward", "height", "0.5 m",
             sketch::BoundaryConstructionKind::arc_chord_height,
             0.5, 8.5 * std::asin(8.0 / 17.0), true, false},
            {"height-inward", "height", "-0.5 m",
             sketch::BoundaryConstructionKind::arc_chord_height,
             -0.5, 8.5 * std::asin(8.0 / 17.0), false, false},
            {"arc-length-outward", "arc_length", "5 m",
             sketch::BoundaryConstructionKind::arc_chord_length, 5.0, 5.0, true, false},
            {"arc-length-inward", "arc_length", "5 m",
             sketch::BoundaryConstructionKind::arc_chord_length, 5.0, 5.0, false, true}};
        for (const auto& test_case : cases) fixed_endpoint_arc_edit(test_case);
        std::cout << "boundary_editing_desktop_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_editing_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
