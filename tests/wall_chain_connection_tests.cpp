#include "sketch/constraint_entity.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/document.hpp"
#include "sketch/model_phases.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace sketch;
using namespace sketch::desktop;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<PersistentConstraint> coincident_constraints(const DocumentSnapshot& snapshot) {
    std::vector<PersistentConstraint> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type != "constraint") continue;
        const auto decoded = decode_constraint_entity(entity);
        require(decoded.supported(), "auto-authored wall constraint decodes as a supported relation");
        if (decoded.constraint->relation == ConstraintRelationKind::coincident)
            result.push_back(*decoded.constraint);
    }
    return result;
}

std::vector<std::string> wall_ids(const DocumentSnapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& [id, entity] : snapshot.entities())
        if (entity.type == "wall") result.push_back(id);
    std::sort(result.begin(), result.end());
    return result;
}

Segment wall_baseline(const Entity& entity) {
    const auto& baseline = entity.properties.at("baseline");
    return {{baseline.at("start").at(0).get<double>(), baseline.at("start").at(1).get<double>()},
            {baseline.at("end").at(0).get<double>(), baseline.at("end").at(1).get<double>()},
            baseline.at("sweep_radians").get<double>()};
}

void test_public_wall_creation_persists_one_atomic_connection() {
    MainWindow window;
    window.setMetricUnits(true);
    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    require(!first.isEmpty(), "first public straight wall is created");
    const auto before_second = window.document().snapshot();

    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!second.isEmpty(), "second public straight wall is created");
    const auto connected = window.document().snapshot();
    const auto constraints = coincident_constraints(connected);
    require(constraints.size() == 1,
            "public straight-wall creation persists one coincident relation at a shared endpoint");
    const auto& relation = constraints.front();
    require(relation.bindings.size() == 2 &&
                relation.bindings[0].owner_id == first.toStdString() &&
                relation.bindings[0].role == WallEndpointRole::end &&
                relation.bindings[1].owner_id == second.toStdString() &&
                relation.bindings[1].role == WallEndpointRole::start,
            "auto-authored relation binds the exact shared endpoint pair");
    require(connected.revision() == before_second.revision() + 1,
            "new wall and its persistent relation share one document history command");
    const auto* navigator = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(navigator, "wall authoring retains its layer navigator");
    for (QTreeWidgetItemIterator item(const_cast<QTreeWidget*>(navigator)); *item; ++item)
        require((*item)->text(0) != QStringLiteral("Constraint") &&
                    (*item)->text(0) != QStringLiteral("Unassigned / unresolved"),
                "wall connections do not expose internal constraint records in the layer navigator");

    require(window.undoCommand() &&
                window.document().snapshot().entities() == before_second.entities(),
            "one undo removes the new wall and its connection together");
    require(window.redoCommand() &&
                window.document().snapshot().entities() == connected.entities(),
            "one redo restores the new wall and its connection together");

    QTemporaryDir directory;
    require(directory.isValid(), "connection persistence uses a temporary project directory");
    const auto path = directory.filePath(QStringLiteral("connected-walls.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "connected wall project saves and reopens");
    const auto reopened = window.document().snapshot();
    require(coincident_constraints(reopened).size() == 1,
            "shared endpoint relation survives project save and reopen");

    require(window.selectEntity(first) && window.editSelectedLength(QStringLiteral("4 m")),
            "a connected wall length can be edited after reopening");
    const auto resized = window.document().snapshot();
    const auto first_baseline = wall_baseline(resized.entities().at(first.toStdString()));
    const auto second_baseline = wall_baseline(resized.entities().at(second.toStdString()));
    require(std::abs(first_baseline.end.x - 4.0) < 1e-8 &&
                std::abs(first_baseline.end.y) < 1e-8 &&
                std::hypot(second_baseline.start.x - first_baseline.end.x,
                           second_baseline.start.y - first_baseline.end.y) < 1e-8,
            "resizing one wall moves its adjacent shared endpoint through the persisted relation");

    require(window.selectEntity(second) && window.deleteSelection(),
            "a connected wall can be deleted through normal selection removal");
    const auto after_delete = window.document().snapshot();
    require(after_delete.entities().contains(first.toStdString()) &&
                !after_delete.entities().contains(second.toStdString()) &&
                coincident_constraints(after_delete).empty(),
            "deleting a connected wall removes only its dangling relation and preserves its neighbor");
}

void test_wall_connections_respect_layer_and_phase_contexts() {
    {
        MainWindow window;
        const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
        require(!first.isEmpty(), "layer-scope fixture wall is created");
        const auto active_layer = window.activeLayerId().toStdString();
        const auto source = window.document().snapshot();
        const auto floor = source.entities().at(active_layer).properties.at("floor_id").get<std::string>();
        const auto other_layer = window.createLayer(QString::fromStdString(floor),
                                                    QStringLiteral("Separate wall layer"));
        require(!other_layer.isEmpty() && window.activeLayerId() == other_layer,
                "layer-scope fixture selects a separate layer on the same floor");
        const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
        require(!second.isEmpty() && coincident_constraints(window.document().snapshot()).empty(),
                "coincident walls on different layers remain unconnected");
    }

    {
        MainWindow window;
        const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
        require(!first.isEmpty(), "phase-scope fixture wall is created");
        const auto first_id = first.toStdString();
        const auto phases = ModelPhases::create(
            {first_id}, {first_id},
            {RemodelingAlternative{"demolish-host", "Demolish host", {first_id}, {}}});
        auto phase_record = Entity::create("model_phases", {{"model", phases.to_json()}});
        phase_record.id = "model-phases-wall-connection-test";
        window.document().apply(ApplyEntityChanges{
            window.document().revision(), {EntityChange::upsert(std::move(phase_record))}, {},
            "Prepare phase-scoped wall connection fixture"});
        require(window.selectRemodelingAlternative(QStringLiteral("demolish-host")),
                "phase-scope fixture selects the alternative that demolishes the existing wall");

        const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
        require(!second.isEmpty() && coincident_constraints(window.document().snapshot()).empty(),
                "a demolished wall from another active phase is not auto-connected");
    }
}

void test_interactive_wall_chain_closure_finishes_at_its_start() {
    MainWindow window;
    window.resize(1200, 800);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto* canvas = dynamic_cast<PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    auto* create_wall = window.findChild<QAction*>(QStringLiteral("createWall"));
    require(canvas && create_wall, "interactive closure fixture exposes its plan and wall command");
    create_wall->trigger();
    QApplication::processEvents();

    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    const auto model_to_canvas = [&](Vec2 point) {
        const auto center = canvas->viewCenter();
        const auto scale = canvas->viewScale();
        const auto viewport = QRectF(canvas->rect());
        return QPointF(viewport.center().x() + (point.x - center.x) * scale,
                       viewport.center().y() - (point.y - center.y) * scale);
    };
    const auto click = [&](Vec2 point) {
        const auto screen = model_to_canvas(point);
        mouse(QEvent::MouseMove, screen, Qt::NoButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, screen, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, screen, Qt::LeftButton, Qt::NoButton);
    };

    click({-2.0, -2.0});
    click({2.0, -2.0});
    click({2.0, 2.0});
    click({-2.0, -2.0});
    require(wall_ids(window.document().snapshot()).size() == 3,
            "clicking the original chain start commits the closing wall segment");
    require(!canvas->wallPreview().has_value(),
            "closing the wall loop retires its draft preview and finishes the chain");

    click({5.0, 4.0});
    require(wall_ids(window.document().snapshot()).size() == 3,
            "a click after loop closure cannot begin an unintended fourth segment");
}

void test_wall_and_boundary_tools_route_from_architectural_views() {
    const std::vector<QString> views{
        QStringLiteral("Plan"), QStringLiteral("Elevation"), QStringLiteral("Section · 1.2 m")};
    for (const auto& view_name : views) {
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        const auto existing = window.createStraightWall({0.37, 0.37}, {3.37, 0.37});
        require(!existing.isEmpty(), "architectural routing fixture creates an endpoint snap target");
        window.setWorkspace(Workspace::architectural);
        auto* architectural_view = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
        auto* create_wall = window.findChild<QAction*>(QStringLiteral("createWall"));
        require(architectural_view && create_wall,
                "architectural routing fixture exposes the view selector and wall command");
        architectural_view->setCurrentText(view_name);
        QApplication::processEvents();
        require(window.workspace() == Workspace::architectural &&
                    architectural_view->currentText() == view_name,
                "fixture is in the requested architectural plan, elevation, or section view");

        auto* canvas = dynamic_cast<PlanCanvas*>(
            window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
        require(canvas, "architectural wall command has a conventional measurement canvas");
        create_wall->trigger();
        QApplication::processEvents();
        require(window.workspace() == Workspace::measurement && canvas->isVisible(),
                "wall authoring from every architectural view switches to the conventional 2D canvas");
        const auto existing_id = existing.toStdString();

        const auto to_screen = [&](Vec2 point) {
            const auto center = canvas->viewCenter();
            const auto scale = canvas->viewScale();
            const auto viewport = QRectF(canvas->rect());
            return QPointF(viewport.center().x() + (point.x - center.x) * scale,
                           viewport.center().y() - (point.y - center.y) * scale);
        };
        const auto click = [&](Vec2 point) {
            const auto screen = to_screen(point);
            QMouseEvent press(QEvent::MouseButtonPress, screen,
                              canvas->mapToGlobal(screen.toPoint()), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, screen,
                                canvas->mapToGlobal(screen.toPoint()), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &release);
        };
        const auto snap_offset = std::min(0.08, 8.0 / canvas->viewScale());
        click({3.37 + snap_offset, 0.37 + snap_offset});
        click({4.20, 2.30});
        const auto after_wall = window.document().snapshot();
        const auto created = wall_ids(after_wall);
        require(created.size() == 2, "routed wall command commits on the measurement canvas");
        const auto new_wall_id = std::find_if(created.begin(), created.end(),
            [&](const auto& id) { return id != existing_id; });
        require(new_wall_id != created.end(), "routed wall is identifiable in the committed model");
        const auto& new_wall = after_wall.entities().at(*new_wall_id);
        const auto snapped_start = wall_baseline(new_wall).start;
        require(std::hypot(snapped_start.x - 3.37, snapped_start.y - 0.37) < 1e-8,
                "routed wall command retains endpoint snapping to the existing wall");
    }

    MainWindow boundary_window;
    boundary_window.setWorkspace(Workspace::architectural);
    auto* architectural_view = boundary_window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    require(architectural_view, "direct boundary routing fixture exposes architectural view selection");
    architectural_view->setCurrentText(QStringLiteral("Elevation"));
    QApplication::processEvents();
    require(boundary_window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                                  QStringLiteral("living")) &&
                boundary_window.workspace() == Workspace::measurement,
            "the public boundary start path routes architectural callers to conventional 2D drawing");
}

void test_escape_keeps_committed_wall_segments() {
    MainWindow window;
    window.resize(1200, 800);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto* canvas = dynamic_cast<PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    auto* create_wall = window.findChild<QAction*>(QStringLiteral("createWall"));
    require(canvas && create_wall, "Escape fixture exposes its plan and wall command");
    create_wall->trigger();
    QApplication::processEvents();
    const auto point = [&](Vec2 model) {
        const auto center = canvas->viewCenter();
        const auto scale = canvas->viewScale();
        const auto viewport = QRectF(canvas->rect());
        return QPointF(viewport.center().x() + (model.x - center.x) * scale,
                       viewport.center().y() - (model.y - center.y) * scale);
    };
    const auto click = [&](Vec2 model) {
        const auto screen = point(model);
        QMouseEvent press(QEvent::MouseButtonPress, screen, canvas->mapToGlobal(screen.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, screen, canvas->mapToGlobal(screen.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
    };
    click({0.0, 0.0});
    click({2.0, 0.0});
    const auto committed = wall_ids(window.document().snapshot());
    require(committed.size() == 1, "two clicks commit the first chain wall before Escape");

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    QApplication::processEvents();
    require(wall_ids(window.document().snapshot()) == committed &&
                !canvas->wallPreview().has_value(),
            "Escape cancels only the unfinished next segment and preserves committed walls");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-wall-chain-test-") +
                                         QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font_id >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
    try {
        test_public_wall_creation_persists_one_atomic_connection();
        test_wall_connections_respect_layer_and_phase_contexts();
        test_interactive_wall_chain_closure_finishes_at_its_start();
        test_wall_and_boundary_tools_route_from_architectural_views();
        test_escape_keeps_committed_wall_segments();
    } catch (const std::exception& error) {
        std::cerr << "wall_chain_connection_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Wall chain connection tests passed\n";
    return 0;
}
