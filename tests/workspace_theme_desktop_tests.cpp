// Runtime theme switching: every header and status glyph stays visible, the
// theme choice is marked and persisted, and themed chrome follows the tokens.
#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/preferences_dialog.hpp"
#include "sketch/visualization/native_model_view.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/vertex_design_tokens.hpp"
#include "../src/desktop/advanced_section.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
namespace design = sketch::desktop::design;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

double luminance(const QColor& color) {
    const auto channel = [](double value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

double contrast(const QColor& a, const QColor& b) {
    const auto x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

const char* theme_name(WorkspaceTheme theme) {
    return theme == WorkspaceTheme::light ? "light" : theme == WorkspaceTheme::dark ? "dark" : "high-contrast";
}

struct GlyphScan {
    int contrasting{};      // icon-area pixels at >= 3:1 against the button fill
    double brightest{};     // lightness of the brightest contrasting pixel
};

// The icon sits at the leading edge of a text-beside-icon button and fills an
// icon-only one. Compare its pixels with the button's own fill at the edge.
GlyphScan scan_glyph(QToolButton* button) {
    // Grab through the window so the actual surface behind a transparent
    // button is composed, exactly as on screen.
    auto* window = button->window();
    const auto image = window->grab(QRect(button->mapTo(window, QPoint()), button->size())).toImage();
    const auto fill = image.pixelColor(3, image.height() / 2);
    const int right = button->toolButtonStyle() == Qt::ToolButtonIconOnly
        ? image.width() : std::min(image.width(), 8 + 2 + button->iconSize().width() + 2);
    GlyphScan result;
    for (int y = 2; y < image.height() - 2; ++y)
        for (int x = 4; x < right; ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (contrast(pixel, fill) < 3.0) continue;
            ++result.contrasting;
            result.brightest = std::max(result.brightest, static_cast<double>(pixel.lightnessF()));
        }
    return result;
}

std::vector<QToolButton*> glyph_buttons(desktop::MainWindow& window) {
    std::vector<QToolButton*> result;
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("primaryToolbar"));
    require(toolbar != nullptr, "the header toolbar exists");
    for (auto* button : toolbar->findChildren<QToolButton*>())
        if (button->isVisible() && !button->icon().isNull()) result.push_back(button);
    for (const auto* name : {"fitViewTool", "gridTool", "snapTool", "overviewMapTool"})
        if (auto* button = window.findChild<QToolButton*>(QString::fromLatin1(name)); button && button->isVisible())
            result.push_back(button);
    require(result.size() >= 12, "the header and status bar expose their glyph buttons");
    return result;
}

QString label_for(const QToolButton* button) {
    return button->text().isEmpty() ? button->objectName() : button->text();
}

void test_runtime_theme_switching(const QString& captures) {
    desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1480, 900);
    window.show();
    QApplication::processEvents();
    const auto buttons = glyph_buttons(window);
    auto* redo = [&]() -> QToolButton* {
        for (auto* button : buttons) if (button->text() == QStringLiteral("Redo")) return button;
        return nullptr;
    }();
    require(redo && !redo->isEnabled(), "a new project has nothing to redo");
    require(window.focusWidget() && window.focusWidget()->objectName() == QStringLiteral("measurementPlanCanvas"),
            "the plan canvas, not a header button, has the initial focus");

    // Start dark (the default), then switch at runtime through every pair.
    for (const auto theme : {WorkspaceTheme::light, WorkspaceTheme::high_contrast, WorkspaceTheme::dark,
                             WorkspaceTheme::light, WorkspaceTheme::dark, WorkspaceTheme::high_contrast,
                             WorkspaceTheme::light}) {
        window.setWorkspaceTheme(theme);
        QApplication::processEvents();
        const auto& tokens = theme == WorkspaceTheme::light ? design::light_theme() : design::dark_theme();
        for (auto* button : buttons) {
            const auto scan = scan_glyph(button);
            if (scan.contrasting < 12 && !captures.isEmpty()) {
                (void)window.grab(QRect(button->mapTo(&window, QPoint()), button->size())).save(QDir(captures).filePath(QStringLiteral("blank-glyph-%1-%2.png")
                    .arg(QString::fromLatin1(theme_name(theme)), label_for(button))));
                (void)window.grab().save(QDir(captures).filePath(QStringLiteral("blank-glyph-window-%1.png")
                    .arg(QString::fromLatin1(theme_name(theme)))));
            }
            require(scan.contrasting >= 12,
                    std::string("glyph must stay visible after a runtime switch to ") + theme_name(theme) + ": " +
                        label_for(button).toStdString() + " (" + std::to_string(scan.contrasting) + " pixels)");
        }
        // High contrast: a disabled command is gray, never enabled white.
        if (theme == WorkspaceTheme::high_contrast)
            require(scan_glyph(redo).brightest < 0.8, "disabled Redo must not look enabled in high contrast");

        // The palette is the theme's, not one restored from the previous sheet.
        const auto expected_ink = theme == WorkspaceTheme::high_contrast ? QColor(Qt::white) : tokens.ink;
        require(window.palette().color(QPalette::ButtonText) == expected_ink,
                std::string("window palette must carry the ") + theme_name(theme) + " ink token");

        // Theme menu marks the theme in effect.
        for (const auto& [id, value] : {std::pair{"themeLight", WorkspaceTheme::light},
                                        std::pair{"themeDark", WorkspaceTheme::dark},
                                        std::pair{"themeHighContrast", WorkspaceTheme::high_contrast}}) {
            auto* action = window.findChild<QAction*>(QString::fromLatin1(id));
            require(action && action->isCheckable() && action->isChecked() == (value == theme),
                    std::string("theme menu must check only the current theme: ") + id);
        }

        // Details links use the accent-text token.
        auto* issues = window.findChild<QLabel*>(QStringLiteral("appraisalDetailsIssues"));
        const auto expected_link = theme == WorkspaceTheme::high_contrast ? QColor(255, 255, 0) : tokens.accent_text;
        require(issues && issues->palette().color(QPalette::Link) == expected_link,
                std::string("Details links must use accent-text in ") + theme_name(theme) + ", got " +
                    (issues ? issues->palette().color(QPalette::Link).name().toStdString() : "none"));

        // Words in the GLA headline are set in the sans face.
        auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalDetailsGla"));
        require(gla && gla->text() == QStringLiteral("Totals unavailable") && !gla->property("figure").toBool() &&
                    !gla->font().family().contains(QStringLiteral("Mono")),
                "the withheld GLA headline must use the sans face, got " +
                    (gla ? gla->font().family().toStdString() : std::string("none")));

        // Area class status reads as muted ink on the panel.
        auto* status = window.findChild<QLabel*>(QStringLiteral("areaClassStatus"));
        const auto expected_muted = theme == WorkspaceTheme::high_contrast ? QColor(Qt::white) : tokens.ink_muted;
        require(status && status->palette().color(QPalette::WindowText) == expected_muted,
                std::string("area class status must use ink-muted in ") + theme_name(theme));

        // Library tiles: transparent thumbnail, so the themed tile shows through.
        auto* library = window.findChild<QListWidget*>(QStringLiteral("symbolLibraryItems"));
        require(library && library->count() > 0, "the component library lists symbols");
        const auto thumbnail = library->item(0)->icon().pixmap(QSize(68, 54)).toImage();
        require(!thumbnail.isNull() && thumbnail.pixelColor(0, 0).alpha() == 0,
                "library thumbnails must not paint their own white tile");

        // The 3D view follows the canvas token.
        auto* model = dynamic_cast<visualization::NativeModelView*>(
            window.findChild<QWidget*>(QStringLiteral("nativeModelView")));
        const auto canvas = theme == WorkspaceTheme::high_contrast ? QColor(Qt::black) : tokens.canvas;
        require(model && model->backgroundColor() == canvas, "the 3D view background must follow the canvas token");

        // Advanced: a quiet disclosure, never an accent-filled second button.
        {
            QDialog dialog(&window);
            dialog.setAttribute(Qt::WA_DontShowOnScreen);
            auto* layout = new QVBoxLayout(&dialog);
            auto* advanced = new desktop::AdvancedSection(QStringLiteral("workspaceThemeTest"), &dialog);
            advanced->form()->addRow(QStringLiteral("Offset"), new QComboBox(advanced->body()));
            layout->addWidget(advanced);
            advanced->setExpanded(true);
            dialog.resize(320, 120);
            dialog.show();
            QApplication::processEvents();
            auto* toggle = dialog.findChild<QToolButton*>(QStringLiteral("advancedSectionToggle"));
            const auto image = dialog.grab(QRect(toggle->mapTo(&dialog, QPoint()), toggle->size())).toImage();
            const auto fill = image.pixelColor(image.width() - 3, image.height() / 2);
            const auto accent = theme == WorkspaceTheme::high_contrast ? QColor(255, 255, 0) : tokens.accent;
            require(toggle->isChecked() && contrast(fill, accent) > 1.5,
                    std::string("expanded Advanced toggle must not be accent-filled in ") + theme_name(theme));
            if (!captures.isEmpty())
                (void)dialog.grab().save(QDir(captures).filePath(
                    QStringLiteral("theme-advanced-%1.png").arg(QString::fromLatin1(theme_name(theme)))));
        }

        if (!captures.isEmpty()) {
            require(window.grab().save(QDir(captures).filePath(
                        QStringLiteral("theme-runtime-%1.png").arg(QString::fromLatin1(theme_name(theme))))),
                    "save the runtime theme capture");
            auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("sidebarTabs"));
            auto* navigator = window.findChild<QWidget*>(QStringLiteral("navigatorPanel"));
            if (tabs && navigator) {
                const auto previous = tabs->currentIndex();
                for (int index = 1; index < tabs->count(); ++index) {
                    tabs->setCurrentIndex(index);
                    if (index == 1 && library->count() > 1) library->setCurrentRow(1);
                    QApplication::processEvents();
                    (void)navigator->grab().save(QDir(captures).filePath(QStringLiteral("theme-sidebar-%1-%2.png")
                        .arg(QString::fromLatin1(theme_name(theme))).arg(index)));
                }
                tabs->setCurrentIndex(previous);
            }
        }
    }

    // Header buttons are reachable by Tab and show a 2px accent focus ring.
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("primaryToolbar"));
    for (auto* button : toolbar->findChildren<QToolButton*>())
        if (button->isVisible() && button->objectName() != QStringLiteral("qt_toolbar_ext_button"))
            require(button->focusPolicy() & Qt::TabFocus,
                    "header button must accept Tab focus: " + label_for(button).toStdString());
    window.setWorkspaceTheme(WorkspaceTheme::dark);
    QApplication::processEvents();
    window.activateWindow();
    auto* first = buttons.front();
    first->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    if (first->hasFocus()) {
        const auto image = window.grab(QRect(first->mapTo(&window, QPoint()), first->size())).toImage();
        const auto edge = image.pixelColor(0, image.height() / 2);
        require(contrast(edge, design::dark_theme().surface_0) >= 3.0,
                "focused header button must show a visible focus ring, got " + edge.name().toStdString());
        if (!captures.isEmpty()) image.save(QDir(captures).filePath(QStringLiteral("theme-focus-ring.png")));
    }
    // Tabs: the focused tab bar outlines its current tab in the accent.
    if (auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("sidebarTabs"))) {
        auto* bar = tabs->tabBar();
        bar->setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        if (bar->hasFocus()) {
            const auto rect = bar->tabRect(bar->currentIndex());
            const auto image = window.grab(QRect(bar->mapTo(&window, rect.topLeft()), rect.size())).toImage();
            const auto accent = design::dark_theme().accent;
            int ring = 0;
            for (int y = 0; y < image.height(); ++y)
                for (const int x : {0, 1, image.width() - 2, image.width() - 1})
                    if (contrast(image.pixelColor(x, y), accent) < 1.2) ++ring;
            require(ring >= image.height(), "the focused tab must show a visible accent focus ring");
            if (!captures.isEmpty()) image.save(QDir(captures).filePath(QStringLiteral("theme-tab-focus.png")));
        }
    }
}

