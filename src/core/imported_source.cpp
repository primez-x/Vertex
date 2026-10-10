#include "sketch/imported_source.hpp"

#include "sketch/dxf_source_receipt.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::string_view dependency_schema = "vertex.dxf.source-dependencies.v1";
constexpr std::string_view receipt_schema = "vertex.dxf.source-receipt.v1";

[[noreturn]] void refuse(std::string_view reason) {
    throw std::invalid_argument("Imported original source: " + std::string(reason));
}
void require(bool value, std::string_view reason) { if (!value) refuse(reason); }

struct MetadataBudget {
    std::uint64_t bytes{};
    std::size_t nodes{}, references{};
    void text(std::string_view value, std::size_t limit = imported_source_json_byte_limit) {
        require(value.size() <= limit && value.find('\0') == std::string_view::npos,
            "invalid or excessive metadata string");
        require(value.size() <= imported_source_json_byte_limit - bytes,
            "cumulative metadata byte limit");
        bytes += value.size();
    }
    void inspect(const Json& value, std::size_t depth = 0, bool omit_diagnostics = false) {
        require(depth <= imported_source_json_depth_limit && nodes < imported_source_json_node_limit,
            "metadata complexity limit");
        ++nodes;
        require(bytes < imported_source_json_byte_limit, "cumulative metadata byte limit");
        ++bytes;
        require(!value.is_binary() && !value.is_discarded(), "nonportable source metadata");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_number_float())
            require(std::isfinite(value.get<double>()), "nonfinite source metadata number");
        else if (value.is_array()) {
            require(value.size() <= imported_source_json_node_limit - nodes, "metadata array limit");
            for (const auto& item : value) inspect(item, depth + 1);
        } else if (value.is_object()) {
            require(value.size() <= imported_source_json_node_limit - nodes, "metadata object limit");
            for (auto item = value.begin(); item != value.end(); ++item) {
                text(item.key());
                // Only the owner's top-level log is separate from its descriptor.
                // Nested metadata and receipt/dependency records remain inspected.
                if (!omit_diagnostics || item.key() != "diagnostics") inspect(item.value(), depth + 1);
            }
        }
    }
    void reference(std::size_t count = 1) {
        require(count <= imported_source_reference_limit - references, "source reference limit");
        references += count;
    }
};
struct DiagnosticBudget {
    // Document admits at most 100,000 JSON values per properties/extensions
    // object; Pinc separately bounds diagnostic rows and string payload. These
    // are owner-local budgets, independent of the descriptor inventory ledger.
    std::uint64_t string_bytes{};
    std::size_t nodes{};
    void inspect(const Json& value, std::size_t depth = 0) {
        require(depth <= imported_source_json_depth_limit && nodes < imported_source_diagnostic_node_limit,
            "diagnostic complexity limit");
        ++nodes;
        require(!value.is_binary() && !value.is_discarded(), "nonportable source diagnostic");
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            require(text.find('\0') == std::string::npos &&
                text.size() <= imported_source_diagnostic_string_byte_limit - string_bytes,
                "diagnostic string byte limit");
            string_bytes += text.size();
        } else if (value.is_number_float()) {
            require(std::isfinite(value.get<double>()), "nonfinite source diagnostic number");
        } else if (value.is_array()) {
            require(value.size() <= imported_source_diagnostic_node_limit - nodes, "diagnostic array limit");
            for (const auto& item : value) inspect(item, depth + 1);
        } else if (value.is_object()) {
            require(value.size() <= imported_source_diagnostic_node_limit - nodes, "diagnostic object limit");
            for (auto item = value.begin(); item != value.end(); ++item) {
                // Document JSON keys have a separate 128-byte ceiling. The Pinc
                // producer's string budget charges values, not repeated keys.
                require(item.key().size() <= 128 && item.key().find('\0') == std::string::npos,
                    "invalid diagnostic key");
                inspect(item.value(), depth + 1);
            }
        }
    }
};
const std::string& string(const Json& value, std::size_t limit, std::string_view reason) {
    require(value.is_string(), reason);
    const auto& result = value.get_ref<const std::string&>();
    require(result.size() <= limit && result.find('\0') == std::string::npos, reason);
    return result;
}
std::string_view identity(const Json& value) {
    const auto& id = string(value, 128, "invalid source identity");
    require(!id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid source identity");
    return id;
}
void owner_identity(std::string_view id) {
    require(!id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid owner identity");
}
std::string_view digest(const Json& value) {
    const auto& hash = string(value, 64, "invalid source SHA-256");
    require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "invalid source SHA-256");
    return hash;
}
void asset_descriptor(const Asset& asset, std::string_view id, std::string_view media) {
    require(asset.id == id && asset.media_type == media &&
        asset.bytes.size() <= native_dxf_phase_asset_payload_limit, "source asset descriptor differs");
    require(asset.sha256.size() == 64 && std::all_of(asset.sha256.begin(), asset.sha256.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), "invalid asset SHA-256");
}
std::uint64_t number(const Json& value) {
    require(value.is_number_unsigned(), "source receipt number must be unsigned");
    return value.get<std::uint64_t>();
}
void fields(const Json& value, std::initializer_list<std::string_view> names) {
    require(value.is_object() && value.size() == names.size(), "invalid source metadata fields");
    for (const auto name : names) require(value.contains(std::string(name)), "missing source metadata field");
}
const Json& member(const Json& value, const char* name) {
    require(value.is_object() && value.contains(name), "missing source metadata field");
    return value.at(name);
}
const Asset& asset(const DocumentSnapshot& snapshot, std::string_view id) {
    const auto found = snapshot.assets().find(id);
    require(found != snapshot.assets().end() && found->second.id == id, "missing exact source asset");
    return found->second;
}
const Json& diagnostics(const Json& properties) {
    const auto& result = member(properties, "diagnostics");
    require(result.is_array() && result.size() <= imported_source_diagnostic_record_limit,
        "invalid source fidelity diagnostics");
    return result;
}

struct Owner {
    const Entity* entity{};
    ImportedSourceFormat format{};
    const Asset* direct{};
    const Json* diagnostics{};
    std::string_view name, hash, recipe_id;
    std::uint64_t size{}, payload_count{}, segment_count{};
    std::vector<const Json*> dependencies;
};
using SegmentGroup = std::map<std::uint64_t, const Json*>;
struct Inventory {
    std::map<std::string_view, Owner, std::less<>> owners;
    std::map<std::string_view, std::string_view, std::less<>> recipes;
    std::map<std::string_view, SegmentGroup, std::less<>> segments;
    MetadataBudget budget;
};

void admit_receipt(Owner& owner) {
    const auto& properties = owner.entity->properties;
    require(!properties.contains("asset_id") && !properties.contains("schema"),
        "ambiguous DXF original owner");
    const auto& envelope = member(properties, "source_receipt");
    fields(envelope, {"schema", "version", "ordinary_asset_id", "ordinary_byte_count", "ordinary_sha256",
        "recipe_asset_id", "recipe_byte_count", "recipe_sha256", "original_byte_count", "original_sha256",
        "asset_count", "dependency_segment_count"});
    require(string(envelope.at("schema"), 64, "invalid source receipt schema") == receipt_schema &&
        number(envelope.at("version")) == 1, "unsupported source receipt schema/version");
    const auto body_id = identity(envelope.at("ordinary_asset_id"));
    owner.recipe_id = identity(envelope.at("recipe_asset_id"));
    require(body_id != owner.recipe_id, "overlapping receipt identities");
    (void)digest(envelope.at("ordinary_sha256"));
    (void)digest(envelope.at("recipe_sha256"));
    owner.hash = digest(envelope.at("original_sha256"));
    owner.size = number(envelope.at("original_byte_count"));
    owner.payload_count = number(envelope.at("asset_count"));
    owner.segment_count = number(envelope.at("dependency_segment_count"));
    const auto body_size = number(envelope.at("ordinary_byte_count"));
    require(body_size >= 6 && body_size <= native_dxf_source_ordinary_byte_limit &&
        number(envelope.at("recipe_byte_count")) <= native_dxf_phase_asset_payload_limit &&
        owner.size > body_size && owner.size <= native_dxf_phase_asset_transport_byte_limit &&
        owner.payload_count != 0 && owner.payload_count <= native_dxf_phase_destination_asset_count_limit &&
        owner.segment_count == (owner.payload_count + native_dxf_source_dependency_segment_limit - 1) /
            native_dxf_source_dependency_segment_limit, "invalid source receipt capacity");
    const auto& refs = member(properties, "asset_ids");
    require(refs.is_array() && refs.size() == 2 && identity(refs[0]) == body_id &&
        identity(refs[1]) == owner.recipe_id, "source receipt references differ");
}

Inventory discover(const DocumentSnapshot& snapshot) {
    require(snapshot.entities().size() <= imported_source_scan_limit &&
        snapshot.assets().size() <= imported_source_scan_limit, "current-state source scan limit");
    Inventory result;
    for (const auto& [id, entity] : snapshot.entities()) {
        const bool dxf = entity.type == "dxf_source", ifc = entity.type == "ifc_source";
        const bool pinc = entity.type == "sheet_view_model" && entity.extensions.contains("pinc_import");
        if (!dxf && !ifc && !pinc) continue;
        owner_identity(id);
        require(entity.id == id, "source owner identity differs from current entity key");
        const auto& properties = pinc ? entity.extensions.at("pinc_import") : entity.properties;
        require(properties.is_object(), "source owner metadata is not an object");
        result.budget.text(id, 128);
        result.budget.inspect(properties, 0, !dxf || !properties.contains("schema"));
        if (dxf && properties.contains("schema")) {
            fields(properties, {"schema", "version", "recipe_asset_id", "segment_index", "segment_count", "asset_ids"});
            require(string(properties.at("schema"), 64, "invalid dependency schema") == dependency_schema &&
                number(properties.at("version")) == 1, "unsupported source dependency schema/version");
            const auto recipe = identity(properties.at("recipe_asset_id"));
            const auto index = number(properties.at("segment_index"));
            const auto count = number(properties.at("segment_count"));
            const auto& refs = properties.at("asset_ids");
            constexpr auto max_segments = (native_dxf_phase_destination_asset_count_limit +
                native_dxf_source_dependency_segment_limit - 1) / native_dxf_source_dependency_segment_limit;
            require(count != 0 && count <= max_segments && index < count && refs.is_array() &&
                !refs.empty() && refs.size() <= native_dxf_source_dependency_segment_limit,
                "invalid source dependency capacity");
            require(result.segments[recipe].emplace(index, &properties).second,
                "duplicate source dependency ordinal");
            continue;
        }
        Owner owner;
        owner.entity = &entity;
        owner.format = dxf ? ImportedSourceFormat::dxf : ifc ? ImportedSourceFormat::ifc : ImportedSourceFormat::pinc;
        owner.diagnostics = &diagnostics(properties);
        if (pinc) {
            const auto& version = member(properties, "version");
            require(version.is_number_integer() && version == 1, "unsupported Pinc import version");
            const auto& pages = member(properties, "pages");
            require(pages.is_array() && !pages.empty() && pages.size() <= 128, "invalid Pinc import pages");
            const auto source_id = identity(member(properties, "source_asset_id"));
            owner.direct = &asset(snapshot, source_id);
            asset_descriptor(*owner.direct, source_id, "application/x-pincsketch");
            result.budget.inspect(owner.direct->metadata);
            require(string(member(owner.direct->metadata, "content"), 64, "invalid Pinc source content") ==
                "pinc-project-original", "Pinc owner does not reference an original project");
            owner.name = string(member(owner.direct->metadata, "source_path"), imported_source_name_byte_limit,
                "invalid original source name");
        } else {
            owner.name = string(member(properties, "source_path"), imported_source_name_byte_limit,
                "invalid original source name");
            (void)string(member(properties, "format"), 128, "invalid imported source format");
            if (dxf && properties.contains("source_receipt")) {
                admit_receipt(owner);
                // Admit the complete advertised reference inventory before any
                // per-owner ordered vectors or uniqueness-set allocations.
                result.budget.reference(static_cast<std::size_t>(owner.payload_count) + 2);
                require(result.recipes.emplace(owner.recipe_id, id).second, "duplicate original receipt recipe");
            } else {
                require(!properties.contains("source_receipt") && !properties.contains("asset_ids"),
                    "ambiguous original source references");
                const auto source_id = identity(member(properties, "asset_id"));
                owner.direct = &asset(snapshot, source_id);
                asset_descriptor(*owner.direct, source_id, dxf ? "application/dxf" : "application/step");
            }
        }
        if (owner.direct) {
            result.budget.reference();
            owner.hash = owner.direct->sha256;
            owner.size = owner.direct->bytes.size();
        }
        result.owners.emplace(id, std::move(owner));
    }
    // Every known segment must have one actual owner. Group counts and ordinals
    // are proved before allocating each owner's ordered dependency span.
    for (const auto& [recipe, group] : result.segments) {
        const auto found = result.recipes.find(recipe);
        require(found != result.recipes.end(), "source dependency has no original owner");
        auto& owner = result.owners.at(found->second);
        require(group.size() == owner.segment_count, "incomplete original source dependencies");
        std::uint64_t next{};
        for (const auto& [ordinal, properties] : group) {
            require(ordinal == next++ && number(properties->at("segment_count")) == owner.segment_count,
                "inconsistent original source dependency ordinals/counts");
            const auto expected = std::min<std::uint64_t>(native_dxf_source_dependency_segment_limit,
                owner.payload_count - ordinal * native_dxf_source_dependency_segment_limit);
            require(properties->at("asset_ids").size() == expected, "incomplete source dependency references");
        }
        owner.dependencies.reserve(group.size());
        for (const auto& [ordinal, properties] : group) { (void)ordinal; owner.dependencies.push_back(properties); }
    }
    for (const auto& [id, owner] : result.owners) {
        (void)id;
        if (owner.direct) continue;
        require(owner.dependencies.size() == owner.segment_count, "missing original source dependencies");
        std::set<std::string_view, std::less<>> unique;
        const auto collect = [&](const Json& properties) {
            for (const auto& value : properties.at("asset_ids")) {
                const auto raw_id = identity(value);
                require(unique.emplace(raw_id).second, "repeated original source dependency reference");
                const auto& retained = asset(snapshot, raw_id);
                require(retained.bytes.size() <= native_dxf_phase_asset_payload_limit &&
                    retained.sha256.size() == 64 && std::all_of(retained.sha256.begin(), retained.sha256.end(),
                        [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
                    "invalid original source dependency descriptor");
            }
        };
        collect(owner.entity->properties);
        for (const auto* properties : owner.dependencies) collect(*properties);
        require(unique.size() == owner.payload_count + 2, "original source dependency inventory differs");
        const auto& envelope = owner.entity->properties.at("source_receipt");
        const auto& body = asset(snapshot, identity(envelope.at("ordinary_asset_id")));
        const auto& recipe = asset(snapshot, owner.recipe_id);
        asset_descriptor(body, body.id, "application/dxf");
        asset_descriptor(recipe, owner.recipe_id, "application/vnd.vertex.dxf-source-recipe");
        require(body.bytes.size() == number(envelope.at("ordinary_byte_count")) &&
            body.sha256 == digest(envelope.at("ordinary_sha256")) &&
            recipe.bytes.size() == number(envelope.at("recipe_byte_count")) &&
            recipe.sha256 == digest(envelope.at("recipe_sha256")), "retained receipt descriptors differ");
    }
    return result;
}
const Owner& selected(const Inventory& inventory, std::string_view owner_id) {
    owner_identity(owner_id);
    const auto found = inventory.owners.find(owner_id);
    require(found != inventory.owners.end(), "exact original source owner is unavailable");
    return found->second;
}
} // namespace

std::vector<ImportedSourceInfo> list_imported_sources(const DocumentSnapshot& snapshot) {
    const auto inventory = discover(snapshot);
    std::vector<ImportedSourceInfo> result;
    result.reserve(inventory.owners.size());
    for (const auto& [id, owner] : inventory.owners)
        result.push_back({std::string(id), owner.format, std::string(owner.name), owner.size,
            std::string(owner.hash), owner.diagnostics->size(), owner.direct == nullptr});
    return result;
}

void stream_imported_source(const DocumentSnapshot& snapshot, std::string_view owner_id,
    const std::function<void(std::span<const std::byte>)>& sink) {
    require(static_cast<bool>(sink), "missing original source sink");
    const auto inventory = discover(snapshot);
    const auto& owner = selected(inventory, owner_id);
    if (owner.direct) {
        require(owner.direct->bytes.verified_sha256() == owner.hash, "original source bytes differ from SHA-256");
        auto remaining = std::span<const std::byte>(owner.direct->bytes.data(), owner.direct->bytes.size());
        while (!remaining.empty()) {
            const auto count = std::min(remaining.size(), imported_source_stream_chunk_bytes);
            sink(remaining.first(count));
            remaining = remaining.subspan(count);
        }
        return;
    }
    NativeDxfPhaseSourceAssetRefs retained;
    const auto collect = [&](const Json& properties) {
        for (const auto& value : properties.at("asset_ids")) {
            const auto& exact_id = value.get_ref<const std::string&>();
            retained.emplace(exact_id, &asset(snapshot, exact_id));
        }
    };
    collect(owner.entity->properties);
    for (const auto* properties : owner.dependencies) collect(*properties);
    stream_native_dxf_source_receipt(owner.entity->properties, owner.dependencies, retained, sink);
}

Json imported_source_diagnostics(const DocumentSnapshot& snapshot, std::string_view owner_id,
    std::size_t offset, std::size_t limit) {
    require(limit != 0 && limit <= imported_source_diagnostic_page_limit, "invalid diagnostic page limit");
    const auto inventory = discover(snapshot);
    const auto& notes = *selected(inventory, owner_id).diagnostics;
    require(offset <= notes.size(), "diagnostic page offset exceeds retained count");
    DiagnosticBudget budget;
    budget.inspect(notes);
    for (const auto& note : notes) require(note.is_object(), "invalid source fidelity diagnostic row");
    const auto count = std::min(limit, notes.size() - offset);
    Json result = Json::array();
    result.get_ref<Json::array_t&>().reserve(count);
    for (std::size_t i = 0; i < count; ++i) result.push_back(notes[offset + i]);
    return result;
}
} // namespace sketch
