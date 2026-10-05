#include "sketch_pdf_output.hpp"
#include "plan_canvas.hpp"

#include <QBuffer>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

#include <cmath>

namespace sketch::desktop {
QByteArray make_sketch_pdf(CanvasSketchContentRecording recording, QString* diagnostic) {
    const auto fail = [&](const char* message) {
        if (diagnostic) *diagnostic = QString::fromLatin1(message);
        return QByteArray{};
    };
    if (diagnostic) diagnostic->clear();
    const auto bounds = recording.ink_bounds;
    const auto dpi = recording.picture.logicalDpiX();
    if (dpi <= 0 || dpi != recording.picture.logicalDpiY() ||
        !std::isfinite(bounds.x()) || !std::isfinite(bounds.y()) ||
        !std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) ||
        bounds.width() <= 0 || bounds.height() <= 0 || recording.picture.isNull()) {
        return fail("The sketch has no valid recorded content to export.");
    }
    const auto pixels_per_mm = dpi / 25.4;
    // Two millimetres around actual painted extents protects antialiasing and
    // PDF device rounding. It is a content crop, not a report sheet or scale.
    constexpr double margin_mm = sketch_content_padding_mm;
    const auto page_mm = QSizeF(bounds.width() / pixels_per_mm + 2 * margin_mm,
                               bounds.height() / pixels_per_mm + 2 * margin_mm);
    // QPdfWriter emits ordinary PDF pages without UserUnit scaling. Reject
    // rather than silently shrink drawings beyond the PDF 200-inch page limit.
    if (page_mm.width() > 5080 || page_mm.height() > 5080)
        return fail("The sketch crop exceeds the PDF page limit. Export smaller visible floors or layers.");
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return fail("The sketch PDF staging buffer could not be opened.");
    {
        QPdfWriter writer(&buffer);
        writer.setTitle(QStringLiteral("Vertex sketch"));
        writer.setCreator(QStringLiteral("Vertex"));
        writer.setResolution(dpi);
        if (!writer.setPageLayout(QPageLayout(
                QPageSize(page_mm, QPageSize::Millimeter, QStringLiteral("Sketch crop"),
                          QPageSize::ExactMatch),
                QPageLayout::Portrait, QMarginsF(), QPageLayout::Millimeter))) {
            return fail("The sketch PDF crop could not be configured.");
        }
        QPainter painter(&writer);
        if (!painter.isActive()) return fail("The sketch PDF renderer could not start.");
        // QPicture replays vector paths, text and SVG primitives. No raster
        // screenshot or page-sized image is manufactured by this route.
        painter.fillRect(QRectF(0, 0, writer.width(), writer.height()), Qt::white);
        painter.translate(margin_mm * pixels_per_mm - bounds.left(),
                          margin_mm * pixels_per_mm - bounds.top());
        if (!recording.picture.play(&painter)) {
            painter.end();
            return fail("The sketch PDF vector recording could not be replayed.");
        }
        if (!painter.end()) return fail("The sketch PDF renderer could not finish.");
    }
    buffer.close();
    if (!bytes.startsWith("%PDF-") || !bytes.contains("%%EOF"))
        return fail("The sketch PDF could not be finalized.");
    return bytes;
}
}  // namespace sketch::desktop
