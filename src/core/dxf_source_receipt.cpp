#include "sketch/dxf_source_receipt.hpp"
#include "sketch/sha256_stream.hpp"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Budget = NativeDxfPhaseAssetWorkBudget;
constexpr std::string_view schema = "vertex.dxf.source-receipt.v1";
constexpr std::string_view segment_schema = "vertex.dxf.source-dependencies.v1";
constexpr std::string_view recipe_media = "application/vnd.vertex.dxf-source-recipe";
constexpr std::string_view magic = "VXDSR001";
constexpr std::string_view terminal = "0\nEOF\n";
constexpr std::string_view begin_marker = "VERTEX_PHASE_ASSETS_V1";
constexpr std::string_view end_marker = "VERTEX_PHASE_ASSETS_END_V1";

[[noreturn]] void refuse(std::string_view reason) {
    throw std::invalid_argument("Native DXF source receipt: " + std::string(reason));
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
void json_admission(Budget& b, std::uint64_t bytes, std::uint64_t nodes) {
    charge(b.consumed_json_bytes, bytes, b.max_json_bytes, "cumulative JSON byte limit");
    charge(b.consumed_json_nodes, nodes, b.max_json_nodes, "cumulative JSON node limit");
    work(b, bytes + nodes);
}
void identity(std::string_view id) {
    require(!id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "invalid asset identity");
}
void digest(std::string_view hash) {
    require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "invalid SHA-256");
}
std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
std::string_view text(const Asset& asset) {
    return {reinterpret_cast<const char*>(asset.bytes.data()), asset.bytes.size()};
}
const std::string& string(const Json& value) {
    require(value.is_string(), "receipt field must be a string");
    return value.get_ref<const std::string&>();
}
std::uint64_t number(const Json& value) {
    // Positive integers parsed from canonical JSON are unsigned. Reject floats,
    // signed in-memory alternatives, booleans and negative values explicitly.
    require(value.is_number_unsigned(), "receipt field must be an unsigned integer");
    return value.get<std::uint64_t>();
}
void fields(const Json& value, std::initializer_list<std::string_view> names) {
    require(value.is_object() && value.size() == names.size(), "invalid receipt object fields");
    for (auto name : names) require(value.contains(std::string(name)), "missing receipt field");
}
std::size_t segment_count(std::size_t count) {
    return (count + native_dxf_source_dependency_segment_limit - 1) /
        native_dxf_source_dependency_segment_limit;
}
struct Reader {
    std::string_view input;
    std::size_t offset{};
    std::string_view take(std::size_t count) {
        require(count <= input.size() - offset, "truncated recipe");
        auto value = input.substr(offset, count); offset += count; return value;
    }
    template<std::size_t N> std::uint64_t integer() {
        auto value = take(N);
        std::uint64_t result{};
        for (std::size_t i = 0; i < N; ++i)
            result |= std::uint64_t(static_cast<unsigned char>(value[i])) << (i * 8);
        return result;
    }
    std::string_view id() {
        const auto count = integer<2>();
        require(count != 0 && count <= 128, "invalid recipe identity length");
        auto result = take(static_cast<std::size_t>(count)); identity(result); return result;
    }
    std::string_view hash() { auto result = take(64); digest(result); return result; }
};
struct Header {
    std::string_view recipe_id, body_id, original_hash, body_hash;
    std::uint64_t original_size{}, body_size{};
    std::size_t count{}, segments{}, rows_offset{};
};
struct Row { std::string_view original_id, fresh_id, hash; std::uint64_t size{}; };
Header header(Reader& reader, const Budget& b) {
    require(reader.input.size() <= b.max_payload_bytes, "recipe asset byte limit");
    require(reader.take(magic.size()) == magic, "unsupported recipe schema/version");
    Header h;
    h.recipe_id = reader.id(); h.body_id = reader.id();
    require(h.recipe_id != h.body_id, "receipt asset identities overlap");
    h.original_size = reader.integer<8>(); h.original_hash = reader.hash();
    h.body_size = reader.integer<8>(); h.body_hash = reader.hash();
    h.count = static_cast<std::size_t>(reader.integer<4>());
    h.segments = static_cast<std::size_t>(reader.integer<4>());
    h.rows_offset = reader.offset;
    require(h.original_size <= native_dxf_phase_asset_transport_byte_limit &&
        h.body_size <= native_dxf_source_ordinary_byte_limit &&
        h.body_size >= terminal.size() && h.original_size > h.body_size,
        "invalid original/body capacity");
    require(h.count != 0 && h.count <= native_dxf_phase_destination_asset_count_limit &&
        h.segments == segment_count(h.count), "invalid recipe inventory/segment count");
    // Admit the advertised count against physical bytes before set allocations.
    require(h.count <= (reader.input.size() - reader.offset) / 78,
        "recipe count exceeds physical rows");
    return h;
}
template<class Callback> void rows(std::string_view input, const Header& h,
    const Budget& b, Callback callback) {
    Reader reader{input, h.rows_offset};
    std::string_view previous;
    std::uint64_t total{};
    for (std::size_t index = 0; index < h.count; ++index) {
        Row row;
        row.original_id = reader.id(); row.fresh_id = reader.id();
        row.size = reader.integer<8>(); row.hash = reader.hash();
        require(previous.empty() || previous < row.original_id,
            "recipe original identities must be sorted and unique");
        previous = row.original_id;
        require(row.original_id != row.fresh_id && row.fresh_id != h.recipe_id &&
            row.fresh_id != h.body_id, "payload identity is not fresh or overlaps receipt");
        require(row.size <= b.max_payload_bytes - total, "complete source payload byte limit");
        total += row.size;
        callback(row, index);
    }
    require(reader.offset == input.size(), "unknown or trailing recipe fields");
}
template<std::size_t N> void put(std::vector<std::byte>& output, std::uint64_t value) {
    for (std::size_t i = 0; i < N; ++i)
        output.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
}
void put(std::vector<std::byte>& output, std::string_view value) {
    output.insert(output.end(), bytes(value).begin(), bytes(value).end());
}
void put_id(std::vector<std::byte>& output, std::string_view id) {
    put<2>(output, id.size()); put(output, id);
}
const Asset& lookup(const NativeDxfPhaseSourceAssetRefs& assets, std::string_view id) {
    const auto found = assets.find(id);
    require(found != assets.end() && found->second != nullptr && found->second->id == id,
        "missing retained asset or mismatched actual identity");
    return *found->second;
}
void actual(const Asset& asset, std::uint64_t size, std::string_view hash, Budget& b,
    bool hash_verified = false) {
    require(asset.bytes.size() == size && size <= b.max_payload_bytes && asset.sha256 == hash,
        "retained asset differs from pinned descriptor");
    if (!hash_verified) {
        work(b, size * 2ULL); // Shared streaming hash stages and hashes each byte.
        require(asset.bytes.verified_sha256() == hash, "retained payload differs from pinned hash");
    }
}
std::uint64_t chunks(std::uint64_t size) {
    return size == 0 ? 1 : (size - 1) / native_dxf_phase_asset_carrier_chunk_bytes + 1;
}
std::size_t chunk_size(std::uint64_t size, std::uint64_t index) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        native_dxf_phase_asset_carrier_chunk_bytes,
        size - index * native_dxf_phase_asset_carrier_chunk_bytes));
}
// All temporary strings are bounded physical carrier records. The caller can
// compare directly against borrowed original bytes or append to admitted output.
template<class Emit> void asset_records(std::string_view original_id, const Asset& asset,
    std::uint64_t ordinal, Budget& b, Emit emit) {
    const auto count = chunks(asset.bytes.size());
    work(b, original_id.size() + 256);
    const auto first = "999\nVXPA1 " + std::to_string(ordinal) + " " + std::string(original_id) +
        " " + std::to_string(asset.bytes.size()) + " " + std::to_string(count) + " " + asset.sha256 + "\n";
    require(first.size() <= 260, "asset header physical record limit"); emit(first);
    for (std::uint64_t index = 0; index < count; ++index) {
        work(b, 80 + 2 * 255);
        const auto raw = std::span<const std::byte>(asset.bytes.data(), asset.bytes.size()).subspan(
            static_cast<std::size_t>(index * native_dxf_phase_asset_carrier_chunk_bytes),
            chunk_size(asset.bytes.size(), index));
        const auto encoded = encode_native_dxf_phase_asset_chunk(raw, &b);
        const auto record = "999\nVXPD1 " + std::to_string(ordinal) + " " + std::to_string(index) +
            " " + encoded + "\n";
        require(record.size() <= 260, "asset data physical record limit"); emit(record);
    }
}
std::uint64_t digits(std::uint64_t value) {
    std::uint64_t count = 1; while (value >= 10) { value /= 10; ++count; } return count;
}
std::uint64_t index_digits(std::uint64_t count) {
    std::uint64_t total = count;
    for (std::uint64_t threshold = 10; threshold < count; threshold *= 10) total += count - threshold;
    return total;
}
void add_output(std::uint64_t& size, std::uint64_t amount) {
    require(amount <= native_dxf_phase_asset_transport_byte_limit - size, "original transport byte limit");
    size += amount;
}
std::uint64_t row_output_size(std::string_view id, std::uint64_t size, std::uint64_t ordinal) {
    const auto count = chunks(size);
    return 15 + digits(ordinal) + id.size() + digits(size) + digits(count) + 64 +
        count * (13 + digits(ordinal)) + index_digits(count) +
        size / native_dxf_phase_asset_carrier_chunk_bytes * 224ULL +
        ((size % native_dxf_phase_asset_carrier_chunk_bytes) + 2) / 3 * 4;
}
void ordinary(std::string_view body, Budget& b) {
    require(body.size() <= native_dxf_source_ordinary_byte_limit && body.ends_with(terminal),
        "ordinary DXF capacity/terminal mismatch");
    work(b, body.size() * 2ULL);
    require(body.find('\r') == std::string_view::npos &&
        !native_dxf_phase_asset_carrier_present(body), "ordinary DXF has noncanonical or reserved records");
    std::size_t offset{};
    std::string_view last_code, last_value;
    while (offset < body.size()) {
        auto end = body.find('\n', offset);
        require(end != std::string_view::npos, "ordinary DXF incomplete code framing");
        last_code = body.substr(offset, end - offset); offset = end + 1;
        end = body.find('\n', offset);
        require(end != std::string_view::npos, "ordinary DXF incomplete value framing");
        last_value = body.substr(offset, end - offset); offset = end + 1;
    }
    require(last_code == "0" && last_value == "EOF", "ordinary DXF terminal pair mismatch");
}

