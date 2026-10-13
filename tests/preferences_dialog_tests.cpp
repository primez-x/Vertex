#include "sketch/desktop/preferences_dialog.hpp"

#include "sketch/quantity.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/measurement_format.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using sketch::desktop::Preferences;
using sketch::desktop::PreferencesDialog;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void require_near(double actual, double expected, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-9)
        throw std::runtime_error(std::string(message) + " (got " + std::to_string(actual) + ")");
}

template <typename T>
T& child(PreferencesDialog& dialog, const char* name) {
    auto* result = dialog.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, std::string("missing control: ") + name);
    return *result;
}

void select_data(QComboBox& combo, const QVariant& value) {
    const auto index = combo.findData(value);
    require(index >= 0, "combo value missing");
    combo.setCurrentIndex(index);
}

void test_defaults() {
    const auto value = Preferences::defaults();
    require(value.units == Preferences::Units::imperial, "default units are imperial");
    require(value.imperial_style == Preferences::ImperialStyle::feet_inches, "default feet-inches");
    require(value.theme == Preferences::Theme::dark, "default theme is dark");
    require(value.grid_enabled && value.snap_enabled && value.overview_map_enabled, "grid, snap, overview on");
    require(value.autosave_enabled && !value.reopen_last_project, "autosave on, reopen off");
    require_near(value.wall_thickness_m, 0.14, "wall thickness 140 mm");
    require_near(value.wall_height_m, 8 * 0.3048, "wall height 8 ft");
    require_near(value.door_width_m, 0.9, "door width");
    require_near(value.door_height_m, 2.1, "door height");
    require_near(value.window_width_m, 1.2, "window width");
    require_near(value.window_height_m, 1.2, "window height");
    require_near(value.window_sill_m, 0.9, "window sill");
    require_near(value.autosave_interval_minutes, 0.5, "autosave interval 30 s");
    require(value.imperial_precision_denominator == 16 && value.metric_precision_mm == 1, "precision");
    require_near(value.angle_snap_degrees, 45.0, "angle snap");
    require(sketch::desktop::resolve_workspace_theme(Preferences::Theme::system, Qt::ColorScheme::Light) ==
                sketch::WorkspaceTheme::light, "system light");
    require(sketch::desktop::resolve_workspace_theme(Preferences::Theme::system, Qt::ColorScheme::Unknown) ==
                sketch::WorkspaceTheme::dark, "system unknown falls back to dark");
}

void test_settings_round_trip(const QString& directory) {
    const auto path = directory + QStringLiteral("/round_trip.ini");
    {
        QSettings empty(path, QSettings::IniFormat);
        require(Preferences::load(empty) == Preferences::defaults(), "empty settings load defaults");
    }
    Preferences value;
    value.units = Preferences::Units::metric;
    value.imperial_style = Preferences::ImperialStyle::decimal_feet;
    value.theme = Preferences::Theme::high_contrast;
    value.grid_enabled = false;
    value.snap_enabled = false;
    value.overview_map_enabled = false;
    value.autosave_enabled = false;
    value.reopen_last_project = true;
    value.wall_thickness_m = 0.2;
    value.wall_height_m = 2.7;
    value.door_width_m = 0.8;
    value.door_height_m = 2.0;
    value.window_width_m = 1.5;
    value.window_height_m = 1.4;
    value.window_sill_m = 0.0;
    value.autosave_interval_minutes = 5.0;
    value.imperial_precision_denominator = 8;
    value.metric_precision_mm = 5;
    value.angle_snap_degrees = 15.0;
    {
        QSettings settings(path, QSettings::IniFormat);
        value.save(settings);
        settings.sync();
        require(settings.status() == QSettings::NoError, "settings written");
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        require(Preferences::load(settings) == value, "round trip through INI QSettings");
    }
    {
        QSettings damaged(directory + QStringLiteral("/damaged.ini"), QSettings::IniFormat);
        damaged.setValue("preferences/units", "furlongs");
        damaged.setValue("preferences/wallThicknessM", -1.0);
        damaged.setValue("preferences/grid", "maybe");
        damaged.setValue("preferences/angleSnapDegrees", 7.0);
        damaged.setValue("preferences/autosaveIntervalMinutes", 0.0);
        require(Preferences::load(damaged) == Preferences::defaults(), "invalid values fall back to defaults");
    }
}

