#pragma once

#include "sketch/document.hpp"
#include "sketch/wall_layer_stack_edit.hpp"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {
class WallLayerStackDialog final : public QDialog {
public:
    WallLayerStackDialog(const DocumentSnapshot&, std::string wall_id, bool metric,
        std::function<DocumentSnapshot()> current_source,
        std::function<std::string()> allocate_layer_id, QWidget* parent = nullptr);
    ~WallLayerStackDialog() override;
    [[nodiscard]] std::optional<WallLayerStackEditIntent> acceptedIntent() const;
    [[nodiscard]] std::optional<Entity> acceptedEntity() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool submit();
    void accept() override;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sketch::desktop
