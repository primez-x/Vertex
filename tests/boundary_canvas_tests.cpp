#include "../src/desktop/plan_canvas.hpp"

#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QDialog>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using sketch::Boundary;
using sketch::Segment;
using sketch::Vec2;
using sketch::desktop::CanvasLabel;
using sketch::desktop::BoundaryDraftLabel;
using sketch::desktop::BoundaryDraftPreview;
using sketch::desktop::CanvasEntity;
using sketch::desktop::CanvasReferenceGrid;
using sketch::desktop::CanvasTool;
using sketch::desktop::PlanCanvas;

const auto background = QColor(24, 29, 37);

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void process_events() {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
}

QImage render(PlanCanvas& canvas, bool fit_to_content) {
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(background.rgba());
    QPainter painter(&image);
    canvas.renderScene(painter, QRectF(image.rect()), fit_to_content, background);
    return image;
}

bool images_equal(const QImage& left, const QImage& right) {
    if (left.size() != right.size() || left.format() != right.format() ||
        left.sizeInBytes() != right.sizeInBytes()) {
        return false;
    }
    return std::equal(left.constBits(), left.constBits() + left.sizeInBytes(), right.constBits());
}

int differing_pixels(const QImage& left, const QImage& right, QRect region) {
    if (left.size() != right.size()) return 0;
    region = region.intersected(left.rect());
    region = region.intersected(right.rect());
    int result = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (left.pixel(x, y) != right.pixel(x, y)) ++result;
        }
    }
    return result;
}

QRect bright_pixel_bounds(const QImage& image, QRect region,
                         const QImage* without_label = nullptr) {
    region = region.intersected(image.rect());
    int left = region.right() + 1;
    int top = region.bottom() + 1;
    int right = region.left() - 1;
    int bottom = region.top() - 1;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (without_label && image.pixel(x, y) == without_label->pixel(x, y)) continue;
            const auto color = image.pixelColor(x, y);
            if (color.red() > 180 && color.green() > 150 && color.blue() > 80 &&
                color.red() > color.blue() + 45) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    return left <= right && top <= bottom ? QRect(QPoint(left, top), QPoint(right, bottom))
                                          : QRect{};
}

void save_capture(const QString& directory, const QString& filename, const QImage& image) {
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "boundary canvas capture directory must be writable");
    require(image.save(QDir(directory).filePath(filename), "PNG"),
            "boundary canvas capture must be written");
}

QPointF fit_screen_point(const PlanCanvas& canvas, Vec2 point) {
    // This mirrors fitView() for the seeded [-2,2] square used below. It lets
    // the image oracle inspect the arc and label separately without exposing
    // the canvas's view transform as public state.
    constexpr double width = 4.0;
    constexpr double height = 4.0;
    constexpr double padding = 4.0 * 0.12 + 0.25;
    const auto scale = std::min(static_cast<double>(canvas.width()) /
                                    (width + padding * 2.0),
                                static_cast<double>(canvas.height()) /
                                    (height + padding * 2.0));
    const auto viewport = QRectF(canvas.rect());
    return {viewport.center().x() + point.x * scale,
            viewport.center().y() - point.y * scale};
}

void send_key(PlanCanvas& canvas, int key, Qt::KeyboardModifiers modifiers) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(&canvas, &release);
}

void test_boundary_draft_rendering_and_history() {
    PlanCanvas canvas;
    const auto expected_dpr_text = qEnvironmentVariable("SKETCH_BOUNDARY_CANVAS_EXPECTED_DPR");
    if (!expected_dpr_text.isEmpty()) {
        bool parsed = false;
        const auto expected_dpr = expected_dpr_text.toDouble(&parsed);
        require(parsed && std::isfinite(expected_dpr) && expected_dpr > 0.0,
                "expected display scale must be finite and positive");
        require(std::abs(canvas.devicePixelRatioF() - expected_dpr) < 0.001,
                "effective canvas display scale must match the requested capture scale");
    }
    std::cout << "Effective canvas DPR: " << canvas.devicePixelRatioF() << '\n';
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setEntities({CanvasEntity{
        QStringLiteral("base-rectangle"),
        QStringLiteral("measurement_boundary"),
        Boundary{
            Segment{{-2.0, -2.0}, {2.0, -2.0}, 0.0},
            Segment{{2.0, -2.0}, {2.0, 2.0}, 0.0},
            Segment{{2.0, 2.0}, {-2.0, 2.0}, 0.0},
            Segment{{-2.0, 2.0}, {-2.0, -2.0}, 0.0},
        },
        0.08,
        false,
    }});
    canvas.show();
    process_events();
    canvas.fitView();
    canvas.setTool(CanvasTool::boundary);
    canvas.clearPreview();

    const auto output_clean = render(canvas, true);
    const auto screen_clean = render(canvas, false);
    canvas.setGridEnabled(true);
    canvas.setSelectedId(QStringLiteral("base-rectangle"));
    require(images_equal(output_clean, render(canvas, true)),
            "printed geometry must ignore editing grid and selection styling");
    require(!images_equal(screen_clean, render(canvas, false)),
            "editing grid and selection must still be visible onscreen");
    canvas.setGridEnabled(false);
    canvas.setSelectedId({});
    std::vector<CanvasLabel> committed_labels{
        CanvasLabel{QStringLiteral("dimension-outside"), {8.0, 5.0},
                    QStringLiteral("Committed 7.25 m"), false},
    };
    canvas.setLabels(committed_labels);
    require(canvas.labels().size() == 1 &&
                canvas.labels().front().id == QStringLiteral("dimension-outside") &&
                canvas.labels().front().text == QStringLiteral("Committed 7.25 m"),
            "setLabels must retain the committed annotation value");
    committed_labels.front().text = QStringLiteral("mutated caller label");
    require(canvas.labels().front().text == QStringLiteral("Committed 7.25 m"),
            "setLabels must retain an owned committed-label copy");
    const auto output_with_label = render(canvas, true);
    auto selected_labels = canvas.labels();
    selected_labels.front().selected = true;
    canvas.setLabels(std::move(selected_labels));
    require(images_equal(output_with_label, render(canvas, true)),
            "printed dimensions must ignore selection highlighting");
    canvas.setLabels({CanvasLabel{QStringLiteral("dimension-outside"), {8.0, 5.0},
                                  QStringLiteral("Committed 7.25 m"), false}});
    // The committed label is deliberately outside the seeded rectangle. Its
    // bright text must still appear in the fit-to-content output, and the
    // label position must participate in the output transform.
    const auto outside_label_region = QRect(470, 10, 170, 140);
    require(differing_pixels(output_clean, output_with_label, outside_label_region) > 20,
            "fit-to-content output must render committed labels outside geometry");
    require(!bright_pixel_bounds(output_with_label, outside_label_region).isEmpty(),
            "committed output label must contain readable bright text pixels");
    canvas.setLabels({});
    require(canvas.labels().empty(), "setLabels(empty) must clear committed annotations");
    require(images_equal(output_clean, render(canvas, true)),
            "clearing committed labels must restore the geometry-only output");
    canvas.setLabels({CanvasLabel{QStringLiteral("dimension-outside"), {8.0, 5.0},
                                  QStringLiteral("Committed 7.25 m"), false}});
    require(images_equal(output_with_label, render(canvas, true)),
            "replacing committed labels with the same value must be deterministic");

    // Keep the legacy overlays populated too: the output oracle must cover
    // both old and new transient paths.
    canvas.setBoundaryPreview({{-1.5, -1.25}, {-1.0, 0.0}});
    canvas.setWallPreview(std::make_pair(Vec2{0.0, -1.0}, Vec2{0.0, 1.0}));
    const auto output_before = render(canvas, true);
    require(images_equal(output_with_label, output_before),
            "fit-to-content output must ignore legacy transient overlays");
    const auto screen_before = render(canvas, false);

    BoundaryDraftPreview draft;
    draft.segments = Boundary{
        Segment{{-1.0, 0.0}, {1.0, 0.0}, -std::numbers::pi},
    };
    draft.labels = {BoundaryDraftLabel{{0.0, 1.5}, QStringLiteral("2.00 m")}};
    draft.anchor = Vec2{-1.5, -1.25};
    draft.pen_position = Vec2{1.0, 0.0};
    draft.rubber_band = Segment{{1.0, 0.0}, {1.75, 0.75}, 0.0};
    draft.instruction = QStringLiteral("Draft boundary  •  place the next dimension");

    canvas.setBoundaryDraftPreview(draft);
    process_events();
    require(canvas.boundaryDraftPreview().has_value(),
            "boundary draft preview getter must expose the stored value");
    require(canvas.boundaryDraftPreview()->labels.size() == 1 &&
                canvas.boundaryDraftPreview()->segments.size() == 1,
            "boundary draft preview getter must retain labels and analytical segments");
    draft.labels.front().text = QStringLiteral("mutated caller value");
    require(canvas.boundaryDraftPreview()->labels.front().text == QStringLiteral("2.00 m"),
            "boundary draft setter must retain an owned value copy");

    const auto screen_draft = render(canvas, false);
    const auto render_without_draft_labels = [&] {
        const auto original = *canvas.boundaryDraftPreview();
        auto without_labels = original;
        without_labels.labels.clear();
        canvas.setBoundaryDraftPreview(without_labels);
        const auto image = render(canvas, false);
        canvas.setBoundaryDraftPreview(original);
        return image;
    };
    const auto screen_without_labels = render_without_draft_labels();
    const auto arc_top = fit_screen_point(canvas, {0.0, 1.0});
    const auto arc_bottom = fit_screen_point(canvas, {0.0, -1.0});
    const auto arc_region_top = QRectF(arc_top.x() - 120.0, arc_top.y() - 18.0,
                                      240.0, 42.0).toAlignedRect();
    const auto arc_region_bottom = QRectF(arc_bottom.x() - 120.0, arc_bottom.y() - 18.0,
                                         240.0, 42.0).toAlignedRect();
    require(std::max(differing_pixels(screen_before, screen_draft, arc_region_top),
                     differing_pixels(screen_before, screen_draft, arc_region_bottom)) > 20,
            "onscreen draft image must contain the analytical arc");
    const auto label_center = fit_screen_point(canvas, {0.0, 1.5});
    const auto label_region = QRectF(label_center.x() - 60.0, label_center.y() - 24.0,
                                    120.0, 48.0).toAlignedRect();
    require(differing_pixels(screen_before, screen_draft, label_region) > 20,
            "onscreen draft image must contain the dimension label");
    const auto normal_label_bounds = bright_pixel_bounds(screen_draft, label_region,
                                                         &screen_without_labels);
    require(!normal_label_bounds.isEmpty(),
            "dimension label must contain readable bright text pixels");

    const auto output_after = render(canvas, true);
    require(images_equal(output_with_label, output_after),
            "fit-to-content output must ignore every transient draft overlay");

    const auto capture_directory = qEnvironmentVariable(
        "SKETCH_BOUNDARY_CANVAS_CAPTURE_DIR",
        qEnvironmentVariable("SKETCH_BOUNDARY_CAPTURE_DIR"));
    const auto normal_grab = canvas.grab();
    require(!normal_grab.isNull(), "normal boundary draft capture must be available");
    save_capture(capture_directory, QStringLiteral("boundary-draft-normal.png"),
                 normal_grab.toImage());
    save_capture(capture_directory, QStringLiteral("boundary-output-before.png"), output_before);
    save_capture(capture_directory, QStringLiteral("boundary-output-after.png"), output_after);

    canvas.zoomBy(1.5);
    process_events();
    const auto zoomed_screen = render(canvas, false);
    const auto zoomed_without_labels = render_without_draft_labels();
    const auto zoomed_grab = canvas.grab();
    require(!zoomed_grab.isNull(), "150 percent boundary draft capture must be available");
    save_capture(capture_directory, QStringLiteral("boundary-draft-150.png"), zoomed_grab.toImage());
    const auto zoomed_output = render(canvas, true);
    require(images_equal(output_before, zoomed_output),
            "fit-to-content output must stay unchanged after interactive zoom");

    // The label font is screen-space text, so zooming changes its position but
    // not its pixel footprint. Restrict the color oracle to pixels changed by
    // the label: Windows subpixel antialiasing can give the nearby instruction
    // text yellow fringes, which otherwise look like part of the zoomed label.
    const auto zoom_label_center = QPointF(canvas.rect().center().x(),
                                           canvas.rect().center().y() - 1.5 *
                                               (static_cast<double>(canvas.height()) /
                                                (4.0 + 2.0 * (4.0 * 0.12 + 0.25))) * 1.5);
    const auto zoom_label_region = QRectF(zoom_label_center.x() - 60.0,
                                          zoom_label_center.y() - 24.0, 120.0, 48.0)
                                             .toAlignedRect();
    const auto zoom_label_bounds = bright_pixel_bounds(zoomed_screen, zoom_label_region,
                                                       &zoomed_without_labels);
    require(!zoom_label_bounds.isEmpty() &&
                std::abs(normal_label_bounds.width() - zoom_label_bounds.width()) <= 2 &&
                std::abs(normal_label_bounds.height() - zoom_label_bounds.height()) <= 2,
            "dimension label must remain readable at 150 percent zoom");

    int undo_requests = 0;
    int redo_requests = 0;
    canvas.setDraftUndoRequested([&] { ++undo_requests; });
    canvas.setDraftRedoRequested([&] { ++redo_requests; });
    send_key(canvas, Qt::Key_Z, Qt::ControlModifier);
    send_key(canvas, Qt::Key_Y, Qt::ControlModifier);
    require(undo_requests == 1 && redo_requests == 1,
            "Ctrl+Z and Ctrl+Y must reach the registered draft callbacks");
    canvas.setDraftUndoRequested(std::function<void()>{});
    canvas.setDraftRedoRequested(std::function<void()>{});
    send_key(canvas, Qt::Key_Z, Qt::ControlModifier);
    send_key(canvas, Qt::Key_Y, Qt::ControlModifier);
    require(undo_requests == 1 && redo_requests == 1,
            "unregistered undo and redo callbacks must not intercept shortcuts");

    canvas.clearPreview();
    require(!canvas.boundaryDraftPreview().has_value(),
            "clearPreview must clear the document-independent draft value");
    require(canvas.labels().size() == 1 &&
                canvas.labels().front().text == QStringLiteral("Committed 7.25 m"),
            "clearPreview must preserve committed annotations");

    PlanCanvas labels_only;
    labels_only.resize(640, 480);
    labels_only.setGridEnabled(false);
    labels_only.setLabels({CanvasLabel{QStringLiteral("labels-only"), {42.0, -17.0},
                                       QStringLiteral("Only committed label"), false}});
    const auto labels_only_output = render(labels_only, true);
    require(!bright_pixel_bounds(labels_only_output, labels_only_output.rect()).isEmpty(),
            "fit-to-content output must remain valid for a labels-only canvas");
    canvas.setLabels({});
    require(canvas.labels().empty(), "committed-label clear must leave an empty label set");
}

