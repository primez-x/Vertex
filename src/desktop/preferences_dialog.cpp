#include "sketch/desktop/preferences_dialog.hpp"

#include "advanced_section.hpp"
#include "measurement_format.hpp"
#include "vertex_design_tokens.hpp"

#include "sketch/quantity.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

#include <array>
#include <cmath>
#include <exception>
#include <optional>
#include <stdexcept>
#include <vector>

namespace sketch::desktop {

namespace {

constexpr auto key_prefix = "preferences/";

QString key(const char* name) { return QString::fromLatin1(key_prefix) + QString::fromLatin1(name); }

constexpr std::array<int, 5> imperial_precisions{16, 8, 4, 2, 1};
constexpr std::array<int, 3> metric_precisions{1, 5, 10};
constexpr std::array<double, 5> angle_snaps{15.0, 22.5, 30.0, 45.0, 90.0};
constexpr double minimum_autosave_minutes = 0.5;
constexpr double maximum_autosave_minutes = 60.0;

QString units_name(Preferences::Units value) {
    return value == Preferences::Units::metric ? QStringLiteral("metric") : QStringLiteral("imperial");
}
QString style_name(Preferences::ImperialStyle value) {
    return value == Preferences::ImperialStyle::decimal_feet ? QStringLiteral("decimal_feet")
                                                             : QStringLiteral("feet_inches");
}
QString theme_name(Preferences::Theme value) {
    switch (value) {
    case Preferences::Theme::dark: return QStringLiteral("dark");
    case Preferences::Theme::light: return QStringLiteral("light");
    case Preferences::Theme::high_contrast: return QStringLiteral("high_contrast");
    case Preferences::Theme::system: return QStringLiteral("system");
    }
    return QStringLiteral("dark");
}

bool read_bool(QSettings& settings, const char* name, bool fallback) {
    const auto value = settings.value(key(name));
    if (!value.isValid()) return fallback;
    const auto text = value.toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1")) return true;
    if (text == QStringLiteral("false") || text == QStringLiteral("0")) return false;
    return fallback;
}

double read_number(QSettings& settings, const char* name, double fallback) {
    bool ok = false;
    const auto value = settings.value(key(name)).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

double read_length(QSettings& settings, const char* name, double fallback, bool allow_zero) {
    const auto value = read_number(settings, name, fallback);
    return (allow_zero ? value >= 0.0 : value > 0.0) ? value : fallback;
}

template <typename T, std::size_t N>
bool contains(const std::array<T, N>& values, T value) {
    for (const auto candidate : values)
        if (candidate == value) return true;
    return false;
}

constexpr std::array<double Preferences::*, 7> drawing_default_members{
    &Preferences::wall_thickness_m, &Preferences::wall_height_m, &Preferences::door_width_m,
    &Preferences::door_height_m, &Preferences::window_width_m, &Preferences::window_height_m,
    &Preferences::window_sill_m};

// The single default set shipped before market-specific defaults (metric
// sizes with an 8 ft wall). Saved copies of it were never chosen.
Preferences legacy_drawing_defaults() {
    Preferences value;
    value.wall_thickness_m = 0.14;
    value.wall_height_m = 2.4384;
    value.door_width_m = 0.9;
    value.door_height_m = 2.1;
    value.window_width_m = 1.2;
    value.window_height_m = 1.2;
    value.window_sill_m = 0.9;
    return value;
}

bool same_length(double left, double right) { return std::abs(left - right) <= 1e-9; }

} // namespace

Preferences Preferences::defaults(Units units) {
    Preferences value;
    value.units = units;
    if (units == Units::metric) {
        value.wall_thickness_m = 0.14;
        value.wall_height_m = 2.4;
        value.door_width_m = 0.9;
        value.door_height_m = 2.1;
        value.window_width_m = 1.2;
        value.window_height_m = 1.2;
        value.window_sill_m = 0.9;
    }
    return value;
}

bool Preferences::untouchedDrawingDefault(double Preferences::*member, double metres) noexcept {
    static const Preferences imperial = defaults(Units::imperial);
    static const Preferences metric = defaults(Units::metric);
    static const Preferences legacy = legacy_drawing_defaults();
    return same_length(metres, imperial.*member) || same_length(metres, metric.*member) ||
           same_length(metres, legacy.*member);
}

Preferences Preferences::withMarketDrawingDefaults(Units target) const {
    Preferences result = *this;
    const auto market = defaults(target);
    for (const auto member : drawing_default_members)
        if (untouchedDrawingDefault(member, result.*member)) result.*member = market.*member;
    return result;
}

Preferences Preferences::load(QSettings& settings) {
    Preferences result = Preferences::defaults();

    const auto units = settings.value(key("units")).toString();
    if (units == QStringLiteral("metric")) result.units = Units::metric;
    else if (units == QStringLiteral("imperial")) result.units = Units::imperial;
    // Missing or invalid drawing defaults fall back to the stored market's.
    const auto defaults = Preferences::defaults(result.units);

    const auto style = settings.value(key("imperialStyle")).toString();
    if (style == QStringLiteral("decimal_feet")) result.imperial_style = ImperialStyle::decimal_feet;
    else if (style == QStringLiteral("feet_inches")) result.imperial_style = ImperialStyle::feet_inches;

    const auto theme = settings.value(key("theme")).toString();
    for (const auto candidate : {Theme::dark, Theme::light, Theme::high_contrast, Theme::system})
        if (theme == theme_name(candidate)) result.theme = candidate;

    result.grid_enabled = read_bool(settings, "grid", defaults.grid_enabled);
    result.snap_enabled = read_bool(settings, "snap", defaults.snap_enabled);
    result.overview_map_enabled = read_bool(settings, "overviewMap", defaults.overview_map_enabled);
    result.autosave_enabled = read_bool(settings, "autosave", defaults.autosave_enabled);
    result.reopen_last_project = read_bool(settings, "reopenLastProject", defaults.reopen_last_project);

    result.wall_thickness_m = read_length(settings, "wallThicknessM", defaults.wall_thickness_m, false);
    result.wall_height_m = read_length(settings, "wallHeightM", defaults.wall_height_m, false);
    result.door_width_m = read_length(settings, "doorWidthM", defaults.door_width_m, false);
    result.door_height_m = read_length(settings, "doorHeightM", defaults.door_height_m, false);
    result.window_width_m = read_length(settings, "windowWidthM", defaults.window_width_m, false);
    result.window_height_m = read_length(settings, "windowHeightM", defaults.window_height_m, false);
    result.window_sill_m = read_length(settings, "windowSillM", defaults.window_sill_m, true);

    const auto interval = read_number(settings, "autosaveIntervalMinutes", defaults.autosave_interval_minutes);
    if (interval >= minimum_autosave_minutes && interval <= maximum_autosave_minutes)
        result.autosave_interval_minutes = interval;
    const auto imperial = static_cast<int>(read_number(settings, "imperialPrecisionDenominator",
                                                       defaults.imperial_precision_denominator));
    if (contains(imperial_precisions, imperial)) result.imperial_precision_denominator = imperial;
    const auto metric = static_cast<int>(read_number(settings, "metricPrecisionMm", defaults.metric_precision_mm));
    if (contains(metric_precisions, metric)) result.metric_precision_mm = metric;
    const auto angle = read_number(settings, "angleSnapDegrees", defaults.angle_snap_degrees);
    if (contains(angle_snaps, angle)) result.angle_snap_degrees = angle;
    // A saved shipped default (including the pre-market 0.9 m door of an
    // imperial install) was never chosen: it follows the stored units.
    return result.withMarketDrawingDefaults(result.units);
}

void Preferences::save(QSettings& settings) const {
    settings.setValue(key("units"), units_name(units));
    settings.setValue(key("imperialStyle"), style_name(imperial_style));
    settings.setValue(key("theme"), theme_name(theme));
    settings.setValue(key("grid"), grid_enabled);
    settings.setValue(key("snap"), snap_enabled);
    settings.setValue(key("overviewMap"), overview_map_enabled);
    settings.setValue(key("autosave"), autosave_enabled);
    settings.setValue(key("reopenLastProject"), reopen_last_project);
    settings.setValue(key("wallThicknessM"), wall_thickness_m);
    settings.setValue(key("wallHeightM"), wall_height_m);
    settings.setValue(key("doorWidthM"), door_width_m);
    settings.setValue(key("doorHeightM"), door_height_m);
    settings.setValue(key("windowWidthM"), window_width_m);
    settings.setValue(key("windowHeightM"), window_height_m);
    settings.setValue(key("windowSillM"), window_sill_m);
    settings.setValue(key("autosaveIntervalMinutes"), autosave_interval_minutes);
    settings.setValue(key("imperialPrecisionDenominator"), imperial_precision_denominator);
    settings.setValue(key("metricPrecisionMm"), metric_precision_mm);
    settings.setValue(key("angleSnapDegrees"), angle_snap_degrees);
}

WorkspaceTheme resolve_workspace_theme(Preferences::Theme theme, Qt::ColorScheme system_scheme) {
    switch (theme) {
    case Preferences::Theme::dark: return WorkspaceTheme::dark;
    case Preferences::Theme::light: return WorkspaceTheme::light;
    case Preferences::Theme::high_contrast: return WorkspaceTheme::high_contrast;
    case Preferences::Theme::system:
        return system_scheme == Qt::ColorScheme::Light ? WorkspaceTheme::light : WorkspaceTheme::dark;
    }
    return WorkspaceTheme::dark;
}

struct PreferencesDialog::Impl {
    struct LengthField {
        QLineEdit* edit{};
        QString label;
        double Preferences::*member{};
        bool allow_zero{};
        QWidget* page{};
        double metres{};
        QString shown; // text last written by the dialog; unchanged text keeps exact metres
    };

