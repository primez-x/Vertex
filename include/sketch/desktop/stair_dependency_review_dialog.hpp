#pragma once

#include "sketch/document.hpp"
#include "sketch/phase_stair_replacement.hpp"

#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Collects only actual-source dependency choices. The controller retains the
// typed stair edit and applies its complete command after its own source fence.
class StairDependencyReviewDialog final : public QDialog {
public:
    StairDependencyReviewDialog(DocumentSnapshot source,
        PhaseStairReplacementDependencyPlan plan,
        std::function<DocumentSnapshot()> current_source, QWidget* parent = nullptr);
    ~StairDependencyReviewDialog() override;

    [[nodiscard]] const std::optional<std::vector<PhaseStairReplacementDependencyDisposition>>&
        acceptedDispositions() const;
    [[nodiscard]] QString lastError() const;
    void accept() override;
    void reject() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
