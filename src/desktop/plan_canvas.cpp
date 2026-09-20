#include "plan_canvas.hpp"

#include <QApplication>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPaintEvent>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace sketch::desktop {
namespace {

// Site surveys can span kilometres. Fit and navigation must not crop them
// at a building-sized zoom floor; the adaptive grid already scales with zoom.
constexpr double minimum_scale = 0.0001;
constexpr double maximum_scale = 4000.0;
constexpr double output_minimum_scale = minimum_scale;
constexpr double pi = std::numbers::pi;

bool drawable_label(const CanvasLabel& label) {
    return !label.text.isEmpty() && std::isfinite(label.position.x) &&
           std::isfinite(label.position.y);
}

struct LabelLayout {
    QFont font;
    QRectF bounds;
};

LabelLayout label_layout(const CanvasLabel& label, QFont base_font,
                         const QPaintDevice* device, double scale, double dpi) {
    const auto paper_pixels = label.paper_height_mm * dpi / 25.4;
    const bool paper = std::isfinite(paper_pixels) && paper_pixels > 0.0 &&
                       paper_pixels <= std::numeric_limits<int>::max();
    if (paper) {
        // Do not apply the legacy screen-readability clamp to physical text:
        // a 600-DPI printer needs more pixels than the screen for the same mm.
        base_font.setPixelSize(static_cast<int>(std::lround(std::max(1.0, paper_pixels))));
        base_font.setBold(label.bold);
        base_font.setItalic(label.italic);
    } else {
        const auto text_height = std::isfinite(label.text_height_metres) &&
                                 label.text_height_metres > 0.0 ? label.text_height_metres : 0.15;
        const auto instance_scale = std::isfinite(label.scale) && label.scale > 0.0 ? label.scale : 1.0;
        base_font.setPixelSize(static_cast<int>(std::lround(
            std::clamp(text_height * scale * instance_scale, 8.0, 96.0))));
        if (label.bold) base_font.setBold(true);
        if (label.italic) base_font.setItalic(true);
    }
    const QFontMetricsF metrics(base_font, device);
    auto bounds = metrics.boundingRect(label.text);
    bounds.moveCenter(QPointF(0.0, 0.0));
    bounds.adjust(-5.0, -3.0, 5.0, 3.0);
    return {base_font, bounds};
}

QTransform label_transform(const CanvasLabel& label, QPointF center) {
    QTransform transform;
    transform.translate(center.x(), center.y());
    if (std::isfinite(label.rotation_radians)) {
        // Model coordinates are y-up while Qt device coordinates are y-down.
        transform.rotate(-label.rotation_radians * 180.0 / pi);
    }
    return transform;
}

double distance(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

QString display_cursor_length(double metres, bool metric) {
    if (!std::isfinite(metres)) return QStringLiteral("—");
    if (metric) return QStringLiteral("%1 m").arg(metres, 0, 'f', 3);
    constexpr double metres_per_foot = 0.3048;
    return QStringLiteral("%1 ft").arg(metres / metres_per_foot, 0, 'f', 2);
}

Vec2 operator+(Vec2 left, Vec2 right) {
    return {left.x + right.x, left.y + right.y};
}

Vec2 operator-(Vec2 left, Vec2 right) {
    return {left.x - right.x, left.y - right.y};
}

Vec2 operator*(Vec2 point, double factor) {
    return {point.x * factor, point.y * factor};
}

struct ArcInfo {
    Vec2 center{};
    double radius{};
    double start_angle{};
};

std::optional<ArcInfo> arc_info(const Segment& segment) {
    if (segment.sweep_radians == 0.0) {
        return std::nullopt;
    }
    const auto chord = segment.end - segment.start;
    const auto chord_length = distance(segment.start, segment.end);
    const auto half_sweep = segment.sweep_radians / 2.0;
    const auto tangent = std::tan(half_sweep);
    const auto sine = std::sin(std::abs(half_sweep));
    if (!(chord_length > 0.0) || tangent == 0.0 || sine == 0.0 ||
        !std::isfinite(tangent) || !std::isfinite(sine)) {
        return std::nullopt;
    }
    const auto midpoint = (segment.start + segment.end) * 0.5;
    const Vec2 left_normal{-chord.y / chord_length, chord.x / chord_length};
    const auto center = midpoint + left_normal * (chord_length / (2.0 * tangent));
    const auto radius = chord_length / (2.0 * sine);
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius)) {
        return std::nullopt;
    }
    return ArcInfo{center, radius,
                   std::atan2(segment.start.y - center.y, segment.start.x - center.x)};
}

Vec2 arc_point(const Segment& segment, const ArcInfo& arc, double parameter) {
    const auto angle = arc.start_angle + segment.sweep_radians * parameter;
    return arc.center + Vec2{std::cos(angle), std::sin(angle)} * arc.radius;
}

double point_segment_distance(QPointF point, QPointF start, QPointF end) {
    const auto dx = end.x() - start.x();
    const auto dy = end.y() - start.y();
    const auto length_squared = dx * dx + dy * dy;
    if (length_squared <= std::numeric_limits<double>::epsilon()) {
        return std::hypot(point.x() - start.x(), point.y() - start.y());
    }
    const auto projection =
        std::clamp(((point.x() - start.x()) * dx + (point.y() - start.y()) * dy) /
                       length_squared,
                   0.0, 1.0);
    return std::hypot(point.x() - (start.x() + projection * dx),
                      point.y() - (start.y() + projection * dy));
}

QColor color_for(const CanvasEntity& entity) {
    if (entity.selected) {
        return QColor(75, 210, 255);
    }
    if (entity.type == QStringLiteral("wall")) {
        return QColor(235, 164, 71);
    }
    if (entity.type == QStringLiteral("room_boundary")) {
        return QColor(103, 203, 141);
    }
    if (entity.type == QStringLiteral("measurement_boundary")) {
        return QColor(86, 159, 232);
    }
    if (entity.type == QStringLiteral("boundary")) {
        return QColor(86, 159, 232);
    }
    if (entity.type == QStringLiteral("slab")) {
        return QColor(112, 183, 211);
    }
    if (entity.type == QStringLiteral("terrain_surface")) {
        return QColor(119, 164, 113);
    }
    if (entity.type == QStringLiteral("dimension_line")) {
        return QColor(196, 203, 214);
    }
    return QColor(182, 191, 205);
}

Qt::BrushStyle hatch_style(QString pattern) {
    pattern = pattern.trimmed().toLower();
    if (pattern.isEmpty() || pattern == QStringLiteral("none")) {
        return Qt::NoBrush;
    }
    if (pattern == QStringLiteral("solid") || pattern == QStringLiteral("filled")) {
        return Qt::SolidPattern;
    }
    if (pattern == QStringLiteral("horizontal") || pattern == QStringLiteral("hor")) {
        return Qt::HorPattern;
    }
    if (pattern == QStringLiteral("vertical") || pattern == QStringLiteral("vert")) {
        return Qt::VerPattern;
    }
    if (pattern == QStringLiteral("cross")) {
        return Qt::CrossPattern;
    }
    if (pattern == QStringLiteral("diagonal") || pattern == QStringLiteral("diag") ||
        pattern == QStringLiteral("backward_diagonal")) {
        return Qt::BDiagPattern;
    }
    if (pattern == QStringLiteral("forward_diagonal")) {
        return Qt::FDiagPattern;
    }
    if (pattern == QStringLiteral("diagonal_cross") || pattern == QStringLiteral("diagcross")) {
        return Qt::DiagCrossPattern;
    }
    if (pattern == QStringLiteral("dots") || pattern == QStringLiteral("concrete")) {
        return Qt::Dense4Pattern;
    }
    if (pattern == QStringLiteral("dense")) {
        return Qt::Dense6Pattern;
    }
    // ViewPresentation deliberately accepts user-defined identifier names.
    // An unknown name still renders deterministically while remaining visible
    // in the persisted document for a future catalog entry.
    return Qt::BDiagPattern;
}

std::optional<QPainterPath> closed_entity_path(const CanvasEntity& entity) {
    if (entity.segments.size() < 3) return std::nullopt;
    constexpr double endpoint_tolerance = 1e-7;
    const auto& first = entity.segments.front();
    QPainterPath path;
    path.moveTo(first.start.x, first.start.y);
    auto previous = first.start;
    for (const auto& segment : entity.segments) {
        if (distance(previous, segment.start) > endpoint_tolerance) {
            return std::nullopt;
        }
        if (segment.sweep_radians == 0.0) {
            path.lineTo(segment.end.x, segment.end.y);
        } else {
            const auto arc = arc_info(segment);
            if (!arc.has_value()) return std::nullopt;
            const QRectF bounds(arc->center.x - arc->radius, arc->center.y - arc->radius,
                               arc->radius * 2.0, arc->radius * 2.0);
            path.arcTo(bounds, -arc->start_angle * 180.0 / pi,
                       -segment.sweep_radians * 180.0 / pi);
        }
        previous = segment.end;
    }
    if (distance(previous, first.start) > endpoint_tolerance) return std::nullopt;
    path.closeSubpath();
    return path;
}

}  // namespace