void test_theme_choice_persists() {
    desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QApplication::processEvents();
    auto* light = window.findChild<QAction*>(QStringLiteral("themeLight"));
    require(light != nullptr, "theme menu offers Light");
    light->trigger();
    QApplication::processEvents();
    QSettings settings;
    require(desktop::Preferences::load(settings).theme == desktop::Preferences::Theme::light,
            "the header theme choice must be saved as the theme preference");
    require(window.palette().color(QPalette::Window) == design::light_theme().surface_0,
            "choosing Light applies it");
    // A new window starts in the saved theme.
    desktop::MainWindow next;
    require(next.findChild<QAction*>(QStringLiteral("themeLight"))->isChecked(),
            "a new session starts with the saved header theme");
}

void test_dialog_verb_is_default() {
    desktop::MainWindow window; // installs the application chrome filter
    QDialog dialog(&window);
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    auto* layout = new QVBoxLayout(&dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    dialog.show();
    QApplication::processEvents();
    require(buttons->button(QDialogButtonBox::Apply)->isDefault() &&
                !buttons->button(QDialogButtonBox::Cancel)->isDefault(),
            "Enter must apply, never cancel, in an Apply|Cancel dialog");
    dialog.close();
}

// On the native Windows platform, the DWM title bar follows the theme.
void test_native_title_bar() {
#ifdef _WIN32
    if (QGuiApplication::platformName() != QStringLiteral("windows")) return;
    using GetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    const auto module = LoadLibraryW(L"dwmapi.dll");
    const auto get = module ? reinterpret_cast<GetWindowAttribute>(
        reinterpret_cast<void*>(GetProcAddress(module, "DwmGetWindowAttribute"))) : nullptr;
    require(get != nullptr, "DWM is available");
    desktop::MainWindow window;
    window.showMinimized();
    QApplication::processEvents();
    const auto dark_frame = [&](QWidget& widget) {
        BOOL value = FALSE;
        require(SUCCEEDED(get(reinterpret_cast<HWND>(widget.winId()), 20, &value, sizeof(value))),
                "DWM reports the immersive dark mode attribute");
        return value != FALSE;
    };
    require(dark_frame(window), "the dark theme asks DWM for a dark title bar");
    QDialog dialog(&window);
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.show();
    QApplication::processEvents();
    require(dark_frame(dialog), "dialogs shown in the dark theme get a dark title bar");
    dialog.close();
    window.setWorkspaceTheme(WorkspaceTheme::light);
    QApplication::processEvents();
    require(!dark_frame(window), "the light theme restores the light title bar");
    window.setWorkspaceTheme(WorkspaceTheme::high_contrast);
    QApplication::processEvents();
    require(dark_frame(window), "high contrast uses the dark title bar");
    std::cout << "Native title bar follows the theme" << std::endl;
#endif
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir settings_directory;
    QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WorkspaceThemeDesktop"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory.path());
    try {
        require(settings_directory.isValid(), "temporary settings directory");
        const auto font = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font >= 0, "bundled Inter font required");
        app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            test_native_title_bar();
            return 0;
        }
        const auto captures = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!captures.isEmpty()) require(QDir().mkpath(captures), "capture directory");
        test_runtime_theme_switching(captures);
        test_dialog_verb_is_default();
        test_theme_choice_persists();
        std::cout << "Workspace theme desktop tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_theme_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