void test_dialog_edits() {
    PreferencesDialog dialog(Preferences::defaults());
    auto& thickness = child<QLineEdit>(dialog, "preferencesWallThickness");
    auto& height = child<QLineEdit>(dialog, "preferencesWallHeight");
    require(thickness.text() == QStringLiteral("5 1/2\""), "140 mm shows in feet-inches");
    require(height.text() == QStringLiteral("8' 0\""), "8 ft wall height shown");

    // Unchanged fields keep exact values (no rounding through display text).
    require(dialog.submit(), "defaults submit");
    require(dialog.result() == Preferences::defaults(), "untouched dialog returns defaults exactly");

    PreferencesDialog edited(Preferences::defaults());
    select_data(child<QComboBox>(edited, "preferencesTheme"), static_cast<int>(Preferences::Theme::light));
    child<QCheckBox>(edited, "preferencesGrid").setChecked(false);
    child<QCheckBox>(edited, "preferencesReopenLastProject").setChecked(true);
    child<QLineEdit>(edited, "preferencesWallHeight").setText(QStringLiteral("9 ft"));
    child<QLineEdit>(edited, "preferencesDoorWidth").setText(QStringLiteral("36 in"));
    child<QLineEdit>(edited, "preferencesWindowSill").setText(QStringLiteral("2.4 m"));
    child<QDoubleSpinBox>(edited, "preferencesAutosaveInterval").setValue(2.0);
    select_data(child<QComboBox>(edited, "preferencesAngleSnap"), 15.0);
    // Switching units re-shows edited text in metres without losing it.
    select_data(child<QComboBox>(edited, "preferencesUnits"), 1);
    require(child<QLineEdit>(edited, "preferencesWallHeight").text() == QStringLiteral("2.743 m"),
            "9 ft re-shown in metres");
    require(child<QLineEdit>(edited, "preferencesWallThickness").text() == QStringLiteral("0.14 m"),
            "140 mm re-shown in metres");
    require(edited.submit(), "edited submit");
    const auto result = edited.result();
    require(result.units == Preferences::Units::metric, "units metric");
    require(result.theme == Preferences::Theme::light, "theme light");
    require(!result.grid_enabled && result.reopen_last_project, "toggles");
    require_near(result.wall_thickness_m, 0.14, "thickness kept exact across unit switch");
    require_near(result.wall_height_m, 9 * 0.3048, "wall height 9 ft exact");
    require_near(result.door_width_m, 36 * 0.0254, "door width 36 in");
    require_near(result.window_sill_m, 2.4, "sill 2.4 m");
    require_near(result.autosave_interval_minutes, 2.0, "interval");
    require_near(result.angle_snap_degrees, 15.0, "angle snap");

    // Bare numbers use the chosen units: metres in metric.
    PreferencesDialog metric_dialog(result);
    child<QLineEdit>(metric_dialog, "preferencesDoorHeight").setText(QStringLiteral("2.05"));
    require(metric_dialog.submit(), "metric submit");
    require_near(metric_dialog.result().door_height_m, 2.05, "bare number parsed as metres");
}

