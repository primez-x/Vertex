#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QTransform>

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void configure(PlanCanvas& canvas) {
    canvas.resize(1000, 700);
    canvas.setCanvasBackground(Qt::white);
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);
    // This switch also gates selection interaction, including clicks and
    // exact move previews. Keep it enabled for the paint/pick workflow.
    canvas.setSelectionControlsVisible(true);
    canvas.setSelectionTransformEnabled(false, false);
    canvas.setSelectionAxisResizeEnabled(false);
    canvas.setViewTransform({}, 80);
}
CanvasLabel label(QString role, QString text, Vec2 position, QColor color) {
    CanvasLabel result;
    result.id = "area-owner";
    result.callout_role = std::move(role);
    result.text = std::move(text);
    result.position = position;
    result.color = color;
    result.text_height_metres = .45;
    result.show_background = false;
    return result;
}
QImage render(PlanCanvas& canvas, bool output = false) {
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setFont(canvas.font());
    if (output) canvas.renderSceneAt(painter, image.rect(), 80, {}, Qt::white);
    else canvas.renderScene(painter, image.rect(), false, Qt::white);
    require(painter.end(), "label image painter closes");
    return image;
}
QImage widget_render(PlanCanvas& canvas) {
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    // paintEvent includes the screen-only floor ghost lane. The public scene
    // renderer intentionally retains only ordinary committed presentation.
    canvas.render(&image);
    return image;
}
QRect colored_bounds(const QImage& image, bool red) {
    QRect result;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const auto color = image.pixelColor(x, y);
        if (red ? color.red() > 140 && color.green() < 100 && color.blue() < 100
                : color.blue() > 140 && color.red() < 100 && color.green() < 100)
            result = result.isNull() ? QRect(x, y, 1, 1) : result.united(QRect(x, y, 1, 1));
    }
    return result;
}
void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
void click(PlanCanvas& canvas, QPointF point) {
    mouse(canvas, QEvent::MouseButtonPress, point);
    mouse(canvas, QEvent::MouseButtonRelease, point);
}

void aligned_anchors_paint_and_pick() {
    PlanCanvas canvas;
    configure(canvas);
    auto text = label({}, "Alignment\nshort", {}, QColor(220, 20, 20));
    const auto anchor = QRectF(canvas.rect()).center();
    const auto center_bounds = canvas.labelLayoutBounds(text, 80);
    require(center_bounds == canvasLabelLayoutBounds(text, canvas.font(), &canvas, 80,
                                                     canvas.logicalDpiY()),
            "widget bounds delegate to the captured-value layout API");
    QImage metrics_device(1, 1, QImage::Format_ARGB32_Premultiplied);
    auto worker_text = text;
    worker_text.text_alignment = "left";
    const auto worker_bounds = canvasLabelLayoutBounds(worker_text, canvas.font(), &metrics_device,
                                                      80, metrics_device.logicalDpiY());
    require(worker_bounds.width() > 20 && worker_bounds.left() == -5 &&
            canvasLabelLayoutBounds(worker_text, canvas.font(), nullptr, 80, 96).isEmpty(),
            "captured fonts and worker-local paint devices resolve aligned bounds without widget access");
    require(std::abs(center_bounds.center().x()) < .001, "legacy label remains centered");
    canvas.setLabels({text});
    const auto legacy = render(canvas);
    text.text_alignment = "center";
    canvas.setLabels({text});
    require(render(canvas) == legacy, "explicit center preserves legacy rendered pixels");
    for (const auto alignment : {QStringLiteral("left"), QStringLiteral("right")}) {
        text.text_alignment = alignment;
        text.selected = false;
        canvas.setLabels({text});
        const auto local = canvas.labelLayoutBounds(text, 80);
        require(std::abs(local.width() - center_bounds.width()) < .001 &&
                std::abs((alignment == "left" ? local.left() : local.right()) -
                         (alignment == "left" ? -5.0 : 5.0)) < .001,
                "alignment retains glyph size and anchors the corresponding edge");
        const auto screen = colored_bounds(render(canvas), true);
        const auto output = colored_bounds(render(canvas, true), true);
        require(!screen.isEmpty() && !output.isEmpty() &&
                (alignment == "left" ? screen.left() >= anchor.x() - 3
                                      : screen.right() <= anchor.x() + 3) &&
                (alignment == "left" ? output.left() >= anchor.x() - 3
                                      : output.right() <= anchor.x() + 3),
                "interactive and output glyphs respect the authored text anchor");
        QString picked;
        canvas.setEntityClicked([&](QString id) { picked = std::move(id); });
        click(canvas, screen.center());
        require(picked == text.id, "aligned painted text picks its native owner");
        picked.clear();
        click(canvas, anchor + QPointF(alignment == "left" ? -40 : 40, 0));
        require(picked.isEmpty(), "opposite side of aligned anchor is not a centered phantom hit");
        text.selected = true;
        text.rotation_radians = std::numbers::pi / 4;
        canvas.setLabels({text});
        QTransform transform;
        transform.translate(anchor.x(), anchor.y());
        transform.rotate(-45);
        const auto expected = transform.mapRect(local);
        const auto selection = canvas.selectionBounds();
        require(selection && selection->contains(expected) &&
                std::hypot(selection->center().x() - expected.center().x(),
                           selection->center().y() - expected.center().y()) < .01,
                "rotated selection follows the asymmetric painted label rectangle");
        text.selected = false;
        canvas.setLabels({text});
        picked.clear();
        click(canvas, transform.map(local.center()));
        require(picked == text.id, "rotated aligned label remains pickable at its painted center");
        text.rotation_radians = 0;
    }
}

