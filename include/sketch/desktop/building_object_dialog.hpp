#pragma once

#include "sketch/building_entity.hpp"

#include <QDialog>
#include <QString>

#include <memory>
#include <optional>

namespace sketch::desktop {

// Dense, atomic authoring dialog for the bounded architectural object forms.
// The dialog owns no Document and only produces a candidate after submit has
// parsed, validated, and built the complete semantic entity.
class BuildingObjectDialog final : public QDialog {
public:
    explicit BuildingObjectDialog(std::optional<Entity> original = std::nullopt,
                                  bool metricUnits = false,
                                  QWidget* parent = nullptr);
    ~BuildingObjectDialog() override;

    BuildingObjectDialog(const BuildingObjectDialog&) = delete;
    BuildingObjectDialog& operator=(const BuildingObjectDialog&) = delete;

    // Empty until submit() succeeds.  A returned edit candidate is a merged
    // copy of the original entity, so opaque properties and extensions remain
    // available to the caller for an atomic Document command.
    [[nodiscard]] std::optional<Entity> candidate() const;

    // Uses the same widgets and parser as the dialog's Submit button.  On
    // success the dialog accepts itself; on failure it remains open and
    // exposes an inline error through lastError().
    [[nodiscard]] bool submit();

    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sketch::desktop
