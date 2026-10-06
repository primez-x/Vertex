#include "assistance_ocr.hpp"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace sketch::desktop {
std::filesystem::path assistanceOcrApplicationRoot() {
    const auto binary = std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString());
    return QString::fromStdWString(binary.filename().native()).compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0
        ? binary.parent_path() : binary;
}
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid() { throw std::invalid_argument("Invalid bounded assistance OCR data."); }
std::vector<AssistanceResource> fixedResources() {
    return {{"vertex-ocr-engine-v1", assistanceOcrEnginePath,
             "Tesseract 5.5.2 offline LSTM recognizer", "Apache-2.0", true},
            {"tessdata-fast-eng-v1", assistanceOcrModelPath,
             "tesseract-ocr/tessdata_fast 4.1.0 revision 65727574dfcd264acbb0c3e07860e4e9e9b22185; SHA-256 " +
                 std::string(assistanceOcrModelSha256), "Apache-2.0", true}};
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) invalid();
    for (const auto* name : names) if (!value.contains(name)) invalid();
}
Json parse(std::span<const std::byte> bytes, std::size_t limit) {
    if (bytes.empty() || bytes.size() > limit) invalid();
    std::map<int, std::set<std::string>> names;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 8) invalid();
        if (event == Json::parse_event_t::object_start) names[depth + 1].clear();
        if (event == Json::parse_event_t::key &&
            !names[depth].insert(value.get<std::string>()).second) invalid();
        if (event == Json::parse_event_t::object_end) names.erase(depth + 1);
        return true;
    };
    const auto* start = reinterpret_cast<const char*>(bytes.data());
    try {
        return Json::parse(start, start + bytes.size(), callback, true, false);
    } catch (const Json::exception&) {
        invalid(); // Do not surface worker-controlled parser excerpts in the GUI.
    }
}
bool utf8(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at++]);
        if (lead < 0x80) { if (!lead) return false; continue; }
        std::size_t count; std::uint32_t code, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; code = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; code = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; code = lead & 7; minimum = 0x10000; }
        else return false;
        if (count > text.size() - at) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[at++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool boundary(std::string_view text, std::size_t at) {
    return at == text.size() || (static_cast<unsigned char>(text[at]) & 0xc0) != 0x80;
}
bool whitespace(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return c == ' ' || c == '\n' || c == '\r' || c == '\t';
    });
}
void validateText(const AssistanceOcrResult& result) {
    if (result.text.size() > assistanceOcrTextLimit || !utf8(result.text) ||
        result.runs.size() > assistanceOcrRunLimit) invalid();
    std::size_t end = 0;
    for (const auto& run : result.runs) {
        if (!run.length || run.offset < end || run.offset > result.text.size() ||
            run.length > result.text.size() - run.offset || !boundary(result.text, run.offset) ||
            !boundary(result.text, run.offset + run.length) ||
            !whitespace(std::string_view(result.text).substr(end, run.offset - end)) ||
            !run.confidence || !std::isfinite(*run.confidence) || *run.confidence < 0 || *run.confidence > 1 ||
            !std::isfinite(run.x) || !std::isfinite(run.y) || !std::isfinite(run.width) || !std::isfinite(run.height) ||
            run.x < 0 || run.y < 0 || run.width <= 0 || run.height <= 0 ||
            run.x + run.width > 1 || run.y + run.height > 1) invalid();
        end = run.offset + run.length;
    }
    if (!whitespace(std::string_view(result.text).substr(end))) invalid();
}
std::size_t integer(const Json& value) {
    if (!value.is_number_unsigned()) invalid();
    const auto n = value.get<std::uint64_t>();
    if (n > assistanceOcrReplyLimit) invalid();
    return static_cast<std::size_t>(n);
}
double real(const Json& value) {
    if (!value.is_number()) invalid();
    const auto n = value.get<double>(); if (!std::isfinite(n)) invalid(); return n;
}
QString nativePath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromStdString(path.native());
#endif
}
void safePath(const std::filesystem::path& path, bool regular) {
    if (!path.is_absolute()) invalid();
    std::filesystem::path current;
    for (const auto& component : path) {
        current /= component;
        if (current == path.root_name()) continue;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_directory(status) && current != path)) invalid();
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) invalid();
#endif
        if (current == path && (regular ? !std::filesystem::is_regular_file(status)
                                      : !std::filesystem::is_directory(status))) invalid();
    }
}
QByteArray read(const std::filesystem::path& path, qsizetype limit) {
    safePath(path, true);
    QFile file(nativePath(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) invalid();
    const auto result = file.read(limit + 1);
    if (result.isEmpty() || result.size() > limit || file.error() != QFileDevice::NoError ||
        !file.atEnd()) invalid();
    return result;
}
std::span<const std::byte> bytes(const QByteArray& data) {
    return {reinterpret_cast<const std::byte*>(data.constData()), static_cast<std::size_t>(data.size())};
}
Json descriptor() {
    return {{"schema", "vertex.assistance.ocr-engine"}, {"version", 1},
        {"id", "vertex-ocr-engine-v1"}, {"engine", "tesseract"}, {"engine_version", "5.5.2"},
        {"model_id", "tessdata-fast-eng-v1"}, {"model_path", assistanceOcrModelPath},
        {"model_sha256", assistanceOcrModelSha256}, {"model_bytes", assistanceOcrModelBytes},
        {"model_revision", "65727574dfcd264acbb0c3e07860e4e9e9b22185"}, {"language", "eng"},
        {"license", "Apache-2.0"}, {"license_path", assistanceOcrLicensePath}};
}
} // namespace

