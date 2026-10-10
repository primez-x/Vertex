#include "sketch/dxf_phase_asset_carrier.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Budget = NativeDxfPhaseAssetWorkBudget;
constexpr std::uint64_t ordinary_byte_limit = 16ULL * 1024 * 1024;
constexpr std::string_view begin_marker = "VERTEX_PHASE_ASSETS_V1";
constexpr std::string_view end_marker = "VERTEX_PHASE_ASSETS_END_V1";
constexpr std::string_view terminal = "0\nEOF\n";
constexpr std::uint64_t record_limit = native_dxf_phase_asset_payload_limit /
    native_dxf_phase_asset_carrier_chunk_bytes + 2 * native_dxf_phase_asset_count_limit + 3;

[[noreturn]] void refuse(std::string_view reason) {
    throw std::invalid_argument("Native DXF phase asset carrier: " + std::string(reason));
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
void work(Budget& b, std::uint64_t amount) {
    if (amount > b.max_work_bytes - b.consumed_work_bytes) {
        b.consumed_work_bytes = b.max_work_bytes;
        refuse("cumulative work limit");
    }
    b.consumed_work_bytes += amount;
}
bool reserved(std::string_view value) {
    return value.starts_with("VERTEX_PHASE_ASSETS") ||
        value.starts_with("VXPA") || value.starts_with("VXPD");
}
std::string_view trim_space(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
        line.remove_prefix(1);
    return line;
}
bool comment_code(std::string_view code) {
    code = trim_space(code);
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t' || code.back() == '\r'))
        code.remove_suffix(1);
    return code == "999";
}
bool marker_scan(std::string_view input) noexcept {
    std::size_t offset{};
    while (offset < input.size()) {
        const auto code_end = input.find('\n', offset);
        if (code_end == std::string_view::npos) break;
        const auto code = input.substr(offset, code_end - offset);
        const auto value_begin = code_end + 1;
        const auto value_end = input.find('\n', value_begin);
        const auto size = value_end == std::string_view::npos ? input.size() - value_begin : value_end - value_begin;
        if (comment_code(code) && reserved(trim_space(input.substr(value_begin, size)))) return true;
        if (value_end == std::string_view::npos) break;
        offset = value_end + 1;
    }
    return false;
}
struct Pair { std::string_view code; std::string_view value; std::size_t begin{}; };
struct Reader {
    std::string_view text;
    std::size_t offset{};
    std::string_view line() {
        const auto end = text.find('\n', offset);
        require(end != std::string_view::npos, "requires complete LF record framing");
        const auto result = text.substr(offset, end - offset);
        offset = end + 1;
        return result;
    }
    Pair pair() {
        const auto begin = offset;
        const auto code = line();
        return {code, line(), begin};
    }
};
void comment(const Pair& pair) {
    require(pair.code == "999" && pair.value.size() <= 255, "invalid physical comment record");
    require(std::all_of(pair.value.begin(), pair.value.end(), [](unsigned char c) {
        return c >= 0x20 && c <= 0x7e;
    }), "comment values require printable ASCII");
}
std::uint64_t decimal(std::string_view text) {
    require(!text.empty() && (text.size() == 1 || text.front() != '0'), "noncanonical decimal");
    std::uint64_t result{};
    for (const auto c : text) {
        require(c >= '0' && c <= '9', "invalid decimal");
        const auto digit = static_cast<unsigned>(c - '0');
        require(result <= (std::numeric_limits<std::uint64_t>::max() - digit) / 10,
            "decimal overflow");
        result = result * 10 + digit;
    }
    return result;
}
template<std::size_t N> std::array<std::string_view, N> fields(std::string_view value) {
    std::array<std::string_view, N> result{};
    for (std::size_t i = 0; i < N - 1; ++i) {
        const auto separator = value.find(' ');
        require(separator != std::string_view::npos && separator != 0, "invalid record fields");
        result[i] = value.substr(0, separator);
        value.remove_prefix(separator + 1);
    }
    require(value.find(' ') == std::string_view::npos, "extra record fields");
    result.back() = value; // The final data field alone may be empty.
    return result;
}
void identity(std::string_view id) {
    require(!id.empty() && id.size() <= 128, "invalid asset identity length");
    require(std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid asset identity");
}
void digest(std::string_view hash) {
    require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "invalid SHA-256");
}
std::uint64_t chunks(std::uint64_t size) {
    return size == 0 ? 1 : (size - 1) / native_dxf_phase_asset_carrier_chunk_bytes + 1;
}
std::size_t chunk_size(std::uint64_t size, std::uint64_t index) {
    const auto offset = index * native_dxf_phase_asset_carrier_chunk_bytes;
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        native_dxf_phase_asset_carrier_chunk_bytes, size - offset));
}
int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
void base64(std::string_view text, std::size_t raw_size) {
    require(text.size() == (raw_size + 2) / 3 * 4, "chunk length differs from declared bytes");
    const auto padding = raw_size % 3 == 0 ? 0U : 3U - static_cast<unsigned>(raw_size % 3);
    const auto plain = text.size() - padding;
    for (std::size_t i = 0; i < plain; ++i)
        require(sextet(text[i]) >= 0, "noncanonical Base64");
    for (std::size_t i = plain; i < text.size(); ++i)
        require(text[i] == '=', "invalid Base64 padding");
    if (padding == 2) require((sextet(text[plain - 1]) & 15) == 0, "nonzero Base64 trailing bits");
    if (padding == 1) require((sextet(text[plain - 1]) & 3) == 0, "nonzero Base64 trailing bits");
}
struct Admission { std::size_t footer{}; std::size_t count{}; std::uint64_t payload{}; };
// First pass admits the complete physical inventory without allocations. A
// second pass can store one view per admitted asset; neither pass decodes bytes.
Admission scan(std::string_view input, Budget& b,
    std::vector<NativeDxfPhaseAssetCarrierRecord>* records = nullptr) {
    require(input.size() <= native_dxf_phase_asset_transport_byte_limit, "transport byte limit");
    work(b, input.size() * 2ULL); // Line scan and ASCII/field/Base64 inspection.
    require(input.find('\r') == std::string_view::npos, "carrier requires canonical LF framing");
    Reader reader{input};
    Admission result;
    bool found{};
    while (reader.offset < input.size()) {
        const auto pair = reader.pair();
        if (comment_code(pair.code) && pair.value == begin_marker) {
            comment(pair); result.footer = pair.begin; found = true; break;
        }
        require(!comment_code(pair.code) || !reserved(trim_space(pair.value)),
            "reserved record outside asset footer");
    }
    require(found, "orphan or malformed asset footer marker");
    require(result.footer + terminal.size() <= ordinary_byte_limit, "ordinary DXF byte limit");
    std::string_view previous;
    std::uint64_t physical_count = 1;
    while (reader.offset < input.size()) {
        const auto header = reader.pair();
        comment(header);
        require(++physical_count <= record_limit, "physical record count limit");
        if (header.value == end_marker) {
            require(result.count != 0, "empty asset footer");
            require(input.substr(reader.offset) == terminal, "asset footer must immediately precede terminal EOF");
            return result;
        }
        const auto values = fields<6>(header.value);
        require(values[0] == "VXPA1" && decimal(values[1]) == result.count,
            "invalid asset header ordinal/order");
        identity(values[2]); digest(values[5]);
        require(previous.empty() || previous < values[2], "asset identities must be sorted and unique");
        previous = values[2];
        const auto size = decimal(values[3]);
        const auto count = decimal(values[4]);
        require(size <= b.max_payload_bytes - result.payload, "complete asset inventory payload limit");
        result.payload += size;
        require(count == chunks(size), "chunk count differs from declared bytes");
        require(++result.count <= native_dxf_phase_asset_count_limit, "asset inventory count limit");
        require(count <= record_limit - physical_count, "physical record count limit");
        physical_count += count;
        const auto data_begin = reader.offset;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto data = reader.pair(); comment(data);
            const auto parts = fields<4>(data.value);
            require(parts[0] == "VXPD1" && decimal(parts[1]) == result.count - 1 &&
                decimal(parts[2]) == index, "invalid data record ordinal/index/order");
            base64(parts[3], chunk_size(size, index));
        }
        if (records) records->push_back({values[2], values[5], size, count,
            input.substr(data_begin, reader.offset - data_begin)});
    }
    refuse("missing asset footer end marker");
}
std::vector<NativeDxfPhaseAssetCarrierRecord> admitted_records(
    std::string_view input, const Admission& admission, Budget& b) {
    work(b, admission.count * sizeof(NativeDxfPhaseAssetCarrierRecord));
    std::vector<NativeDxfPhaseAssetCarrierRecord> records;
    records.reserve(admission.count);
    scan(input, b, &records);
    return records;
}
std::uint64_t digits(std::uint64_t number) {
    std::uint64_t count = 1;
    while (number >= 10) { number /= 10; ++count; }
    return count;
}
std::uint64_t index_digits(std::uint64_t count) {
    std::uint64_t total = count;
    for (std::uint64_t threshold = 10; threshold < count; threshold *= 10)
        total += count - threshold;
    return total;
}
void output_size(std::uint64_t& size, std::uint64_t amount) {
    require(size <= native_dxf_phase_asset_transport_byte_limit &&
        amount <= native_dxf_phase_asset_transport_byte_limit - size, "transport byte limit");
    size += amount;
}
void append_comment(std::string& output, std::string_view value) {
    require(value.size() <= 255, "physical comment value byte limit");
    output += "999\n"; output += value; output += '\n';
}
void canonical_ordinary(std::string_view input, Budget& b) {
    work(b, input.size());
    require(input.ends_with(terminal) && input.find('\r') == std::string_view::npos,
        "asset export requires canonical LF terminal EOF");
    Reader reader{input};
    Pair last;
    while (reader.offset < input.size()) last = reader.pair();
    require(last.code == "0" && last.value == "EOF", "invalid terminal EOF framing");
}
} // namespace

