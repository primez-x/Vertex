#pragma once

// Collapsed "Advanced" section shared by every dialog (Vertex design system,
// InspectorSection pattern). Everyday controls sit above it; expert controls
// go inside it at documented defaults so most people never open it.
// The expanded state is remembered per dialog through QSettings.

#include <QFormLayout>
#include <QSettings>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace sketch::desktop {

class AdvancedSection : public QWidget {
public:
    // settings_key identifies the hosting dialog, e.g. "buildingObjectDialog".
    explicit AdvancedSection(const QString& settings_key, QWidget* parent = nullptr,
                             const QString& title = QStringLiteral("Advanced"))
        : QWidget(parent), m_key(QStringLiteral("advancedSection/") + settings_key) {
        setObjectName(QStringLiteral("advancedSection"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        m_toggle = new QToolButton(this);
        m_toggle->setObjectName(QStringLiteral("advancedSectionToggle"));
        m_toggle->setText(title);
        m_toggle->setCheckable(true);
        m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_toggle->setAutoRaise(true);
        m_body = new QWidget(this);
        m_body->setObjectName(QStringLiteral("advancedSectionBody"));
        m_form = new QFormLayout(m_body);
        m_form->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_toggle);
        layout->addWidget(m_body);
        bool expanded = false;
        try {
            expanded = QSettings().value(m_key, false).toBool();
        } catch (...) {
            expanded = false;
        }
        setExpanded(expanded);
        QObject::connect(m_toggle, &QToolButton::toggled, this, [this](bool on) {
            setExpanded(on);
            QSettings().setValue(m_key, on);
        });
    }

    QFormLayout* form() const noexcept { return m_form; }
    QWidget* body() const noexcept { return m_body; }
    bool expanded() const noexcept { return m_body->isVisibleTo(this); }

    // Opens the section without persisting, e.g. to reveal an invalid advanced field.
    void reveal() { setExpanded(true); }

    void setExpanded(bool expanded) {
        m_toggle->blockSignals(true);
        m_toggle->setChecked(expanded);
        m_toggle->blockSignals(false);
        m_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_body->setVisible(expanded);
    }

private:
    QString m_key;
    QToolButton* m_toggle{};
    QWidget* m_body{};
    QFormLayout* m_form{};
};

} // namespace sketch::desktop
