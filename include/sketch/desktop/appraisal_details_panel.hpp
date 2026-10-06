#pragma once

#include "sketch/appraisal_document.hpp"
#include <QWidget>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Always rebuilds from the document. Only semantic design-phase IDs belong in
// the optional mask; workspace presentation visibility must never be passed.
class AppraisalDetailsPanel final : public QWidget {
public:
    explicit AppraisalDetailsPanel(QWidget* parent = nullptr);
    ~AppraisalDetailsPanel() override;
    void setDocument(const DocumentSnapshot& source, const std::string& property_id,
        bool metric, const std::set<std::string, std::less<>>* semantic_phase_ids = nullptr);
    void setSelectedBoundary(const QString& boundary_id);
    [[nodiscard]] QString selectedBoundaryId() const;
    [[nodiscard]] QString propertyId() const;
    [[nodiscard]] const std::optional<AppraisalDocumentReport>& report() const;
    void setLocateRequested(std::function<void(const QString&, Revision)> callback);
    void setSetupRequested(std::function<void(const QString&)> callback);
    void setFactsRequested(std::function<void(const QString&, Revision)> callback);
    void setReportRequested(std::function<void(const QString&)> callback);
    void setReportingRequested(std::function<void(const QString&, Revision)> callback);
    void setSourceReviewRequested(std::function<void(const QString&, Revision)> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