PlanCanvas::PlanCanvas(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(480, 360);
    setMouseTracking(true);
    setAcceptDrops(true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setAttribute(Qt::WA_TabletTracking, true);
    setAutoFillBackground(false);
    qApp->installEventFilter(this);
}

void PlanCanvas::setEntities(std::vector<CanvasEntity> entities) {
    m_entities = std::move(entities);
    update();
}

void PlanCanvas::setTool(CanvasTool tool) {
    resetGesture();
    m_tool = tool;
    setCursor(tool == CanvasTool::select ? Qt::ArrowCursor : Qt::CrossCursor);
    setFocus();
    update();
}

void PlanCanvas::setGridEnabled(bool enabled) {
    m_grid_enabled = enabled;
    update();
}

void PlanCanvas::setSnapEnabled(bool enabled) {
    if (m_snap_enabled == enabled) return;
    m_snap_enabled = enabled;
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::setPerformanceMeasured(std::function<void(PerformanceMetric,
                                       std::chrono::steady_clock::duration)> callback) {
    resetPerformanceMeasurements();
    m_performance_measured = std::move(callback);
}

void PlanCanvas::beginPerformanceMeasurement(PerformanceMetric metric,
                                            PerformanceClock::time_point started) {
    if (!m_performance_measured || !isVisible() || QApplication::activeModalWidget()) return;
    // Use only the enum: the standalone canvas does not link core telemetry.
    switch (metric) {
    case PerformanceMetric::navigation:
    case PerformanceMetric::input:
    case PerformanceMetric::edit:
    case PerformanceMetric::open:
    case PerformanceMetric::save:
        break;
    default:
        return;
    }
    const auto pending = std::find_if(m_pending_measurements.begin(), m_pending_measurements.end(),
        [metric](const auto& item) { return item.first == metric; });
    if (pending == m_pending_measurements.end())
        m_pending_measurements.emplace_back(metric, started);
    else
        pending->second = std::min(pending->second, started);
    // Even an input that only changes focus needs a completed canvas paint.
    update();
}

void PlanCanvas::cancelPerformanceMeasurement(PerformanceMetric metric) {
    std::erase_if(m_pending_measurements, [metric](const auto& item) { return item.first == metric; });
}

void PlanCanvas::resetPerformanceMeasurements() {
    m_pending_measurements.clear();
    ++m_measurement_generation;
}

void PlanCanvas::setOverviewMapEnabled(bool enabled) {
    if (m_overview_map_enabled == enabled) return;
    m_overview_map_enabled = enabled;
    update();
}

void PlanCanvas::setMetricUnits(bool metric) {
    m_metric_units = metric;
    update();
}

void PlanCanvas::setCanvasBackground(QColor background) {
    if (!background.isValid()) return;
    if (m_canvas_background == background) return;
    m_canvas_background = std::move(background);
    update();
}

void PlanCanvas::setSelectedId(const QString& entity_id) {
    setSelectedIds(entity_id.isEmpty() ? QStringList{} : QStringList{entity_id});
}

void PlanCanvas::setSelectedIds(const QStringList& entity_ids) {
    for (auto& entity : m_entities) {
        entity.selected = entity_ids.contains(entity.id);
    }
    for (auto& label : m_labels) label.selected = entity_ids.contains(label.id);
    for (auto& reference : m_references) reference.selected = entity_ids.contains(reference.id);
    update();
}

void PlanCanvas::setLabels(std::vector<CanvasLabel> labels) {
    m_labels = std::move(labels);
    update();
}

void PlanCanvas::setReference(std::optional<CanvasReference> reference) {
    std::vector<CanvasReference> references;
    if (reference) references.push_back(std::move(*reference));
    setReferences(std::move(references));
}

void PlanCanvas::setReferences(std::vector<CanvasReference> references) {
    m_references = std::move(references);
    update();
}

void PlanCanvas::setReferenceGrids(std::vector<CanvasReferenceGrid> grids) {
    m_reference_grids = std::move(grids);
    update();
}

void PlanCanvas::setBoundaryPreview(std::vector<Vec2> points) {
    m_boundary_preview = std::move(points);
    update();
}

void PlanCanvas::setWallPreview(std::optional<std::pair<Vec2, Vec2>> wall) {
    m_wall_preview = std::move(wall);
    update();
}

void PlanCanvas::setBoundaryDraftPreview(std::optional<BoundaryDraftPreview> preview) {
    m_boundary_draft_preview = std::move(preview);
    update();
}

void PlanCanvas::clearPreview() {
    m_boundary_preview.clear();
    m_wall_preview.reset();
    m_boundary_draft_preview.reset();
    update();
}

std::optional<std::pair<Vec2, Vec2>> PlanCanvas::contentBounds() const {
    Vec2 minimum{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 maximum{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool has_content = false;
    const auto include = [&](Vec2 point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        has_content = true;
    };
    for (const auto& entity : m_entities) {
        for (const auto& segment : entity.segments) {
            include(segment.start);
            include(segment.end);
            try {
                const auto bounds = segment_bounds(segment);
                include(bounds.minimum);
                include(bounds.maximum);
            } catch (const std::invalid_argument&) {
                // Invalid retained presentation geometry still contributes its
                // finite endpoints; semantic diagnostics belong to the model.
            }
        }
    }
    for (const auto& label : m_labels) include(label.position);
    for (const auto& reference : m_references) {
        if (reference.visible && !reference.image.isNull() &&
            std::isfinite(reference.metres_per_source_unit) &&
            reference.metres_per_source_unit > 0.0 && std::isfinite(reference.scale) &&
            reference.scale > 0.0) {
            const auto width = reference.image.width() * reference.metres_per_source_unit *
                               reference.scale;
            const auto height = reference.image.height() * reference.metres_per_source_unit *
                                reference.scale;
            include({reference.position.x - width * 0.5, reference.position.y - height * 0.5});
            include({reference.position.x + width * 0.5, reference.position.y + height * 0.5});
        }
    }
    for (const auto& grid : m_reference_grids) {
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            include(line.start);
            include(line.end);
        }
    }
    if (!has_content) return std::nullopt;
    return std::make_pair(minimum, maximum);
}

QRectF PlanCanvas::overviewMapRect() const noexcept {
    if (!m_overview_map_enabled) return {};
    constexpr qreal width = 180.0;
    constexpr qreal height = 118.0;
    constexpr qreal margin = 12.0;
    const auto available_width = std::max<qreal>(0.0, rect().width() - margin * 2.0);
    const auto available_height = std::max<qreal>(0.0, rect().height() - margin * 2.0);
    const auto map_width = std::min(width, available_width);
    const auto map_height = std::min(height, available_height);
    if (map_width < 80.0 || map_height < 56.0) return {};
    return QRectF(rect().right() - margin - map_width + 1.0,
                  rect().bottom() - margin - map_height + 1.0, map_width, map_height);
}

void PlanCanvas::fitView() {
    beginPerformanceMeasurement(PerformanceMetric::navigation);
    const auto bounds = contentBounds();
    if (!bounds) {
        m_view_center = {0.0, 0.0};
        m_scale = 80.0;
        if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
        update();
        return;
    }
    const auto minimum = bounds->first;
    const auto maximum = bounds->second;
    const auto width = std::max(maximum.x - minimum.x, 0.1);
    const auto height = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(width, height) * 0.12 + 0.25;
    m_view_center = {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y)};
    m_scale = std::clamp(std::min((width + padding * 2.0) > 0.0
                                      ? std::max(1.0, static_cast<double>(size().width())) /
                                            (width + padding * 2.0)
                                      : 80.0,
                                  (height + padding * 2.0) > 0.0
                                      ? std::max(1.0, static_cast<double>(size().height())) /
                                            (height + padding * 2.0)
                                      : 80.0),
                        minimum_scale, maximum_scale);
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::zoomBy(double factor, QPointF anchor) {
    if (!(factor > 0.0) || !std::isfinite(factor)) {
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::navigation);
    if (anchor.isNull()) {
        anchor = rect().center();
    }
    const auto before = toModel(anchor, rect());
    m_scale = std::clamp(m_scale * factor, minimum_scale, maximum_scale);
    const auto after = toModel(anchor, rect());
    m_view_center = m_view_center + (before - after);
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

void PlanCanvas::renderScene(QPainter& painter, const QRectF& viewport) const {
    renderSceneWithTransform(painter, viewport, false, m_canvas_background,
                             std::nullopt, std::nullopt);
}

void PlanCanvas::renderScene(QPainter& painter, const QRectF& viewport, bool fit_to_content,
                             QColor background) const {
    renderSceneWithTransform(painter, viewport, fit_to_content, background,
                             std::nullopt, std::nullopt);
}

void PlanCanvas::renderSceneAt(QPainter& painter, const QRectF& viewport, double scale,
                               Vec2 view_center, QColor background,
                               std::optional<double> paper_pixels_per_mm) const {
    if (!(std::isfinite(scale) && scale > 0.0) || !std::isfinite(view_center.x) ||
        !std::isfinite(view_center.y)) {
        return;
    }
    renderSceneWithTransform(painter, viewport, false, background, scale, view_center,
                             paper_pixels_per_mm);
}

Vec2 PlanCanvas::contentCenter() const noexcept {
    const auto bounds = contentBounds();
    return bounds ? Vec2{std::midpoint(bounds->first.x, bounds->second.x),
                         std::midpoint(bounds->first.y, bounds->second.y)} : m_view_center;
}

void PlanCanvas::renderSceneWithTransform(QPainter& painter, const QRectF& viewport,
                                          bool fit_to_content, QColor background,
                                          std::optional<double> explicit_scale,
                                          std::optional<Vec2> explicit_center,
                                          std::optional<double> paper_pixels_per_mm) const {
    if (viewport.width() <= 0.0 || viewport.height() <= 0.0) {
        return;
    }
    auto scale = explicit_scale.value_or(m_scale);
    auto view_center = explicit_center.value_or(m_view_center);
    // An explicit transform is the sheet/output path even when the caller
    // supplies its own model scale and center. Keep that path free of
    // interactive-only state just like fit-to-content output.
    const bool output = fit_to_content || explicit_scale.has_value();
    if (!explicit_scale.has_value() && fit_to_content) {
        if (const auto bounds = contentBounds()) {
            const auto minimum = bounds->first;
            const auto maximum = bounds->second;
            const auto width = std::max(maximum.x - minimum.x, 0.1);
            const auto height = std::max(maximum.y - minimum.y, 0.1);
            const auto padding = std::max(width, height) * 0.12 + 0.25;
            view_center = {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y)};
            scale = std::min(viewport.width() / (width + padding * 2.0),
                             viewport.height() / (height + padding * 2.0));
            scale = std::clamp(scale, output_minimum_scale, maximum_scale);
        }
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(viewport, background);
    painter.translate(viewport.center());
    painter.scale(scale, -scale);
    painter.translate(-view_center.x, -view_center.y);

    for (const auto& reference : m_references) {
        if (!reference.visible) continue;
        if (!output && m_move_preview_delta && m_move_ids.contains(reference.id)) {
            painter.save();
            painter.translate(m_move_preview_delta->x, m_move_preview_delta->y);
            drawReference(painter, reference);
            painter.restore();
        } else {
            drawReference(painter, reference);
        }
    }

    if (m_grid_enabled && !output) {
        drawGrid(painter, viewport, scale, view_center);
    }
    drawReferenceGrids(painter);
    for (const auto& entity : m_entities) {
        if (!output && m_move_preview_delta && m_move_ids.contains(entity.id)) {
            painter.save();
            painter.translate(m_move_preview_delta->x, m_move_preview_delta->y);
            drawEntity(painter, entity, output, background, paper_pixels_per_mm);
            painter.restore();
        } else {
            drawEntity(painter, entity, output, background, paper_pixels_per_mm);
        }
    }

    // Transient overlays belong to the interactive canvas only. Both fitted
    // and explicitly scaled output must contain document entities alone.
    if (!output) {
        if (m_boundary_preview.size() >= 2) {
            QPen pen(QColor(255, 220, 126), 0.0, Qt::DashLine);
            painter.setPen(pen);
            QPainterPath path;
            path.moveTo(m_boundary_preview.front().x, m_boundary_preview.front().y);
            for (std::size_t index = 1; index < m_boundary_preview.size(); ++index) {
                path.lineTo(m_boundary_preview[index].x, m_boundary_preview[index].y);
            }
            painter.drawPath(path);
        }
        if (m_wall_preview.has_value()) {
            QPen pen(QColor(255, 220, 126), 0.0, Qt::DashLine);
            painter.setPen(pen);
            QPainterPath path;
            path.moveTo(m_wall_preview->first.x, m_wall_preview->first.y);
            path.lineTo(m_wall_preview->second.x, m_wall_preview->second.y);
            painter.drawPath(path);
        }
        if (m_boundary_draft_preview.has_value()) {
            const auto& draft = *m_boundary_draft_preview;
            QPen draft_pen(QColor(255, 220, 126, 185), 1.5,
                           Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            draft_pen.setCosmetic(true);
            painter.setPen(draft_pen);
            for (const auto& segment : draft.segments) {
                drawSegment(painter, segment);
            }

            if (draft.rubber_band.has_value()) {
                painter.setPen(QPen(QColor(255, 111, 173, 235), 0.0,
                                    Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
                drawSegment(painter, *draft.rubber_band);
            }

            const auto draw_marker = [&](std::optional<Vec2> point, QColor color) {
                if (!point.has_value()) return;
                const auto radius = 8.0 / scale;
                painter.setPen(QPen(color, 0.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QPointF(point->x, point->y), radius, radius);
                painter.drawLine(QPointF(point->x - radius, point->y),
                                 QPointF(point->x + radius, point->y));
                painter.drawLine(QPointF(point->x, point->y - radius),
                                 QPointF(point->x, point->y + radius));
            };
            draw_marker(draft.anchor, QColor(114, 222, 164));
            draw_marker(draft.pen_position, QColor(103, 202, 255));
        }
    }
    painter.restore();

    drawReferenceGridLabels(painter, viewport, scale, view_center, output, background,
                            paper_pixels_per_mm);

    // Committed labels use the same model-to-screen mapping as the current
    // scene, including fit-to-content output. Drawing after restoring the
    // world transform keeps text upright and readable at its device scale.
    drawLabels(painter, viewport, scale, view_center, output, background, paper_pixels_per_mm);

    if (!output) {
        drawSelectionFrame(painter, viewport);
        drawCursorReadout(painter, viewport, background);
    }

    if (!output) {
        QString instruction;
        if (m_boundary_draft_preview.has_value() &&
            !m_boundary_draft_preview->instruction.isEmpty()) {
            instruction = m_boundary_draft_preview->instruction;
        } else if (m_tool != CanvasTool::select) {
            if (m_tool == CanvasTool::boundary) {
                instruction = QStringLiteral("Boundary tool  •  click points, Enter closes, D precise segment");
            } else if (m_tool == CanvasTool::sloped_wall) {
                instruction = QStringLiteral("Sloped wall tool  •  click two points, then enter the signed rise");
            } else {
                instruction = QStringLiteral("Wall tool  •  click two points");
            }
        }
        if (!instruction.isEmpty()) {
            painter.save();
            painter.setPen(background.lightnessF() > 0.5
                               ? QColor(86, 102, 124)
                               : QColor(190, 201, 219));
            painter.drawText(viewport.adjusted(12.0, 10.0, -12.0, -10.0),
                             Qt::AlignTop | Qt::AlignLeft, instruction);
            painter.restore();
        }
    }
    if (!output && m_boundary_draft_preview.has_value()) {
        const auto& draft = *m_boundary_draft_preview;
        const auto to_screen = [&](Vec2 point) {
            return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                           viewport.center().y() - (point.y - view_center.y) * scale);
        };
        painter.save();
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        const QFontMetricsF metrics(painter.font());
        for (const auto& label : draft.labels) {
            if (label.text.isEmpty()) continue;
            const auto center = to_screen(label.position);
            auto bounds = metrics.boundingRect(label.text);
            bounds.moveCenter(center);
            bounds.adjust(-5.0, -3.0, 5.0, 3.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background.lightnessF() > 0.5
                                 ? QColor(255, 255, 255, 238)
                                 : QColor(20, 25, 34, 225));
            painter.drawRoundedRect(bounds, 3.0, 3.0);
            painter.setPen(background.lightnessF() > 0.5
                               ? QColor(50, 65, 84)
                               : QColor(255, 239, 172));
            painter.setBrush(Qt::NoBrush);
            painter.drawText(bounds, Qt::AlignCenter, label.text);
        }
        painter.restore();
    }
    if (!output) drawOverviewMap(painter);
}

void PlanCanvas::drawOverviewMap(QPainter& painter) const {
    const auto map = overviewMapRect();
    const auto bounds = contentBounds();
    if (map.isEmpty()) return;
    const auto light = m_canvas_background.lightnessF() > 0.5;
    painter.save();
    painter.setPen(QPen(light ? QColor(128, 147, 173) : QColor(120, 143, 174), 1.0));
    painter.setBrush(light ? QColor(255, 255, 255, 242) : QColor(17, 25, 38, 242));
    painter.drawRoundedRect(map, 8.0, 8.0);
    painter.setPen(light ? QColor(50, 65, 84) : QColor(220, 232, 246));
    QFont map_font = painter.font();
    map_font.setPixelSize(10);
    painter.setFont(map_font);
    painter.drawText(map.adjusted(8.0, 3.0, -8.0, -3.0), Qt::AlignTop | Qt::AlignLeft,
                     tr("Overview · click or drag to pan"));
    if (!bounds) {
        painter.drawText(map.adjusted(8.0, 22.0, -8.0, -8.0), Qt::AlignCenter,
                         tr("No drawing yet"));
        painter.restore();
        return;
    }
    painter.restore();

    const auto inner = map.adjusted(8.0, 22.0, -8.0, -8.0);
    if (inner.width() <= 0.0 || inner.height() <= 0.0) return;
    const auto minimum = bounds->first;
    const auto maximum = bounds->second;
    const auto span_x = std::max(maximum.x - minimum.x, 0.1);
    const auto span_y = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(span_x, span_y) * 0.08 + 0.1;
    const auto world_min = Vec2{minimum.x - padding, minimum.y - padding};
    const auto world_max = Vec2{maximum.x + padding, maximum.y + padding};
    const auto world_span_x = std::max(world_max.x - world_min.x, 0.1);
    const auto world_span_y = std::max(world_max.y - world_min.y, 0.1);
    const auto map_scale = std::min(inner.width() / world_span_x,
                                    inner.height() / world_span_y);
    const auto world_center = Vec2{(world_min.x + world_max.x) * 0.5,
                                   (world_min.y + world_max.y) * 0.5};
    const auto to_map = [&](Vec2 point) {
        return QPointF(inner.center().x() + (point.x - world_center.x) * map_scale,
                       inner.center().y() - (point.y - world_center.y) * map_scale);
    };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setClipRect(inner);
    for (const auto& entity : m_entities) {
        auto pen_color = color_for(entity);
        pen_color.setAlpha(light ? 235 : 220);
        QPen pen(pen_color, entity.selected ? 2.0 : 1.0,
                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        pen.setCosmetic(true);
        painter.setPen(pen);
        for (const auto& segment : entity.segments) {
            if (segment.sweep_radians == 0.0) {
                painter.drawLine(to_map(segment.start), to_map(segment.end));
                continue;
            }
            if (const auto arc = arc_info(segment)) {
                QPainterPath path;
                path.moveTo(to_map(segment.start));
                constexpr int samples = 32;
                for (int index = 1; index <= samples; ++index)
                    path.lineTo(to_map(arc_point(segment, *arc,
                                                 static_cast<double>(index) / samples)));
                painter.drawPath(path);
            }
        }
    }
    const auto visible_width = width() / std::max(m_scale, minimum_scale);
    const auto visible_height = height() / std::max(m_scale, minimum_scale);
    const auto top_left = to_map({m_view_center.x - visible_width * 0.5,
                                  m_view_center.y + visible_height * 0.5});
    const auto bottom_right = to_map({m_view_center.x + visible_width * 0.5,
                                      m_view_center.y - visible_height * 0.5});
    auto viewport_rect = QRectF(top_left, bottom_right).normalized();
    QPen viewport_pen(light ? QColor(32, 139, 220, 235) : QColor(103, 202, 255, 245), 1.5,
                      Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
    viewport_pen.setCosmetic(true);
    painter.setPen(viewport_pen);
    painter.setBrush(QColor(32, 139, 220, 28));
    // Keep an enclosing viewport visible instead of clipping all four edges
    // away when the full drawing is already in view.
    painter.drawRect(viewport_rect.intersected(inner.adjusted(1.0, 1.0, -1.0, -1.0)));
    painter.restore();

    painter.save();
    painter.setPen(QPen(light ? QColor(128, 147, 173, 220) : QColor(120, 143, 174, 220), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(map, 8.0, 8.0);
    painter.restore();
}

bool PlanCanvas::navigateOverviewMap(QPointF position) {
    const auto map = overviewMapRect();
    if (map.isEmpty() || !map.contains(position)) return false;
    const auto bounds = contentBounds();
    if (!bounds) return true;
    const auto inner = map.adjusted(8.0, 22.0, -8.0, -8.0);
    const auto minimum = bounds->first;
    const auto maximum = bounds->second;
    const auto span_x = std::max(maximum.x - minimum.x, 0.1);
    const auto span_y = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(span_x, span_y) * 0.08 + 0.1;
    const auto world_min = Vec2{minimum.x - padding, minimum.y - padding};
    const auto world_max = Vec2{maximum.x + padding, maximum.y + padding};
    const auto world_span_x = std::max(world_max.x - world_min.x, 0.1);
    const auto world_span_y = std::max(world_max.y - world_min.y, 0.1);
    const auto map_scale = std::min(inner.width() / world_span_x,
                                    inner.height() / world_span_y);
    if (!(map_scale > 0.0) || !std::isfinite(map_scale)) return true;
    const auto world_center = Vec2{(world_min.x + world_max.x) * 0.5,
                                   (world_min.y + world_max.y) * 0.5};
    m_view_center = {world_center.x + (position.x() - inner.center().x()) / map_scale,
                     world_center.y - (position.y() - inner.center().y()) / map_scale};
    updateCursor(position);
    update();
    return true;
}

void PlanCanvas::setPointClicked(std::function<void(Vec2)> callback) {
    m_point_clicked = std::move(callback);
}

void PlanCanvas::setEntityClicked(std::function<void(QString)> callback) {
    m_entity_clicked = std::move(callback);
}

void PlanCanvas::setEntityDoubleClicked(std::function<void(QString)> callback) {
    m_entity_double_clicked = std::move(callback);
}

void PlanCanvas::setEntitySelectionClicked(std::function<void(QString, bool)> callback) {
    m_entity_selection_clicked = std::move(callback);
}

void PlanCanvas::setEntitiesSelected(std::function<void(QStringList, bool)> callback) {
    m_entities_selected = std::move(callback);
}

void PlanCanvas::setEntitiesMoveRequested(std::function<bool(QStringList, Vec2)> callback) {
    m_entities_move_requested = std::move(callback);
}

void PlanCanvas::setSymbolDropped(std::function<void(QString, double, Vec2)> callback) {
    m_symbol_dropped = std::move(callback);
}

void PlanCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (m_symbol_dropped && event->mimeData()->hasFormat("application/x-vertex-symbol") &&
        event->mimeData()->data("application/x-vertex-symbol").size() <= 4096)
        event->acceptProposedAction();
}

void PlanCanvas::dragMoveEvent(QDragMoveEvent* event) {
    if (m_symbol_dropped && event->mimeData()->hasFormat("application/x-vertex-symbol")) {
        updateCursor(event->position());
        event->acceptProposedAction();
    }
}

void PlanCanvas::dropEvent(QDropEvent* event) {
    if (!m_symbol_dropped) return;
    const auto payload = event->mimeData()->data("application/x-vertex-symbol");
    if (payload.size() > 4096) return;
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) return;
    const auto object = document.object();
    const auto id = object.value("id").toString();
    const auto scale = object.value("scale").toDouble(0.0);
    if (id.isEmpty() || id.size() > 256 || !std::isfinite(scale) || scale <= 0.0 || scale > 100.0) return;
    m_symbol_dropped(id, scale, snapped(toModel(event->position(), rect())));
    event->acceptProposedAction();
}

void PlanCanvas::setCursorMoved(std::function<void(Vec2)> callback) {
    m_cursor_moved = std::move(callback);
}

void PlanCanvas::setFinishRequested(std::function<void()> callback) {
    m_finish_requested = std::move(callback);
}

void PlanCanvas::setCancelRequested(std::function<void()> callback) {
    m_cancel_requested = std::move(callback);
}

void PlanCanvas::setPreciseInputRequested(std::function<void()> callback) {
    m_precise_input_requested = std::move(callback);
}

void PlanCanvas::setDraftUndoRequested(std::function<void()> callback) {
    m_draft_undo_requested = std::move(callback);
}

void PlanCanvas::setDraftRedoRequested(std::function<void()> callback) {
    m_draft_redo_requested = std::move(callback);
}

bool PlanCanvas::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show) {
        const auto* dialog = qobject_cast<QDialog*>(watched);
        if (dialog && dialog->isModal()) {
            resetPerformanceMeasurements();
            resetGesture();
        }
    } else if ((event->type() == QEvent::WindowBlocked ||
                event->type() == QEvent::WindowDeactivate) && watched == window()) {
        resetPerformanceMeasurements();
        resetGesture();
    }
    return QWidget::eventFilter(watched, event);
}

bool PlanCanvas::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Hide:
    case QEvent::WindowBlocked:
    case QEvent::WindowDeactivate:
    case QEvent::UngrabMouse:
        resetPerformanceMeasurements();
        resetGesture();
        m_space_pan_armed = false;
        m_touch_active = false;
        m_touch_id = -1;
        m_tablet_active = false;
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        beginPerformanceMeasurement(PerformanceMetric::input);
        break;
    default:
        break;
    }
    switch (event->type()) {
    case QEvent::TouchBegin: {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (!m_touch_active && !touch->points().isEmpty()) {
            const auto& point = touch->points().front();
            m_touch_active = true;
            m_touch_id = point.id();
            pointerPress(point.position(), Qt::LeftButton);
        }
        event->accept();
        return true;
    }
    case QEvent::TouchUpdate: {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (m_touch_active) {
            for (const auto& point : touch->points()) {
                if (point.id() == m_touch_id) {
                    pointerMove(point.position());
                    break;
                }
            }
        }
        event->accept();
        return true;
    }
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (m_touch_active) {
            QPointF position = m_last_mouse_position.value_or(QPointF(width() / 2.0, height() / 2.0));
            for (const auto& point : touch->points()) {
                if (point.id() == m_touch_id) {
                    position = point.position();
                    break;
                }
            }
            if (event->type() == QEvent::TouchCancel) {
                resetGesture();
            } else {
                pointerRelease(position, Qt::LeftButton);
            }
            m_touch_active = false;
            m_touch_id = -1;
        }
        event->accept();
        return true;
    }
    case QEvent::TabletPress: {
        auto* tablet = static_cast<QTabletEvent*>(event);
        m_tablet_active = true;
        pointerPress(tablet->position(), Qt::LeftButton, tablet->modifiers());
        event->accept();
        return true;
    }
    case QEvent::TabletMove: {
        auto* tablet = static_cast<QTabletEvent*>(event);
        pointerMove(tablet->position());
        event->accept();
        return true;
    }
    case QEvent::TabletRelease: {
        auto* tablet = static_cast<QTabletEvent*>(event);
        if (m_tablet_active) {
            pointerRelease(tablet->position(), Qt::LeftButton);
            m_tablet_active = false;
        }
        event->accept();
        return true;
    }
    default:
        return QWidget::event(event);
    }
}

void PlanCanvas::pointerPress(QPointF position, Qt::MouseButton button,
                              Qt::KeyboardModifiers modifiers) {
    const bool middle_pan = button == Qt::MiddleButton;
    // Middle navigation can take over a pending left gesture. Other extra
    // buttons cannot retarget or complete the gesture that owns the press.
    if (m_gesture_button != Qt::NoButton && (m_panning || !middle_pan)) return;
    resetGesture();
    if (middle_pan || (button == Qt::LeftButton && overviewMapRect().contains(position)))
        beginPerformanceMeasurement(PerformanceMetric::navigation);
    setFocus();
    updateCursor(position);
    m_gesture_button = button;
    if (middle_pan) {
        m_panning = true;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (button == Qt::RightButton) {
        m_right_start = position;
        return;
    }
    if (button != Qt::LeftButton) return;
    if (navigateOverviewMap(position)) {
        m_overview_dragging = true;
        return;
    }
    if (m_space_pan_armed) {
        m_left_gesture = LeftGesture::space_pan;
        m_left_start = position;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    if (control) {
        m_left_gesture = LeftGesture::marquee;
        m_selection_start = position;
        m_selection_end = position;
        m_selection_dragging = false;
        m_selection_additive = true;
        return;
    }
    m_left_start = position;
    m_left_dragging = false;
    m_pressed_entity = hitTest(position);
    if (m_tool == CanvasTool::select && !m_pressed_entity.isEmpty() &&
        selectedIds().contains(m_pressed_entity)) {
        m_left_gesture = LeftGesture::object_move;
        m_move_ids = selectedIds();
        if (m_move_ids.isEmpty()) m_move_ids = {m_pressed_entity};
    } else {
        m_left_gesture = LeftGesture::canvas_pan;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
    }
}

void PlanCanvas::pointerMove(QPointF position) {
    m_last_mouse_position = position;
    if (m_gesture_button == Qt::RightButton &&
        (position - m_right_start).manhattanLength() >= QApplication::startDragDistance())
        m_right_dragging = true;
    if (m_overview_dragging) {
        (void)navigateOverviewMap(position);
        return;
    }
    if (m_left_gesture == LeftGesture::marquee && m_selection_start) {
        m_selection_end = position;
        if ((position - *m_selection_start).manhattanLength() >= QApplication::startDragDistance())
            m_selection_dragging = true;
        update();
    }
    if (m_left_gesture == LeftGesture::canvas_pan) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_left_dragging = true;
            m_panning = true;
            setCursor(Qt::ClosedHandCursor);
        }
    } else if (m_left_gesture == LeftGesture::object_move) {
        if (!m_left_dragging &&
            (position - m_left_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_left_dragging = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_left_dragging) {
            m_move_preview_delta = dragDelta(position);
            update();
        }
    }
    if (m_panning) {
        beginPerformanceMeasurement(PerformanceMetric::navigation);
        const auto delta = position - m_pan_start;
        m_view_center = {m_pan_view_start.x - delta.x() / m_scale,
                         m_pan_view_start.y + delta.y() / m_scale};
        update();
    }
    updateCursor(position);
}

void PlanCanvas::pointerRelease(QPointF position, Qt::MouseButton button) {
    if (button != m_gesture_button) return;
    // Publish the final effective point before any click callback. A normal
    // click can cross a snap boundary between press and release without Qt
    // delivering an intervening move event; authoring must use the release
    // point the user actually chose.
    updateCursor(position);
    if (button == Qt::RightButton) {
        const bool clicked = !m_right_dragging &&
            (position - m_right_start).manhattanLength() < QApplication::startDragDistance();
        const auto target = clicked ? hitTest(position) : QString{};
        resetGesture();
        if (clicked && m_right_clicked) m_right_clicked(inputPoint(position), target);
        return;
    }
    if (button == Qt::LeftButton) {
        const auto gesture = m_left_gesture;
        const auto start = m_left_start;
        const auto selection_start = m_selection_start;
        const bool dragging = m_left_dragging || m_selection_dragging ||
            (gesture != LeftGesture::none &&
             (position - (selection_start ? *selection_start : start)).manhattanLength() >=
                 QApplication::startDragDistance());
        const auto move_ids = m_move_ids;
        const auto pressed_entity = m_pressed_entity;
        const auto delta = dragDelta(position);
        const auto closing = closingAnchor(position);
        resetGesture();
        if (gesture == LeftGesture::marquee && selection_start) {
            if (dragging) {
                const auto ids = rectangleHits(QRectF(*selection_start, position).normalized(),
                                               position.x() < selection_start->x());
                if (m_entities_selected) m_entities_selected(ids, true);
            } else if (m_entity_selection_clicked) {
                m_entity_selection_clicked(hitTest(position), true);
            }
        } else if (gesture == LeftGesture::canvas_pan && !dragging) {
            if (m_tool == CanvasTool::select && !pressed_entity.isEmpty()) {
                if (m_entity_selection_clicked)
                    m_entity_selection_clicked(pressed_entity, false);
                else if (m_entity_clicked)
                    m_entity_clicked(pressed_entity);
            } else if (closing && m_finish_requested) {
                m_finish_requested();
            } else if (m_point_clicked) {
                m_point_clicked(inputPoint(position));
            }
        } else if (gesture == LeftGesture::object_move && dragging &&
                   m_entities_move_requested &&
                   (std::abs(delta.x) > 1e-12 || std::abs(delta.y) > 1e-12)) {
            (void)m_entities_move_requested(move_ids, delta);
        }
        updateCursor(position);
    }
    if (m_gesture_button != Qt::NoButton) resetGesture();
}

void PlanCanvas::resetGesture() {
    m_gesture_button = Qt::NoButton;
    m_panning = false;
    m_overview_dragging = false;
    m_selection_start.reset();
    m_selection_dragging = false;
    m_right_dragging = false;
    m_left_gesture = LeftGesture::none;
    m_left_dragging = false;
    m_pressed_entity.clear();
    m_move_ids.clear();
    m_move_preview_delta.reset();
    if (m_space_pan_armed) {
        setCursor(Qt::OpenHandCursor);
    } else if (m_tool == CanvasTool::select && m_last_mouse_position) {
        const auto target = hitTest(*m_last_mouse_position);
        setCursor(target.isEmpty() ? Qt::CrossCursor
                                   : selectedIds().contains(target) ? Qt::SizeAllCursor
                                                                    : Qt::ArrowCursor);
    } else {
        setCursor(Qt::CrossCursor);
    }
    update();
}

void PlanCanvas::setRightClicked(std::function<void(Vec2, QString)> callback) {
    m_right_clicked = std::move(callback);
}

void PlanCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    // Qt dispatches press/release/double-click/release. The first click already
    // authored or selected. In Select mode an unmodified left double-click
    // opens contextual properties for the stable hit target. Every other mode
    // consumes the second press so a double-click cannot add a duplicate point
    // or replay Ctrl-selection.
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
        m_tool == CanvasTool::select && m_entity_double_clicked) {
        const auto target = hitTest(event->position());
        if (!target.isEmpty()) m_entity_double_clicked(target);
    }
    event->accept();
}

std::optional<QRectF> PlanCanvas::selectionBounds() const {
    return selectionBounds(QRectF(rect()));
}

std::optional<QRectF> PlanCanvas::selectionBounds(const QRectF& viewport) const {
    std::optional<QRectF> result;
    const auto include = [&](const QRectF& bounds) {
        if (!std::isfinite(bounds.left()) || !std::isfinite(bounds.right()) ||
            !std::isfinite(bounds.top()) || !std::isfinite(bounds.bottom())) return;
        // Explicit extrema retain zero-width/height geometry such as lines.
        result = result ? QRectF(QPointF(std::min(result->left(), bounds.left()),
                                         std::min(result->top(), bounds.top())),
                                 QPointF(std::max(result->right(), bounds.right()),
                                         std::max(result->bottom(), bounds.bottom())))
                        : bounds;
    };
    for (const auto& entity : m_entities) {
        if (!entity.selected) continue;
        const auto preview = m_move_preview_delta && m_move_ids.contains(entity.id)
            ? *m_move_preview_delta : Vec2{};
        const auto width = entity.type == QStringLiteral("wall")
            ? std::max(entity.thickness_metres, 0.04) : entity.stroke_width_metres;
        const auto padding = std::isfinite(width) && width > 0.0 ? width * m_scale * 0.5 : 1.5;
        for (const auto& segment : entity.segments) {
            Vec2 minimum = segment.start;
            Vec2 maximum = segment.end;
            try {
                const auto bounds = segment_bounds(segment);
                minimum = bounds.minimum;
                maximum = bounds.maximum;
            } catch (const std::invalid_argument&) {
                // Match the finite endpoint fallback used for view fitting.
            }
            minimum.x += preview.x;
            minimum.y += preview.y;
            maximum.x += preview.x;
            maximum.y += preview.y;
            include(QRectF(toScreen(minimum, viewport), toScreen(maximum, viewport))
                        .normalized().adjusted(-padding, -padding, padding, padding));
        }
    }
    for (const auto& label : m_labels) {
        if (!label.selected || !drawable_label(label)) continue;
        const auto layout = label_layout(label, font(), this, m_scale, logicalDpiY());
        auto position = label.position;
        if (m_move_preview_delta && m_move_ids.contains(label.id)) {
            position.x += m_move_preview_delta->x;
            position.y += m_move_preview_delta->y;
        }
        include(label_transform(label, toScreen(position, viewport)).mapRect(layout.bounds));
    }
    for (const auto& reference : m_references) {
        if (!reference.selected || !reference.visible || reference.image.isNull()) continue;
        const auto unit = reference.metres_per_source_unit * reference.scale;
        if (!std::isfinite(unit) || unit <= 0.0) continue;
        QTransform transform;
        transform.translate(viewport.center().x(), viewport.center().y());
        transform.scale(m_scale, -m_scale);
        auto position = reference.position;
        if (m_move_preview_delta && m_move_ids.contains(reference.id)) {
            position.x += m_move_preview_delta->x;
            position.y += m_move_preview_delta->y;
        }
        transform.translate(position.x - m_view_center.x,
                            position.y - m_view_center.y);
        if (std::isfinite(reference.rotation_degrees)) transform.rotate(reference.rotation_degrees);
        const auto width = reference.image.width() * unit;
        const auto height = reference.image.height() * unit;
        include(transform.mapRect(QRectF(-width * 0.5, -height * 0.5, width, height)));
    }
    if (result) *result = result->adjusted(-6.0, -6.0, 6.0, 6.0);
    return result;
}

void PlanCanvas::drawSelectionFrame(QPainter& painter, const QRectF& viewport) const {
    const auto bounds = selectionBounds(viewport);
    if (!bounds) return;
    // Keep a frame visible when zooming into a large selected item. The API
    // retains its true bounds; only the painted frame hugs the visible extent.
    const auto frame = bounds->intersected(viewport.adjusted(5.0, 5.0, -5.0, -5.0));
    if (frame.isEmpty()) return;
    painter.save();
    painter.setClipRect(viewport, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    // A white halo keeps the blue frame legible over dark fills and underlays.
    painter.setPen(QPen(QColor(255, 255, 255, 235), 4.0));
    painter.drawRect(frame);
    painter.setPen(QPen(QColor(37, 99, 235), 1.5));
    painter.drawRect(frame);
    painter.restore();
}

void PlanCanvas::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    const auto pending = std::exchange(m_pending_measurements, {});
    const auto generation = m_measurement_generation;
    {
        QPainter painter(this);
        renderScene(painter, QRectF(rect()));
        if (m_selection_start && m_selection_dragging) {
            painter.setPen(QPen(QColor(37, 99, 235), 1.5, Qt::DashLine));
            painter.setBrush(QColor(37, 99, 235, 38));
            painter.drawRect(QRectF(*m_selection_start, m_selection_end).normalized());
        }
        if (m_last_mouse_position) {
            if (const auto anchor = closingAnchor(*m_last_mouse_position)) {
                const auto screen = toScreen(*anchor, rect());
                painter.setPen(QPen(QColor(22, 163, 74), 2.0));
                painter.setBrush(QColor(22, 163, 74, 60));
                painter.drawEllipse(screen, 10.0, 10.0);
                painter.drawText(screen + QPointF(14.0, -12.0), tr("Click to close"));
            }
        }
    }
    const auto completed = PerformanceClock::now();
    const auto callback = m_performance_measured;
    if (!callback || !isVisible() || QApplication::activeModalWidget()) return;
    for (const auto& [metric, started] : pending) {
        // A reset from a callback must also discard the remainder of this batch.
        if (generation != m_measurement_generation) break;
        callback(metric, completed - started);
    }
}

void PlanCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->source() != Qt::MouseEventNotSynthesized) {
        event->accept();
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::input);
    pointerPress(event->position(), event->button(), event->modifiers());
    event->accept();
}

void PlanCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (event->source() != Qt::MouseEventNotSynthesized) {
        event->accept();
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::input);
    pointerMove(event->position());
    event->accept();
}

void PlanCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->source() != Qt::MouseEventNotSynthesized) {
        event->accept();
        return;
    }
    beginPerformanceMeasurement(PerformanceMetric::input);
    pointerRelease(event->position(), event->button());
    event->accept();
}

void PlanCanvas::wheelEvent(QWheelEvent* event) {
    beginPerformanceMeasurement(PerformanceMetric::input);
    const auto steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (steps != 0.0) {
        zoomBy(std::pow(1.18, steps), event->position());
    }
    event->accept();
}

void PlanCanvas::keyPressEvent(QKeyEvent* event) {
    // Precise input opens a dialog; exclude the entire dispatch even if a
    // supplied callback happens to be nonmodal (for example in an embedder).
    if (event->key() == Qt::Key_D && m_tool == CanvasTool::boundary) {
        resetPerformanceMeasurements();
    } else if ((event->matches(QKeySequence::Undo) && m_draft_undo_requested) ||
               (event->matches(QKeySequence::Redo) && m_draft_redo_requested) ||
               event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
               event->key() == Qt::Key_Escape || event->key() == Qt::Key_F) {
        beginPerformanceMeasurement(PerformanceMetric::input);
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_space_pan_armed = true;
        if (m_gesture_button == Qt::NoButton) setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Undo) && m_draft_undo_requested) {
        m_draft_undo_requested();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo) && m_draft_redo_requested) {
        m_draft_redo_requested();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_finish_requested) {
            m_finish_requested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (m_gesture_button != Qt::NoButton) {
            resetGesture();
            event->accept();
            return;
        }
        if (m_cancel_requested) {
            m_cancel_requested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_D && m_tool == CanvasTool::boundary) {
        if (m_precise_input_requested) {
            m_precise_input_requested();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F) {
        fitView();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PlanCanvas::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_space_pan_armed = false;
        if (m_gesture_button == Qt::NoButton) {
            if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
            else setCursor(Qt::CrossCursor);
        }
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void PlanCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_last_mouse_position) updateCursor(*m_last_mouse_position);
    update();
}

QPointF PlanCanvas::toScreen(Vec2 point, const QRectF& viewport) const {
    return {viewport.center().x() + (point.x - m_view_center.x) * m_scale,
            viewport.center().y() - (point.y - m_view_center.y) * m_scale};
}

Vec2 PlanCanvas::toModel(QPointF point, const QRectF& viewport) const {
    return {m_view_center.x + (point.x() - viewport.center().x()) / m_scale,
            m_view_center.y - (point.y() - viewport.center().y()) / m_scale};
}

Vec2 PlanCanvas::snapped(Vec2 point) const {
    if (!m_snap_enabled) {
        return point;
    }
    constexpr double grid = 0.25;
    return {std::round(point.x / grid) * grid, std::round(point.y / grid) * grid};
}

std::optional<Vec2> PlanCanvas::closingAnchor(QPointF point) const {
    if (m_tool != CanvasTool::boundary || !m_boundary_draft_preview ||
        !m_boundary_draft_preview->can_close_on_anchor ||
        !m_boundary_draft_preview->anchor) return std::nullopt;
    const auto anchor = *m_boundary_draft_preview->anchor;
    const auto offset = point - toScreen(anchor, rect());
    // A fixed pixel target remains easy to hit at any zoom and takes priority
    // over grid rounding, including with grid snap disabled.
    return std::hypot(offset.x(), offset.y()) <= 12.0
        ? std::optional{anchor} : std::nullopt;
}

Vec2 PlanCanvas::inputPoint(QPointF point) const {
    if (const auto anchor = closingAnchor(point)) return *anchor;
    return snapped(toModel(point, rect()));
}

QStringList PlanCanvas::rectangleHits(const QRectF& rectangle, bool crossing) const {
    QStringList result;
    const auto add = [&](const QString& id) {
        if (!id.isEmpty() && !result.contains(id)) result.push_back(id);
    };
    const auto matches = [&](const QPainterPath& path) {
        return !path.isEmpty() && (crossing ? path.intersects(rectangle)
                                           : rectangle.contains(path.boundingRect()));
    };
    QTransform model_to_screen;
    model_to_screen.translate(QRectF(rect()).center().x(), QRectF(rect()).center().y());
    model_to_screen.scale(m_scale, -m_scale);
    model_to_screen.translate(-m_view_center.x, -m_view_center.y);
    for (const auto& entity : m_entities) {
        QPainterPath path;
        for (const auto& segment : entity.segments) {
            path.moveTo(segment.start.x, segment.start.y);
            if (segment.sweep_radians == 0.0) {
                path.lineTo(segment.end.x, segment.end.y);
            } else if (const auto arc = arc_info(segment)) {
                path.arcTo(QRectF(arc->center.x - arc->radius, arc->center.y - arc->radius,
                                 2.0 * arc->radius, 2.0 * arc->radius),
                           -arc->start_angle * 180.0 / pi,
                           -segment.sweep_radians * 180.0 / pi);
            }
        }
        QPainterPathStroker stroker;
        const auto width = entity.type == QStringLiteral("wall")
            ? std::max(entity.thickness_metres, 0.04) * m_scale
            : entity.stroke_width_metres * m_scale;
        stroker.setWidth(std::isfinite(width) ? std::max(3.0, width) : 3.0);
        // Stroke open paths before intersection so Qt cannot implicitly fill
        // an open chain and select empty space between unrelated segments.
        auto screen_path = stroker.createStroke(model_to_screen.map(path));
        if (entity.filled) {
            if (const auto fill = closed_entity_path(entity))
                screen_path.addPath(model_to_screen.map(*fill));
        }
        if (matches(screen_path)) add(entity.id);
    }
    for (const auto& label : m_labels) {
        if (!drawable_label(label)) continue;
        QPainterPath path;
        path.addRect(label_layout(label, font(), this, m_scale, logicalDpiY()).bounds);
        if (matches(label_transform(label, toScreen(label.position, rect())).map(path)))
            add(label.id);
    }
    for (const auto& reference : m_references) {
        const auto unit = reference.metres_per_source_unit * reference.scale;
        if (!reference.visible || reference.image.isNull() ||
            !std::isfinite(reference.position.x) || !std::isfinite(reference.position.y) ||
            !std::isfinite(reference.rotation_degrees) || !std::isfinite(unit) || unit <= 0.0) continue;
        QTransform transform = model_to_screen;
        transform.translate(reference.position.x, reference.position.y);
        transform.rotate(reference.rotation_degrees);
        QPainterPath path;
        path.addRect(QRectF(-reference.image.width() * unit * 0.5,
                            -reference.image.height() * unit * 0.5,
                            reference.image.width() * unit, reference.image.height() * unit));
        if (matches(transform.map(path))) add(reference.id);
    }
    return result;
}

QString PlanCanvas::hitTest(QPointF point) const {
    constexpr double hit_pixels = 9.0;
    QString result;
    auto best = std::numeric_limits<double>::max();
    for (const auto& entity : m_entities) {
        QPainterPath painted_footprint;
        for (const auto& segment : entity.segments) {
            const auto start = toScreen(segment.start, rect());
            painted_footprint.moveTo(start);
            if (segment.sweep_radians == 0.0) {
                const auto end = toScreen(segment.end, rect());
                painted_footprint.lineTo(end);
                const auto candidate = point_segment_distance(point, start, end);
                if (candidate < best) {
                    best = candidate;
                    result = entity.id;
                }
                continue;
            }
            const auto arc = arc_info(segment);
            if (!arc.has_value()) {
                continue;
            }
            auto previous = start;
            constexpr int samples = 40;
            for (int index = 1; index <= samples; ++index) {
                const auto current = toScreen(
                    arc_point(segment, *arc, static_cast<double>(index) / samples), rect());
                painted_footprint.lineTo(current);
                const auto candidate = point_segment_distance(point, previous, current);
                if (candidate < best) {
                    best = candidate;
                    result = entity.id;
                }
                previous = current;
            }
        }
        // Plan components are picked by their complete painted footprint, not
        // only by a thin stroke. This keeps an empty-looking seat cushion or
        // appliance centre from being misclassified as canvas space and panned.
        if (entity.type == QStringLiteral("symbol") && !painted_footprint.isEmpty() &&
            painted_footprint.boundingRect().adjusted(-4.0, -4.0, 4.0, 4.0).contains(point)) {
            best = 0.0;
            result = entity.id;
        }
    }
    // Measure the same font and padded rotated rectangle as interactive paint.
    // Retain the geometry selection tolerance outside that painted rectangle.
    for (const auto& label : m_labels) {
        if (!drawable_label(label)) continue;
        const auto screen = toScreen(label.position, rect());
        const auto layout = label_layout(label, font(), this, m_scale, logicalDpiY());
        const auto local = label_transform(label, screen).inverted().map(point);
        const auto dx = std::max({layout.bounds.left() - local.x(), 0.0,
                                  local.x() - layout.bounds.right()});
        const auto dy = std::max({layout.bounds.top() - local.y(), 0.0,
                                  local.y() - layout.bounds.bottom()});
        const auto candidate = std::hypot(dx, dy);
        // Labels paint after geometry; a hit inside their painted rectangle
        // wins a zero-distance tie, including later overlapping labels.
        if (candidate < best || candidate == 0.0) {
            best = candidate;
            result = label.id;
        }
    }
    if (best <= hit_pixels) return result;
    const auto model = toModel(point, rect());
    // Underlays sit beneath geometry; pick the topmost visible reference only
    // when no authored geometry or label was hit.
    for (auto it = m_references.rbegin(); it != m_references.rend(); ++it) {
        const auto& reference = *it;
        if (!reference.visible || reference.image.isNull() ||
            !std::isfinite(reference.rotation_degrees)) continue;
        const auto radians = reference.rotation_degrees * std::numbers::pi / 180.0;
        const auto dx = model.x - reference.position.x;
        const auto dy = model.y - reference.position.y;
        const auto x = dx * std::cos(radians) + dy * std::sin(radians);
        const auto y = -dx * std::sin(radians) + dy * std::cos(radians);
        const auto unit = reference.metres_per_source_unit * reference.scale;
        if (std::isfinite(unit) && unit > 0.0 &&
            std::abs(x) <= reference.image.width() * unit * 0.5 &&
            std::abs(y) <= reference.image.height() * unit * 0.5) return reference.id;
    }
    return {};
}

QStringList PlanCanvas::selectedIds() const {
    QStringList result;
    const auto add = [&](const QString& id) {
        if (!id.isEmpty() && !result.contains(id)) result.push_back(id);
    };
    for (const auto& entity : m_entities) if (entity.selected) add(entity.id);
    for (const auto& label : m_labels) if (label.selected) add(label.id);
    for (const auto& reference : m_references) if (reference.selected) add(reference.id);
    return result;
}

Vec2 PlanCanvas::dragDelta(QPointF position) const {
    const auto start = toModel(m_left_start, rect());
    const auto end = toModel(position, rect());
    if (!m_snap_enabled) return {end.x - start.x, end.y - start.y};
    const auto snapped_start = snapped(start);
    const auto snapped_end = snapped(end);
    return {snapped_end.x - snapped_start.x, snapped_end.y - snapped_start.y};
}

void PlanCanvas::updateCursor(QPointF point) {
    m_last_mouse_position = point;
    if (m_cursor_moved) {
        m_cursor_moved(inputPoint(point));
    }
    if (m_panning || (m_left_gesture == LeftGesture::object_move && m_left_dragging)) {
        setCursor(Qt::ClosedHandCursor);
    } else if (m_space_pan_armed && m_gesture_button == Qt::NoButton) {
        setCursor(Qt::OpenHandCursor);
    } else if (m_tool == CanvasTool::select && m_gesture_button == Qt::NoButton) {
        const auto target = hitTest(point);
        setCursor(target.isEmpty() ? Qt::CrossCursor
                                   : selectedIds().contains(target) ? Qt::SizeAllCursor
                                                                    : Qt::ArrowCursor);
    } else if (m_tool != CanvasTool::select && m_gesture_button == Qt::NoButton) {
        setCursor(Qt::CrossCursor);
    }
    update();
}

void PlanCanvas::drawGrid(QPainter& painter, const QRectF& viewport, double scale,
                          Vec2 view_center) const {
    const auto to_model = [&](QPointF point) {
        return Vec2{view_center.x + (point.x() - viewport.center().x()) / scale,
                    view_center.y - (point.y() - viewport.center().y()) / scale};
    };
    const auto top_left = to_model(viewport.topLeft());
    const auto bottom_right = to_model(viewport.bottomRight());
    const auto min_x = std::min(top_left.x, bottom_right.x);
    const auto max_x = std::max(top_left.x, bottom_right.x);
    const auto min_y = std::min(top_left.y, bottom_right.y);
    const auto max_y = std::max(top_left.y, bottom_right.y);

    double step = 0.25;
    const auto desired_world_spacing = 38.0 / scale;
    while (step < desired_world_spacing) {
        step *= 2.0;
    }
    const bool light_surface = m_canvas_background.lightnessF() > 0.5;
    QPen minor(light_surface ? QColor(229, 235, 243) : QColor(45, 53, 65), 0.0);
    QPen major(light_surface ? QColor(207, 218, 232) : QColor(57, 67, 81), 0.0);
    const auto first_x = std::floor(min_x / step) * step;
    const auto first_y = std::floor(min_y / step) * step;
    for (auto x = first_x; x <= max_x + step; x += step) {
        const auto index = std::round(x / step);
        painter.setPen(std::fmod(std::abs(index), 5.0) < 0.001 ? major : minor);
        painter.drawLine(QLineF(x, min_y, x, max_y));
    }
    for (auto y = first_y; y <= max_y + step; y += step) {
        const auto index = std::round(y / step);
        painter.setPen(std::fmod(std::abs(index), 5.0) < 0.001 ? major : minor);
        painter.drawLine(QLineF(min_x, y, max_x, y));
    }
}

void PlanCanvas::drawReferenceGrids(QPainter& painter) const {
    const bool light_surface = m_canvas_background.lightnessF() > 0.5;
    const QColor minor_color = light_surface ? QColor(121, 149, 181, 135)
                                             : QColor(136, 178, 221, 155);
    const QColor major_color = light_surface ? QColor(65, 111, 157, 205)
                                             : QColor(176, 214, 244, 220);
    for (const auto& grid : m_reference_grids) {
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            if (!std::isfinite(line.start.x) || !std::isfinite(line.start.y) ||
                !std::isfinite(line.end.x) || !std::isfinite(line.end.y)) {
                continue;
            }
            QPen pen(line.major ? major_color : minor_color,
                     line.major ? 1.35 : 0.75, Qt::SolidLine,
                     Qt::SquareCap, Qt::MiterJoin);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.drawLine(QLineF(line.start.x, line.start.y, line.end.x, line.end.y));
        }
    }
}

