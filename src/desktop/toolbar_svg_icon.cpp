#include "toolbar_svg_icon.hpp"
#include "sketch/desktop/svg_admission.hpp"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QSvgRenderer>
#include <QWidget>

#include <cmath>
#include <limits>
#include <utility>

namespace sketch::desktop {
namespace {

class ToolbarSvgIconEngine final : public QIconEngine {
public:
    ToolbarSvgIconEngine(QByteArray glyphs, QWidget* palette_owner)
        : glyphs_(std::move(glyphs)), palette_owner_(palette_owner) {}

    QIconEngine* clone() const override {
        return new ToolbarSvgIconEngine(glyphs_, palette_owner_.data());
    }

    bool isNull() override { return glyphs_.isEmpty(); }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override {
        return size;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State) override {
        if (!painter || rect.isEmpty() || glyphs_.isEmpty()) return;
        const auto palette = palette_owner_ ? palette_owner_->palette() : QApplication::palette();
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled
                                                  : palette.currentColorGroup();
        const auto role = mode == QIcon::Selected ? QPalette::HighlightedText
                                                  : QPalette::ButtonText;
        const auto color = palette.color(group, role);
        const QByteArray svg = QByteArrayLiteral(
            "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24'>"
            "<g fill='none' stroke='") + color.name(QColor::HexRgb).toUtf8() +
            QByteArrayLiteral("' stroke-opacity='") + QByteArray::number(color.alphaF(), 'g', 8) +
            QByteArrayLiteral("' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>") +
            glyphs_ + QByteArrayLiteral("</g></svg>");
        QSvgRenderer renderer;
        if (!load_admitted_svg(renderer, svg)) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // Keep the square glyph proportional when a caller requests a rectangle.
        const qreal side = qMin(rect.width(), rect.height());
        const QRectF target(rect.x() + (rect.width() - side) / 2,
                            rect.y() + (rect.height() - side) / 2, side, side);
        renderer.render(painter, target);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        if (size.isEmpty() || !std::isfinite(scale) || scale <= 0) return {};
        // Qt 6.8+ supplies a logical size here, including for fractional DPRs.
        // Render anew at the resulting physical size rather than scaling a bitmap.
        const qreal width = std::ceil(size.width() * scale);
        const qreal height = std::ceil(size.height() * scale);
        if (width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max()) return {};
        QPixmap result(static_cast<int>(width), static_cast<int>(height));
        if (result.isNull()) return {};
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.scale(scale, scale);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        painter.end();
        result.setDevicePixelRatio(scale);
        return result;
    }

private:
    QByteArray glyphs_;
    QPointer<QWidget> palette_owner_;
};

} // namespace

QIcon toolbar_svg_icon(const QByteArray& glyphs, QWidget* palette_owner) {
    return QIcon(new ToolbarSvgIconEngine(glyphs, palette_owner));
}

} // namespace sketch::desktop
