#pragma once

#include <QWidget>
#include <QString>
#include <QSize>

#include <cstddef>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace sketch {

class DocumentSnapshot;

namespace visualization {

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
    // Gesture press capture, retained through the synchronous commit callback.
    // Empty outside an active Move/manipulator gesture or its commit callback.
    [[nodiscard]] std::shared_ptr<const DocumentSnapshot> gestureSourceSnapshot() const noexcept;
    // Called after the native press capture and before the first preview. The
    // shell captures selection/workspace/context here; this emits no command.
    std::function<void(QString)> onTransformGestureStarted;
    // Authorize semantic input before native picking/manipulation. Starting
    // captures context; later calls validate that same press. Camera input is
    // independent, and a stationary context click starts its own admission.
    std::function<bool(bool starting)> onSceneInputRequested;
    void fitAll();
    // Synchronize the shell's single semantic selection into the native view.
    // Transformable visible solids receive an OCCT manipulator; empty, hidden,
    // missing, or unsupported IDs clear it without changing the document.
    void setSelectedEntity(const QString& entity_id);
    [[nodiscard]] bool transformControlsVisible() const noexcept;
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
    std::function<void(QString, QPoint)> onContextMenuRequested;
    std::function<void(QString)> onError;
    // Geometry status for the viewport banner: preparation progress, terminal
    // geometry diagnostics, or empty when preparation/publication succeeds.
    // A progress notification is not an authoring or operation error.
    std::function<void(QString)> onGeometryStatusChanged;

    void setEntitySelectedCallback(std::function<void(QString)> callback);
    void setEntityEditRequestedCallback(std::function<void(QString)> callback);
    void setEntityTranslationRequestedCallback(
        std::function<void(QString, double, double, double)> callback);
    void setEntityTransformRequestedCallback(
        std::function<void(QString, double, double, double, double, double)> callback);
    void setErrorCallback(std::function<void(QString)> callback);
    void setGeometryStatusChangedCallback(std::function<void(QString)> callback);
    // Arm one plain left drag of the supplied visible architectural entity.
    // Ctrl+left and middle always pan; right always orbits or opens context actions.
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
    void wheelEvent(QWheelEvent* event) override;
    QPaintEngine* paintEngine() const override;

private:
    [[nodiscard]] bool admitSceneInput(bool starting);
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visualization
}  // namespace sketch
