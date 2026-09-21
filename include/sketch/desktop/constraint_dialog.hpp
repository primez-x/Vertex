#pragma once

#include "sketch/constraint_authoring.hpp"

#include <QDialog>
#include <QString>

#include <memory>
#include <optional>

namespace sketch::desktop {

// Collects intent against an immutable snapshot. The caller applies only the
// accepted service preview to its current Document, which revalidates identity,
// revision and the displayed candidate before making one reversible change.
class ConstraintDialog final : public QDialog {
public:
    // Supports straight walls and identified straight measurement boundaries.
    [[nodiscard]] static bool supportsEntity(const Entity& entity) noexcept;
    ConstraintDialog(DocumentSnapshot snapshot, QString selected_entity_id,
                     bool metric_units = false, QWidget* parent = nullptr);
    ~ConstraintDialog() override;
    void setLengthExpression(const QString& expression);
    [[nodiscard]] bool previewEdit();
    [[nodiscard]] bool submit();
    [[nodiscard]] std::optional<ConstraintAuthoringPreview> acceptedPreview() const;
    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