void test_effective_cursor_matches_click() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setTool(CanvasTool::boundary);
    std::optional<Vec2> preview;
    std::optional<Vec2> placed;
    canvas.setCursorMoved([&](Vec2 point) { preview = point; });
    canvas.setPointClicked([&](Vec2 point) { placed = point; });
    canvas.setSnapEnabled(false);
    require(!preview, "snap toggle before pointer input must not invent a cursor");
    canvas.setSnapEnabled(true);
    const auto position = QRectF(canvas.rect()).center() + QPointF(27.0, 31.0);
    QMouseEvent move(QEvent::MouseMove, position, position, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    require(preview && preview->x == 0.25 && preview->y == -0.5,
            "off-grid cursor must preview the snapped point");
    const auto click = [&] {
        placed.reset();
        QMouseEvent press(QEvent::MouseButtonPress, position, position, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        require(!placed, "point authoring must wait for release so the same press can become navigation");
        QMouseEvent release(QEvent::MouseButtonRelease, position, position, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &release);
        require(placed && preview && placed->x == preview->x && placed->y == preview->y,
                "released point must exactly match the effective cursor");
    };
    click();
    canvas.setSnapEnabled(false);
    require(preview && preview->x == 27.0 / 80.0 && preview->y == -31.0 / 80.0,
            "disabling snap must refresh the stationary cursor to raw coordinates");
    click();
    canvas.setSnapEnabled(true);
    require(preview && preview->x == 0.25 && preview->y == -0.5,
            "enabling snap must refresh the stationary cursor");
    click();
    // A click can arrive without an intervening move (for example pen input).
    // The coordinate shown to the tool must agree inside its release callback.
    canvas.setPointClicked([&](Vec2 point) {
        require(preview && preview->x == point.x && preview->y == point.y,
                "release must publish its effective cursor before placing the point");
        placed = point;
    });
    placed.reset();
    const auto next_position = QRectF(canvas.rect()).center() + QPointF(-47.0, -53.0);
    QMouseEvent next_press(QEvent::MouseButtonPress, next_position, next_position,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &next_press);
    require(!placed, "press without motion must remain eligible to become a direct-draw drag");
    QMouseEvent next_release(QEvent::MouseButtonRelease, next_position, next_position,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &next_release);
    require(placed && placed->x == -0.5 && placed->y == 0.75,
            "release without preceding motion must use its own snapped coordinates");
}

void test_cursor_measurement_readout_is_transient_and_contextual() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setTool(CanvasTool::boundary);

    BoundaryDraftPreview draft;
    draft.anchor = Vec2{0.0, 0.0};
    draft.instruction = QStringLiteral("Place next point");
    canvas.setBoundaryDraftPreview(draft);

    const auto before_pointer = render(canvas, false);
    const auto position = QRectF(canvas.rect()).center() + QPointF(72.0, -48.0);
    QMouseEvent move(QEvent::MouseMove, position, position, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    const auto with_pointer = render(canvas, false);
    require(differing_pixels(before_pointer, with_pointer,
                             QRect(0, 0, canvas.width(), canvas.height())) > 30,
            "active boundary cursor must expose a transient measurement readout");

    // Cursor guidance is an editing affordance. It must never leak into a
    // fitted/exported scene, even when a draft and a pointer are active.
    const auto output_before = render(canvas, true);
    canvas.setBoundaryDraftPreview(std::nullopt);
    canvas.setTool(CanvasTool::select);
    const auto output_after = render(canvas, true);
    require(images_equal(output_before, output_after),
            "cursor measurement readout must stay out of fitted output");

    // Selecting is intentionally quiet: the readout appears when a precision
    // drawing tool is active, avoiding a permanent status label over the
    // canvas during ordinary navigation.
    canvas.setBoundaryDraftPreview(draft);
    canvas.setTool(CanvasTool::boundary);
    const auto drawing_mode = render(canvas, false);
    canvas.setTool(CanvasTool::select);
    const auto selected_mode = render(canvas, false);
    require(differing_pixels(drawing_mode, selected_mode,
                             QRect(0, 0, canvas.width(), canvas.height())) > 30,
            "select mode must hide the cursor measurement panel");
}

void test_overview_map_navigation() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setEntities({CanvasEntity{
        QStringLiteral("overview-rectangle"),
        QStringLiteral("measurement_boundary"),
        Boundary{
            Segment{{-12.0, -8.0}, {12.0, -8.0}, 0.0},
            Segment{{12.0, -8.0}, {12.0, 8.0}, 0.0},
            Segment{{12.0, 8.0}, {-12.0, 8.0}, 0.0},
            Segment{{-12.0, 8.0}, {-12.0, -8.0}, 0.0},
        },
        0.08,
        false,
    }});
    canvas.fitView();
    const auto map = canvas.overviewMapRect();
    require(map.width() >= 120.0 && map.height() >= 80.0 &&
                map.right() <= canvas.width() && map.bottom() <= canvas.height(),
            "overview map must reserve a compact in-canvas navigation surface");
    const auto interactive = render(canvas, false);
    QImage plain(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    plain.fill(background.rgba());
    require(differing_pixels(interactive, plain, map.toAlignedRect()) > 40,
            "overview map must render content and viewport affordances");

    // Move the viewport away from the map center, then click a model location
    // in the map. A real navigation click must update the view center.
    const auto center_before = canvas.viewCenter();
    const auto drag_start = QPointF(canvas.rect().center());
    const auto drag_end = drag_start + QPointF(180.0, -90.0);
    QMouseEvent press(QEvent::MouseButtonPress, drag_start, drag_start,
                      Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, drag_end, drag_end,
                     Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, drag_end, drag_end,
                        Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
    const auto center_panned = canvas.viewCenter();
    require(std::abs(center_panned.x - center_before.x) > 1e-9 ||
                std::abs(center_panned.y - center_before.y) > 1e-9,
            "canvas pan must move the viewport before overview navigation");
    const auto map_target = map.topLeft() + QPointF(map.width() * 0.2, map.height() * 0.8);
    QMouseEvent map_press(QEvent::MouseButtonPress, map_target, map_target,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &map_press);
    QMouseEvent map_release(QEvent::MouseButtonRelease, map_target, map_target,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &map_release);
    const auto center_overview = canvas.viewCenter();
    require(std::abs(center_overview.x - center_panned.x) > 1e-9 ||
                std::abs(center_overview.y - center_panned.y) > 1e-9,
            "overview map click must recenter the viewport on the selected model location");
    canvas.setOverviewMapEnabled(false);
    require(canvas.overviewMapRect().isEmpty(),
            "overview map visibility must be user-controllable");
    canvas.setOverviewMapEnabled(true);
    require(!canvas.overviewMapRect().isEmpty(),
            "overview map can be restored after being hidden");
}

}  // namespace

void test_site_scale_fit() {
    PlanCanvas canvas;
    canvas.resize(480, 360);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    for (const double extent : {100.0, 10000.0}) {
        canvas.setEntities({});
        const auto blank = render(canvas, false);
        canvas.setEntities({{"site", "measurement_boundary",
            {{{0, 0}, {extent, 0}, 0}, {{extent, 0}, {extent, extent}, 0},
             {{extent, extent}, {0, extent}, 0}, {{0, extent}, {0, 0}, 0}}}});
        canvas.fitView();
        const auto fitted = render(canvas, false);
        require(differing_pixels(blank, fitted, canvas.rect()) > 200,
                "fit must render site-scale boundaries in the current viewport");
        require(differing_pixels(blank, fitted, QRect(0, 0, 480, 10)) == 0 &&
                    differing_pixels(blank, fitted, QRect(0, 350, 480, 10)) == 0,
                "fit must leave margin around site boundaries");
        const auto output = render(canvas, true);
        require(differing_pixels(blank, output, canvas.rect()) > 200 &&
                    differing_pixels(blank, output, QRect(0, 0, 480, 10)) == 0 &&
                    differing_pixels(blank, output, QRect(0, 350, 480, 10)) == 0,
                "fit-to-page output must retain the complete site boundary with margins");
    }
}

void test_arc_render_orientation() {
    PlanCanvas canvas;
    canvas.resize(400,400);
    canvas.setGridEnabled(false);
    const auto output=[&] {
        QImage image(400,400,QImage::Format_ARGB32_Premultiplied);
        image.fill(background);
        QPainter painter(&image);
        canvas.renderSceneAt(painter,QRectF(image.rect()),100,{0,0},background);
        return image;
    };
    const auto blank=output();
    for(double sign:{-1.0,1.0}) {
        canvas.setEntities({CanvasEntity{"arc","opening",{{{1,0},{0,sign},sign*std::numbers::pi/2}},0,false}});
        const auto image=output();
        const auto expected_y=qRound(200-sign*100/std::sqrt(2.0));
        const auto opposite_y=qRound(200+sign*100/std::sqrt(2.0));
        require(differing_pixels(blank,image,QRect(265,expected_y-5,12,12))>0 &&
            differing_pixels(blank,image,QRect(265,opposite_y-5,12,12))==0,
            "rendered analytic arcs must follow model sweep and meet the leaf endpoint");
    }
}

QRect red_text_bounds(const QImage& image) {
    QRect result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (color.red() > 150 && color.green() < 100 && color.blue() < 100)
                result = result.united(QRect(x, y, 1, 1));
        }
    }
    return result;
}

