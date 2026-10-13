#pragma once

#include <QCoreApplication>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

#include <mutex>

namespace sketch::desktop {

// Family names of the bundled IBM Plex faces (assets/fonts, SIL OFL 1.1).
// The faces are compiled into sketch_desktop's `:/fonts` resource.
inline const QString& bundled_sans_family() {
    static const QString value = QStringLiteral("IBM Plex Sans");
    return value;
}
inline const QString& bundled_mono_family() {
    static const QString value = QStringLiteral("IBM Plex Mono");
    return value;
}

// The bundled faces in registration order. The Sans Regular face is the
// output font whose bytes are bound into output fingerprints.
inline const QStringList& bundled_font_resources() {
    static const QStringList value{
        QStringLiteral(":/fonts/IBMPlexSans-Regular.ttf"),
        QStringLiteral(":/fonts/IBMPlexSans-Medium.ttf"),
        QStringLiteral(":/fonts/IBMPlexSans-SemiBold.ttf"),
        QStringLiteral(":/fonts/IBMPlexMono-Regular.ttf"),
        QStringLiteral(":/fonts/IBMPlexMono-Medium.ttf"),
    };
    return value;
}

// Registers the bundled IBM Plex faces with the application font database
// once per process. Returns true when every face was registered and the Sans
// family is available. Calls made before a Qt application object exists do
// nothing and return false, so a later call can still register the faces.
inline bool ensure_bundled_fonts() {
    static std::mutex mutex;
    static bool attempted = false;
    static bool loaded = false;
    const std::lock_guard lock(mutex);
    if (attempted) return loaded;
    if (QCoreApplication::instance() == nullptr) return false;
    attempted = true;
    bool all_faces = true;
    bool sans = false;
    for (const auto& resource : bundled_font_resources()) {
        const auto font_id = QFontDatabase::addApplicationFont(resource);
        if (font_id < 0) {
            all_faces = false;
            continue;
        }
        if (QFontDatabase::applicationFontFamilies(font_id).contains(bundled_sans_family())) {
            sans = true;
        }
    }
    loaded = all_faces && sans;
    return loaded;
}

} // namespace sketch::desktop
