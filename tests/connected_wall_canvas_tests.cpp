#include "sketch/constraint_entity.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/document.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace sketch;
using namespace sketch::desktop;

constexpr double kTolerance = 1e-7;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool nearly_equal(double actual, double expected, double tolerance = kTolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool nearly_equal(Vec2 actual, Vec2 expected, double tolerance = kTolerance) {
    return nearly_equal(actual.x, expected.x, tolerance) &&
           nearly_equal(actual.y, expected.y, tolerance);
}

Segment wall_baseline(const Entity& entity) {
    const auto& value = entity.properties.at("baseline");
    return {{value.at("start").at(0).get<double>(), value.at("start").at(1).get<double>()},
            {value.at("end").at(0).get<double>(), value.at("end").at(1).get<double>()},
            value.at("sweep_radians").get<double>()};
}

Segment canvas_wall_baseline(const CanvasEntity& entity) {
    require(entity.type == QStringLiteral("wall") && entity.snap_points.size() >= 2,
            "wall preview retains its analytical baseline endpoints");
    return {entity.snap_points[0], entity.snap_points[1], 0.0};
}

const CanvasEntity& find_canvas_entity(const std::vector<CanvasEntity>& entities,
                                       const QString& id) {
    const auto found = std::find_if(entities.begin(), entities.end(),
        [&](const auto& entity) { return entity.id == id; });
    require(found != entities.end(), "retained canvas scene includes the requested entity");
    return *found;
}

void require_boundary_translation(const Boundary& before, const Boundary& after,
                                  Vec2 delta, const char* message) {
    require(before.size() == after.size(), message);
    for (std::size_t index = 0; index < before.size(); ++index) {
        require(nearly_equal(after[index].start,
                     {before[index].start.x + delta.x, before[index].start.y + delta.y}) &&
                    nearly_equal(after[index].end,
                         {before[index].end.x + delta.x, before[index].end.y + delta.y}) &&
                    nearly_equal(after[index].sweep_radians, before[index].sweep_radians),
                message);
    }
}

PlanCanvas& measurement_canvas(MainWindow& window) {
    auto* canvas = dynamic_cast<PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas != nullptr, "measurement workspace exposes its plan canvas");
    return *canvas;
}

QPointF model_to_canvas(const PlanCanvas& canvas, Vec2 point) {
    const auto center = canvas.viewCenter();
    const auto scale = canvas.viewScale();
    const auto viewport = QRectF(canvas.rect());
    return {viewport.center().x() + (point.x - center.x) * scale,
            viewport.center().y() - (point.y - center.y) * scale};
}

void send_mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point,
                Qt::MouseButton button = Qt::NoButton,
                Qt::MouseButtons buttons = Qt::NoButton,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                bool process_events = true) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), button, buttons, modifiers);
    QApplication::sendEvent(&canvas, &event);
    if (process_events) QApplication::processEvents();
}

void click(PlanCanvas& canvas, QPointF point,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    send_mouse(canvas, QEvent::MouseMove, point);
    send_mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton,
               Qt::LeftButton, modifiers);
    send_mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton,
               Qt::NoButton, modifiers);
}

bool wait_for_exact_move_preview(PlanCanvas& canvas, int timeout_ms = 2500) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!canvas.entitiesMovePreviewPending() && !canvas.entitiesMovePreview().empty())
            return true;
        QThread::msleep(1);
    } while (timer.elapsed() < timeout_ms);
    return false;
}

bool wait_for_move_preview_settled(PlanCanvas& canvas, int timeout_ms = 2500) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!canvas.entitiesMovePreviewPending()) return true;
        QThread::msleep(1);
    } while (timer.elapsed() < timeout_ms);
    return false;
}

bool wait_for_move_commit(MainWindow& window, PlanCanvas& canvas, Revision expected_revision,
                          int timeout_ms = 2500) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (window.document().revision() == expected_revision &&
            !canvas.entitiesMovePreviewPending() && canvas.entitiesMovePreview().empty())
            return true;
        QThread::msleep(1);
    } while (timer.elapsed() < timeout_ms);
    return false;
}

