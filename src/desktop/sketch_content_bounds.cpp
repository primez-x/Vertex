#include "sketch_content_bounds.hpp"

#include <QFontMetricsF>
#include <QImage>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPicture>
#include <QPixmap>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace sketch::desktop {
class SketchContentBoundsDevice::Engine final : public QPaintEngine {
public:
    explicit Engine(QPicture& picture) : QPaintEngine(AllFeatures), m_picture(picture) {}
    Type type() const override { return User; }
    bool begin(QPaintDevice* device) override {
        setPaintDevice(device);
        m_bounds.reset(); m_valid = true;
        const bool active = m_forward.begin(&m_picture);
        setActive(active);
        return active;
    }
    bool end() override {
        const bool success = m_forward.end();
        setActive(false);
        return success;
    }
    void updateState(const QPaintEngineState& incoming) override {
        const auto dirty = incoming.state();
        if (dirty & DirtyPen) { m_pen = incoming.pen(); m_forward.setPen(m_pen); }
        if (dirty & DirtyBrush) { m_brush = incoming.brush(); m_forward.setBrush(m_brush); }
        if (dirty & DirtyBrushOrigin) m_forward.setBrushOrigin(incoming.brushOrigin());
        if (dirty & DirtyFont) m_forward.setFont(incoming.font());
        if (dirty & DirtyBackground) m_forward.setBackground(incoming.backgroundBrush());
        if (dirty & DirtyBackgroundMode) m_forward.setBackgroundMode(incoming.backgroundMode());
        if (dirty & DirtyTransform) {
            m_transform = incoming.transform();
            m_forward.setTransform(m_transform);
            if (!m_transform.isAffine()) m_valid = false;
        }
        if (dirty & DirtyOpacity) { m_opacity = incoming.opacity(); m_forward.setOpacity(m_opacity); }
        if (dirty & DirtyCompositionMode) m_forward.setCompositionMode(incoming.compositionMode());
        if (dirty & DirtyHints) {
            m_forward.setRenderHints(m_forward.renderHints(), false);
            m_forward.setRenderHints(incoming.renderHints());
        }
        if (dirty & (DirtyClipPath | DirtyClipRegion | DirtyClipEnabled)) {
            if (dirty & (DirtyClipPath | DirtyClipRegion)) {
                // Restore can replay temporary clip states before the painter
                // exposes its final state. Consume those engine operations in
                // device coordinates, including an explicit NoClip reset.
                const auto operation = incoming.clipOperation();
                if (operation == Qt::NoClip) {
                    m_clip = {};
                    m_has_clip = false;
                    m_clip_enabled = false;
                } else {
                    QPainterPath path;
                    if (dirty & DirtyClipPath) path = incoming.clipPath();
                    else path.addRegion(incoming.clipRegion());
                    path = incoming.transform().map(path);
                    m_clip = operation == Qt::IntersectClip && m_clip_enabled
                        ? m_clip.intersected(path) : path;
                    m_has_clip = true;
                    m_clip_enabled = true;
                }
            }
            if (dirty & DirtyClipEnabled) m_clip_enabled = m_has_clip && incoming.isClipEnabled();
            m_forward.setClipping(false);
            if (m_clip_enabled) {
                m_forward.setTransform(QTransform{});
                m_forward.setClipPath(m_clip, Qt::ReplaceClip);
                m_forward.setTransform(m_transform);
            }
            m_forward.setClipping(m_clip_enabled);
        }
    }
    void drawPath(const QPainterPath& path) override {
        measure(path, true); m_forward.drawPath(path);
    }
    void drawRects(const QRectF* rects, int count) override {
        for (int i = 0; i < count; ++i) { QPainterPath path; path.addRect(rects[i]); measure(path, true); }
        m_forward.drawRects(rects, count);
    }
    void drawRects(const QRect* rects, int count) override {
        for (int i = 0; i < count; ++i) { QPainterPath path; path.addRect(QRectF(rects[i])); measure(path, true); }
        m_forward.drawRects(rects, count);
    }
    void drawLines(const QLineF* lines, int count) override {
        for (int i = 0; i < count; ++i) {
            QPainterPath path; path.moveTo(lines[i].p1()); path.lineTo(lines[i].p2()); measure(path, false);
        }
        m_forward.drawLines(lines, count);
    }
    void drawLines(const QLine* lines, int count) override {
        for (int i = 0; i < count; ++i) {
            QPainterPath path; path.moveTo(lines[i].p1()); path.lineTo(lines[i].p2()); measure(path, false);
        }
        m_forward.drawLines(lines, count);
    }
    void drawEllipse(const QRectF& rect) override {
        QPainterPath path; path.addEllipse(rect); measure(path, true); m_forward.drawEllipse(rect);
    }
    void drawEllipse(const QRect& rect) override { drawEllipse(QRectF(rect)); }
    void drawPolygon(const QPointF* points, int count, PolygonDrawMode mode) override {
        if (count <= 0) return;
        QPainterPath path; path.moveTo(points[0]);
        for (int i = 1; i < count; ++i) path.lineTo(points[i]);
        if (mode != PolylineMode) path.closeSubpath();
        path.setFillRule(mode == OddEvenMode ? Qt::OddEvenFill : Qt::WindingFill);
        measure(path, mode != PolylineMode);
        if (mode == PolylineMode) m_forward.drawPolyline(points, count);
        else m_forward.drawPolygon(points, count, path.fillRule());
    }
    void drawPolygon(const QPoint* points, int count, PolygonDrawMode mode) override {
        QPolygonF converted; converted.reserve(count);
        for (int i = 0; i < count; ++i) converted.append(points[i]);
        drawPolygon(converted.constData(), count, mode);
    }
    void drawPoints(const QPointF* points, int count) override {
        if (visible(m_pen.brush()) && m_pen.style() != Qt::NoPen) {
            const bool cosmetic = m_pen.isCosmetic() || m_pen.widthF() == 0;
            const auto width = m_pen.widthF() == 0 ? 1.0 : m_pen.widthF();
            for (int i = 0; i < count; ++i) {
                const auto point = cosmetic ? m_transform.map(points[i]) : points[i];
                QPainterPath path;
                const QRectF rect(point.x() - width / 2, point.y() - width / 2, width, width);
                if (m_pen.capStyle() == Qt::RoundCap) path.addEllipse(rect);
                else path.addRect(rect);
                include(cosmetic ? path : m_transform.map(path));
            }
        }
        m_forward.drawPoints(points, count);
    }
    void drawPoints(const QPoint* points, int count) override {
        QPolygonF converted; converted.reserve(count);
        for (int i = 0; i < count; ++i) converted.append(points[i]);
        drawPoints(converted.constData(), count);
    }
    void drawTextItem(const QPointF& position, const QTextItem& item) override {
        if (visible(m_pen.brush()) && m_pen.style() != Qt::NoPen && !item.text().isEmpty()) {
            const QFontMetricsF metrics(item.font(), paintDevice());
            // Preserve side bearings and italic/combining-mark overhangs. The
            // shaped run metrics also cover fallback fonts and decorations.
            auto bounds = metrics.tightBoundingRect(item.text());
            // Font metrics can omit fallback/italic glyph outlines. Resolve
            // their actual vector contour as well as run advance/line metrics;
            // it is a bounds input only, not a replacement for PDF text.
            QPainterPath glyph_outline;
            glyph_outline.addText(QPointF{}, item.font(), item.text());
            bounds = bounds.united(glyph_outline.boundingRect());
            bounds = bounds.united(QRectF(0, -item.ascent(), item.width(), item.ascent() + item.descent()));
            // Justification changes the run width independently of the raw
            // string metrics. Retain its final glyph's italic overhang too.
            const auto right_overhang = std::max(0.0, -metrics.rightBearing(item.text().back()));
            bounds.setRight(std::max(bounds.right(), item.width() + right_overhang));
            if (item.renderFlags() & (QTextItem::Underline | QTextItem::Overline | QTextItem::StrikeOut))
                bounds = bounds.adjusted(-metrics.lineWidth(), -metrics.lineWidth(),
                                          metrics.lineWidth(), metrics.lineWidth());
            QPainterPath path; path.addRect(bounds.translated(position));
            include(m_transform.map(path));
        }
        // The outer QPainter already emits decorations and opaque text
        // backgrounds. Forward its shaped item directly after synchronizing
        // the recording painter state, avoiding a second decoration pass.
        auto* engine = m_forward.paintEngine();
        engine->syncState();
        engine->drawTextItem(position, item);
    }
    void drawPixmap(const QRectF& rect, const QPixmap& pixmap, const QRectF& source) override {
        if (!pixmap.isNull() && m_opacity > 0) includeRect(rect);
        m_forward.drawPixmap(rect, pixmap, source);
    }
    void drawImage(const QRectF& rect, const QImage& image, const QRectF& source,
                   Qt::ImageConversionFlags flags) override {
        if (!image.isNull() && m_opacity > 0) includeRect(rect);
        m_forward.drawImage(rect, image, source, flags);
    }
    void drawTiledPixmap(const QRectF& rect, const QPixmap& pixmap, const QPointF& offset) override {
        if (!pixmap.isNull() && m_opacity > 0) includeRect(rect);
        m_forward.drawTiledPixmap(rect, pixmap, offset);
    }
    std::optional<QRectF> bounds() const {
        // One recorder pixel covers antialiasing support and font hinting.
        return m_bounds ? std::optional{m_bounds->adjusted(-1, -1, 1, 1)} : std::nullopt;
    }
    bool valid() const { return m_valid; }

private:
    bool visible(const QBrush& brush) const {
        return m_opacity > 0 && brush.style() != Qt::NoBrush &&
            (brush.gradient() || brush.style() == Qt::TexturePattern || brush.color().alpha() > 0);
    }
    void includeRect(QRectF rect) {
        QPainterPath path; path.addRect(rect); include(m_transform.map(path));
    }
    void include(QPainterPath path) {
        const auto original = path.boundingRect();
        if (!std::isfinite(original.left()) || !std::isfinite(original.right()) ||
            !std::isfinite(original.top()) || !std::isfinite(original.bottom()) ||
            std::max({std::abs(original.left()), std::abs(original.right()),
                      std::abs(original.top()), std::abs(original.bottom())}) > 1'000'000) {
            m_valid = false; return;
        }
        if (m_clip_enabled) path = path.intersected(m_clip);
        if (path.isEmpty()) return;
        const auto rect = path.boundingRect();
        if (rect.isEmpty()) return;
        m_bounds = m_bounds ? m_bounds->united(rect) : rect;
    }
    void measure(const QPainterPath& path, bool fill) {
        if (fill && visible(m_brush)) include(m_transform.map(path));
        if (m_pen.style() == Qt::NoPen || !visible(m_pen.brush()) || path.isEmpty()) return;
        QPainterPathStroker stroker;
        stroker.setCapStyle(m_pen.capStyle()); stroker.setJoinStyle(m_pen.joinStyle());
        stroker.setMiterLimit(m_pen.miterLimit());
        stroker.setWidth(m_pen.widthF() == 0 ? 1.0 : m_pen.widthF());
        if (m_pen.style() != Qt::SolidLine) {
            stroker.setDashPattern(m_pen.dashPattern()); stroker.setDashOffset(m_pen.dashOffset());
        }
        if (m_pen.isCosmetic() || m_pen.widthF() == 0)
            include(stroker.createStroke(m_transform.map(path)));
        else include(m_transform.map(stroker.createStroke(path)));
    }
    QPicture& m_picture;
    QPainter m_forward;
    QPen m_pen;
    QBrush m_brush{Qt::NoBrush};
    QTransform m_transform;
    QPainterPath m_clip;
    qreal m_opacity{1};
    bool m_clip_enabled{};
    bool m_has_clip{};
    bool m_valid{true};
    std::optional<QRectF> m_bounds;
};

