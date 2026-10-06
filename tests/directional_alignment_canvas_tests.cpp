#include "sketch/desktop/main_window.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/measurement_linework.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
bool same(Vec2 a, Vec2 b) { return std::hypot(a.x-b.x, a.y-b.y) < 1e-10; }
QPointF screen(const PlanCanvas& canvas, Vec2 point) {
    return QRectF(canvas.rect()).center() + QPointF(
        (point.x-canvas.viewCenter().x)*canvas.viewScale(),
        -(point.y-canvas.viewCenter().y)*canvas.viewScale());
}
void mouse(PlanCanvas& canvas, QEvent::Type type, Vec2 point,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    const auto position = screen(canvas, point);
    QMouseEvent event(type, position, canvas.mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    events();
}
void click(PlanCanvas& canvas, Vec2 point) {
    mouse(canvas, QEvent::MouseMove, point);
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
}
void key(QWidget& widget, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         bool repeat = false) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, {}, repeat);
    QApplication::sendEvent(&widget, &event);
    events();
}
Segment baseline(const Entity& entity) {
    const auto& value = entity.properties.at("baseline");
    return {{value.at("start").at(0).get<double>(), value.at("start").at(1).get<double>()},
        {value.at("end").at(0).get<double>(), value.at("end").at(1).get<double>()},
        value.value("sweep_radians", 0.0)};
}
CanvasEntity structural(const QString& type, Boundary segments) {
    CanvasEntity entity;
    entity.id = type;
    entity.type = type;
    entity.segments = segments;
    if (type == QStringLiteral("wall")) entity.snap_segments = std::move(segments);
    return entity;
}
void target(const PlanCanvas& canvas, Vec2 origin, int dx, int dy, bool intersections_only,
            std::optional<Vec2> expected, const char* message) {
    const auto result = canvas.directionalDrawingAlignment(origin, dx, dy, intersections_only);
    if (result.has_value() != expected.has_value() || (result && !same(*result, *expected))) {
        std::cerr << std::setprecision(17) << "resolver expected ";
        if (expected) std::cerr << '(' << expected->x << ',' << expected->y << ')';
        else std::cerr << "no target";
        std::cerr << ", actual ";
        if (result) std::cerr << '(' << result->x << ',' << result->y << ')';
        else std::cerr << "no target";
        std::cerr << ", origin(" << origin.x << ',' << origin.y << "), direction(" << dx << ',' << dy << ")\n";
        for (const auto& entity : canvas.entities()) {
            const auto& segments = !entity.drawing_alignment_segments.empty() ? entity.drawing_alignment_segments :
                !entity.snap_segments.empty() ? entity.snap_segments : entity.segments;
            for (const auto& segment : segments) {
                if (segment.sweep_radians == 0.0) continue;
                const auto bounds = segment_bounds(segment);
                std::cerr << "arc start(" << segment.start.x << ',' << segment.start.y << ") end("
                    << segment.end.x << ',' << segment.end.y << ") sweep=" << segment.sweep_radians
                    << " bounds min(" << bounds.minimum.x << ',' << bounds.minimum.y << ") max("
                    << bounds.maximum.x << ',' << bounds.maximum.y << ")\n";
                if (same(segment.start, {3, 0}) && same(segment.end, {1, 0}) &&
                    segment.sweep_radians == std::numbers::pi && dx != 0) {
                    // Independent unit-chord values for this semicircle's
                    // horizontal ray; diagnostics only, never an oracle.
                    const auto sine = std::sin(segment.sweep_radians*0.5);
                    const auto cosine = std::cos(segment.sweep_radians*0.5);
                    const auto k = std::fma(sine, origin.y*origin.y*0.25, cosine*origin.y*0.5);
                    std::cerr << "semicircle half-angle sin=" << sine << " cos=" << cosine
                        << " chord quadratic A=" << sine << " B=" << -sine << " K=" << k
                        << " D=" << std::fma(sine, sine, -4.0*sine*k) << '\n';
                }
            }
        }
        std::cerr << std::setprecision(6);
    }
    require(result.has_value() == expected.has_value() && (!result || same(*result, *expected)), message);
    if (result) require(dx != 0 ? result->y == origin.y : result->x == origin.x,
        "directional resolution preserves the untouched world coordinate exactly");
}
void endpoint_coordinates_advance_exactly_without_artwork_or_grid_targets() {
    PlanCanvas canvas;
    const Vec2 origin{0.01317, -0.01931};
    auto wall = structural("wall", {{{1.24567, 8.11931}, {3.24567, 8.11931}, 0}});
    // Deliberately nearer painted faces must not replace analytical baselines.
    wall.segments = {{{0.11317, 0}, {0.21317, 0}, 0}};
    wall.snap_points = {{1.24567, 8.11931}, {3.24567, 8.11931}};
    auto symbol = structural("symbol", {{{0.02317, 0}, {0.03317, 0}, 0}});
    symbol.snap_segments = symbol.segments;
    symbol.snap_points = {{0.02317, 0}};
    auto dimension = structural("dimension_line", {{{0.04317, 0}, {0.05317, 0}, 0}});
    canvas.setEntities({wall, symbol, dimension,
        structural("boundary", {{{-2.01317, 0}, {-1.01317, 0}, 0}}),
        structural("room_boundary", {{{0, 4.01317}, {0, 5.01317}, 0}}),
        structural("measurement_boundary", {{{0, -4.01317}, {0, -3.01317}, 0}})});
    canvas.setViewTransform({100, -50}, 0.001);
    canvas.setSnapEnabled(true);
    target(canvas, origin, 1, 0, false, Vec2{1.24567, origin.y}, "Ctrl+Right uses baseline endpoint X");
    target(canvas, {1.24567, origin.y}, 1, 0, false, Vec2{3.24567, origin.y},
        "repeated Ctrl+Right advances past duplicate endpoint coordinates");
    target(canvas, {3.24567, origin.y}, 1, 0, false, {}, "last endpoint produces no invented target");
    target(canvas, origin, -1, 0, false, Vec2{0, origin.y}, "Ctrl+Left orders all visible structural endpoints");
    target(canvas, {-0.01317, origin.y}, -1, 0, false, Vec2{-1.01317, origin.y}, "Ctrl+Left moves negatively");
    target(canvas, origin, 0, 1, false, Vec2{origin.x, 0}, "Ctrl+Up preserves exact X");
    target(canvas, {origin.x, 0}, 0, 1, false, Vec2{origin.x, 4.01317}, "Ctrl+Up includes room boundaries");
    target(canvas, origin, 0, -1, false, Vec2{origin.x, -3.01317}, "Ctrl+Down includes measured boundaries");
    canvas.setViewTransform({0, 0}, 4000);
    canvas.setSnapEnabled(false);
    target(canvas, origin, 1, 0, false, Vec2{1.24567, origin.y}, "zoom and snapping do not change exact target");
    canvas.setEntities({structural("boundary", {{{origin.x+1e-10, 10}, {4, 10}, 0}})});
    target(canvas, origin, 1, 0, false, Vec2{origin.x+1e-10, origin.y},
        "representable endpoint coordinates are never rounded to a grid or metre tolerance");
}
void independent_measurement_strokes_are_analytical_alignment_targets() {
    PlanCanvas canvas;
    const Vec2 origin{0.01317, -0.01931};
    auto stroke = structural("measurement_linework", {{{1.24567, -3}, {1.24567, 4}, 0}});
    stroke.snap_points = {{1.24567, -3}, {1.24567, 4}};
    stroke.snap_segments = stroke.segments;
    canvas.setEntities({stroke});
    target(canvas, origin, 1, 0, false, Vec2{1.24567, origin.y},
        "loose measured stroke endpoints participate in Ctrl-arrow alignment");
    target(canvas, origin, 1, 0, true, Vec2{1.24567, origin.y},
        "loose measured stroke analytical span participates in Ctrl-Shift-arrow intersection");
}
void intersections_use_actual_ray_contacts_and_accepted_draft_geometry() {
    PlanCanvas canvas;
    canvas.setEntities({structural("wall", {{{1, 3}, {1, 4}, 0}}),
        structural("boundary", {{{2, -2}, {6, 2}, 0}}),
        structural("room_boundary", {{{8, -1}, {8, 1}, 0}})});
    target(canvas, {0, 0}, 1, 0, false, Vec2{1, 0}, "plain Ctrl sees an off-ray endpoint coordinate");
    target(canvas, {0, 0}, 1, 0, true, Vec2{4, 0}, "Ctrl+Shift skips off-ray endpoints and crosses a diagonal");
    target(canvas, {4, 0}, 1, 0, true, Vec2{8, 0}, "repeat skips the contact at the current cursor");
    target(canvas, {8, 0}, 1, 0, true, {}, "no forward contact leaves the cursor unchanged");
    target(canvas, {7, 0}, -1, 0, true, Vec2{4, 0}, "negative ray finds exact line crossing");
    target(canvas, {4, -3}, 0, 1, true, Vec2{4, 0}, "vertical ray intersects diagonal analytically");
    target(canvas, {4, 3}, 0, -1, true, Vec2{4, 0}, "down ray finds the closest exact intersection");
    BoundaryDraftPreview preview;
    preview.segments = {{{0.75, -1}, {0.75, 1}, 0}};
    preview.rubber_band = Segment{{0.25, -1}, {0.25, 1}, 0};
    preview.pen_position = Vec2{0.5, 0};
    preview.anchor = Vec2{0.125, 0};
    canvas.setBoundaryDraftPreview(preview);
    target(canvas, {0, 0}, 1, 0, true, Vec2{0.75, 0}, "only accepted draft segments participate in intersection targets");
    target(canvas, {0, 0}, 1, 0, false, Vec2{0.75, 0}, "draft rubber band and unattached cursor are excluded from endpoint targets");
    canvas.setBoundaryDraftPreview({});
    canvas.setEntities({structural("boundary", {{{2, 0}, {5, 0}, 0}})});
    target(canvas, {0, 0}, 1, 0, true, Vec2{2, 0}, "coincident segment chooses its nearest finite endpoint");
    target(canvas, {2, 0}, 1, 0, true, Vec2{5, 0}, "coincident repeat advances to the other endpoint");
    target(canvas, {3, 0}, 1, 0, true, Vec2{5, 0}, "cursor inside coincident segment chooses forward endpoint");
    target(canvas, {0, 1e-10}, 1, 0, true, {}, "parallel near-ray line is not an actual intersection");
}
void directional_wall_baselines_are_independent_of_mouse_snap_geometry() {
    PlanCanvas canvas;
    auto wall = structural("wall", {{{4, -1}, {4, 1}, 0}});
    wall.segments = {{{0.25, -1}, {0.25, 1}, 0}};
    wall.drawing_alignment_segments = {{{2, -1}, {2, 1}, 0}};
    canvas.setEntities({wall});
    for (const bool intersections : {false, true})
        target(canvas, {0, 0}, 1, 0, intersections, Vec2{2, 0},
            "dedicated analytical baseline takes precedence over mouse snap and painted geometry");
    wall.snap_segments.clear();
    canvas.setEntities({wall});
    for (const bool intersections : {false, true})
        target(canvas, {0, 0}, 1, 0, intersections, Vec2{2, 0},
            "visible wall baseline remains directional geometry without ordinary mouse snap targets");
    wall.drawing_alignment_segments.clear();
    canvas.setEntities({wall});
    for (const bool intersections : {false, true})
        target(canvas, {0, 0}, 1, 0, intersections, {},
            "painted wall footprint cannot substitute for an unavailable analytical baseline");
}
void analytical_arcs_keep_sweeps_tangencies_and_endpoint_contacts() {
    PlanCanvas canvas;
    canvas.setEntities({structural("wall", {{{3, 0}, {1, 0}, std::numbers::pi}})});
    target(canvas, {0, 0.5}, 1, 0, true, Vec2{1.1339745962155614, 0.5}, "semicircle intersects at exact first circle root");
    target(canvas, {1.1339745962155614, 0.5}, 1, 0, true, Vec2{2.8660254037844386, 0.5},
        "repeat advances to second arc intersection without reselecting the first");
    target(canvas, {2.8660254037844386, 0.5}, 1, 0, true, {}, "arc sweep has no further intersection");
    target(canvas, {0, 1}, 1, 0, true, Vec2{2, 1}, "arc tangent is a true single contact");
    target(canvas, {0, std::nextafter(1.0, std::numeric_limits<double>::infinity())}, 1, 0, true, {},
        "one representable step outside the canonical semicircle is not a tangent");
    const auto just_inside = std::nextafter(1.0, 0.0);
    const auto near_root = std::sqrt(1.0-just_inside*just_inside);
    target(canvas, {0, just_inside}, 1, 0, true, Vec2{2-near_root, just_inside},
        "one representable step inside the semicircle retains two distinct contacts");
    target(canvas, {2-near_root, just_inside}, 1, 0, true, Vec2{2+near_root, just_inside},
        "canonical semicircle near-tangent repeat advances to the second true contact");
    target(canvas, {2, 1}, 1, 0, true, {}, "repeated tangency has no positive movement");
    target(canvas, {0, 0}, 1, 0, true, Vec2{1, 0}, "arc endpoints are admitted as ray contacts");
    target(canvas, {0, -0.5}, 1, 0, true, {}, "circle roots outside the signed sweep are excluded");
    target(canvas, {2, -2}, 0, 1, true, Vec2{2, 1}, "vertical ray finds analytical curved extremum");
    target(canvas, {0, 0.5}, 1, 0, false, Vec2{1, 0.5}, "plain Ctrl aligns arc endpoint coordinates rather than extrema");
    canvas.setEntities({structural("wall", {{{3, 0}, {1, 0}, -std::numbers::pi}})});
    target(canvas, {0, -0.5}, 1, 0, true, Vec2{1.1339745962155614, -0.5}, "clockwise arc preserves its negative sweep");
    canvas.setEntities({structural("wall", {{{3, 0}, {2, 1}, -1.5*std::numbers::pi}})});
    target(canvas, {0, 0.5}, 1, 0, true, Vec2{1.1339745962155614, 0.5}, "major arc retains contact in its long sweep");
    target(canvas, {1.1339745962155614, 0.5}, 1, 0, true, {}, "major arc excludes the missing short quadrant");
}
void long_empty_rays_preserve_near_tangent_arc_contacts() {
    PlanCanvas canvas;
    canvas.setEntities({structural("wall", {{{3, 0}, {1, 0}, std::numbers::pi}})});
    const double y = 0.9999999;
    const double root = std::sqrt(1-y*y);
    target(canvas, {-1e8, y}, 1, 0, true, Vec2{2-root, y},
        "long empty ray must retain the first genuine near-tangent arc root");
    target(canvas, {1e8, y}, -1, 0, true, Vec2{2+root, y},
        "long negative empty ray must retain its first genuine near-tangent arc root");
    target(canvas, {-1e8, 1.0000001}, 1, 0, true, {},
        "long empty ray outside the circle must not invent a tangent contact");
    const double x = 2.9999999;
    const double height = std::sqrt(1-(x-2)*(x-2));
    target(canvas, {x, -1e8}, 0, 1, true, Vec2{x, height},
        "long vertical ray retains its actual near-endpoint arc intersection");
    target(canvas, {3.0000001, -1e8}, 0, 1, true, {},
        "long vertical ray outside the circle has no contact");
}
void shallow_arc_intersection_keeps_the_first_actual_contact() {
    PlanCanvas canvas;
    const Segment shallow{{0, 0}, {2, 0}, 1e-8};
    canvas.setEntities({structural("wall", {shallow})});
    // The shallow arc has sagitta -2.5e-9; y=-1e-9 cuts it at
    // x=1 +/- sqrt(0.6), rather than at the tangent midpoint.
    target(canvas, {-1, -1e-9}, 1, 0, true, Vec2{0.2254033307585166, -1e-9},
        "shallow arc retains the nearest genuine contact instead of inventing a midpoint tangent");
    target(canvas, {3, -1e-9}, -1, 0, true, Vec2{1.7745966692414834, -1e-9},
        "negative ray orders the two shallow arc contacts in reverse");
    target(canvas, {0.2254033307585166, -1e-9}, 1, 0, true, Vec2{1.7745966692414834, -1e-9},
        "shallow arc repeat advances to its second genuine contact");
    target(canvas, {0.5, 0}, 1, 0, true, Vec2{2, 0},
        "shallow arc endpoint bound does not prove a false interior tangent");
    target(canvas, {1, -1e-8}, 0, 1, true, Vec2{1, -2.5e-9},
        "vertical ray preserves shallow arc sagitta without distant-center cancellation");
    const auto bottom = segment_bounds(shallow).minimum.y;
    // This stored bound is -sweep/4. The true sagitta is -tan(sweep/4),
    // strictly below it, so the rounded ordinate does not prove a tangent.
    // The two very close contacts are numerically unresolved at this scale.
    target(canvas, {-1, bottom}, 1, 0, true, {},
        "rounded shallow arc bound must not invent a midpoint tangent");
    target(canvas, {1, bottom}, 1, 0, true, {},
        "unresolved rounded shallow bound cannot invent a repeated contact");
    target(canvas, {-1, std::nextafter(bottom, -std::numeric_limits<double>::infinity())}, 1, 0, true, {},
        "one representable step outside a shallow arc cannot become a tangent");
    canvas.setEntities({structural("wall", {{{0, 0}, {2, 0}, -1e-8}})});
    target(canvas, {-1, 1e-9}, 1, 0, true, Vec2{0.2254033307585166, 1e-9},
        "negative shallow sweep reflects the actual contacts above the chord");
    target(canvas, {0.5, 0}, 1, 0, true, Vec2{2, 0},
        "negative shallow sweep endpoint bound cannot invent an interior tangent");
    canvas.setEntities({structural("wall", {{{2, 0}, {0, 0}, -1e-8}})});
    target(canvas, {-1, -1e-9}, 1, 0, true, Vec2{0.2254033307585166, -1e-9},
        "reversing endpoints and sweep retains the same shallow arc contact");
    canvas.setEntities({structural("wall", {{{0, 0}, {0, 2}, 1e-8}})});
    target(canvas, {1e-9, -1}, 0, 1, true, Vec2{1e-9, 0.2254033307585166},
        "vertical shallow chord preserves the nearest upward ray contact");
    target(canvas, {1e-9, 3}, 0, -1, true, Vec2{1e-9, 1.7745966692414834},
        "vertical shallow chord preserves the nearest downward ray contact");
    canvas.setEntities({structural("wall", {{{3, 0}, {2, 1}, -1.5*std::numbers::pi}})});
    target(canvas, {4, 0.5}, -1, 0, true, Vec2{1.1339745962155614, 0.5},
        "major arc half-plane membership excludes its missing short quadrant from a reverse ray");
    canvas.setEntities({structural("wall", {{{2, 1}, {3, 0}, 1.5*std::numbers::pi}})});
    target(canvas, {0, 0.5}, 1, 0, true, Vec2{1.1339745962155614, 0.5},
        "major arc reversal preserves its signed sweep and exact first contact");
}
void distant_origins_order_targets_by_exact_world_coordinate() {
    PlanCanvas canvas;
    canvas.setEntities({structural("boundary", {{{1.05, -1}, {1.05, 1}, 0}, {{1, -1}, {1, 1}, 0}})});
    for (const bool intersections : {false, true})
        target(canvas, {-1e15, 0}, 1, 0, intersections, Vec2{1, 0},
            "distant origin cannot tie distinct positive target coordinates by subtraction rounding");
    canvas.setEntities({structural("boundary", {{{-1.05, -1}, {-1.05, 1}, 0}, {{-1, -1}, {-1, 1}, 0}})});
    for (const bool intersections : {false, true})
        target(canvas, {1e15, 0}, -1, 0, intersections, Vec2{-1, 0},
            "distant origin cannot tie distinct negative target coordinates by subtraction rounding");
    canvas.setEntities({structural("boundary", {{{1e308, -1}, {1e308, 1}, 0}})});
    target(canvas, {-1e308, 0}, 1, 0, false, Vec2{1e308, 0},
        "finite endpoint coordinates remain ordered even when origin distance would overflow");
}
void malformed_geometry_and_non_cardinal_directions_fail_closed() {
    PlanCanvas canvas;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    auto invalid = structural("wall", {{{nan, 0}, {1, 0}, 0}, {{2, 0}, {2, 0}, 0},
        {{3, 0}, {4, 0}, 2*std::numbers::pi}});
    invalid.snap_points = {{nan, 0}, {5, nan}};
    canvas.setEntities({invalid});
    target(canvas, {0, 0}, 1, 0, false, {}, "invalid structural geometry cannot become endpoint targets");
    target(canvas, {0, 0}, 1, 0, true, {}, "invalid structural geometry cannot become intersection targets");
    canvas.setEntities({structural("wall", {{{1, -1}, {1, 1}, 0}})});
    for (const auto direction : {std::pair{0, 0}, std::pair{1, 1}, std::pair{2, 0}})
        target(canvas, {0, 0}, direction.first, direction.second, false, {}, "non-cardinal resolver input is rejected");
    target(canvas, {nan, 0}, 1, 0, false, {}, "non-finite origin cannot move");
}
void directional_dispatch_requires_focused_idle_drawing_context() {
    PlanCanvas canvas;
    canvas.setAttribute(Qt::WA_DontShowOnScreen, true);
    canvas.resize(600, 400);
    canvas.show();
    QApplication::setActiveWindow(&canvas);
    canvas.setTool(CanvasTool::wall);
    canvas.setFocus();
    events();
    require(canvas.hasFocus(), "isolated canvas receives drawing keyboard focus");
    std::vector<std::tuple<int, int, bool>> requests;
    canvas.setDirectionalAlignmentRequested([&](int dx, int dy, bool intersections) {
        requests.emplace_back(dx, dy, intersections);
    });
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    key(canvas, Qt::Key_Left, Qt::ControlModifier);
    canvas.setTool(CanvasTool::boundary);
    key(canvas, Qt::Key_Up, Qt::ControlModifier | Qt::ShiftModifier);
    key(canvas, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
    require(requests == std::vector<std::tuple<int, int, bool>>{
        {1, 0, false}, {-1, 0, false}, {0, 1, true}, {0, -1, true}},
        "both drawing tools dispatch each axis with exact intersection intent");
    const auto count = requests.size();
    for (const auto modifiers : {Qt::KeyboardModifiers{Qt::NoModifier},
            Qt::KeyboardModifiers{Qt::ShiftModifier},
            Qt::KeyboardModifiers{Qt::ControlModifier | Qt::AltModifier},
            Qt::KeyboardModifiers{Qt::ControlModifier | Qt::MetaModifier}})
        key(canvas, Qt::Key_Right, modifiers);
    require(requests.size() == count, "nonexact Ctrl modifiers must not dispatch drawing alignment");
    key(canvas, Qt::Key_Right, Qt::ControlModifier, true);
    require(requests.size() == count, "autorepeated Ctrl+Arrow must not dispatch drawing alignment");
    auto selected_object = structural("wall", {{{-1, -1}, {-2, -1}, 0}});
    selected_object.id = "selected-object";
    canvas.setEntities({selected_object});
    canvas.setSelectedId("selected-object");
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "selected retained object must suppress drawing alignment");
    canvas.setSelectedIds({});
    canvas.setPointPlacementRequested([](Vec2) {});
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "point placement must suppress drawing alignment");
    canvas.setPointPlacementRequested({});
    canvas.setTool(CanvasTool::select);
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "Select tool must preserve its Ctrl+Arrow behavior");
    canvas.setTool(CanvasTool::sloped_wall);
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "Sloped Wall tool must not dispatch the Wall directional shortcut");
    canvas.setTool(CanvasTool::wall);
    mouse(canvas, QEvent::MouseButtonPress, {0, 0}, Qt::MiddleButton, Qt::MiddleButton);
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "middle-button navigation must suppress drawing alignment");
    mouse(canvas, QEvent::MouseButtonRelease, {0, 0}, Qt::MiddleButton);
    key(canvas, Qt::Key_Space);
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "space-armed navigation must suppress drawing alignment");
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
    canvas.clearFocus();
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(requests.size() == count, "selection, placement, gestures, focus, repeats and extra modifiers suppress drawing dispatch");
    QLineEdit input(&canvas);
    input.setText("one two");
    input.show();
    input.setFocus();
    input.setCursorPosition(7);
    key(input, Qt::Key_Left, Qt::ControlModifier);
    require(input.cursorPosition() == 4 && requests.size() == count,
        "Ctrl+Arrow retains native word navigation in focused text input");
}

