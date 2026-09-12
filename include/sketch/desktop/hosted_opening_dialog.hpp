#pragma once
#include "sketch/wall_semantics.hpp"
#include "sketch/quantity.hpp"
#include <QDialog>
#include <array>

class QLineEdit;
class QLabel;
class QPushButton;
namespace sketch::desktop {
class HostedOpeningDialog final : public QDialog {
public:
    HostedOpeningDialog(const Wall& host, Unit unit, bool window, QWidget* parent = nullptr);
    [[nodiscard]] QString offsetExpression() const;
    [[nodiscard]] QString widthExpression() const;
    [[nodiscard]] QString sillExpression() const;
    [[nodiscard]] QString heightExpression() const;
    bool submit();
private:
    bool validate();
    Wall host_;
    Unit unit_;
    std::array<QLineEdit*,4> fields_{};
    QWidget* preview_{};
    QLabel* error_{};
    QPushButton* create_{};
};
} // namespace sketch::desktop
