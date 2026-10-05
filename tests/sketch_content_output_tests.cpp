#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/sketch_content_bounds.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QPainter>
#include <QPicture>
#include <QSvgRenderer>

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
CanvasEntity line(QString id, Vec2 start, Vec2 end, double stroke = .05) {
    CanvasEntity result;
    result.id = std::move(id);
    result.type = "annotation_line";
    result.segments = {{start, end, 0}};
    result.stroke_width_metres = stroke;
    return result;
}
CanvasSketchContentRecording record(PlanCanvas& canvas) {
    QString reason;
    auto result = canvas.recordSketchContent(80, &reason);
    require(result.has_value(), qPrintable(reason));
    return std::move(*result);
}
QImage replay(CanvasSketchContentRecording value, int margin = 16) {
    const auto rect = value.ink_bounds.toAlignedRect();
    QImage image(rect.width() + 2 * margin, rect.height() + 2 * margin,
                 QImage::Format_ARGB32_Premultiplied);
    image.setDotsPerMeterX(qRound(value.picture.logicalDpiX() / .0254));
    image.setDotsPerMeterY(qRound(value.picture.logicalDpiY() / .0254));
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.translate(margin - rect.left(), margin - rect.top());
    require(value.picture.play(&painter), "recorded content replays");
    require(painter.end(), "replay painter closes");
    return image;
}
QByteArray image_digest(const QImage& image) {
    return QCryptographicHash::hash(QByteArrayView(
        reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
        QCryptographicHash::Sha256);
}
void require_contained_ink(const CanvasSketchContentRecording& value) {
    constexpr int margin = 16;
    const auto image = replay(value, margin);
    bool found = false;
    int minx = image.width(), miny = image.height(), maxx = -1, maxy = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!qAlpha(image.pixel(x, y))) continue;
            found = true;
            minx = std::min(minx, x); miny = std::min(miny, y);
            maxx = std::max(maxx, x); maxy = std::max(maxy, y);
        }
    }
    require(found, "recording includes committed painted ink");
    if (minx < margin || miny < margin || maxx >= image.width() - margin || maxy >= image.height() - margin)
        throw std::runtime_error("paint outside crop: actual " + std::to_string(minx) + "," + std::to_string(miny) +
            " to " + std::to_string(maxx) + "," + std::to_string(maxy) + " in " +
            std::to_string(image.width()) + "x" + std::to_string(image.height()) + "; bounds " +
            std::to_string(value.ink_bounds.x()) + "," + std::to_string(value.ink_bounds.y()) + "," +
            std::to_string(value.ink_bounds.width()) + "," + std::to_string(value.ink_bounds.height()));
}
void exclusions_and_zoom() {
    PlanCanvas canvas;
    canvas.resize(1000, 700);
    canvas.setEntities({line("stroke", {-2, 0}, {2, 0}, .1)});
    CanvasLabel text;
    text.id = "text"; text.position = {0, 1}; text.text = "Committed drawing";
    text.show_background = false;
    text.automatic_linear_placement = CanvasLinearLabelPlacement{
        {{-2, 0}, {2, 0}, 0}, {0, 1}, .2};
    canvas.setLabels({text});
    auto initial = record(canvas);
    require_contained_ink(initial);
    const auto digest = image_digest(replay(initial));

    CanvasReference reference;
    reference.id = "underlay"; reference.image = QImage(50, 50, QImage::Format_ARGB32);
    reference.image.fill(Qt::magenta); reference.position = {300, 400}; reference.scale = 20;
    canvas.setReference(reference);
    CanvasReferenceGrid grid;
    grid.id = "reference-grid"; grid.x_label = "AXIS"; grid.y_label = "GRID";
    grid.lines = {{{0, -1}, {0, 1.075}, ReferenceGridAxis::x, 1},
                  {{-100, 200}, {300, 200}, ReferenceGridAxis::y, 2}};
    canvas.setReferenceGrids({grid});
    canvas.setGridEnabled(true);
    canvas.setBoundaryPreview({{-100, -100}, {100, 100}});
    canvas.setWallPreview(std::pair{Vec2{-30, 30}, Vec2{30, 30}});
    BoundaryDraftPreview draft;
    draft.segments = {{{-40, -20}, {40, -20}, 0}};
    draft.labels = {{{20, 20}, "Draft text", 0}};
    draft.anchor = Vec2{90, 90}; draft.pen_position = Vec2{91, 91};
    canvas.setBoundaryDraftPreview(draft);
    canvas.setDrawingWitnesses({{{{-200, -100}, {200, -100}, 0}, true, true, "Guide", "Command"}});
    canvas.setSelectedIds({"stroke", "text"});
    canvas.setSelectionCaption("Selected drawing");
    canvas.setSketchCompositionGuideEnabled(true);
    require(canvas.sketchCompositionGuideEnabled() && canvas.sketchCompositionGuideRect(),
            "optional composition guide maps recorded crop to canvas");
    canvas.setViewTransform({150, -40}, 2500);
    const auto changed = record(canvas);
    require(changed.ink_bounds == initial.ink_bounds &&
            image_digest(replay(changed)) == digest,
            "references, grids, drafts, guides, selection, pan and zoom are excluded");
    canvas.setSketchCompositionGuideEnabled(false);
    require(!canvas.sketchCompositionGuideRect(), "disabled guide has no visible frame");
    require(image_digest(replay(record(canvas))) == digest, "guide never enters recorded output");

    // Existing explicitly transformed output keeps its underlay/grid contract.
    QImage normal(600, 400, QImage::Format_ARGB32_Premultiplied);
    normal.fill(Qt::white);
    QPainter painter(&normal);
    canvas.renderSceneAt(painter, normal.rect(), 5, {300, 400}, Qt::white);
    painter.end();
    bool magenta = false;
    for (int y = 0; y < normal.height(); ++y)
        for (int x = 0; x < normal.width(); ++x)
            if (normal.pixelColor(x, y) == QColor(Qt::magenta)) magenta = true;
    require(magenta, "existing output still paints reference images");
}
void painted_extents() {
    PlanCanvas canvas;
    auto curve = line("curve", {-3, 0}, {3, 0}, .6);
    curve.segments.front().sweep_radians = std::numbers::pi;
    auto dimension = line("dimension", {-4, -1}, {4, -1}, .01);
    dimension.type = "dimension_line"; dimension.dimension_end_ticks = true;
    dimension.output_stroke_width_mm = 1.5;
    canvas.setEntities({curve, dimension});
    CanvasLabel text;
    text.id = "large"; text.text = "Large rotated text\nwith a callout";
    text.position = {5, 4}; text.rotation_radians = .8; text.paper_height_mm = 20;
    text.show_background = true; text.leader_start = Vec2{-8, -6};
    canvas.setLabels({text});
    const auto result = record(canvas);
    require_contained_ink(result);
    require(result.ink_bounds.width() > 800 && result.ink_bounds.height() > 700,
            "crop includes large rotated font, leader, curve and paper ticks");

    CanvasEntity svg;
    svg.id = "svg"; svg.type = "symbol";
    CanvasSvgSymbol symbol;
    symbol.document = QByteArrayLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
        "<path d='M 5 10 C 10 95 95 90 90 5' fill='none' stroke='blue' stroke-width='8'/>"
        "</svg>");
    symbol.view_box = {0, 0, 100, 100}; symbol.footprint_view_box = symbol.view_box;
    symbol.position = {0, 0}; symbol.width_metres = 3; symbol.depth_metres = 2;
    symbol.rotation_radians = .6;
    svg.svg_symbol = symbol;
    canvas.setEntities({svg}); canvas.setLabels({});
    const auto artwork = record(canvas);
    require_contained_ink(artwork);
    require(artwork.ink_bounds.width() > 100 && artwork.ink_bounds.height() > 100,
            "SVG artwork with no fallback segments contributes painted bounds");
}
void origin_and_rejections() {
    PlanCanvas canvas;
    QString reason;
    require(!canvas.recordSketchContent(80, &reason) && !reason.isEmpty(),
            "empty committed drawing rejects with a diagnostic");
    CanvasReference underlay;
    underlay.image = QImage(30, 30, QImage::Format_ARGB32); underlay.image.fill(Qt::red);
    canvas.setReference(underlay);
    require(!canvas.recordSketchContent(80, &reason), "reference-only scene remains empty");
    canvas.setEntities({line("local", {-2, -1}, {2, 1})});
    const auto local = record(canvas);
    canvas.setEntities({line("distant", {1e9 - 2, 1e9 - 1}, {1e9 + 2, 1e9 + 1})});
    const auto distant = record(canvas);
    require(local.ink_bounds == distant.ink_bounds &&
            image_digest(replay(local)) == image_digest(replay(distant)),
            "stable recording origin preserves drawings far from world zero");
    for (const double scale : {0., -1., 1e-12, 1e12,
                             std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
        require(!canvas.recordSketchContent(scale, &reason) && !reason.isEmpty(),
                "invalid or unsupported output scale rejects");
    canvas.setEntities({line("overlarge", {-1e12, 0}, {1e12, 0})});
    require(!canvas.recordSketchContent(80, &reason) && !reason.isEmpty(),
            "overlarge recording rejects instead of silently clipping");
    canvas.setEntities({line("invalid", {0, 0}, {std::numeric_limits<double>::quiet_NaN(), 1})});
    require(!canvas.recordSketchContent(80, &reason), "invalid retained geometry rejects");
    canvas.setEntities({});
    CanvasLabel label; label.text = "Invalid font size";
    label.paper_height_mm = std::numeric_limits<double>::infinity();
    canvas.setLabels({label});
    require(!canvas.recordSketchContent(80, &reason), "nonfinite committed text presentation rejects");
    label.paper_height_mm = 0; label.scale = 1e200; label.text_height_metres = 1e200;
    canvas.setLabels({label});
    require(!canvas.recordSketchContent(80, &reason), "overflowing model font admission rejects before layout");
}
void text_and_subpixel_strokes() {
    PlanCanvas canvas;
    CanvasLabel label;
    label.id = "text-only"; label.text = QString::fromUtf8("Italic overhang fj — العربية\nSecond line");
    label.show_background = false; label.paper_height_mm = 8; label.italic = true;
    label.rotation_radians = -.7; label.position = {8, 7};
    canvas.setLabels({label});
    const auto only_text = record(canvas);
    std::cout << "text-only\n" << std::flush;
    require_contained_ink(only_text);
    require(only_text.ink_bounds.width() > 150 && only_text.ink_bounds.height() > 50,
            "standalone backgroundless rotated shaped text contributes crop");
    canvas.setEntities({line("small-stroke", {0, 0}, {.05, 0}, .002)});
    const auto outside = record(canvas);
    std::cout << "text-outside\n" << std::flush;
    require_contained_ink(outside);
    require(outside.ink_bounds.width() > 600 && outside.ink_bounds.height() > 450,
            "backgroundless label beyond all model strokes remains inside crop");
    canvas.setLabels({});
    for (const auto endpoint : {Vec2{2, 0}, Vec2{0, 2}}) {
        auto hairline = line("hairline", {0, 0}, endpoint, 0);
        hairline.output_stroke_width_mm = .0001;
        canvas.setEntities({hairline});
        const auto result = record(canvas);
        std::cout << "fractional-cosmetic " << endpoint.x << "," << endpoint.y << "\n" << std::flush;
        require_contained_ink(result);
        require(result.ink_bounds.width() >= 2 && result.ink_bounds.height() >= 2,
                "horizontal and vertical fractional cosmetic strokes have nonempty crop");
        hairline.output_stroke_width_mm = 0; hairline.stroke_width_metres = .002;
        canvas.setEntities({hairline});
        std::cout << "thin-model\n" << std::flush;
        require_contained_ink(record(canvas));
        hairline.output_stroke_width_mm = 6;
        canvas.setEntities({hairline});
        const auto wide = record(canvas);
        std::cout << "wide-cosmetic\n" << std::flush;
        require_contained_ink(wide);
        const auto cross_span = endpoint.x == 0 ? wide.ink_bounds.width() : wide.ink_bounds.height();
        require(cross_span >= 6 * wide.pixels_per_mm,
                "wide cosmetic paper stroke is measured after model transform");
    }
}
void forwarding_state_and_primitives() {
    CanvasSketchContentRecording value;
    SketchContentBoundsDevice device(value.picture);
    QPainter painter(&device);
    require(painter.isActive(), "forwarding measurement recorder starts");
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, .3, Qt::DashLine, Qt::SquareCap, Qt::MiterJoin));
    painter.scale(10, -8);
    const QPointF points[] = {{-3, 0}, {0, 3}, {3, 0}};
    painter.drawPolyline(points, 3);
    painter.save();
    painter.setClipRect(QRectF(-1, -1, 2, 2));
    painter.setBrush(Qt::red); painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(-20, -20, 40, 40));
    painter.restore();
    painter.setPen(QPen(Qt::blue, 1, Qt::SolidLine, Qt::RoundCap));
    painter.drawPoint(QPointF(5, 0));
    painter.drawRect(QRectF(-5, -2, 1, 1));
    require(painter.end() && device.valid() && device.inkBounds(), "forwarding recorder finishes valid");
    value.ink_bounds = *device.inkBounds();
    value.picture.setBoundingRect(value.ink_bounds.toAlignedRect());
    require(value.ink_bounds.width() > 100 && value.ink_bounds.width() < 150,
            "aggregate clips and restored drawing state bound actual primitives");
    require_contained_ink(value);
}
void decorated_svg_matches_direct_paint() {
    QSvgRenderer renderer(QByteArrayLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 280 120'>"
        "<text x='10' y='70' font-family='sans-serif' font-size='28' "
        "text-decoration='underline' fill='blue' opacity='.45'>Decorated text</text></svg>"));
    require(renderer.isValid(), "decorated SVG fixture is valid");
    CanvasSketchContentRecording value;
    SketchContentBoundsDevice device(value.picture);
    QPainter painter(&device);
    renderer.render(&painter, QRectF(0, 0, 280, 120));
    require(painter.end() && device.valid() && device.inkBounds(), "decorated SVG records");
    value.ink_bounds = *device.inkBounds();
    value.picture.setBoundingRect(value.ink_bounds.toAlignedRect());
    const auto actual = replay(value);
    QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied);
    expected.setDotsPerMeterX(actual.dotsPerMeterX()); expected.setDotsPerMeterY(actual.dotsPerMeterY());
    expected.fill(Qt::transparent);
    QPainter direct(&expected);
    const auto rect = value.ink_bounds.toAlignedRect();
    direct.translate(16 - rect.left(), 16 - rect.top());
    renderer.render(&direct, QRectF(0, 0, 280, 120));
    require(direct.end(), "direct decorated SVG render ends");
    require(image_digest(actual) == image_digest(expected),
            "recording preserves decorated SVG shape and opacity without duplicate decoration");
    require_contained_ink(value);
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
            "bundled application font loads");
    application.setFont(QFont(QStringLiteral("Inter"), 10));
    try {
        std::cout << "exclusions_and_zoom\n" << std::flush; exclusions_and_zoom();
        std::cout << "painted_extents\n" << std::flush; painted_extents();
        std::cout << "origin_and_rejections\n" << std::flush; origin_and_rejections();
        std::cout << "text_and_subpixel_strokes\n" << std::flush; text_and_subpixel_strokes();
        std::cout << "forwarding_state_and_primitives\n" << std::flush; forwarding_state_and_primitives();
        std::cout << "decorated_svg_matches_direct_paint\n" << std::flush; decorated_svg_matches_direct_paint();
        std::cout << "Sketch content output tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