void test_paper_label_style_and_hit_testing() {
    PlanCanvas canvas;
    canvas.resize(800, 600);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    CanvasLabel label{QStringLiteral("paper-label"), {0, 0}, QStringLiteral("Dimension 123.45")};
    label.paper_height_mm = 4.0;
    label.color = QColor(220, 20, 20);
    canvas.setLabels({label});
    const auto output = [&](double scale, int dpi,
                            std::optional<double> paper_pixels_per_mm = std::nullopt) {
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        image.setDotsPerMeterX(qRound(dpi / 0.0254));
        image.setDotsPerMeterY(qRound(dpi / 0.0254));
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas.renderSceneAt(painter, QRectF(image.rect()), scale, {0, 0}, Qt::white,
                             paper_pixels_per_mm);
        return image;
    };
    const auto at_50 = output(50, 96);
    require(images_equal(at_50, output(200, 96)),
        "paper label pixels must be independent of model-to-output scale");
    const auto normal_bounds = red_text_bounds(at_50);
    const auto high_dpi_bounds = red_text_bounds(output(50, 192));
    require(!normal_bounds.isEmpty() && !high_dpi_bounds.isEmpty(),
        "explicit label color must reach output glyph pixels");
    require(std::abs(high_dpi_bounds.height() - 2 * normal_bounds.height()) <= 3 &&
            std::abs(high_dpi_bounds.width() - 2 * normal_bounds.width()) <= 5,
        "paper text must preserve physical size across output-device DPI");
    const auto fitted_paper = output(50, 96, 4.0);
    const auto fitted_bounds = red_text_bounds(fitted_paper);
    const auto double_paper_bounds = red_text_bounds(output(50, 96, 8.0));
    require(!fitted_bounds.isEmpty() &&
            std::abs(double_paper_bounds.height() - 2 * fitted_bounds.height()) <= 3 &&
            std::abs(double_paper_bounds.width() - 2 * fitted_bounds.width()) <= 5,
        "doubling fitted sheet paper scale must double label pixels at the same device DPI");
    require(images_equal(fitted_paper, output(200, 96, 4.0)),
        "fitted paper label size must remain independent of viewport model scale");
    require(images_equal(fitted_paper, output(50, 192, 4.0)),
        "explicit fitted sheet paper scale must override output-device DPI");
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        require(images_equal(at_50, output(50, 96, invalid)),
            "invalid paper scale must preserve the device-DPI fallback");
    }
    const auto screen_output = [&](int dpi) {
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        image.setDotsPerMeterX(qRound(dpi / 0.0254));
        image.setDotsPerMeterY(qRound(dpi / 0.0254));
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas.renderScene(painter, QRectF(image.rect()), false, Qt::white);
        return image;
    };
    const auto screen_before_zoom = red_text_bounds(screen_output(96));
    canvas.zoomBy(2.0, QRectF(canvas.rect()).center());
    require(screen_before_zoom == red_text_bounds(screen_output(192)),
        "interactive paper text must use fixed widget DPI and ignore model zoom");
    const auto unselected_screen = screen_output(96);
    label.selected = true;
    canvas.setLabels({label});
    const auto selected_screen = screen_output(96);
    require(!images_equal(unselected_screen, selected_screen) &&
            red_text_bounds(unselected_screen) == red_text_bounds(selected_screen),
        "styled screen selection must add an outline while retaining authored text color");
    require(images_equal(at_50, output(50, 96)),
        "paper label output must suppress the screen selection outline");
    label.selected = false;
    canvas.setLabels({label});
    label.bold = true;
    canvas.setLabels({label});
    const auto bold = output(50, 96);
    require(!images_equal(at_50, bold), "bold label style must affect rendered glyphs");
    label.italic = true;
    canvas.setLabels({label});
    require(!images_equal(bold, output(50, 96)), "italic label style must affect rendered glyphs");
    label.rotation_radians = std::numbers::pi / 2;
    canvas.setLabels({label});
    const auto rotated_bounds = red_text_bounds(output(50, 96));
    require(rotated_bounds.height() > rotated_bounds.width() * 2,
        "paper style must retain model-to-screen label rotation");

    // A point near the rotated text's far end is well outside the old
    // nine-pixel anchor hit radius, but inside the actual styled text bounds.
    QFont styled_font = canvas.font();
    styled_font.setPixelSize(qRound(label.paper_height_mm * canvas.logicalDpiY() / 25.4));
    styled_font.setBold(true);
    styled_font.setItalic(true);
    const QFontMetricsF metrics(styled_font, &canvas);
    const auto far_offset = metrics.boundingRect(label.text).width() * 0.35;
    require(far_offset > 9, "styled hit-test fixture must exceed anchor radius");
    const auto center = QRectF(canvas.rect()).center();
    QString selected;
    canvas.setEntityClicked([&](QString id) { selected = std::move(id); });
    const auto click = [&](QPointF point) {
        selected.clear();
        QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &release);
    };
    click(center + QPointF(0, -far_offset));
    require(selected == label.id, "rotated styled label must be selectable across its painted extent");
    click(center + QPointF(far_offset, 0));
    require(selected.isEmpty(), "rotated hit-test must not select the unrotated text extent");
    canvas.zoomBy(2.0, center);
    click(center + QPointF(0, -far_offset));
    require(selected == label.id, "paper-space label hit extent must remain fixed during zoom");
    canvas.setEntities({{"under-label", "wall", {{{-2,0},{2,0},0}}, 0.08, false}});
    click(center);
    require(selected == label.id, "a label painted over geometry must win an overlapping hit");
    canvas.setEntities({});

    label.rotation_radians = 0;
    label.paper_height_mm = 0;
    label.bold = false;
    label.italic = false;
    canvas.setLabels({label});
    require(!images_equal(output(50, 96), output(200, 96)),
        "legacy model-height labels must continue scaling with model-to-output scale");
}

void test_analytic_arc_fit_bounds() {
    sketch::desktop::PlanCanvas canvas;
    const auto end=std::numbers::pi+0.3;
    canvas.setEntities({{"arc","wall",{{{std::cos(-0.1),std::sin(-0.1)},
        {std::cos(end),std::sin(end)},std::numbers::pi+0.4}},0.1,false}});
    const auto center=canvas.contentCenter();
    require(std::abs(center.x)<1e-12 && std::abs(center.y-(1-std::sin(0.3))/2)<1e-12,
        "canvas fitting must use analytical extrema instead of sampled arc points");
    canvas.resize(800,600);
    canvas.fitView();
    require(std::abs(canvas.viewCenter().x-center.x)<1e-12 && std::abs(canvas.viewCenter().y-center.y)<1e-12,
        "interactive fit and content-center queries must use identical geometry bounds");
}