struct Validated { Header h; const Asset* recipe{}; const Asset* body{}; };
Validated validate(const Json& properties, NativeDxfSourceDependencyRefs segments,
    const NativeDxfPhaseSourceAssetRefs& retained, Budget& b) {
    require(properties.is_object() && properties.contains("source_receipt") &&
        properties.contains("asset_ids"), "missing source receipt envelope/references");
    const auto& value = properties.at("source_receipt");
    fields(value, {"schema", "version", "ordinary_asset_id", "ordinary_byte_count", "ordinary_sha256",
        "recipe_asset_id", "recipe_byte_count", "recipe_sha256", "original_byte_count", "original_sha256",
        "asset_count", "dependency_segment_count"});
    require(string(value.at("schema")) == schema && number(value.at("version")) == 1,
        "unsupported source receipt schema/version");
    const auto& body_id = string(value.at("ordinary_asset_id")); identity(body_id);
    const auto& recipe_id = string(value.at("recipe_asset_id")); identity(recipe_id);
    const auto& body_hash = string(value.at("ordinary_sha256")); digest(body_hash);
    const auto& recipe_hash = string(value.at("recipe_sha256")); digest(recipe_hash);
    const auto& original_hash = string(value.at("original_sha256")); digest(original_hash);
    const auto body_size = number(value.at("ordinary_byte_count"));
    const auto recipe_size = number(value.at("recipe_byte_count"));
    const auto original_size = number(value.at("original_byte_count"));
    const auto count = number(value.at("asset_count"));
    const auto segment_size = number(value.at("dependency_segment_count"));
    require(body_id != recipe_id && body_size <= native_dxf_source_ordinary_byte_limit &&
        recipe_size <= b.max_payload_bytes && original_size <= native_dxf_phase_asset_transport_byte_limit &&
        count != 0 && count <= native_dxf_phase_destination_asset_count_limit &&
        segment_size == segment_count(static_cast<std::size_t>(count)) && segments.size() == segment_size &&
        retained.size() == count + 2, "invalid source receipt capacity or incomplete dependencies");
    for (const auto* segment : segments) require(segment != nullptr, "null dependency segment");
    json_admission(b, 1024 + body_id.size() + recipe_id.size(), 18);
    const auto& refs = properties.at("asset_ids");
    require(refs.is_array() && refs.size() == 2 && string(refs[0]) == body_id &&
        string(refs[1]) == recipe_id, "main receipt asset references differ");
    const auto& recipe = lookup(retained, recipe_id);
    const auto& body = lookup(retained, body_id);
    require(recipe.media_type == recipe_media && body.media_type == "application/dxf",
        "receipt asset media type differs");
    actual(recipe, recipe_size, recipe_hash, b);
    actual(body, body_size, body_hash, b);
    work(b, recipe_size * 2);
    Reader reader{text(recipe)};
    auto h = header(reader, b);
    require(h.recipe_id == recipe_id && h.body_id == body_id && h.body_size == body_size &&
        h.body_hash == body_hash && h.original_size == original_size && h.original_hash == original_hash &&
        h.count == count && h.segments == segment_size, "recipe differs from pinned source envelope");
    // First pass admits all physical recipe rows and length totals before the
    // uniqueness set or any reconstruction output allocation.
    rows(text(recipe), h, b, [](const Row&, std::size_t) {});
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = *segments[i];
        fields(segment, {"schema", "version", "recipe_asset_id", "segment_index", "segment_count", "asset_ids"});
        const auto& ids = segment.at("asset_ids");
        const auto expected = std::min(native_dxf_source_dependency_segment_limit,
            h.count - i * native_dxf_source_dependency_segment_limit);
        require(string(segment.at("schema")) == segment_schema && number(segment.at("version")) == 1 &&
            string(segment.at("recipe_asset_id")) == recipe_id &&
            number(segment.at("segment_index")) == i && number(segment.at("segment_count")) == h.segments &&
            ids.is_array() && ids.size() == expected, "malformed or incomplete dependency segment");
        json_admission(b, 256 + recipe_id.size(), 8);
        for (const auto& id : ids) {
            const auto& raw = string(id); identity(raw);
            json_admission(b, raw.size() + 3, 1);
        }
    }
    // Actual retained inventory includes body/recipe overhead in addition to
    // the source payload. Admit this lane at the native destination's 512 MiB
    // aggregate, retaining the independent 256 MiB limit for every Asset.
    validate_native_dxf_phase_destination_asset_refs(retained, &b);
    work(b, h.count * (sizeof(std::string_view) + 64ULL));
    std::set<std::string_view, std::less<>> unique;
    std::uint64_t reconstructed_size = h.body_size;
    add_output(reconstructed_size, 5 + begin_marker.size() + 5 + end_marker.size());
    rows(text(recipe), h, b, [&](const Row& row, std::size_t index) {
        require(unique.emplace(row.fresh_id).second, "duplicate retained payload mapping");
        const auto& ids = segments[index / native_dxf_source_dependency_segment_limit]->at("asset_ids");
        require(string(ids[index % native_dxf_source_dependency_segment_limit]) == row.fresh_id,
            "dependency reference differs from recipe mapping");
        actual(lookup(retained, row.fresh_id), row.size, row.hash, b, true);
        add_output(reconstructed_size, row_output_size(row.original_id, row.size, index));
    });
    require(reconstructed_size == h.original_size, "recipe cannot reconstruct pinned original length");
    ordinary(text(body), b);
    // A newly authored, internally consistent receipt must also prove its
    // claimed full original digest. Stream bounded canonical records into the
    // shared hash helper; never allocate the full Base64 carrier for admission.
    work(b, h.original_size * 2ULL); // Stream-buffer copy and full output hash.
    const auto reconstructed_hash = sha256_hex_stream([&](const Sha256Sink& sink) {
        std::uint64_t emitted{};
        const auto emit = [&](std::string_view part) {
            require(part.size() <= h.original_size - emitted, "stream exceeds pinned original length");
            sink(bytes(part)); emitted += part.size();
        };
        const auto ordinary_body = text(body);
        emit(ordinary_body.substr(0, ordinary_body.size() - terminal.size()));
        emit("999\n"); emit(begin_marker); emit("\n");
        rows(text(recipe), h, b, [&](const Row& row, std::size_t index) {
            asset_records(row.original_id, lookup(retained, row.fresh_id), index, b, emit);
        });
        emit("999\n"); emit(end_marker); emit("\n"); emit(terminal);
        require(emitted == h.original_size, "stream differs from pinned original length");
    });
    require(reconstructed_hash == h.original_hash, "canonical original differs from pinned SHA-256");
    return {h, &recipe, &body};
}
} // namespace