    PreferencesDialog* owner{};
    Preferences accepted;

    QTabWidget* tabs{};
    QWidget* general_page{};
    QWidget* drawing_page{};
    QComboBox* units{};
    QComboBox* imperial_style{};
    QComboBox* theme{};
    QCheckBox* grid{};
    QCheckBox* snap{};
    QCheckBox* overview{};
    QCheckBox* autosave{};
    QCheckBox* reopen{};
    QDoubleSpinBox* autosave_interval{};
    QComboBox* imperial_precision{};
    QComboBox* metric_precision{};
    QComboBox* angle_snap{};
    AdvancedSection* general_advanced{};
    AdvancedSection* drawing_advanced{};
    std::vector<LengthField> lengths;
    QLabel* error{};
    QDialogButtonBox* buttons{};
    bool rendered_metric{};
    Preferences::ImperialStyle rendered_style{};

    [[nodiscard]] bool metric() const { return units->currentData().toInt() == 1; }
    [[nodiscard]] Preferences::ImperialStyle style() const {
        return imperial_style->currentData().toInt() == 1 ? Preferences::ImperialStyle::decimal_feet
                                                          : Preferences::ImperialStyle::feet_inches;
    }

    [[nodiscard]] QString format(double metres) const {
        if (metric()) return units::length_input_text(metres, true);
        if (style() == Preferences::ImperialStyle::decimal_feet) return units::decimal_feet_text(metres);
        return units::length_input_text(metres, false);
    }