std::vector<std::byte> encodeAssistanceOcrFrame(const AssistanceRaster& raster) {
    if (!raster.width || !raster.height || raster.width > assistanceOcrDimensionLimit ||
        raster.height > assistanceOcrDimensionLimit ||
        raster.width * raster.height > assistanceOcrPixelLimit ||
        raster.luminance.size() != raster.width * raster.height) invalid();
    std::vector<std::byte> result(16 + raster.luminance.size());
    std::memcpy(result.data(), "VXOC0001", 8);
    qToLittleEndian<quint32>(static_cast<quint32>(raster.width), reinterpret_cast<uchar*>(result.data()) + 8);
    qToLittleEndian<quint32>(static_cast<quint32>(raster.height), reinterpret_cast<uchar*>(result.data()) + 12);
    std::memcpy(result.data() + 16, raster.luminance.data(), raster.luminance.size());
    return result;
}
AssistanceRaster decodeAssistanceOcrFrame(std::span<const std::byte> frame) {
    if (frame.size() < 16 || frame.size() > 16 + assistanceOcrPixelLimit ||
        std::memcmp(frame.data(), "VXOC0001", 8) != 0) invalid();
    AssistanceRaster result;
    result.width = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(frame.data()) + 8);
    result.height = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(frame.data()) + 12);
    if (!result.width || !result.height || result.width > assistanceOcrDimensionLimit ||
        result.height > assistanceOcrDimensionLimit || result.width * result.height > assistanceOcrPixelLimit ||
        frame.size() != 16 + result.width * result.height) invalid();
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(frame.data()) + 16;
    result.luminance.assign(pixels, pixels + result.width * result.height);
    return result;
}