bool native_dxf_phase_asset_carrier_present(std::string_view input) noexcept {
    return marker_scan(input.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(
        input.size(), native_dxf_phase_asset_transport_byte_limit))));
}

NativeDxfPhaseAssetCarrier split_native_dxf_phase_asset_carrier(std::string_view input, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    require(input.size() <= native_dxf_phase_asset_transport_byte_limit, "transport byte limit");
    work(b, input.size());
    NativeDxfPhaseAssetCarrier result;
    result.full_input = input;
    if (!marker_scan(input)) {
        require(input.size() <= ordinary_byte_limit, "ordinary DXF byte limit");
        work(b, input.size()); result.ordinary_dxf.assign(input);
        return result;
    }
    const auto admission = scan(input, b);
    result.records = admitted_records(input, admission, b);
    work(b, admission.footer + terminal.size());
    result.ordinary_dxf.assign(input.substr(0, admission.footer));
    result.ordinary_dxf += terminal;
    result.has_assets = true;
    return result;
}

NativeDxfPhaseSourceAssets decode_native_dxf_phase_asset_carrier(
    const NativeDxfPhaseAssetCarrier& carrier, const NativeDxfPhaseAssetManifest& manifest, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    const auto json_before = b.consumed_json_bytes;
    validate_native_dxf_phase_asset_manifest(manifest, &b);
    if (!carrier.has_assets) {
        require(manifest.empty() && carrier.records.empty(), "missing asset footer for graph manifest");
        require(carrier.full_input.size() <= ordinary_byte_limit, "ordinary DXF byte limit");
        work(b, carrier.full_input.size() * 2ULL);
        require(!marker_scan(carrier.full_input) && carrier.full_input == carrier.ordinary_dxf,
            "inconsistent plain carrier");
        return {};
    }
    // Re-admit the original span rather than trusting externally mutable views,
    // flags or descriptor rows. This still allocates no payload before binding.
    const auto admission = scan(carrier.full_input, b);
    require(admission.count == manifest.size() && !manifest.empty(), "orphan or incomplete asset inventory");
    work(b, admission.footer + terminal.size());
    require(carrier.ordinary_dxf.size() == admission.footer + terminal.size() &&
        std::string_view(carrier.ordinary_dxf).substr(0, admission.footer) ==
            carrier.full_input.substr(0, admission.footer) &&
        std::string_view(carrier.ordinary_dxf).substr(admission.footer) == terminal,
        "ordinary DXF differs from original carrier");
    const auto records = admitted_records(carrier.full_input, admission, b);
    require(carrier.records.size() == records.size(), "inconsistent carrier records");
    auto expected = manifest.begin();
    for (std::size_t i = 0; i < records.size(); ++i, ++expected) {
        const auto& record = records[i]; const auto& supplied = carrier.records[i];
        work(b, record.id.size() * 3ULL + record.sha256.size() * 3ULL);
        require(record.id == expected->first && record.id == expected->second.id &&
            record.byte_count == expected->second.byte_count && record.sha256 == expected->second.sha256,
            "asset footer differs from actual graph manifest");
        // Compare range identity, not megabytes of data a third time.
        require(supplied.id.data() == record.id.data() && supplied.id.size() == record.id.size() &&
            supplied.sha256.data() == record.sha256.data() && supplied.sha256.size() == record.sha256.size() &&
            supplied.byte_count == record.byte_count && supplied.chunk_count == record.chunk_count &&
            supplied.data_records.data() == record.data_records.data() &&
            supplied.data_records.size() == record.data_records.size(), "inconsistent borrowed carrier record");
    }
    NativeDxfPhaseSourceAssets result;
    // Charge the validated complete descriptor/metadata copy once, rather than
    // charging a cumulative ledger again for every descriptor in the table.
    work(b, b.consumed_json_bytes - json_before + manifest.size() * sizeof(Asset));
    expected = manifest.begin();
    for (const auto& record : records) {
        const auto& row = expected->second;
        work(b, record.id.size() * 2ULL + row.media_type.size() + row.sha256.size());
        work(b, record.byte_count * 4ULL); // Initialization, chunk copy, hash, immutable freeze.
        std::vector<std::byte> bytes(static_cast<std::size_t>(record.byte_count));
        Reader reader{record.data_records};
        std::size_t offset{};
        for (std::uint64_t index = 0; index < record.chunk_count; ++index) {
            const auto pair = reader.pair();
            work(b, pair.value.size());
            const auto parts = fields<4>(pair.value);
            const auto decoded = decode_native_dxf_phase_asset_chunk(parts[3], &b);
            require(decoded.size() == chunk_size(record.byte_count, index), "decoded chunk byte count differs");
            std::copy(decoded.begin(), decoded.end(), bytes.begin() + offset);
            offset += decoded.size();
        }
        require(reader.offset == reader.text.size() && offset == bytes.size(), "incomplete decoded asset");
        require(sha256_hex(bytes) == row.sha256, "decoded asset hash differs from graph manifest");
        Asset asset{row.id, row.media_type, std::move(bytes), row.sha256, row.metadata};
        result.emplace(row.id, std::move(asset));
        ++expected;
    }
    return result;
}

