#pragma once

#include "sketch/document.hpp"
#include "sketch/slab_layer_stack_edit.hpp"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {
class HorizontalLayerStackDialog final : public QDialog {
public:
    HorizontalLayerStackDialog(const DocumentSnapshot&, std::string slab_id, bool metric,
        std::function<DocumentSnapshot()> current_source,
        std::function<std::string()> allocate_layer_id, QWidget* parent = nullptr);
    ~HorizontalLayerStackDialog() override;
    [[nodiscard]] std::optional<SlabLayerStackEditIntent> acceptedIntent() const;
    [[nodiscard]] std::optional<Entity> acceptedEntity() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool submit();
    void accept() override;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sketch::desktop