bool wait_for_quiet_document_state(MainWindow& window, PlanCanvas& canvas,
                                   Revision expected_revision, int quiet_ms = 500,
                                   int timeout_ms = 2500) {
    QElapsedTimer timer;
    QElapsedTimer quiet;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        const bool unchanged = window.document().revision() == expected_revision &&
            !canvas.entitiesMovePreviewPending() && canvas.entitiesMovePreview().empty();
        if (unchanged) {
            if (!quiet.isValid()) quiet.start();
            if (quiet.elapsed() >= quiet_ms) return true;
        } else {
            quiet.invalidate();
        }
        QThread::msleep(1);
    } while (timer.elapsed() < timeout_ms);
    return false;
}

QPointF frame_grab_point(PlanCanvas& canvas, const Segment& baseline) {
    const auto frame = canvas.selectionBounds();
    require(frame.has_value(), "selected wall exposes a visible move frame");
    const auto selected_wall = std::find_if(canvas.entities().begin(), canvas.entities().end(),
        [](const auto& entity) { return entity.selected && entity.type == QStringLiteral("wall"); });
    require(selected_wall != canvas.entities().end(), "selected move frame belongs to a wall");
    const auto midpoint = model_to_canvas(canvas,
        {(baseline.start.x + baseline.end.x) * 0.5,
         (baseline.start.y + baseline.end.y) * 0.5});
    // Move from the visible frame's empty interior, away from both the wall
    // footprint and its midpoint resize handle. This makes the gesture depend
    // on the selected frame rather than a precise hit on a painted wall edge.
    const auto clearance = selected_wall->thickness_metres * canvas.viewScale() * 0.5 + 3.0;
    auto point = QPointF(midpoint.x() + 26.0, midpoint.y() + clearance);
    if (!frame->contains(point)) point.setY(midpoint.y() - clearance);
    require(frame->contains(point), "off-wall press point remains inside selected move frame");
    return point;
}

void save_capture(MainWindow& window, const QString& file_name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "visual capture directory is available");
    require(window.grab().save(QDir(directory).filePath(file_name)),
            "connected-wall workflow screenshot saves");
}