void PlanCanvas::drawReferenceGridLabels(QPainter& painter, const QRectF& viewport,
                                         double scale, Vec2 view_center, bool output,
                                         QColor background,
                                         std::optional<double> paper_pixels_per_mm) const {
    if (!(scale > 0.0) || !std::isfinite(scale)) return;
    const auto to_screen = [&](Vec2 point) {
        return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                       viewport.center().y() - (point.y - view_center.y) * scale);
    };

    double dpi = output ? painter.device()->logicalDpiY() : logicalDpiY();
    if (output && paper_pixels_per_mm.has_value() && *paper_pixels_per_mm > 0.0 &&
        std::isfinite(*paper_pixels_per_mm * 25.4)) {
        dpi = *paper_pixels_per_mm * 25.4;
    }
    const auto pixel_height = output
        ? std::clamp(std::lround(2.5 * dpi / 25.4), 8L, 48L)
        : 11L;
    QFont font = painter.font();
    font.setPixelSize(static_cast<int>(pixel_height));
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font, painter.device());
    const bool light_surface = background.lightnessF() > 0.5;
    const auto foreground = light_surface ? QColor(47, 83, 121, 235)
                                          : QColor(202, 226, 246, 245);
    const auto fill = light_surface ? QColor(255, 255, 255, 228)
                                    : QColor(18, 30, 45, 232);
    const auto border = light_surface ? QColor(134, 164, 194, 210)
                                      : QColor(93, 132, 171, 230);

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setClipRect(viewport);
    painter.setFont(font);
    for (const auto& grid : m_reference_grids) {
        if (!grid.visible) continue;
        for (const auto& line : grid.lines) {
            const auto& prefix = line.axis == ReferenceGridAxis::x ? grid.x_label : grid.y_label;
            if (prefix.isEmpty() || !std::isfinite(line.start.x) ||
                !std::isfinite(line.start.y) || !std::isfinite(line.end.x) ||
                !std::isfinite(line.end.y)) {
                continue;
            }
            const auto start = to_screen(line.start);
            const auto end = to_screen(line.end);
            const auto outward = line.axis == ReferenceGridAxis::x
                ? end - start
                : start - end;
            const auto length = std::hypot(outward.x(), outward.y());
            if (!(length > 0.0) || !std::isfinite(length)) continue;
            const auto unit = outward / length;
            auto anchor = line.axis == ReferenceGridAxis::x ? end : start;
            anchor += unit * 6.0;
            const auto text = prefix + QString::number(line.index);
            auto bounds = metrics.boundingRect(text);
            bounds.moveCenter(anchor);
            bounds.adjust(-4.0, -2.0, 4.0, 2.0);
            painter.setPen(QPen(border, 1.0));
            painter.setBrush(fill);
            painter.drawRoundedRect(bounds, 3.0, 3.0);
            painter.setPen(foreground);
            painter.setBrush(Qt::NoBrush);
            painter.drawText(bounds, Qt::AlignCenter, text);
        }
    }
    painter.restore();
}

