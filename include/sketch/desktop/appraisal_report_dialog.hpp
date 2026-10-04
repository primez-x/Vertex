#pragma once

#include "sketch/appraisal_document.hpp"
#include "sketch/schedule_model.hpp"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>

namespace sketch::desktop {

// Shared per-property policy presentation for the table and printed schedule.
// Returns empty for rows without a square-metre area quantity.
[[nodiscard]] QString appraisal_schedule_area_text(const ScheduleRow& row, bool metric);

// Acquisition precision governs ceiling eligibility; recorded facts remain
// untouched. Facts, Details and PDF use this same presentation.
[[nodiscard]] QString appraisal_rounded_ceiling_height_text(
    double observed_metres, AcquisitionIncrement increment);
[[nodiscard]] QString appraisal_ceiling_height_rows(const AnsiAppraisalFacts& facts);
// Unrounded candidate-area basis shared by Details and the printed audit trace.
[[nodiscard]] QString appraisal_sloped_ceiling_rows(
    const AppraisalBoundaryStatus& boundary, const AppraisalDocumentReport& report);
// Shared geometric derivation for Details and printed audit output. Reconciles
// against the current measured gross boundary, separately from qualification.
[[nodiscard]] QString appraisal_area_arithmetic_rows(
    const Entity& owner, double gross_square_metres,
    const AppraisalDocumentReport& report, bool metric, bool compact = false);

// Readable bounded summary placement; overflow points to the complete report.
void render_appraisal_summary_schedule(QPainter& painter,const QRectF& bounds,
    double pixels_per_mm,const std::vector<const ScheduleRow*>& rows,bool metric);

// All displayed and exported rows belong to one immutable document revision.
[[nodiscard]] QString appraisal_report_html(const DocumentSnapshot& source,
    const AppraisalDocumentReport& report, bool metric, bool include_details = true);
// Writes complete, automatically paginated audit output through an atomic
// destination. Invalid/stale projections are refused before touching it.
[[nodiscard]] bool write_appraisal_report_pdf(const DocumentSnapshot& source,
    const AppraisalDocumentReport& report, bool metric, const QString& path, QString& error);

class AppraisalReportDialog final : public QDialog {
public:
    AppraisalReportDialog(const DocumentSnapshot& source,
        std::vector<AppraisalDocumentReport> reports, bool metric, QWidget* parent = nullptr);
    ~AppraisalReportDialog() override;
    void setReports(const DocumentSnapshot& source, std::vector<AppraisalDocumentReport> reports, bool metric);
    [[nodiscard]] QString selectedPropertyId() const;
    void showError(const QString& message);
    void setRefreshRequested(std::function<void()> callback);
    void setExportRequested(std::function<void(const QString&, Revision)> callback);
    void setLocateRequested(std::function<void(const QString&, Revision)> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