NativeDxfSourceReceipt create_native_dxf_source_receipt(std::string_view original,
    const NativeDxfPhaseSourceAssets& source_assets, const NativeDxfSourceAssetMapping& mapping,
    const NativeDxfPhaseSourceAssetRefs& retained, std::string body_id, std::string recipe_id, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    identity(body_id); identity(recipe_id);
    require(body_id != recipe_id && !retained.contains(body_id) && !retained.contains(recipe_id),
        "receipt asset identities overlap retained payloads");
    require(!source_assets.empty() && source_assets.size() <= native_dxf_phase_destination_asset_count_limit &&
        mapping.size() == source_assets.size() && retained.size() == source_assets.size(),
        "missing or orphan source/fresh mapping");
    auto carrier = split_native_dxf_phase_asset_carrier(original, &b);
    require(carrier.has_assets && carrier.records.size() == source_assets.size(),
        "receipt requires complete asset-bearing original");
    validate_native_dxf_phase_source_assets(source_assets, &b);
    ordinary(carrier.ordinary_dxf, b);
    work(b, mapping.size() * (sizeof(std::string_view) + 64ULL));
    std::set<std::string_view, std::less<>> unique;
    std::uint64_t recipe_size = magic.size() + 4 + body_id.size() + recipe_id.size() + 16 + 128 + 8;
    require(recipe_size <= b.max_payload_bytes, "recipe asset byte limit");
    auto mapped = mapping.begin();
    std::size_t ordinal{};
    // Authenticate every actual source payload against its physical original
    // records; no decoded duplicate inventory or full Base64 copy is created.
    for (const auto& [id, asset] : source_assets) {
        require(mapped->first == id, "source mapping identities differ");
        const auto& fresh_id = mapped->second; identity(fresh_id);
        require(fresh_id != id && unique.emplace(fresh_id).second,
            "source mapping is not fresh and one-to-one");
        const auto& destination = lookup(retained, fresh_id);
        actual(destination, asset.bytes.size(), asset.sha256, b);
        const auto& record = carrier.records[ordinal];
        require(record.id == id && record.byte_count == asset.bytes.size() && record.sha256 == asset.sha256,
            "original carrier differs from actual source inventory");
        std::size_t offset{};
        bool first = true;
        asset_records(id, asset, ordinal, b, [&](std::string_view part) {
            if (first) { first = false; return; } // split authenticated canonical header fields.
            require(part.size() <= record.data_records.size() - offset,
                "original carrier data length differs from actual payload");
            work(b, part.size());
            require(record.data_records.substr(offset, part.size()) == part,
                "original carrier payload differs from actual source bytes");
            offset += part.size();
        });
        require(offset == record.data_records.size(), "orphan original carrier data");
        const auto amount = 4 + id.size() + fresh_id.size() + 8 + 64;
        require(amount <= b.max_payload_bytes - recipe_size, "recipe asset byte limit");
        recipe_size += amount;
        ++mapped; ++ordinal;
    }
    require(recipe_size <= b.max_payload_bytes, "recipe asset byte limit");
    work(b, original.size() * 2ULL);
    const auto original_hash = sha256_hex(bytes(original));
    work(b, carrier.ordinary_dxf.size() * 3ULL);
    std::vector<std::byte> body_bytes(bytes(carrier.ordinary_dxf).begin(), bytes(carrier.ordinary_dxf).end());
    Asset body = Asset::create(std::move(body_id), "application/dxf", std::move(body_bytes));
    work(b, recipe_size * 3);
    std::vector<std::byte> recipe_bytes;
    recipe_bytes.reserve(static_cast<std::size_t>(recipe_size));
    put(recipe_bytes, magic); put_id(recipe_bytes, recipe_id); put_id(recipe_bytes, body.id);
    put<8>(recipe_bytes, original.size()); put(recipe_bytes, original_hash);
    put<8>(recipe_bytes, body.bytes.size()); put(recipe_bytes, body.sha256);
    put<4>(recipe_bytes, source_assets.size()); put<4>(recipe_bytes, segment_count(source_assets.size()));
    for (const auto& [id, asset] : source_assets) {
        put_id(recipe_bytes, id); put_id(recipe_bytes, mapping.at(id));
        put<8>(recipe_bytes, asset.bytes.size()); put(recipe_bytes, asset.sha256);
    }
    require(recipe_bytes.size() == recipe_size, "recipe size differs from admitted bytes");
    Asset recipe = Asset::create(std::move(recipe_id), std::string(recipe_media), std::move(recipe_bytes));
    json_admission(b, 1024 + body.id.size() + recipe.id.size(), 18);
    Json properties = {{"asset_ids", Json::array({body.id, recipe.id})}, {"source_receipt", {
        {"schema", schema}, {"version", std::uint64_t{1}},
        {"ordinary_asset_id", body.id}, {"ordinary_byte_count", std::uint64_t(body.bytes.size())},
        {"ordinary_sha256", body.sha256}, {"recipe_asset_id", recipe.id},
        {"recipe_byte_count", std::uint64_t(recipe.bytes.size())}, {"recipe_sha256", recipe.sha256},
        {"original_byte_count", std::uint64_t(original.size())}, {"original_sha256", original_hash},
        {"asset_count", std::uint64_t(source_assets.size())},
        {"dependency_segment_count", std::uint64_t(segment_count(source_assets.size()))}}}};
    std::vector<Json> dependencies;
    work(b, segment_count(source_assets.size()) * sizeof(Json));
    dependencies.reserve(segment_count(source_assets.size()));
    ordinal = 0;
    for (const auto& [id, fresh_id] : mapping) {
        (void)id;
        if (ordinal % native_dxf_source_dependency_segment_limit == 0) {
            json_admission(b, 256 + recipe.id.size(), 8);
            dependencies.push_back({{"schema", segment_schema}, {"version", std::uint64_t{1}},
                {"recipe_asset_id", recipe.id},
                {"segment_index", std::uint64_t(dependencies.size())},
                {"segment_count", std::uint64_t(segment_count(source_assets.size()))},
                {"asset_ids", Json::array()}});
        }
        json_admission(b, fresh_id.size() + 3, 1);
        dependencies.back()["asset_ids"].push_back(fresh_id);
        ++ordinal;
    }
    return {std::move(body), std::move(recipe), std::move(properties), std::move(dependencies)};
}

