#pragma once

#include "sketch/phase_wall_profile_edit.hpp"

#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Prepares exact profile input from an immutable actual document source. The
// provider must also reject changes to the controller's drawing context. The
// controller owns final command authority and ordinary/proposed publication.
class WallProfileDialog final : public QDialog {
public:
    WallProfileDialog(const DocumentSnapshot& source, std::string wall_id,
                      bool metric, std::function<DocumentSnapshot()> current_source,
                      QWidget* parent = nullptr);
    ~WallProfileDialog() override;

    [[nodiscard]] std::optional<WallProfileEditIntent> acceptedIntent() const;
    [[nodiscard]] std::optional<Entity> acceptedEntity() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool submit();
    void accept() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
