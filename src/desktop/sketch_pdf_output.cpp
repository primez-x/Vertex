#include "sketch_pdf_output.hpp"
#include "plan_canvas.hpp"

#include <QBuffer>
#include <QFont>
#include <QFontMetricsF>
#include <QPageLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>

#include <algorithm>
#include <cmath>

namespace sketch::desktop {
namespace {
constexpr double sheet_margin_mm = 10.0;
constexpr double title_block_height_mm = 30.0;
constexpr double title_gap_mm = 4.0;
}  // namespace

SketchPdfLayout sketch_pdf_layout(QPageSize::PageSizeId page_size, bool landscape) {
    auto size = QPageSize(page_size).size(QPageSize::Millimeter);
    if (landscape != (size.width() > size.height())) size.transpose();
    SketchPdfLayout layout;
    layout.page_mm = size;
    layout.title_block_mm = QRectF(sheet_margin_mm, size.height() - sheet_margin_mm - title_block_height_mm,
                                   size.width() - 2 * sheet_margin_mm, title_block_height_mm);
    layout.drawing_mm = QRectF(sheet_margin_mm, sheet_margin_mm, size.width() - 2 * sheet_margin_mm,
                               layout.title_block_mm.top() - title_gap_mm - sheet_margin_mm);
    return layout;
}

std::vector<SketchDrawingScale> sketch_standard_scales(bool metric) {
    if (metric) {
        std::vector<SketchDrawingScale> result;
        for (const double ratio : {20.0, 25.0, 50.0, 75.0, 100.0, 125.0, 200.0, 250.0, 500.0,
                                   1000.0, 2000.0, 2500.0, 5000.0, 10000.0})
            result.push_back({ratio, QStringLiteral("1:%1").arg(ratio, 0, 'f', 0)});
        return result;
    }
    const auto architectural = [](const char* fraction, double ratio) {
        return SketchDrawingScale{ratio, QStringLiteral("%1\" = 1'-0\" (1:%2)")
                                             .arg(QString::fromLatin1(fraction)).arg(ratio, 0, 'f', 0)};
    };
    const auto engineering = [](int feet) {
        return SketchDrawingScale{feet * 12.0, QStringLiteral("1\" = %1' (1:%2)").arg(feet).arg(feet * 12)};
    };
    return {architectural("1/2", 24), architectural("3/8", 32), architectural("1/4", 48),
            architectural("3/16", 64), architectural("1/8", 96), architectural("3/32", 128),
            architectural("1/16", 192), architectural("1/32", 384), engineering(50),
            engineering(100), engineering(200), engineering(500), engineering(1000)};
}

QByteArray make_sketch_pdf(CanvasSketchContentRecording recording, const SketchPdfSheet& sheet,
                           QString* diagnostic) {
    const auto fail = [&](const QString& message) {
        if (diagnostic) *diagnostic = message;
        return QByteArray{};
    };
    if (diagnostic) diagnostic->clear();
    const auto bounds = recording.ink_bounds;
    const auto dpi = recording.picture.logicalDpiX();
    if (dpi <= 0 || dpi != recording.picture.logicalDpiY() ||
        !std::isfinite(bounds.x()) || !std::isfinite(bounds.y()) ||
        !std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) ||
        bounds.width() <= 0 || bounds.height() <= 0 || recording.picture.isNull()) {
        return fail(QStringLiteral("The sketch has no valid recorded content to export."));
    }
    const auto ppmm = dpi / 25.4;
    const auto layout = sketch_pdf_layout(sheet.page_size, sheet.landscape);
    const auto drawing_px = QRectF(layout.drawing_mm.topLeft() * ppmm, layout.drawing_mm.size() * ppmm);
    // The scale is printed on the sheet, so the drawing is never shrunk to fit.
    if (bounds.width() > drawing_px.width() + 0.5 || bounds.height() > drawing_px.height() + 0.5)
        return fail(QStringLiteral("The drawing does not fit the page at %1. Choose a smaller scale or export fewer floors.")
                        .arg(sheet.scale_text.isEmpty() ? QStringLiteral("this scale") : sheet.scale_text));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("The sketch PDF staging buffer could not be opened."));
    {
        QPdfWriter writer(&buffer);
        writer.setTitle(sheet.title.isEmpty() ? QStringLiteral("Vertex sketch")
                                              : QStringLiteral("%1 sketch").arg(sheet.title));
        writer.setCreator(QStringLiteral("Vertex"));
        writer.setResolution(dpi);
        if (!writer.setPageLayout(QPageLayout(QPageSize(sheet.page_size),
                sheet.landscape ? QPageLayout::Landscape : QPageLayout::Portrait,
                QMarginsF(), QPageLayout::Millimeter))) {
            return fail(QStringLiteral("The sketch PDF page could not be configured."));
        }
        QPainter painter(&writer);
        if (!painter.isActive()) return fail(QStringLiteral("The sketch PDF renderer could not start."));
        painter.fillRect(QRectF(0, 0, writer.width(), writer.height()), Qt::white);

        // Drawing: centered in its area at the exact recorded scale. QPicture
        // replays vector paths and text; no raster screenshot is produced.
        painter.save();
        painter.translate(drawing_px.center() - bounds.center());
        if (!recording.picture.play(&painter)) {
            painter.restore();
            painter.end();
            return fail(QStringLiteral("The sketch PDF vector recording could not be replayed."));
        }
        painter.restore();

        // Title block.
        const auto mm = [&](double value) { return value * ppmm; };
        const QRectF block(layout.title_block_mm.topLeft() * ppmm, layout.title_block_mm.size() * ppmm);
        const QColor ink(31, 36, 43), muted(88, 97, 109), rule(150, 158, 168);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen frame(rule, mm(0.3));
        frame.setJoinStyle(Qt::RoundJoin);
        frame.setCapStyle(Qt::RoundCap);
        painter.setPen(frame);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(block, mm(2), mm(2));
        const auto left_width = block.width() * 0.38;
        const auto right_width = block.width() * 0.24;
        const QRectF identity(block.left(), block.top(), left_width, block.height());
        const QRectF areas(identity.right(), block.top(), block.width() - left_width - right_width, block.height());
        const QRectF facts(areas.right(), block.top(), right_width, block.height());
        painter.drawLine(QPointF(identity.right(), block.top() + mm(3)), QPointF(identity.right(), block.bottom() - mm(3)));
        painter.drawLine(QPointF(areas.right(), block.top() + mm(3)), QPointF(areas.right(), block.bottom() - mm(3)));

        const auto sans = [&](double height_mm, bool strong) {
            QFont font(QStringLiteral("IBM Plex Sans"));
            font.setPixelSize(std::max(1, static_cast<int>(std::lround(mm(height_mm)))));
            font.setWeight(strong ? QFont::DemiBold : QFont::Normal);
            font.setFeature("calt", 0);
            return font;
        };
        const auto mono = [&](double height_mm) {
            QFont font(QStringLiteral("IBM Plex Mono"));
            font.setStyleHint(QFont::Monospace);
            font.setPixelSize(std::max(1, static_cast<int>(std::lround(mm(height_mm)))));
            return font;
        };
        const auto inset = mm(3);
        const auto caption = [&](const QRectF& cell, const QString& text) {
            painter.setFont(sans(2.4, false));
            painter.setPen(muted);
            painter.drawText(QRectF(cell.left() + inset, cell.top() + mm(2), cell.width() - 2 * inset, mm(3.4)),
                             Qt::AlignLeft | Qt::AlignVCenter, text);
        };
        const auto elided = [&](const QFont& font, const QString& text, double width) {
            return QFontMetricsF(font, &writer).elidedText(text, Qt::ElideRight, width);
        };

        // Property.
        caption(identity, QStringLiteral("Property"));
        {
            const auto width = identity.width() - 2 * inset;
            auto title_font = sans(4.0, true);
            painter.setFont(title_font);
            painter.setPen(ink);
            const auto title = sheet.title.isEmpty() ? QStringLiteral("Untitled property") : sheet.title;
            painter.drawText(QRectF(identity.left() + inset, identity.top() + mm(6.5), width, mm(6)),
                             Qt::AlignLeft | Qt::AlignVCenter, elided(title_font, title, width));
            if (!sheet.address.isEmpty()) {
                auto address_font = sans(3.0, false);
                painter.setFont(address_font);
                painter.drawText(QRectF(identity.left() + inset, identity.top() + mm(13), width, mm(10)),
                                 Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, sheet.address);
            }
        }

        // Area summary in appraisal terms.
        caption(areas, QStringLiteral("Area summary"));
        {
            const auto row_height = mm(3.9);
            const auto top = areas.top() + mm(6.2);
            const auto available = block.bottom() - mm(2) - top - (sheet.area_note.isEmpty() ? 0.0 : row_height);
            const int capacity = std::max(1, static_cast<int>(std::floor(available / row_height)));
            auto lines = sheet.area_lines;
            if (lines.isEmpty()) lines << QStringLiteral("No measured areas");
            if (lines.size() > capacity) {
                const auto hidden = lines.size() - capacity + 1;
                lines = lines.mid(0, capacity - 1);
                lines << QStringLiteral("%1 more\tSee the appraisal report").arg(hidden);
            }
            const auto label_font = sans(3.0, false);
            const auto value_font = mono(3.0);
            const auto width = areas.width() - 2 * inset;
            for (int index = 0; index < lines.size(); ++index) {
                const auto parts = lines[index].split(QLatin1Char('\t'));
                const QRectF row(areas.left() + inset, top + index * row_height, width, row_height);
                const bool total = index == 0 && parts.front().startsWith(QStringLiteral("GLA"));
                painter.setPen(ink);
                auto label = label_font;
                if (total) label.setWeight(QFont::DemiBold);
                painter.setFont(label);
                painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, elided(label, parts.front(), width * 0.62));
                if (parts.size() > 1) {
                    auto value = value_font;
                    if (total) value.setWeight(QFont::DemiBold);
                    painter.setFont(!parts[1].isEmpty() && parts[1].front().isDigit() ? value : label_font);
                    painter.drawText(row, Qt::AlignRight | Qt::AlignVCenter, parts[1]);
                }
            }
            if (!sheet.area_note.isEmpty()) {
                const auto note_font = sans(2.4, false);
                painter.setFont(note_font);
                painter.setPen(muted);
                painter.drawText(QRectF(areas.left() + inset, block.bottom() - mm(2) - row_height, width, row_height),
                                 Qt::AlignLeft | Qt::AlignVCenter, elided(note_font, sheet.area_note, width));
            }
        }

        // Scale, date and page.
        {
            const auto width = facts.width() - 2 * inset;
            const auto pair = [&](double top_mm, const QString& name, const QString& value) {
                painter.setFont(sans(2.4, false));
                painter.setPen(muted);
                painter.drawText(QRectF(facts.left() + inset, facts.top() + mm(top_mm), width, mm(3.2)),
                                 Qt::AlignLeft | Qt::AlignVCenter, name);
                const auto font = sans(3.0, false);
                painter.setFont(font);
                painter.setPen(ink);
                painter.drawText(QRectF(facts.left() + inset, facts.top() + mm(top_mm + 3.2), width, mm(4)),
                                 Qt::AlignLeft | Qt::AlignVCenter, elided(font, value, width));
            };
            pair(2.0, QStringLiteral("Scale"), sheet.scale_text);
            pair(10.0, QStringLiteral("Date"), sheet.date_text);
            pair(18.0, QStringLiteral("Sheet"), sheet.page_text);
        }
        if (!painter.end()) return fail(QStringLiteral("The sketch PDF renderer could not finish."));
    }
    buffer.close();
    if (!bytes.startsWith("%PDF-") || !bytes.contains("%%EOF"))
        return fail(QStringLiteral("The sketch PDF could not be finalized."));
    return bytes;
}
}  // namespace sketch::desktop
