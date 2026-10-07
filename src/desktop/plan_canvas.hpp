#pragma once

#include "sketch/geometry.hpp"
#include "sketch/annotation_catalog.hpp"
#include "sketch/performance_telemetry.hpp"
#include "sketch/reference_grid.hpp"

#include <QColor>
#include <QByteArray>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QMouseEvent>
#include <QPicture>
#include <QPainterPath>
#include <QPointer>
#include <QRectF>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QWidget>

#include <functional>
#include <limits>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QPainter;
class QPaintDevice;
class QSvgRenderer;
class QTouchEvent;
class QPointingDevice;

namespace sketch::desktop {

enum class CanvasTool {
    select,
    boundary,
    wall,
    sloped_wall,
};

// Controls new picks only; retained scene content and selections stay intact.
enum class CanvasSelectionFilter { all, areas, objects, dimensions, labels, symbols, references };

// Retained vector artwork for one placed catalog symbol. The fallback boundary
// on CanvasEntity remains the authoritative hit-test, transform, minimap, and
// interchange geometry; this payload supplies the detailed screen/print/export
// presentation from the same physical footprint.
struct CanvasSvgSymbol {
    QString catalog_id;
    QByteArray document;
    // SHA-256 of document, computed when the retained scene is built so paint
    // never hashes every SVG on every frame.
    QByteArray artwork_sha256;
    QRectF view_box;
    QRectF footprint_view_box;
    Vec2 position{};
    double rotation_radians{};
    double width_metres{};
    double depth_metres{};
    bool flip_horizontal{};
    bool flip_vertical{};
    // Explicit instance paint intent; document and artwork_sha256 always refer
    // to the untouched source, shared by screen and output derivative rendering.
    std::optional<SymbolSvgPalette> svg_palette;
};

// Stable, screen-only edit point supplied by the document projection. Handles
// never participate in print/export geometry; the owning document remains the
// source of truth and receives exactly one commit after a completed drag.
struct CanvasVertexHandle {
    QString id;
    Vec2 position{};
    std::uint64_t source_revision{};
};

// Physical local axes for semantic objects whose retained strokes do not
// themselves preserve an instance's orientation. SVG symbols supply these
// values directly from their footprint. Positive dimensions are in metres.
struct CanvasSelectionFrame {
    Vec2 center{};
    double rotation_radians{};
    double width_metres{};
    double depth_metres{};
    // Optional authored angle for a projected model object. Its presented
    // angle may be shifted or reflected; snapping and readouts use the source.
    std::optional<double> source_rotation_radians;
    double source_rotation_direction{1.0};
};

// Semantic, screen-only opening controls. The jamb points lie on the host
// baseline; width editing pins the opposite jamb and never scales the artwork.
struct CanvasOpeningWidthControls {
    Vec2 start_jamb{};
    Vec2 end_jamb{};
    double width_metres{};
    double height_metres{};
    std::uint64_t source_revision{};
    // Exact analytical host and distance from its start to the start jamb.
    // Omitted by legacy callers that supply straight jamb controls only.
    std::optional<Segment> host_baseline;
    double offset_metres{};
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
    // Semantic defaults may provide a second high-contrast screen color for
    // dark canvases. User-authored overrides intentionally leave this unset.
    QColor dark_stroke_color{};
    double stroke_width_metres{};
    // Fixed paper-space line weight for fitted or explicitly scaled output.
    // Zero keeps the normal model-space or cosmetic canvas default.
    double output_stroke_width_mm{};
    bool dimension_end_ticks{false};
    std::optional<CanvasSvgSymbol> svg_symbol;
    std::vector<CanvasVertexHandle> vertex_handles;
    // Closed semantic voids retained with the outer boundary. They share the
    // entity transform and stroke, while OddEvenFill keeps their interiors
    // clear in the interactive canvas and in print/export output.
    std::vector<Boundary> holes;
    std::optional<CanvasSelectionFrame> resize_frame;
    std::optional<CanvasOpeningWidthControls> opening_width_controls;
    // Opt-in area styling uses the explicit paper width in screen pixels too.
    // Appended to retain existing aggregate initialization order.
    bool paper_stroke_width_on_screen{false};
    // Analytical source points used by the interactive wall snap resolver.
    // Wall footprints are offsets from their baselines, so their centerline
    // endpoints must be retained separately from the painted outline.
    std::vector<Vec2> snap_points;
    Boundary snap_segments;
    // Complete segments retain physical fill and picking geometry. Joined
    // walls omit only their internal corner seams from this derived stroke
    // path, shared by interactive painting and fitted print/export output.
    std::optional<Boundary> stroke_segments;
    // Interaction-only geometry, such as the analytical span through an open
    // casement. It remains pickable without painting a false closed pane.
    Boundary hit_segments;
    // Derived comparison presentation only; historical geometry stays exact.
    bool dashed_stroke{false};
    // Analytical baselines for directional alignment of visible walls,
    // independent of the active floor's ordinary mouse snap candidates.
    Boundary drawing_alignment_segments;
    // Explicit persisted styles; absence preserves legacy screen/output alpha.
    std::optional<double> fill_opacity;
    QString line_pattern{QStringLiteral("solid")};
    // Derived profile identity within one semantic edit target. Empty retains
    // legacy single-presentation matching; picking and callbacks use id.
    QString presentation_key;
    // Physical model-plan annotations are projected with their source axes.
    // Legacy symbols retain saved view-overlay XY when false.
    bool model_plan{false};
};

// A retained document annotation. Unlike BoundaryDraftPreview, labels are
// part of the committed drawing and therefore render in both screen and
// fit-to-content output.
struct CanvasLinearLabelPlacement {
    Segment anchor;
    Vec2 outward_normal{};
    double clearance_metres{};
};

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
    // Dimension callouts use a compact surface for legibility. Plan labels
    // and floor titles render directly on the drawing like conventional
    // architectural annotations.
    bool show_background{true};
    // Automatically derived room/area labels may shift within their owning
    // boundary to avoid covering placed furniture and fixtures.
    bool avoid_components{false};
    // Room names and floor titles belong to plan views. Explicit annotation
    // labels remain available to elevation/section presentation.
    bool plan_only{false};
    // Label-only semantic entities (for example an area dimension) have no
    // line CanvasEntity from which category picking could infer their type.
    QString selection_type;
    // Derived wall measurements only. Authored annotations keep their anchor.
    std::optional<CanvasLinearLabelPlacement> automatic_linear_placement;
    // Derived area presentation; both positions are model-space metres.
    std::optional<Vec2> leader_start;
    std::optional<Vec2> plan_label_offset;
    // Empty or generic sans-serif retains the bundled workspace font.
    QString font_family;
    // Authored model-space plan anchor; unlike derived plan_only labels,
    // these remain selectable and have normal annotation transform handles.
    bool model_plan{false};
    // Wall text with an explicit world angle keeps that authored orientation;
    // automatically derived angles stay upright after named-plan projection.
    bool wall_dimension_manual_rotation{false};
    // Horizontal text anchor in the label's local axes. Center preserves
    // historical labels; left/right put the corresponding text edge at position.
    QString text_alignment{QStringLiteral("center")};
    // Distinguishes derived presentations sharing one native owner identity.
    // Selection and viewport membership continue to use id.
    QString callout_role;
    std::optional<double> fill_opacity;
};