void test_selected_connected_wall_move_previews_commits_and_round_trips() {
    MainWindow window;
    window.resize(1200, 800);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    require(!first.isEmpty(), "connected-wall fixture creates the selected horizontal wall");
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!second.isEmpty(), "connected-wall fixture creates its vertical neighbor");
    require(window.selectEntity(first), "horizontal wall selects for the canvas move");
    const auto opening = window.createHostedOpening(QStringLiteral("window"), QStringLiteral("1 m"),
        QStringLiteral("0.8 m"), QStringLiteral("0.8 m"), QStringLiteral("1.2 m"));
    require(!opening.isEmpty(), "connected-wall fixture adds a hosted window");
    require(window.selectEntity(first), "host wall reselects after opening creation");

    const auto before = window.document().snapshot();
    const auto first_before = wall_baseline(before.entities().at(first.toStdString()));
    const auto second_before = wall_baseline(before.entities().at(second.toStdString()));
    const auto opening_before = before.entities().at(opening.toStdString()).properties;
    const auto opening_canvas_before = find_canvas_entity(canvas.entities(), opening);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    const auto grab = frame_grab_point(canvas, first_before);
    const auto preview_delta = Vec2{0.0, 0.5};
    const auto preview_point = grab + QPointF(preview_delta.x * canvas.viewScale(),
                                             -preview_delta.y * canvas.viewScale());

    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, preview_point, Qt::NoButton, Qt::LeftButton);
    require(wait_for_exact_move_preview(canvas),
            "selected wall drag publishes a bounded exact connected-geometry preview");
    require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "mouse-move preview leaves document geometry and history untouched");

    const auto preview = canvas.entitiesMovePreview();
    const auto first_preview = canvas_wall_baseline(find_canvas_entity(preview, first));
    const auto second_preview = canvas_wall_baseline(find_canvas_entity(preview, second));
    require(nearly_equal(first_preview.start, {first_before.start.x, first_before.start.y + 0.5}) &&
                nearly_equal(first_preview.end, {first_before.end.x, first_before.end.y + 0.5}),
            "preview translates both selected wall endpoints by the pointer delta");
    require(nearly_equal(second_preview.start, {second_before.start.x, second_before.start.y + 0.5}) &&
                nearly_equal(second_preview.end, second_before.end),
            "preview moves the connected shared endpoint while pinning the neighbor's far end");
    const auto& opening_preview = find_canvas_entity(preview, opening);
    require_boundary_translation(opening_canvas_before.segments, opening_preview.segments,
                                 preview_delta,
                                 "hosted opening preview follows the moved host at unchanged physical placement");
    save_capture(window, QStringLiteral("connected-wall-move-preview.png"));

    const auto commit_delta = Vec2{0.0, 0.75};
    const auto release = grab + QPointF(commit_delta.x * canvas.viewScale(),
                                       -commit_delta.y * canvas.viewScale());
    const auto revision_before_commit = window.document().revision();
    // The release target intentionally differs from the last MouseMove. Its
    // exact candidate must replace the earlier +0.5 m preview before commit.
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(wait_for_move_commit(window, canvas, revision_before_commit + 1),
            "accepted exact release target commits once after bounded Qt event processing");
    const auto committed = window.document().snapshot();
    require(committed.revision() == revision_before_commit + 1,
            "connected wall move commits as one document history command");
    const auto first_after = wall_baseline(committed.entities().at(first.toStdString()));
    const auto second_after = wall_baseline(committed.entities().at(second.toStdString()));
    require(nearly_equal(first_after.start, {first_before.start.x, first_before.start.y + commit_delta.y}) &&
                nearly_equal(first_after.end, {first_before.end.x, first_before.end.y + commit_delta.y}) &&
                nearly_equal(second_after.start,
                             {second_before.start.x, second_before.start.y + commit_delta.y}) &&
                nearly_equal(second_after.end, second_before.end),
            "release commits +0.75 m for the selected wall and only the neighbor's shared endpoint");
    const auto opening_after = committed.entities().at(opening.toStdString()).properties;
    require(opening_after.at("wall_id") == opening_before.at("wall_id") &&
                nearly_equal(opening_after.at("offset_m").get<double>(),
                              opening_before.at("offset_m").get<double>()) &&
                nearly_equal(opening_after.at("width_m").get<double>(),
                              opening_before.at("width_m").get<double>()),
            "committing a host translation preserves opening identity, offset and width");
    const auto opening_canvas_after = find_canvas_entity(canvas.entities(), opening);
    require_boundary_translation(opening_canvas_before.segments, opening_canvas_after.segments,
                                 commit_delta,
                                 "committed hosted opening follows the host by the previewed translation");
    save_capture(window, QStringLiteral("connected-wall-move-committed.png"));

    require(window.undoCommand() &&
                window.document().snapshot().entities() == before.entities(),
            "one undo restores both connected wall baselines and hosted opening");
    require(window.redoCommand() &&
                window.document().snapshot().entities() == committed.entities(),
            "one redo restores the exact previewed connected wall move");

    QTemporaryDir directory;
    require(directory.isValid(), "connected-wall archive fixture has a temporary directory");
    const auto path = directory.filePath(QStringLiteral("connected-wall-move.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "connected wall move saves and reopens through the project workflow");
    const auto reopened = window.document().snapshot();
    require(nearly_equal(wall_baseline(reopened.entities().at(first.toStdString())).start,
                 first_after.start) &&
                nearly_equal(wall_baseline(reopened.entities().at(second.toStdString())).start,
                     second_after.start) &&
                nearly_equal(reopened.entities().at(opening.toStdString()).properties.at("offset_m").get<double>(),
                     opening_before.at("offset_m").get<double>()),
            "reopened project preserves the connected move and hosted opening station");
}

void test_canvas_release_without_intermediate_move_commits_pointer_target() {
    MainWindow window;
    window.resize(1000, 700);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!first.isEmpty() && !second.isEmpty(),
            "no-intermediate-move fixture creates a connected wall pair");
    require(window.selectEntity(first), "no-intermediate-move fixture selects its host wall");
    const auto before = window.document().snapshot();
    const auto first_before = wall_baseline(before.entities().at(first.toStdString()));
    const auto second_before = wall_baseline(before.entities().at(second.toStdString()));
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    const auto grab = frame_grab_point(canvas, first_before);
    const auto release_delta = Vec2{0.0, 0.4};
    const auto release = grab + QPointF(release_delta.x * canvas.viewScale(),
                                       -release_delta.y * canvas.viewScale());

    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    // Deliberately omit MouseMove: a qualifying release must solve and commit
    // the pointer's actual position rather than reusing the press position.
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(wait_for_move_commit(window, canvas, before.revision() + 1),
            "no-move release computes and commits its pointer target after bounded event processing");
    const auto committed = window.document().snapshot();
    require(committed.revision() == before.revision() + 1 &&
                nearly_equal(wall_baseline(committed.entities().at(first.toStdString())).start,
                             {first_before.start.x, first_before.start.y + release_delta.y}) &&
                nearly_equal(wall_baseline(committed.entities().at(first.toStdString())).end,
                             {first_before.end.x, first_before.end.y + release_delta.y}) &&
                nearly_equal(wall_baseline(committed.entities().at(second.toStdString())).start,
                             {second_before.start.x, second_before.start.y + release_delta.y}) &&
                nearly_equal(wall_baseline(committed.entities().at(second.toStdString())).end,
                             second_before.end),
            "committed no-move gesture equals its release-position candidate in one revision");
}

