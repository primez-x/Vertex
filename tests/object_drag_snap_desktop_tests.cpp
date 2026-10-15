#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool close_enough(double a, double b) { return std::abs(a-b) < 1e-7; }

template<class Predicate> void await(Predicate ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (ready()) return;
    } while (timer.elapsed() < 5000);
    require(false, message);
}

sketch::desktop::CanvasSvgSymbol retained(sketch::desktop::PlanCanvas& canvas,
                                        const QString& id) {
    const auto& entities = canvas.entities();
    const auto found = std::find_if(entities.begin(), entities.end(),
        [&](const auto& item) { return item.id == id; });
    require(found != entities.end() && found->svg_symbol, "retained sofa SVG is missing");
    return *found->svg_symbol;
}

sketch::SymbolInstance persisted(sketch::desktop::MainWindow& window, const QString& id) {
    const auto snapshot = window.document().snapshot();
    for (const auto& [key, entity] : snapshot.entities()) {
        (void)key;
        if (entity.type != sketch::kAnnotationEntityType) continue;
        for (const auto& symbol : sketch::decode_annotation_entity(entity).symbols)
            if (symbol.id == id.toStdString()) return symbol;
    }
    throw std::runtime_error("persisted sofa annotation is missing");
}

void capture(sketch::desktop::MainWindow& window, const QString& name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && window.grab().save(QDir(directory).filePath(name)),
            "full-window object snap capture failed");
}

