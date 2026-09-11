#include "plan_canvas.hpp"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace sketch::desktop {
namespace {

constexpr double minimum_scale = 8.0;
constexpr double maximum_scale = 4000.0;
constexpr double output_minimum_scale = 0.05;
constexpr double pi = std::numbers::pi;

double distance(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y);
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
    return QColor(182, 191, 205);
}

}  // namespace

PlanCanvas::PlanCanvas(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(480, 360);
    setMouseTracking(true);
    setAutoFillBackground(false);
}

void PlanCanvas::setEntities(std::vector<CanvasEntity> entities) {
    m_entities = std::move(entities);
    update();
}

void PlanCanvas::setTool(CanvasTool tool) {
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

void PlanCanvas::setMetricUnits(bool metric) {
    m_metric_units = metric;
    update();
}

void PlanCanvas::setSelectedId(const QString& entity_id) {
    m_selected_id = entity_id;
    for (auto& entity : m_entities) {
        entity.selected = entity.id == m_selected_id;
    }
    update();
}

void PlanCanvas::setLabels(std::vector<CanvasLabel> labels) {
    m_labels = std::move(labels);
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

void PlanCanvas::fitView() {
    if (m_entities.empty() && m_labels.empty()) {
        m_view_center = {0.0, 0.0};
        m_scale = 80.0;
        update();
        return;
    }

    Vec2 minimum{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2 maximum{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    bool has_content = false;
    const auto include = [&](Vec2 point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            return;
        }
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
            if (const auto arc = arc_info(segment)) {
                for (int index = 1; index < 32; ++index) {
                    include(arc_point(segment, *arc, static_cast<double>(index) / 32.0));
                }
            }
        }
    }
    for (const auto& label : m_labels) {
        include(label.position);
    }
    if (!has_content) {
        m_view_center = {0.0, 0.0};
        m_scale = 80.0;
        update();
        return;
    }
    const auto width = std::max(maximum.x - minimum.x, 0.1);
    const auto height = std::max(maximum.y - minimum.y, 0.1);
    const auto padding = std::max(width, height) * 0.12 + 0.25;
    m_view_center = {(minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5};
    m_scale = std::clamp(std::min((width + padding * 2.0) > 0.0
                                      ? std::max(1.0, static_cast<double>(size().width())) /
                                            (width + padding * 2.0)
                                      : 80.0,
                                  (height + padding * 2.0) > 0.0
                                      ? std::max(1.0, static_cast<double>(size().height())) /
                                            (height + padding * 2.0)
                                      : 80.0),
                        minimum_scale, maximum_scale);
    update();
}

void PlanCanvas::zoomBy(double factor, QPointF anchor) {
    if (!(factor > 0.0) || !std::isfinite(factor)) {
        return;
    }
    if (anchor.isNull()) {
        anchor = rect().center();
    }
    const auto before = toModel(anchor, rect());
    m_scale = std::clamp(m_scale * factor, minimum_scale, maximum_scale);
    const auto after = toModel(anchor, rect());
    m_view_center = m_view_center + (before - after);
    update();
}

void PlanCanvas::renderScene(QPainter& painter, const QRectF& viewport) const {
    renderSceneWithTransform(painter, viewport, false, QColor(24, 29, 37),
                             std::nullopt, std::nullopt);
}

void PlanCanvas::renderScene(QPainter& painter, const QRectF& viewport, bool fit_to_content,
                             QColor background) const {
    renderSceneWithTransform(painter, viewport, fit_to_content, background,
                             std::nullopt, std::nullopt);
}

void PlanCanvas::renderSceneAt(QPainter& painter, const QRectF& viewport, double scale,
                               Vec2 view_center, QColor background) const {
    if (!(std::isfinite(scale) && scale > 0.0) || !std::isfinite(view_center.x) ||
        !std::isfinite(view_center.y)) {
        return;
    }
    renderSceneWithTransform(painter, viewport, false, background, scale, view_center);
}

Vec2 PlanCanvas::contentCenter() const noexcept {
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
            if (const auto arc = arc_info(segment)) {
                for (int index = 1; index < 32; ++index)
                    include(arc_point(segment, *arc, static_cast<double>(index) / 32.0));
            }
        }
    }
    for (const auto& label : m_labels) include(label.position);
    return has_content ? Vec2{(minimum.x + maximum.x) * 0.5,
                              (minimum.y + maximum.y) * 0.5}
                       : m_view_center;
}

void PlanCanvas::renderSceneWithTransform(QPainter& painter, const QRectF& viewport,
                                          bool fit_to_content, QColor background,
                                          std::optional<double> explicit_scale,
                                          std::optional<Vec2> explicit_center) const {
    if (viewport.width() <= 0.0 || viewport.height() <= 0.0) {
        return;
    }
    auto scale = explicit_scale.value_or(m_scale);
    auto view_center = explicit_center.value_or(m_view_center);
    if (!explicit_scale.has_value() && fit_to_content && (!m_entities.empty() || !m_labels.empty())) {
        Vec2 minimum{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
        Vec2 maximum{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
        bool has_content = false;
        const auto include = [&](Vec2 point) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                return;
            }
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
                if (const auto arc = arc_info(segment)) {
                    for (int index = 1; index < 32; ++index) {
                        include(arc_point(segment, *arc, static_cast<double>(index) / 32.0));
                    }
                }
            }
        }
        for (const auto& label : m_labels) {
            include(label.position);
        }
        if (has_content) {
            const auto width = std::max(maximum.x - minimum.x, 0.1);
            const auto height = std::max(maximum.y - minimum.y, 0.1);
            const auto padding = std::max(width, height) * 0.12 + 0.25;
            view_center = {(minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5};
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

    if (m_grid_enabled && !fit_to_content) {
        drawGrid(painter, viewport, scale, view_center);
    }
    for (const auto& entity : m_entities) {
        drawEntity(painter, entity, fit_to_content, background);
    }

    // Transient overlays belong to the interactive canvas only. The output
    // path uses fit_to_content=true and must contain document entities alone.
    if (!fit_to_content) {
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

    // Committed labels use the same model-to-screen mapping as the current
    // scene, including fit-to-content output. Drawing after restoring the
    // world transform keeps text upright and readable at its device scale.
    drawLabels(painter, viewport, scale, view_center, fit_to_content, background);

    if (!fit_to_content) {
        QString instruction;
        if (m_boundary_draft_preview.has_value() &&
            !m_boundary_draft_preview->instruction.isEmpty()) {
            instruction = m_boundary_draft_preview->instruction;
        } else if (m_tool != CanvasTool::select) {
            instruction = m_tool == CanvasTool::boundary
                ? QStringLiteral("Boundary tool  •  click points, Enter closes, D precise segment")
                : QStringLiteral("Wall tool  •  click two points");
        }
        if (!instruction.isEmpty()) {
            painter.save();
            painter.setPen(QColor(190, 201, 219));
            painter.drawText(viewport.adjusted(12.0, 10.0, -12.0, -10.0),
                             Qt::AlignTop | Qt::AlignLeft, instruction);
            painter.restore();
        }
    }
    if (!fit_to_content && m_boundary_draft_preview.has_value()) {
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
            painter.setBrush(QColor(20, 25, 34, 225));
            painter.drawRoundedRect(bounds, 3.0, 3.0);
            painter.setPen(QColor(255, 239, 172));
            painter.setBrush(Qt::NoBrush);
            painter.drawText(bounds, Qt::AlignCenter, label.text);
        }
        painter.restore();
    }
}

void PlanCanvas::setPointClicked(std::function<void(Vec2)> callback) {
    m_point_clicked = std::move(callback);
}

void PlanCanvas::setEntityClicked(std::function<void(QString)> callback) {
    m_entity_clicked = std::move(callback);
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

void PlanCanvas::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    renderScene(painter, QRectF(rect()));
}

void PlanCanvas::mousePressEvent(QMouseEvent* event) {
    setFocus();
    const auto position = event->position();
    updateCursor(position);
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
        m_panning = true;
        m_pan_start = position;
        m_pan_view_start = m_view_center;
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (m_tool == CanvasTool::select) {
        if (m_entity_clicked) {
            m_entity_clicked(hitTest(position));
        }
    } else if (m_point_clicked) {
        m_point_clicked(snapped(toModel(position, rect())));
    }
    event->accept();
}

void PlanCanvas::mouseMoveEvent(QMouseEvent* event) {
    const auto position = event->position();
    m_last_mouse_position = position;
    if (m_panning) {
        const auto delta = position - m_pan_start;
        m_view_center = {m_pan_view_start.x - delta.x() / m_scale,
                         m_pan_view_start.y + delta.y() / m_scale};
        update();
    }
    updateCursor(position);
    event->accept();
}

void PlanCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton) {
        m_panning = false;
    }
    event->accept();
}

void PlanCanvas::wheelEvent(QWheelEvent* event) {
    const auto steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (steps != 0.0) {
        zoomBy(std::pow(1.18, steps), event->position());
    }
    event->accept();
}

void PlanCanvas::keyPressEvent(QKeyEvent* event) {
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

void PlanCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
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

QString PlanCanvas::hitTest(QPointF point) const {
    constexpr double hit_pixels = 9.0;
    QString result;
    auto best = std::numeric_limits<double>::max();
    for (const auto& entity : m_entities) {
        for (const auto& segment : entity.segments) {
            const auto start = toScreen(segment.start, rect());
            if (segment.sweep_radians == 0.0) {
                const auto end = toScreen(segment.end, rect());
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
                const auto candidate = point_segment_distance(point, previous, current);
                if (candidate < best) {
                    best = candidate;
                    result = entity.id;
                }
                previous = current;
            }
        }
    }
    // Labels are retained presentation entities and use the same spatial
    // selection path as geometry. Keep the hit radius in device pixels so
    // selection remains stable across zoom and DPI changes.
    for (const auto& label : m_labels) {
        const auto screen = toScreen(label.position, rect());
        const auto candidate = std::hypot(point.x() - screen.x(), point.y() - screen.y());
        if (candidate < best) {
            best = candidate;
            result = label.id;
        }
    }
    return best <= hit_pixels ? result : QString{};
}

void PlanCanvas::updateCursor(QPointF point) {
    m_last_mouse_position = point;
    if (m_cursor_moved) {
        m_cursor_moved(snapped(toModel(point, rect())));
    }
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
    QPen minor(QColor(45, 53, 65), 0.0);
    QPen major(QColor(57, 67, 81), 0.0);
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

void PlanCanvas::drawEntity(QPainter& painter, const CanvasEntity& entity, bool output,
                           QColor background) const {
    const auto color = output ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25)
                                                                : QColor(235, 235, 235))
                              : color_for(entity);
    QPen pen(color, 0.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (entity.type == QStringLiteral("wall")) {
        pen.setWidthF(std::max(entity.thickness_metres, 0.04));
    } else {
        // Boundary line weight is a presentation size, never a model-space
        // wall thickness. Printed output uses a quarter-millimetre stroke.
        pen.setCosmetic(true);
        pen.setWidthF(output ? painter.device()->logicalDpiX() * 0.25 / 25.4
                             : entity.selected ? 3.0 : 1.5);
    }
    painter.setPen(pen);
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
        path.arcTo(bounds, arc->start_angle * 180.0 / pi,
                   segment.sweep_radians * 180.0 / pi);
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
        path.arcTo(bounds, arc->start_angle * 180.0 / pi,
                   segment.sweep_radians * 180.0 / pi);
    } else {
        return;
    }
    painter.drawPath(path);
}

void PlanCanvas::drawLabels(QPainter& painter, const QRectF& viewport, double scale,
                            Vec2 view_center, bool output, QColor background) const {
    if (m_labels.empty() || !(scale > 0.0) || !std::isfinite(scale)) {
        return;
    }
    const auto to_screen = [&](Vec2 point) {
        return QPointF(viewport.center().x() + (point.x - view_center.x) * scale,
                       viewport.center().y() - (point.y - view_center.y) * scale);
    };

    painter.save();
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const auto& label : m_labels) {
        if (label.text.isEmpty() || !std::isfinite(label.position.x) ||
            !std::isfinite(label.position.y)) {
            continue;
        }
        QFont font = painter.font();
        const auto text_height = std::isfinite(label.text_height_metres) &&
                                         label.text_height_metres > 0.0
                                     ? label.text_height_metres
                                     : 0.15;
        const auto instance_scale = std::isfinite(label.scale) && label.scale > 0.0
                                        ? label.scale
                                        : 1.0;
        const auto pixel_height = std::clamp(text_height * scale * instance_scale, 8.0, 96.0);
        font.setPixelSize(static_cast<int>(std::lround(pixel_height)));
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        auto bounds = metrics.boundingRect(label.text);
        bounds.moveCenter(QPointF(0.0, 0.0));
        bounds.adjust(-5.0, -3.0, 5.0, 3.0);
        const auto center = to_screen(label.position);
        painter.save();
        painter.translate(center);
        if (std::isfinite(label.rotation_radians)) {
            // Model coordinates are y-up while the Qt viewport is y-down.
            painter.rotate(-label.rotation_radians * 180.0 / 3.14159265358979323846);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(output ? background : QColor(20, 25, 34, 225));
        painter.drawRoundedRect(bounds, 3.0, 3.0);
        painter.setPen(output ? (background.lightnessF() > 0.5 ? QColor(25, 25, 25)
                                                                : QColor(255, 239, 172))
                              : label.selected ? QColor(112, 222, 255) : QColor(255, 239, 172));
        painter.setBrush(Qt::NoBrush);
        painter.drawText(bounds, Qt::AlignCenter, label.text);
        painter.restore();
    }
    painter.restore();
}

}  // namespace sketch::desktop
