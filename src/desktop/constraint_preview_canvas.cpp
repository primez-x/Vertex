#include "sketch/desktop/constraint_preview_canvas.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {

ConstraintPreviewCanvas::ConstraintPreviewCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("constraintPreviewCanvas"));
    setMinimumSize(240, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Wall movement preview"));
    setAccessibleDescription(QStringLiteral("Dashed gray lines show current walls. Blue lines show proposed walls. Unchanged endpoints are marked with a ring."));
}

QSize ConstraintPreviewCanvas::sizeHint() const { return {420, 250}; }

void ConstraintPreviewCanvas::setWalls(std::vector<WallPreviewDrawing> walls) {
    for (const auto& wall : walls) {
        for (const auto point : {wall.before.start, wall.before.end, wall.after.start, wall.after.end})
            if (!std::isfinite(point.x) || !std::isfinite(point.y))
                throw std::invalid_argument("Wall preview coordinates must be finite");
        (void)segment_bounds(wall.before);
        (void)segment_bounds(wall.after);
    }
    m_walls = std::move(walls);
    update();
}

void ConstraintPreviewCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool light = palette().color(QPalette::Base).lightness() > 128;
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setPen(palette().color(QPalette::Text));
    if (m_walls.empty()) {
        painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
                         QStringLiteral("Choose an edit, then Preview to inspect wall movement."));
        return;
    }
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    double max_x = -min_x;
    double max_y = -min_x;
    for (const auto& wall : m_walls)
        for (const auto& segment : {wall.before, wall.after}) {
            const auto bounds = segment_bounds(segment);
            min_x = std::min(min_x, bounds.minimum.x); max_x = std::max(max_x, bounds.maximum.x);
            min_y = std::min(min_y, bounds.minimum.y); max_y = std::max(max_y, bounds.maximum.y);
        }
    const auto span_x = max_x - min_x;
    const auto span_y = max_y - min_y;
    if (!std::isfinite(span_x) || !std::isfinite(span_y)) {
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Coordinates exceed the preview range."));
        return;
    }
    const auto viewport = QRectF(rect()).adjusted(30, 62, -30, -30);
    const auto scale = std::min(viewport.width() / std::max(span_x, 0.01),
                                viewport.height() / std::max(span_y, 0.01));
    const auto project = [&](Vec2 p) {
        return QPointF(viewport.center().x() + ((p.x - min_x) - span_x * 0.5) * scale,
                       viewport.center().y() - ((p.y - min_y) - span_y * 0.5) * scale);
    };
    const auto path = [&](const Segment& segment) {
        QPainterPath result;
        result.moveTo(project(segment.start));
        if (segment.sweep_radians == 0.0) {
            result.lineTo(project(segment.end));
        } else {
            const auto dx = segment.end.x - segment.start.x;
            const auto dy = segment.end.y - segment.start.y;
            const auto chord = std::hypot(dx, dy);
            const auto offset = chord / (2.0 * std::tan(segment.sweep_radians / 2.0));
            const Vec2 center{segment.start.x + dx * 0.5 - dy / chord * offset,
                segment.start.y + dy * 0.5 + dx / chord * offset};
            const auto radius = chord / (2.0 * std::sin(std::abs(segment.sweep_radians / 2.0))) * scale;
            const auto screen_center = project(center);
            const auto start = std::atan2(segment.start.y - center.y, segment.start.x - center.x);
            result.arcTo(QRectF(screen_center.x() - radius, screen_center.y() - radius, 2 * radius, 2 * radius),
                start * 180.0 / std::numbers::pi, segment.sweep_radians * 180.0 / std::numbers::pi);
        }
        return result;
    };
    painter.drawText(QRectF(12, 7, width() - 24, 48), Qt::AlignLeft | Qt::TextWordWrap,
                     QStringLiteral("Current: dashed   Proposed: blue   Fixed endpoint: ring"));
    for (const auto& wall : m_walls) {
        painter.setPen(QPen(QColor(light ? "#8793a2" : "#a1acb9"), 4.0, Qt::DashLine));
        painter.drawPath(path(wall.before));
    }
    for (const auto& wall : m_walls) {
        painter.setPen(QPen(QColor(light ? "#2563eb" : "#69b9ff"), 2.5));
        painter.drawPath(path(wall.after));
        painter.setBrush(QColor(light ? "#2563eb" : "#69b9ff"));
        for (const auto p : {wall.after.start, wall.after.end}) painter.drawEllipse(project(p), 3, 3);
        painter.setBrush(Qt::NoBrush);
        for (const auto& pair : {std::pair{wall.before.start, wall.after.start},
                                 std::pair{wall.before.end, wall.after.end}})
            if (pair.first.x == pair.second.x && pair.first.y == pair.second.y)
                painter.drawEllipse(project(pair.second), 6, 6);
        painter.setPen(QPen(QColor(light ? "#526579" : "#c5ced9"), 1.5));
        for (const auto p : {wall.before.start, wall.before.end})
            painter.drawRect(QRectF(project(p) - QPointF(4, 4), QSizeF(8, 8)));
    }
}

} // namespace sketch::desktop