void existingObjectSnapsToWallFace(bool vertical, bool design = false) {
    sketch::desktop::MainWindow window;
    window.setWorkspace(design ? sketch::desktop::Workspace::architectural : sketch::desktop::Workspace::measurement);
    window.resize(1500, 1000);
    window.show();
    QApplication::processEvents();
    auto* thickness = window.findChild<QLineEdit*>(QStringLiteral("wallDrawThickness"));
    require(thickness, "wall thickness control is missing");
    thickness->setText(QStringLiteral("200 mm"));
    const auto wall = vertical ? window.createStraightWall({0,-4}, {0,4})
                               : window.createStraightWall({-4,0}, {4,0});
    require(!wall.isEmpty(), "physical wall fixture failed");
    const auto snapshot = window.document().snapshot();
    require(close_enough(snapshot.entities().at(wall.toStdString()).properties.at("thickness_m").get<double>(), .2),
            "fixture must have wall faces at +/-0.1 metres");
    const sketch::Vec2 original = vertical ? sketch::Vec2{3,1.4} : sketch::Vec2{1.2,2.5};
    const auto id = window.createAnnotationSymbol(
        QStringLiteral("svg-v2-04_living-sofa-three-seat"), original);
    require(!id.isEmpty(), "sofa fixture failed");
    QTemporaryDir directory;
    require(directory.isValid(), "temporary project directory failed");
    const auto path = directory.filePath(QStringLiteral("object-snap.bldproj"));
    // Reopening gives this gesture an empty history, independently of native
    // command revision accounting. One Undo must restore the exact fixture.
    require(window.saveProjectAs(path) && window.openProject(path), "fixture save/reopen failed");
    auto* undo = window.findChild<QAction*>(QStringLiteral("undoCommand"));
    require(undo && !undo->isEnabled(), "reopened fixture must have empty undo history");
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(design ? QStringLiteral("architecturalPlanCanvas") : QStringLiteral("measurementPlanCanvas")));
    require(canvas && canvas->isVisible(), "measurement canvas is missing or hidden");
    require(window.selectEntity(id), "sofa selection failed");
    canvas->setTool(sketch::desktop::CanvasTool::select);
    canvas->setGridEnabled(false);
    canvas->setSnapEnabled(true);
    canvas->setWallSnapEnabled(true);
    canvas->fitView();
    QApplication::processEvents();
    const auto before = retained(*canvas, id);
    const auto saved = persisted(window, id);
    require(close_enough(before.rotation_radians, 0) && before.width_metres > 0 && before.depth_metres > 0,
            "sofa must have a positive unrotated physical footprint");
    const auto screen = [&](sketch::Vec2 point) {
        const auto centre = canvas->viewCenter();
        return QRectF(canvas->rect()).center() + QPointF(
            (point.x-centre.x)*canvas->viewScale(), -(point.y-centre.y)*canvas->viewScale());
    };
    const auto mouse = [&](QEvent::Type type, QPointF point) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    // Keep the pointer well inside the artwork and away from transform grips.
    // Its nonzero grab offset must survive both movement and wall attraction.
    const sketch::Vec2 grab{.23, .13};
    const auto gap = 6.0/canvas->viewScale();
    const auto expected_rotation=vertical ? std::numbers::pi/2 : 0.0;
    const sketch::Vec2 expected = vertical
        ? sketch::Vec2{.1+before.depth_metres/2, .83+grab.y-grab.x}
        : sketch::Vec2{.83, .1+before.depth_metres/2};
    auto proposed = vertical ? sketch::Vec2{.1+before.width_metres/2,.83} : expected;
    if (vertical) proposed.x += gap; else proposed.y += gap;
    const auto press = screen({original.x+grab.x, original.y+grab.y});
    const auto target = screen({proposed.x+grab.x, proposed.y+grab.y});
    require(QRectF(canvas->rect()).adjusted(15,15,-15,-15).contains(press) &&
            QRectF(canvas->rect()).adjusted(15,15,-15,-15).contains(target),
            "drag fixture is clipped by the viewport");
    mouse(QEvent::MouseButtonPress, press);
    mouse(QEvent::MouseMove, target);
    await([&] { return !canvas->entitiesMovePreviewPending() && !canvas->entitiesMovePreview().empty(); },
          "actual MainWindow drag did not publish its deferred preview");
    const auto previews = canvas->entitiesMovePreview();
    const auto preview = std::find_if(previews.begin(), previews.end(),
        [&](const auto& entity) { return entity.id == id; });
    require(preview != previews.end() && preview->svg_symbol, "move preview omitted sofa artwork");
    const auto live = *preview->svg_symbol;
    capture(window, QString(design ? "design-" : "measure-") + (vertical ? "object-snap-vertical-preview.png" : "object-snap-horizontal-preview.png"));
    if (!close_enough(live.position.x,expected.x) || !close_enough(live.position.y,expected.y))
        std::cerr << "expected center " << expected.x << ',' << expected.y << "; actual "
                  << live.position.x << ',' << live.position.y << "; scale " << canvas->viewScale() << '\n';
    require(close_enough(live.position.x, expected.x) && close_enough(live.position.y, expected.y),
            "drag preview must attract sofa footprint to wall face and preserve tangential grab offset");
    require(close_enough(live.rotation_radians, expected_rotation), "wall attraction did not align the sofa width axis");
    const auto still_saved = persisted(window, id);
    require(close_enough(still_saved.placement.position.x, original.x) &&
            close_enough(still_saved.placement.position.y, original.y), "preview mutated persisted placement");
    mouse(QEvent::MouseButtonRelease, target);
    await([&] { return !canvas->entitiesMovePreviewPending() &&
        close_enough(persisted(window,id).placement.position.x, expected.x) &&
        close_enough(persisted(window,id).placement.position.y, expected.y); }, "snapped drag did not commit");
    capture(window, QString(design ? "design-" : "measure-") + (vertical ? "object-snap-vertical-committed.png" : "object-snap-horizontal-committed.png"));
    const auto committed = retained(*canvas, id);
    const auto moved = persisted(window, id);
    require(close_enough(committed.position.x, live.position.x) && close_enough(committed.position.y, live.position.y) &&
            close_enough(moved.placement.rotation_radians, expected_rotation) &&
            close_enough(moved.placement.scale, saved.placement.scale) && moved.pinned_svg == saved.pinned_svg,
            "release changed preview placement, orientation, scale or artwork");
    require(window.undoCommand(), "drag cannot undo");
    const auto undone = persisted(window, id);
    require(close_enough(undone.placement.position.x, original.x) && close_enough(undone.placement.position.y, original.y) &&
            !undo->isEnabled(), "one drag must create exactly one undoable placement command");
    require(window.redoCommand(), "drag cannot redo");
    const auto redone = persisted(window, id);
    require(close_enough(redone.placement.position.x, expected.x) && close_enough(redone.placement.position.y, expected.y),
            "redo did not restore snapped placement");
    require(window.saveProject() && window.openProject(path), "snapped placement save/reopen failed");
    const auto reopened = persisted(window, id);
    require(close_enough(reopened.placement.position.x, expected.x) && close_enough(reopened.placement.position.y, expected.y) &&
            close_enough(reopened.placement.rotation_radians, expected_rotation) && !undo->isEnabled(),
            "save/reopen lost snapped placement, orientation or empty history");
}

