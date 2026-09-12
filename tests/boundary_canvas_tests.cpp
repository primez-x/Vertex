#include "../src/desktop/plan_canvas.hpp"

#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

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

QRect bright_pixel_bounds(const QImage& image, QRect region) {
    region = region.intersected(image.rect());
    int left = region.right() + 1;
    int top = region.bottom() + 1;
    int right = region.left() - 1;
    int bottom = region.top() - 1;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
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
    const auto normal_label_bounds = bright_pixel_bounds(screen_draft, label_region);
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
    const auto zoomed_grab = canvas.grab();
    require(!zoomed_grab.isNull(), "150 percent boundary draft capture must be available");
    save_capture(capture_directory, QStringLiteral("boundary-draft-150.png"), zoomed_grab.toImage());
    const auto zoomed_output = render(canvas, true);
    require(images_equal(output_before, zoomed_output),
            "fit-to-content output must stay unchanged after interactive zoom");

    // The label font is screen-space text, so zooming changes its position but
    // not its pixel footprint.
    const auto zoom_label_center = QPointF(canvas.rect().center().x(),
                                           canvas.rect().center().y() - 1.5 *
                                               (static_cast<double>(canvas.height()) /
                                                (4.0 + 2.0 * (4.0 * 0.12 + 0.25))) * 1.5);
    const auto zoom_label_region = QRectF(zoom_label_center.x() - 60.0,
                                          zoom_label_center.y() - 24.0, 120.0, 48.0)
                                             .toAlignedRect();
    const auto zoom_label_bounds = bright_pixel_bounds(zoomed_screen, zoom_label_region);
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
        QMouseEvent press(QEvent::MouseButtonPress, position, position, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        require(placed && preview && placed->x == preview->x && placed->y == preview->y,
                "placed point must exactly match the effective cursor");
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
    // A press can arrive without an intervening move (for example pen input).
    // The coordinate shown to the tool must already agree inside its callback.
    canvas.setPointClicked([&](Vec2 point) {
        require(preview && preview->x == point.x && preview->y == point.y,
                "press must publish its effective cursor before placing the point");
        placed = point;
    });
    const auto next_position = QRectF(canvas.rect()).center() + QPointF(-47.0, -53.0);
    QMouseEvent next_press(QEvent::MouseButtonPress, next_position, next_position,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &next_press);
    require(placed && placed->x == -0.5 && placed->y == 0.75,
            "press without preceding motion must use its own snapped coordinates");
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
        test_boundary_draft_rendering_and_history();
        test_effective_cursor_matches_click();
        test_overview_map_navigation();
        test_site_scale_fit();
        test_arc_render_orientation();
        test_analytic_arc_fit_bounds();
        test_paper_label_style_and_hit_testing();
        std::cout << "Boundary canvas tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_canvas_tests: " << error.what() << '\n';
        return 1;
    }
}
