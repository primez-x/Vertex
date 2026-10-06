#include "sketch/desktop/drawing_input_panel.hpp"

#include <QButtonGroup>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace sketch::desktop {

DrawingInputPanel::DrawingInputPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("drawingInputPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "QWidget#drawingInputPanel { background: palette(base); color: palette(text); "
        "border: 1px solid palette(mid); border-radius: 4px; }"
        "QToolButton { padding: 1px; border-radius: 4px; }"
        "QLabel#drawingInputError { color: palette(text); }"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto* row = new QHBoxLayout;
    row->setSpacing(3);
    layout->addLayout(row);

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("drawingLengthInput"));
    m_input->setPlaceholderText(QStringLiteral("Length (ft)"));
    m_input->setAccessibleName(QStringLiteral("Drawing length"));
    m_input->setToolTip(QStringLiteral(
        "Type a length and press an arrow to draw. J jumps or arms travel. Empty Enter lifts the pen; empty arrows walk connected corners. Ctrl+arrow edits text."));
    m_input->setMaxLength(256);
    m_input->setMinimumWidth(60);
    m_input->installEventFilter(this);
    row->addWidget(m_input, 1);

    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    const std::array<Qt::ArrowType, 4> arrows{
        Qt::RightArrow, Qt::UpArrow, Qt::LeftArrow, Qt::DownArrow};
    const std::array<QString, 4> names{
        QStringLiteral("Right"), QStringLiteral("Up"),
        QStringLiteral("Left"), QStringLiteral("Down")};
    for (std::size_t index = 0; index < m_directions.size(); ++index) {
        auto* button = new QToolButton(this);
        m_directions[index] = button;
        button->setObjectName(QStringLiteral("drawingDirection") + names[index]);
        button->setArrowType(arrows[index]);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(names[index]);
        button->setAccessibleName(names[index]);
        button->setFixedSize(27, 28);
        group->addButton(button);
        row->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, index] {
            submit(static_cast<DrawingCardinalDirection>(index));
        });
    }
    selectDirection(DrawingCardinalDirection::right);

    auto* toggle = new QToolButton(this);
    toggle->setObjectName(QStringLiteral("drawingKeypadToggle"));
    toggle->setText(QStringLiteral("123"));
    toggle->setToolTip(QStringLiteral("Show keypad"));
    toggle->setAccessibleName(QStringLiteral("Show keypad"));
    toggle->setCheckable(true);
    toggle->setFocusPolicy(Qt::NoFocus);
    row->addWidget(toggle);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("drawingInputError"));
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);

    m_keypad = new QWidget(this);
    m_keypad->setObjectName(QStringLiteral("drawingKeypad"));
    auto* keys = new QGridLayout(m_keypad);
    keys->setContentsMargins(0, 0, 0, 0);
    keys->setSpacing(3);
    const std::array<QString, 15> labels{
        QStringLiteral("7"), QStringLiteral("8"), QStringLiteral("9"),
        QStringLiteral("4"), QStringLiteral("5"), QStringLiteral("6"),
        QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
        QStringLiteral("0"), QStringLiteral("."), QStringLiteral("/"),
        QStringLiteral("Space"), QStringLiteral("Backspace"), QStringLiteral("Enter")};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const QString label = labels[index];
        auto* key = new QPushButton(label, m_keypad);
        key->setFocusPolicy(Qt::NoFocus);
        keys->addWidget(key, static_cast<int>(index / 3), static_cast<int>(index % 3));
        connect(key, &QPushButton::clicked, this, [this, label] {
            m_input->setFocus(Qt::OtherFocusReason);
            if (label == QStringLiteral("Enter")) {
                if (m_input->text().isEmpty()) {
                    if (m_emptyCommandRequested) m_emptyCommandRequested(Qt::Key_Return);
                } else {
                    submit(m_selectedDirection);
                }
            } else if (label == QStringLiteral("Backspace")) {
                m_input->backspace();
            } else {
                m_input->insert(label == QStringLiteral("Space") ? QStringLiteral(" ") : label);
            }
        });
    }
    layout->addWidget(m_keypad);
    m_keypad->hide();

    connect(toggle, &QToolButton::toggled, this, [this, toggle](bool checked) {
        m_keypad->setVisible(checked);
        for (auto* direction : m_directions)
            direction->setFixedSize(checked ? QSize(44, 44) : QSize(27, 28));
        toggle->setToolTip(checked ? QStringLiteral("Hide keypad") : QStringLiteral("Show keypad"));
        toggle->setAccessibleName(toggle->toolTip());
        m_input->setFocus(Qt::OtherFocusReason);
        updatePlacement();
    });
    connect(m_input, &QLineEdit::textChanged, this, [this](const QString& text) {
        const bool started = !m_hadInput && !text.isEmpty();
        m_hadInput = !text.isEmpty();
        setError({});
        if (started && m_inputStarted) {
            const auto callback = m_inputStarted;
            callback();
        }
    });
    installEventFilter(this);
    if (parent) {
        parent->installEventFilter(this);
    }
    updatePlacement();
}

