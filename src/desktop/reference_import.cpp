#include "reference_import.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtEndian>
#include <cstring>
#include <stdexcept>

namespace sketch::desktop {
namespace {
QString mimeFor(const QString& suffix) {
    if (suffix == "pdf") return QStringLiteral("application/pdf");
    if (suffix == "png") return QStringLiteral("image/png");
    if (suffix == "jpg" || suffix == "jpeg") return QStringLiteral("image/jpeg");
    if (suffix == "bmp") return QStringLiteral("image/bmp");
    throw std::invalid_argument("Reference image extension must be PDF, PNG, JPEG, or BMP.");
}
}

DecodedReference decodeReferenceBytes(const QByteArray& source, const QString& suffix,
                                      int page_index, WindowsImportWorkerOptions options,
                                      const ReferenceBroker& broker) {
    const auto mime = mimeFor(suffix);
    if (source.isEmpty() || source.size() > referenceInputLimit)
        throw std::invalid_argument("Reference files must contain between 1 byte and 64 MiB.");
    if (page_index < 0 || page_index >= static_cast<int>(referencePageLimit) ||
        (suffix != "pdf" && page_index != 0))
        throw std::invalid_argument("The selected reference page does not exist.");
    options.arguments = {suffix.toStdWString(), std::to_wstring(page_index)};
    options.input.resize(static_cast<std::size_t>(source.size()));
    std::memcpy(options.input.data(), source.constData(), options.input.size());
    options.max_output_bytes = referenceHeaderSize + 4ULL * referenceDimensionLimit * referenceDimensionLimit;
    options.timeout_ms = 30'000;
    options.memory_bytes = 512ULL * 1024 * 1024;
    options.max_active_processes = 1;
    options.proj_offline_required = true;
    const auto report = broker(options);
    if (!report.controls_attested()) {
        QStringList codes;
        for (const auto& code : report.diagnostics) codes.push_back(QString::fromStdString(code));
        throw std::runtime_error(QStringLiteral(
            "Isolated reference import is unavailable (%1). Install or repair the bundled "
            "property-studio-import-worker and its runtime in a read-only application directory; "
            "the Windows sandbox must be available. No source file was decoded in the desktop.")
            .arg(codes.isEmpty() ? QStringLiteral("worker_not_attested") : codes.join(',')).toStdString());
    }
    const auto& output = report.output;
    if (output.size() < referenceHeaderSize || std::memcmp(output.data(), "PSIR0001", 8) != 0)
        throw std::runtime_error("The isolated import worker returned an invalid response.");
    const auto* data = reinterpret_cast<const uchar*>(output.data());
    const auto width = qFromLittleEndian<quint32>(data + 8);
    const auto height = qFromLittleEndian<quint32>(data + 12);
    const auto pages = qFromLittleEndian<quint32>(data + 16);
    const auto page = qFromLittleEndian<quint32>(data + 20);
    if (!width || !height || width > referenceDimensionLimit || height > referenceDimensionLimit ||
        !pages || pages > referencePageLimit || page != static_cast<quint32>(page_index) || page >= pages ||
        (suffix != "pdf" && pages != 1) ||
        output.size() != referenceHeaderSize + 4ULL * width * height)
        throw std::runtime_error("The isolated import worker returned invalid image bounds.");
    // No encoded image parser runs here. Validate the exact raw pixel extent
    // before copying out of the broker-owned reply.
    QImage image(data + referenceHeaderSize, static_cast<int>(width), static_cast<int>(height),
                 static_cast<int>(width * 4), QImage::Format_RGBA8888);
    image = image.copy();
    if (image.isNull()) throw std::runtime_error("The reference preview could not be allocated.");
    return {source, mime, std::move(image), static_cast<int>(pages)};
}

DecodedReference decodeReferenceFile(const QString& path, int page_index) {
    const auto suffix = QFileInfo(path).suffix().trimmed().toLower();
    (void)mimeFor(suffix);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::invalid_argument("The reference file could not be opened.");
    if (file.size() <= 0 || file.size() > referenceInputLimit)
        throw std::invalid_argument("Reference files must contain between 1 byte and 64 MiB.");
    const auto bytes = file.read(referenceInputLimit + 1);
    if (file.error() != QFileDevice::NoError || !file.atEnd())
        throw std::invalid_argument("The reference file could not be read within the import limit.");
    WindowsImportWorkerOptions options;
    const auto root = std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString());
    options.executable = root / "property-studio-import-worker.exe";
    options.immutable_module_roots = {root, root.parent_path() / "plugins"};
    options.temporary_root = std::filesystem::path(QDir::tempPath().toStdWString());
    return decodeReferenceBytes(bytes, suffix, page_index, std::move(options), run_windows_import_worker);
}
} // namespace sketch::desktop
