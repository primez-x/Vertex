#pragma once

#include "sketch/sheet_view_model.hpp"
#include <QDialog>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Edits a detached snapshot. The caller commits acceptedModel() through its
// normal document/undo transaction only after QDialog::Accepted.
class SheetLayoutDialog final : public QDialog {
public:
    explicit SheetLayoutDialog(const SheetViewModel& model, const QString& selected_sheet_id,
                               QWidget* parent = nullptr);
    ~SheetLayoutDialog() override;
    bool selectSheet(const QString& id);
    bool selectViewport(const QString& id);
    bool selectSchedulePlacement(const QString& id);
    [[nodiscard]] QString selectedSheetId() const;
    [[nodiscard]] const SheetViewModel& workingModel() const;
    [[nodiscard]] const std::optional<SheetViewModel>& acceptedModel() const;
    // Validates and stages the current placement, leaving the source untouched.
    bool applyCurrentEdit();
    void accept() override;
    void reject() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
