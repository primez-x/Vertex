#pragma once

#include "sketch/geometry.hpp"
#include "sketch/reference_grid.hpp"

#include <QColor>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
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
    sloped_wall,
};

struct CanvasEntity {
    QString id;
    QString type;
    Boundary segments;
    double thickness_metres{0.08};
    bool selected{false};
    // Optional section/material presentation. These values are deliberately
    // retained on the canvas value rather than inferred from pixels so screen,
    // print, and export rendering share the same persisted view settings.
    bool filled{false};
    QString hatch_pattern{QStringLiteral("none")};
    double hatch_scale{1.0};
    QColor fill_color{};
    // Optional presentation overrides from semantic annotation/style records.
    // An invalid color or non-positive width keeps the canvas default.
    QColor stroke_color{};
    double stroke_width_metres{};
    // Fixed paper-space line weight for fitted or explicitly scaled output.
    // Zero keeps the normal model-space or cosmetic canvas default.
    double output_stroke_width_mm{};
};

// A retained document annotation. Unlike BoundaryDraftPreview, labels are
// part of the committed drawing and therefore render in both screen and
// fit-to-content output.
struct CanvasLabel {
    QString id;
    Vec2 position{};
    QString text;
    bool selected{false};
    double rotation_radians{};
    double scale{1.0};
    double text_height_metres{0.15};
    // Positive finite values specify a fixed paper font size in millimetres;
    // zero retains legacy model-height sizing. Paper size ignores model zoom
    // and instance scale, using the fitted sheet scale when supplied,
    // otherwise output-device DPI or interactive widget DPI.
    double paper_height_mm{};
    QColor color{}; // Invalid retains the canvas/output theme color.
    bool bold{};
    bool italic{};
    // Optional semantic annotation fill. `none` keeps the normal readable
    // canvas label background while other patterns are rendered consistently
    // in interactive and fitted/output scenes.
    QColor fill_color{};
    QString fill_pattern{QStringLiteral("none")};
};

// A raster underlay is a retained presentation value sourced from a
// Document Asset. Source pixels never become measurement truth; the explicit
// metres-per-source-unit calibration controls its model-space footprint.
struct CanvasReference {
    QString id;
    QImage image;
    Vec2 position{};
    double metres_per_source_unit{0.01};
    double scale{1.0};
    double rotation_degrees{};
    bool flip_horizontal{};
    bool flip_vertical{};
    double intensity{1.0};
    bool visible{true};
    bool selected{false};
};

