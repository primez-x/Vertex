#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QSize>

#include <cstddef>
#include <cstdint>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

QT_BEGIN_NAMESPACE
class QContextMenuEvent;
class QSinglePointEvent;
class QTouchEvent;
QT_END_NAMESPACE

namespace sketch {

class DocumentSnapshot;

namespace visualization {

struct NativeRoofOpeningTarget {
    QString roof_id;
    QString opening_id;
    bool operator==(const NativeRoofOpeningTarget&) const = default;
};

// Exact world-space proposal for a typed skylight cohort. The pivot is captured
// from the displayed fills at press; translation does not include pivot motion
// caused by rotation or scaling. Hosts retain their authored placements.
struct NativeRoofOpeningTransform {
    std::vector<NativeRoofOpeningTarget> targets;
    std::array<double, 3> pivot_world_m{};
    std::array<double, 3> translation_world_m{};
    double rotation_radians{};
    double uniform_scale{1.0};
    bool operator==(const NativeRoofOpeningTransform&) const = default;
};

// A native Open CASCADE viewport for the architectural solids that have a
// semantic representation in the document. The widget owns only derived AIS
// presentations; the supplied snapshot remains the authoritative model.
class NativeModelView final : public QWidget {
public:
    explicit NativeModelView(QWidget* parent = nullptr);
    ~NativeModelView() override;

    NativeModelView(const NativeModelView&) = delete;
    NativeModelView& operator=(const NativeModelView&) = delete;

    // An absent mask displays all solids. An empty mask hides every solid.
    // Hidden solids remain validated; visibility cannot bypass output errors.
    using VisibleEntityIds = std::set<std::string, std::less<>>;
    // Schedules immutable geometry preparation. The previous complete scene
    // stays visible; isReady()/exportViewImage reject pending or failed output.
    void setSnapshot(const DocumentSnapshot& snapshot,
                     std::optional<VisibleEntityIds> visible_ids = std::nullopt);
    // Exact immutable source of the latest semantic preparation request,
    // including when the native pane is hidden or has not initialized.
    // Preparation readiness does not imply AIS presentation or input authority.
    [[nodiscard]] std::shared_ptr<const DocumentSnapshot> preparationSourceSnapshot() const noexcept;
    // Exact immutable source of the successfully published AIS scene. A newer
    // queued request never substitutes its source for displayed geometry.
    [[nodiscard]] std::shared_ptr<const DocumentSnapshot> publishedSnapshot() const noexcept;
    // Actual displayed-source press or idle keyboard-context capture, retained
    // through synchronous selection/edit/context and Move/manipulator callbacks.
    [[nodiscard]] std::shared_ptr<const DocumentSnapshot> gestureSourceSnapshot() const noexcept;
    // Called after the native press capture and before the first preview. The
    // shell captures selection/workspace/context here; this emits no command.
    std::function<void(QString)> onTransformGestureStarted;
    // Authorize semantic input before native picking/manipulation. Starting
    // captures context; later calls validate that same press. Camera input is
    // independent; stationary pointer and idle keyboard context requests each
    // start their own admission.
    std::function<bool(bool starting)> onSceneInputRequested;
    void fitAll();
    // Synchronize complete logical selection; IDs without a displayed solid
    // still count toward multi-selection. The last supplied ID is primary.
    // Highlights cover valid visible semantic presentations only. Whole-owner
    // manipulators require one logical member; child-only cohorts share a pivot.
    // Mixed ordinary/child selections retain highlights without subset controls.
    // Replaces a typed child selection.
    void setSelectedEntities(const QStringList& entity_ids);
    // Backward-compatible replacement with one ID (empty clears selection).
    void setSelectedEntity(const QString& entity_id);
    // Synchronize a single semantic skylight child after its logical owners.
    // This emits no callback and preserves the separately published ordinary IDs.
    void setSelectedRoofOpening(std::optional<NativeRoofOpeningTarget> target);
    // Synchronize the complete typed child cohort while preserving owner IDs.
    // The last distinct valid target is primary. No callback is emitted.
    void setSelectedRoofOpenings(std::vector<NativeRoofOpeningTarget> targets);
    // Publish owner and child intent together, without an intermediate owner
    // manipulator or highlight. This emits no selection callback.
    void setSemanticSelection(const QStringList& entity_ids,
                              std::optional<NativeRoofOpeningTarget> target);
    void setSemanticSelections(const QStringList& entity_ids,
                               std::vector<NativeRoofOpeningTarget> targets);
    // Explicit primary: null selects the last ordinary ID (or an empty cohort).
    // A child primary must belong to the normalized typed roster. Invalid
    // primaries throw std::invalid_argument before changing the selection.
    void setSemanticSelections(const QStringList& entity_ids,
                               std::vector<NativeRoofOpeningTarget> targets,
                               std::optional<NativeRoofOpeningTarget> primary_child);
    [[nodiscard]] bool transformControlsVisible() const noexcept;
    std::function<void(std::vector<NativeRoofOpeningTarget>)> onRoofOpeningTransformStarted;
    std::function<void(NativeRoofOpeningTransform, std::uint64_t)> onRoofOpeningTransformPreviewRequested;
    std::function<bool(NativeRoofOpeningTransform, std::uint64_t)> onRoofOpeningTransformRequested;
    std::function<void()> onRoofOpeningTransformCanceled;
    [[nodiscard]] bool roofOpeningTransformPreviewCurrent(std::uint64_t serial) const noexcept;
    // Completion admits a candidate for separate asynchronous native geometry
    // manufacture. It does not publish it as the authoritative scene snapshot.
    [[nodiscard]] bool completeRoofOpeningTransformPreview(
        std::uint64_t serial, const DocumentSnapshot& candidate,
        std::vector<NativeRoofOpeningTarget> remapped_targets,
        VisibleEntityIds candidate_visible_ids);
    [[nodiscard]] bool rejectRoofOpeningTransformPreview(std::uint64_t serial, QString message);
    // Unit changes retire a press-owned proposal before changing its magnet.
    void setRoofOpeningTransformMetricUnits(bool metric);
    // Export the OCCT framebuffer directly. This deliberately does not use
    // QWidget::grab(), which cannot capture the native OCCT child surface.
    // Qt encodes and atomically writes the captured pixels, avoiding the native
    // image codec's narrow filename and temporary-path limitations.
    [[nodiscard]] bool exportViewImage(const QString& path);

    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] bool isGeometryPending() const noexcept;
    // Collect completed work on this widget's owner thread without requiring
    // a Qt event loop or initializing a native window. Other threads are ignored.
    void pollGeometryPreparation() noexcept;
    // Real geometry completion for the latest request, including a hidden or
    // not-yet-native view. isReady additionally requires GUI publication.
    [[nodiscard]] bool isGeometryPrepared() const noexcept;
    [[nodiscard]] QString lastError() const;

