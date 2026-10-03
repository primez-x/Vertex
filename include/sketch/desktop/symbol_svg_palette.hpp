#pragma once

#include "sketch/annotation_catalog.hpp"

#include <QByteArray>

namespace sketch::desktop {

// Creates a paint-only derivative of supported catalog artwork. The original
// bytes remain the instance's immutable artwork identity. Invalid palettes,
// unsupported paint roles, or unsafe/malformed XML throw std::invalid_argument.
[[nodiscard]] QByteArray colored_symbol_svg(const QByteArray& document,
                                            const SymbolSvgPalette& palette);

} // namespace sketch::desktop
