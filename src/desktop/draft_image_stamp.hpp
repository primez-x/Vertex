#pragma once

#include <QColorSpace>
#include <QGlyphRun>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QSaveFile>
#include <QTextLayout>

#include <cmath>
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
    font.setHintingPreference(QFont::PreferNoHinting);
    constexpr int margin = 12;
    const int text_width = original.width() - margin * 2;
    if (text_width <= 0) return false;
    // Draw the font outlines rather than cached platform glyph bitmaps. The
    // latter can differ with glyph-cache history between export and reopen.
    QString layout_text = stamp;
    layout_text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    layout_text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(layout_text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal text_height = 0;
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(text_width);
        line.setPosition(QPointF(0, text_height));
        text_height += line.height();
    }
    layout.endLayout();
    if (!std::isfinite(text_height) || text_height <= 0 ||
        text_height > std::numeric_limits<int>::max() - margin * 2) return false;
    const int footer_height = static_cast<int>(std::ceil(text_height)) + margin * 2;
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
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(margin, original.height() + margin);
    for (const auto& run : layout.glyphRuns()) {
        const auto raw_font = run.rawFont();
        if (!raw_font.isValid()) return false;
        const auto glyphs = run.glyphIndexes();
        const auto positions = run.positions();
        if (glyphs.size() != positions.size()) return false;
        for (qsizetype index = 0; index < glyphs.size(); ++index) {
            const auto outline = raw_font.pathForGlyph(glyphs[index]);
            painter.fillPath(outline.translated(positions[index]), QColor(150, 50, 50));
        }
    }
    painter.end();

    QSaveFile destination(path);
    if (!destination.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&destination, encoding);
    if (!writer.write(image)) return false;
    return destination.commit();
}

} // namespace sketch::desktop
