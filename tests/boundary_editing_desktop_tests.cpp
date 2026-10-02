#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/desktop/main_window.hpp"
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
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTimer>
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
    QApplication app(argc, argv);
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled font must load for actual editor captures");
        app.setFont(QFont(QStringLiteral("Inter"), 10));
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