SketchContentBoundsDevice::SketchContentBoundsDevice(QPicture& picture)
    : m_picture(picture), m_engine(std::make_unique<Engine>(picture)) {}
SketchContentBoundsDevice::~SketchContentBoundsDevice() = default;
QPaintEngine* SketchContentBoundsDevice::paintEngine() const { return m_engine.get(); }
int SketchContentBoundsDevice::devType() const { return QPaintDevice::devType(); }
std::optional<QRectF> SketchContentBoundsDevice::inkBounds() const { return m_engine->bounds(); }
bool SketchContentBoundsDevice::valid() const { return m_engine->valid(); }
int SketchContentBoundsDevice::metric(PaintDeviceMetric value) const {
    switch (value) {
    case PdmWidth: return m_picture.width();
    case PdmHeight: return m_picture.height();
    case PdmWidthMM: return m_picture.widthMM();
    case PdmHeightMM: return m_picture.heightMM();
    case PdmNumColors: return m_picture.colorCount();
    case PdmDepth: return m_picture.depth();
    case PdmDpiX: return m_picture.logicalDpiX();
    case PdmDpiY: return m_picture.logicalDpiY();
    case PdmPhysicalDpiX: return m_picture.physicalDpiX();
    case PdmPhysicalDpiY: return m_picture.physicalDpiY();
    case PdmDevicePixelRatio: return 1;
    case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
    case PdmDevicePixelRatioF_EncodedA:
    case PdmDevicePixelRatioF_EncodedB: return encodeMetricF(value, 1.0);
    }
    return 0;
}
} // namespace sketch::desktop
