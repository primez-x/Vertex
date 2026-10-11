#pragma once

#include "sketch/document.hpp"
#include "sketch/physical_wall_phase.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
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
    // The wall replacement or demolition's full analytical stage supplies room evidence;
    // the final command and source fence still name this actual capture.
    PhysicalWallPhaseRoomReviewDialog(DocumentSnapshot source,PhaseConstraintAuthoringIntent wall_authoring_intent,
        bool metric_units,std::function<DocumentSnapshot()> current_source,QWidget* parent=nullptr);
    // A composed scalar stage is drawing evidence only. Completion reconstructs
    // the enclosing mixed prefix against the actual captured source.
    PhysicalWallPhaseRoomReviewDialog(DocumentSnapshot source,PhaseConstraintAuthoringIntent wall_authoring_intent,
        PhaseWallReplacementEntities analytical_stage,bool metric_units,
        std::function<DocumentSnapshot()> current_source,
        std::function<PhaseWallReplacementEntities(const nlohmann::json&)> completion,
        QWidget* parent=nullptr,std::set<std::string,std::less<>> deferred_constraint_ids={});
    ~PhysicalWallPhaseRoomReviewDialog() override;
    [[nodiscard]] const std::optional<ApplyBoundaryConstraintChanges>& acceptedCommand() const;
    [[nodiscard]] const std::optional<nlohmann::json>& acceptedWallReplacement() const;
    [[nodiscard]] QString lastError() const;
    void accept() override;
    void reject() override;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sketch::desktop
