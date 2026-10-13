#pragma once

// Vertex design system tokens for the Qt desktop application.
//
// Source of truth: docs/design/vertex-design-system/tokens.json (the Vertex
// design system). Every value here is copied exactly from that file; change
// the design system first, then mirror it here. Dark is the primary theme.
// High contrast is an accessibility theme outside the design system and keeps
// its own values in MainWindow::applyTheme.

#include <QColor>
#include <QString>

namespace sketch::desktop::design {

struct ThemeColors {
    // Surfaces, back to front.
    QColor canvas, surface_0, surface_1, surface_2, surface_3;
    QColor border, border_control;
    // Text.
    QColor ink, ink_muted, ink_faint;
    // The one chrome accent.
    QColor accent, on_accent, accent_text, accent_subtle;
    // Status: always paired with an icon or a word.
    QColor success, warning, danger;
    // Drawing.
    QColor draw_wall, draw_wall_interior, draw_dimension, draw_guide, draw_snap, draw_selection;
    QColor grid_major, grid_minor;
    // Translucent area fills by appraisal area type.
    QColor area_gla, area_basement, area_garage, area_porch, area_other;
};

inline const ThemeColors& dark_theme() {
    static const ThemeColors colors{
        QColor(0x14, 0x16, 0x19), QColor(0x1a, 0x1c, 0x20), QColor(0x21, 0x23, 0x28),
        QColor(0x29, 0x2c, 0x32), QColor(0x32, 0x36, 0x3d),
        QColor(0x2e, 0x31, 0x37), QColor(0x76, 0x7d, 0x88),
        QColor(0xe7, 0xe9, 0xec), QColor(0xa6, 0xab, 0xb3), QColor(0x90, 0x97, 0xa1),
        QColor(0x5b, 0x9b, 0xff), QColor(0x0a, 0x12, 0x20), QColor(0x7c, 0xb0, 0xff),
        QColor(91, 155, 255, 41),
        QColor(0x3e, 0xc5, 0xd6), QColor(0xf5, 0x9e, 0x3b), QColor(0xff, 0x6b, 0x6b),
        QColor(0xe7, 0xe9, 0xec), QColor(0x9a, 0xa0, 0xa8), QColor(0xe8, 0xc3, 0x5a),
        QColor(0xe8, 0x79, 0xc9), QColor(0x4a, 0xde, 0x80), QColor(0x5b, 0x9b, 0xff),
        QColor(0x25, 0x28, 0x2d), QColor(0x1c, 0x1e, 0x22),
        QColor(91, 155, 255, 51), QColor(62, 197, 214, 46), QColor(166, 171, 179, 51),
        QColor(232, 195, 90, 51), QColor(232, 121, 201, 41)};
    return colors;
}

inline const ThemeColors& light_theme() {
    static const ThemeColors colors{
        QColor(0xfb, 0xfb, 0xfc), QColor(0xe9, 0xeb, 0xee), QColor(0xf4, 0xf5, 0xf7),
        QColor(0xff, 0xff, 0xff), QColor(0xdf, 0xe2, 0xe6),
        QColor(0xd6, 0xd9, 0xde), QColor(0x7a, 0x81, 0x8c),
        QColor(0x16, 0x18, 0x1b), QColor(0x4b, 0x51, 0x5b), QColor(0x5f, 0x65, 0x70),
        QColor(0x25, 0x63, 0xd9), QColor(0xff, 0xff, 0xff), QColor(0x1f, 0x56, 0xc2),
        QColor(37, 99, 217, 26),
        QColor(0x0b, 0x6f, 0x7c), QColor(0x9c, 0x46, 0x04), QColor(0xc0, 0x26, 0x2d),
        QColor(0x16, 0x18, 0x1b), QColor(0x5a, 0x60, 0x6a), QColor(0x8a, 0x5a, 0x00),
        QColor(0xb0, 0x30, 0x7f), QColor(0x15, 0x80, 0x3d), QColor(0x25, 0x63, 0xd9),
        QColor(0xe2, 0xe5, 0xe9), QColor(0xf0, 0xf1, 0xf3),
        QColor(37, 99, 217, 33), QColor(11, 111, 124, 33), QColor(75, 81, 91, 36),
        QColor(184, 134, 11, 41), QColor(176, 48, 127, 28)};
    return colors;
}

// Canvas and output painting choose by the actual surface: a light surface
// (light theme, paper output) uses the light drawing colors.
inline const ThemeColors& for_surface(bool light_surface) {
    return light_surface ? light_theme() : dark_theme();
}

// CSS-style color text for Qt style sheets, keeping alpha.
inline QString css(const QColor& color) {
    return color.alpha() == 255 ? color.name(QColor::HexRgb)
        : QStringLiteral("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green())
              .arg(color.blue()).arg(QString::number(color.alphaF(), 'f', 3));
}

// Spacing (4px grid), radius, fixed sizes and strokes, in device-independent px.
inline constexpr int space_1 = 4, space_2 = 8, space_3 = 12, space_4 = 16, space_5 = 24, space_6 = 32;
inline constexpr int radius_sm = 4, radius_md = 6, radius_lg = 10;
inline constexpr int control_sm = 24, control_md = 28, tool_button = 32, toolbar = 40,
                     status_bar = 24, icon = 16;
inline constexpr double stroke_hairline = 1.0, stroke_wall = 2.0, stroke_selection = 2.0;

// Type: IBM Plex is preferred; the design system's fallbacks apply when the
// faces are not installed or bundled.
inline const QString& font_sans() {
    static const QString value = QStringLiteral("\"IBM Plex Sans\", \"Segoe UI\"");
    return value;
}
inline const QString& font_mono() {
    static const QString value = QStringLiteral("\"IBM Plex Mono\", \"Cascadia Mono\", Consolas");
    return value;
}

} // namespace sketch::desktop::design