void travel_dispatch_preserves_native_gesture_arbitration() {
    PlanCanvas canvas;
    canvas.setAttribute(Qt::WA_DontShowOnScreen,true);
    canvas.resize(600,400); canvas.show();
    QApplication::setActiveWindow(&canvas); canvas.setFocus(); events();
    canvas.setTool(CanvasTool::wall);
    int jumps=0, walks=0, lifts=0;
    canvas.setDrawingCornerJumpRequested([&] { ++jumps; return true; });
    canvas.setDrawingTravelRequested([&](int dx,int dy) { require(dx==1 && dy==0,"travel preserves arrow axis"); ++walks; return true; });
    canvas.setDrawingPenUpRequested([&] { ++lifts; return true; });
    key(canvas,Qt::Key_J); key(canvas,Qt::Key_Right); key(canvas,Qt::Key_Return);
    require(jumps==1 && walks==1 && lifts==1,"idle native keys dispatch corner jump, walk and pen-up");
    for (const auto modifiers : {Qt::ControlModifier,Qt::ShiftModifier,Qt::AltModifier}) {
        key(canvas,Qt::Key_J,modifiers); key(canvas,Qt::Key_Right,modifiers);
    }
    key(canvas,Qt::Key_J,Qt::NoModifier,true); key(canvas,Qt::Key_Right,Qt::NoModifier,true);
    canvas.setPointPlacementRequested([](Vec2) {});
    key(canvas,Qt::Key_J); key(canvas,Qt::Key_Right); key(canvas,Qt::Key_Return);
    canvas.setPointPlacementRequested({});
    auto selected=structural("wall",{{{0,0},{1,0},0}});
    selected.id="selected"; canvas.setEntities({selected}); canvas.setSelectedId("selected");
    key(canvas,Qt::Key_J); key(canvas,Qt::Key_Right); key(canvas,Qt::Key_Return);
    canvas.setSelectedIds({});
    mouse(canvas,QEvent::MouseButtonPress,{0,0},Qt::MiddleButton,Qt::MiddleButton);
    key(canvas,Qt::Key_J); key(canvas,Qt::Key_Right); key(canvas,Qt::Key_Return);
    mouse(canvas,QEvent::MouseButtonRelease,{0,0},Qt::MiddleButton);
    require(jumps==1 && walks==1 && lifts==1,
        "modifiers, repeats, placement, selection and active pan suppress travel dispatch");
}
PlanCanvas& native_canvas(MainWindow& window) {
    window.setMetricUnits(true);
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900);
    window.show();
    QApplication::setActiveWindow(&window);
    events();
    auto* snap = window.findChild<QToolButton*>("snapTool");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(snap && canvas, "native directional fixture uses actual drawing canvas and snap control");
    snap->setChecked(false);
    canvas->setSnapEnabled(false);
    canvas->setOverviewMapEnabled(false);
    canvas->setViewTransform({3, 1}, 65);
    canvas->setFocus();
    events();
    return *canvas;
}
void select_native_wall_drawing(MainWindow& window, PlanCanvas& canvas) {
    auto* mode = window.findChild<QComboBox*>("drawingMode");
    auto* wall = window.findChild<QAction*>("createWall");
    require(mode && wall && wall->isEnabled(), "native wall fixture uses the actual drawing controls");
    const auto index = mode->findData(QStringLiteral("wall"));
    require(index >= 0, "native drawing control exposes Wall mode");
    mode->setCurrentIndex(index);
    wall->trigger();
    canvas.setFocus();
    events();
    require(mode->currentData() == QStringLiteral("wall") && canvas.hasFocus() &&
        canvas.drawingCommandIdle() && !canvas.boundaryDraftPreview() && window.lastError().isEmpty(),
        "native wall fixture starts in focused idle Wall drawing mode");
}
const DrawingWitness& selected_directional(const PlanCanvas& canvas, Vec2 start, Vec2 end,
                                          const QString& last_error = {}) {
    const auto& witnesses = canvas.drawingWitnesses();
    const auto found = std::find_if(witnesses.begin(), witnesses.end(), [](const auto& value) {
        return value.selected && !value.command_text.isEmpty();
    });
    if (found == witnesses.end() || !same(found->segment.start, start) || !same(found->segment.end, end)) {
        std::cerr << "selected directional expected start(" << start.x << ',' << start.y << ") end("
            << end.x << ',' << end.y << "), host error: " << last_error.toStdString() << '\n';
        std::cerr << "canvas focus=" << canvas.hasFocus() << " idle=" << canvas.drawingCommandIdle()
            << " selected=" << std::count_if(canvas.entities().begin(), canvas.entities().end(),
                [](const auto& entity) { return entity.selected; })
            << " wall preview=" << canvas.wallPreview().has_value()
            << " boundary preview=" << canvas.boundaryDraftPreview().has_value() << '\n';
        if (canvas.wallPreview()) std::cerr << "wall pen(" << canvas.wallPreview()->start.x << ','
            << canvas.wallPreview()->start.y << ")\n";
        for (const auto& witness : witnesses)
            std::cerr << "witness selected=" << witness.selected << " command=" << witness.command_text.toStdString()
                << " start(" << witness.segment.start.x << ',' << witness.segment.start.y << ") end("
                << witness.segment.end.x << ',' << witness.segment.end.y << ")\n";
    }
    require(found != witnesses.end() && same(found->segment.start, start) && same(found->segment.end, end),
        "selected native directional witness displays exact original-to-target geometry");
    require(std::count_if(witnesses.begin(), witnesses.end(), [](const auto& value) { return value.selected; }) == 1,
        "only the active directional proposal is selected");
    return *found;
}
void native_measured_lines_accept_alignment_and_automatic_closure() {
    MainWindow window;
    auto& canvas=native_canvas(window);
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({1,-2}) &&
        window.appendMeasurementLineworkPoint({1,2}), "create loose alignment target");
    window.finishMeasurementLinework();
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}),
        "start independent measured stroke for alignment");
    canvas.setFocus(); events();
    key(canvas,Qt::Key_Right,Qt::ControlModifier); events();
    selected_directional(canvas,{0,0},{1,0},window.lastError());
    const auto source=window.document().snapshot();
    key(canvas,Qt::Key_Return); events();
    require(window.document().revision()==source.revision()+1, "Enter accepts measured-line alignment without ending the stroke");
    require(window.appendMeasurementLineworkPoint({1,1}) && window.appendMeasurementLineworkPoint({0,1}),
        "drawing continues after aligned measured edge");
    const auto before_close=window.document().snapshot();
    canvas.setFocus(); events();
    key(canvas,Qt::Key_A); events();
    require(window.document().revision()==before_close.revision()+1, "A closes measured linework without accessing a boundary session");
    bool found=false;
    const auto closed_snapshot = window.document().snapshot();
    for(const auto& [id,entity]:closed_snapshot.entities()) if(entity.type=="measurement_linework") {
        const auto model=*decode_measurement_linework_model(entity.properties.at("model")).model;
        if(model.closed) { found=true; require(model.edges.size()==4 && model.edges.back().receipt.kind==BoundaryConstructionKind::line_closure,
            "automatic measured closure retains its exact closure receipt"); }
    }
    require(found,"closed measured stroke survives actual native A command");
}
void repeated_native_proposals_and_boundary_phases(bool boundary, bool manual, bool first_side) {
    std::cout << "native directional: boundary=" << boundary << " manual=" << manual
        << " first_side=" << first_side << '\n' << std::flush;
    QTemporaryDir directory;
    require(directory.isValid(), "native directional phase fixture isolates library storage");
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    auto& canvas = native_canvas(window);
    const auto target_wall = window.createStraightWall({4.01317, 4.11931}, {6.24567, 4.11931});
    require(!target_wall.isEmpty() &&
        !window.createStraightWall({8.01317, 4.11931}, {9.24567, 4.11931}).isEmpty(),
        "native repeated fixture provides four independent endpoint coordinates");
    (void)window.selectEntity({});
    if (boundary) require(window.beginBoundaryDrawing(manual ? BoundaryAuthoringMode::define_first :
        BoundaryAuthoringMode::draw_first, "measurement"), "native classified boundary authoring starts");
    const Vec2 anchor{0.01317, -0.01931};
    const Vec2 endpoint = first_side ? anchor : Vec2{1.24567, 2.34567};
    click(canvas, anchor);
    if (!first_side) {
        click(canvas, endpoint);
        if (manual) {
            const auto pending = *canvas.boundaryDraftPreview();
            key(canvas, Qt::Key_Right, Qt::ControlModifier);
            require(canvas.boundaryDraftPreview()->segments.size() == pending.segments.size() &&
                canvas.boundaryDraftPreview()->instruction.contains("place this edge") &&
                std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
                    [](const auto& value) { return value.selected; }),
                "Ctrl+Arrow cannot bypass Define First manual dimension placement");
            click(canvas, {1.7, 0.5});
            require(!canvas.boundaryDraftPreview()->instruction.contains("place this edge"),
                "ordinary dimension click restores Define First drawing phase");
        }
    }
    mouse(canvas, QEvent::MouseMove, endpoint);
    const auto before = window.document().snapshot();
    const auto original_count = boundary ? canvas.boundaryDraftPreview()->segments.size() : 0;
    for (const auto theme : {WorkspaceTheme::light, WorkspaceTheme::dark}) {
        window.setWorkspaceTheme(theme);
        canvas.setFocus();
        mouse(canvas, QEvent::MouseMove, endpoint);
        const auto passive = canvas.grab().toImage();
        key(canvas, Qt::Key_Right, Qt::ControlModifier);
        const Vec2 first{4.01317, endpoint.y};
        const auto& witness = selected_directional(canvas, endpoint, first, window.lastError());
        // The intended chain measurement is 2.7675 m. Independent floating
        // subtraction can land below the half-mm while the actual witness
        // length lands above it; assert its readable rounded presentation.
        require(witness.command_text.startsWith(QStringLiteral("→")) &&
            witness.dimension_text == (first_side ? QStringLiteral("4 m") : QStringLiteral("≈ 2.768 m")),
            "native directional cue includes the arrow and an honest readable segment measurement");
        const auto painted = canvas.grab().toImage();
        const auto position = screen(canvas, first)*painted.devicePixelRatio();
        const QRect marker(qRound(position.x())-10, qRound(position.y())-10, 21, 21);
        require(!painted.isNull() && passive.copy(marker) != painted.copy(marker),
            "selected native directional endpoint visibly renders in light and dark themes");
        const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture_directory.isEmpty()) {
            const auto name = QStringLiteral("directional-%1-%2-%3-%4.png")
                .arg(boundary ? "boundary" : "wall", manual ? "manual" : "auto",
                    first_side ? "first" : "chain", theme == WorkspaceTheme::light ? "light" : "dark");
            require(QDir().mkpath(capture_directory) && painted.save(QDir(capture_directory).filePath(name)),
                "native directional screenshot saves for review");
        }
        // Moving the pointer removes the proposal before the next theme pass.
        mouse(canvas, QEvent::MouseMove, endpoint);
    }
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, endpoint, {4.01317, endpoint.y}, window.lastError());
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, endpoint, {6.24567, endpoint.y}, window.lastError());
    require(window.document().snapshot().entities() == before.entities(),
        "repeated proposals remain ephemeral until Enter");
    key(canvas, Qt::Key_Return);
    if (boundary) {
        const auto& accepted = *canvas.boundaryDraftPreview();
        require(accepted.segments.size() == original_count+1 && same(accepted.segments.back().start, endpoint) &&
            same(accepted.segments.back().end, {6.24567, endpoint.y}) &&
            window.document().snapshot().entities() == before.entities(),
            "native boundary Enter appends one exact proposed draft side without publishing the draft");
        if (manual) {
            require(accepted.instruction.contains("place this edge"),
                "directional acceptance retains Define First dimension placement phase");
            key(canvas, Qt::Key_Right, Qt::ControlModifier);
            require(canvas.boundaryDraftPreview()->segments.size() == original_count+1 &&
                std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
                    [](const auto& value) { return value.selected; }),
                "directional repeat during manual placement cannot append or propose another side");
            click(canvas, {4, endpoint.y-0.5});
            mouse(canvas, QEvent::MouseMove, {6.24567, endpoint.y});
        }
    } else require(canvas.wallPreview() && same(canvas.wallPreview()->start, {6.24567, endpoint.y}),
        "native wall Enter advances actual construction start to the accepted endpoint");
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, {6.24567, endpoint.y}, {8.01317, endpoint.y}, window.lastError());
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, {6.24567, endpoint.y}, {9.24567, endpoint.y}, window.lastError());
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, {6.24567, endpoint.y}, {9.24567, endpoint.y}, window.lastError());
    const auto before_change = window.document().snapshot();
    auto source = before_change.entities().at(target_wall.toStdString());
    source.properties["baseline"]["end"][0] = 6.34567;
    window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(source)}, {},
        "Change directional alignment target source"});
    const auto changed = window.document().snapshot();
    key(canvas, Qt::Key_Return);
    require(window.document().snapshot().entities() == changed.entities() &&
        window.document().snapshot().revision() == changed.revision() &&
        std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
            [](const auto& value) { return value.selected; }) && !window.lastError().isEmpty(),
        "changed source invalidates the directional proposal before exact acceptance");
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    require(window.document().snapshot().entities() == changed.entities() &&
        std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
            [](const auto& value) { return value.selected; }) && !window.lastError().isEmpty(),
        "changed source with stale retained projection cannot create a new directional proposal");
}
void native_intersection_shortcut_requires_an_actual_crossing() {
    QTemporaryDir directory;
    require(directory.isValid(), "native ray fixture uses isolated storage");
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    auto& canvas = native_canvas(window);
    require(!window.createStraightWall({1, 3}, {1, 4}).isEmpty(), "native off-ray endpoint wall creates");
    require(!window.createStraightWall({4, -2}, {4, 2}).isEmpty(), "native exact crossing wall creates");
    (void)window.selectEntity({});
    click(canvas, {-1, -1});
    click(canvas, {0, 0});
    mouse(canvas, QEvent::MouseMove, {0, 0});
    const auto before = window.document().snapshot();
    key(canvas, Qt::Key_Left, Qt::ControlModifier | Qt::ShiftModifier);
    require(window.document().snapshot().entities() == before.entities() &&
        std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
            [](const auto& value) { return value.selected; }),
        "Ctrl+Shift+Left leaves native cursor proposal unchanged with no intersecting line");
    key(canvas, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
    selected_directional(canvas, {0, 0}, {4, 0}, window.lastError());
    const auto proposal_source = window.document().snapshot();
    key(canvas, Qt::Key_Return);
    const auto accepted = window.document().snapshot();
    std::optional<Segment> appended;
    for (const auto& [id, entity] : accepted.entities()) {
        if (entity.type == "wall" && !proposal_source.entities().contains(id)) {
            require(!appended, "intersection acceptance appends exactly one wall");
            appended = baseline(entity);
        }
    }
    if (!appended || !same(appended->start, {0, 0}) || !same(appended->end, {4, 0})) {
        std::cerr << "native intersection acceptance: revision " << proposal_source.revision() << " -> "
            << accepted.revision() << ", last error: " << window.lastError().toStdString()
            << ", appended: " << appended.has_value();
        if (appended) std::cerr << " start(" << appended->start.x << ',' << appended->start.y << ") end("
            << appended->end.x << ',' << appended->end.y << ')';
        if (canvas.wallPreview()) std::cerr << ", wall pen(" << canvas.wallPreview()->start.x << ','
            << canvas.wallPreview()->start.y << ')';
        std::cerr << '\n';
    }
    require(appended && same(appended->start, {0, 0}) && same(appended->end, {4, 0}),
        "native Ctrl+Shift+Right accepts the actual crossing rather than the nearer off-ray endpoint");
}
void recovered_directional_cursor_keeps_its_transverse_coordinate() {
    QTemporaryDir directory;
    require(directory.isValid(), "recovered directional fixture isolates archive and library storage");
    MainWindow seed({}, nullptr, directory.filePath("seed-library.json"));
    require(!seed.createStraightWall({2, 3}, {3, 3}).isEmpty(), "recovered fixture has a structural target X");
    ProjectWorkspace workspace(seed.document().snapshot());
    BoundaryAuthoringOptions options;
    options.geometry_tolerance_metres = 0.01;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
    session.set_classification("measurement");
    (void)session.anchor({0, 0});
    (void)session.add_line_to({1, 0});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(),
        {"property-1", "building-1", "floor-1", "layer-1"}), session.recovery_checkpoint()};
    auto checkpoint = workspace.prepare_boundary_checkpoint(active);
    (void)workspace.commit(checkpoint);
    const RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(
        workspace.snapshot(), capture_workspace_history_record(workspace.capture()), active)},
        {"active", "boundary_active", encode_boundary_active_recovery(active)}};
    const auto path = directory.filePath("directional-tolerance.bldproj");
    (void)ProjectStore::save_archive(std::filesystem::path(path.toStdWString()),
        ProjectArchiveSnapshot(workspace.snapshot(), ledger, ArchiveRole::ordinary));
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    auto& canvas = native_canvas(window);
    require(window.openProject(path) && canvas.boundaryDraftPreview() &&
        canvas.boundaryDraftPreview()->segments.size() == 1,
        "native reopen restores the authoring pen at the recovered endpoint");
    (void)native_canvas(window);
    mouse(canvas, QEvent::MouseMove, {0.999, 0.005});
    const auto before = window.document().snapshot();
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, {1, 0}, {2, 0.005}, window.lastError());
    key(canvas, Qt::Key_Return);
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size() == 2 &&
        same(canvas.boundaryDraftPreview()->segments.back().start, {1, 0}) &&
        same(canvas.boundaryDraftPreview()->segments.back().end, {2, 0.005}) &&
        window.document().snapshot().entities() == before.entities(),
        "near-pen directional acceptance preserves transverse cursor and retained construction start");
}
void separated_native_cursor_acceptance_survives_undo_and_reopen() {
    QTemporaryDir directory;
    require(directory.isValid(), "separated cursor fixture isolates archive and library storage");
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    auto& canvas = native_canvas(window);
    require(!window.createStraightWall({4, 3}, {6, 3}).isEmpty(), "separated cursor fixture has target X");
    (void)window.selectEntity({});
    select_native_wall_drawing(window, canvas);
    click(canvas, {0, 0});
    require(canvas.wallPreview() && same(canvas.wallPreview()->start, {0, 0}) &&
        !canvas.boundaryDraftPreview(), "separated cursor fixture records the physical wall pen before proposing");
    mouse(canvas, QEvent::MouseMove, {2, 1});
    const auto before = window.document().snapshot();
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, {0, 0}, {4, 1}, window.lastError());
    require(window.document().snapshot().entities() == before.entities(), "separated cursor proposal is ephemeral");
    key(canvas, Qt::Key_Return);
    const auto accepted = window.document().snapshot();
    std::string appended_id;
    for (const auto& [id, entity] : accepted.entities()) {
        if (entity.type == "wall" && !before.entities().contains(id)) {
            require(appended_id.empty() && same(baseline(entity).start, {0, 0}) &&
                same(baseline(entity).end, {4, 1}),
                "separated pointer appends one wall from retained pen to cursor-aligned endpoint");
            appended_id = id;
        }
    }
    require(!appended_id.empty(), "separated cursor Enter creates its proposed wall");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
        "directional wall acceptance is one undoable document command");
    require(window.redoCommand() && window.document().snapshot().entities() == accepted.entities(),
        "directional wall redo restores accepted geometry and receipt");
    const auto path = directory.filePath("directional-wall.bldproj");
    require(window.saveProjectAs(path), "accepted directional wall saves through native host");
    MainWindow restored({}, nullptr, directory.filePath("restored-library.json"));
    require(restored.openProject(path) &&
        restored.document().snapshot().entities().at(appended_id) == accepted.entities().at(appended_id),
        "native reopen preserves directional wall baseline and construction receipt exactly");
}
void native_directional_wall_closure_returns_to_anchor() {
    QTemporaryDir directory;
    require(directory.isValid(), "directional closure fixture isolates library storage");
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    auto& canvas = native_canvas(window);
    select_native_wall_drawing(window, canvas);
    for (const auto point : {Vec2{0, 0}, Vec2{2, 0}, Vec2{2, 2}, Vec2{0, 2}}) click(canvas, point);
    mouse(canvas, QEvent::MouseMove, {0, 2});
    const auto before = window.document().snapshot();
    key(canvas, Qt::Key_Down, Qt::ControlModifier);
    selected_directional(canvas, {0, 2}, {0, 0}, window.lastError());
    key(canvas, Qt::Key_Return);
    const auto accepted = window.document().snapshot();
    std::size_t closing_walls = 0;
    for (const auto& [id, entity] : accepted.entities()) {
        if (entity.type == "wall" && !before.entities().contains(id)) {
            require(same(baseline(entity).start, {0, 2}) && same(baseline(entity).end, {0, 0}),
                "directional wall closure persists the exact closing side");
            ++closing_walls;
        }
    }
    require(closing_walls == 1 && !canvas.wallPreview() && window.lastError().isEmpty(),
        "directional acceptance of a valid closing side completes the native wall chain");
}
void native_visible_other_floor_wall_is_a_directional_target() {
    QTemporaryDir directory;
    require(directory.isValid(), "two floor directional fixture isolates library storage");
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    auto& canvas = native_canvas(window);
    require(window.setActiveLayer("layer-1"), "two floor fixture starts on the original floor");
    const auto wall_id = window.createStraightWall({4, -2}, {4, 2});
    const auto other_floor = window.createFloor("building-1", "Directional authoring floor");
    require(!wall_id.isEmpty() && !other_floor.isEmpty(), "two floor fixture creates its wall and second floor");
    const auto other_layer = window.createLayer(other_floor, "Directional authoring layer");
    require(!other_layer.isEmpty() && window.setActiveLayer(other_layer) && window.entityVisible(wall_id),
        "wall on the original floor stays visible from the second floor's active layer");
    const auto retained = std::find_if(canvas.entities().begin(), canvas.entities().end(),
        [&](const auto& entity) { return entity.id == wall_id; });
    require(retained != canvas.entities().end() && retained->snap_points.empty() &&
        retained->snap_segments.empty() && retained->drawing_alignment_segments.size() == 1 &&
        same(retained->drawing_alignment_segments.front().start, {4, -2}) &&
        same(retained->drawing_alignment_segments.front().end, {4, 2}),
        "visible other floor wall exposes exact directional baseline without ordinary snap candidates");
    (void)window.selectEntity({});
    select_native_wall_drawing(window, canvas);
    click(canvas, {0, 0});
    require(canvas.wallPreview() && same(canvas.wallPreview()->start, {0, 0}),
        "two floor directional fixture records a wall pen on the active floor");
    const auto before = window.document().snapshot();
    mouse(canvas, QEvent::MouseMove, {0, 0});
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    selected_directional(canvas, {0, 0}, {4, 0}, window.lastError());
    mouse(canvas, QEvent::MouseMove, {0, 0});
    key(canvas, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
    selected_directional(canvas, {0, 0}, {4, 0}, window.lastError());
    require(window.setContainerVisible("floor-1", false) && !window.entityVisible(wall_id),
        "hiding the source floor removes the directional target from the visible scene");
    require(std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
        [](const auto& value) { return value.selected; }),
        "source floor visibility change invalidates the selected directional proposal");
    for (const bool intersections : {false, true})
        target(canvas, {0, 0}, 1, 0, intersections, {}, "hidden floor wall is not a directional candidate");
    mouse(canvas, QEvent::MouseMove, {0, 0});
    key(canvas, Qt::Key_Right, Qt::ControlModifier);
    key(canvas, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
    require(window.document().snapshot().entities() == before.entities() &&
        canvas.wallPreview() && same(canvas.wallPreview()->start, {0, 0}) &&
        std::none_of(canvas.drawingWitnesses().begin(), canvas.drawingWitnesses().end(),
            [](const auto& value) { return value.selected; }),
        "both native directional shortcuts preserve the pen and model after the target floor is hidden");
}
void ctrl_arrow_proposes_exact_native_wall_endpoint() {
    QTemporaryDir directory;
    require(directory.isValid(), "directional fixture has isolated project storage");
    MainWindow window({}, nullptr, directory.filePath("library.json"));
    window.setMetricUnits(true);
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900);
    window.show();
    QApplication::setActiveWindow(&window);
    events();
    auto* snap = window.findChild<QToolButton*>("snapTool");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(snap && canvas, "directional fixture uses actual measurement canvas and snap control");
    snap->setChecked(false);
    canvas->setSnapEnabled(false);
    canvas->setOverviewMapEnabled(false);
    canvas->setViewTransform({2, 1}, 65);
    // The existing wall is above the ray; Ctrl alone aligns its endpoint X.
    require(!window.createStraightWall({4.01317, 4.11931}, {6.24567, 4.11931}).isEmpty(),
        "independent structural endpoint target is visible");
    (void)window.selectEntity({});
    canvas->setFocus();
    events();
    const Vec2 anchor{0.01317, -0.01931}, endpoint{1.24567, 2.34567};
    click(*canvas, anchor);
    click(*canvas, endpoint);
    require(canvas->wallPreview() && same(canvas->wallPreview()->start, endpoint),
        "native wall chain starts from the literal off-grid endpoint");
    mouse(*canvas, QEvent::MouseMove, endpoint);
    const auto before = window.document().snapshot();
    key(*canvas, Qt::Key_Right, Qt::ControlModifier);
    require(window.document().snapshot().entities() == before.entities(),
        "directional proposal leaves the committed model unchanged");
    key(*canvas, Qt::Key_Return);
    const auto after = window.document().snapshot();
    std::optional<Segment> appended;
    for (const auto& [id, entity] : after.entities()) {
        if (entity.type == "wall" && !before.entities().contains(id)) {
            require(!appended, "directional acceptance adds exactly one wall");
            appended = baseline(entity);
        }
    }
    require(appended && same(appended->start, endpoint) &&
        same(appended->end, {4.01317, 2.34567}),
        "Ctrl+Right then Enter appends the nearest structural endpoint X and preserves exact Y");
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Vertex-tests");
    QCoreApplication::setApplicationName("Vertex-directional-test-" + QUuid::createUuid().toString());
    try {
        require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf") >= 0,
            "native directional rendering loads the bundled font");
        app.setFont(QFont("Inter", 10));
        endpoint_coordinates_advance_exactly_without_artwork_or_grid_targets();
        independent_measurement_strokes_are_analytical_alignment_targets();
        native_measured_lines_accept_alignment_and_automatic_closure();
        intersections_use_actual_ray_contacts_and_accepted_draft_geometry();
        directional_wall_baselines_are_independent_of_mouse_snap_geometry();
        distant_origins_order_targets_by_exact_world_coordinate();
        long_empty_rays_preserve_near_tangent_arc_contacts();
        analytical_arcs_keep_sweeps_tangencies_and_endpoint_contacts();
        shallow_arc_intersection_keeps_the_first_actual_contact();
        malformed_geometry_and_non_cardinal_directions_fail_closed();
        directional_dispatch_requires_focused_idle_drawing_context();
        travel_dispatch_preserves_native_gesture_arbitration();
        ctrl_arrow_proposes_exact_native_wall_endpoint();
        native_intersection_shortcut_requires_an_actual_crossing();
        recovered_directional_cursor_keeps_its_transverse_coordinate();
        separated_native_cursor_acceptance_survives_undo_and_reopen();
        native_directional_wall_closure_returns_to_anchor();
        native_visible_other_floor_wall_is_a_directional_target();
        for (const bool first_side : {false, true}) {
            repeated_native_proposals_and_boundary_phases(false, false, first_side);
            repeated_native_proposals_and_boundary_phases(true, false, first_side);
            repeated_native_proposals_and_boundary_phases(true, true, first_side);
        }
        std::cout << "directional_alignment_canvas_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "directional_alignment_canvas_tests: " << error.what() << '\n';
        return 1;
    }
}
