#include "reference_import.hpp"
#include "assistance_ocr.hpp"
#include "assistance_ocr_recognizer.hpp"
#include "pinc_import_worker.hpp"
#include "cad_library_bridge.hpp"
#include "sketch/project_import_worker.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/dxf_phase_asset_carrier.hpp"
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
#include <cstring>
#include <limits>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace {

void append_library_diagnostics(sketch::ProjectImportCandidate& candidate,
                                const nlohmann::json& result) {
    const auto& diagnostics = result.at("diagnostics");
    if (!diagnostics.is_array() || diagnostics.size() > sketch::project_import_diagnostic_limit)
        throw std::invalid_argument("Invalid CAD adapter diagnostics");
    for (const auto& item : diagnostics) {
        sketch::project_import_detail::fields(item, {"source_id", "source_kind", "code"});
        if (candidate.kind == sketch::ProjectImportKind::ifc && item.at("code") == "ifc_geometry_unavailable") {
            auto source = sketch::project_import_detail::text(item.at("source_id"));
            if (!source.empty() && source.front() == '#') source.erase(0, 1);
            const bool unreliable = std::any_of(candidate.diagnostics.begin(), candidate.diagnostics.end(),
                [&](const auto& diagnostic) {
                    return diagnostic.source_id == "#" + source &&
                        (diagnostic.code == "geometry_semantics_not_reconstructed" ||
                         diagnostic.code == "placement_rotation_unsupported" ||
                         diagnostic.code == "product_geometry_missing");
                });
            if (!unreliable && std::any_of(candidate.entities.begin(), candidate.entities.end(), [&](const auto& entity) {
                return entity.id == "ifc-" + source && entity.type == "boundary";
            })) continue; // A native 2D outline does not require a tessellated body.
        }
        candidate.diagnostics.push_back({sketch::project_import_detail::text(item.at("source_id")),
            sketch::project_import_detail::text(item.at("source_kind")),
            sketch::project_import_detail::text(item.at("code"), false)});
    }
}

void merge_ifc_sections(sketch::ProjectImportCandidate& candidate, const nlohmann::json& result) {
    if (std::any_of(result.at("diagnostics").begin(), result.at("diagnostics").end(),
        [](const auto& diagnostic) { return diagnostic.at("code") == "ifc_length_units_unresolved"; })) {
        // Neither parser may turn unitless source coordinates into metre-based
        // document geometry. Keep only inert references and the original asset.
        std::erase_if(candidate.entities, [](const auto& entity) { return entity.type != "ifc_reference"; });
        append_library_diagnostics(candidate, result);
        candidate.source_retention_required = true;
        return;
    }
    const auto& products = result.at("boundaries");
    if (!products.is_array() || products.size() > sketch::project_import_entity_limit)
        throw std::invalid_argument("Invalid IFC sections");
    std::set<std::string> seen;
    for (const auto& product : products) {
        const auto source = sketch::project_import_detail::text(product.at("source_id"), false, 64);
        const auto kind = sketch::project_import_detail::text(product.at("source_kind"), false, 128);
        auto record = source;
        if (record.front() == '#') record.erase(0, 1);
        if (record.empty() || record.find_first_not_of("0123456789") != std::string::npos ||
            !seen.insert(record).second) throw std::invalid_argument("Invalid IFC section identity");
        const auto id = "ifc-" + record;
        auto existing = std::find_if(candidate.entities.begin(), candidate.entities.end(),
            [&](const auto& entity) { return entity.id == id; });
        // Keep native architectural entities intact. Foreign typed products
        // may also need measurement outlines (including holes), so publish
        // those under separate identities rather than silently discarding them.
        const bool preserve_existing = existing != candidate.entities.end() && existing->type != "boundary";
        const auto& loops = product.at("loops");
        const auto& roles = product.at("roles");
        if (!loops.is_array() || loops.empty() || !roles.is_array() || roles.size() != loops.size())
            throw std::invalid_argument("Invalid IFC contour topology");
        nlohmann::json source_extensions = existing == candidate.entities.end()
            ? nlohmann::json::object() : existing->extensions;
        if (existing != candidate.entities.end() && !preserve_existing) candidate.entities.erase(existing);
        for (std::size_t index = 0; index < loops.size(); ++index) {
            const auto& loop = loops[index];
            const auto role = sketch::project_import_detail::text(roles[index], false, 16);
            if (!loop.is_array() || loop.size() < 3 ||
                loop.size() > sketch::project_import_boundary_segment_limit ||
                (role != "outer" && role != "hole"))
                throw std::invalid_argument("Invalid IFC contour");
            auto segments = nlohmann::json::array();
            for (std::size_t point = 0; point < loop.size(); ++point) {
                (void)sketch::project_import_detail::point(loop[point]);
                segments.push_back({{"start", loop[point]}, {"end", loop[(point + 1) % loop.size()]},
                                    {"sweep_radians", 0.0}});
            }
            sketch::Entity entity;
            entity.id = index == 0 && !preserve_existing ? id : id + "-section-" + std::to_string(index);
            entity.type = "boundary";
            entity.properties = {{"boundary", std::move(segments)}, {"closed", true},
                {"classification", "ifc_section_" + role}, {"ifc_type", kind}};
            entity.extensions = source_extensions;
            entity.extensions["ifc_library_section"] = {{"source_id", source}, {"source_kind", kind},
                {"group", id}, {"role", role}, {"approximation", product.at("approximation")}};
            candidate.entities.push_back(std::move(entity));
        }
    }
    append_library_diagnostics(candidate, result);
    candidate.source_retention_required = true;
}

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

