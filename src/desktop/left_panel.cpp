#include "left_panel.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace sketch::desktop {
namespace {

struct DrawingKind {
    std::string_view type;
    const char* singular;
    const char* key;
    const char* plural;
};

// Order is irrelevant; the navigator sorts groups by their label.
constexpr std::array<DrawingKind, 22> drawing_kinds{{
    {"wall", "Wall", "walls", "Walls"},
    {"opening", "Opening", "openings", "Openings"},
    {"corner_window", "Corner window", "openings", "Openings"},
    {"measurement_boundary", "Area", "areas", "Areas"},
    {"boundary", "Area", "areas", "Areas"},
    {"room_boundary", "Room outline", "room-outlines", "Room outlines"},
    {"room", "Room", "rooms", "Rooms"},
    {"measurement_linework", "Measured line", "measured-lines", "Measured lines"},
    {"dimension", "Dimension", "dimensions", "Dimensions"},
    {"slab", "Slab", "slabs", "Slabs"},
    {"roof", "Roof", "roofs", "Roofs"},
    {"stair", "Stair", "stairs", "Stairs"},
    {"railing", "Railing", "railings", "Railings"},
    {"column", "Column", "columns", "Columns"},
    {"beam", "Beam", "beams", "Beams"},
    {"terrain_surface", "Terrain surface", "terrain", "Terrain"},
    {"assembly_instance", "Assembly", "assemblies", "Assemblies"},
    {"reference_asset", "Reference", "references", "References"},
    {"ifc_reference", "IFC reference", "references", "References"},
    {"reference_grid", "Reference grid", "reference-grids", "Reference grids"},
    {"label", "Label", "labels", "Labels"},
    {"text", "Text", "labels", "Labels"},
}};

const DrawingKind* find_kind(std::string_view type) noexcept {
    for (const auto& kind : drawing_kinds)
        if (kind.type == type) return &kind;
    return nullptr;
}

QString settings_path() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/left-panel.json");
}

bool remembers_panel_state() {
    // Only the installed application remembers its panel. Test executables
    // and embedding hosts use their own names and always start predictably.
    return !QStandardPaths::isTestModeEnabled() &&
           QCoreApplication::applicationName() == QStringLiteral("Vertex") &&
           QCoreApplication::organizationName() == QStringLiteral("Vertex");
}

} // namespace

QString library_display_name(const QString& raw) {
    static const QStringList acronyms{
        QStringLiteral("AC"), QStringLiteral("ADA"), QStringLiteral("ATM"), QStringLiteral("BBQ"),
        QStringLiteral("DVD"), QStringLiteral("DXF"), QStringLiteral("EV"), QStringLiteral("GLA"),
        QStringLiteral("HVAC"), QStringLiteral("IFC"), QStringLiteral("LED"), QStringLiteral("PC"),
        QStringLiteral("TV"), QStringLiteral("USB"), QStringLiteral("WC"), QStringLiteral("2D"),
        QStringLiteral("3D")};
    auto text = raw.trimmed();
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.replace(QRegularExpression(QStringLiteral("(?<=\\S)-(?=\\S)")), QStringLiteral(" "));
    const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList result;
    result.reserve(words.size());
    for (const auto& word : words) {
        const auto upper = word.toUpper();
        if (acronyms.contains(upper)) {
            result.append(upper);
            continue;
        }
        // Keep model numbers and mixed tokens ("W2", "910mm") as written,
        // but sentence-case ordinary words.
        auto lowered = word.toLower();
        if (result.isEmpty() && !lowered.isEmpty()) lowered[0] = lowered[0].toUpper();
        result.append(lowered);
    }
    return result.join(QLatin1Char(' '));
}

bool navigator_is_hierarchy_type(std::string_view type) noexcept {
    return type == "property" || type == "building" || type == "floor" || type == "layer";
}

bool navigator_is_drawing_type(std::string_view type) noexcept {
    return find_kind(type) != nullptr;
}

QString navigator_object_label(std::string_view type) {
    if (const auto* kind = find_kind(type)) return QString::fromLatin1(kind->singular);
    return library_display_name(QString::fromUtf8(type.data(), static_cast<qsizetype>(type.size())));
}

QString navigator_kind_key(std::string_view type) {
    if (const auto* kind = find_kind(type)) return QString::fromLatin1(kind->key);
    return QStringLiteral("other");
}

QString navigator_kind_label(std::string_view type) {
    if (const auto* kind = find_kind(type)) return QString::fromLatin1(kind->plural);
    return QStringLiteral("Other objects");
}

int remembered_left_panel_tab(int fallback) {
    if (!remembers_panel_state()) return fallback;
    QFile file(settings_path());
    if (!file.exists() || file.size() > 4096 || !file.open(QIODevice::ReadOnly)) return fallback;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return fallback;
    const auto value = document.object().value(QStringLiteral("tab"));
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

void remember_left_panel_tab(int index) {
    if (!remembers_panel_state() || index < 0) return;
    const auto path = settings_path();
    (void)QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("tab"), index}}).toJson(QJsonDocument::Compact));
    (void)file.commit();
}

CollapsibleSection::CollapsibleSection(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_header = new QToolButton(this);
    m_header->setObjectName(QStringLiteral("collapsibleSectionHeader"));
    m_header->setText(title);
    m_header->setAccessibleName(title);
    m_header->setCheckable(true);
    m_header->setAutoRaise(true);
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setArrowType(Qt::RightArrow);
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(m_header);
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("collapsibleSectionBody"));
    auto* body_layout = new QVBoxLayout(m_body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(4);
    layout->addWidget(m_body);
    m_body->hide();
    QObject::connect(m_header, &QToolButton::toggled, this, [this](bool expanded) {
        m_header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_body->setVisible(expanded);
    });
}

bool CollapsibleSection::isExpanded() const noexcept {
    return m_header->isChecked();
}

void CollapsibleSection::setExpanded(bool expanded) {
    m_header->setChecked(expanded);
}

} // namespace sketch::desktop
