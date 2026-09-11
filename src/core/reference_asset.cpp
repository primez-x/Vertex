#include "sketch/reference_asset.hpp"
#include "sketch/document.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
void check(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}
bool safe_path(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.front() == '/') return false;
    if (path.find_first_of("\\:") != std::string::npos) return false;
    for (unsigned char c : path) if (c < 32 || c == 127) return false;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        if (part.find_first_of("<>\"|?*") != std::string::npos) return false;
        auto stem = part.substr(0, part.find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
        });
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
             stem[3] >= '1' && stem[3] <= '9')) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}
double distance(ReferencePoint a, ReferencePoint b) {
    check(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y),
        "reference points must be finite");
    const double result = std::hypot(b.x - a.x, b.y - a.y);
    check(std::isfinite(result), "reference distance overflow");
    return result;
}
void validate_transform(const ReferenceTransform& t) {
    check(std::isfinite(t.scale) && t.scale > 0 && std::isfinite(t.rotation_degrees) &&
        std::isfinite(t.intensity) && t.intensity >= 0 && t.intensity <= 1,
        "invalid reference transform");
}
}

const ReferenceAsset& ReferenceAssetCatalog::import(std::string id, std::string path,
    std::string mime, std::vector<std::byte> bytes, std::optional<unsigned> page) {
    check(!id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
    }), "invalid reference id");
    check(!assets_.contains(id), "duplicate reference id");
    check(safe_path(path), "unsafe reference provenance path");
    check(!bytes.empty() && bytes.size() <= 256ULL * 1024 * 1024, "invalid reference size");
    auto prefix = [&](std::string_view signature) {
        return bytes.size() >= signature.size() && std::equal(signature.begin(), signature.end(),
            bytes.begin(), [](char a, std::byte b) { return static_cast<unsigned char>(a) == std::to_integer<unsigned char>(b); });
    };
    const bool pdf = mime == "application/pdf";
    check((pdf && prefix("%PDF-")) || (mime == "image/png" && prefix("\x89PNG\r\n\x1a\n")) ||
        (mime == "image/jpeg" && prefix("\xff\xd8\xff")) ||
        (mime == "image/bmp" && prefix("BM")) ||
        (mime == "image/tiff" && (prefix(std::string_view("II\x2a\0", 4)) || prefix(std::string_view("MM\0\x2a", 4)))),
        "unsupported MIME type or mismatched reference signature");
    check(pdf ? page.has_value() && *page > 0 : !page.has_value(), "PDF requires a positive page; rasters have no page");
    const auto hash = sha256_hex(bytes);
    auto [it, inserted] = assets_.emplace(id, ReferenceAsset{id, std::move(path), std::move(mime),
        hash, std::move(bytes), page, std::nullopt, {}});
    return it->second;
}
const ReferenceAsset& ReferenceAssetCatalog::at(const std::string& id) const { return assets_.at(id); }
void ReferenceAssetCatalog::calibrate(const std::string& id, ReferencePoint a, ReferencePoint b,
    std::string expression, Unit unit) {
    check(unit >= Unit::metre && unit <= Unit::inch, "invalid calibration unit");
    const auto known = parse_quantity(expression, unit);
    const auto span = distance(a, b);
    check(span > 0 && known.metres > 0, "calibration distances must be positive");
    const auto scale = known.metres / span;
    check(std::isfinite(scale) && scale > 0, "invalid calibration scale");
    auto& asset = assets_.at(id);
    undo_.push_back({id, asset.calibration, asset.transform});
    asset.calibration = ReferenceCalibration{a, b, std::move(expression), unit, scale};
}
void ReferenceAssetCatalog::set_transform(const std::string& id, ReferenceTransform transform) {
    validate_transform(transform);
    auto& asset = assets_.at(id);
    undo_.push_back({id, asset.calibration, asset.transform});
    asset.transform = transform;
}
bool ReferenceAssetCatalog::undo() {
    if (undo_.empty()) return false;
    auto edit = std::move(undo_.back()); undo_.pop_back();
    auto& asset = assets_.at(edit.id);
    asset.calibration = std::move(edit.calibration); asset.transform = edit.transform;
    return true;
}
double ReferenceAssetCatalog::measure_metres(const std::string& id, ReferencePoint a, ReferencePoint b) const {
    const auto& calibration = at(id).calibration;
    check(calibration.has_value(), "reference is not calibrated");
    const double value = distance(a, b) * calibration->metres_per_source_unit;
    check(std::isfinite(value), "reference measurement overflow");
    return value;
}
nlohmann::json ReferenceAssetCatalog::snapshot() const {
    auto records = nlohmann::json::array();
    for (const auto& [id, a] : assets_) {
        std::vector<unsigned> bytes; bytes.reserve(a.bytes.size());
        for (auto b : a.bytes) bytes.push_back(std::to_integer<unsigned>(b));
        const auto& t = a.transform;
        nlohmann::json record{{"id", id}, {"path", a.source_path}, {"mime", a.mime_type},
            {"sha256", a.sha256}, {"bytes", bytes}, {"page", a.page ? nlohmann::json(*a.page) : nlohmann::json(nullptr)},
            {"fidelity", "signature-only; decoding and page existence unverified"},
            {"content", "traceable-reference; no editable extraction"},
            {"transform", {t.scale, t.rotation_degrees, t.flip_horizontal, t.flip_vertical, t.intensity, t.visible}},
            {"calibration", nullptr}};
        if (a.calibration) {
            const auto& c = *a.calibration;
            record["calibration"] = {{"first", {c.first.x, c.first.y}}, {"second", {c.second.x, c.second.y}},
                {"known_distance", c.known_distance}, {"input_unit", static_cast<int>(c.input_unit)},
                {"metres_per_source_unit", c.metres_per_source_unit}};
        }
        records.push_back(std::move(record));
    }
    return {{"version", 1}, {"assets", records}};
}
ReferenceAssetCatalog ReferenceAssetCatalog::restore(const nlohmann::json& input) {
    check(input.at("version") == 1 && input.at("assets").is_array(), "unsupported reference snapshot");
    ReferenceAssetCatalog result;
    for (const auto& row : input.at("assets")) {
        const auto& data = row.at("bytes");
        check(data.is_array() && data.size() <= 256ULL * 1024 * 1024, "invalid snapshot bytes");
        std::vector<std::byte> bytes; bytes.reserve(data.size());
        for (const auto& value : data) {
            check(value.is_number_integer() && value >= 0 && value <= 255, "invalid byte");
            bytes.push_back(static_cast<std::byte>(value.get<unsigned>()));
        }
        std::optional<unsigned> page;
        if (!row.at("page").is_null()) {
            check(row.at("page").is_number_unsigned() && row.at("page") <= 4294967295ULL, "invalid page");
            page = row.at("page").get<unsigned>();
        }
        const auto id = row.at("id").get<std::string>();
        const auto& asset = result.import(id, row.at("path"), row.at("mime"), std::move(bytes), page);
        check(asset.sha256 == row.at("sha256").get<std::string>(), "reference hash mismatch");
        const auto& t = row.at("transform");
        check(t.is_array() && t.size() == 6, "invalid transform record");
        result.set_transform(id, {t[0].get<double>(), t[1].get<double>(), t[2].get<bool>(),
            t[3].get<bool>(), t[4].get<double>(), t[5].get<bool>()});
        const auto& c = row.at("calibration");
        if (!c.is_null()) {
            const auto unit = c.at("input_unit").get<int>();
            check(unit >= 0 && unit <= static_cast<int>(Unit::inch), "invalid calibration unit");
            result.calibrate(id, {c.at("first").at(0), c.at("first").at(1)},
                {c.at("second").at(0), c.at("second").at(1)}, c.at("known_distance"), static_cast<Unit>(unit));
            check(result.at(id).calibration->metres_per_source_unit == c.at("metres_per_source_unit").get<double>(),
                "calibration provenance mismatch");
        }
    }
    result.undo_.clear();
    return result;
}
} // namespace sketch
