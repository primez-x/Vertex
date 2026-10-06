#include "pinc_import_worker.hpp"

#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace sketch::desktop {
namespace {
constexpr std::size_t header_size = 16;
constexpr std::size_t record_size = 12;
[[noreturn]] void invalid() {
    throw std::invalid_argument("Invalid bounded Pinc import worker reply.");
}
std::uint32_t read32(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) invalid();
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.data() + at));
}
void write32(std::vector<std::byte>& bytes, std::size_t at, std::size_t value) {
    if (value > (std::numeric_limits<std::uint32_t>::max)() || at > bytes.size() ||
        bytes.size() - at < 4) invalid();
    qToLittleEndian<quint32>(static_cast<quint32>(value), reinterpret_cast<uchar*>(bytes.data() + at));
}
std::span<const std::byte> take(std::span<const std::byte> input, std::size_t& offset,
                               std::size_t count) {
    if (offset > input.size() || count > input.size() - offset) invalid();
    const auto result = input.subspan(offset, count); offset += count; return result;
}
std::uint64_t pixelCharge(std::span<const std::byte> frame) {
    if (frame.size() < 32 || std::memcmp(frame.data(), "PSIR0002", 8) != 0) invalid();
    const auto width = read32(frame, 8), height = read32(frame, 12);
    if (!width || !height || width > referenceDimensionLimit || height > referenceDimensionLimit ||
        read32(frame, 16) != 1 || read32(frame, 20) != 0 || read32(frame, 24) || read32(frame, 28)) invalid();
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (frame.size() != 32 + pixels * 4) invalid();
    return pixels;
}
const PincImportUnderlay& descriptor(const PincImportProject& project, std::size_t index) {
    if (index >= project.pages.size() || !project.pages[index].underlay ||
        !project.pages[index].underlay->supported_raster_descriptor) invalid();
    return *project.pages[index].underlay;
}
bool reportedDecodeFailure(const PincImportProject& project, const PincImportUnderlay& image) {
    return std::any_of(project.diagnostics.begin(), project.diagnostics.end(), [&](const auto& item) {
        return item.source_pointer == image.source_pointer && item.code == "underlay_decode_failed";
    });
}
void requireCoverage(const PincImportProject& project, const std::set<std::size_t>& seen) {
    for (std::size_t page = 0; page < project.pages.size(); ++page) {
        const auto& image = project.pages[page].underlay;
        if (!image || !image->supported_raster_descriptor) continue;
        if (seen.contains(page) == reportedDecodeFailure(project, *image)) invalid();
    }
}
struct FrameView {
    std::size_t page_index{};
    std::span<const std::byte> source;
    std::span<const std::byte> pixels;
};
void charge(std::uint64_t& total, std::uint64_t add, std::uint64_t limit) {
    if (add > limit || total > limit - add) invalid(); total += add;
}
} // namespace

QString pincUnderlaySuffix(const PincImportUnderlay& image) {
    if (!image.supported_raster_descriptor) invalid();
    if (image.mime_type == "image/png") return QStringLiteral("png");
    if (image.mime_type == "image/jpeg") return QStringLiteral("jpeg");
    if (image.mime_type == "image/bmp") return QStringLiteral("bmp");
    if (image.mime_type == "image/tiff") return QStringLiteral("tiff");
    invalid();
}

QByteArray pincUnderlaySourceBytes(const PincImportUnderlay& image) {
    (void)pincUnderlaySuffix(image);
    if (!image.data_url || image.data_url->size() > PincImportLimits{}.max_string_bytes) invalid();
    const auto prefix = std::string("data:") + image.mime_type + ";base64,";
    if (!image.data_url->starts_with(prefix)) invalid();
    const auto encoded = QByteArray::fromRawData(image.data_url->data() + prefix.size(),
        static_cast<qsizetype>(image.data_url->size() - prefix.size()));
    const auto result = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (result.decodingStatus != QByteArray::Base64DecodingStatus::Ok || result.decoded.isEmpty() ||
        static_cast<std::uint64_t>(result.decoded.size()) > pincImageSourceLimit) invalid();
    return result.decoded;
}

