#pragma once

#include "sketch/windows_import_worker.hpp"
#include <QByteArray>
#include <QImage>
#include <QString>
#include <functional>

namespace sketch::desktop {

inline constexpr qsizetype referenceInputLimit = 64 * 1024 * 1024;
inline constexpr int referenceDimensionLimit = 4096;
inline constexpr std::uint32_t referencePageLimit = 100000;
inline constexpr qsizetype referenceHeaderSize = 24;

struct DecodedReference {
    QByteArray source;
    QString mime;
    QImage image;
    int page_count{};
};

// Test seam for the broker boundary. Desktop entry points always use the
// production overload below, which resolves only the bundled executable.
using ReferenceBroker = std::function<WindowsImportWorkerReport(const WindowsImportWorkerOptions&)>;
[[nodiscard]] DecodedReference decodeReferenceBytes(
    const QByteArray& source, const QString& suffix, int page_index,
    WindowsImportWorkerOptions options, const ReferenceBroker& broker);
[[nodiscard]] DecodedReference decodeReferenceFile(const QString& path, int page_index = 0);

} // namespace sketch::desktop