void PlanCanvas::drawCursorReadout(QPainter& painter, const QRectF& viewport,
                                   QColor background) const {
    if (!m_last_mouse_position || m_tool == CanvasTool::select ||
        !viewport.contains(*m_last_mouse_position)) {
        return;
    }
    const auto point = inputPoint(*m_last_mouse_position);
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;

    QString text = QStringLiteral("X %1   Y %2")
        .arg(display_cursor_length(point.x, m_metric_units),
             display_cursor_length(point.y, m_metric_units));
    if (m_boundary_draft_preview && m_boundary_draft_preview->anchor) {
        const auto anchor = *m_boundary_draft_preview->anchor;
        const auto dx = point.x - anchor.x;
        const auto dy = point.y - anchor.y;
        const auto length = std::hypot(dx, dy);
        if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(length)) {
            const auto angle = std::atan2(dy, dx) * 180.0 / pi;
            text += QStringLiteral("\nΔ %1   ∠ %2°")
                .arg(display_cursor_length(length, m_metric_units))
                .arg(angle, 0, 'f', 1);
        }
    }

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font = painter.font();
    font.setPixelSize(11);
    font.setWeight(QFont::Medium);
    painter.setFont(font);
    const QFontMetricsF metrics(font, painter.device());
    const QRectF text_bounds = metrics.boundingRect(QRectF(), Qt::TextExpandTabs, text);
    const QRectF panel_bounds = text_bounds.adjusted(-9.0, -6.0, 9.0, 6.0);
    const bool light_surface = background.lightnessF() > 0.5;
    const auto border = light_surface ? QColor(176, 191, 211, 235)
                                      : QColor(98, 119, 149, 245);
    const auto surface = light_surface ? QColor(255, 255, 255, 244)
                                       : QColor(24, 34, 49, 246);
    const auto foreground = light_surface ? QColor(31, 48, 69)
                                          : QColor(232, 240, 251);
    QPointF top_left = *m_last_mouse_position + QPointF(14.0, 14.0);
    if (top_left.x() + panel_bounds.width() > viewport.right() - 8.0) {
        top_left.setX(m_last_mouse_position->x() - panel_bounds.width() - 14.0);
    }
    if (top_left.y() + panel_bounds.height() > viewport.bottom() - 8.0) {
        top_left.setY(m_last_mouse_position->y() - panel_bounds.height() - 14.0);
    }
    top_left.setX(std::clamp(top_left.x(), viewport.left() + 8.0,
                             viewport.right() - panel_bounds.width() - 8.0));
    top_left.setY(std::clamp(top_left.y(), viewport.top() + 8.0,
                             viewport.bottom() - panel_bounds.height() - 8.0));
    const QRectF panel(top_left, panel_bounds.size());
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(surface);
    painter.drawRoundedRect(panel, 6.0, 6.0);
    painter.setPen(foreground);
    painter.drawText(panel.adjusted(9.0, 6.0, -9.0, -6.0),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, text);
    painter.restore();
}

