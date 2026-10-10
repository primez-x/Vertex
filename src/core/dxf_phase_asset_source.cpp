#include "sketch/dxf_phase_asset_source.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Budget = NativeDxfPhaseAssetWorkBudget;
constexpr std::string_view schema = "vertex.dxf.phase-assets.v1";
constexpr std::string_view table_magic = "PSAS0001";
constexpr std::uint64_t metadata_byte_limit = 1024 * 1024;
constexpr std::size_t metadata_node_limit = 100'000;

[[noreturn]] void refuse(std::string_view reason) {
    throw std::invalid_argument("Native DXF phase assets: " + std::string(reason));
}
void require(bool condition, std::string_view reason) { if (!condition) refuse(reason); }
void limits(const Budget& b) {
    require(b.max_payload_bytes <= native_dxf_phase_asset_payload_limit &&
        b.max_work_bytes <= native_dxf_phase_asset_work_limit &&
        b.max_json_bytes <= native_dxf_phase_asset_json_byte_limit &&
        b.max_json_nodes <= native_dxf_phase_asset_json_node_limit &&
        b.consumed_work_bytes <= b.max_work_bytes &&
        b.consumed_json_bytes <= b.max_json_bytes &&
        b.consumed_json_nodes <= b.max_json_nodes, "invalid resource ledger");
}
void charge(std::uint64_t& consumed, std::uint64_t amount, std::uint64_t maximum,
    std::string_view reason) {
    if (amount > maximum - consumed) { consumed = maximum; refuse(reason); }
    consumed += amount;
}
void work(Budget& b, std::uint64_t amount) {
    charge(b.consumed_work_bytes, amount, b.max_work_bytes, "cumulative work limit");
}
void text_bytes(Budget& b, std::uint64_t amount) {
    charge(b.consumed_json_bytes, amount, b.max_json_bytes, "cumulative JSON byte limit");
}
void nodes(Budget& b, std::uint64_t amount = 1) {
    charge(b.consumed_json_nodes, amount, b.max_json_nodes, "cumulative JSON node limit");
}
bool utf8(std::string_view value, bool allow_nul = false) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        if (!first && !allow_nul) return false;
        if (first <= 0x7f) { ++i; continue; }
        std::size_t count{};
        unsigned char low = 0x80, high = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) count = 1;
        else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            if (first == 0xe0) low = 0xa0;
            if (first == 0xed) high = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            if (first == 0xf0) low = 0x90;
            if (first == 0xf4) high = 0x8f;
        } else return false;
        if (count >= value.size() - i) return false;
        const auto second = static_cast<unsigned char>(value[i + 1]);
        if (second < low || second > high) return false;
        for (std::size_t offset = 2; offset <= count; ++offset) {
            const auto next = static_cast<unsigned char>(value[i + offset]);
            if (next < 0x80 || next > 0xbf) return false;
        }
        i += count + 1;
    }
    return true;
}
void identifier(std::string_view id, Budget& b) {
    require(!id.empty() && id.size() <= 128, "invalid asset identity length");
    work(b, id.size());
    require(std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid asset identity");
}
void media_type(std::string_view media, Budget& b) {
    require(!media.empty() && media.size() <= 256, "invalid asset media type length");
    work(b, media.size() * 2);
    require(utf8(media) && media.find_first_of("\r\n") == std::string_view::npos,
        "invalid asset media type");
}
void digest(std::string_view hash, Budget& b) {
    require(hash.size() == 64, "invalid SHA-256 length");
    work(b, hash.size());
    require(std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "invalid SHA-256");
}
// Exact default JSON encoding size, without dumping untrusted containers.
// UTF-8 remains UTF-8; JSON control-character escapes are counted explicitly.
std::uint64_t string_size(std::string_view value, Budget& b) {
    require(value.size() <= metadata_byte_limit, "metadata string byte limit");
    work(b, value.size() * 2 + 1);
    require(utf8(value, true), "metadata requires valid UTF-8");
    std::uint64_t result = 2;
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        result += byte == '"' || byte == '\\' || byte == '\b' || byte == '\f' ||
            byte == '\n' || byte == '\r' || byte == '\t' ? 2 : byte < 0x20 ? 6 : 1;
    }
    return result;
}
void metadata_bytes(std::uint64_t& total, std::uint64_t amount, Budget& b) {
    text_bytes(b, amount);
    require(amount <= metadata_byte_limit - total, "metadata encoded byte limit");
    total += amount;
}
void metadata_value(const Json& value, Budget& b, std::size_t depth,
    std::size_t& count, std::uint64_t& bytes) {
    nodes(b); work(b, 1);
    require(depth <= 64 && ++count <= metadata_node_limit, "metadata complexity limit");
    require(!value.is_binary() && !value.is_discarded(), "non-portable metadata value");
    if (value.is_object()) {
        metadata_bytes(bytes, 2, b);
        bool first = true;
        for (const auto& [key, child] : value.items()) {
            require(key.size() <= 128, "metadata key byte limit");
            metadata_bytes(bytes, string_size(key, b) + 1 + (first ? 0 : 1), b);
            first = false;
            metadata_value(child, b, depth + 1, count, bytes);
        }
    } else if (value.is_array()) {
        metadata_bytes(bytes, 2, b);
        bool first = true;
        for (const auto& child : value) {
            if (!first) metadata_bytes(bytes, 1, b);
            first = false;
            metadata_value(child, b, depth + 1, count, bytes);
        }
    } else if (value.is_string()) {
        metadata_bytes(bytes, string_size(value.get_ref<const std::string&>(), b), b);
    } else if (value.is_number()) {
        require(!value.is_number_float() || std::isfinite(value.get<double>()),
            "non-finite metadata number");
        // Only a bounded scalar is serialized, after reserving its work.
        work(b, 32);
        metadata_bytes(bytes, value.dump().size(), b);
    } else {
        require(value.is_boolean() || value.is_null(), "unsupported metadata scalar");
        metadata_bytes(bytes, value.is_null() ? 4 : value.get<bool>() ? 4 : 5, b);
    }
}
std::uint64_t metadata(const Json& value, Budget& b) {
    require(value.is_object(), "asset metadata must be an object");
    std::size_t count{};
    std::uint64_t bytes{};
    metadata_value(value, b, 0, count, bytes);
    return bytes;
}
std::uint64_t fields(std::string_view key, std::string_view id, std::string_view media,
    std::uint64_t size, std::string_view hash, const Json& raw, Budget& b) {
    identifier(key, b); identifier(id, b);
    require(key == id, "asset map key differs from its identity");
    media_type(media, b); digest(hash, b);
    require(size <= b.max_payload_bytes, "asset payload byte limit");
    return metadata(raw, b) + id.size() + media.size() + hash.size();
}
void inventory_size(std::uint64_t& total, std::uint64_t amount, const Budget& b,
    std::uint64_t aggregate_limit = 0) {
    const auto maximum = aggregate_limit ? aggregate_limit : b.max_payload_bytes;
    require(total <= maximum && amount <= maximum - total, "complete asset inventory payload limit");
    total += amount;
}
void inventory_count(std::size_t count) {
    require(count <= native_dxf_phase_asset_count_limit, "asset inventory count limit");
}
// Charge the fixed manifest envelope and descriptor fields separately from
// metadata. The bounds include punctuation and worst-case 20-digit lengths.
void manifest_envelope(std::size_t count, Budget& b) {
    nodes(b, 4 + static_cast<std::uint64_t>(count) * 5);
    text_bytes(b, 80 + static_cast<std::uint64_t>(count) * 90);
    work(b, 80 + static_cast<std::uint64_t>(count) * 90);
}
void descriptor_text(std::string_view id, std::string_view media, std::string_view hash, Budget& b) {
    text_bytes(b, string_size(id, b) + string_size(media, b) + string_size(hash, b));
}
const Asset& actual_asset(const Asset& asset) { return asset; }
const Asset& actual_asset(const Asset* asset) {
    require(asset != nullptr, "null borrowed asset");
    return *asset;
}
template<class Assets> std::uint64_t assets_validated(const Assets& assets, Budget& b,
    bool manifest_text, std::uint64_t aggregate_limit = 0,
    std::size_t count_limit = native_dxf_phase_asset_count_limit) {
    require(assets.size() <= count_limit, "asset inventory count limit");
    std::uint64_t total{}, copies{};
    // Complete inventory admission precedes hashing any payload.
    for (const auto& [id, entry] : assets) {
        const auto& asset = actual_asset(entry);
        inventory_size(total, asset.bytes.size(), b, aggregate_limit);
        copies += fields(id, asset.id, asset.media_type, asset.bytes.size(), asset.sha256,
            asset.metadata, b);
        if (manifest_text) descriptor_text(asset.id, asset.media_type, asset.sha256, b);
    }
    work(b, total);
    for (const auto& [id, entry] : assets) {
        (void)id;
        const auto& asset = actual_asset(entry);
        require(sha256_hex(asset.bytes) == asset.sha256, "asset SHA-256 differs from actual bytes");
    }
    return copies;
}
std::uint64_t manifest_validated(const NativeDxfPhaseAssetManifest& manifest, Budget& b) {
    inventory_count(manifest.size());
    manifest_envelope(manifest.size(), b);
    std::uint64_t total{}, copies{};
    for (const auto& [id, row] : manifest) {
        inventory_size(total, row.byte_count, b);
        copies += fields(id, row.id, row.media_type, row.byte_count, row.sha256, row.metadata, b);
        descriptor_text(row.id, row.media_type, row.sha256, b);
    }
    return copies;
}
bool exact_raw(const Json& left, const Json& right, Budget& b) {
    work(b, 1);
    if (left.type() != right.type() || left.size() != right.size()) return false;
    if (left.is_object()) {
        auto other = right.begin();
        for (auto item = left.begin(); item != left.end(); ++item, ++other) {
            work(b, item.key().size() + other.key().size());
            if (item.key() != other.key() || !exact_raw(item.value(), other.value(), b)) return false;
        }
        return true;
    }
    if (left.is_array()) {
        for (std::size_t i = 0; i < left.size(); ++i)
            if (!exact_raw(left[i], right[i], b)) return false;
        return true;
    }
    if (left.is_string()) work(b, left.get_ref<const std::string&>().size());
    if (left.is_number_float()) {
        const auto a = left.get<double>(), c = right.get<double>();
        return a == c && std::signbit(a) == std::signbit(c);
    }
    return left == right;
}
void bound(const NativeDxfPhaseAssetManifest& manifest, const NativeDxfPhaseSourceAssets& assets,
    Budget& b) {
    require(manifest.size() == assets.size(), "manifest and actual asset counts differ");
    manifest_validated(manifest, b);
    assets_validated(assets, b, false);
    auto actual = assets.begin();
    for (const auto& [id, row] : manifest) {
        const auto& asset = actual->second;
        work(b, id.size() + actual->first.size() + row.media_type.size() + row.sha256.size());
        require(id == actual->first && row.id == asset.id && row.media_type == asset.media_type &&
            row.byte_count == asset.bytes.size() && row.sha256 == asset.sha256 &&
            exact_raw(row.metadata, asset.metadata, b), "manifest differs from actual asset descriptor");
        ++actual;
    }
}
void exact_fields(const Json& value, std::initializer_list<std::string_view> keys) {
    require(value.is_object() && value.size() == keys.size(), "invalid manifest object fields");
    for (const auto key : keys) require(value.contains(std::string(key)), "missing manifest field");
}
const std::string& json_string(const Json& value) {
    require(value.is_string(), "manifest descriptor field must be a string");
    return value.get_ref<const std::string&>();
}
std::uint64_t unsigned_number(const Json& value) {
    require(value.is_number_unsigned() ||
        (value.is_number_integer() && value.get<std::int64_t>() >= 0),
        "manifest byte count/version must be a nonnegative integer");
    return value.get<std::uint64_t>();
}
unsigned byte(std::byte value) { return std::to_integer<unsigned>(value); }
int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
void write_exact(std::ostream& output, const char* data, std::size_t size, Budget& b) {
    work(b, size);
    if (size) output.write(data, static_cast<std::streamsize>(size));
    require(static_cast<bool>(output), "asset table write failed");
}
void read_exact(std::istream& input, char* data, std::size_t size, Budget& b) {
    work(b, size);
    if (size) input.read(data, static_cast<std::streamsize>(size));
    require(static_cast<bool>(input), "truncated or unreadable asset table");
}
template<std::size_t N> void write_le(std::ostream& output, std::uint64_t value, Budget& b) {
    std::array<char, N> bytes{};
    for (std::size_t i = 0; i < N; ++i) bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    write_exact(output, bytes.data(), bytes.size(), b);
}
template<std::size_t N> std::uint64_t read_le(std::istream& input, Budget& b) {
    std::array<char, N> bytes{};
    read_exact(input, bytes.data(), bytes.size(), b);
    std::uint64_t result{};
    for (std::size_t i = 0; i < N; ++i)
        result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    return result;
}
} // namespace