std::vector<std::byte> encodePincWorkerReply(const PincImportProject& project,
                                           const std::vector<PincRasterFrame>& frames) {
    const auto candidate = encode_pinc_import_candidate(project);
    if (frames.size() > project.pages.size()) invalid();
    std::set<std::size_t> seen;
    std::uint64_t source_total = 0, pixels_total = 0, wire_total = header_size + candidate.size();
    for (const auto& frame : frames) {
        const auto& image = descriptor(project, frame.page_index);
        if (!seen.insert(frame.page_index).second || frame.source.isEmpty()) invalid();
        charge(source_total, static_cast<std::uint64_t>(frame.source.size()), pincImageSourceLimit);
        charge(pixels_total, pixelCharge(frame.pixel_frame), pincDecodedPixelLimit);
        charge(wire_total, record_size + static_cast<std::uint64_t>(frame.source.size()) +
            frame.pixel_frame.size(), pincReplyLimit);
        if (pincUnderlaySourceBytes(image) != frame.source) invalid();
    }
    requireCoverage(project, seen);
    std::vector<std::byte> result(static_cast<std::size_t>(wire_total));
    std::memcpy(result.data(), "VPIW0001", 8);
    write32(result, 8, candidate.size()); write32(result, 12, frames.size());
    std::size_t offset = header_size;
    std::memcpy(result.data() + offset, candidate.data(), candidate.size()); offset += candidate.size();
    for (const auto& frame : frames) {
        write32(result, offset, frame.page_index); write32(result, offset + 4, frame.source.size());
        write32(result, offset + 8, frame.pixel_frame.size()); offset += record_size;
        std::memcpy(result.data() + offset, frame.source.constData(), frame.source.size());
        offset += static_cast<std::size_t>(frame.source.size());
        std::memcpy(result.data() + offset, frame.pixel_frame.data(), frame.pixel_frame.size());
        offset += frame.pixel_frame.size();
    }
    return result;
}

PincWorkerProject validatePincWorkerReply(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size || bytes.size() > pincReplyLimit ||
        std::memcmp(bytes.data(), "VPIW0001", 8) != 0) invalid();
    const auto candidate_size = read32(bytes, 8), frame_count = read32(bytes, 12);
    if (!candidate_size || candidate_size > PincImportLimits{}.max_bytes ||
        frame_count > PincImportLimits{}.max_pages) invalid();
    std::size_t offset = header_size;
    PincWorkerProject result;
    result.project = decode_pinc_import_candidate(take(bytes, offset, candidate_size));
    if (frame_count > result.project.pages.size()) invalid();
    std::vector<FrameView> frames; frames.reserve(frame_count);
    std::set<std::size_t> seen;
    std::uint64_t source_total = 0, pixels_total = 0;
    // Check the complete aggregate framing before allocating any image/source.
    for (std::size_t index = 0; index < frame_count; ++index) {
        const auto record = take(bytes, offset, record_size);
        const auto page = read32(record, 0), source_size = read32(record, 4), pixel_size = read32(record, 8);
        (void)descriptor(result.project, page);
        if (!source_size || !seen.insert(page).second) invalid();
        charge(source_total, source_size, pincImageSourceLimit);
        const auto source = take(bytes, offset, source_size), pixels = take(bytes, offset, pixel_size);
        charge(pixels_total, pixelCharge(pixels), pincDecodedPixelLimit);
        frames.push_back({page, source, pixels});
    }
    if (offset != bytes.size()) invalid();
    requireCoverage(result.project, seen);
    for (const auto& frame : frames) {
        const auto& image = descriptor(result.project, frame.page_index);
        const auto source = QByteArray::fromRawData(reinterpret_cast<const char*>(frame.source.data()),
                                                  static_cast<qsizetype>(frame.source.size()));
        if (pincUnderlaySourceBytes(image) != source) invalid();
        result.underlays.push_back({frame.page_index,
            validateReferencePixelFrame(frame.pixels, source, pincUnderlaySuffix(image))});
    }
    return result;
}

PincWorkerProject importPincProjectBytes(std::span<const std::byte> source,
    WindowsImportWorkerOptions options, const ReferenceBroker& broker) {
    if (source.empty() || source.size() > PincImportLimits{}.max_bytes)
        throw std::invalid_argument("Pinc projects must contain between 1 byte and 64 MiB.");
    const auto cancelled = [&options]() noexcept {
        return options.cancellation_requested &&
            options.cancellation_requested->load(std::memory_order_acquire);
    };
    if (cancelled()) throw std::runtime_error("Pinc import cancelled.");
    options.arguments = {L"pinc", L"0"};
    options.input.assign(source.begin(), source.end());
    options.timeout_ms = 30'000;
    options.memory_bytes = 768ULL * 1024 * 1024;
    options.max_active_processes = 1;
    options.max_output_bytes = pincReplyLimit;
    options.proj_offline_required = true;
    const auto report = broker(options);
    if (report.status == WindowsImportWorkerStatus::cancelled || cancelled())
        throw std::runtime_error("Pinc import cancelled.");
    if (!report.controls_attested())
        throw std::runtime_error("Isolated Pinc import is unavailable. Install or repair the bundled "
            "vertex-import-worker in a read-only application directory with Windows sandbox support.");
    auto result = validatePincWorkerReply(report.output);
    if (cancelled()) throw std::runtime_error("Pinc import cancelled.");
    result.isolation_controls_attested = true;
    return result;
}
} // namespace sketch::desktop
