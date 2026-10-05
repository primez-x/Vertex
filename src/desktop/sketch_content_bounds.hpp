#pragma once

#include <QPaintDevice>
#include <QRectF>

#include <memory>
#include <optional>

class QPicture;

namespace sketch::desktop {
// Records vectors into QPicture while measuring painted geometry. QPicture's
// own integer bounds omit text and mishandle cosmetic/subpixel strokes.
class SketchContentBoundsDevice final : public QPaintDevice {
public:
    explicit SketchContentBoundsDevice(QPicture& picture);
    ~SketchContentBoundsDevice() override;
    QPaintEngine* paintEngine() const override;
    int devType() const override;
    [[nodiscard]] std::optional<QRectF> inkBounds() const;
    [[nodiscard]] bool valid() const;

protected:
    int metric(PaintDeviceMetric metric) const override;

private:
    class Engine;
    QPicture& m_picture;
    std::unique_ptr<Engine> m_engine;
};
} // namespace sketch::desktop
