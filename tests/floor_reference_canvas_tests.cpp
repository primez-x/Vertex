#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void configure(PlanCanvas& canvas) {
    canvas.resize(1200, 800);
    canvas.setCanvasBackground(Qt::white);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setSelectionControlsVisible(false);
    canvas.setViewTransform({0, 0}, 70);
}
CanvasEntity line(QString id, Vec2 start, Vec2 end) {
    CanvasEntity value;
    value.id = std::move(id); value.type = "annotation_line";
    value.segments = {{start, end, 0}};
    value.stroke_color = QColor(210, 20, 40); value.stroke_width_metres = .12;
    return value;
}
std::vector<CanvasEntity> source_entities() {
    auto dimension = line("same-id", {-2, 1}, {0, 1});
    dimension.type = "dimension_line"; dimension.dimension_end_ticks = true;
    dimension.selected = true;
    auto curve = line("curve", {-2, -.5}, {0, -.5});
    curve.segments.front().sweep_radians = std::numbers::pi;
    CanvasEntity area;
    area.id = "area"; area.type = "boundary"; area.filled = true; area.hatch_pattern = "solid";
    area.fill_color = QColor(20, 180, 80); area.stroke_color = Qt::darkGreen;
    area.segments = {{{1, -1}, {3, -1}}, {{3, -1}, {3, 1}},
                     {{3, 1}, {1, 1}}, {{1, 1}, {1, -1}}};
    area.holes = {{{{1.5, -.5}, {2.5, -.5}}, {{2.5, -.5}, {2.5, .5}},
                   {{2.5, .5}, {1.5, .5}}, {{1.5, .5}, {1.5, -.5}}}};
    area.stroke_segments = Boundary{{{1, -1}, {3, -1}}, {{3, -1}, {3, 1}}};
    CanvasEntity svg;
    svg.id = "svg"; svg.type = "symbol";
    CanvasSvgSymbol artwork;
    artwork.document = QByteArrayLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 60'>"
        "<path d='M 5 50 C 10 5 90 5 95 50' fill='none' stroke='#a010d0' stroke-width='8'/>"
        "<rect x='35' y='25' width='30' height='20' fill='#10a0d0'/></svg>");
    artwork.artwork_sha256 = QCryptographicHash::hash(artwork.document, QCryptographicHash::Sha256).toHex();
    artwork.view_box = {0, 0, 100, 60}; artwork.footprint_view_box = artwork.view_box;
    artwork.position = {1.5, 2.5}; artwork.width_metres = 1.8; artwork.depth_metres = 1.1;
    artwork.rotation_radians = -.45;
    svg.svg_symbol = artwork;
    return {dimension, curve, area, svg};
}
std::vector<CanvasLabel> source_labels() {
    CanvasLabel text;
    text.id = "same-id"; text.position = {-1, 2.5}; text.text = "Source floor";
    text.color = QColor(20, 40, 210); text.show_background = false;
    text.text_height_metres = .25; text.rotation_radians = .3; text.selected = true;
    return {text};
}
QImage screen_image(PlanCanvas& canvas) {
    QImage result(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    canvas.render(&result); // Exercises paintEvent, not the public output renderer.
    return result;
}
QImage output_image(const PlanCanvas& canvas, int mode) {
    QImage result(1200, 800, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    if (mode == 0) canvas.renderScene(painter, result.rect());
    else if (mode == 1) canvas.renderScene(painter, result.rect(), true, Qt::white);
    else canvas.renderSceneAt(painter, result.rect(), 70, {0, 0}, Qt::white);
    require(painter.end(), "output painter closes");
    return result;
}
QByteArray digest(const QImage& image) {
    return QCryptographicHash::hash(QByteArrayView(
        reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
        QCryptographicHash::Sha256);
}
QImage replay(CanvasSketchContentRecording value) {
    const auto bounds = value.ink_bounds.toAlignedRect();
    QImage image(bounds.size() + QSize(32, 32), QImage::Format_ARGB32_Premultiplied);
    image.setDotsPerMeterX(qRound(value.picture.logicalDpiX() / .0254));
    image.setDotsPerMeterY(qRound(value.picture.logicalDpiY() / .0254));
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.translate(16 - bounds.left(), 16 - bounds.top());
    require(value.picture.play(&painter) && painter.end(), "recording replays");
    return image;
}
QPointF screen(const PlanCanvas& canvas, Vec2 point) {
    return QRectF(canvas.rect()).center() + QPointF(
        (point.x - canvas.viewCenter().x) * canvas.viewScale(),
        -(point.y - canvas.viewCenter().y) * canvas.viewScale());
}
int changed_pixels(const QImage& a, const QImage& b, QRect region) {
    int count = 0;
    region = region.intersected(a.rect());
    for (int y = region.top(); y <= region.bottom(); ++y)
        for (int x = region.left(); x <= region.right(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) ++count;
    return count;
}
void screen_geometry_and_refresh() {
    PlanCanvas canvas;
    configure(canvas);
    const auto blank = screen_image(canvas);
    const Vec2 offset{-2, -1};
    auto entities = source_entities(); auto labels = source_labels();
    canvas.setFloorGhost(entities, .5, offset, labels);
    const auto ghost = screen_image(canvas);
    require(digest(ghost) != digest(blank), "paintEvent includes the floor reference");
    require(canvas.entities().empty() && canvas.labels().empty(), "ghost remains outside committed content");
    require(!canvas.recordSketchContent(), "reference-only scene has no committed output content");
    require(entities.front().selected && labels.front().selected &&
            entities.front().segments.front().start.x == -2,
            "retained source geometry and selection are not mutated");

    // Independent known regions exercise line, analytical arc, holes, SVG rotation,
    // and text at a negative world offset. A missing primitive cannot pass via another.
    for (const auto [point, minimum] : {
            std::pair{Vec2{-3, 0}, 30}, std::pair{Vec2{-3, -2.5}, 20},
            std::pair{Vec2{-.8, -1}, 50}, std::pair{Vec2{-.5, 1.5}, 50},
            std::pair{Vec2{-3, 1.5}, 30}}) {
        const auto p = screen(canvas, point).toPoint();
        require(changed_pixels(blank, ghost, QRect(p - QPoint(45, 45), QSize(90, 90))) > minimum,
                "translated source primitive contributes screen ink");
    }
    require(ghost.pixelColor(screen(canvas, {0, -1}).toPoint()) == QColor(Qt::white),
            "reference boundary hole stays transparent");
    const auto fill = ghost.pixelColor(screen(canvas, {-.8, -1}).toPoint());
    require(fill.green() > fill.red(), "source boundary fill retains its authored green color");
    require(ghost.pixelColor(screen(canvas, {-1, -1}).toPoint()).red() > 220 &&
            ghost.pixelColor(screen(canvas, {0, -2}).toPoint()).red() < 180,
            "derived stroke path omits source internal seams while retaining the drawable edge");
    const auto colored = ghost.pixelColor(screen(canvas, {-3, 0}).toPoint());
    require(colored.red() > colored.green() && colored.green() > 100,
            "opacity fades the authored red line without a selected blue override");
    auto active = line("active", {-4, 0}, {-2, 0}); active.stroke_color = Qt::black;
    canvas.setEntities({active});
    require(screen_image(canvas).pixelColor(screen(canvas, {-3, 0}).toPoint()) == QColor(Qt::black),
            "committed ink paints in front of the floor reference");
    canvas.setEntities({});

    canvas.setFloorGhost(entities, .75, offset, labels);
    const auto stronger = screen_image(canvas);
    require(stronger.pixelColor(screen(canvas, {-3, 0}).toPoint()).green() < colored.green(),
            "opacity updates repaint the source artwork");
    canvas.setFloorGhost(entities, 99, offset, labels);
    require(digest(screen_image(canvas)) == digest(stronger), "high opacity is limited to readable reference intensity");
    canvas.setFloorGhost(entities, .05, offset, labels);
    const auto faint = screen_image(canvas);
    require(digest(faint) != digest(stronger), "low opacity updates the reference");
    canvas.setFloorGhost(entities, -99, offset, labels);
    require(digest(screen_image(canvas)) == digest(faint), "low opacity remains a visible reference");
    canvas.setViewTransform({-1, .5}, 110);
    require(digest(screen_image(canvas)) != digest(ghost), "reference follows pan and zoom");
    canvas.setViewTransform({0, 0}, 70);
    entities.front().segments.front().end = {-1, 1};
    labels.front().text = "Updated source";
    canvas.setFloorGhost(entities, .5, offset, labels);
    require(digest(screen_image(canvas)) != digest(ghost), "live source geometry and labels replace the projection");
    canvas.setFloorGhost(entities, .5, {1, 1}, labels);
    require(digest(screen_image(canvas)) != digest(ghost), "alignment offset updates screen projection");
    canvas.clearFloorGhost();
    require(digest(screen_image(canvas)) == digest(blank), "clearing the reference restores the screen");
    canvas.setFloorGhost(entities, .5, {std::numeric_limits<double>::quiet_NaN(), 0}, labels);
    require(digest(screen_image(canvas)) == digest(blank), "invalid offset does not paint corrupt geometry");
}
void outputs_bounds_and_guide_are_unchanged() {
    PlanCanvas canvas;
    configure(canvas);
    canvas.setEntities({line("committed", {3, -3}, {5, -3})});
    CanvasLabel label; label.text = "Committed"; label.position = {4, -2.5}; label.show_background = false;
    canvas.setLabels({label});
    canvas.setSketchCompositionGuideEnabled(true);
    const auto guide = canvas.sketchCompositionGuideRect();
    const auto recording = canvas.recordSketchContent();
    require(guide && recording, "committed scene has a composition guide and recording");
    const auto ink = digest(replay(*recording));
    std::vector<QByteArray> outputs;
    for (int mode = 0; mode != 3; ++mode) outputs.push_back(digest(output_image(canvas, mode)));
    canvas.fitView(); const auto fit_center = canvas.viewCenter(); const auto fit_scale = canvas.viewScale();
    canvas.setViewTransform({0, 0}, 70);
    canvas.setFloorGhost(source_entities(), .5, {-2, -1}, source_labels());
    for (int mode = 0; mode != 3; ++mode)
        require(digest(output_image(canvas, mode)) == outputs[mode], "every public output excludes the ghost");
    const auto with_ghost = canvas.recordSketchContent();
    require(with_ghost && with_ghost->ink_bounds == recording->ink_bounds && digest(replay(*with_ghost)) == ink,
            "tight recording pixels and crop exclude the ghost");
    require(canvas.sketchCompositionGuideRect() == guide, "ghost update leaves composition guide crop unchanged");
    canvas.fitView();
    require(canvas.viewCenter().x == fit_center.x && canvas.viewCenter().y == fit_center.y &&
            canvas.viewScale() == fit_scale, "fit ignores source floor extent");
    canvas.setViewTransform({0, 0}, 70);
    canvas.setOverviewMapEnabled(true);
    const auto overview = canvas.overviewMapRect().toAlignedRect();
    const auto map_with = screen_image(canvas).copy(overview);
    canvas.clearFloorGhost();
    require(digest(screen_image(canvas).copy(overview)) == digest(map_with), "overview contains only committed content");
}
void ghost_is_not_selectable_or_snappable() {
    PlanCanvas canvas;
    configure(canvas);
    auto entity = line("ghost-only", {-2, 0}, {2, 0});
    entity.type = "wall";
    entity.snap_points = {{-2, 0}, {2, 0}}; entity.snap_segments = entity.segments;
    entity.drawing_alignment_segments = entity.segments;
    canvas.setFloorGhost({entity}, .5, {0, 0});
    QString target = "unset";
    canvas.setRightClicked([&](Vec2, QString id) { target = id; });
    const auto p = screen(canvas, {0, 0});
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, p, canvas.mapToGlobal(p.toPoint()), Qt::RightButton,
            type == QEvent::MouseButtonPress ? Qt::RightButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }
    require(target.isEmpty(), "ghost geometry is absent from context hit testing");
    canvas.setSelectedIds({"ghost-only"});
    require(!canvas.selectionBounds(), "ghost cannot acquire selection controls");
    require(!canvas.directionalDrawingAlignment({-3, 0}, 1, 0, true),
            "ghost geometry is absent from directional alignment");
    canvas.setSnapEnabled(false); canvas.setWallSnapEnabled(true);
    Vec2 picked{}; bool received = false;
    canvas.setPointClicked([&](Vec2 value) { picked = value; received = true; });
    const Vec2 raw{-1.96, .03}; const auto endpoint = screen(canvas, raw);
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, endpoint, canvas.mapToGlobal(endpoint.toPoint()), Qt::LeftButton,
            type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }
    require(received && std::hypot(picked.x - raw.x, picked.y - raw.y) < 1e-10,
            "ordinary input does not snap to ghost source endpoints");
}
void reference_remains_visible_through_room_and_over_underlay() {
    PlanCanvas canvas;
    configure(canvas);
    CanvasEntity room;
    room.id = "room"; room.type = "boundary"; room.filled = true;
    room.fill_color = Qt::white; room.stroke_color = Qt::black; room.hatch_pattern = "solid";
    room.segments = {{{-4, -3}, {4, -3}}, {{4, -3}, {4, 3}},
                     {{4, 3}, {-4, 3}}, {{-4, 3}, {-4, -3}}};
    canvas.setEntities({room});
    CanvasReference underlay;
    underlay.id = "opaque-image"; underlay.image = QImage(800, 600, QImage::Format_RGB32);
    underlay.image.fill(QColor(240, 240, 240));
    underlay.position = {-4, -3}; underlay.metres_per_source_unit = .01;
    canvas.setReferences({underlay});
    const auto without = screen_image(canvas);
    canvas.setFloorGhost({line("source-wall", {-3, 0}, {3, 0})}, .5, {});
    const auto with = screen_image(canvas);
    const auto center = screen(canvas, {0, 0}).toPoint();
    require(changed_pixels(without, with, QRect(center - QPoint(40, 10), QSize(80, 20))) > 100,
            "reference wall remains visible over an opaque underlay inside a filled destination room");
    require(with.pixelColor(center).red() > with.pixelColor(center).green(),
            "reference wall retains distinguishable source color under the active room fill");
    const auto output_with = digest(output_image(canvas, 0));
    canvas.clearFloorGhost();
    require(digest(output_image(canvas, 0)) == output_with,
            "underlay composition change never introduces reference ink into output");
}
}

namespace {
void exact_reference_gesture_payload() {
    PlanCanvas canvas;
    configure(canvas);
    canvas.setSelectionControlsVisible(true);
    canvas.setSnapEnabled(false);
    canvas.setSelectionTransformEnabled(true, true);
    CanvasReference source;
    source.id = "posed-underlay";
    source.image = QImage(160, 90, QImage::Format_ARGB32_Premultiplied);
    source.image.fill(QColor(30, 110, 210));
    { QPainter ink(&source.image); ink.fillRect(3, 7, 37, 21, Qt::yellow); }
    // Asymmetric presented building pose; calibration and flips remain immutable.
    source.position = {1.3, -.7}; source.rotation_degrees = 37.0;
    source.metres_per_source_unit = .013; source.scale = 1.4;
    source.flip_horizontal = true; source.intensity = .63; source.selected = true;
    canvas.setReferences({source});
    canvas.setSelectedId(source.id);
    CanvasReference candidate = source;
    int commits = 0;
    std::uint64_t captured_serial = 0;
    bool complete_move_automatically = false;
    bool refuse_move = false;
    const auto mouse = [&](QEvent::Type type, QPointF point) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    canvas.setEntityTransformPreviewRequested([&](QString id, double scale, double radians, Vec2, std::uint64_t serial)
        -> std::optional<std::vector<CanvasEntity>> {
        require(id == source.id, "reference preview retains source identity");
        captured_serial = serial;
        candidate = source;
        candidate.scale *= scale;
        candidate.rotation_degrees += radians * 180 / std::numbers::pi;
        require(canvas.markEntityTransformPreviewPending(serial), "reference transform marks exact pending");
        require(canvas.completeEntityTransformPreview(serial, std::vector<CanvasEntity>{}, {}, {candidate}),
            "reference-only exact payload completes");
        return std::nullopt;
    });
    canvas.setEntityTransformRequested([&](QString, double, double) {
        ++commits; canvas.setReferences({candidate}); return true;
    });
    canvas.setEntitiesMovePreviewRequested([&](QStringList, Vec2 delta, std::uint64_t serial)
        -> std::optional<std::vector<CanvasEntity>> {
        captured_serial = serial;
        candidate = source; candidate.position = {source.position.x + delta.x, source.position.y + delta.y};
        require(canvas.markEntitiesMovePreviewPending(serial), "reference move marks pending");
        if (complete_move_automatically)
            require(canvas.completeEntitiesMovePreview(serial,
                refuse_move ? std::optional<std::vector<CanvasEntity>>{} : std::vector<CanvasEntity>{},
                {}, refuse_move ? std::vector<CanvasReference>{} : std::vector<CanvasReference>{candidate}),
                "release-position reference preview completes its own serial");
        return std::nullopt;
    });
    canvas.setEntitiesMoveRequested([&](QStringList, Vec2) {
        ++commits; canvas.setReferences({candidate}); return true;
    });
    const auto reference_artwork = [&](PlanCanvas& value) {
        value.setSelectionControlsVisible(false);
        const auto result = digest(screen_image(value));
        value.setSelectionControlsVisible(true);
        return result;
    };
    const auto expected_artwork = [&](const CanvasReference& value) {
        PlanCanvas expected; configure(expected); expected.setReferences({value});
        return digest(screen_image(expected));
    };
    const auto original_screen = digest(screen_image(canvas));
    const auto original_unfitted_output = digest(output_image(canvas, 0));
    const auto original_fitted_output = digest(output_image(canvas, 1));
    const auto original_fixed_output = digest(output_image(canvas, 2));
    const auto center = screen(canvas, source.position);
    mouse(QEvent::MouseButtonPress, center);
    mouse(QEvent::MouseMove, center + QPointF(28, 14));
    const auto stale = captured_serial;
    mouse(QEvent::MouseMove, center + QPointF(70, 35));
    require(!canvas.completeEntitiesMovePreview(stale, std::vector<CanvasEntity>{}, {}, {candidate}),
        "new mouse position rejects stale queued reference move");
    require(canvas.completeEntitiesMovePreview(captured_serial, std::vector<CanvasEntity>{}, {}, {candidate}),
        "exact reference move payload completes");
    require(digest(screen_image(canvas)) != original_screen && canvas.references().front().position.x == source.position.x,
        "exact move paints transient reference without altering retained position");
    require(reference_artwork(canvas) == expected_artwork(candidate), "move exact payload paints the complete calibrated flipped candidate");
    require(digest(output_image(canvas, 0)) == original_unfitted_output &&
            digest(output_image(canvas, 1)) == original_fitted_output &&
            digest(output_image(canvas, 2)) == original_fixed_output,
        "all public output modes exclude transient reference move payload");
    const auto moved = candidate;
    complete_move_automatically = true;
    mouse(QEvent::MouseButtonRelease, center + QPointF(70, 35));
    QApplication::processEvents();
    require(commits == 1 && canvas.references().front().position.x == moved.position.x &&
            canvas.references().front().position.y == moved.position.y,
        "reference move preview and commit agree");
    require(reference_artwork(canvas) == expected_artwork(moved), "move commit renders the same candidate as preview");
    canvas.setReferences({source}); canvas.setSelectedId(source.id); commits = 0;
    complete_move_automatically = false;
    mouse(QEvent::MouseButtonPress, center); mouse(QEvent::MouseMove, center + QPointF(35, 21));
    require(canvas.completeEntitiesMovePreview(captured_serial, std::nullopt), "rejected move completion admitted");
    complete_move_automatically = true; refuse_move = true;
    mouse(QEvent::MouseButtonRelease, center + QPointF(35, 21));
    require(commits == 0 && digest(screen_image(canvas)) == original_screen, "rejected reference move restores source exactly");
    const auto baseline = screen_image(canvas);
    const auto unfitted_output = digest(output_image(canvas, 0));
    const auto output = digest(output_image(canvas, 1));
    const auto explicit_output = digest(output_image(canvas, 2));
    for (bool rotate : {true, false}) {
        const auto frame = canvas.selectionBounds().value();
        const auto angle = -source.rotation_degrees * std::numbers::pi / 180;
        const QPointF local_corner(source.image.width()*source.metres_per_source_unit*source.scale*70/2 + 7.5,
                                   source.image.height()*source.metres_per_source_unit*source.scale*70/2 + 7.5);
        const QPointF corner = screen(canvas,source.position) + QPointF(
            std::cos(angle)*local_corner.x()-std::sin(angle)*local_corner.y(),
            std::sin(angle)*local_corner.x()+std::cos(angle)*local_corner.y());
        const auto start = rotate ? canvas.selectionRotationHandlePosition().value() : corner;
        const auto end = rotate ? frame.center() + QPointF(110, 10)
                                : frame.center() + (start - frame.center()) * 1.3;
        mouse(QEvent::MouseButtonPress, start); mouse(QEvent::MouseMove, end);
        const auto preview = screen_image(canvas);
        require(reference_artwork(canvas) == expected_artwork(candidate), "rotation/scale preview renders exact full candidate artwork");
        require(digest(preview) != digest(baseline), "reference exact transform paints candidate artwork");
        require(canvas.references().front().scale == source.scale &&
                canvas.references().front().rotation_degrees == source.rotation_degrees,
            "preview preserves committed references");
        require(digest(output_image(canvas, 0)) == unfitted_output &&
                digest(output_image(canvas, 1)) == output &&
                digest(output_image(canvas, 2)) == explicit_output,
            "all public output modes exclude transient reference transform payload");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &escape);
        require(!canvas.completeEntityTransformPreview(captured_serial, std::vector<CanvasEntity>{}, {}, {candidate}),
            "cancel rejects queued reference completion");
        mouse(QEvent::MouseButtonRelease, end);
        require(commits == 0 && digest(screen_image(canvas)) == digest(baseline),
            "cancel restores exact posed calibrated flipped reference");
        mouse(QEvent::MouseButtonPress, start); mouse(QEvent::MouseMove, end);
        const auto accepted = candidate;
        mouse(QEvent::MouseButtonRelease, end);
        require(commits == 1 && canvas.references().front().scale == accepted.scale &&
                canvas.references().front().rotation_degrees == accepted.rotation_degrees &&
                canvas.references().front().metres_per_source_unit == source.metres_per_source_unit &&
                canvas.references().front().flip_horizontal == source.flip_horizontal,
            "reference preview and committed transform agree with calibration and flips intact");
        require(reference_artwork(canvas) == expected_artwork(accepted), "rotation/scale commit renders the same exact candidate as preview");
        // Restore source before the next independent real mouse gesture.
        canvas.setReferences({source}); canvas.setSelectedId(source.id); commits = 0;
    }
}

}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled application font loads");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        screen_geometry_and_refresh();
        outputs_bounds_and_guide_are_unchanged();
        ghost_is_not_selectable_or_snappable();
        reference_remains_visible_through_room_and_over_underlay();
        exact_reference_gesture_payload();
        std::cout << "Floor reference canvas tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