// Padded local pixel bounds before rotation, using only captured rendering
// values. A worker may supply its own QImage paint device without a widget.
// Invalid scale, DPI, or an absent device yields an empty rectangle.
[[nodiscard]] QRectF canvasLabelLayoutBounds(const CanvasLabel& label, QFont font,
    const QPaintDevice* device, double pixels_per_metre, double dpi_y);

// Exact geometric totals from the candidate document, used only while editing.
struct CanvasBoundaryPreviewMetrics {
    double area_square_metres{};
    double perimeter_metres{};
    // An open physical axis uses its admitted length instead of area totals.
    std::optional<double> length_metres;
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
    double rotation_radians{};
};

// Captured by the host at key-down and re-admitted at key-up. Canvas gestures
// never manufacture document authority or infer which pending edge to omit.
struct CanvasPendingDimensionTarget {
    std::string identity_namespace;
    std::string boundary_id;
    std::string segment_id;
    std::uint64_t revision{};
    std::uint64_t semantic_serial{};
    bool operator==(const CanvasPendingDimensionTarget&) const = default;
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
    // Authoring owns phase/pen state; dimension placement must never close.
    bool can_close_on_anchor{false};
    // Only a mouse-authored next segment uses the relative length magnet.
    // Anchor/dimension placement and precise typed construction stay exact.
    bool length_snap_active{false};
};

// Model-space geometry for a pending physical wall segment. The canvas uses
// the actual thickness to show the footprint and its current length while the
// document remains untouched until the click is committed.
struct WallDraftPreview {
    Vec2 start{};
    Vec2 end{};
    double thickness_metres{0.14};
    QString dimension_text;
};

// Model-authorized alignment choices for the active drawing session. These
// guides are screen-only; the host owns selection, validation and acceptance.
struct DrawingWitness {
    Segment segment;
    bool horizontal{};
    bool selected{};
    QString dimension_text;
    QString command_text;
};

inline constexpr double sketch_content_padding_mm = 2.0;

// Vector commands in a stable, drawing-local coordinate system. ink_bounds
// comes from explicit painted-primitive measurements, not QPicture's own
// bounds or model anchors. Replay on
// a device with the picture's logical DPI to preserve paper fonts and strokes.
struct CanvasSketchContentRecording {
    QPicture picture;
    QRectF ink_bounds;
    Vec2 model_center{};
    double model_scale{};
    double pixels_per_mm{};
};

class PlanCanvas final : public QWidget {
public:
    explicit PlanCanvas(QWidget* parent = nullptr);

    void setPerformanceMeasured(std::function<void(PerformanceMetric,
                                std::chrono::steady_clock::duration)> callback);
    void beginPerformanceMeasurement(PerformanceMetric metric,
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now());
    void cancelPerformanceMeasurement(PerformanceMetric metric);
    void resetPerformanceMeasurements();