void distinct_roles_survive_exact_preview() {
    for (const bool introduce_calculation : {false, true}) {
        PlanCanvas canvas;
        configure(canvas);
        auto name = label("area_name", "Room", {-2, 1}, QColor(220, 20, 20));
        auto calculation = label("area_calculation", "10.24 m2", {2, -1}, QColor(20, 20, 220));
        name.selected = calculation.selected = true;
        canvas.setLabels(introduce_calculation ? std::vector<CanvasLabel>{name}
                                              : std::vector<CanvasLabel>{name, calculation});
        const auto committed_output = render(canvas, true);
        std::uint64_t serial = 0;
        canvas.setEntitiesMovePreviewRequested([&](QStringList ids, Vec2, std::uint64_t value)
            -> std::optional<std::vector<CanvasEntity>> {
            require(ids == QStringList{name.id}, "two callout roles retain one selection identity");
            serial = value;
            require(canvas.markEntitiesMovePreviewPending(serial), "role preview becomes pending");
            return std::nullopt;
        });
        canvas.setEntitiesMoveRequested([](QStringList, Vec2) { return false; });
        const auto selected = canvas.selectionBounds();
        require(selected.has_value(), "callout owner has a move frame");
        const auto start = selected->center();
        mouse(canvas, QEvent::MouseButtonPress, start);
        mouse(canvas, QEvent::MouseMove, start + QPointF(80, -40));
        require(serial != 0, "move requests an exact callout preview");
        name.position = {-1, 2};
        calculation.position = {3, -.5};
        calculation.text = "11.50 m2";
        require(canvas.completeEntitiesMovePreview(serial, std::vector<CanvasEntity>{}, {name, calculation}),
                "distinct same-owner label projections are admitted");
        PlanCanvas expected;
        configure(expected);
        expected.setLabels({name, calculation});
        const auto preview = render(canvas);
        const auto expected_image = render(expected);
        require(!colored_bounds(preview, false).isEmpty() &&
                colored_bounds(preview, true) == colored_bounds(expected_image, true) &&
                colored_bounds(preview, false) == colored_bounds(expected_image, false),
                "exact preview preserves each role's own content and anchor, including a new role");
        require(render(canvas, true) == committed_output,
                "live callout previews never replace committed output");
        mouse(canvas, QEvent::MouseButtonRelease, start + QPointF(80, -40));
    }
}

void ghost_and_content_recording_use_alignment() {
    PlanCanvas canvas;
    configure(canvas);
    auto text = label("area_name", "Aligned floor", {}, QColor(220, 20, 20));
    text.text_alignment = "left";
    canvas.setLabels({text});
    const auto left_record = canvas.recordSketchContent(80);
    require(left_record && left_record->ink_bounds.width() > 20,
            "aligned labels retain a drawable tight output recording");
    canvas.setLabels({});
    const auto empty_output = render(canvas, true);
    canvas.setFloorGhost({}, .8, {}, {text});
    const auto ghost = colored_bounds(widget_render(canvas), true);
    require(!ghost.isEmpty() && ghost.left() >= canvas.width() / 2 - 3,
            "floor ghost text uses the same left anchor as ordinary labels");
    require(render(canvas, true) == empty_output && !canvas.recordSketchContent(80),
            "aligned floor ghosts stay outside output and content recordings");
    canvas.clearFloorGhost();
    canvas.setLabels({text});
    const auto left_pixels = colored_bounds(render(canvas), true);
    text.text_alignment = "right";
    canvas.setLabels({text});
    const auto right_pixels = colored_bounds(render(canvas), true);
    const auto right_record = canvas.recordSketchContent(80);
    require(right_pixels.right() < left_pixels.left() + 4,
            "changing only alignment invalidates placement cache and moves the glyph footprint");
    require(right_record && left_record->ink_bounds.center().x() > 0 &&
            right_record->ink_bounds.center().x() < 0,
            "tight output bounds retain the actual aligned ink on either side of its anchor");
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled label font loads");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        aligned_anchors_paint_and_pick();
        distinct_roles_survive_exact_preview();
        ghost_and_content_recording_use_alignment();
        std::cout << "Canvas label presentation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
