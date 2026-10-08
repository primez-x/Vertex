#pragma once
#include "sketch/document.hpp"
#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace sketch::desktop {
// Collects complete decisions against one immutable source, optionally after
// a detached curve proposal. The owner applies the accepted single command
// after its own workspace/selection fence against the original captured source.
class PhysicalWallRoomReviewDialog final : public QDialog {
public:
    PhysicalWallRoomReviewDialog(DocumentSnapshot source,std::string selected_wall_id,
        bool metric_units,std::function<DocumentSnapshot()> current_source,QWidget* parent=nullptr,
        std::optional<Command> predecessor=std::nullopt);
    ~PhysicalWallRoomReviewDialog() override;
    [[nodiscard]] const std::optional<ApplyBoundaryConstraintChanges>& acceptedCommand() const;
    [[nodiscard]] QString lastError() const;
    void accept() override;
    void reject() override;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sketch::desktop
