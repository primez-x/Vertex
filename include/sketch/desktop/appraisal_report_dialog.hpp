#pragma once

#include "sketch/appraisal_document.hpp"
#include "sketch/schedule_model.hpp"
#include <QDialog>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace sketch::desktop {

// Shared visible form projection in Details and paginated HTML/PDF output.
[[nodiscard]] QString appraisal_form_projection_html(const AppraisalDocumentReport& report, bool compact = false);
[[nodiscard]] QString appraisal_ansi_declaration_rows(const AnsiMeasurementDeclarations& value);

// User-facing name for an appraisal source: its declared name, else its type,
// an ordinal among unnamed siblings and its floor ("Area 2 · First floor").
// Never returns a raw entity ID.
[[nodiscard]] QString appraisal_source_label(const DocumentSnapshot& source, const std::string& id);
// Appraisal-term label for a derived category ("GLA", "Non-GLA · garage",
// "Basement · finished"). Used for area rows as "<type> · <name>".
[[nodiscard]] QString appraisal_category_label(AppraisalAreaCategory category);
[[nodiscard]] QString appraisal_area_title(const DocumentSnapshot& source, const AppraisalBoundaryStatus& status);
// Report issues are "<source-id>: <validator message>" or property-wide text.
// This splits the source, names it for people and states the fact, then the fix.
struct AppraisalIssuePresentation {
    enum class Fix { none, facts, setup, reporting, locate };
    std::string source_id;   // empty for property-wide issues
    QString subject;         // user-facing source name; empty for property-wide issues
    QString text;            // plain "fact. fix." sentence(s)
    Fix fix{Fix::none};
};
[[nodiscard]] AppraisalIssuePresentation appraisal_issue_presentation(
    const DocumentSnapshot& source, const std::string& issue);
// Plain-language fact-then-fix text for one validator or qualification message.
[[nodiscard]] QString appraisal_issue_message(const DocumentSnapshot& source, std::string_view message);
// Compact area summary in appraisal terms for title blocks: "GLA<TAB>1,842 sf"
// first, then each other measured category with a nonzero total. When totals
// are withheld the GLA row says so and note explains why in one sentence.
[[nodiscard]] QStringList appraisal_area_summary_lines(const AppraisalDocumentReport& report,
    bool metric, QString* note = nullptr);
// The rounding basis every displayed total follows, for labels and tooltips.
[[nodiscard]] QString appraisal_rounding_basis(const AppraisalDocumentReport& report, bool metric);

class AppraisalReportingDialog final : public QDialog {
public:
    AppraisalReportingDialog(const DocumentSnapshot& source, const AppraisalDocumentReport& report, QWidget* parent = nullptr,
        const std::set<std::string, std::less<>>* visible_entity_ids = nullptr);
    ~AppraisalReportingDialog() override;
    // Root owns the transaction and validates the complete source fingerprint.
    void setApplyRequested(std::function<bool(const AppraisalReportingChanges&, QString&)> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
// Refuses output when the complete capacity/empty notice cannot fit.
bool render_appraisal_summary_schedule(QPainter& painter,const QRectF& bounds,
    double pixels_per_mm,const std::vector<const ScheduleRow*>& rows,bool metric,
    QString* error = nullptr);

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
