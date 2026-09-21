#include "reference_import.hpp"
#include "sketch/project_import_worker.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/ifc_project_exchange.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QImageReader>
#include <QPdfDocument>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace {

#ifdef _WIN32
struct ComScope {
    const HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ComScope() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
    [[nodiscard]] bool usable() const noexcept {
        // RPC_E_CHANGED_MODE means the thread was already initialized by the
        // host. WIC remains callable, but this scope must not uninitialize it.
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
};

QImage decode_tiff_wic(const QByteArray& input) {
    if (input.isEmpty() || static_cast<quint64>(input.size()) > (std::numeric_limits<DWORD>::max)())
        return {};
    ComScope com;
    if (!com.usable()) return {};

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return {};
    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(input.constData())),
            static_cast<DWORD>(input.size()))))
        return {};
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnLoad, &decoder)))
        return {};
    UINT frame_count = 0;
    if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count == 0) return {};
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return {};
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > static_cast<UINT>(sketch::desktop::referenceDimensionLimit) ||
        height > static_cast<UINT>(sketch::desktop::referenceDimensionLimit))
        return {};
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return {};
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    const auto stride = width * 4U;
    if (height > (std::numeric_limits<UINT>::max)() / stride) return {};
    if (FAILED(converter->CopyPixels(nullptr, stride, stride * height,
                                     image.bits())))
        return {};
    return image;
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setLibraryPaths({QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../plugins")});
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    bool valid_page = false;
    const auto page = args[2].toInt(&valid_page);
    if (!valid_page || page < 0 || page >= static_cast<int>(sketch::desktop::referencePageLimit)) return 2;
    QByteArray input;
    char chunk[65536];
    for (;;) {
        const auto count = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (input.size() + static_cast<qsizetype>(count) > sketch::desktop::referenceInputLimit) return 3;
        input.append(chunk, static_cast<qsizetype>(count));
        if (count < sizeof(chunk)) {
            if (std::ferror(stdin)) return 3;
            break;
        }
    }
    if (input.isEmpty()) return 3;
    if (args[1] == "dxf" || args[1] == "ifc") {
        if (page != 0) return 2;
        try {
            sketch::ProjectImportCandidate candidate;
            const std::string_view bytes(input.constData(), static_cast<std::size_t>(input.size()));
            const auto copy_result = [&](auto result) {
                candidate.entities = std::move(result.entities);
                candidate.source_retention_required = result.source_retention_required;
                for (auto& diagnostic : result.diagnostics)
                    candidate.diagnostics.push_back({std::move(diagnostic.source_id),
                        std::move(diagnostic.source_kind), std::move(diagnostic.code)});
            };
            if (args[1] == "dxf") {
                candidate.kind = sketch::ProjectImportKind::dxf;
                copy_result(sketch::import_project_dxf(bytes));
            } else {
                candidate.kind = sketch::ProjectImportKind::ifc;
                copy_result(sketch::import_project_ifc(bytes));
            }
            // Serialize fully before writing: malformed input and candidate
            // validation failures never publish a partial result.
            const auto output = sketch::encode_project_import_candidate(candidate);
            if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size()) return 5;
            return std::fflush(stdout) == 0 ? 0 : 5;
        } catch (...) {
            return 4;
        }
    }
    QBuffer buffer(&input);
    if (!buffer.open(QIODevice::ReadOnly)) return 3;
    QImage image;
    int pages = 1;
    QByteArray text;
    std::vector<sketch::desktop::ReferenceTextRun> text_runs;
    if (args[1] == "pdf") {
        QPdfDocument pdf;
        pdf.load(&buffer);
        pages = pdf.pageCount();
        if (pdf.status() != QPdfDocument::Status::Ready || pages < 1 ||
            pages > static_cast<int>(sketch::desktop::referencePageLimit) || page >= pages) return 4;
        const auto points = pdf.pagePointSize(page);
        const auto pixels = [](double value) {
            // Clamp before integer conversion, including huge finite PDF boxes.
            return static_cast<int>(std::lround(std::clamp(value * 2.0, 256.0, 4096.0)));
        };
        if (!std::isfinite(points.width()) || !std::isfinite(points.height()) ||
            points.width() <= 0 || points.height() <= 0) return 4;
        image = pdf.render(page, QSize(pixels(points.width()), pixels(points.height())));
        // Page parsing stays inside the worker's memory/time limits; enforce
        // the much smaller transport limit before constructing selection runs.
        // Qt/PDFium indices are character indices, not UTF-8 byte offsets.
        const auto selection = pdf.getAllText(page);
        const auto page_text = selection.text();
        text = page_text.toUtf8();
        if (text.size() > sketch::desktop::referenceTextLimit || text.contains('\0')) return 4;
        for (qsizetype begin = 0; begin < page_text.size();) {
            auto end = begin;
            while (end < page_text.size() && page_text[end] != '\r' && page_text[end] != '\n') ++end;
            if (end > begin && !page_text.mid(begin, end - begin).trimmed().isEmpty()) {
                if (text_runs.size() >= sketch::desktop::referenceTextRunLimit) return 4;
                const auto run = pdf.getSelectionAtIndex(page, static_cast<int>(begin),
                                                         static_cast<int>(end - begin));
                // Do not attach a rectangle if the PDF's selection indexing
                // cannot reproduce the corresponding text exactly.
                if (!run.isValid() || run.text() != page_text.mid(begin, end - begin)) return 4;
                // Qt preserves page rotation in the selection orientation and
                // can therefore return negative width or height. Normalize the
                // rectangle before mapping it into the rotated page extent.
                const auto box = run.boundingRectangle().normalized();
                const QRectF normalized(box.x() / points.width(), box.y() / points.height(),
                                        box.width() / points.width(), box.height() / points.height());
                if (!std::isfinite(normalized.x()) || !std::isfinite(normalized.y()) ||
                    !std::isfinite(normalized.width()) || !std::isfinite(normalized.height()) ||
                    normalized.x() < 0 || normalized.y() < 0 || normalized.width() <= 0 ||
                    normalized.height() <= 0 || normalized.right() > 1 || normalized.bottom() > 1) return 4;
                text_runs.push_back({static_cast<quint32>(page_text.left(begin).toUtf8().size()),
                    static_cast<quint32>(run.text().toUtf8().size()), normalized});
            }
            begin = end + 1;
        }
    } else {
        if (page != 0) return 2;
        auto format = args[1].toLatin1();
        if (format == "jpg") format = "jpeg";
        if (format == "tif" || format == "tiff") {
#ifdef _WIN32
            image = decode_tiff_wic(input);
#else
            return 4;
#endif
        } else {
            if (format != "png" && format != "jpeg" && format != "bmp") return 2;
            QImageReader::setAllocationLimit(128);
            QImageReader reader(&buffer, format);
            reader.setDecideFormatFromContent(false);
            const auto size = reader.size();
            if (!size.isValid() || size.width() > sketch::desktop::referenceDimensionLimit ||
                size.height() > sketch::desktop::referenceDimensionLimit) return 4;
            image = reader.read();
        }
    }
    if (image.isNull() || image.width() > sketch::desktop::referenceDimensionLimit ||
        image.height() > sketch::desktop::referenceDimensionLimit) return 4;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) return 4;
    uchar header[32] = {'P', 'S', 'I', 'R', '0', '0', '0', '2'};
    qToLittleEndian<quint32>(static_cast<quint32>(image.width()), header + 8);
    qToLittleEndian<quint32>(static_cast<quint32>(image.height()), header + 12);
    qToLittleEndian<quint32>(static_cast<quint32>(pages), header + 16);
    qToLittleEndian<quint32>(static_cast<quint32>(page), header + 20);
    qToLittleEndian<quint32>(static_cast<quint32>(text.size()), header + 24);
    qToLittleEndian<quint32>(static_cast<quint32>(text_runs.size()), header + 28);
    if (std::fwrite(header, 1, sizeof(header), stdout) != sizeof(header)) return 5;
    const auto row_bytes = static_cast<std::size_t>(image.width()) * 4;
    for (int row = 0; row < image.height(); ++row)
        if (std::fwrite(image.constScanLine(row), 1, row_bytes, stdout) != row_bytes) return 5;
    if (std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout) !=
        static_cast<std::size_t>(text.size())) return 5;
    for (const auto& run : text_runs) {
        uchar record[40]{};
        qToLittleEndian<quint32>(run.offset, record);
        qToLittleEndian<quint32>(run.length, record + 4);
        const double values[]{run.bounds.x(), run.bounds.y(), run.bounds.width(), run.bounds.height()};
        for (int i = 0; i < 4; ++i)
            qToLittleEndian<quint64>(std::bit_cast<quint64>(values[i]), record + 8 + i * 8);
        if (std::fwrite(record, 1, sizeof(record), stdout) != sizeof(record)) return 5;
    }
    return std::fflush(stdout) == 0 ? 0 : 5;
}
