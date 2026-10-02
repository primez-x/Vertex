#pragma once

#include <QString>
#include <QWidget>

#include <array>
#include <functional>

class QLabel;
class QLineEdit;
class QToolButton;

namespace sketch::desktop {

enum class DrawingCardinalDirection { right, up, left, down };

// Native length entry. The caller owns the draft and all document changes.
class DrawingInputPanel final : public QWidget {
public:
    explicit DrawingInputPanel(QWidget* parent = nullptr);

    void setMetricUnits(bool metricUnits);
    void setSubmitRequested(std::function<bool(const QString&, DrawingCardinalDirection)> callback);
    void setInputStarted(std::function<void()> callback);
    void setCancelRequested(std::function<void()> callback);
    void beginText(const QString& text);
    void clearInput();
    void setError(const QString& error);
    [[nodiscard]] QString inputText() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void submit(DrawingCardinalDirection direction);
    void selectDirection(DrawingCardinalDirection direction);
    void updatePlacement();

    QLineEdit* m_input{};
    QLabel* m_error{};
    QWidget* m_keypad{};
    std::array<QToolButton*, 4> m_directions{};
    DrawingCardinalDirection m_selectedDirection{DrawingCardinalDirection::right};
    std::function<bool(const QString&, DrawingCardinalDirection)> m_submitRequested;
    std::function<void()> m_inputStarted;
    std::function<void()> m_cancelRequested;
    bool m_metricUnits{};
    bool m_hadInput{};
    bool m_updatingPlacement{};
};

}  // namespace sketch::desktop