    struct PublicationMetrics {
        // New AIS handles, retained live handles, and detached old handles.
        // Counts a solid's body and separately selectable skylight fills.
        // Replacing one solid contributes to both created and removed.
        std::size_t created{};
        std::size_t reused{};
        std::size_t removed{};
        double elapsed_ms{};
    };
    // Latest successful owner-thread AIS publication, including redraw. These
    // are process-local diagnostics, not compositor or display frame timings.
    // A new request clears the result until it has been published successfully.
    [[nodiscard]] std::optional<PublicationMetrics> lastPublicationMetrics() const noexcept;

    // Actual OCCT render-window extent in physical pixels. This read-only
    // diagnostic does not resize, redraw, or export the view; a widget size or
    // exported image alone cannot detect a stale native OpenGL viewport.
    [[nodiscard]] std::optional<QSize> nativeRenderSizePixels() const noexcept;

    enum class TransformControl { translation, rotation, scale };
    // Native picker diagnostic in Qt logical pixels; changes only hover state.
    [[nodiscard]] std::optional<TransformControl> transformControlAt(const QPointF& point);
    // Actual derived presentation's 3x4 local transform, row-major. These
    // diagnostics do not change the authoritative document or preview state.
    [[nodiscard]] std::optional<std::array<double, 12>> nativePresentationTransform(
        const QString& entity_id) const;