void DrawingInputPanel::setMetricUnits(bool metricUnits) {
    if (m_metricUnits == metricUnits) {
        return;
    }
    m_metricUnits = metricUnits;
    m_input->setPlaceholderText(metricUnits ? QStringLiteral("Length (m)") : QStringLiteral("Length (ft)"));
    clearInput();
}

void DrawingInputPanel::setSubmitRequested(
    std::function<bool(const QString&, DrawingCardinalDirection)> callback) {
    m_submitRequested = std::move(callback);
}

void DrawingInputPanel::setInputStarted(std::function<void()> callback) {
    m_inputStarted = std::move(callback);
}

void DrawingInputPanel::setCancelRequested(std::function<void()> callback) {
    m_cancelRequested = std::move(callback);
}

void DrawingInputPanel::beginText(const QString& text) {
    m_input->setFocus(Qt::OtherFocusReason);
    m_input->deselect();
    m_input->setCursorPosition(m_input->text().size());
    m_input->insert(text);
}

void DrawingInputPanel::clearInput() {
    m_input->clear();
    setError({});
}

void DrawingInputPanel::setError(const QString& error) {
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    updatePlacement();
}

QString DrawingInputPanel::inputText() const { return m_input->text(); }

void DrawingInputPanel::selectDirection(DrawingCardinalDirection direction) {
    m_selectedDirection = direction;
    m_directions[static_cast<std::size_t>(direction)]->setChecked(true);
}

void DrawingInputPanel::submit(DrawingCardinalDirection direction) {
    selectDirection(direction);
    if (m_input->text().isEmpty()) {
        const std::array<int,4> keys{Qt::Key_Right,Qt::Key_Up,Qt::Key_Left,Qt::Key_Down};
        if (m_emptyCommandRequested) m_emptyCommandRequested(keys[static_cast<std::size_t>(direction)]);
        return;
    }
    if (!m_submitRequested) {
        return;
    }
    const auto callback = m_submitRequested;
    const QString text = m_input->text();
    const QPointer<DrawingInputPanel> guard(this);
    const bool accepted = callback(text, direction);
    if (guard && accepted) {
        clearInput();
    }
}

void DrawingInputPanel::setEmptyCommandRequested(std::function<bool(int)> callback) {
    m_emptyCommandRequested = std::move(callback);
}

bool DrawingInputPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_input && event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool arrow = key->key() == Qt::Key_Right || key->key() == Qt::Key_Up ||
                           key->key() == Qt::Key_Left || key->key() == Qt::Key_Down;
        const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (key->key() == Qt::Key_Escape ||
            (key->key() == Qt::Key_J && key->modifiers() == Qt::NoModifier && m_input->text().isEmpty()) ||
            (arrow && key->modifiers() == Qt::NoModifier) ||
            (enter && !(key->modifiers() &
                        (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))) {
            key->accept();
            return true;
        }
    }
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::NoModifier && m_input->text().isEmpty() &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_J ||
             key->key() == Qt::Key_Right || key->key() == Qt::Key_Left ||
             key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
            if (!key->isAutoRepeat() && m_emptyCommandRequested) m_emptyCommandRequested(key->key());
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            clearInput();
            const auto callback = m_cancelRequested;
            if (callback) {
                callback();
            }
            return true;
        }
        if (key->modifiers() == Qt::NoModifier) {
            switch (key->key()) {
                case Qt::Key_Right: submit(DrawingCardinalDirection::right); return true;
                case Qt::Key_Up: submit(DrawingCardinalDirection::up); return true;
                case Qt::Key_Left: submit(DrawingCardinalDirection::left); return true;
                case Qt::Key_Down: submit(DrawingCardinalDirection::down); return true;
                default: break;
            }
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            submit(m_selectedDirection);
            return true;
        }
    }
    if ((watched == parentWidget() &&
         (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
          event->type() == QEvent::LayoutRequest)) ||
        (watched == this &&
         (event->type() == QEvent::Resize || event->type() == QEvent::Show))) {
        updatePlacement();
    }
    return QWidget::eventFilter(watched, event);
}

void DrawingInputPanel::updatePlacement() {
    if (m_updatingPlacement) {
        return;
    }
    m_updatingPlacement = true;
    constexpr int margin = 12;
    const auto* host = parentWidget();
    const int width = host ? std::max(1, std::min(330, host->width() - margin * 2)) : 330;
    setFixedWidth(width);
    if (layout()) {
        layout()->activate();
    }
    adjustSize();
    if (host) {
        move(margin, std::max(margin, host->height() - height() - margin));
    }
    m_updatingPlacement = false;
}

}  // namespace sketch::desktop