void test_canvas_wall_move_release_refuses_stale_document_head() {
    MainWindow window;
    window.resize(1000, 700);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!first.isEmpty() && !second.isEmpty(),
            "stale-head fixture creates a connected wall pair");
    require(window.selectEntity(first), "stale-head fixture selects its host wall");
    const auto before = window.document().snapshot();
    const auto first_before = wall_baseline(before.entities().at(first.toStdString()));
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    const auto grab = frame_grab_point(canvas, first_before);
    const auto delta = Vec2{0.0, 0.5};
    const auto target = grab + QPointF(delta.x * canvas.viewScale(),
                                      -delta.y * canvas.viewScale());

    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    require(wait_for_exact_move_preview(canvas),
            "stale-head fixture first obtains an accepted transient move preview");
    require(window.document().snapshot().entities() == before.entities(),
            "stale-head preview has not yet changed committed geometry");

    auto externally_edited_wall = before.entities().at(first.toStdString());
    externally_edited_wall.properties["name"] = "external edit before wall drag release";
    window.document().apply(ApplyEntityChanges{
        window.document().revision(), {EntityChange::upsert(externally_edited_wall)}, {},
        "External wall metadata edit during canvas drag"});
    const auto external_head = window.document().snapshot();
    require(external_head.revision() == before.revision() + 1 &&
                external_head.entities().at(first.toStdString()).properties.at("name") ==
                    "external edit before wall drag release",
            "external metadata edit advances and remains visible as the document head");

    send_mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(wait_for_quiet_document_state(window, canvas, external_head.revision()),
            "stale drag release settles without replacing a newer document head");
    const auto after = window.document().snapshot();
    require(after.revision() == external_head.revision() &&
                after.entities() == external_head.entities(),
            "stale release preserves the exact external document snapshot");
    require(nearly_equal(wall_baseline(after.entities().at(first.toStdString())).start,
                         first_before.start) &&
                nearly_equal(wall_baseline(after.entities().at(first.toStdString())).end,
                             first_before.end),
            "stale drag cannot overwrite wall geometry from its captured source snapshot");
}

