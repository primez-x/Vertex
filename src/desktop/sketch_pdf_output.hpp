#pragma once

#include <QByteArray>
#include <QPageSize>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <vector>

namespace sketch::desktop {
struct CanvasSketchContentRecording;

// One printable sketch sheet: a standard page with a title block naming the
// property, its area summary in appraisal terms, the drawing scale and date.
struct SketchPdfSheet {
    QPageSize::PageSizeId page_size{QPageSize::Letter};
    bool landscape{true};
    QString title;              // property name
    QString address;            // optional second line
    QStringList area_lines;     // "GLA\t1,842 sf" rows (label TAB value)
    QString area_note;          // optional muted note under the areas
    QString scale_text;         // "1/8\" = 1'-0\" (1:96)" or "1:100"
    QString date_text;
    QString page_text{QStringLiteral("Page 1 of 1")};
};

// Page geometry in millimetres for the chosen paper and orientation.
struct SketchPdfLayout {
    QSizeF page_mm;
    QRectF drawing_mm;          // where the fitted drawing may print
    QRectF title_block_mm;
};
[[nodiscard]] SketchPdfLayout sketch_pdf_layout(QPageSize::PageSizeId page_size, bool landscape);

// Standard drawing scales, largest first. ratio is model:paper (1:ratio).
struct SketchDrawingScale {
    double ratio{};
    QString label;
};
[[nodiscard]] std::vector<SketchDrawingScale> sketch_standard_scales(bool metric);

// Finalized vector PDF bytes. The recording must already be at the sheet's
// drawing scale; it is centered in the drawing area, never shrunk to fit.
[[nodiscard]] QByteArray make_sketch_pdf(CanvasSketchContentRecording recording,
                                        const SketchPdfSheet& sheet,
                                        QString* diagnostic = nullptr);
}  // namespace sketch::desktop