    void setEntities(std::vector<CanvasEntity> entities);
    [[nodiscard]] const std::vector<CanvasEntity>& entities() const noexcept { return m_entities; }
    // Screen-only projection of another floor. Source world geometry stays
    // exact; offset is applied by the renderer. Ghosts never enter document
    // bounds, picking, snapping, composition guides, or public output.
    void setFloorGhost(std::vector<CanvasEntity> entities, double opacity,
                       Vec2 offset_metres, std::vector<CanvasLabel> labels = {});
    void clearFloorGhost();
    [[nodiscard]] const std::vector<CanvasEntity>& floorGhostEntities() const noexcept {
        return m_floor_ghost_entities;
    }
    [[nodiscard]] const std::vector<CanvasLabel>& floorGhostLabels() const noexcept {
        return m_floor_ghost_labels;
    }
    [[nodiscard]] double floorGhostOpacity() const noexcept { return m_floor_ghost_opacity; }
    [[nodiscard]] Vec2 floorGhostOffset() const noexcept { return m_floor_ghost_offset; }
    void setTool(CanvasTool tool);
    void setSelectionFilter(CanvasSelectionFilter filter);
    [[nodiscard]] CanvasSelectionFilter selectionFilter() const noexcept { return m_selection_filter; }
    void setGridEnabled(bool enabled);
    void setSnapEnabled(bool enabled);
    void setWallSnapEnabled(bool enabled);
    // Tools that project onto a host consume raw coordinates, independently
    // of the user's retained grid/geometry snapping preference.
    void setRawPointInput(bool enabled);
    void setOverviewMapEnabled(bool enabled);
    [[nodiscard]] bool overviewMapEnabled() const noexcept { return m_overview_map_enabled; }
    void setMetricUnits(bool metric);
    // Updates the interactive canvas surface without changing the model or
    // any explicit output background passed to renderScene(...).
    void setCanvasBackground(QColor background);
    void setSelectedId(const QString& entity_id);
    void setSelectedIds(const QStringList& entity_ids);
    // Screen-only retained selection caption. It is painted inside the canvas
    // and is intentionally excluded from print/export rendering.
    void setSelectionCaption(QString caption);
    void setSelectionTransformEnabled(bool resize_enabled, bool rotate_enabled);
    void setSelectionAxisResizeEnabled(bool enabled);
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
    void setWallPreview(std::optional<WallDraftPreview> wall);
    // Compatibility for existing smoke fixtures and callers that only need
    // the former baseline-only rubber band.
    void setWallPreview(std::pair<Vec2, Vec2> wall) {
        setWallPreview(WallDraftPreview{wall.first, wall.second, 0.14, {}});
    }
    [[nodiscard]] const std::optional<WallDraftPreview>& wallPreview() const noexcept {
        return m_wall_preview;
    }
    void setDrawingWitnesses(std::vector<DrawingWitness> witnesses);
    [[nodiscard]] const std::vector<DrawingWitness>& drawingWitnesses() const noexcept {
        return m_drawing_witnesses;
    }
    void setBoundaryDraftPreview(std::optional<BoundaryDraftPreview> preview);
    [[nodiscard]] const std::optional<BoundaryDraftPreview>& boundaryDraftPreview() const noexcept {
        return m_boundary_draft_preview;
    }
    // A pending library component is never part of committed geometry,
    // selection, snapping, content bounds, minimap, print or export.
    void setComponentPlacementPreview(std::optional<CanvasEntity> preview);
    void clearPreview();
    void fitView();
    void zoomBy(double factor, QPointF anchor = {});
    // Synchronize read-only drawing views in model coordinates. Invalid
    // transforms are ignored; valid scales use the interactive zoom limits.
    void setViewTransform(Vec2 center, double scale);
    void setNavigationChanged(std::function<void(Vec2, double)> callback);
    // Compact in-canvas navigation aid. The map is screen-only and never
    // participates in printable/exported scene output.
    [[nodiscard]] QRectF overviewMapRect() const noexcept;
    [[nodiscard]] Vec2 viewCenter() const noexcept { return m_view_center; }
    [[nodiscard]] double viewScale() const noexcept { return m_scale; }
    // Monotonic input authority: restoring the same camera values does not
    // revive an edit captured before navigation or a display change.
    [[nodiscard]] std::uint64_t navigationGeneration() const noexcept { return m_navigation_generation; }
    // Current screen grid increment, shared by painting and interactive snap.
    [[nodiscard]] double gridSpacingMetres() const noexcept;
    [[nodiscard]] double drawingLengthIncrementMetres() const noexcept;
    // Transient drawing labels use exact inch fractions when representable.
    // Arbitrary exact object snaps retain decimal precision instead of rounding.
    [[nodiscard]] static QString drawingLengthText(double metres, bool metric);
    // Public scene renderers use committed content, excluding selection,
    // cursor feedback and transient edits even at the current viewport scale.
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
    // Separate tight-output contract: committed vectors/labels only, with no
    // canvas surface, references, grids or interaction overlays. The fixed
    // default scale is presentation sizing, independent of interactive zoom.
    [[nodiscard]] std::optional<CanvasSketchContentRecording> recordSketchContent(
        double model_scale = 80.0, QString* diagnostic = nullptr) const;
    void setSketchCompositionGuideEnabled(bool enabled);
    [[nodiscard]] bool sketchCompositionGuideEnabled() const noexcept {
        return m_sketch_composition_guide_enabled;
    }
    [[nodiscard]] std::optional<QRectF> sketchCompositionGuideRect() const;
    [[nodiscard]] Vec2 contentCenter() const noexcept;
    // Interactive selection frame in widget-local logical pixels, including
    // its fixed screen padding. Empty when no drawable selection is retained.
    // Re-query after selection, pan, zoom, or resize to anchor contextual UI.
    [[nodiscard]] std::optional<QRectF> selectionBounds() const;
    // Padded local pixel bounds at the supplied model scale, before rotation.
    // Use this for derived label placement so anchors match canvas painting.
    [[nodiscard]] QRectF labelLayoutBounds(const CanvasLabel& label,
                                          double pixels_per_metre) const;
    // Actual painted control, including rotation and annotation avoidance.
    [[nodiscard]] std::optional<QPointF> selectionRotationHandlePosition() const;
    void setSelectionControlsVisible(bool visible) { m_selection_controls_visible = visible; update(); }