    // Parses using the units the text was written in; throws with a message.
    [[nodiscard]] double parse(const LengthField& field, bool metric_input) const {
        const auto text = field.edit->text().trimmed();
        if (text == field.shown) return field.metres;
        const auto example = metric_input ? QStringLiteral("2.4 m or 140 mm") : QStringLiteral("8 ft or 6 in");
        if (text.isEmpty())
            throw std::invalid_argument(QStringLiteral("%1 is empty. Enter a length such as %2.")
                                            .arg(field.label, example).toStdString());
        double metres{};
        try {
            metres = parse_quantity(text.toStdString(), metric_input ? Unit::metre : Unit::foot).metres;
        } catch (const std::exception&) {
            throw std::invalid_argument(QStringLiteral("%1 \"%2\" is not a length. Enter a value such as %3.")
                                            .arg(field.label, text, example).toStdString());
        }
        if (!std::isfinite(metres) || (field.allow_zero ? metres < 0.0 : metres <= 0.0))
            throw std::invalid_argument(
                QStringLiteral("%1 must be %2. Enter a value such as %3.")
                    .arg(field.label,
                         field.allow_zero ? QStringLiteral("zero or greater") : QStringLiteral("greater than zero"),
                         example).toStdString());
        return metres;
    }

    void setLength(LengthField& field, double metres) {
        field.metres = metres;
        field.shown = format(metres);
        field.edit->setText(field.shown);
    }