void libraryObjectSnapsToWallFace() {
    sketch::desktop::MainWindow window;
    window.resize(1500,1000); window.show(); QApplication::processEvents();
    auto* thickness=window.findChild<QLineEdit*>("wallDrawThickness");
    require(thickness,"library fixture wall thickness control missing");
    thickness->setText("200 mm");
    require(!window.createStraightWall({-4,0},{4,0}).isEmpty(),"library wall fixture failed");
    auto* canvas=dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas,"library fixture canvas missing");
    canvas->setTool(sketch::desktop::CanvasTool::select);
    canvas->setSnapEnabled(true); canvas->setWallSnapEnabled(true);
    canvas->setViewTransform({0,0},100); QApplication::processEvents();
    QMimeData mime;
    mime.setData("application/x-vertex-symbol",QJsonDocument(QJsonObject{
        {"id","svg-v2-04_living-sofa-three-seat"},{"scale",1.0}}).toJson(QJsonDocument::Compact));
    // The actual native artwork is 0.95 m deep. Approach its lower edge
    // within six pixels of the wall's upper physical face.
    const auto center=QRectF(canvas->rect()).center();
    const auto point=(center+QPointF(83,-63.5)).toPoint();
    const auto raw_x=canvas->viewCenter().x+(point.x()-center.x())/canvas->viewScale();
    const auto source=window.document().snapshot();
    QDragEnterEvent enter(point,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&enter);
    QDragMoveEvent move(point,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&move);
    require(enter.isAccepted() && move.isAccepted() && canvas->symbolDragPreviewActive(),
        "native library drag did not publish its preview");
    require(window.document().snapshot().entities()==source.entities(),"library preview mutated the document");
    capture(window,"object-snap-library-preview.png");
    QDropEvent drop(QPointF(point),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&drop);
    require(drop.isAccepted(),"native library drop rejected");
    const auto placed=std::find_if(canvas->entities().begin(),canvas->entities().end(),
        [](const auto& entity){return entity.svg_symbol.has_value();});
    require(placed!=canvas->entities().end(),"native library drop did not place the sofa");
    const auto svg=*placed->svg_symbol;
    require(close_enough(svg.position.x,raw_x) && close_enough(svg.position.y-svg.depth_metres/2,.1),
        "native library drop did not make physical footprint contact with the wall face");
    capture(window,"object-snap-library-committed.png");
    require(window.undoCommand() && window.document().snapshot().entities()==source.entities(),
        "one undo must restore the exact source before library placement");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-object-snap-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        existingObjectSnapsToWallFace(false);
        existingObjectSnapsToWallFace(true);
        existingObjectSnapsToWallFace(false,true);
        existingObjectSnapsToWallFace(true,true);
        libraryObjectSnapsToWallFace();
        std::cout << "object drag snap desktop checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "object_drag_snap_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