struct CanvasReferenceGrid {
    QString id;
    std::vector<ReferenceGridLine> lines;
    bool visible{true};
    // Axis labels are retained with the rendered grid so screen, print, and
    // export output share the same presentation metadata.
    QString x_label;
    QString y_label;
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
    void setOverviewMapEnabled(bool enabled);
    [[nodiscard]] bool overviewMapEnabled() const noexcept { return m_overview_map_enabled; }
    void setMetricUnits(bool metric);
    // Updates the interactive canvas surface without changing the model or
    // any explicit output background passed to renderScene(...).
    void setCanvasBackground(QColor background);
    void setSelectedId(const QString& entity_id);
    void setLabels(std::vector<CanvasLabel> labels);
    [[nodiscard]] const std::vector<CanvasLabel>& labels() const noexcept { return m_labels; }
    void setReference(std::optional<CanvasReference> reference);
    void setReferences(std::vector<CanvasReference> references);
    [[nodiscard]] const std::vector<CanvasReference>& references() const noexcept { return m_references; }
    [[nodiscard]] std::optional<CanvasReference> reference() const {
        return m_references.empty() ? std::nullopt : std::optional{m_references.front()};
    }
    void setReferenceGrids(std::vector<CanvasReferenceGrid> grids);
    [[nodiscard]] const std::vector<CanvasReferenceGrid>& referenceGrids() const noexcept {
        return m_reference_grids;
    }
    void setBoundaryPreview(std::vector<Vec2> points);
    void setWallPreview(std::optional<std::pair<Vec2, Vec2>> wall);
    void setBoundaryDraftPreview(std::optional<BoundaryDraftPreview> preview);
    [[nodiscard]] const std::optional<BoundaryDraftPreview>& boundaryDraftPreview() const noexcept {
        return m_boundary_draft_preview;
    }
    void clearPreview();
    void fitView();
    void zoomBy(double factor, QPointF anchor = {});
    // Compact in-canvas navigation aid. The map is screen-only and never
    // participates in printable/exported scene output.
    [[nodiscard]] QRectF overviewMapRect() const noexcept;
    [[nodiscard]] Vec2 viewCenter() const noexcept { return m_view_center; }
    void renderScene(QPainter& painter, const QRectF& viewport) const;
    void renderScene(QPainter& painter, const QRectF& viewport, bool fit_to_content,
                     QColor background) const;
    // Renders a committed scene at an explicit model-to-device scale and
    // center. This is used by persisted sheet viewports so paper scale is
    // independent from the interactive canvas zoom. A finite positive paper
    // pixels/mm override sizes paper labels to the fitted sheet; absent or
    // invalid overrides retain output-device DPI sizing.
    void renderSceneAt(QPainter& painter, const QRectF& viewport, double scale,
                       Vec2 view_center, QColor background,
                       std::optional<double> paper_pixels_per_mm = std::nullopt) const;
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
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void pointerPress(QPointF position, Qt::MouseButton button,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void pointerMove(QPointF position);
    void pointerRelease(QPointF position, Qt::MouseButton button);
    [[nodiscard]] std::optional<std::pair<Vec2, Vec2>> contentBounds() const;
    [[nodiscard]] bool navigateOverviewMap(QPointF position);
    void drawOverviewMap(QPainter& painter) const;
    [[nodiscard]] QPointF toScreen(Vec2 point, const QRectF& viewport) const;
    [[nodiscard]] Vec2 toModel(QPointF point, const QRectF& viewport) const;
    [[nodiscard]] Vec2 snapped(Vec2 point) const;
    [[nodiscard]] QString hitTest(QPointF point) const;
    void updateCursor(QPointF point);
    void drawGrid(QPainter& painter, const QRectF& viewport, double scale,
                  Vec2 view_center) const;
    void drawReferenceGrids(QPainter& painter) const;
    void drawReferenceGridLabels(QPainter& painter, const QRectF& viewport,
                                 double scale, Vec2 view_center, bool output,
                                 QColor background,
                                 std::optional<double> paper_pixels_per_mm) const;
    void drawCursorReadout(QPainter& painter, const QRectF& viewport,
                           QColor background) const;
    void drawEntity(QPainter& painter, const CanvasEntity& entity, bool output,
                    QColor background,
                    std::optional<double> paper_pixels_per_mm) const;
    void drawSegment(QPainter& painter, const Segment& segment) const;
    void drawLabels(QPainter& painter, const QRectF& viewport, double scale,
                    Vec2 view_center, bool output, QColor background,
                    std::optional<double> paper_pixels_per_mm) const;
    void drawReference(QPainter& painter, const CanvasReference& reference) const;
    void renderSceneWithTransform(QPainter& painter, const QRectF& viewport,
                                  bool fit_to_content, QColor background,
                                  std::optional<double> explicit_scale,
                                  std::optional<Vec2> explicit_center,
                                  std::optional<double> paper_pixels_per_mm = std::nullopt) const;

    std::vector<CanvasEntity> m_entities;
    std::vector<CanvasLabel> m_labels;
    std::vector<CanvasReference> m_references;
    std::vector<CanvasReferenceGrid> m_reference_grids;
    std::vector<Vec2> m_boundary_preview;
    std::optional<std::pair<Vec2, Vec2>> m_wall_preview;
    std::optional<BoundaryDraftPreview> m_boundary_draft_preview;
    CanvasTool m_tool{CanvasTool::select};
    bool m_grid_enabled{true};
    bool m_snap_enabled{true};
    bool m_overview_map_enabled{true};
    bool m_metric_units{false};
    QColor m_canvas_background{248, 250, 252};
    QString m_selected_id;
    double m_scale{80.0};
    Vec2 m_view_center{0.0, 0.0};
    std::optional<QPointF> m_last_mouse_position;
    bool m_panning{false};
    QPointF m_pan_start;
    Vec2 m_pan_view_start{};
    bool m_touch_active{false};
    int m_touch_id{-1};
    bool m_tablet_active{false};

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