    // Callbacks receive stable semantic entity IDs. onError receives unavailable,
    // incomplete or failed geometry diagnostics and operation/export failures.
    // Transient preparation progress uses onGeometryStatusChanged instead.
    // Exceptions from status/error observers are contained so they cannot
    // interrupt preparation or replace diagnostics.
    std::function<void(QString)> onEntitySelected;
    // Plain click: replacement (false); Ctrl click: toggle the hit (true).
    // Stationary Alt click cycles distinct visible overlapping semantic hits
    // and replaces selection (false); Ctrl takes priority over Alt. An empty
    // Alt click emits nothing. Alt drag pans without changing selection.
    // A plain background click supplies an empty ID. Selected group members
    // retain the group for subsequent double-click/context editing.
    // Installed semantic handlers receive the captured selection unchanged and
    // publish the complete accepted result through setSemanticSelections.
    std::function<void(QString, bool)> onEntitySelectionClicked;
    // Explicit Alt replacement. The controller publishes the complete accepted
    // selection before any local mutation; fallback replaces ordinary selection.
    void setEntitySelectionCycledCallback(std::function<void(QString)> callback);
    std::function<void(QString)> onEntitySelectionCycled;
    std::function<void(NativeRoofOpeningTarget, bool)> onRoofOpeningSelectionClicked;
    // Explicit Alt replacement, including a hit already in the cohort. Falls
    // back to onRoofOpeningSelectionClicked(target, false) when not installed.
    // Ordinary plain member clicks retain the cohort and still notify the shell.
    std::function<void(NativeRoofOpeningTarget)> onRoofOpeningSelectionCycled;
    std::function<void(NativeRoofOpeningTarget)> onRoofOpeningEditRequested;
    std::function<void(NativeRoofOpeningTarget, QPoint)> onRoofOpeningContextMenuRequested;
    // One atomic marquee notification, including distinct typed children. If
    // installed, this supersedes onEntitiesSelected for that gesture.
    std::function<void(QStringList, std::vector<NativeRoofOpeningTarget>, bool)>
        onSelectionMarqueeRequested;
    // Directional Ctrl marquee supplies displayed semantic hits, in stable ID
    // order, with additive=true. The shell owns its full logical selection.
    std::function<void(QStringList, bool)> onEntitiesSelected;
    // Stationary plain-left double-click release on a visible selectable entity.
    // Receives its stable semantic ID after selection; never requests translation.
    std::function<void(QString)> onEntityEditRequested;
    // Explicit Move mode requests a translation of a native architectural object.
    // The callback reports the stable entity ID and a world-space delta in
    // metres. The desktop shell owns the authoritative document transaction;
    // this view only previews the derived presentation and emits the request.
    std::function<void(QString, double, double, double)> onEntityTranslationRequested;
    // Direct manipulator commit: stable entity ID, world-space translation in
    // metres, signed Z rotation in radians, and a positive uniform scale. The
    // desktop shell applies one authoritative undoable document transaction.
    std::function<void(QString, double, double, double, double, double)>
        onEntityTransformRequested;
    // Stationary right release: hit entity ID (empty for background), followed
    // by global Qt logical pixels. Hit selection is notified before the menu.
    // Idle keyboard context: retained primary ID (empty without selection),
    // with no picking or selection notification. The anchor uses its displayed
    // projected bounds, or the visible viewport center when unavailable.
    std::function<void(QString, QPoint)> onContextMenuRequested;
    std::function<void(QString)> onError;
    // Geometry status for the viewport banner: preparation progress, terminal
    // geometry diagnostics, or empty when preparation/publication succeeds.
    // A progress notification is not an authoring or operation error.
    std::function<void(QString)> onGeometryStatusChanged;

    void setEntitySelectedCallback(std::function<void(QString)> callback);
    void setEntitySelectionClickedCallback(std::function<void(QString, bool)> callback);
    void setEntitiesSelectedCallback(std::function<void(QStringList, bool)> callback);
    void setEntityEditRequestedCallback(std::function<void(QString)> callback);
    void setEntityTranslationRequestedCallback(
        std::function<void(QString, double, double, double)> callback);
    void setEntityTransformRequestedCallback(
        std::function<void(QString, double, double, double, double, double)> callback);
    void setErrorCallback(std::function<void(QString)> callback);
    void setGeometryStatusChangedCallback(std::function<void(QString)> callback);
    // Arm one plain left drag of the supplied visible architectural entity.
    // Ctrl+left selects additively; Alt+left cycles hits or pans on drag;
    // middle pans; right orbits or opens context actions.
    [[nodiscard]] bool beginMove(const QString& entity_id);
    void cancelInteraction();
    [[nodiscard]] bool isMoveActive() const noexcept;

protected:
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    QPaintEngine* paintEngine() const override;

private:
    [[nodiscard]] bool admitSceneInput(bool starting);
    void resetInteraction(bool restore_controls, bool keep_tablet_dispatch = false);
    void resetCompletedPointerInteraction(bool restore_controls);
    [[nodiscard]] bool claimPointer(Qt::MouseButton button);
    [[nodiscard]] bool suppressMouseInput(const QMouseEvent* event) const;
    void retireDisconnectedTablet() noexcept;
    void cancelTabletInteraction() noexcept;
    void retireDisconnectedTouch() noexcept;
    void cancelTouchInteraction() noexcept;
    void touchEvent(QTouchEvent* event);
    void pointerPress(QSinglePointEvent* event);
    void pointerDoubleClick(QSinglePointEvent* event);
    void pointerMove(QSinglePointEvent* event);
    void pointerRelease(QSinglePointEvent* event);
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visualization
}  // namespace sketch