void test_restore_defaults() {
    Preferences custom;
    custom.units = Preferences::Units::metric;
    custom.theme = Preferences::Theme::system;
    custom.snap_enabled = false;
    custom.wall_thickness_m = 0.3;
    custom.angle_snap_degrees = 90.0;
    PreferencesDialog dialog(custom);
    require(child<QLineEdit>(dialog, "preferencesWallThickness").text() == QStringLiteral("0.3 m"),
            "custom thickness shown in metres");
    auto* button = child<QDialogButtonBox>(dialog, "preferencesButtons").button(QDialogButtonBox::RestoreDefaults);
    require(button != nullptr && button->text() == QStringLiteral("Restore defaults"), "restore button");
    child<QLineEdit>(dialog, "preferencesDoorWidth").setText(QStringLiteral("nonsense"));
    button->click();
    require(dialog.errorText().isEmpty(), "restore clears messages");
    require(child<QLineEdit>(dialog, "preferencesWallThickness").text() == QStringLiteral("5 1/2\""),
            "restore re-shows imperial");
    require(dialog.submit(), "submit after restore");
    require(dialog.result() == Preferences::defaults(), "restore defaults produces defaults");

    PreferencesDialog cancelled(custom);
    child<QCheckBox>(cancelled, "preferencesSnap").setChecked(true);
    child<QDialogButtonBox>(cancelled, "preferencesButtons").button(QDialogButtonBox::Cancel)->click();
    require(cancelled.result() == custom, "cancel keeps the original value");
}

void test_invalid_length_blocks_save() {
    PreferencesDialog dialog(Preferences::defaults());
    auto& tabs = child<QTabWidget>(dialog, "preferencesTabs");
    tabs.setCurrentIndex(0);
    child<QLineEdit>(dialog, "preferencesWallThickness").setText(QStringLiteral("thick"));
    auto* save = child<QDialogButtonBox>(dialog, "preferencesButtons").button(QDialogButtonBox::Save);
    require(save != nullptr && save->text() == QStringLiteral("Save preferences"), "save button verb");
    save->click();
    require(dialog.result() == Preferences::defaults(), "invalid input leaves result unchanged");
    require(dialog.result() == Preferences::defaults() && dialog.errorText().contains(QStringLiteral("Wall thickness")),
            "message names the field");
    require(dialog.errorText().contains(QStringLiteral("not a length")), "message says what happened");
    require(tabs.currentWidget()->objectName() == QStringLiteral("preferencesDrawingPage"),
            "invalid field's tab is shown");
    require(dialog.result() == Preferences::defaults(), "not accepted");

    child<QLineEdit>(dialog, "preferencesWallThickness").setText(QStringLiteral("0 in"));
    require(!dialog.submit(), "zero thickness rejected");
    require(dialog.errorText().contains(QStringLiteral("greater than zero")), "zero message");
    child<QLineEdit>(dialog, "preferencesWallThickness").setText(QString{});
    require(!dialog.submit(), "empty thickness rejected");
    child<QLineEdit>(dialog, "preferencesWallThickness").setText(QStringLiteral("6 in"));
    child<QLineEdit>(dialog, "preferencesWindowSill").setText(QStringLiteral("-1 ft"));
    require(!dialog.submit(), "negative sill rejected");
    child<QLineEdit>(dialog, "preferencesWindowSill").setText(QStringLiteral("0"));
    require(dialog.submit(), "zero sill allowed");
    require(dialog.errorText().isEmpty(), "message cleared after valid save");
    require(dialog.result().window_sill_m == 0.0, "sill zero");
    require_near(dialog.result().wall_thickness_m, 6 * 0.0254, "thickness 6 in");
}

} // namespace

void require_text(const QString& actual, const QString& expected, std::string_view message) {
    if (actual != expected)
        throw std::runtime_error(std::string(message) + " (got \"" + actual.toStdString() +
                                 "\", expected \"" + expected.toStdString() + "\")");
}