std::vector<std::byte> encodeAssistanceOcrReply(const AssistanceOcrResult& result) {
    validateText(result);
    if ((!result.producer.empty() && result.producer != assistanceOcrProducer) ||
        (!result.resources.empty() && result.resources != fixedResources())) invalid();
    Json runs = Json::array();
    for (const auto& run : result.runs)
        runs.push_back({{"offset", run.offset}, {"length", run.length}, {"x", run.x}, {"y", run.y},
            {"width", run.width}, {"height", run.height}, {"confidence", *run.confidence}});
    const auto text = Json{{"schema", "vertex.assistance.ocr"}, {"version", 1},
        {"engine_id", "vertex-ocr-engine-v1"}, {"engine_version", "5.5.2"},
        {"model_id", "tessdata-fast-eng-v1"}, {"model_sha256", assistanceOcrModelSha256},
        {"text", result.text}, {"runs", std::move(runs)}}.dump();
    if (text.size() > assistanceOcrReplyLimit) invalid();
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

AssistanceOcrResult validateAssistanceOcrReply(std::span<const std::byte> reply) {
    const auto value = parse(reply, assistanceOcrReplyLimit);
    fields(value, {"schema", "version", "engine_id", "engine_version", "model_id", "model_sha256", "text", "runs"});
    if (value.at("schema") != "vertex.assistance.ocr" || integer(value.at("version")) != 1 ||
        value.at("engine_id") != "vertex-ocr-engine-v1" || value.at("engine_version") != "5.5.2" ||
        value.at("model_id") != "tessdata-fast-eng-v1" || value.at("model_sha256") != assistanceOcrModelSha256 ||
        !value.at("text").is_string() || !value.at("runs").is_array() ||
        value.at("runs").size() > assistanceOcrRunLimit) invalid();
    AssistanceOcrResult result;
    result.text = value.at("text").get<std::string>();
    for (const auto& item : value.at("runs")) {
        fields(item, {"offset", "length", "x", "y", "width", "height", "confidence"});
        result.runs.push_back({integer(item.at("offset")), integer(item.at("length")),
            real(item.at("x")), real(item.at("y")), real(item.at("width")), real(item.at("height")),
            real(item.at("confidence"))});
    }
    validateText(result); result.resources = fixedResources(); result.producer = assistanceOcrProducer;
    return result;
}

std::vector<AssistanceResource> verifiedAssistanceOcrResources(const std::filesystem::path& root) {
    safePath(root, false);
    const auto description = read(root / assistanceOcrEnginePath, 16384);
    if (parse(bytes(description), 16384) != descriptor()) invalid();
    const auto license = read(root / assistanceOcrLicensePath, 65536);
    if (!license.contains("Apache License") || !license.contains("Version 2.0")) invalid();
    const auto model = read(root / assistanceOcrModelPath, assistanceOcrModelBytes);
    if (model.size() != assistanceOcrModelBytes ||
        QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex() != assistanceOcrModelSha256) invalid();
    return fixedResources();
}

AssistanceOcrResult recognizeAssistanceRaster(const AssistanceRaster& raster,
    WindowsImportWorkerOptions options, const AssistanceOcrBroker& broker) {
    const auto cancelled = [&options] {
        return options.cancellation_requested && options.cancellation_requested->load(std::memory_order_acquire);
    };
    if (cancelled()) throw std::runtime_error("Assistance OCR cancelled.");
    options.input = encodeAssistanceOcrFrame(raster);
    options.arguments = {L"ocr", L"0"};
    options.timeout_ms = std::min<std::uint64_t>(options.timeout_ms, 30000);
    options.memory_bytes = 512ULL * 1024 * 1024;
    options.max_active_processes = 1; options.max_output_bytes = assistanceOcrReplyLimit;
    options.proj_offline_required = true;
    if (cancelled()) throw std::runtime_error("Assistance OCR cancelled.");
    const auto report = broker(options);
    if (report.status == WindowsImportWorkerStatus::cancelled || cancelled())
        throw std::runtime_error("Assistance OCR cancelled.");
    if (!report.controls_attested())
        throw std::runtime_error("Isolated assistance OCR is unavailable. Install or repair the bundled "
            "vertex-import-worker and OCR resources in a read-only application directory with Windows sandbox support.");
    auto result = validateAssistanceOcrReply(report.output);
    if (cancelled()) throw std::runtime_error("Assistance OCR cancelled.");
    result.isolation_controls_attested = true;
    return result;
}
} // namespace sketch::desktop
