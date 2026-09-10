#pragma once

#include "sketch/boundary_authoring_session.hpp"

#include <QDialog>
#include <QString>

#include <memory>
#include <optional>

namespace sketch::desktop {

// Native precision entry for one analytical boundary edge.  The source
// session belongs to the caller; this dialog only returns a copied session
// after the requested construction has validated successfully.
class BoundaryInputDialog final : public QDialog {
public:
    explicit BoundaryInputDialog(const BoundaryAuthoringSession& source,
                                 bool metricUnits,
                                 QWidget* parent = nullptr);
    ~BoundaryInputDialog() override;

    BoundaryInputDialog(const BoundaryInputDialog&) = delete;
    BoundaryInputDialog& operator=(const BoundaryInputDialog&) = delete;

    // Empty until the Add segment action has validated and accepted the
    // private candidate.  Returned sessions are independent copies.
    [[nodiscard]] std::optional<BoundaryAuthoringSession> candidate() const;

    // Validates the current method and inputs, then accepts the dialog on
    // success.  Failure leaves the dialog open and exposes an inline error.
    [[nodiscard]] bool submit();

    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sketch::desktop