void validate_native_dxf_phase_source_assets(const NativeDxfPhaseSourceAssets& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    assets_validated(assets, b, false);
}
void validate_native_dxf_phase_source_asset_refs(const NativeDxfPhaseSourceAssetRefs& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    assets_validated(assets, b, false);
}
void validate_native_dxf_phase_destination_asset_refs(const NativeDxfPhaseSourceAssetRefs& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    assets_validated(assets, b, false, native_dxf_phase_destination_asset_payload_limit,
        native_dxf_phase_destination_asset_count_limit);
}
void validate_native_dxf_phase_asset_manifest(const NativeDxfPhaseAssetManifest& manifest, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    manifest_validated(manifest, b);
}
void bind_native_dxf_phase_asset_manifest(const NativeDxfPhaseAssetManifest& manifest,
    const NativeDxfPhaseSourceAssets& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    bound(manifest, assets, b);
}
Json encode_native_dxf_phase_asset_manifest(const NativeDxfPhaseSourceAssets& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    inventory_count(assets.size());
    manifest_envelope(assets.size(), b);
    const auto copies = assets_validated(assets, b, true);
    work(b, copies);
    auto rows = Json::array();
    for (const auto& [id, asset] : assets) {
        rows.push_back({{"id", id}, {"media_type", asset.media_type},
            {"byte_count", static_cast<std::uint64_t>(asset.bytes.size())},
            {"sha256", asset.sha256}, {"metadata", asset.metadata}});
    }
    return {{"schema", schema}, {"version", 1}, {"assets", std::move(rows)}};
}
NativeDxfPhaseAssetManifest decode_native_dxf_phase_asset_manifest(const Json& value, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    exact_fields(value, {"schema", "version", "assets"});
    const auto& schema_value = json_string(value.at("schema"));
    require(schema_value.size() == schema.size(), "unsupported asset manifest schema");
    work(b, schema_value.size());
    require(schema_value == schema && unsigned_number(value.at("version")) == 1,
        "unsupported asset manifest schema/version");
    const auto& rows = value.at("assets");
    require(rows.is_array(), "asset manifest rows must be an array");
    inventory_count(rows.size());
    manifest_envelope(rows.size(), b);
    NativeDxfPhaseAssetManifest result;
    std::uint64_t total{};
    std::string_view previous;
    for (const auto& row : rows) {
        exact_fields(row, {"id", "media_type", "byte_count", "sha256", "metadata"});
        const auto& id = json_string(row.at("id"));
        const auto& media = json_string(row.at("media_type"));
        const auto& hash = json_string(row.at("sha256"));
        const auto size = unsigned_number(row.at("byte_count"));
        inventory_size(total, size, b);
        const auto copies = fields(id, id, media, size, hash, row.at("metadata"), b);
        descriptor_text(id, media, hash, b);
        work(b, id.size() + previous.size());
        require(previous.empty() || previous < id, "asset manifest rows must be sorted and unique");
        previous = id;
        work(b, copies + id.size());
        result.emplace(id, NativeDxfPhaseAssetDescriptor{id, media, size, hash, row.at("metadata")});
    }
    return result;
}
std::string encode_native_dxf_phase_asset_chunk(std::span<const std::byte> bytes, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    require(bytes.size() <= native_dxf_phase_asset_chunk_byte_limit && bytes.size() <= b.max_payload_bytes,
        "asset chunk payload limit");
    const auto size = ((bytes.size() + 2) / 3) * 4;
    work(b, bytes.size() + size);
    std::string result(size, '=');
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0, out = 0; i < bytes.size(); i += 3, out += 4) {
        const auto a = byte(bytes[i]);
        const auto c = i + 1 < bytes.size() ? byte(bytes[i + 1]) : 0;
        const auto d = i + 2 < bytes.size() ? byte(bytes[i + 2]) : 0;
        result[out] = alphabet[a >> 2];
        result[out + 1] = alphabet[((a & 3) << 4) | (c >> 4)];
        if (i + 1 < bytes.size()) result[out + 2] = alphabet[((c & 15) << 2) | (d >> 6)];
        if (i + 2 < bytes.size()) result[out + 3] = alphabet[d & 63];
    }
    return result;
}
std::vector<std::byte> decode_native_dxf_phase_asset_chunk(std::string_view text, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    require(text.size() <= native_dxf_phase_asset_chunk_text_limit && text.size() % 4 == 0,
        "asset chunk text length limit");
    work(b, text.size());
    std::size_t padding{};
    if (!text.empty() && text.back() == '=') {
        padding = 1;
        if (text[text.size() - 2] == '=') padding = 2;
    }
    const auto plain = text.size() - padding;
    for (std::size_t i = 0; i < plain; ++i)
        require(sextet(text[i]) >= 0, "asset chunk contains noncanonical Base64");
    for (std::size_t i = plain; i < text.size(); ++i)
        require(text[i] == '=', "asset chunk padding is invalid");
    if (padding == 2) require((sextet(text[plain - 1]) & 15) == 0, "asset chunk has nonzero trailing bits");
    if (padding == 1) require((sextet(text[plain - 1]) & 3) == 0, "asset chunk has nonzero trailing bits");
    const auto size = text.size() / 4 * 3 - padding;
    require(size <= native_dxf_phase_asset_chunk_byte_limit && size <= b.max_payload_bytes,
        "asset chunk decoded payload limit");
    work(b, text.size() + size);
    std::vector<std::byte> result(size);
    for (std::size_t i = 0, out = 0; i < text.size(); i += 4) {
        const auto a = static_cast<unsigned>(sextet(text[i]));
        const auto c = static_cast<unsigned>(sextet(text[i + 1]));
        const auto d = text[i + 2] == '=' ? 0U : static_cast<unsigned>(sextet(text[i + 2]));
        const auto e = text[i + 3] == '=' ? 0U : static_cast<unsigned>(sextet(text[i + 3]));
        if (out < size) result[out++] = static_cast<std::byte>((a << 2) | (c >> 4));
        if (out < size) result[out++] = static_cast<std::byte>((c << 4) | (d >> 2));
        if (out < size) result[out++] = static_cast<std::byte>((d << 6) | e);
    }
    return result;
}
void write_native_dxf_phase_asset_table(std::ostream& output,
    const NativeDxfPhaseAssetManifest& manifest, const NativeDxfPhaseSourceAssets& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    bound(manifest, assets, b);
    write_exact(output, table_magic.data(), table_magic.size(), b);
    write_le<4>(output, assets.size(), b);
    for (const auto& [id, asset] : assets) {
        write_le<4>(output, id.size(), b);
        write_exact(output, id.data(), id.size(), b);
        write_le<8>(output, asset.bytes.size(), b);
        write_exact(output, reinterpret_cast<const char*>(asset.bytes.data()), asset.bytes.size(), b);
    }
}
NativeDxfPhaseSourceAssets read_native_dxf_phase_asset_table(std::istream& input,
    const NativeDxfPhaseAssetManifest& manifest, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    const auto copies = manifest_validated(manifest, b);
    std::array<char, 8> magic{};
    read_exact(input, magic.data(), magic.size(), b);
    require(std::string_view(magic.data(), magic.size()) == table_magic, "invalid asset table magic");
    require(read_le<4>(input, b) == manifest.size(), "asset table count differs from manifest");
    work(b, copies);
    NativeDxfPhaseSourceAssets result;
    std::array<char, 128> id_buffer{};
    for (const auto& [id, row] : manifest) {
        const auto id_size = read_le<4>(input, b);
        require(id_size == id.size(), "asset table identity length differs from manifest");
        read_exact(input, id_buffer.data(), static_cast<std::size_t>(id_size), b);
        work(b, id_size);
        require(std::string_view(id_buffer.data(), static_cast<std::size_t>(id_size)) == id,
            "asset table identity/order differs from manifest");
        const auto size = read_le<8>(input, b);
        require(size == row.byte_count && size <= b.max_payload_bytes &&
            size <= std::numeric_limits<std::size_t>::max(), "asset table payload length differs from manifest");
        // Reserve allocation/initialization, input copy, and hashing before allocation.
        work(b, size * 2);
        Asset asset{id, row.media_type, std::vector<std::byte>(static_cast<std::size_t>(size)),
            row.sha256, row.metadata};
        read_exact(input, reinterpret_cast<char*>(asset.bytes.data()), asset.bytes.size(), b);
        require(sha256_hex(asset.bytes) == row.sha256, "asset table payload hash differs from manifest");
        work(b, id.size());
        result.emplace(id, std::move(asset));
    }
    return result;
}
} // namespace sketch
