#pragma once

#include <QWidget>
#include <QString>

#include <cstddef>
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
    void fitAll();
    // Export the OCCT framebuffer directly. This deliberately does not use
    // QWidget::grab(), which cannot capture the native OCCT child surface.
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

    // Callbacks receive stable semantic entity IDs. onError also receives
    // pending-geometry messages when the viewport cannot claim an authoritative
    // representation of the complete snapshot. Exceptions from onError are
    // contained so observers cannot interrupt preparation or replace diagnostics.
    std::function<void(QString)> onEntitySelected;
    // Ctrl+left-drag requests a translation of a native architectural object.
    // The callback reports the stable entity ID and a world-space delta in
    // metres. The desktop shell owns the authoritative document transaction;
    // this view only previews the derived presentation and emits the request.
    std::function<void(QString, double, double, double)> onEntityTranslationRequested;
    std::function<void(QString)> onError;

    void setEntitySelectedCallback(std::function<void(QString)> callback);
    void setEntityTranslationRequestedCallback(
        std::function<void(QString, double, double, double)> callback);
    void setErrorCallback(std::function<void(QString)> callback);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    QPaintEngine* paintEngine() const override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visualization
}  // namespace sketch
