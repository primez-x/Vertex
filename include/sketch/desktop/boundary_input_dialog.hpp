#pragma once

#include "sketch/boundary_authoring_session.hpp"

#include <QDialog>
#include <QString>

#include <memory>
#include <map>
#include <optional>
#include <string>

namespace sketch::desktop {

// Accepted non-coordinate input for one unfinished drawing session. Absolute
// coordinates always come from the current session context rather than repeat.
struct BoundaryInputPreferences {
    int method_index{};
    std::map<std::string,QString,std::less<>> expressions;
    bool clockwise{};
    std::optional<bool> metric_units;
    bool operator==(const BoundaryInputPreferences&) const = default;
};

enum class BoundaryInputPresentation { boundary, wall };

// An open measured stroke uses the same controls and receipt replay kernel,
// without importing the topology or area rules of a boundary session.
struct MeasurementLineworkInputContext {
    std::optional<Vec2> start;
    std::optional<Segment> previous_segment;
    std::optional<Vec2> pointer;
};

// Native precision entry for an anchor, analytical edge or pending dimension. The source
// session belongs to the caller; this dialog only returns a copied session
// after the requested construction has validated successfully.
class BoundaryInputDialog final : public QDialog {
public:
    explicit BoundaryInputDialog(const BoundaryAuthoringSession& source,
                                 bool metricUnits,
                                 QWidget* parent = nullptr,
                                 const BoundaryInputPreferences& preferences = {},
                                 BoundaryInputPresentation presentation = BoundaryInputPresentation::boundary);
    explicit BoundaryInputDialog(const MeasurementLineworkInputContext& source,
                                 bool metricUnits, QWidget* parent = nullptr,
                                 const BoundaryInputPreferences& preferences = {});
    ~BoundaryInputDialog() override;

    BoundaryInputDialog(const BoundaryInputDialog&) = delete;
    BoundaryInputDialog& operator=(const BoundaryInputDialog&) = delete;

    // Empty until the Add segment action has validated and accepted the
    // private candidate.  Returned sessions are independent copies.
    [[nodiscard]] std::optional<BoundaryAuthoringSession> candidate() const;
    // Measured-stroke results are empty until successful submission. An
    // unanchored context returns only a start point; an anchored one a receipt.
    [[nodiscard]] std::optional<ConstructionReceipt> receipt() const;
    [[nodiscard]] std::optional<Vec2> anchor() const;
    // Empty until successful submission. Cancellation/invalid input never
    // publishes changed preferences; coordinate phases retain the effective
    // edge preferences after applying the current input-unit basis.
    [[nodiscard]] std::optional<BoundaryInputPreferences> acceptedPreferences() const;

    // Validates the current method and inputs, then accepts the dialog on
    // success.  Failure leaves the dialog open and exposes an inline error.
    [[nodiscard]] bool submit();

    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sketch::desktop