QImage decode_tiff_wic(const QByteArray& input,
    std::uint64_t max_pixels = 4096ULL * 4096) {
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
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatTiff)
        return {};
    UINT frame_count = 0;
    if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count == 0) return {};
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return {};
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > static_cast<UINT>(sketch::desktop::referenceDimensionLimit) ||
        height > static_cast<UINT>(sketch::desktop::referenceDimensionLimit) ||
        static_cast<std::uint64_t>(width) * height > max_pixels)
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

QImage decode_raster_image(const QByteArray& input, QByteArray format,
    std::uint64_t max_pixels = 4096ULL * 4096) {
    if (format == "jpg") format = "jpeg";
    if (format == "tif" || format == "tiff") {
#ifdef _WIN32
        return decode_tiff_wic(input, max_pixels);
#else
        return {};
#endif
    }
    if (format != "png" && format != "jpeg" && format != "bmp") return {};
    QBuffer buffer;
    buffer.setData(input);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader::setAllocationLimit(128);
    QImageReader reader(&buffer, format);
    reader.setDecideFormatFromContent(false);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > sketch::desktop::referenceDimensionLimit ||
        size.height() > sketch::desktop::referenceDimensionLimit ||
        static_cast<std::uint64_t>(size.width()) * size.height() > max_pixels) return {};
    return reader.read();
}

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
    const bool native_dxf_assets = args[1] == "dxf-assets";
    if (native_dxf_assets && args[2] != "0") return 2;
    bool valid_page = false;
    const auto page = args[2].toInt(&valid_page);
    if (!valid_page || page < 0 || page >= static_cast<int>(sketch::desktop::referencePageLimit)) return 2;
    const auto input_limit = native_dxf_assets ?
        static_cast<qsizetype>(sketch::windows_import_worker_native_dxf_input_limit) :
        sketch::desktop::referenceInputLimit;
    QByteArray input;
    char chunk[65536];
    for (;;) {
        const auto count = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (input.size() > input_limit || static_cast<qsizetype>(count) > input_limit - input.size()) return 3;
        input.append(chunk, static_cast<qsizetype>(count));
        if (count < sizeof(chunk)) {
            if (std::ferror(stdin)) return 3;
            break;
        }
    }
    if (input.isEmpty()) return 3;
    if (args[1] == "ocr") {
        if (page != 0) return 2;
        try {
            const auto output = sketch::desktop::recognizeAssistanceOcrFrame(std::span(
                reinterpret_cast<const std::byte*>(input.constData()), static_cast<std::size_t>(input.size())),
                sketch::desktop::assistanceOcrApplicationRoot());
            if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size()) return 5;
            return std::fflush(stdout) == 0 ? 0 : 5;
        } catch (const sketch::desktop::AssistanceOcrFailure& error) {
            // The broker admits only this bounded failure grammar as diagnostics;
            // failed output is never a candidate OCR reply.
            std::fprintf(stderr, "VERTEX_OCR_FAILURE_V1:%s:%u\n", error.stage_code(), error.os_error());
            std::fflush(stderr);
            return 4;
        } catch (...) { return 4; }
    }
    if (args[1] == "pinc") {
        if (page != 0) return 2;
        try {
            auto project = sketch::parse_pinc_project(std::span(
                reinterpret_cast<const std::byte*>(input.constData()), static_cast<std::size_t>(input.size())));
            std::vector<sketch::desktop::PincRasterFrame> frames;
            std::uint64_t used_pixels = 0, source_bytes = 0;
            for (std::size_t index = 0; index < project.pages.size(); ++index) {
                const auto& descriptor = project.pages[index].underlay;
                if (!descriptor || !descriptor->supported_raster_descriptor) continue;
                try {
                    const auto source = sketch::desktop::pincUnderlaySourceBytes(*descriptor);
                    const auto count = static_cast<std::uint64_t>(source.size());
                    if (count > sketch::desktop::pincImageSourceLimit - source_bytes)
                        throw std::invalid_argument("Underlay source budget exceeded");
                    source_bytes += count;
                    auto image = decode_raster_image(source, sketch::desktop::pincUnderlaySuffix(*descriptor).toLatin1(),
                        sketch::desktop::pincDecodedPixelLimit - used_pixels);
                    if (image.isNull()) throw std::invalid_argument("Underlay decoding failed");
                    const auto pixels = static_cast<std::uint64_t>(image.width()) * image.height();
                    if (pixels > sketch::desktop::pincDecodedPixelLimit - used_pixels)
                        throw std::invalid_argument("Underlay pixel budget exceeded");
                    image = image.convertToFormat(QImage::Format_RGBA8888);
                    if (image.isNull()) throw std::invalid_argument("Underlay conversion failed");
                    sketch::desktop::PincRasterFrame frame;
                    frame.page_index = index; frame.source = source;
                    frame.pixel_frame.resize(static_cast<std::size_t>(32 + pixels * 4));
                    auto* header = reinterpret_cast<uchar*>(frame.pixel_frame.data());
                    std::memcpy(header, "PSIR0002", 8);
                    qToLittleEndian<quint32>(static_cast<quint32>(image.width()), header + 8);
                    qToLittleEndian<quint32>(static_cast<quint32>(image.height()), header + 12);
                    qToLittleEndian<quint32>(1, header + 16);
                    const auto row_bytes = static_cast<std::size_t>(image.width()) * 4;
                    for (int row = 0; row < image.height(); ++row)
                        std::memcpy(header + 32 + static_cast<std::size_t>(row) * row_bytes,
                                    image.constScanLine(row), row_bytes);
                    used_pixels += pixels;
                    frames.push_back(std::move(frame));
                } catch (const std::exception&) {
                    project.diagnostics.push_back({descriptor->source_pointer, "underlay_decode_failed",
                        "Underlay could not be decoded within supported image budgets; original content is retained."});
                }
            }
            const auto output = sketch::desktop::encodePincWorkerReply(project, frames);
            if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size()) return 5;
            return std::fflush(stdout) == 0 ? 0 : 5;
        } catch (...) { return 4; }
    }
    if (args[1] == "dxf" || native_dxf_assets || args[1] == "ifc") {
        if (page != 0) return 2;
        const char* stage = "core";
        try {
            sketch::ProjectImportCandidate candidate;
            const std::string_view bytes(input.constData(), static_cast<std::size_t>(input.size()));
            const auto copy_result = [&](auto result) {
                candidate.entities = std::move(result.entities);
                candidate.source_retention_required = result.source_retention_required;
                if constexpr (requires { result.physical_source_graphs; })
                    candidate.physical_source_graphs = std::move(result.physical_source_graphs);
                if constexpr (requires { result.catalog_sources; result.authoring_catalog_ids; }) {
                    candidate.catalog_sources = std::move(result.catalog_sources);
                    candidate.authoring_catalog_ids = std::move(result.authoring_catalog_ids);
                }
                if constexpr (requires { result.phase_source_graph; })
                    candidate.phase_source_graph = std::move(result.phase_source_graph);
                if constexpr (requires { result.phase_source_assets; })
                    candidate.phase_source_assets = std::move(result.phase_source_assets);
                for (auto& diagnostic : result.diagnostics)
                    candidate.diagnostics.push_back({std::move(diagnostic.source_id),
                        std::move(diagnostic.source_kind), std::move(diagnostic.code)});
            };
            if (native_dxf_assets) {
                candidate.kind = sketch::ProjectImportKind::dxf;
                // Presence selects only this bounded transport. The core must
                // strictly admit the original carrier and retain its ordinary
                // geometry budgets; normalization cannot repair this input.
                if (!sketch::native_dxf_phase_asset_carrier_present(bytes))
                    throw std::invalid_argument("Native DXF asset carrier required");
                copy_result(sketch::import_project_dxf(bytes));
            } else if (args[1] == "dxf") {
                candidate.kind = sketch::ProjectImportKind::dxf;
                // Verify native metadata against the original representation.
                // A library's repair/normalization must never rehabilitate it.
                if (bytes.find("VERTEX_ENTITY_V1") != std::string_view::npos) {
                    copy_result(sketch::import_project_dxf(bytes));
                } else {
                    const auto runtime = std::filesystem::path(
                        QCoreApplication::applicationDirPath().toStdWString()) / "cad-runtime";
                    stage = "library";
                    const auto normalized = sketch::desktop::call_cad_library(runtime, "normalize_dxf", bytes);
                    const auto text = sketch::project_import_detail::text(
                        normalized.at("normalized_text"), false, 16 * 1024 * 1024);
                    stage = "core";
                    copy_result(sketch::import_project_dxf(text));
                    stage = "merge";
                    append_library_diagnostics(candidate, normalized);
                    candidate.source_retention_required = true;
                }
            } else {
                candidate.kind = sketch::ProjectImportKind::ifc;
                copy_result(sketch::import_project_ifc(bytes));
                const auto runtime = std::filesystem::path(
                    QCoreApplication::applicationDirPath().toStdWString()) / "cad-runtime";
                stage = "merge";
                std::vector<std::uint64_t> admitted;
                for (const auto& entity : candidate.entities) {
                    if (entity.type != "stair" && entity.type != "railing") continue;
                    const auto& proof = entity.extensions.at("ifc_source");
                    const auto& id = proof.at("record_id");
                    if (!id.is_number_integer() || id <= 0 ||
                        proof.at("record_type") != (entity.type == "stair" ? "IFCSTAIR" : "IFCRAILING"))
                        throw std::invalid_argument("Invalid admitted native IFC product identity");
                    admitted.push_back(id.get<std::uint64_t>());
                }
                stage = "library";
                const auto result = sketch::desktop::call_cad_library(runtime, "project_ifc", bytes, admitted);
                stage = "merge";
                merge_ifc_sections(candidate, result);
            }
            // Serialize fully before writing: malformed input and candidate
            // validation failures never publish a partial result.
            stage = "candidate";
            const auto output = sketch::encode_project_import_candidate(candidate);
            if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size()) return 5;
            return std::fflush(stdout) == 0 ? 0 : 5;
        } catch (...) {
            // Only a fixed stage crosses the worker boundary; source content
            // and arbitrary library/exception text must never leave this catch.
            std::fprintf(stderr, "VERTEX_PROJECT_FAILURE_V1:%s\n", stage);
            std::fflush(stderr);
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
        if (format != "png" && format != "jpg" && format != "jpeg" &&
            format != "bmp" && format != "tif" && format != "tiff") return 2;
        image = decode_raster_image(input, format);
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