void test_escape_cancels_exact_release_preview_before_commit() {
    MainWindow window;
    window.resize(1000, 700);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!first.isEmpty() && !second.isEmpty(),
            "Escape fixture creates a connected wall pair");
    require(window.selectEntity(first), "Escape fixture selects its host wall");
    const auto before = window.document().snapshot();
    const auto baseline = wall_baseline(before.entities().at(first.toStdString()));
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    const auto grab = frame_grab_point(canvas, baseline);
    const auto preview_delta = Vec2{0.0, 0.5};
    const auto preview_target = grab + QPointF(preview_delta.x * canvas.viewScale(),
                                              -preview_delta.y * canvas.viewScale());
    const auto release_delta = Vec2{0.0, 0.75};
    const auto release_target = grab + QPointF(release_delta.x * canvas.viewScale(),
                                               -release_delta.y * canvas.viewScale());

    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, preview_target, Qt::NoButton, Qt::LeftButton);
    require(wait_for_exact_move_preview(canvas),
            "Escape fixture first obtains an accepted move preview");
    send_mouse(canvas, QEvent::MouseButtonRelease, release_target, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(canvas.entitiesMovePreviewPending(),
            "different release target remains pending before its deferred candidate commits");

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    require(wait_for_quiet_document_state(window, canvas, before.revision()),
            "Escape cancels the final-target solve and settles the gesture without a command");
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() && after.entities() == before.entities(),
            "Escape while exact release preview is pending never persists the wall move");
}

void test_plan_canvas_async_unavailable_candidate_never_commits() {
    MainWindow window;
    window.resize(1000, 700);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!first.isEmpty() && !second.isEmpty(),
            "unavailable-candidate fixture creates a connected wall pair");
    require(window.selectEntity(first),
            "unavailable-candidate fixture selects its host wall");
    const auto before = window.document().snapshot();
    const auto baseline = wall_baseline(before.entities().at(first.toStdString()));
    const auto grab = frame_grab_point(canvas, baseline);
    const auto delta = Vec2{0.0, 0.5};
    const auto target = grab + QPointF(delta.x * canvas.viewScale(),
                                      -delta.y * canvas.viewScale());

    std::uint64_t pending_serial{};
    int move_commits{};
    int rejected_moves{};
    canvas.setEntitiesMoveRequested([&](QStringList, Vec2) {
        ++move_commits;
        return true;
    });
    canvas.setEntitiesMoveRejected([&](QStringList, Vec2) { ++rejected_moves; });
    canvas.setEntitiesMovePreviewRequested(
        [&](QStringList, Vec2 requested_delta, std::uint64_t serial)
            -> std::optional<std::vector<CanvasEntity>> {
            if (std::abs(requested_delta.y) > 0.1) {
                pending_serial = serial;
                require(canvas.markEntitiesMovePreviewPending(serial),
                        "fault-injected exact candidate marks its live serial pending");
            }
            return std::nullopt;
        });

    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    require(pending_serial != 0 && canvas.entitiesMovePreviewPending(),
            "fault-injected connected move waits for its asynchronous exact result");
    send_mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(canvas.entitiesMovePreviewPending(),
            "release retains the pending exact result until it can decide the transaction");

    require(canvas.completeEntitiesMovePreview(pending_serial, std::nullopt),
            "canvas accepts completion of the matching serial with no available proposal");
    require(wait_for_quiet_document_state(window, canvas, before.revision()),
            "unavailable exact result clears preview and settles release without a command");
    require(move_commits == 0 && rejected_moves == 1,
            "a missing exact wall proposal invokes rejection once and never the commit callback");
    require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "missing asynchronous proposal leaves all committed wall geometry unchanged");
}

