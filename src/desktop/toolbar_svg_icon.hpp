#pragma once

#include <QByteArray>
#include <QIcon>

class QWidget;

namespace sketch::desktop {

// Glyphs are source-owned inline SVG elements in a 24 by 24 view box.
// Color and raster size are resolved at paint time from the current palette
// and requested device pixel ratio; no fixed-color bitmap is retained.
[[nodiscard]] QIcon toolbar_svg_icon(const QByteArray& glyphs, QWidget* palette_owner = nullptr);

} // namespace sketch::desktop
