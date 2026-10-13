#pragma once

// Application preferences: one plain value type plus the single Preferences
// dialog that edits it. Vertex ships sensible defaults; the dialog shows only
// everyday options and keeps expert ones inside a collapsed Advanced section.
//
// Lengths are stored in metres. The dialog shows and accepts them in the
// chosen units through sketch::desktop::units and sketch::parse_quantity, so
// "8 ft", "2.4 m", "140 mm" and "6 in" are all valid entries.

#include "sketch/workspace_accessibility.hpp"

#include <QDialog>
#include <QSettings>
#include <Qt>

#include <memory>

namespace sketch::desktop {

struct Preferences {
    enum class Units { imperial, metric };
    enum class ImperialStyle { feet_inches, decimal_feet };
    enum class Theme { dark, light, high_contrast, system };

    // General
    Units units{Units::imperial};
    ImperialStyle imperial_style{ImperialStyle::feet_inches};
    Theme theme{Theme::dark};
    bool grid_enabled{true};
    bool snap_enabled{true};
    bool overview_map_enabled{true};
    bool autosave_enabled{true};
    bool reopen_last_project{false};

    // Drawing defaults (metres). Each market starts at its own standard
    // sizes (see defaults(Units)); these initializers are the imperial (US)
    // ones because imperial is the default unit system.
    double wall_thickness_m{0.1524};   // 6"
    double wall_height_m{2.4384};      // 8' 0"
    double door_width_m{0.9144};       // 3' 0"
    double door_height_m{2.032};       // 6' 8"
    double window_width_m{0.9144};     // 3' 0"
    double window_height_m{1.2192};    // 4' 0"
    double window_sill_m{0.9144};      // 3' 0"

    // Advanced
    // Longest wait between recovery saves while editing continues (the
    // autosave scheduler's maximum interval). 0.5 min = the shipped 30 s.
    double autosave_interval_minutes{0.5};
    // Display rounding step: 1/N inch (16, 8, 4, 2 or 1) and whole millimetres
    // (1, 5 or 10). Presentation only; stored values are never rounded.
    int imperial_precision_denominator{16};
    int metric_precision_mm{1};
    // Rotation / placement angle snap step; Shift always gives free angles.
    double angle_snap_degrees{45.0};

    [[nodiscard]] static Preferences defaults() { return Preferences{}; }
    // Shipped defaults with the drawing defaults of that market:
    //   imperial: wall 6" × 8' 0", door 3' 0" × 6' 8", window 3' 0" × 4' 0"
    //             with a 3' 0" sill;
    //   metric:   wall 140 mm × 2.4 m, door 900 × 2100 mm, window
    //             1200 × 1200 mm with a 900 mm sill.
    [[nodiscard]] static Preferences defaults(Units units);
    // True when the drawing default `member` still holds a shipped value (either
    // market's default, or the single pre-market default set), i.e. the person
    // never chose it.
    [[nodiscard]] static bool untouchedDrawingDefault(double Preferences::*member, double metres) noexcept;
    // This value with every untouched drawing default replaced by `units`'s
    // market default; chosen values are kept exactly.
    [[nodiscard]] Preferences withMarketDrawingDefaults(Units units) const;
    // Reads keys under "preferences/"; missing or invalid values fall back to
    // the defaults so a damaged settings file never blocks startup. Drawing
    // defaults that still hold a shipped value follow the stored units.
    [[nodiscard]] static Preferences load(QSettings& settings);
    void save(QSettings& settings) const;

    [[nodiscard]] bool metric() const noexcept { return units == Units::metric; }

    bool operator==(const Preferences&) const = default;
};

// Maps the theme preference onto the workspace theme. "System" follows the
// platform colour scheme (Qt::ColorScheme::Light -> light, otherwise dark,
// the design system's primary theme).
[[nodiscard]] WorkspaceTheme resolve_workspace_theme(Preferences::Theme theme,
                                                     Qt::ColorScheme system_scheme);

class PreferencesDialog final : public QDialog {
public:
    explicit PreferencesDialog(Preferences current = Preferences::defaults(),
                               QWidget* parent = nullptr);
    ~PreferencesDialog() override;

    PreferencesDialog(const PreferencesDialog&) = delete;
    PreferencesDialog& operator=(const PreferencesDialog&) = delete;

    // The accepted preferences. Before a successful Save it is the value the
    // dialog was opened with.
    [[nodiscard]] Preferences result() const;

    // Puts every control back to Preferences::defaults() (same as the
    // "Restore defaults" button). Nothing is saved until Save.
    void restoreDefaults();

    // Validates and accepts, as the Save button does. Returns false and shows
    // an inline message when a field is invalid.
    bool submit();

    // Current inline validation message (empty when none).
    [[nodiscard]] QString errorText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
