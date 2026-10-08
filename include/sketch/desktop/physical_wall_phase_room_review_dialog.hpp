#pragma once

#include "sketch/document.hpp"
#include "sketch/physical_wall_phase.hpp"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {
// Collects explicit decisions against one captured source. The owner retains
// workspace/edit authority and publishes the accepted command exactly once.
// Existing proposals require explicit redefinition/retirement, dependent
// Keep/Remove and topology decisions before a detached typed scene preview.
// Saved presentation/annotation removals require exact individual confirmation.
class PhysicalWallPhaseRoomReviewDialog final : public QDialog {
public:
    PhysicalWallPhaseRoomReviewDialog(DocumentSnapshot source,ApplyEntityChanges registry_command,
        PhysicalWallPhaseSelection destination,bool metric_units,
        std::function<DocumentSnapshot()> current_source,QWidget* parent=nullptr);
    ~PhysicalWallPhaseRoomReviewDialog() override;
    [[nodiscard]] const std::optional<ApplyBoundaryConstraintChanges>& acceptedCommand() const;
    [[nodiscard]] QString lastError() const;
    void accept() override;
    void reject() override;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sketch::desktop
