#pragma once

#include "sketch/geometry.hpp"

#include <QWidget>
#include <QString>

#include <vector>

namespace sketch::desktop {

struct WallPreviewDrawing {
    QString id;
    Segment before;
    Segment after;
};

// Read-only comparison of semantic wall baselines; pixels never enter the
// constraint request or document command.
class ConstraintPreviewCanvas final : public QWidget {
public:
    explicit ConstraintPreviewCanvas(QWidget* parent = nullptr);
    void setWalls(std::vector<WallPreviewDrawing> walls);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<WallPreviewDrawing> m_walls;
};

} // namespace sketch::desktop