void test_reference_grid_labels_render_in_screen_and_output() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);

    sketch::ReferenceGridModel model;
    model.origin_m = {0.0, 0.0};
    model.spacing_x_m = 2.0;
    model.spacing_y_m = 2.0;
    model.count_x = 1;
    model.count_y = 1;
    model.major_every = 1;
    model.x_label = "X";
    model.y_label = "Y";
    const auto lines = model.lines();
    canvas.setReferenceGrids({CanvasReferenceGrid{QStringLiteral("grid"), lines, true,
                                                 QString(), QString()}});
    canvas.fitView();

    const auto without_labels = render(canvas, false);
    canvas.setReferenceGrids({CanvasReferenceGrid{QStringLiteral("grid"), lines, true,
                                                 QStringLiteral("X"), QStringLiteral("Y")}});
    const auto screen = render(canvas, false);
    require(differing_pixels(without_labels, screen, canvas.rect()) > 20,
            "reference-grid labels must be present in the interactive canvas");
    canvas.setReferenceGrids({CanvasReferenceGrid{QStringLiteral("grid"), lines, true,
                                                 QString(), QString()}});
    const auto output_without_labels = render(canvas, true);
    const auto without_labels_output = render(canvas, true);
    require(images_equal(without_labels_output, output_without_labels),
            "reference-grid output must remain deterministic when labels are absent");
    canvas.setReferenceGrids({CanvasReferenceGrid{QStringLiteral("grid"), lines, true,
                                                 QStringLiteral("X"), QStringLiteral("Y")}});
    const auto output = render(canvas, true);
    require(differing_pixels(output_without_labels, output, canvas.rect()) > 20,
            "reference-grid labels must be present in fit-to-content output");
}

void test_closed_entity_hatching_and_open_path_safety() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);

    CanvasEntity square{
        QStringLiteral("section-cut"),
        QStringLiteral("slab"),
        Boundary{
            Segment{{-2.0, -2.0}, {2.0, -2.0}, 0.0},
            Segment{{2.0, -2.0}, {2.0, 2.0}, 0.0},
            Segment{{2.0, 2.0}, {-2.0, 2.0}, 0.0},
            Segment{{-2.0, 2.0}, {-2.0, -2.0}, 0.0},
        },
    };
    canvas.setEntities({square});
    canvas.fitView();
    const auto outline = render(canvas, true);

    square.filled = true;
    square.hatch_pattern = QStringLiteral("cross");
    square.hatch_scale = 1.5;
    canvas.setEntities({square});
    const auto hatched = render(canvas, true);
    require(differing_pixels(outline, hatched, QRect(150, 100, 340, 280)) > 20,
            "closed section paths must render the selected hatch pattern");

    square.hatch_pattern = QStringLiteral("solid");
    canvas.setEntities({square});
    const auto filled = render(canvas, true);
    require(differing_pixels(outline, filled, QRect(150, 100, 340, 280)) > 100,
            "solid section presentation must fill the closed projected path");

    square.holes = {Boundary{
        sketch::arc_from_chord_angle({-0.6, 0.4}, {0.6, 0.4}, std::numbers::pi),
        Segment{{0.6, 0.4}, {-0.6, 0.4}, 0.0},
    }};
    canvas.setEntities({square});
    const auto with_void = render(canvas, true);
    require(differing_pixels(filled, with_void, QRect(270, 170, 100, 140)) > 100,
            "semantic holes must remain clear in filled screen and output paths");
    canvas.setTool(CanvasTool::select);
    QString selected;
    canvas.setRightClicked([&](Vec2, QString id) { selected = std::move(id); });
    const auto center = QRectF(canvas.rect()).center();
    const auto context_click = [&](QPointF point) {
        selected.clear();
        QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::RightButton,
                          Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::RightButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &release);
    };
    context_click(center);
    require(selected.isEmpty(),
            "clicking a two-segment curved semantic void must target canvas space");
    canvas.setSelectedId(square.id);
    const auto frame = canvas.selectionBounds();
    require(frame.has_value(), "curved-hole fixture must expose its outer selection frame");
    const auto body_point = frame->center() + QPointF(frame->width() * 0.30, 0.0);
    context_click(body_point);
    require(selected == square.id,
            "filled room interior outside a curved void must remain selectable");
    const auto chord_point = frame->center() - QPointF(0.0, frame->height() * 0.10);
    context_click(chord_point);
    require(selected == square.id, "the curved void outline must remain selectable");
    square.holes.clear();
    square.selected = false;
    canvas.setEntities({square});
    canvas.setSelectedId({});
    context_click(center);
    require(selected == square.id,
            "the same point must select the painted entity after its void is removed");

    square.segments.pop_back();
    canvas.setEntities({square});
    const auto open_filled = render(canvas, true);
    square.filled = false;
    canvas.setEntities({square});
    const auto open_outline = render(canvas, true);
    require(images_equal(open_filled, open_outline),
            "open or split paths must never receive a misleading material fill");
}

void test_explicit_output_excludes_interactive_state() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(true);
    canvas.setOverviewMapEnabled(false);
    CanvasEntity wall{
        QStringLiteral("wall"), QStringLiteral("wall"),
        Boundary{{Segment{{-1.0, 0.0}, {1.0, 0.0}, 0.0}}}, 0.12, true};
    canvas.setEntities({wall});
    canvas.setBoundaryPreview({{-1.0, -1.0}, {1.0, -1.0}});
    canvas.setWallPreview(std::make_pair(Vec2{-1.0, 1.0}, Vec2{1.0, 1.0}));
    BoundaryDraftPreview draft;
    draft.segments = {Segment{{-1.0, -0.5}, {1.0, -0.5}, 0.0}};
    draft.instruction = QStringLiteral("interactive draft instruction");
    canvas.setBoundaryDraftPreview(draft);
    canvas.setTool(CanvasTool::boundary);

    const auto output = [&](PlanCanvas& target) {
        QImage image(640, 480, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        target.renderSceneAt(painter, QRectF(image.rect()), 100.0, {0.0, 0.0}, Qt::white);
        return image;
    };

    PlanCanvas clean;
    clean.resize(640, 480);
    clean.setGridEnabled(true);
    clean.setOverviewMapEnabled(false);
    clean.setEntities({wall});
    require(images_equal(output(canvas), output(clean)),
            "explicit sheet output must exclude interactive overlays and instructions");
}

void test_output_stroke_width_is_paper_space() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    CanvasEntity line{
        QStringLiteral("line"), QStringLiteral("dimension_line"),
        Boundary{{Segment{{-4.0, 0.0}, {4.0, 0.0}, 0.0}}}};
    line.output_stroke_width_mm = 1.0;
    canvas.setEntities({line});

    const auto render_output = [&](double scale) {
        QImage image(640, 480, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas.renderSceneAt(painter, QRectF(image.rect()), scale, {0.0, 0.0}, Qt::white,
                             4.0);
        return image;
    };
    const auto dark_span = [](const QImage& image) {
        int span = 0;
        for (int y = 0; y < image.height(); ++y) {
            const auto color = image.pixelColor(image.width() / 2, y);
            if (color.lightness() < 180) ++span;
        }
        return span;
    };

    const auto at_50 = render_output(50.0);
    const auto at_200 = render_output(200.0);
    require(std::abs(dark_span(at_50) - dark_span(at_200)) <= 1,
            "output line treatment must stay the same across independent viewport scales");

    line.output_stroke_width_mm = 3.0;
    canvas.setEntities({line});
    require(dark_span(render_output(50.0)) > dark_span(at_50) + 4,
            "larger persisted paper line width must visibly increase output stroke weight");
}