    // Re-shows every valid length in the newly chosen units; text that does
    // not parse is left for the person to correct. Switching units resets a
    // drawing default the person never changed to the new market's value
    // (a 3' 0" door becomes 900 mm, not 0.914 m); chosen values are kept.
    void rerenderLengths() {
        const bool units_changed = rendered_metric != metric();
        const auto market = Preferences::defaults(metric() ? Preferences::Units::metric
                                                           : Preferences::Units::imperial);
        for (auto& field : lengths) {
            try {
                const bool typed = field.edit->text().trimmed() != field.shown;
                auto metres = parse(field, rendered_metric);
                if (units_changed && !typed && Preferences::untouchedDrawingDefault(field.member, metres))
                    metres = market.*(field.member);
                setLength(field, metres);
            } catch (const std::exception&) {
            }
        }
        rendered_metric = metric();
        rendered_style = style();
        imperial_style->setEnabled(!rendered_metric);
        imperial_precision->setEnabled(!rendered_metric);
        metric_precision->setEnabled(rendered_metric);
    }

    void clearError() {
        error->clear();
        error->hide();
    }

    void showError(const QString& text) {
        error->setText(text);
        error->show();
    }

    static void selectData(QComboBox* combo, const QVariant& value) {
        const auto index = combo->findData(value);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    }

    void load(const Preferences& value) {
        {
            const QSignalBlocker blocker_units(units);
            const QSignalBlocker blocker_style(imperial_style);
            selectData(units, value.units == Preferences::Units::metric ? 1 : 0);
            selectData(imperial_style, value.imperial_style == Preferences::ImperialStyle::decimal_feet ? 1 : 0);
        }
        rendered_metric = metric();
        rendered_style = style();
        imperial_style->setEnabled(!rendered_metric);
        imperial_precision->setEnabled(!rendered_metric);
        metric_precision->setEnabled(rendered_metric);
        selectData(theme, static_cast<int>(value.theme));
        grid->setChecked(value.grid_enabled);
        snap->setChecked(value.snap_enabled);
        overview->setChecked(value.overview_map_enabled);
        autosave->setChecked(value.autosave_enabled);
        reopen->setChecked(value.reopen_last_project);
        autosave_interval->setValue(value.autosave_interval_minutes);
        autosave_interval->setEnabled(value.autosave_enabled);
        selectData(imperial_precision, value.imperial_precision_denominator);
        selectData(metric_precision, value.metric_precision_mm);
        selectData(angle_snap, value.angle_snap_degrees);
        for (auto& field : lengths) setLength(field, value.*(field.member));
        clearError();
    }

