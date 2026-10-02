#pragma once

#include "sketch/appraisal_document.hpp"
#include "sketch/schedule_model.hpp"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>

namespace sketch::desktop {

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
