#pragma once

#include <QColorSpace>
#include <QFontMetrics>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>

#include <cstring>
#include <limits>

namespace sketch::desktop {

// Add a legible footer without painting over model pixels. Keep the detected
// file encoding (including alpha for PNG) and replace the image atomically.
inline bool stampDraftImage(const QString& path, const QString& stamp) {
    QByteArray encoding;
    const QImage original = [&] {
        QImageReader reader(path);
        encoding = reader.format();
        return reader.read().convertToFormat(QImage::Format_ARGB32);
    }(); // Close the input before replacing it on Windows.
    if (original.isNull() || encoding.isEmpty() || stamp.trimmed().isEmpty()) return false;

    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(16);
    font.setBold(true);
    constexpr int margin = 12;
    const int text_width = original.width() - margin * 2;
    if (text_width <= 0) return false;
    const QFontMetrics metrics(font);
    const auto text_bounds = metrics.boundingRect(QRect(0, 0, text_width, 0),
                                                  Qt::TextWordWrap, stamp);
    const int footer_height = text_bounds.height() + margin * 2;
    if (footer_height <= 0 || original.height() > std::numeric_limits<int>::max() - footer_height) return false;
    QImage image(original.width(), original.height() + footer_height, QImage::Format_ARGB32);
    if (image.isNull()) return false;
    image.setColorSpace(original.colorSpace());
    image.setDotsPerMeterX(original.dotsPerMeterX());
    image.setDotsPerMeterY(original.dotsPerMeterY());
    image.fill(Qt::white);
    for (int y = 0; y < original.height(); ++y) {
        std::memcpy(image.scanLine(y), original.constScanLine(y), original.bytesPerLine());
    }
    QPainter painter(&image);
    if (!painter.isActive()) return false;
    painter.setFont(font);
    painter.setPen(QColor(150, 50, 50));
    painter.drawText(QRect(margin, original.height() + margin, text_width, text_bounds.height()),
                     Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, stamp);
    painter.end();

    QSaveFile destination(path);
    if (!destination.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&destination, encoding);
    if (!writer.write(image)) return false;
    return destination.commit();
}

} // namespace sketch::desktop
