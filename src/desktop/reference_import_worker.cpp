#include "reference_import.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QImageReader>
#include <QPdfDocument>
#include <QtEndian>
#include <algorithm>
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
    QBuffer buffer(&input);
    if (!buffer.open(QIODevice::ReadOnly)) return 3;
    QImage image;
    int pages = 1;
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
    uchar header[24] = {'P', 'S', 'I', 'R', '0', '0', '0', '1'};
    qToLittleEndian<quint32>(static_cast<quint32>(image.width()), header + 8);
    qToLittleEndian<quint32>(static_cast<quint32>(image.height()), header + 12);
    qToLittleEndian<quint32>(static_cast<quint32>(pages), header + 16);
    qToLittleEndian<quint32>(static_cast<quint32>(page), header + 20);
    if (std::fwrite(header, 1, sizeof(header), stdout) != sizeof(header)) return 5;
    const auto row_bytes = static_cast<std::size_t>(image.width()) * 4;
    for (int row = 0; row < image.height(); ++row)
        if (std::fwrite(image.constScanLine(row), 1, row_bytes, stdout) != row_bytes) return 5;
    return std::fflush(stdout) == 0 ? 0 : 5;
}