void test_plan_canvas_exact_move_provider_release_contract() {
    MainWindow window;
    window.resize(1000, 700);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    const auto wall = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    require(!wall.isEmpty() && window.selectEntity(wall),
            "exact move provider fixture selects a wall");
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);
    const auto before = window.document().snapshot();
    const auto grab = frame_grab_point(canvas, wall_baseline(before.entities().at(wall.toStdString())));
    const Vec2 intermediate_delta{0.0, 0.35};
    const Vec2 final_delta{0.0, 0.6};
    const auto intermediate = grab + QPointF(0.0, -intermediate_delta.y * canvas.viewScale());
    const auto release = grab + QPointF(0.0, -final_delta.y * canvas.viewScale());
    int commits{};
    int rejections{};
    Vec2 committed_delta{};
    Vec2 requested_delta{};
    std::uint64_t pending_serial{};
    canvas.setEntitiesMoveRequested([&](QStringList ids, Vec2 delta) {
        require(ids == QStringList{wall}, "move commit retains the selected identity");
        committed_delta = delta;
        ++commits;
        return true;
    });
    canvas.setEntitiesMoveRejected([&](QStringList, Vec2) { ++rejections; });
    canvas.setEntitiesMovePreviewRequested(
        [](QStringList, Vec2, std::uint64_t) -> std::optional<std::vector<CanvasEntity>> {
            return std::nullopt;
        });
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, intermediate, Qt::NoButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton);
    require(commits == 0 && rejections == 1 && !canvas.entitiesMovePreviewPending(),
            "configured synchronous null proposal refuses release exactly once");

    canvas.setEntitiesMovePreviewRequested(
        [&](QStringList, Vec2 delta, std::uint64_t serial)
            -> std::optional<std::vector<CanvasEntity>> {
            requested_delta = delta;
            pending_serial = serial;
            require(canvas.markEntitiesMovePreviewPending(serial),
                    "controlled move provider marks its live request pending");
            return std::nullopt;
        });
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, intermediate, Qt::NoButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    const auto canceled_serial = pending_serial;
    require(canvas.entitiesMovePreviewPending() && nearly_equal(requested_delta, final_delta),
            "released async move requests the final pointer position");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    require(!canvas.entitiesMovePreviewPending(), "Escape clears released pending state immediately");
    require(!canvas.completeEntitiesMovePreview(canceled_serial, canvas.entities()),
            "late completion after Escape cannot revive the canceled move");
    QApplication::processEvents();
    require(commits == 0 && rejections == 1, "canceled move invokes no commit or rejection");

    // The next gesture must work without replacing the provider or scene.
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, intermediate, Qt::NoButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(pending_serial != canceled_serial && canvas.entitiesMovePreviewPending(),
            "next gesture starts a fresh pending release after cancellation");
    require(canvas.completeEntitiesMovePreview(pending_serial, canvas.entities()),
            "matching final async candidate completes");
    QApplication::processEvents();
    QApplication::processEvents();
    require(commits == 1 && nearly_equal(committed_delta, final_delta) &&
                !canvas.entitiesMovePreviewPending(),
            "async release commits its final position exactly once");
    require(!canvas.completeEntitiesMovePreview(pending_serial, canvas.entities()),
            "repeated async completion cannot commit twice");

    canvas.setEntitiesMovePreviewRequested(
        [&](QStringList, Vec2 delta, std::uint64_t) -> std::optional<std::vector<CanvasEntity>> {
            requested_delta = delta;
            return canvas.entities();
        });
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, intermediate, Qt::NoButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton);
    QApplication::processEvents();
    require(commits == 2 && rejections == 1 && nearly_equal(requested_delta, final_delta) &&
                nearly_equal(committed_delta, final_delta),
            "synchronous exact release commits final position exactly once");

    canvas.setEntitiesMovePreviewRequested({});
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton);
    require(commits == 3 && rejections == 1 && nearly_equal(committed_delta, final_delta),
            "legacy move without a preview provider retains simple translation");
    require(window.document().snapshot().entities() == before.entities(),
            "controlled canvas callbacks keep the native fixture document unchanged");
}

