#pragma once

#include <QtGlobal>
#include <QByteArray>

namespace sketch::desktop {

// Run before QApplication. Qt 6.8's Windows DirectWrite backend represents
// bundled application fonts without a file identity, so its PDF engine outlines
// their text instead of embedding searchable glyphs. The native Windows plugin's
// FreeType backend preserves the bundled font and vector PDF text. Explicit
// platform overrides remain available to controlled development hosts.
inline void configure_application_platform() {
#ifdef Q_OS_WIN
    if (qgetenv("QT_QPA_PLATFORM").isEmpty())
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows:fontengine=freetype"));
#endif
}

} // namespace sketch::desktop