// Display precision from Preferences: presentation only, ANSI untouched.
void test_display_precision() {
    namespace units = sketch::desktop::units;
    struct Restore { ~Restore() { units::set_display_precision(units::default_display_precision()); } } restore;
    constexpr double inch = 0.0254;
    const double length = 294.19 * inch;      // 24' 6.19"
    const double feet_24_5 = 24.5 * 0.3048;
    const double tiny = inch * 3.0 / 64.0;  // 0.75/16 in, 0.375/8 in
    const double metric = 2.2537;
    const auto ansi_length = units::ansi_length_text(length);
    const auto ansi_area = units::ansi_area_text(171.23);

    require(units::display_precision() == units::default_display_precision(), "default precision active");
    require_text(units::length_text(length, false), QStringLiteral("24' 6 3/16\""), "default 1/16 inch");
    require_text(units::length_text(tiny, false), QStringLiteral("1/16\""), "3/64 inch rounds to 1/16");
    require_text(units::length_text(metric, true), QStringLiteral("2.254 m"), "default 1 mm");

    units::DisplayPrecision eighth;
    eighth.imperial_denominator = 8;
    units::set_display_precision(eighth);
    require_text(units::length_text(length, false), QStringLiteral("24' 6 1/4\""), "1/8 inch precision");
    require_text(units::length_input_text(length, false), QStringLiteral("24' 6 1/4\""), "1/8 inch input text");
    require_text(units::length_text(tiny, false), QStringLiteral("≈ 0\""), "approximate below 1/8 inch");
    require_text(units::length_input_text(tiny, false), QStringLiteral("0\""), "input text never approximate");
    require_text(units::length_text(length, false, units::default_display_precision()),
                 QStringLiteral("24' 6 3/16\""), "explicit precision overrides the active one");
    require_text(units::ansi_length_text(length), ansi_length, "ANSI length ignores precision");
    require_text(units::ansi_area_text(171.23), ansi_area, "ANSI area ignores precision");

    units::DisplayPrecision whole_inch;
    whole_inch.imperial_denominator = 1;
    units::set_display_precision(whole_inch);
    require_text(units::length_text(length, false), QStringLiteral("24' 6\""), "1 inch precision");

    units::DisplayPrecision decimal;
    decimal.decimal_feet = true;
    units::set_display_precision(decimal);
    require_text(units::length_text(feet_24_5, false), QStringLiteral("24.50 ft"), "decimal feet");
    require_text(units::length_text(-feet_24_5, false), QStringLiteral("-24.50 ft"), "negative decimal feet");
    require_text(units::length_text(0.0001, false), QStringLiteral("≈ 0.00 ft"), "approximate decimal zero");
    require_text(units::length_input_text(0.0001, false), QStringLiteral("0.00 ft"), "decimal input not approximate");
    require_text(units::length_text(metric, true), QStringLiteral("2.254 m"), "decimal feet leaves metric alone");
    require_near(sketch::parse_quantity(units::length_input_text(feet_24_5, false).toStdString()).metres,
                 feet_24_5, "decimal feet input text parses");
    require_text(units::ansi_length_text(length), ansi_length, "ANSI length ignores decimal feet");

    units::DisplayPrecision five_mm;
    five_mm.metric_step_mm = 5.0;
    units::set_display_precision(five_mm);
    require_text(units::length_text(metric, true), QStringLiteral("2.255 m"), "5 mm precision");
    require_text(units::length_text(0.002, true), QStringLiteral("≈ 0 m"), "approximate below 5 mm");
    require_text(units::length_input_text(0.002, true), QStringLiteral("0 m"), "metric input not approximate");
    require_text(units::length_text(length, false), QStringLiteral("24' 6 3/16\""), "5 mm leaves imperial alone");
    units::DisplayPrecision ten_mm;
    ten_mm.metric_step_mm = 10.0;
    units::set_display_precision(ten_mm);
    require_text(units::length_text(metric, true), QStringLiteral("2.25 m"), "10 mm precision");
    require_text(units::area_text(171.23, true), QStringLiteral("171.2 m²"), "areas keep one decimal");

    units::DisplayPrecision invalid;
    invalid.imperial_denominator = 0;
    invalid.metric_step_mm = -1.0;
    units::set_display_precision(invalid);
    require(units::display_precision() == units::default_display_precision(), "invalid steps fall back");
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QApplication::setApplicationName(QStringLiteral("preferences_dialog_tests"));
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        // AdvancedSection remembers its expanded state through QSettings();
        // keep that in the temporary directory too.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        test_defaults();
        test_settings_round_trip(directory.path());
        test_dialog_edits();
        test_restore_defaults();
        test_invalid_length_blocks_save();
        test_display_precision();
    } catch (const std::exception& error) {
        std::cerr << "preferences_dialog_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "preferences_dialog_tests passed\n";
    return 0;
}