std::string export_native_dxf_phase_asset_carrier(std::string_view ordinary,
    const NativeDxfPhaseSourceAssets& assets, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    require(ordinary.size() <= ordinary_byte_limit, "ordinary DXF byte limit");
    work(b, ordinary.size());
    require(!marker_scan(ordinary), "ordinary DXF contains reserved asset records");
    if (assets.empty()) { work(b, ordinary.size()); return std::string(ordinary); }
    canonical_ordinary(ordinary, b);
    validate_native_dxf_phase_source_assets(assets, &b);
    auto size = static_cast<std::uint64_t>(ordinary.size());
    output_size(size, 5 + begin_marker.size() + 5 + end_marker.size());
    std::uint64_t ordinal{};
    for (const auto& [id, asset] : assets) {
        const auto count = chunks(asset.bytes.size());
        output_size(size, 15 + digits(ordinal) + id.size() + digits(asset.bytes.size()) + digits(count) + 64);
        const auto full = asset.bytes.size() / native_dxf_phase_asset_carrier_chunk_bytes;
        const auto tail = asset.bytes.size() % native_dxf_phase_asset_carrier_chunk_bytes;
        const auto encoded = full * 224ULL + (tail + 2) / 3 * 4;
        output_size(size, count * (13 + digits(ordinal)) + index_digits(count) + encoded);
        work(b, id.size() + 64 + 80); // Header numeric formatting/temporary storage.
        ++ordinal;
    }
    // Reserve output allocation and writes before constructing any carrier.
    work(b, size * 2ULL);
    std::string result; result.reserve(static_cast<std::size_t>(size));
    result.append(ordinary.substr(0, ordinary.size() - terminal.size()));
    append_comment(result, begin_marker);
    ordinal = 0;
    for (const auto& [id, asset] : assets) {
        const auto count = chunks(asset.bytes.size());
        append_comment(result, "VXPA1 " + std::to_string(ordinal) + " " + id + " " +
            std::to_string(asset.bytes.size()) + " " + std::to_string(count) + " " + asset.sha256);
        for (std::uint64_t index = 0; index < count; ++index) {
            // Numeric formatting and concatenation can create multiple bounded
            // record temporaries in addition to the separately charged Base64.
            work(b, 80 + 2 * 255);
            const auto raw = std::span<const std::byte>(asset.bytes).subspan(
                static_cast<std::size_t>(index * native_dxf_phase_asset_carrier_chunk_bytes),
                chunk_size(asset.bytes.size(), index));
            const auto encoded = encode_native_dxf_phase_asset_chunk(raw, &b);
            append_comment(result, "VXPD1 " + std::to_string(ordinal) + " " +
                std::to_string(index) + " " + encoded);
        }
        ++ordinal;
    }
    append_comment(result, end_marker); result += terminal;
    require(result.size() == size, "carrier output size differs from admitted size");
    return result;
}
} // namespace sketch