void test_request_to_paint_telemetry() {
    using sketch::PerformanceMetric;
    using Clock = std::chrono::steady_clock;
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.show();
    process_events();
    std::vector<std::pair<PerformanceMetric, Clock::duration>> samples;
    auto earliest_completion = Clock::time_point::min();
    canvas.setPerformanceMeasured([&](PerformanceMetric metric, Clock::duration elapsed) {
        require(!canvas.paintingActive(), "performance callback must follow QPainter completion");
        require(elapsed >= Clock::duration::zero(), "paint latency must be nonnegative");
        earliest_completion = Clock::now() - elapsed;
        samples.emplace_back(metric, elapsed);
    });
    const auto count = [&](PerformanceMetric metric) {
        return std::count_if(samples.begin(), samples.end(), [&](const auto& sample) {
            return sample.first == metric;
        });
    };
    canvas.fitView();
    require(samples.empty(), "fit handler must not report before painting");
    const auto before_coalesced = Clock::now();
    canvas.zoomBy(1.2);
    canvas.fitView();
    render(canvas, true);
    require(samples.empty(), "export rendering must not complete a pending screen measurement");
    process_events();
    require(count(PerformanceMetric::navigation) == 1,
            "coalesced fit and zoom must report exactly one sample after paint");
    require(earliest_completion <= before_coalesced + std::chrono::milliseconds(1),
            "coalesced navigation must retain the oldest request timestamp");
    canvas.update();
    process_events();
    require(samples.size() == 1, "unrelated repaint must not add samples");

    samples.clear();
    canvas.beginPerformanceMeasurement(PerformanceMetric::edit);
    canvas.setEntities({{"wall", "wall", {{{-1, 0}, {1, 0}, 0}}, 0.08, false}});
    require(samples.empty(), "edit dispatch must wait for paint");
    process_events();
    require(count(PerformanceMetric::edit) == 1, "edit must complete at its screen paint");

    samples.clear();
    const auto mouse = [&](QEvent::Type type, QPointF position, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, {200, 200}, Qt::MiddleButton, Qt::MiddleButton);
    mouse(QEvent::MouseMove, {220, 220}, Qt::NoButton, Qt::MiddleButton);
    mouse(QEvent::MouseButtonRelease, {220, 220}, Qt::MiddleButton, Qt::NoButton);
    require(samples.empty(), "pan and mouse dispatch must wait for paint");
    process_events();
    require(count(PerformanceMetric::navigation) == 1 && count(PerformanceMetric::input) == 1,
            "mouse pan batch must measure input and navigation through paint");
    samples.clear();
    mouse(QEvent::MouseButtonPress, canvas.overviewMapRect().center(),
          Qt::LeftButton, Qt::LeftButton);
    process_events();
    require(count(PerformanceMetric::navigation) == 1 && count(PerformanceMetric::input) == 1,
            "overview click must measure input and navigation");

    samples.clear();
    QKeyEvent fit(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &fit);
    require(samples.empty(), "keyboard dispatch must wait for paint");
    process_events();
    require(count(PerformanceMetric::navigation) == 1 && count(PerformanceMetric::input) == 1,
            "nonmodal keyboard navigation must reach paint");

    samples.clear();
    canvas.fitView();
    canvas.hide();
    canvas.beginPerformanceMeasurement(PerformanceMetric::edit);
    canvas.fitView();
    render(canvas, false);
    canvas.show();
    process_events();
    require(samples.empty(), "hide must discard pending measurements and hidden requests");

    canvas.fitView();
    QDialog modal(&canvas);
    modal.setModal(true);
    modal.show();
    process_events();
    modal.hide();
    canvas.update();
    process_events();
    require(samples.empty(), "opening a modal must discard pending operation latency");
    canvas.setTool(CanvasTool::boundary);
    process_events();
    canvas.setPreciseInputRequested([&] { canvas.setBoundaryPreview({{0, 0}, {1, 1}}); });
    QKeyEvent precise(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &precise);
    process_events();
    require(samples.empty(), "precise-input dispatch must not sample a modal deliberation interval");

    canvas.beginPerformanceMeasurement(PerformanceMetric::edit);
    canvas.cancelPerformanceMeasurement(PerformanceMetric::edit);
    canvas.update();
    process_events();
    require(samples.empty(), "cancelled edit must not attach to an unrelated paint");

    canvas.beginPerformanceMeasurement(PerformanceMetric::edit);
    canvas.fitView();
    canvas.resetPerformanceMeasurements();
    canvas.setEntities({});
    process_events();
    require(samples.empty(), "reset before document replacement must discard the entire pending batch");

    // Send Qt input events through the widget dispatch path rather than
    // substituting explicit begin calls for actual input instrumentation.
    int authored_points = 0;
    canvas.setPointClicked([&](Vec2) { ++authored_points; });
    QPointingDevice touch_device(QStringLiteral("canvas-test-touch"), 101,
        QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger,
        QInputDevice::Capability::Position, 1, 0);
    const auto touch = [&](QEvent::Type type, QEventPoint::State state) {
        QTouchEvent event(type, &touch_device, Qt::NoModifier,
                         {QEventPoint(1, state, QPointF(180, 180), QPointF(180, 180))});
        QApplication::sendEvent(&canvas, &event);
    };
    touch(QEvent::TouchBegin, QEventPoint::State::Pressed);
    touch(QEvent::TouchUpdate, QEventPoint::State::Updated);
    touch(QEvent::TouchEnd, QEventPoint::State::Released);
    require(authored_points == 1, "touch events must reach the authoring callback");
    require(samples.empty(), "touch dispatch must wait for a screen paint");
    process_events();
    require(count(PerformanceMetric::input) == 1,
            "touch press, move and release must coalesce into one painted input sample");

    samples.clear();
    QPointingDevice pen_device(QStringLiteral("canvas-test-pen"), 102,
        QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
    const auto tablet = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QTabletEvent event(type, &pen_device, QPointF(190, 190), QPointF(190, 190),
                           0.5, 0, 0, 0, 0, 0, Qt::NoModifier, button, buttons);
        QApplication::sendEvent(&canvas, &event);
    };
    tablet(QEvent::TabletPress, Qt::LeftButton, Qt::LeftButton);
    tablet(QEvent::TabletMove, Qt::NoButton, Qt::LeftButton);
    tablet(QEvent::TabletRelease, Qt::LeftButton, Qt::NoButton);
    require(authored_points == 2, "pen events must reach the authoring callback");
    require(samples.empty(), "pen dispatch must wait for a screen paint");
    process_events();
    require(count(PerformanceMetric::input) == 1,
            "pen press, move and release must coalesce into one painted input sample");

    samples.clear();
    QWheelEvent wheel(QPointF(200, 200), QPointF(200, 200), {}, QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &wheel);
    require(samples.empty(), "wheel dispatch must wait for a screen paint");
    process_events();
    require(count(PerformanceMetric::navigation) == 1 && count(PerformanceMetric::input) == 1,
            "wheel zoom must measure input and navigation through paint");
}

void test_selection_frame_for_styled_geometry() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    CanvasEntity symbol{QStringLiteral("symbol"), QStringLiteral("symbol"),
                        Boundary{Segment{{-1.0, 0.0}, {1.0, 0.0}, 0.0}}};
    symbol.stroke_color = QColor(110, 110, 110);
    symbol.stroke_width_metres = 0.02;
    canvas.setEntities({symbol});
    const auto plain = render(canvas, false);
    const auto output = render(canvas, true);
    canvas.setSelectedId(symbol.id);
    const auto bounds = canvas.selectionBounds();
    require(bounds && bounds->contains(QPointF(240, 240)) && bounds->contains(QPointF(400, 240)),
            "contextual selection bounds must enclose the selected geometry");
    const auto selected = render(canvas, false);
    require(differing_pixels(plain, selected, QRect(225, 225, 190, 30)) > 100,
            "selection frame must remain visible when authored styles override selected strokes");
    require(images_equal(output, render(canvas, true)),
            "selection frame must not appear in fitted output");
    canvas.zoomBy(0.000001, QPointF(320, 240));
    const auto tiny_selected = render(canvas, false);
    canvas.setSelectedId({});
    require(!canvas.selectionBounds(), "cleared selection must have no contextual anchor");
    require(differing_pixels(tiny_selected, render(canvas, false), QRect(290, 210, 60, 60)) > 80,
            "selection markers must retain screen size when geometry shrinks below a pixel");

    canvas.zoomBy(1000000000, QPointF(320, 240));
    symbol.id = QStringLiteral("ordinary");
    symbol.type = QStringLiteral("wall");
    symbol.segments = {Segment{{-0.02, 0.0}, {0.02, 0.0}, 1.0}};
    symbol.thickness_metres = 0.001;
    canvas.setEntities({symbol});
    const auto ordinary = render(canvas, false);
    canvas.setSelectedId(symbol.id);
    require(!images_equal(ordinary, render(canvas, false)),
            "ordinary entities must also retain selection feedback at maximum zoom");
    symbol.segments = {Segment{{-100.0, -100.0}, {100.0, 100.0}, 0.0}};
    symbol.selected = false;
    canvas.setEntities({symbol});
    const auto oversized = render(canvas, false);
    canvas.setSelectedId(symbol.id);
    require(differing_pixels(oversized, render(canvas, false), QRect(0, 0, 640, 15)) > 100,
            "a selection larger than the viewport must keep a visible frame at its edge");
    const auto explicit_output = [&] {
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        canvas.renderSceneAt(painter, image.rect(), 100, {}, background);
        return image;
    };
    const auto selected_output = explicit_output();
    canvas.setSelectedId({});
    require(images_equal(selected_output, explicit_output()),
            "explicit-scale output must suppress the selection frame");
}