void PlanCanvas::drawEntity(QPainter& painter, const CanvasEntity& entity, bool output,
                           QColor background,
                           std::optional<double> paper_pixels_per_mm) const {
    const auto default_color = output
        ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25) : QColor(235, 235, 235))
        : color_for(entity);
    const auto color = entity.stroke_color.isValid() ? entity.stroke_color : default_color;
    QPen pen(color, 0.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const auto paper_width = output && std::isfinite(entity.output_stroke_width_mm) &&
                             entity.output_stroke_width_mm > 0.0
        ? entity.output_stroke_width_mm *
              (paper_pixels_per_mm && std::isfinite(*paper_pixels_per_mm) &&
                       *paper_pixels_per_mm > 0.0
                   ? *paper_pixels_per_mm
                   : painter.device()->logicalDpiX() / 25.4)
        : 0.0;
    if (paper_width > 0.0 && std::isfinite(paper_width)) {
        // Output line treatment is a paper-space width. Cosmetic pens keep it
        // independent of the model-to-paper scale and avoid altering geometry.
        pen.setCosmetic(true);
        pen.setWidthF(std::max(0.1, paper_width));
    } else if (!output && entity.type == QStringLiteral("symbol")) {
        // Library components remain legible at normal plan zoom without
        // turning their persisted geometry into model-space wall thickness.
        pen.setCosmetic(true);
        pen.setWidthF(std::max(1.15, entity.stroke_width_metres * m_scale));
    } else if (entity.type == QStringLiteral("wall")) {
        pen.setWidthF(std::max(entity.thickness_metres, 0.04));
    } else if (entity.stroke_width_metres > 0.0 &&
               std::isfinite(entity.stroke_width_metres)) {
        pen.setWidthF(entity.stroke_width_metres);
    } else {
        // Boundary line weight is a presentation size, never a model-space
        // wall thickness. Printed output uses a quarter-millimetre stroke.
        pen.setCosmetic(true);
        pen.setWidthF(output ? painter.device()->logicalDpiX() * 0.25 / 25.4
                             : entity.selected ? 3.0 : 1.5);
    }
    if (entity.filled) {
        if (const auto fill_path = closed_entity_path(entity)) {
            const auto style = hatch_style(entity.hatch_pattern);
            if (style != Qt::NoBrush) {
                auto fill_color = entity.fill_color.isValid()
                                      ? entity.fill_color
                                      : output
                                          ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25)
                                                                             : QColor(235, 235, 235))
                                          : color;
                fill_color.setAlpha(output ? 64 : 48);
                QBrush brush(fill_color, style);
                const auto scale = std::isfinite(entity.hatch_scale) && entity.hatch_scale > 0.0
                                        ? std::clamp(entity.hatch_scale, 0.1, 10.0)
                                        : 1.0;
                QTransform brush_transform;
                brush_transform.scale(scale, scale);
                brush.setTransform(brush_transform);
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(brush);
                painter.drawPath(*fill_path);
                painter.restore();
            }
        }
    }
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPainterPath path;
    for (const auto& segment : entity.segments) {
        // Start each semantic segment independently. This preserves opening
        // gaps when a wall baseline has been split around hosted openings and
        // avoids connecting unrelated entities in one painter path.
        path.moveTo(segment.start.x, segment.start.y);
        if (segment.sweep_radians == 0.0) {
            path.lineTo(segment.end.x, segment.end.y);
            continue;
        }
        const auto arc = arc_info(segment);
        if (!arc.has_value()) {
            return;
        }
        const QRectF bounds(arc->center.x - arc->radius, arc->center.y - arc->radius,
                           arc->radius * 2.0, arc->radius * 2.0);
        // QPainterPath defines arc angles with a screen-style inverted Y.
        // Negate both angles in our Cartesian path before the view transform.
        path.arcTo(bounds, -arc->start_angle * 180.0 / pi,
                   -segment.sweep_radians * 180.0 / pi);
    }
    if (!entity.segments.empty()) {
        painter.drawPath(path);
    }
}