void test_canvas_ctrl_click_group_move_round_trips() {
    MainWindow window;
    window.resize(1200, 800);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({-2.0, -1.0}, {1.0, -1.0});
    const auto second = window.createStraightWall({-2.0, 2.0}, {-2.0, 4.0});
    require(!first.isEmpty() && !second.isEmpty(), "group fixture creates two independent walls");
    canvas.fitView();
    QApplication::processEvents();
    const auto before = window.document().snapshot();
    const auto first_before = wall_baseline(before.entities().at(first.toStdString()));
    const auto second_before = wall_baseline(before.entities().at(second.toStdString()));
    click(canvas, model_to_canvas(canvas,
        {(first_before.start.x + first_before.end.x) * 0.5,
         (first_before.start.y + first_before.end.y) * 0.5}));
    click(canvas, model_to_canvas(canvas,
        {(second_before.start.x + second_before.end.x) * 0.5,
         (second_before.start.y + second_before.end.y) * 0.5}), Qt::ControlModifier);
    require(window.selectedEntityIds().size() == 2 &&
                window.selectedEntityIds().contains(first) &&
                window.selectedEntityIds().contains(second),
            "actual Control-click canvas events add both walls to the retained selection");

    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    const auto grab = frame_grab_point(canvas, first_before);
    const auto move_delta = Vec2{0.45, 0.35};
    const auto release = grab + QPointF(move_delta.x * canvas.viewScale(),
                                       -move_delta.y * canvas.viewScale());
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, release, Qt::NoButton, Qt::LeftButton);
    require(wait_for_exact_move_preview(canvas),
            "multiwall selection publishes a group-move preview through canvas input");
    require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "group-move preview remains transient until release");
    const auto preview = canvas.entitiesMovePreview();
    const auto first_preview = canvas_wall_baseline(find_canvas_entity(preview, first));
    const auto second_preview = canvas_wall_baseline(find_canvas_entity(preview, second));
    require(nearly_equal(first_preview.start, {first_before.start.x + move_delta.x,
                                       first_before.start.y + move_delta.y}) &&
                nearly_equal(first_preview.end, {first_before.end.x + move_delta.x,
                                         first_before.end.y + move_delta.y}) &&
                nearly_equal(second_preview.start, {second_before.start.x + move_delta.x,
                                            second_before.start.y + move_delta.y}) &&
                nearly_equal(second_preview.end, {second_before.end.x + move_delta.x,
                                          second_before.end.y + move_delta.y}),
            "one group drag translates every endpoint of each selected wall equally");

    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(wait_for_move_commit(window, canvas, before.revision() + 1),
            "group-move release completes through its deferred canvas commit");
    const auto moved = window.document().snapshot();
    require(moved.revision() == before.revision() + 1,
            "group drag commits both walls in one document revision");
    require(window.selectedEntityIds().size() == 2 &&
                window.selectedEntityIds().contains(first) &&
                window.selectedEntityIds().contains(second),
            "canvas group move retains both selected wall identities");

    QTemporaryDir directory;
    require(directory.isValid(), "group-move archive fixture has a temporary directory");
    const auto path = directory.filePath(QStringLiteral("wall-group-move.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path),
            "group-moved walls save and reopen through the project workflow");
    const auto reopened = window.document().snapshot();
    require(reopened.entities() == moved.entities(),
            "reopened project retains the exact group-moved wall geometry");
}

void test_rigid_wall_rotation_keeps_connected_corner_and_hosted_opening() {
    MainWindow window;
    window.setMetricUnits(true);
    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!first.isEmpty() && !second.isEmpty(), "rotation fixture creates a connected wall pair");
    require(window.selectEntity(first), "rotation fixture selects the horizontal host wall");
    const auto opening = window.createHostedOpening(QStringLiteral("window"), QStringLiteral("1 m"),
        QStringLiteral("0.8 m"), QStringLiteral("0.8 m"), QStringLiteral("1.2 m"));
    require(!opening.isEmpty() && window.selectEntity(first),
            "rotation fixture creates and then reselects its hosted window");
    const auto before = window.document().snapshot();
    const auto before_opening = before.entities().at(opening.toStdString());
    const auto before_second = wall_baseline(before.entities().at(second.toStdString()));

    require(window.transformSelectedBoundary(QStringLiteral("90"), false, false,
                QStringLiteral("0 m"), QStringLiteral("0 m"), false),
            "public rigid wall transform accepts a connected host rotation");
    const auto rotated = window.document().snapshot();
    require(rotated.revision() == before.revision() + 1,
            "connected rigid wall rotation commits as one history command");
    const auto first_after = wall_baseline(rotated.entities().at(first.toStdString()));
    const auto second_after = wall_baseline(rotated.entities().at(second.toStdString()));
    require(nearly_equal(first_after.start, {1.5, -1.5}) &&
                nearly_equal(first_after.end, {1.5, 1.5}),
            "selected wall rotates rigidly about its endpoint midpoint");
    require(nearly_equal(second_after.start, first_after.end) &&
                nearly_equal(second_after.end, before_second.end),
            "connected wall's shared endpoint follows the rigid transform while its far end stays anchored");
    require(rotated.entities().at(opening.toStdString()) == before_opening,
            "hosted window keeps its exact semantic offset and width through host rotation");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
            "one undo restores the connected host, neighbor and opening after rotation");
    require(window.redoCommand() && window.document().snapshot().entities() == rotated.entities(),
            "one redo restores the exact connected wall rotation");
}

