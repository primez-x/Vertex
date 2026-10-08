#pragma once

#include "sketch/phase_slab_profile_edit.hpp"

#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Prepares profile input from an immutable actual document source. The current
// source provider must also reject changes to the controller's drawing context.
// The controller retains responsibility for phase commands and final authority.
class HorizontalProfileDialog final : public QDialog {
public:
    HorizontalProfileDialog(const DocumentSnapshot& source, std::string slab_id,
                            bool metric, std::function<DocumentSnapshot()> current_source,
                            QWidget* parent = nullptr);
    ~HorizontalProfileDialog() override;

    [[nodiscard]] std::optional<SlabProfileEditIntent> acceptedIntent() const;
    [[nodiscard]] std::optional<Entity> acceptedEntity() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool submit();
    void accept() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