    void setPointClicked(std::function<void(Vec2)> callback);
    // A temporary one-click command consumes input before picks or authoring.
    void setPointPlacementRequested(std::function<void(Vec2)> callback);
    void setEntityClicked(std::function<void(QString)> callback);
    // Unmodified left-button double-click in Select mode. The first click has
    // already applied ordinary selection; this callback requests the object's
    // contextual editor without replaying a second selection/authoring press.
    void setEntityDoubleClicked(std::function<void(QString)> callback);
    void setEntitySelectionClicked(std::function<void(QString, bool)> callback);
    // Alt-click supplies distinct overlapping targets, with the ordinary pick
    // first. Starting captures source authority; completion resolves the pick.
    // The shell owns cycling and admission; a drag still navigates.
    void setOverlapSelectionRequested(std::function<bool(bool, QStringList)> callback);
    // Ctrl-drag rectangle selection adds to the retained selection.
    void setEntitiesSelected(std::function<void(QStringList, bool)> callback);
    // Commits one model-space translation after the interactive preview ends.
    // Returning false rejects the preview without leaving canvas-only geometry.
    void setEntitiesMoveRequested(std::function<bool(QStringList, Vec2)> callback);
    // Capture document authority when a selected frame takes the press.
    void setEntitiesMoveStarted(std::function<void(QStringList)> callback);
    void setEntitiesMoveRejected(std::function<void(QStringList, Vec2)> callback);
    // Optional exact proposal for a group move. A disengaged result uses the
    // ordinary translation preview; an engaged empty result rejects it. The
    // host may mark this serial pending and complete it asynchronously.
    void setEntitiesMovePreviewRequested(std::function<std::optional<std::vector<CanvasEntity>>(
        QStringList, Vec2, std::uint64_t)> callback);
    [[nodiscard]] std::uint64_t entitiesMovePreviewSerial() const noexcept { return m_move_preview_serial; }
    [[nodiscard]] bool entitiesMovePreviewPending() const noexcept { return m_move_preview_pending || m_move_release_pending; }
    [[nodiscard]] std::vector<CanvasEntity> entitiesMovePreview() const { return m_move_entities_preview; }
    bool markEntitiesMovePreviewPending(std::uint64_t serial);
    bool completeEntitiesMovePreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels = {}, std::vector<CanvasReference> references = {});
    // Commits a single-selection transform after the interactive preview.
    // Scale is relative and uniform; rotation is a relative radian delta.
    void setEntityTransformRequested(std::function<bool(QString, double, double)> callback);
    void setEntityTransformStarted(std::function<void(QString)> callback);
    // Capture authority at an opening-width or boundary-vertex press.
    void setEntityEditGestureStarted(std::function<void(QString)> callback);
    // Admit semantic input against the exact displayed source before any hit
    // or handle work. Context cancellation remains a shell-owned decision.
    void setInteractionAdmissionRequested(std::function<bool(bool context)> callback);
    // nullopt retains the ordinary transform preview; an engaged empty
    // proposal rejects it. Pending exact projections never invent geometry.
    void setEntityTransformPreviewRequested(std::function<std::optional<std::vector<CanvasEntity>>(
        QString, double, double, Vec2, std::uint64_t)> callback);
    [[nodiscard]] std::uint64_t entityTransformPreviewSerial() const noexcept { return m_transform_preview_serial; }
    [[nodiscard]] bool entityTransformPreviewPending() const noexcept { return m_transform_preview_pending || m_transform_release_pending; }
    [[nodiscard]] std::vector<CanvasEntity> entityTransformPreview() const { return m_transform_entities_preview; }
    bool markEntityTransformPreviewPending(std::uint64_t serial);
    bool completeEntityTransformPreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels = {}, std::vector<CanvasReference> references = {});
    // Local-axis scales around the model-space midpoint of the opposite edge.
    // Capability gating belongs to the document; rejected previews restore.
    void setEntityAxisResizeRequested(
        std::function<bool(QString, double, double, Vec2)> callback);
    // Analytical document projection supplies opening and host-wall overrides
    // for interactive rendering only. nullopt rejects the candidate unless
    // the callback marks this serial pending for deferred exact projection.
    // The bool pins the start jamb when true, or the end jamb when false.
    void setOpeningWidthPreviewRequested(std::function<std::optional<std::vector<CanvasEntity>>(
        QString, double, bool, std::uint64_t)> callback);
    // UI-thread deferred preview protocol. Capture the serial inside the
    // callback, mark it pending, and complete with exact proposal geometry.
    // Cancellation, scene replacement, and each new proposal invalidate old
    // serials, even when their parameters match. Completion never persists.
    [[nodiscard]] std::uint64_t openingWidthPreviewSerial() const noexcept {
        return m_opening_width_preview_serial;
    }
    [[nodiscard]] bool openingWidthPreviewPending() const noexcept {
        return m_opening_width_preview_pending;
    }
    [[nodiscard]] const std::vector<CanvasEntity>& openingWidthPreviewEntities() const noexcept {
        return m_opening_width_entities_preview;
    }
    bool markOpeningWidthPreviewPending(std::uint64_t serial);
    bool completeOpeningWidthPreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result);
    void setOpeningWidthResizeRequested(
        std::function<bool(QString, double, bool, std::uint64_t)> callback);
    // Exact document projection for a selected stable boundary vertex. The
    // returned entities override screen geometry only, including related owners.
    // nullopt rejects unless the callback marks its current serial pending.
    void setBoundaryVertexPreviewRequested(std::function<std::optional<std::vector<CanvasEntity>>(
        QString, QString, Vec2, std::uint64_t)> callback);
    [[nodiscard]] std::uint64_t boundaryVertexPreviewSerial() const noexcept {
        return m_boundary_vertex_preview_serial;
    }
    [[nodiscard]] bool boundaryVertexPreviewPending() const noexcept {
        return m_boundary_vertex_preview_pending || m_vertex_release_pending;
    }
    bool markBoundaryVertexPreviewPending(std::uint64_t serial);
    bool completeBoundaryVertexPreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result,
        std::vector<CanvasLabel> labels = {},
        std::optional<CanvasBoundaryPreviewMetrics> metrics = std::nullopt);
    // Read-only exact interactive overrides; committed/exported entities stay separate.
    [[nodiscard]] const std::vector<CanvasEntity>& boundaryVertexPreviewEntities() const noexcept {
        return m_boundary_vertex_entities_preview;
    }
    [[nodiscard]] const std::vector<CanvasLabel>& boundaryVertexPreviewLabels() const noexcept {
        return m_boundary_vertex_labels_preview;
    }
    [[nodiscard]] const std::optional<CanvasBoundaryPreviewMetrics>&
    boundaryVertexPreviewMetrics() const noexcept {
        return m_boundary_vertex_metrics_preview;
    }
    // Publishes one selected vertex's absolute canvas-view target. The
    // controller maps it through the captured view before model admission;
    // final admission recomputes the edit rather than trusting preview geometry.
    void setBoundaryVertexMoveRequested(
        std::function<bool(QString, QString, Vec2, std::uint64_t)> callback);
    void setSymbolDropped(std::function<void(QString, double, Vec2)> callback,
                          std::function<bool(const QString&)> uses_raw_point = {});
    void setAreaClassDropped(std::function<bool(QString, Vec2)> callback,
        std::function<void()> malformed_drop_rejected = {});
    // Interaction projection only; authoritative geometry is never changed.
    [[nodiscard]] static bool containsAreaPoint(const Boundary& boundary, Vec2 point);
    [[nodiscard]] QStringList areaIdsAt(Vec2 point) const;
    void setAreaClassCaption(QString caption);
    void setCursorMoved(std::function<void(Vec2)> callback);
    // Emitted only on a stationary right-button release. The target is the
    // painted entity under the pointer, or an empty string for canvas space.
    void setRightClicked(std::function<void(Vec2, QString)> callback);
    void setFinishRequested(std::function<void()> callback);
    void setPendingDimensionTargetRequested(
        std::function<std::optional<CanvasPendingDimensionTarget>()> callback);
    void setPendingDimensionOrientationRequested(
        std::function<void(CanvasPendingDimensionTarget, bool horizontal)> callback);
    void setPendingDimensionOmissionRequested(
        std::function<void(CanvasPendingDimensionTarget)> callback);
    void setCancelRequested(std::function<void()> callback);
    void setPreciseInputRequested(std::function<void()> callback);
    void setBayWindowReturnRequested(std::function<void()> callback);
    void setWitnessAlignmentRequested(std::function<void(bool horizontal)> callback);
    void setDirectionalAlignmentRequested(
        std::function<void(int dx, int dy, bool intersections_only)> callback);
    void setDrawingCornerJumpRequested(std::function<bool()> callback);
    void setDrawingTravelRequested(std::function<bool(int dx, int dy)> callback);
    void setDrawingPenUpRequested(std::function<bool()> callback);
    // Exact world-axis targets from visible structural source geometry. Ctrl
    // uses endpoint coordinates; Ctrl+Shift requires an actual ray contact.
    [[nodiscard]] std::optional<Vec2> directionalDrawingAlignment(
        Vec2 origin, int dx, int dy, bool intersections_only) const;
    void setAutoCloseDrawingRequested(std::function<void()> callback);
    [[nodiscard]] bool drawingCommandIdle() const noexcept;
    // A numeric key can start the host's exact drawing input. The host owns
    // applicability, parsing and document mutation; false preserves shortcuts.
    void setDrawingTextRequested(std::function<bool(const QString&)> callback);
    void setDraftUndoRequested(std::function<void()> callback);
    void setDraftRedoRequested(std::function<void()> callback);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    [[nodiscard]] bool admitInteraction(bool context = false);
    void notifyNavigationChanged(Vec2 previous_center, double previous_scale);
    using PerformanceClock = std::chrono::steady_clock;
    std::function<void(PerformanceMetric, PerformanceClock::duration)> m_performance_measured;
    std::vector<std::pair<PerformanceMetric, PerformanceClock::time_point>> m_pending_measurements;
    std::size_t m_measurement_generation{};

    void pointerPress(QPointF position, Qt::MouseButton button,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void pointerMove(QPointF position, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void pointerRelease(QPointF position, Qt::MouseButton button,
                        Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void resetGesture();
    void resetTabletInput();
    void retireDisconnectedTablet();
    enum class SnapKind { none, grid, length, endpoint, on_wall, on_boundary, alignment, perpendicular };
    struct SnapResult {
        Vec2 point{};
        SnapKind kind{SnapKind::none};
        std::optional<Vec2> anchor;
        std::optional<Segment> guide;
    };
    [[nodiscard]] SnapResult snapResult(QPointF point) const;
    [[nodiscard]] std::optional<Vec2> drawingOrigin() const;
    void handleTouchEvent(QTouchEvent& event);
    void resetTouchInput();
    [[nodiscard]] std::optional<std::pair<Vec2, Vec2>> committedContentBounds() const;
    [[nodiscard]] std::optional<std::pair<Vec2, Vec2>> contentBounds(bool include_drafts = false) const;
    [[nodiscard]] std::optional<QRectF> selectionBounds(const QRectF& viewport) const;
    [[nodiscard]] std::optional<QRectF> selectionFrame(const QRectF& viewport) const;
    [[nodiscard]] std::optional<QRectF> computeSelectionBounds(const QRectF& viewport) const;
    [[nodiscard]] std::optional<QRectF> computeSelectionFrame(const QRectF& viewport) const;
    [[nodiscard]] QByteArray retainedSelectionKey(const QRectF& viewport, bool model_axes = false) const;
    [[nodiscard]] bool hasInteractivePresentation() const;
    void invalidateRetainedPresentation();
    void ensureRetainedSelection() const;
    enum class SelectionHandle { none, resize, rotate, left, right, top, bottom };
    [[nodiscard]] std::optional<CanvasSelectionFrame> entitySelectionAxes(
        const CanvasEntity& entity) const;
    [[nodiscard]] std::optional<CanvasSelectionFrame> selectionAxes() const;
    [[nodiscard]] std::optional<CanvasSelectionFrame> computeSelectionAxes() const;
    [[nodiscard]] CanvasLabel presentedLabel(const CanvasLabel& label, bool output) const;
    [[nodiscard]] const std::vector<CanvasLabel>& positionedLabels(
        const QFont& base_font, const QPaintDevice* device, double scale,
        double dpi, bool output, bool content_only = false, Vec2 layout_origin = {},
        bool floor_ghost = false) const;
    [[nodiscard]] std::vector<QRectF> selectionAnnotationFootprints(
        const QRectF& viewport, const QFont& base_font) const;
    [[nodiscard]] QTransform selectionControlTransform(const QRectF& viewport) const;
    [[nodiscard]] QRectF selectionControlRect(const QRectF& viewport) const;
    [[nodiscard]] QPointF selectionHandlePoint(QPointF anchor, QPointF preferred_direction,
        const QRectF& viewport, const std::vector<QRectF>& annotation_footprints) const;
    [[nodiscard]] QPointF selectionRotationPoint(
        const QRectF& viewport, const std::vector<QRectF>& annotation_footprints) const;
    void drawSelectionDimensions(QPainter& painter, const QRectF& viewport,
        const std::vector<QRectF>& annotation_footprints) const;
    [[nodiscard]] SelectionHandle selectionHandleAt(QPointF point,
                                                     const QRectF& viewport) const;
    struct VertexHandleHit {
        QString entity_id;
        QString vertex_id;
        Vec2 source_position{};
        std::uint64_t source_revision{};
    };
    [[nodiscard]] std::optional<VertexHandleHit> vertexHandleAt(
        QPointF point, const QRectF& viewport) const;
    void updateBoundaryVertexPreview(QPointF point);
    bool applyBoundaryVertexPreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result,
        std::vector<CanvasLabel> labels = {},
        std::optional<CanvasBoundaryPreviewMetrics> metrics = std::nullopt);
    void drawVertexHandles(QPainter& painter, const QRectF& viewport) const;
    struct OpeningWidthHandleHit {
        QString entity_id;
        CanvasOpeningWidthControls source;
        bool keep_start_jamb{};
    };
    [[nodiscard]] const CanvasEntity* selectedOpening() const;
    [[nodiscard]] std::optional<OpeningWidthHandleHit> openingWidthHandleAt(
        QPointF point, const QRectF& viewport) const;
    [[nodiscard]] const CanvasEntity& interactiveEntity(const CanvasEntity& entity) const;
    [[nodiscard]] const CanvasReference& interactiveReference(const CanvasReference& reference) const;
    bool applyEntitiesMovePreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels = {}, std::vector<CanvasReference> references = {});
    void updateEntityTransformPreview();
    bool applyEntityTransformPreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result, std::vector<CanvasLabel> labels = {}, std::vector<CanvasReference> references = {});
    void finishEntityTransformPreview(std::uint64_t serial);
    void finishBoundaryVertexPreview(std::uint64_t serial);
    void updateOpeningWidthPreview(QPointF point);
    bool applyOpeningWidthPreview(std::uint64_t serial,
        std::optional<std::vector<CanvasEntity>> result);
    void drawOpeningWidthHandles(QPainter& painter, const QRectF& viewport) const;
    void drawSelectionFrame(QPainter& painter, const QRectF& viewport,
        const std::vector<QRectF>& annotation_footprints) const;
    void drawSelectionCaption(QPainter& painter, const QRectF& viewport,
                              QColor background) const;
    [[nodiscard]] bool navigateOverviewMap(QPointF position);
    void drawOverviewMap(QPainter& painter) const;
    [[nodiscard]] QPointF toScreen(Vec2 point, const QRectF& viewport) const;
    [[nodiscard]] Vec2 toModel(QPointF point, const QRectF& viewport) const;
    [[nodiscard]] Vec2 snapped(Vec2 point) const;
    [[nodiscard]] QString hitTest(QPointF point, bool filtered = true,
                                  QStringList* overlapping = nullptr) const;
    [[nodiscard]] bool matchesSelectionFilter(const QString& id) const;
    [[nodiscard]] bool matchesSelectionType(const QString& type) const;
    [[nodiscard]] bool selectionInteractionEnabled() const;
    [[nodiscard]] QString contextTarget(QPointF point) const;
    [[nodiscard]] const QStringList& selectedIds() const;
    [[nodiscard]] Vec2 dragDelta(QPointF position) const;
    [[nodiscard]] QStringList rectangleHits(const QRectF& rectangle, bool crossing) const;
    [[nodiscard]] std::optional<Vec2> closingAnchor(QPointF point) const;
    [[nodiscard]] Vec2 inputPoint(QPointF point) const;
    void updatePointerCursor(QPointF point);
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
    void drawDrawingWitnesses(QPainter& painter, const QRectF& viewport, double scale,
                              Vec2 view_center, QColor background) const;
    struct EntityGeometry {
        QPainterPath stroke;
        std::optional<QPainterPath> fill;
        // Conservative model-space control-point bounds, independent of view
        // and selection. Missing/unsafe bounds keep the original draw path.
        std::optional<QRectF> bounds;
    };
    void ensurePublishedEntityGeometry() const;
    void ensurePublishedGeometryIndex() const;
    struct LocalSnapTarget {
        std::size_t entity_index{};
        std::size_t target_index{};
        bool segment{};
    };
    void ensureLocalSnapIndex() const;
    [[nodiscard]] std::optional<std::vector<LocalSnapTarget>> retainedSnapCandidates(
        QPointF point, double radius_pixels,
        std::optional<Vec2> alignment_anchor = std::nullopt) const;
    void ensureEntityHitIndex() const;
    [[nodiscard]] std::optional<std::vector<std::size_t>> entityHitCandidates(
        QPointF point, double hit_pixels) const;
    [[nodiscard]] std::optional<std::vector<std::size_t>> labelHitCandidates(
        QPointF point, double hit_pixels) const;
    [[nodiscard]] std::optional<std::vector<std::size_t>> visiblePublishedEntityIndices(
        const QTransform& model_to_device, const QTransform& canvas_to_device,
        const QRectF& device_viewport) const;
    void drawEntity(QPainter& painter, const CanvasEntity& entity, bool output,
                    QColor background,
                    std::optional<double> paper_pixels_per_mm,
                    const EntityGeometry* geometry = nullptr) const;
    void drawSegment(QPainter& painter, const Segment& segment) const;
    void drawLabels(QPainter& painter, const QRectF& viewport, double scale,
                    Vec2 view_center, bool output, QColor background,
                    std::optional<double> paper_pixels_per_mm,
                    std::vector<QRectF>* annotation_footprints = nullptr,
                    bool content_only = false, bool floor_ghost = false) const;
    void drawReference(QPainter& painter, const CanvasReference& reference) const;
    enum class SceneLayer { committed, screen_with_floor_ghost, floor_ghost };
    void renderSceneWithTransform(QPainter& painter, const QRectF& viewport,
                                  bool fit_to_content, QColor background,
                                  std::optional<double> explicit_scale,
                                  std::optional<Vec2> explicit_center,
                                  std::optional<double> paper_pixels_per_mm = std::nullopt,
                                  bool content_only = false,
                                  SceneLayer layer = SceneLayer::committed) const;

    std::vector<CanvasEntity> m_entities;
    // These indices and IDs belong to owned retained inputs, not document IDs
    // or revisions. Every replacement and selected-flag update invalidates them.
    mutable bool m_retained_selection_ready{};
    mutable QStringList m_retained_selected_ids;
    mutable std::vector<std::size_t> m_selected_entity_indices;
    mutable bool m_has_selected_label{};
    mutable bool m_has_selected_reference{};
    struct SelectionRectCache {
        QByteArray key;
        std::optional<QRectF> bounds;
    };
    mutable SelectionRectCache m_selection_bounds_cache;
    mutable SelectionRectCache m_selection_frame_cache;
    mutable QByteArray m_selection_axes_key;
    mutable std::optional<CanvasSelectionFrame> m_selection_axes_cache;
    // Model-space paths belong only to the current published vector. Selection
    // flags may change in place; every setEntities replacement clears the cache.
    // Preview projections and committed print/export never consume these paths.
    mutable std::vector<EntityGeometry> m_published_entity_geometry;
    struct GeometryIndexEntry {
        QRectF bounds; // Includes the model-space round pen envelope.
        std::size_t entity_index{};
        double cosmetic_pixels{};
        double symbol_metres{};
        double paper_mm{};
    };
    struct GeometryIndexNode {
        QRectF bounds;
        double cosmetic_pixels{};
        double symbol_metres{};
        double paper_mm{};
        std::size_t first{};
        std::size_t count{}; // Nonzero only for leaves.
        std::size_t left{};
        std::size_t right{};
    };
    // Owned input replacement, never IDs/count/revision, defines index identity.
    // Selection changes do not alter bounds; selected entries bypass the query.
    mutable bool m_published_geometry_index_ready{};
    mutable std::vector<GeometryIndexEntry> m_published_geometry_index_entries;
    mutable std::vector<GeometryIndexNode> m_published_geometry_index_nodes;
    mutable std::vector<std::size_t> m_published_geometry_index_fallback;
    struct LocalSnapIndexEntry {
        QRectF bounds; // Actual endpoint or complete baseline support, never paint bounds.
        LocalSnapTarget target;
    };
    struct LocalSnapIndexNode {
        QRectF bounds;
        std::size_t first{};
        std::size_t count{}; // Nonzero only for leaves.
        std::size_t left{};
        std::size_t right{};
    };
    // Model-space target identity survives view/selection changes; every owned
    // setEntities replacement invalidates it, even identical IDs/revisions/count.
    mutable bool m_local_snap_index_ready{};
    mutable std::vector<LocalSnapIndexEntry> m_local_snap_index_entries;
    mutable std::vector<LocalSnapIndexNode> m_local_snap_index_nodes;
    mutable std::vector<LocalSnapTarget> m_local_snap_index_fallback;
    struct HitIndexEntry {
        QRectF bounds; // Exact pick strokes/symbol footprint and interior controls.
        std::size_t entity_index{};
        double cosmetic_pixels{};
        double symbol_metres{};
        double paper_mm{};
    };
    struct HitIndexNode {
        QRectF bounds;
        double cosmetic_pixels{};
        double symbol_metres{};
        double paper_mm{};
        std::size_t first{};
        std::size_t count{}; // Nonzero only for leaves.
        std::size_t left{};
        std::size_t right{};
    };
    // Picking has its own envelope: paint bounds omit interaction-only spans.
    // Only owned setEntities replacement changes this retained input identity.
    mutable bool m_entity_hit_index_ready{};
    mutable std::vector<HitIndexEntry> m_entity_hit_index_entries;
    mutable std::vector<HitIndexNode> m_entity_hit_index_nodes;
    mutable std::vector<std::size_t> m_entity_hit_index_fallback;
    std::vector<CanvasLabel> m_labels;
    std::vector<CanvasEntity> m_floor_ghost_entities;
    std::vector<CanvasLabel> m_floor_ghost_labels;
    double m_floor_ghost_opacity{0.25};
    Vec2 m_floor_ghost_offset{};
    struct ContentBoundsCache {
        QByteArray metrics_key;
        bool ready{}; // Includes a committed source with no finite content.
        std::optional<std::pair<Vec2, Vec2>> bounds;
    };
    mutable ContentBoundsCache m_content_bounds_cache;
    struct OverviewGeometryCache {
        QByteArray key;
        QImage image;
        QPointF origin;
    };
    mutable OverviewGeometryCache m_overview_geometry_cache;
    struct LabelPaintLayout {
        QFont font;
        QRectF bounds;
        QRectF ink_bounds;
    };
    struct LabelHitIndexEntry {
        QRectF bounds; // Conservative model-space padded rotated rectangle.
        std::size_t label_index{};
    };
    struct LabelHitIndexNode {
        QRectF bounds;
        std::size_t first{};
        std::size_t count{};
        std::size_t left{};
        std::size_t right{};
    };
    struct LabelHitIndex {
        bool ready{};
        bool usable{}; // Unsafe published labels keep the original full loop.
        std::vector<LabelHitIndexEntry> entries;
        std::vector<LabelHitIndexNode> nodes;
    };
    struct LabelPlacementCache {
        QByteArray key;
        // A small device/layout key can reuse owned unchanged source labels.
        // Active previews continue using the complete content signature below.
        QByteArray retained_key;
        std::vector<CanvasLabel> labels;
        // Owned immutable derivatives, indexed exactly like labels. Replaced
        // with the placement key and cleared by the same setter invalidation.
        std::vector<LabelPaintLayout> paint_layouts;
        // Owned derivatives of this exact publication; no pan-dependent key.
        LabelHitIndex hit_index;
    };
    // Keep interactive picking warm while a separate output device is used.
    mutable std::array<LabelPlacementCache, 4> m_label_placement_cache;
    bool m_sketch_composition_guide_enabled{};
    std::uint64_t m_sketch_content_revision{};
    mutable std::uint64_t m_sketch_guide_revision{std::numeric_limits<std::uint64_t>::max()};
    mutable QFont m_sketch_guide_font;
    mutable std::optional<CanvasSketchContentRecording> m_sketch_guide_recording;
    std::vector<CanvasReference> m_references;
    std::vector<CanvasReference> m_move_references_preview;
    std::vector<CanvasReference> m_transform_references_preview;
    std::vector<CanvasReferenceGrid> m_reference_grids;
    mutable QHash<QString, QSharedPointer<QSvgRenderer>> m_svg_renderers;
    QString m_selection_caption;
    std::vector<Vec2> m_boundary_preview;
    std::optional<WallDraftPreview> m_wall_preview;
    std::vector<DrawingWitness> m_drawing_witnesses;
    std::optional<BoundaryDraftPreview> m_boundary_draft_preview;
    std::optional<CanvasEntity> m_component_placement_preview;
    CanvasTool m_tool{CanvasTool::select};
    bool m_grid_enabled{true};
    bool m_snap_enabled{true};
    bool m_wall_snap_enabled{false};
    bool m_raw_point_input{false};
    bool m_overview_map_enabled{true};
    bool m_metric_units{false};
    QColor m_canvas_background{248, 250, 252};
    double m_scale{80.0};
    Vec2 m_view_center{0.0, 0.0};
    std::uint64_t m_navigation_generation{};
    std::optional<QPointF> m_last_mouse_position;
    bool m_panning{false};
    enum class LeftGesture {
        none, canvas_pan, object_move, selection_resize, selection_rotate, selection_axis_resize,
        vertex_move, opening_width_resize, marquee, space_pan
    };
    LeftGesture m_left_gesture{LeftGesture::none};
    QPointF m_left_start;
    bool m_left_dragging{false};
    QString m_pressed_entity;
    bool m_pressed_occupied{false};
    bool m_overlap_selection{false};
    double m_overlap_view_scale{};
    QSize m_overlap_view_size;
    qreal m_overlap_view_dpr{};
    std::uint64_t m_overlap_navigation_generation{};
    CanvasSelectionFilter m_selection_filter{CanvasSelectionFilter::all};
    QStringList m_move_ids;
    std::optional<Vec2> m_move_preview_delta;
    std::vector<CanvasEntity> m_move_entities_preview;
    std::vector<CanvasLabel> m_move_labels_preview;
    std::uint64_t m_move_preview_serial{};
    bool m_move_preview_exact{};
    bool m_move_preview_valid{};
    bool m_move_preview_pending{};
    bool m_move_preview_request_in_progress{};
    bool m_move_release_pending{};
    bool m_selection_resize_enabled{};
    bool m_selection_rotate_enabled{};
    bool m_selection_axis_resize_enabled{};
    std::optional<QRectF> m_transform_frame_start;
    QPointF m_transform_center;
    QPointF m_transform_start;
    double m_transform_scale_preview{1.0};
    double m_transform_rotation_preview{};
    SelectionHandle m_axis_handle{SelectionHandle::none};
    Vec2 m_axis_anchor{};
    double m_axis_rotation{};
    double m_axis_extent{};
    double m_axis_scale_x_preview{1.0};
    double m_axis_scale_y_preview{1.0};
    double m_transform_initial_rotation{};
    std::optional<double> m_transform_source_rotation;
    double m_transform_source_rotation_direction{1.0};
    QString m_transform_source_id;
    bool m_selection_controls_visible{true};
    Vec2 m_transform_pivot{};
    std::vector<CanvasEntity> m_transform_entities_preview;
    std::vector<CanvasLabel> m_transform_labels_preview;
    std::uint64_t m_transform_preview_serial{};
    bool m_transform_preview_exact{};
    bool m_transform_preview_valid{};
    bool m_transform_preview_pending{};
    bool m_transform_preview_request_in_progress{};
    bool m_transform_release_pending{};
    std::optional<VertexHandleHit> m_vertex_move_handle;
    std::optional<Vec2> m_vertex_move_press_pointer;
    std::optional<Vec2> m_vertex_move_preview;
    std::vector<CanvasEntity> m_boundary_vertex_entities_preview;
    std::vector<CanvasLabel> m_boundary_vertex_labels_preview;
    std::optional<CanvasBoundaryPreviewMetrics> m_boundary_vertex_metrics_preview;
    bool m_boundary_vertex_preview_valid{};
    bool m_boundary_vertex_preview_pending{};
    bool m_vertex_release_pending{};
    bool m_boundary_vertex_preview_request_in_progress{};
    std::uint64_t m_boundary_vertex_preview_serial{};
    std::optional<QPointF> m_boundary_vertex_preview_pointer;
    std::optional<OpeningWidthHandleHit> m_opening_width_handle;
    std::optional<double> m_opening_width_press_station;
    std::optional<double> m_opening_width_pointer_station;
    std::optional<Vec2> m_opening_width_jamb_preview;
    std::vector<CanvasEntity> m_opening_width_entities_preview;
    double m_opening_width_scale_preview{1.0};
    bool m_opening_width_preview_valid{};
    bool m_opening_width_preview_pending{};
    bool m_opening_width_preview_request_in_progress{};
    std::uint64_t m_opening_width_preview_serial{};
    std::optional<QPointF> m_opening_width_preview_pointer;
    bool m_space_pan_armed{false};
    Qt::MouseButton m_gesture_button{Qt::NoButton};
    QPointF m_right_start;
    bool m_right_dragging{false};
    bool m_overview_dragging{false};
    std::optional<QPointF> m_selection_start;
    QPointF m_selection_end;
    bool m_selection_dragging{false};
    bool m_selection_additive{false};
    QPointF m_pan_start;
    Vec2 m_pan_view_start{};
    bool m_touch_active{false};
    int m_touch_id{-1};
    bool m_touch_navigation{false};
    struct TouchNavigation {
        std::pair<int, int> ids;
        Vec2 anchor;
        double initial_distance{};
        double initial_scale{};
    };
    std::optional<TouchNavigation> m_touch_navigation_start;
    bool m_tablet_active{false};
    Qt::MouseButton m_tablet_button{Qt::NoButton};
    QPointer<const QPointingDevice> m_tablet_device;

    std::function<void(Vec2)> m_point_clicked;
    std::function<void(Vec2, double)> m_navigation_changed;
    std::function<void(Vec2)> m_point_placement_requested;
    std::function<void(QString)> m_entity_clicked;
    std::function<void(QString)> m_entity_double_clicked;
    std::function<void(QString, bool)> m_entity_selection_clicked;
    std::function<bool(bool, QStringList)> m_overlap_selection_requested;
    std::function<void(QStringList, bool)> m_entities_selected;
    std::function<bool(QStringList, Vec2)> m_entities_move_requested;
    std::function<void(QStringList)> m_entities_move_started;
    std::function<void(QStringList, Vec2)> m_entities_move_rejected;
    std::function<std::optional<std::vector<CanvasEntity>>(
        QStringList, Vec2, std::uint64_t)> m_entities_move_preview_requested;
    std::function<bool(QString, double, double)> m_entity_transform_requested;
    std::function<void(QString)> m_entity_transform_started;
    std::function<void(QString)> m_entity_edit_gesture_started;
    std::function<bool(bool)> m_interaction_admission_requested;
    std::function<std::optional<std::vector<CanvasEntity>>(
        QString, double, double, Vec2, std::uint64_t)> m_entity_transform_preview_requested;
    std::function<bool(QString, double, double, Vec2)> m_entity_axis_resize_requested;
    std::function<std::optional<std::vector<CanvasEntity>>(
        QString, double, bool, std::uint64_t)> m_opening_width_preview_requested;
    std::function<bool(QString, double, bool, std::uint64_t)> m_opening_width_resize_requested;
    std::function<std::optional<std::vector<CanvasEntity>>(
        QString, QString, Vec2, std::uint64_t)> m_boundary_vertex_preview_requested;
    std::function<bool(QString, QString, Vec2, std::uint64_t)>
        m_boundary_vertex_move_requested;
    std::function<void(QString, double, Vec2)> m_symbol_dropped;
    std::function<bool(QString, Vec2)> m_area_class_dropped;
    std::function<void()> m_area_class_drop_rejected;
    QString m_area_class_caption;
    std::function<bool(const QString&)> m_symbol_drop_uses_raw_point;
    std::function<void(Vec2)> m_cursor_moved;
    std::function<void(Vec2, QString)> m_right_clicked;
    std::function<void()> m_finish_requested;
    std::function<std::optional<CanvasPendingDimensionTarget>()> m_pending_dimension_target_requested;
    std::function<void(CanvasPendingDimensionTarget, bool)> m_pending_dimension_orientation_requested;
    std::function<void(CanvasPendingDimensionTarget)> m_pending_dimension_omission_requested;
    std::optional<CanvasPendingDimensionTarget> m_pending_dimension_space_tap;
    std::function<void()> m_cancel_requested;
    std::function<void()> m_precise_input_requested;
    std::function<void()> m_bay_window_return_requested;
    std::function<void(bool)> m_witness_alignment_requested;
    std::function<void(int, int, bool)> m_directional_alignment_requested;
    std::function<bool()> m_drawing_corner_jump_requested;
    std::function<bool(int, int)> m_drawing_travel_requested;
    std::function<bool()> m_drawing_pen_up_requested;
    std::function<void()> m_auto_close_drawing_requested;
    std::function<bool(const QString&)> m_drawing_text_requested;
    std::function<void()> m_draft_undo_requested;
    std::function<void()> m_draft_redo_requested;
};

}  // namespace sketch::desktop