void test_fixed_anchor_conflict_rejects_selected_connected_wall_move_atomically() {
    MainWindow window;
    window.resize(1000, 700);
    window.setMetricUnits(true);
    window.show();
    window.setWorkspace(Workspace::measurement);
    QApplication::processEvents();
    auto& canvas = measurement_canvas(window);
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);

    const auto first = window.createStraightWall({0.0, 0.0}, {3.0, 0.0});
    const auto second = window.createStraightWall({3.0, 0.0}, {3.0, 2.0});
    require(!first.isEmpty() && !second.isEmpty(), "fixed-anchor fixture creates a connected wall pair");
    PersistentConstraint pin;
    pin.id = "canvas-wall-start-pin";
    pin.relation = ConstraintRelationKind::fixed_anchor;
    pin.bindings = {{first.toStdString(), WallEndpointRole::start}};
    pin.anchor = Vec2{0.0, 0.0};
    window.document().apply(ApplyEntityChanges{
        window.document().revision(),
        {EntityChange::upsert(encode_constraint_entity(pin))}, {},
        "Pin connected wall start for canvas move rejection"});
    require(window.selectEntity(first), "pinned wall selects for rejected canvas drag");
    const auto before = window.document().snapshot();
    const auto baseline = wall_baseline(before.entities().at(first.toStdString()));
    canvas.setSnapEnabled(false);
    canvas.setWallSnapEnabled(false);
    const auto grab = frame_grab_point(canvas, baseline);
    const QPointF release = grab + QPointF(0.0, -0.4 * canvas.viewScale());
    send_mouse(canvas, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseMove, release, Qt::NoButton, Qt::LeftButton);
    require(wait_for_move_preview_settled(canvas),
            "fixed-anchor move preview finishes without blocking the Qt event loop");
    require(canvas.entitiesMovePreview().empty(),
            "fixed-anchor conflict exposes no accepted connected-wall preview");
    require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "rejected fixed-anchor preview cannot mutate committed document state");

    send_mouse(canvas, QEvent::MouseButtonRelease, release, Qt::LeftButton,
               Qt::NoButton, Qt::NoModifier, false);
    require(wait_for_quiet_document_state(window, canvas, before.revision()),
            "anchored release settles without a deferred document command");
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() && after.entities() == before.entities(),
            "release cannot partially commit an infeasible anchored connected-wall move");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-connected-wall-canvas-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font_id >= 0)
        QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
    try {
        test_selected_connected_wall_move_previews_commits_and_round_trips();
        test_canvas_release_without_intermediate_move_commits_pointer_target();
        test_canvas_wall_move_release_refuses_stale_document_head();
        test_escape_cancels_exact_release_preview_before_commit();
        test_plan_canvas_async_unavailable_candidate_never_commits();
        test_plan_canvas_exact_move_provider_release_contract();
        test_canvas_ctrl_click_group_move_round_trips();
        test_rigid_wall_rotation_keeps_connected_corner_and_hosted_opening();
        test_fixed_anchor_conflict_rejects_selected_connected_wall_move_atomically();
    } catch (const std::exception& error) {
        std::cerr << "connected_wall_canvas_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Connected wall canvas tests passed\n";
    return 0;
}