void test_mouse_gesture_contract() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setOverviewMapEnabled(false);
    canvas.setSnapEnabled(false);
    int points = 0, selections = 0, rights = 0, finishes = 0, cursors = 0;
    Vec2 right_point{}, cursor{};
    QStringList hits;
    bool additive = false;
    canvas.setPointClicked([&](Vec2) { ++points; });
    canvas.setEntitySelectionClicked([&](QString, bool) { ++selections; });
    canvas.setEntitiesSelected([&](QStringList ids, bool add) {
        ++selections; hits = ids; additive = add;
    });
    canvas.setRightClicked([&](Vec2 p, QString) { ++rights; right_point = p; });
    canvas.setFinishRequested([&] { ++finishes; });
    canvas.setCursorMoved([&](Vec2 p) { ++cursors; cursor = p; });
    const auto mouse = [&](QEvent::Type type, QPointF p, Qt::MouseButton button,
                           Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent event(type, p, canvas.mapToGlobal(p.toPoint()), button, buttons, mods);
        QApplication::sendEvent(&canvas, &event);
    };
    for (auto tool : {CanvasTool::select, CanvasTool::boundary, CanvasTool::wall, CanvasTool::sloped_wall}) {
        canvas.setTool(tool);
        canvas.fitView();
        mouse(QEvent::MouseButtonPress, {320,240}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(QEvent::MouseButtonPress, {320,240}, Qt::LeftButton, Qt::MiddleButton | Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, {320,240}, Qt::LeftButton, Qt::MiddleButton);
        mouse(QEvent::MouseMove, {360,240}, Qt::NoButton, Qt::MiddleButton);
        require(std::abs(canvas.viewCenter().x + 0.5) < 1e-9,
                "middle drag must pan every tool and ignore unrelated buttons");
        mouse(QEvent::MouseButtonRelease, {360,240}, Qt::MiddleButton, Qt::NoButton);
    }
    require(points == 0 && selections == 0 && finishes == 0, "navigation must never author or select");
    canvas.setTool(CanvasTool::boundary);
    canvas.fitView();
    mouse(QEvent::MouseButtonPress, {360,220}, Qt::RightButton, Qt::RightButton);
    require(rights == 0 && finishes == 0, "right press must not dispatch actions");
    mouse(QEvent::MouseButtonRelease, {360,220}, Qt::RightButton, Qt::NoButton);
    require(rights == 1 && std::abs(right_point.x - 0.5) < 1e-9 &&
            std::abs(right_point.y - 0.25) < 1e-9, "right release supplies effective plan point");
    mouse(QEvent::MouseButtonPress, {360,220}, Qt::RightButton, Qt::RightButton);
    mouse(QEvent::MouseMove, {400,260}, Qt::NoButton, Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, {360,220}, Qt::RightButton, Qt::NoButton);
    require(rights == 1 && finishes == 0 && points == 0, "right drag returning to origin must not click");

    BoundaryDraftPreview closable;
    closable.anchor = Vec2{0.0, 0.0};
    closable.segments = {
        Segment{{0.0, 0.0}, {2.0, 0.0}, 0.0},
        Segment{{2.0, 0.0}, {2.0, 2.0}, 0.0},
        Segment{{2.0, 2.0}, {0.0, 2.0}, 0.0},
    };
    closable.can_close_on_anchor = true;
    canvas.setBoundaryDraftPreview(closable);
    mouse(QEvent::MouseButtonPress, {320,240}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {320,240}, Qt::LeftButton, Qt::NoButton);
    require(finishes == 1 && points == 0,
            "clicking the highlighted first node must close instead of adding another node");
    canvas.setBoundaryDraftPreview(std::nullopt);

    for (auto tool : {CanvasTool::boundary, CanvasTool::select}) {
        canvas.setTool(tool);
        mouse(QEvent::MouseButtonPress, {320,240}, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        mouse(QEvent::MouseButtonRelease, {320,240}, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        mouse(QEvent::MouseButtonDblClick, {320,240}, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        mouse(QEvent::MouseButtonRelease, {320,240}, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    }
    require(points == 2 && selections == 0,
            "double click must not duplicate either boundary-mode or unified empty-canvas authoring");
    const int before = selections;
    mouse(QEvent::MouseButtonPress, {220,220}, Qt::LeftButton, Qt::LeftButton,
          Qt::ControlModifier);
    mouse(QEvent::MouseButtonPress, {220,220}, Qt::MiddleButton, Qt::LeftButton | Qt::MiddleButton);
    mouse(QEvent::MouseMove, {260,240}, Qt::NoButton, Qt::LeftButton | Qt::MiddleButton);
    mouse(QEvent::MouseButtonRelease, {260,240}, Qt::MiddleButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {260,240}, Qt::LeftButton, Qt::NoButton);
    require(selections == before, "pan takeover must abandon marquee");
    for (auto type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
        mouse(QEvent::MouseButtonPress, {220,220}, Qt::LeftButton, Qt::LeftButton,
              Qt::ControlModifier);
        QEvent cancel(type);
        QApplication::sendEvent(&canvas, &cancel);
        mouse(QEvent::MouseButtonRelease, {420,260}, Qt::LeftButton, Qt::NoButton);
    }
    require(selections == before, "lost gesture ownership must not complete selection");
    mouse(QEvent::MouseButtonPress, {220,220}, Qt::LeftButton, Qt::LeftButton,
          Qt::ControlModifier);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    mouse(QEvent::MouseButtonRelease, {420,260}, Qt::LeftButton, Qt::NoButton);
    require(selections == before, "Escape must abandon a pending marquee");
    const int cursor_before = cursors;
    canvas.zoomBy(2.0, {100,100});
    require(cursors > cursor_before, "zoom must refresh effective cursor callback");
    canvas.setSnapEnabled(true);
    mouse(QEvent::MouseButtonPress, {337,231}, Qt::RightButton, Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, {337,231}, Qt::RightButton, Qt::NoButton);
    require(right_point.x == cursor.x && right_point.y == cursor.y,
            "right-click point must match snapped effective cursor");

    canvas.setTool(CanvasTool::select);
    canvas.setSnapEnabled(false);
    canvas.fitView();
    bool point_saw_release_cursor = false;
    canvas.setPointClicked([&](Vec2 point) {
        point_saw_release_cursor = true;
        right_point = cursor;
        require(point.x == cursor.x && point.y == cursor.y,
                "drawing node must use the effective release cursor");
    });
    mouse(QEvent::MouseButtonPress, {319,240}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {321,240}, Qt::LeftButton, Qt::NoButton);
    require(point_saw_release_cursor && right_point.x == cursor.x && right_point.y == cursor.y,
            "empty-canvas drawing click must observe the effective release point when no move event occurs");
}

void test_direct_canvas_manipulation_contract() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setTool(CanvasTool::select);
    canvas.setEntities({{QStringLiteral("component"), QStringLiteral("symbol"),
                         {Segment{{-1, 0}, {1, 0}, 0}}}});

    QStringList selected;
    int selection_clicks = 0;
    int marquee_requests = 0;
    int move_requests = 0;
    int point_clicks = 0;
    QStringList moved_ids;
    Vec2 moved_delta{};
    Vec2 clicked_point{};
    QString context_target;
    QString double_clicked;
    canvas.setEntitySelectionClicked([&](QString id, bool toggle) {
        ++selection_clicks;
        if (id.isEmpty()) {
            if (!toggle) selected.clear();
        } else if (toggle && selected.contains(id)) {
            selected.removeAll(id);
        } else {
            if (!toggle) selected.clear();
            if (!selected.contains(id)) selected.push_back(id);
        }
        canvas.setSelectedIds(selected);
    });
    canvas.setEntitiesSelected([&](QStringList ids, bool additive) {
        ++marquee_requests;
        require(additive, "Ctrl-drag marquee must add to the current selection");
        for (const auto& id : ids) if (!selected.contains(id)) selected.push_back(id);
        canvas.setSelectedIds(selected);
    });
    canvas.setEntitiesMoveRequested([&](QStringList ids, Vec2 delta) {
        ++move_requests;
        moved_ids = std::move(ids);
        moved_delta = delta;
        return true;
    });
    canvas.setPointClicked([&](Vec2 point) {
        ++point_clicks;
        clicked_point = point;
    });
    canvas.setRightClicked([&](Vec2, QString id) { context_target = std::move(id); });
    canvas.setEntityDoubleClicked([&](QString id) { double_clicked = std::move(id); });

    const auto mouse = [&](QEvent::Type type, QPointF p, Qt::MouseButton button,
                           Qt::MouseButtons buttons,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, p, canvas.mapToGlobal(p.toPoint()), button, buttons, modifiers);
        QApplication::sendEvent(&canvas, &event);
    };

    const auto center = QPointF(320, 240);
    mouse(QEvent::MouseMove, {100, 100}, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::CrossCursor,
            "empty canvas must show a drawing cursor on the unified pointer surface");
    mouse(QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::ArrowCursor,
            "an object under the pointer must show the selection cursor");
    mouse(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    require(selected == QStringList{QStringLiteral("component")} && selection_clicks == 1,
            "plain click must select the component under the pointer");
    require(canvas.cursor().shape() == Qt::SizeAllCursor,
            "a selected object must advertise direct movement");

    mouse(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, center + QPointF(80, -40), Qt::NoButton, Qt::LeftButton);
    require(canvas.selectionBounds() && canvas.selectionBounds()->center().x() > center.x() + 70,
            "dragging a selected component must show a live move preview");
    mouse(QEvent::MouseButtonRelease, center + QPointF(80, -40),
          Qt::LeftButton, Qt::NoButton);
    require(move_requests == 1 && moved_ids == selected &&
                std::abs(moved_delta.x - 1.0) < 1e-9 &&
                std::abs(moved_delta.y - 0.5) < 1e-9,
            "selected component drag must request one model-space translation");

    const auto before_click = canvas.viewCenter();
    mouse(QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton, Qt::NoButton);
    require(selected.isEmpty() && point_clicks == 0 &&
                canvas.viewCenter().x == before_click.x &&
                canvas.viewCenter().y == before_click.y,
            "the first empty-canvas click after a selection must only clear the selection");

    mouse(QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {100, 100}, Qt::LeftButton, Qt::NoButton);
    require(point_clicks == 1 &&
                std::abs(clicked_point.x + 2.75) < 1e-9 &&
                std::abs(clicked_point.y - 1.75) < 1e-9 &&
                canvas.viewCenter().x == before_click.x &&
                canvas.viewCenter().y == before_click.y,
            "an empty-canvas click with no selection must place a drawing node without moving the view");

    const auto before_pan = canvas.viewCenter();
    mouse(QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, {140, 120}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {140, 120}, Qt::LeftButton, Qt::NoButton);
    require(canvas.viewCenter().x < before_pan.x - 0.4 &&
                canvas.viewCenter().y > before_pan.y + 0.2 &&
                point_clicks == 1 && move_requests == 1,
            "plain drag beginning on empty canvas must pan without placing a node");

    const auto before_space_pan = canvas.viewCenter();
    QKeyEvent space_press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &space_press);
    require(canvas.cursor().shape() == Qt::OpenHandCursor,
            "holding Space must advertise canvas navigation before dragging");
    mouse(QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, {140, 120}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {140, 120}, Qt::LeftButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::OpenHandCursor,
            "Space navigation must remain armed between drags while Space is held");
    QKeyEvent space_release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &space_release);
    require(canvas.cursor().shape() == Qt::CrossCursor,
            "releasing Space over empty canvas must restore the drawing cursor");
    require(canvas.viewCenter().x < before_space_pan.x - 0.4 &&
                canvas.viewCenter().y > before_space_pan.y + 0.2 &&
                point_clicks == 1,
            "Space-left-drag must pan without placing a drawing node");

    canvas.fitView();
    mouse(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    require(selected == QStringList{QStringLiteral("component")},
            "plain click must restore the component selection before toggle testing");
    mouse(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton,
          Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton,
          Qt::ControlModifier);
    require(selected.isEmpty(), "Ctrl-click must toggle the object in the retained selection");

    const auto before_unselected_pan = canvas.viewCenter();
    mouse(QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, center + QPointF(40, 20), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, center + QPointF(40, 20),
          Qt::LeftButton, Qt::NoButton);
    require((canvas.viewCenter().x != before_unselected_pan.x ||
             canvas.viewCenter().y != before_unselected_pan.y) &&
                move_requests == 1 && selected.isEmpty(),
            "dragging from an unselected object must pan instead of moving or selecting it");

    canvas.fitView();
    mouse(QEvent::MouseButtonPress, {390, 210}, Qt::LeftButton, Qt::LeftButton,
          Qt::ControlModifier);
    mouse(QEvent::MouseMove, {250, 270}, Qt::NoButton, Qt::LeftButton,
          Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, {250, 270}, Qt::LeftButton, Qt::NoButton,
          Qt::ControlModifier);
    require(marquee_requests == 1 && selected.contains(QStringLiteral("component")) &&
                move_requests == 1,
            "Ctrl-drag must marquee-select even when the window crosses an object");

    mouse(QEvent::MouseButtonPress, center, Qt::RightButton, Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, center, Qt::RightButton, Qt::NoButton);
    require(context_target == QStringLiteral("component"),
            "stationary right-click must identify the object under the pointer");

    const auto selection_clicks_before_double = selection_clicks;
    double_clicked.clear();
    mouse(QEvent::MouseButtonDblClick, center, Qt::LeftButton, Qt::LeftButton);
    require(double_clicked == QStringLiteral("component") &&
                selection_clicks == selection_clicks_before_double,
            "unmodified double-click must request properties without replaying selection");
    double_clicked.clear();
    mouse(QEvent::MouseButtonDblClick, center, Qt::LeftButton, Qt::LeftButton,
          Qt::ControlModifier);
    require(double_clicked.isEmpty() && selection_clicks == selection_clicks_before_double,
            "Ctrl-double-click must not open properties or replay selection");
    double_clicked.clear();
    mouse(QEvent::MouseButtonDblClick, {100, 100}, Qt::LeftButton, Qt::LeftButton);
    require(double_clicked.isEmpty(), "empty-canvas double-click must have no special action");

    canvas.setTool(CanvasTool::boundary);
    int points = 0;
    canvas.setPointClicked([&](Vec2) { ++points; });
    canvas.setBoundaryDraftPreview(BoundaryDraftPreview{});
    mouse(QEvent::MouseButtonDblClick, center, Qt::LeftButton, Qt::LeftButton);
    require(points == 0 && double_clicked.isEmpty(),
            "authoring double-click must suppress the second point and properties");
}

void test_selected_boundary_is_the_move_hit_target() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setTool(CanvasTool::select);

    CanvasEntity room{
        QStringLiteral("room"), QStringLiteral("measurement_boundary"),
        Boundary{
            Segment{{-1.0, -1.0}, {1.0, -1.0}, 0.0},
            Segment{{1.0, -1.0}, {1.0, 1.0}, 0.0},
            Segment{{1.0, 1.0}, {-1.0, 1.0}, 0.0},
            Segment{{-1.0, 1.0}, {-1.0, -1.0}, 0.0},
        }};
    CanvasEntity overlapping{
        QStringLiteral("overlap"), QStringLiteral("wall"),
        Boundary{Segment{{-0.2, 0.0}, {0.2, 0.0}, 0.0}}};
    canvas.setEntities({room, overlapping});

    QStringList selected;
    int move_requests = 0;
    int point_clicks = 0;
    QStringList moved_ids;
    Vec2 moved_delta{};
    canvas.setEntitySelectionClicked([&](QString id, bool) {
        selected = id.isEmpty() ? QStringList{} : QStringList{id};
        canvas.setSelectedIds(selected);
    });
    canvas.setEntitiesMoveRequested([&](QStringList ids, Vec2 delta) {
        ++move_requests;
        moved_ids = std::move(ids);
        moved_delta = delta;
        return true;
    });
    canvas.setPointClicked([&](Vec2) { ++point_clicks; });

    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };

    const QPointF top_edge{320, 160};
    const QPointF interior{320, 240};
    mouse(QEvent::MouseButtonPress, top_edge, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, top_edge, Qt::LeftButton, Qt::NoButton);
    require(selected == QStringList{QStringLiteral("room")},
            "fixture must select the room from its visible edge");
    const auto selection = canvas.selectionBounds();
    require(selection && selection->contains(interior),
            "visible selection boundary must contain the room interior");

    const QPointF empty_in_frame{270, 240};
    mouse(QEvent::MouseButtonPress, empty_in_frame, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, empty_in_frame, Qt::LeftButton, Qt::NoButton);
    require(selected.isEmpty() && point_clicks == 0,
            "an empty click within the selection frame must deselect before drawing");
    mouse(QEvent::MouseButtonPress, top_edge, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, top_edge, Qt::LeftButton, Qt::NoButton);
    require(selected == QStringList{QStringLiteral("room")},
            "the room must remain directly selectable after a deselection click");

    mouse(QEvent::MouseMove, interior, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::SizeAllCursor,
            "any point inside the selection boundary must advertise movement");
    const auto view_before_move = canvas.viewCenter();
    mouse(QEvent::MouseButtonPress, interior, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, interior + QPointF(40, -20), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, interior + QPointF(40, -20),
          Qt::LeftButton, Qt::NoButton);
    require(move_requests == 1 && moved_ids == QStringList{QStringLiteral("room")} &&
                std::abs(moved_delta.x - 0.5) < 1e-9 &&
                std::abs(moved_delta.y - 0.25) < 1e-9 &&
                canvas.viewCenter().x == view_before_move.x &&
                canvas.viewCenter().y == view_before_move.y,
            "drag inside the selection boundary must move the retained selection even over another object");

    const QPointF outside{560, 420};
    mouse(QEvent::MouseButtonPress, outside, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    require(selected.isEmpty() && point_clicks == 0,
            "first empty click outside a selected object must only clear selection");
    mouse(QEvent::MouseButtonPress, outside, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    require(point_clicks == 1,
            "empty click after deselection must be available for drawing input");

    const auto view_before_pan = canvas.viewCenter();
    mouse(QEvent::MouseButtonPress, interior, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, interior + QPointF(40, 20), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, interior + QPointF(40, 20),
          Qt::LeftButton, Qt::NoButton);
    require(move_requests == 1 &&
                (canvas.viewCenter().x != view_before_pan.x ||
                 canvas.viewCenter().y != view_before_pan.y),
            "the same drag must pan after the selection boundary is cleared");
}

void test_single_selection_transform_handles() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setTool(CanvasTool::select);
    canvas.setEntities({CanvasEntity{
        QStringLiteral("symbol"), QStringLiteral("annotation_symbol"),
        Boundary{Segment{{-0.5, -0.35}, {0.5, -0.35}, 0.0},
                 Segment{{0.5, -0.35}, {0.5, 0.35}, 0.0},
                 Segment{{0.5, 0.35}, {-0.5, 0.35}, 0.0},
                 Segment{{-0.5, 0.35}, {-0.5, -0.35}, 0.0}}}});
    canvas.setSelectedId(QStringLiteral("symbol"));
    canvas.setSelectionTransformEnabled(true, true);

    int transforms = 0;
    double scale = 1.0;
    double rotation = 0.0;
    canvas.setEntityTransformRequested([&](QString id, double next_scale, double next_rotation) {
        require(id == QStringLiteral("symbol"), "transform handle must retain selected identity");
        ++transforms;
        scale = next_scale;
        rotation = next_rotation;
        return true;
    });
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };

    const auto frame = canvas.selectionBounds();
    require(frame.has_value(), "selected symbol must expose a transform frame");
    const auto resize = frame->bottomRight();
    mouse(QEvent::MouseButtonPress, resize, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, resize + QPointF(48, 36), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, resize + QPointF(48, 36),
          Qt::LeftButton, Qt::NoButton);
    require(transforms == 1 && scale > 1.1 && std::abs(rotation) < 1e-9,
            "corner handle must commit a larger uniform scale");

    const auto rotate = QPointF(frame->center().x(), frame->top() - 24.0);
    const auto target = frame->center() + QPointF(80.0, 0.0);
    mouse(QEvent::MouseButtonPress, rotate, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    require(transforms == 2 && std::abs(rotation) > 0.5,
            "rotation handle must commit an angular transform");
}

void test_boundary_vertex_handles_preview_and_commit_once() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setTool(CanvasTool::select);
    CanvasEntity boundary{QStringLiteral("boundary"), QStringLiteral("measurement_boundary"),
        Boundary{Segment{{-1, -1}, {1, -1}, 0}, Segment{{1, -1}, {1, 1}, 0},
                 Segment{{1, 1}, {-1, 1}, 0}, Segment{{-1, 1}, {-1, -1}, 0}},
        0.08, true};
    boundary.vertex_handles = {
        {QStringLiteral("v0"), {-1, -1}, 41},
        {QStringLiteral("v1"), {1, -1}, 41},
        {QStringLiteral("v2"), {1, 1}, 41},
        {QStringLiteral("v3"), {-1, 1}, 41}};
    canvas.setEntities({boundary});

    int commits = 0;
    QString owner;
    QString vertex;
    Vec2 target{};
    std::uint64_t revision = 0;
    canvas.setBoundaryVertexMoveRequested(
        [&](QString next_owner, QString next_vertex, Vec2 next_target,
            std::uint64_t next_revision) {
            ++commits;
            owner = std::move(next_owner);
            vertex = std::move(next_vertex);
            target = next_target;
            revision = next_revision;
            return true;
        });
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const QPointF handle{400, 160}; // model (1,1) at the default transform.
    mouse(QEvent::MouseMove, handle, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::SizeAllCursor,
            "vertex handle must expose a generous direct-manipulation target");
    mouse(QEvent::MouseButtonPress, handle, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, handle + QPointF(40, 16), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, handle + QPointF(40, 16),
          Qt::LeftButton, Qt::NoButton);
    require(commits == 1 && owner == QStringLiteral("boundary") &&
                vertex == QStringLiteral("v2") && revision == 41 &&
                std::abs(target.x - 1.5) < 1e-9 && std::abs(target.y - 0.8) < 1e-9,
            "vertex drag must commit one absolute model point with its source revision");

    mouse(QEvent::MouseButtonPress, handle, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, handle + QPointF(-40, 24),
          Qt::LeftButton, Qt::NoButton);
    require(commits == 2 && std::abs(target.x - 0.5) < 1e-9 &&
                std::abs(target.y - 0.7) < 1e-9,
            "vertex drag must use the release point even without an intermediate move event");

    mouse(QEvent::MouseButtonPress, handle, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, handle + QPointF(-40, 0), Qt::NoButton, Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    mouse(QEvent::MouseButtonRelease, handle + QPointF(-40, 0),
          Qt::LeftButton, Qt::NoButton);
    require(commits == 2, "Escape must cancel a vertex drag without a document request");

    const auto with_handles = render(canvas, false);
    boundary.vertex_handles.clear();
    canvas.setEntities({boundary});
    require(!images_equal(with_handles, render(canvas, false)),
            "selected vertex handles must be visible on the interactive canvas");
    boundary.vertex_handles = {{QStringLiteral("v0"), {-1, -1}, 41}};
    canvas.setEntities({boundary});
    const auto output_with = render(canvas, true);
    boundary.vertex_handles.clear();
    canvas.setEntities({boundary});
    require(images_equal(output_with, render(canvas, true)),
            "vertex handles must never appear in print or export output");
}

void test_boundary_tool_uses_unified_selection_until_a_draft_starts() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setOverviewMapEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setTool(CanvasTool::boundary);
    canvas.setEntities({CanvasEntity{QStringLiteral("line"), QStringLiteral("wall"),
        Boundary{Segment{{-1.0, 0.0}, {1.0, 0.0}, 0.0}}}});
    int points = 0, moves = 0, finishes = 0;
    QString selected, context, properties;
    canvas.setPointClicked([&](Vec2) { ++points; });
    canvas.setEntitySelectionClicked([&](QString id, bool) {
        selected = id;
        canvas.setSelectedId(id);
    });
    canvas.setEntitiesMoveRequested([&](QStringList ids, Vec2 delta) {
        require(ids == QStringList{QStringLiteral("line")} && delta.x == 0.5,
                "boundary-tool selection drag must retain identity and model delta");
        ++moves;
        return true;
    });
    canvas.setRightClicked([&](Vec2, QString id) { context = id; });
    canvas.setEntityDoubleClicked([&](QString id) { properties = id; });
    canvas.setFinishRequested([&] { ++finishes; });
    const auto mouse = [&](QEvent::Type type, QPointF p, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, p, canvas.mapToGlobal(p.toPoint()), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const auto click = [&](QPointF p, Qt::MouseButton button = Qt::LeftButton) {
        mouse(QEvent::MouseButtonPress, p, button, button);
        mouse(QEvent::MouseButtonRelease, p, button, Qt::NoButton);
    };
    click({320, 240});
    require(selected == QStringLiteral("line") && points == 0,
            "idle boundary tool must select existing geometry without placing a node");
    // A thin line has a visible 44 px minimum frame; its interior away from
    // the painted stroke remains a usable touch target.
    const QPointF frame_interior{320, 259};
    mouse(QEvent::MouseMove, frame_interior, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::SizeAllCursor,
            "idle boundary tool must advertise movement throughout the selection frame");
    click(frame_interior, Qt::RightButton);
    require(context == selected, "right-click inside the selection frame must target the selection");
    mouse(QEvent::MouseButtonPress, frame_interior, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, frame_interior + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, frame_interior + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    require(moves == 1 && points == 0, "idle boundary tool must move a selected object from its frame");
    mouse(QEvent::MouseButtonDblClick, {320, 240}, Qt::LeftButton, Qt::LeftButton);
    require(properties == selected, "idle boundary tool must open quick properties");
    click({100, 100});
    require(selected.isEmpty() && points == 0, "first outside click must only deselect in boundary tool");
    click({100, 100});
    require(points == 1, "next empty click must start drawing in boundary tool");

    BoundaryDraftPreview draft;
    draft.anchor = Vec2{0.0, 0.0};
    draft.can_close_on_anchor = false;
    canvas.setBoundaryDraftPreview(draft);
    click({320, 240});
    require(points == 2 && selected.isEmpty(), "active draft must accept points over existing geometry");
    properties.clear();
    mouse(QEvent::MouseButtonDblClick, {320, 240}, Qt::LeftButton, Qt::LeftButton);
    require(properties.isEmpty(), "active draft must suppress quick properties");
    draft.can_close_on_anchor = true;
    canvas.setBoundaryDraftPreview(draft);
    canvas.setTool(CanvasTool::select);
    click({320, 240});
    require(finishes == 1 && points == 2 && selected.isEmpty(),
            "active draft anchor must close through the unified select surface even over geometry");
    canvas.setBoundaryDraftPreview(std::nullopt);
    for (const auto tool : {CanvasTool::wall, CanvasTool::sloped_wall}) {
        canvas.setTool(tool);
        click({320, 240});
    }
    require(points == 4 && selected.isEmpty() && finishes == 1,
            "wall tools must still accept authoring points over existing geometry");
}

void test_dimension_ticks_are_paper_space() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    CanvasEntity dimension{QStringLiteral("section-dimension"),
        QStringLiteral("section_overlay"),
        {{{-1.0, 0.0}, {1.0, 0.0}, 0.0}}};
    dimension.dimension_end_ticks = true;
    dimension.output_stroke_width_mm = 0.25;
    canvas.setEntities({dimension});

    const auto output = [&](double scale, int dpi,
                            std::optional<double> paper_pixels_per_mm = std::nullopt) {
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        image.setDotsPerMeterX(qRound(dpi / 0.0254));
        image.setDotsPerMeterY(qRound(dpi / 0.0254));
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas.renderSceneAt(painter, QRectF(image.rect()), scale, {0.0, 0.0},
                             Qt::white, paper_pixels_per_mm);
        return image;
    };
    const auto tick_span = [](const QImage& image, int endpoint_x) {
        int top = image.height(), bottom = -1;
        for (int x = endpoint_x - 3; x <= endpoint_x + 3; ++x) {
            for (int y = 0; y < image.height(); ++y) {
                if (image.pixelColor(x, y).lightness() < 160) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        return bottom >= top ? bottom - top + 1 : 0;
    };
    const auto span = [&](const QImage& image, double scale) {
        return tick_span(image, qRound(image.width() / 2.0 - scale));
    };

    const auto dpi_96 = span(output(100.0, 96), 100.0);
    const auto dpi_192 = span(output(100.0, 192), 100.0);
    require(dpi_96 >= 8 && std::abs(dpi_192 - 2 * dpi_96) <= 3,
            "dimension ticks must preserve their 2.5 mm physical size across output DPI");
    const auto fitted_50 = span(output(50.0, 96, 4.0), 50.0);
    const auto fitted_200 = span(output(200.0, 96, 4.0), 200.0);
    require(fitted_50 >= 9 && std::abs(fitted_50 - fitted_200) <= 1,
            "dimension tick pixels must be independent of model-to-output scale");
    const auto fitted_double = span(output(100.0, 96, 8.0), 100.0);
    require(std::abs(fitted_double - 2 * fitted_50) <= 3,
            "fitted sheet paper scale must control dimension tick pixels");
    require(span(output(100.0, 192, 4.0), 100.0) ==
                span(output(100.0, 96, 4.0), 100.0),
            "explicit fitted sheet scale must override output-device DPI for ticks");
}

void test_dimension_ticks_respect_angular_geometry() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    CanvasEntity dimension{QStringLiteral("dimension"), QStringLiteral("dimension_line"),
        {{{0, 0}, {1, 0}, 0}, {{1, 0}, {0, 1}, std::numbers::pi / 2},
         {{0, 1}, {0, 0}, 0}}};
    for (const auto output : {false, true}) {
        dimension.dimension_end_ticks = false;
        canvas.setEntities({dimension});
        canvas.fitView();
        const auto without_ticks = render(canvas, output);
        dimension.dimension_end_ticks = true;
        canvas.setEntities({dimension});
        require(images_equal(without_ticks, render(canvas, output)),
                "angular dimensions must ignore stale endpoint ticks in screen and output");
    }
    dimension.segments = {{{0, 0}, {0, 1}, 0}, {{2, 0}, {2, 1}, 0},
                          {{0, 1}, {2, 1}, 0}};
    for (const auto output : {false, true}) {
        dimension.dimension_end_ticks = false;
        canvas.setEntities({dimension});
        canvas.fitView();
        const auto without_ticks = render(canvas, output);
        dimension.dimension_end_ticks = true;
        canvas.setEntities({dimension});
        require(!images_equal(without_ticks, render(canvas, output)),
                "linear dimensions must retain endpoint ticks in screen and output");
    }
}

void test_dark_canvas_semantic_strokes_and_overrides() {
    PlanCanvas canvas;
    canvas.resize(640, 480);
    canvas.setGridEnabled(false);
    canvas.setOverviewMapEnabled(false);
    CanvasEntity entity{QStringLiteral("geometry"), QStringLiteral("wall"),
                         {{{-1, 0}, {1, 0}, 0}}};
    const auto capture = [&](QColor surface, bool output = false) {
        canvas.setEntities({entity});
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        canvas.renderScene(painter, QRectF(image.rect()), output, surface);
        return image;
    };
    const auto semantic_dark = capture(background);
    entity.stroke_color = QColor(35, 77, 113);
    require(images_equal(semantic_dark, capture(background)),
            "light semantic wall stroke must not suppress dark theme contrast");
    entity.stroke_color = Qt::black;
    require(images_equal(semantic_dark, capture(background)),
            "default black stroke must use semantic dark contrast");
    entity.stroke_color = QColor(220, 35, 90);
    require(!images_equal(semantic_dark, capture(background)),
            "intentional custom stroke must survive dark mode");
    const auto custom_dark = capture(background);
    const auto custom_light = capture(Qt::white);
    const auto custom_output = capture(Qt::white, true);
    entity.dark_stroke_color = QColor(143, 198, 245);
    require(images_equal(semantic_dark, capture(background)),
            "explicit dark stroke must take precedence on dark interactive canvases");
    require(images_equal(custom_light, capture(Qt::white)) &&
                images_equal(custom_output, capture(Qt::white, true)),
            "dark stroke must not change light canvases or printed output");
    entity.selected = true;
    const auto selected = capture(background);
    entity.dark_stroke_color = {};
    require(images_equal(selected, capture(background)) &&
                !images_equal(custom_dark, selected),
            "selection color must take precedence over both stroke overrides");
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font_id >= 0, "bundled capture font must load");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "bundled capture font must expose a family");
        QApplication::setFont(QFont(families.front(), 10));
        const QFontMetrics metrics(QApplication::font());
        for (const auto character : QStringLiteral("2.00 m Draft boundary • place the next dimension")) {
            require(metrics.inFont(character), "capture font must contain each rendered character");
        }
        test_dimension_ticks_are_paper_space();
        test_dimension_ticks_respect_angular_geometry();
        test_dark_canvas_semantic_strokes_and_overrides();
        test_selection_frame_for_styled_geometry();
        test_boundary_tool_uses_unified_selection_until_a_draft_starts();
        test_direct_canvas_manipulation_contract();
        test_selected_boundary_is_the_move_hit_target();
        test_single_selection_transform_handles();
        test_boundary_vertex_handles_preview_and_commit_once();
        test_mouse_gesture_contract();
        test_boundary_draft_rendering_and_history();
        test_request_to_paint_telemetry();
        test_effective_cursor_matches_click();
        test_cursor_measurement_readout_is_transient_and_contextual();
        test_overview_map_navigation();
        test_site_scale_fit();
        test_arc_render_orientation();
        test_analytic_arc_fit_bounds();
        test_reference_grid_labels_render_in_screen_and_output();
        test_closed_entity_hatching_and_open_path_safety();
        test_explicit_output_excludes_interactive_state();
        test_output_stroke_width_is_paper_space();
        test_paper_label_style_and_hit_testing();
        std::cout << "Boundary canvas tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_canvas_tests: " << error.what() << '\n';
        return 1;
    }
}
