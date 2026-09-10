#pragma once

#include <QWidget>
#include <QString>

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
    void setSnapshot(const DocumentSnapshot& snapshot,
                     std::optional<VisibleEntityIds> visible_ids = std::nullopt);
    void fitAll();
    // Export the OCCT framebuffer directly. This deliberately does not use
    // QWidget::grab(), which cannot capture the native OCCT child surface.
    [[nodiscard]] bool exportViewImage(const QString& path);

    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] QString lastError() const;

    // Callbacks receive stable semantic entity IDs. onError also receives
    // pending-geometry messages when the viewport cannot claim an authoritative
    // representation of the complete snapshot.
    std::function<void(QString)> onEntitySelected;
    std::function<void(QString)> onError;

    void setEntitySelectedCallback(std::function<void(QString)> callback);
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
