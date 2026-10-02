#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
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

void exact_vertex_refuses_locked_source_and_stale_context() {
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
    const auto refuses = [&](const QString& owner, const std::string& vertex, bool source) {
        require(window.selectEntity(owner), "select corner for refusal validation");
        const auto before = window.document().snapshot();
        const auto model = sketch::decode_identified_boundary_entity(before.entities().at(owner.toStdString()));
        const auto target = std::find_if(model.segments.begin(),model.segments.end(),
            [&](const auto& item) { return item.start_vertex_id==vertex; });
        require(target!=model.segments.end(), "refusal fixture must resolve its exact stable corner");
        const sketch::Vec2 proposed_point{source ? target->segment.start.x+0.2 : 5.0,target->segment.start.y};
        if (source) {
            sketch::ConstraintAuthoringIntent intent;
            intent.boundary_vertex_move = sketch::BoundaryVertexMoveIntent{
                {owner.toStdString(),sketch::BoundaryGeometryEditKind::move_vertex,vertex,proposed_point},true};
            const auto preview = sketch::preview_constraint_authoring(before,intent);
            require(preview.accepted(), "source refusal fixture must use a geometrically valid exact corner movement");
            const auto candidate = sketch::preview_constraint_authoring_snapshot(before,preview);
            require(sketch::wall_measurement_source_current(before,before.entities().at(owner.toStdString())) &&
                !sketch::wall_measurement_source_current(candidate,candidate.entities().at(owner.toStdString())),
                "source refusal fixture must specifically stale a previously current measured exterior");
            for (const auto& wall : sketch::exterior_wall_measurement_source_ids(before.entities().at(owner.toStdString())))
                require(candidate.entities().at(wall)==before.entities().at(wall),
                    "source refusal fixture must retain every authoritative physical source wall");
        }
        auto& button = child<QPushButton>(window,"editBoundaryGeometry");
        modal_interaction(window,"boundaryGeometryDialog",[&] { button.click(); },[&](QDialog& dialog) {
            choose_data(child<QComboBox>(dialog,"boundaryEditOperation"),QStringLiteral("vertex"));
            choose_data(child<QComboBox>(dialog,"boundaryVertex"),QString::fromStdString(vertex));
            child<QLineEdit>(dialog,"boundaryVertexX").setText(QString::number(proposed_point.x,'g',17)+QStringLiteral(" m"));
            auto& status = child<QLabel>(dialog,"boundaryGeometryStatus");
            const auto enabled = child<QDialogButtonBox>(dialog,"boundaryGeometryButtons").button(QDialogButtonBox::Apply)->isEnabled();
            require(!enabled && !status.text().isEmpty() && (!source || status.text().contains("source walls",Qt::CaseInsensitive)),
                QStringLiteral("Locked/source corner refusal failed: owner=%1 vertex=%2 source=%3 Apply=%4 X=%5 status=%6")
                    .arg(owner,QString::fromStdString(vertex)).arg(source).arg(enabled)
                    .arg(child<QLineEdit>(dialog,"boundaryVertexX").text(),status.text()).toStdString());
        });
        require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),
            "refused exact movement must preserve source geometry, constraints and history");
    };
    refuses(area, edge.start_vertex_id, false);
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
    refuses(sourced, source_boundary.segments.front().start_vertex_id, true);
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
    require(unconstrained_source_preview.accepted() &&
        unconstrained_source_preview.candidate_entities().at(sourced.toStdString()) !=
            indirect_before.entities().at(sourced.toStdString()),
        "indirect source regression must exercise a geometrically valid shared-corner candidate");
    const auto indirect_candidate = sketch::preview_constraint_authoring_snapshot(indirect_before,unconstrained_source_preview);
    require(!sketch::wall_measurement_source_current(indirect_candidate,indirect_candidate.entities().at(sourced.toStdString())),
        "indirect source regression must specifically stale its previously current measured neighbor");
    for (const auto& wall : walls)
        require(unconstrained_source_preview.candidate_entities().at(wall.toStdString())==indirect_before.entities().at(wall.toStdString()),
            "shared boundary corner candidate leaves authoritative physical source walls fixed");
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
            exact_vertex_refuses_locked_source_and_stale_context();
            std::cout << "exact completed-boundary vertex editor passed\n";
            return 0;
        }
        if (app.arguments().contains(QStringLiteral("--curved-related-only"))) {
            curved_edge_resize_offers_related_object_choice(false);
            curved_edge_resize_offers_related_object_choice(true);
            std::cout << "curved related-object editor passed\n";
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
        length_dimension_delete_recreate_and_restore();
        default_length_operation_remains_available();
        exact_vertex_editor(false);
        exact_vertex_editor(true);
        exact_vertex_refuses_locked_source_and_stale_context();
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
