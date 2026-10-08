#pragma once

#include "sketch/constraint_authoring.hpp"

#include <QDialog>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Collects intent against an immutable snapshot. The caller applies only the
// accepted service preview or replacement command to its current Document,
// which revalidates identity, revision and the displayed candidate before
// making one reversible change.
// An unchanged measured resize can close successfully without a service preview
// or a history event; callers check acceptedCommand, then acceptedPreview.
class ConstraintDialog final : public QDialog {
public:
    // Supports analytical walls, receipt-backed measured strokes and identified boundaries.
    [[nodiscard]] static bool supportsEntity(const Entity& entity) noexcept;
    ConstraintDialog(DocumentSnapshot snapshot, QString selected_entity_id,
                     bool metric_units = false, QWidget* parent = nullptr);
    ~ConstraintDialog() override;
    void setLengthExpression(const QString& expression);
    void setCurrentSource(std::function<DocumentSnapshot()> current_source);
    [[nodiscard]] bool previewEdit();
    [[nodiscard]] bool submit();
    [[nodiscard]] std::optional<ConstraintAuthoringPreview> acceptedPreview() const;
    // Physical phase replacements retain the original snapshot and complete
    // any mandatory room review before exposing their atomic command.
    [[nodiscard]] std::optional<ApplyBoundaryConstraintChanges> acceptedCommand() const;
    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
