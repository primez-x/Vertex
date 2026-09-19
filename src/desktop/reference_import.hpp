#pragma once

#include "sketch/windows_import_worker.hpp"
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QRectF>
#include <functional>
#include <vector>

namespace sketch::desktop {

inline constexpr qsizetype referenceInputLimit = 64 * 1024 * 1024;
inline constexpr int referenceDimensionLimit = 4096;
inline constexpr std::uint32_t referencePageLimit = 100000;
inline constexpr qsizetype referenceHeaderSize = 32;
inline constexpr quint32 referenceTextLimit = 16384;
inline constexpr quint32 referenceTextRunLimit = 512;
inline constexpr quint32 referenceTextRunSize = 40;

struct ReferenceTextRun {
    quint32 offset{}; // UTF-8 byte offset in source_text.
    quint32 length{};
    QRectF bounds; // Normalized, untransformed page coordinates.
};

struct DecodedReference {
    QByteArray source;
    QString mime;
    QImage image;
    int page_count{};
    QString source_text;
    std::vector<ReferenceTextRun> text_runs;
};

// Test seam for the broker boundary. Desktop entry points always use the
// production overload below, which resolves only the bundled executable.
using ReferenceBroker = std::function<WindowsImportWorkerReport(const WindowsImportWorkerOptions&)>;
[[nodiscard]] DecodedReference decodeReferenceBytes(
    const QByteArray& source, const QString& suffix, int page_index,
    WindowsImportWorkerOptions options, const ReferenceBroker& broker);
[[nodiscard]] DecodedReference decodeReferenceFile(const QString& path, int page_index = 0);

} // namespace sketch::desktop