void validate_native_dxf_source_receipt(const Json& properties, NativeDxfSourceDependencyRefs dependencies,
    const NativeDxfPhaseSourceAssetRefs& retained, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    (void)validate(properties, dependencies, retained, b);
}

std::string reconstruct_native_dxf_source_receipt(const Json& properties,
    NativeDxfSourceDependencyRefs dependencies, const NativeDxfPhaseSourceAssetRefs& retained, Budget* budget) {
    Budget local; auto& b = budget ? *budget : local; limits(b);
    const auto admitted = validate(properties, dependencies, retained, b);
    const auto& h = admitted.h;
    work(b, h.original_size * 3ULL); // Output allocation, writes and final full hash.
    std::string output;
    output.reserve(static_cast<std::size_t>(h.original_size));
    const auto body = text(*admitted.body);
    output.append(body.substr(0, body.size() - terminal.size()));
    const auto append = [&](std::string_view part) {
        require(part.size() <= h.original_size - output.size(), "reconstruction exceeds pinned length");
        output.append(part);
    };
    append("999\n"); append(begin_marker); append("\n");
    rows(text(*admitted.recipe), h, b, [&](const Row& row, std::size_t index) {
        asset_records(row.original_id, lookup(retained, row.fresh_id), index, b, append);
    });
    append("999\n"); append(end_marker); append("\n"); append(terminal);
    require(output.size() == h.original_size && sha256_hex(bytes(output)) == h.original_hash,
        "reconstructed original length or SHA-256 differs");
    return output;
}
} // namespace sketch