void PlanCanvas::drawSegment(QPainter& painter, const Segment& segment) const {
    QPainterPath path;
    path.moveTo(segment.start.x, segment.start.y);
    if (segment.sweep_radians == 0.0) {
        path.lineTo(segment.end.x, segment.end.y);
    } else if (const auto arc = arc_info(segment)) {
        const QRectF bounds(arc->center.x - arc->radius, arc->center.y - arc->radius,
                            arc->radius * 2.0, arc->radius * 2.0);
        path.arcTo(bounds, -arc->start_angle * 180.0 / pi,
                   -segment.sweep_radians * 180.0 / pi);
    } else {
        return;
    }
    painter.drawPath(path);
}

void PlanCanvas::drawLabels(QPainter& painter, const QRectF& viewport, double scale,
                            Vec2 view_center, bool output, QColor background,
                            std::optional<double> paper_pixels_per_mm) const {
    if (m_labels.empty() || !(scale > 0.0) || !std::isfinite(scale)) {
        return;
    }
    const auto to_screen = [&](Vec2 point) {
        return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                       viewport.center().y() - (point.y - view_center.y) * scale);
    };

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const auto legacy_font = painter.font();
    const auto* metrics_device = output ? painter.device() : static_cast<const QPaintDevice*>(this);
    double dpi = output ? painter.device()->logicalDpiY() : logicalDpiY();
    if (output && paper_pixels_per_mm.has_value() && *paper_pixels_per_mm > 0.0 &&
        std::isfinite(*paper_pixels_per_mm * 25.4)) {
        // Fitted sheet previews need their page's paper transform, which can
        // differ from device DPI. Model scale remains independent of this.
        dpi = *paper_pixels_per_mm * 25.4;
    }
    for (const auto& label : m_labels) {
        if (!drawable_label(label)) continue;
        const auto paper = std::isfinite(label.paper_height_mm) && label.paper_height_mm > 0.0;
        const auto layout = label_layout(label, paper ? font() : legacy_font,
                                          metrics_device, scale, dpi);
        painter.setFont(layout.font);
        const auto& bounds = layout.bounds;
        auto position = label.position;
        if (!output && m_move_preview_delta && m_move_ids.contains(label.id)) {
            position.x += m_move_preview_delta->x;
            position.y += m_move_preview_delta->y;
        }
        const auto center = to_screen(position);
        painter.save();
        painter.setTransform(label_transform(label, center), true);
        if (!output && label.selected) {
            painter.setPen(QPen(QColor(37, 99, 235), 1.0));
        } else {
            painter.setPen(Qt::NoPen);
        }
        const auto label_background = output
            ? background
            : background.lightnessF() > 0.5
                ? QColor(255, 255, 255, 238)
                : QColor(20, 25, 34, 225);
        const auto label_fill_style = label.fill_color.isValid()
            ? hatch_style(label.fill_pattern)
            : Qt::NoBrush;
        const auto custom_fill = label.fill_color.isValid() &&
                                 label_fill_style != Qt::NoBrush;
        auto brush = QBrush(custom_fill ? label.fill_color : label_background,
                            custom_fill ? label_fill_style : Qt::SolidPattern);
        if (custom_fill) {
            auto fill = label.fill_color;
            fill.setAlpha(output ? 220 : 238);
            brush.setColor(fill);
        }
        painter.setBrush(brush);
        painter.drawRoundedRect(bounds, 3.0, 3.0);
        painter.setPen(label.color.isValid() ? label.color
                       : output ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25)
                                                                : QColor(255, 239, 172))
                              : label.selected ? QColor(37, 99, 235)
                                                : background.lightnessF() > 0.5
                                                    ? QColor(50, 65, 84)
                                                    : QColor(255, 239, 172));
        painter.setBrush(Qt::NoBrush);
        painter.drawText(bounds, Qt::AlignCenter, label.text);
        painter.restore();
    }
    painter.restore();
}

void PlanCanvas::drawReference(QPainter& painter, const CanvasReference& reference) const {
    if (reference.image.isNull() || !std::isfinite(reference.position.x) ||
        !std::isfinite(reference.position.y) ||
        !(reference.metres_per_source_unit > 0.0) || !(reference.scale > 0.0) ||
        !std::isfinite(reference.metres_per_source_unit) || !std::isfinite(reference.scale)) {
        return;
    }
    auto image = reference.image;
    if (reference.flip_horizontal || reference.flip_vertical) {
        image = image.mirrored(reference.flip_horizontal, reference.flip_vertical);
    }
    const auto width = image.width() * reference.metres_per_source_unit * reference.scale;
    const auto height = image.height() * reference.metres_per_source_unit * reference.scale;
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) || !std::isfinite(height)) {
        return;
    }
    painter.save();
    painter.translate(reference.position.x, reference.position.y);
    if (std::isfinite(reference.rotation_degrees)) {
        painter.rotate(reference.rotation_degrees);
    }
    painter.setOpacity(std::clamp(reference.intensity, 0.0, 1.0));
    painter.drawImage(QRectF(-width * 0.5, -height * 0.5, width, height), image);
    painter.restore();
}

}  // namespace sketch::desktop