    // Builds the candidate from the controls, or throws with the first problem.
    Preferences collect(LengthField** failed) {
        Preferences value;
        value.units = metric() ? Preferences::Units::metric : Preferences::Units::imperial;
        value.imperial_style = style();
        value.theme = static_cast<Preferences::Theme>(theme->currentData().toInt());
        value.grid_enabled = grid->isChecked();
        value.snap_enabled = snap->isChecked();
        value.overview_map_enabled = overview->isChecked();
        value.autosave_enabled = autosave->isChecked();
        value.reopen_last_project = reopen->isChecked();
        value.autosave_interval_minutes = autosave_interval->value();
        value.imperial_precision_denominator = imperial_precision->currentData().toInt();
        value.metric_precision_mm = metric_precision->currentData().toInt();
        value.angle_snap_degrees = angle_snap->currentData().toDouble();
        for (auto& field : lengths) {
            *failed = &field;
            value.*(field.member) = parse(field, rendered_metric);
        }
        *failed = nullptr;
        return value;
    }

    bool submit() {
        LengthField* failed = nullptr;
        try {
            accepted = collect(&failed);
        } catch (const std::exception& problem) {
            showError(QString::fromUtf8(problem.what()));
            if (failed != nullptr) {
                tabs->setCurrentWidget(failed->page);
                failed->edit->setFocus();
                failed->edit->selectAll();
            }
            return false;
        }
        clearError();
        return true;
    }
};

PreferencesDialog::PreferencesDialog(Preferences current, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.accepted = current;
    setObjectName(QStringLiteral("preferencesDialog"));
    setWindowTitle(QStringLiteral("Preferences"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(design::space_4, design::space_4, design::space_4, design::space_4);
    layout->setSpacing(design::space_3);

    d.tabs = new QTabWidget(this);
    d.tabs->setObjectName(QStringLiteral("preferencesTabs"));
    layout->addWidget(d.tabs);

    const auto make_page = [&](const QString& title, const char* name) {
        auto* page = new QWidget(d.tabs);
        page->setObjectName(QString::fromLatin1(name));
        auto* page_layout = new QVBoxLayout(page);
        page_layout->setContentsMargins(design::space_3, design::space_3, design::space_3, design::space_3);
        page_layout->setSpacing(design::space_3);
        d.tabs->addTab(page, title);
        return page;
    };
    const auto make_form = [](QWidget* page) {
        auto* form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(design::space_3);
        form->setVerticalSpacing(design::space_2);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        static_cast<QVBoxLayout*>(page->layout())->addLayout(form);
        return form;
    };
    const auto make_combo = [](QWidget* page, const char* name, const QString& accessible) {
        auto* combo = new QComboBox(page);
        combo->setObjectName(QString::fromLatin1(name));
        combo->setAccessibleName(accessible);
        return combo;
    };
    const auto make_check = [](QWidget* page, const char* name, const QString& text) {
        auto* check = new QCheckBox(text, page);
        check->setObjectName(QString::fromLatin1(name));
        return check;
    };

    // General: everyday display and workspace behaviour.
    d.general_page = make_page(QStringLiteral("General"), "preferencesGeneralPage");
    auto* general = make_form(d.general_page);
    d.units = make_combo(d.general_page, "preferencesUnits", QStringLiteral("Units"));
    d.units->addItem(QStringLiteral("Imperial (ft, in)"), 0);
    d.units->addItem(QStringLiteral("Metric (m)"), 1);
    general->addRow(QStringLiteral("Units"), d.units);
    d.imperial_style = make_combo(d.general_page, "preferencesImperialStyle", QStringLiteral("Imperial lengths"));
    d.imperial_style->addItem(QStringLiteral("Feet and inches (24' 6\")"), 0);
    d.imperial_style->addItem(QStringLiteral("Decimal feet (24.50 ft)"), 1);
    general->addRow(QStringLiteral("Imperial lengths"), d.imperial_style);
    d.theme = make_combo(d.general_page, "preferencesTheme", QStringLiteral("Theme"));
    d.theme->addItem(QStringLiteral("Dark"), static_cast<int>(Preferences::Theme::dark));
    d.theme->addItem(QStringLiteral("Light"), static_cast<int>(Preferences::Theme::light));
    d.theme->addItem(QStringLiteral("High contrast"), static_cast<int>(Preferences::Theme::high_contrast));
    d.theme->addItem(QStringLiteral("Match system"), static_cast<int>(Preferences::Theme::system));
    general->addRow(QStringLiteral("Theme"), d.theme);
    d.grid = make_check(d.general_page, "preferencesGrid", QStringLiteral("Show grid"));
    d.snap = make_check(d.general_page, "preferencesSnap", QStringLiteral("Snap while drawing"));
    d.overview = make_check(d.general_page, "preferencesOverviewMap", QStringLiteral("Show overview map"));
    d.autosave = make_check(d.general_page, "preferencesAutosave",
                            QStringLiteral("Save a recovery copy while I work"));
    d.reopen = make_check(d.general_page, "preferencesReopenLastProject",
                          QStringLiteral("Reopen the last project on startup"));
    for (auto* check : {d.grid, d.snap, d.overview, d.autosave, d.reopen}) general->addRow(QString{}, check);

    d.general_advanced = new AdvancedSection(QStringLiteral("preferencesGeneral"), d.general_page);
    d.autosave_interval = new QDoubleSpinBox(d.general_advanced->body());
    d.autosave_interval->setObjectName(QStringLiteral("preferencesAutosaveInterval"));
    d.autosave_interval->setAccessibleName(QStringLiteral("Recovery save interval"));
    d.autosave_interval->setRange(minimum_autosave_minutes, maximum_autosave_minutes);
    d.autosave_interval->setDecimals(1);
    d.autosave_interval->setSingleStep(0.5);
    d.autosave_interval->setSuffix(QStringLiteral(" min"));
    d.autosave_interval->setToolTip(QStringLiteral("Longest wait between recovery copies while you keep editing."));
    d.general_advanced->form()->addRow(QStringLiteral("Recovery save interval"), d.autosave_interval);
    d.imperial_precision = new QComboBox(d.general_advanced->body());
    d.imperial_precision->setObjectName(QStringLiteral("preferencesImperialPrecision"));
    d.imperial_precision->setAccessibleName(QStringLiteral("Imperial precision"));
    for (const auto denominator : imperial_precisions)
        d.imperial_precision->addItem(denominator == 1 ? QStringLiteral("1\"")
                                                       : QStringLiteral("1/%1\"").arg(denominator),
                                      denominator);
    d.general_advanced->form()->addRow(QStringLiteral("Imperial precision"), d.imperial_precision);
    d.metric_precision = new QComboBox(d.general_advanced->body());
    d.metric_precision->setObjectName(QStringLiteral("preferencesMetricPrecision"));
    d.metric_precision->setAccessibleName(QStringLiteral("Metric precision"));
    for (const auto millimetres : metric_precisions)
        d.metric_precision->addItem(QStringLiteral("%1 mm").arg(millimetres), millimetres);
    d.general_advanced->form()->addRow(QStringLiteral("Metric precision"), d.metric_precision);
    static_cast<QVBoxLayout*>(d.general_page->layout())->addWidget(d.general_advanced);
    static_cast<QVBoxLayout*>(d.general_page->layout())->addStretch(1);

    // Drawing defaults: starting sizes for new walls, doors and windows.
    d.drawing_page = make_page(QStringLiteral("Drawing defaults"), "preferencesDrawingPage");
    auto* drawing = make_form(d.drawing_page);
    const auto add_length = [&](const QString& label, const char* name, double Preferences::*member,
                                bool allow_zero) {
        auto* edit = new QLineEdit(d.drawing_page);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setAccessibleName(label);
        edit->setToolTip(QStringLiteral("Enter a length with units, for example 140 mm, 0.14 m, 8 ft or 6 in."));
        drawing->addRow(label, edit);
        d.lengths.push_back({edit, label, member, allow_zero, d.drawing_page, 0.0, {}});
        QObject::connect(edit, &QLineEdit::textEdited, this, [this] { m_impl->clearError(); });
    };
    add_length(QStringLiteral("Wall thickness"), "preferencesWallThickness", &Preferences::wall_thickness_m, false);
    add_length(QStringLiteral("Wall height"), "preferencesWallHeight", &Preferences::wall_height_m, false);
    add_length(QStringLiteral("Door width"), "preferencesDoorWidth", &Preferences::door_width_m, false);
    add_length(QStringLiteral("Door height"), "preferencesDoorHeight", &Preferences::door_height_m, false);
    add_length(QStringLiteral("Window width"), "preferencesWindowWidth", &Preferences::window_width_m, false);
    add_length(QStringLiteral("Window height"), "preferencesWindowHeight", &Preferences::window_height_m, false);
    add_length(QStringLiteral("Window sill height"), "preferencesWindowSill", &Preferences::window_sill_m, true);

    d.drawing_advanced = new AdvancedSection(QStringLiteral("preferencesDrawing"), d.drawing_page);
    d.angle_snap = new QComboBox(d.drawing_advanced->body());
    d.angle_snap->setObjectName(QStringLiteral("preferencesAngleSnap"));
    d.angle_snap->setAccessibleName(QStringLiteral("Angle snap"));
    d.angle_snap->setToolTip(QStringLiteral("Rotation snaps to this step. Hold Shift for any angle."));
    for (const auto degrees : angle_snaps) d.angle_snap->addItem(units::angle_text(degrees), degrees);
    d.drawing_advanced->form()->addRow(QStringLiteral("Angle snap"), d.angle_snap);
    static_cast<QVBoxLayout*>(d.drawing_page->layout())->addWidget(d.drawing_advanced);
    static_cast<QVBoxLayout*>(d.drawing_page->layout())->addStretch(1);

    // Inline message: what happened and what to do.
    d.error = new QLabel(this);
    d.error->setObjectName(QStringLiteral("preferencesError"));
    d.error->setWordWrap(true);
    d.error->setAccessibleName(QStringLiteral("Preferences problem"));
    const bool light_surface = palette().color(QPalette::Window).lightness() > 128;
    d.error->setStyleSheet(QStringLiteral("color: %1;").arg(design::css(design::for_surface(light_surface).danger)));
    d.error->hide();
    layout->addWidget(d.error);

    d.buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    d.buttons->setObjectName(QStringLiteral("preferencesButtons"));
    d.buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Save preferences"));
    d.buttons->button(QDialogButtonBox::Save)->setDefault(true);
    d.buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    d.buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("Restore defaults"));
    layout->addWidget(d.buttons);

    QObject::connect(d.buttons, &QDialogButtonBox::accepted, this, [this] { submit(); });
    QObject::connect(d.buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(d.buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
                     [this] { restoreDefaults(); });
    QObject::connect(d.units, &QComboBox::currentIndexChanged, this, [this] { m_impl->rerenderLengths(); });
    QObject::connect(d.imperial_style, &QComboBox::currentIndexChanged, this, [this] { m_impl->rerenderLengths(); });
    QObject::connect(d.autosave, &QCheckBox::toggled, d.autosave_interval, &QWidget::setEnabled);

    d.load(current);
}

PreferencesDialog::~PreferencesDialog() = default;

Preferences PreferencesDialog::result() const { return m_impl->accepted; }

void PreferencesDialog::restoreDefaults() { m_impl->load(Preferences::defaults()); }

bool PreferencesDialog::submit() {
    if (!m_impl->submit()) return false;
    accept();
    return true;
}

QString PreferencesDialog::errorText() const { return m_impl->error->text(); }

} // namespace sketch::desktop
