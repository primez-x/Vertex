#pragma once

#include "sketch/geometry.hpp"

#include <QColor>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QPainter;

namespace sketch::desktop {

enum class CanvasTool {
    select,
    boundary,
    wall,
};

struct CanvasEntity {
    QString id;
    QString type;
    Boundary segments;
    double thickness_metres{0.08};
    bool selected{false};
};

// A retained document annotation. Unlike BoundaryDraftPreview, labels are
// part of the committed drawing and therefore render in both screen and
// fit-to-content output.
struct CanvasLabel {
    QString id;
    Vec2 position{};
    QString text;
    bool selected{false};
};

struct BoundaryDraftLabel {
    Vec2 position{};
    QString text;
};

// A document-independent rendering value for an unfinished boundary. The
// canvas owns only a copy; the authoring session remains the source of truth.
struct BoundaryDraftPreview {
    Boundary segments;
    std::vector<BoundaryDraftLabel> labels;
    std::optional<Vec2> anchor;
    std::optional<Vec2> pen_position;
    std::optional<Segment> rubber_band;
    QString instruction;
};

class PlanCanvas final : public QWidget {
public:
    explicit PlanCanvas(QWidget* parent = nullptr);

    void setEntities(std::vector<CanvasEntity> entities);
    [[nodiscard]] const std::vector<CanvasEntity>& entities() const noexcept { return m_entities; }
    void setTool(CanvasTool tool);
    void setGridEnabled(bool enabled);
    void setSnapEnabled(bool enabled);
    void setMetricUnits(bool metric);
    void setSelectedId(const QString& entity_id);
    void setLabels(std::vector<CanvasLabel> labels);
    [[nodiscard]] const std::vector<CanvasLabel>& labels() const noexcept { return m_labels; }
    void setBoundaryPreview(std::vector<Vec2> points);
    void setWallPreview(std::optional<std::pair<Vec2, Vec2>> wall);
    void setBoundaryDraftPreview(std::optional<BoundaryDraftPreview> preview);
    [[nodiscard]] const std::optional<BoundaryDraftPreview>& boundaryDraftPreview() const noexcept {
        return m_boundary_draft_preview;
    }
    void clearPreview();
    void fitView();
    void zoomBy(double factor, QPointF anchor = {});
    void renderScene(QPainter& painter, const QRectF& viewport) const;
    void renderScene(QPainter& painter, const QRectF& viewport, bool fit_to_content,
                     QColor background) const;
    // Renders a committed scene at an explicit model-to-device scale and
    // center. This is used by persisted sheet viewports so paper scale is
    // independent from the interactive canvas zoom.
    void renderSceneAt(QPainter& painter, const QRectF& viewport, double scale,
                       Vec2 view_center, QColor background) const;
    [[nodiscard]] Vec2 contentCenter() const noexcept;

    void setPointClicked(std::function<void(Vec2)> callback);
    void setEntityClicked(std::function<void(QString)> callback);
    void setCursorMoved(std::function<void(Vec2)> callback);
    void setFinishRequested(std::function<void()> callback);
    void setCancelRequested(std::function<void()> callback);
    void setPreciseInputRequested(std::function<void()> callback);
    void setDraftUndoRequested(std::function<void()> callback);
    void setDraftRedoRequested(std::function<void()> callback);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    [[nodiscard]] QPointF toScreen(Vec2 point, const QRectF& viewport) const;
    [[nodiscard]] Vec2 toModel(QPointF point, const QRectF& viewport) const;
    [[nodiscard]] Vec2 snapped(Vec2 point) const;
    [[nodiscard]] QString hitTest(QPointF point) const;
    void updateCursor(QPointF point);
    void drawGrid(QPainter& painter, const QRectF& viewport, double scale,
                  Vec2 view_center) const;
    void drawEntity(QPainter& painter, const CanvasEntity& entity, bool output,
                    QColor background) const;
    void drawSegment(QPainter& painter, const Segment& segment) const;
    void drawLabels(QPainter& painter, const QRectF& viewport, double scale,
                    Vec2 view_center, bool output, QColor background) const;
    void renderSceneWithTransform(QPainter& painter, const QRectF& viewport,
                                  bool fit_to_content, QColor background,
                                  std::optional<double> explicit_scale,
                                  std::optional<Vec2> explicit_center) const;

    std::vector<CanvasEntity> m_entities;
    std::vector<CanvasLabel> m_labels;
    std::vector<Vec2> m_boundary_preview;
    std::optional<std::pair<Vec2, Vec2>> m_wall_preview;
    std::optional<BoundaryDraftPreview> m_boundary_draft_preview;
    CanvasTool m_tool{CanvasTool::select};
    bool m_grid_enabled{true};
    bool m_snap_enabled{true};
    bool m_metric_units{false};
    QString m_selected_id;
    double m_scale{80.0};
    Vec2 m_view_center{0.0, 0.0};
    std::optional<QPointF> m_last_mouse_position;
    bool m_panning{false};
    QPointF m_pan_start;
    Vec2 m_pan_view_start{};

    std::function<void(Vec2)> m_point_clicked;
    std::function<void(QString)> m_entity_clicked;
    std::function<void(Vec2)> m_cursor_moved;
    std::function<void()> m_finish_requested;
    std::function<void()> m_cancel_requested;
    std::function<void()> m_precise_input_requested;
    std::function<void()> m_draft_undo_requested;
    std::function<void()> m_draft_redo_requested;
};

}  // namespace sketch::desktop
