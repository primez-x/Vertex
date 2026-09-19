#include "sketch/document_digest.hpp"
#include "sketch/boundary_translation.hpp"
#include "sketch/boundary_transform.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>

namespace sketch {
namespace {
using ordered_json = nlohmann::ordered_json;

ordered_json entity_json(const Entity& entity) {
    return {{"id", entity.id}, {"type", entity.type},
            {"properties", ordered_json(entity.properties)}, {"required", entity.required},
            {"extensions", ordered_json(entity.extensions)}};
}

std::string byte_hex(const std::vector<std::byte>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[index]);
        result[index * 2] = digits[value >> 4];
        result[index * 2 + 1] = digits[value & 0x0f];
    }
    return result;
}

ordered_json entities_json(const std::map<std::string, Entity, std::less<>>& entities) {
    auto result = ordered_json::array();
    for (const auto& [id, entity] : entities) {
        if (id != entity.id)
            throw std::invalid_argument("Entity map key does not match its stable ID");
        result.push_back(entity_json(entity));
    }
    return result;
}

ordered_json assets_json(const std::map<std::string, Asset, std::less<>>& assets) {
    auto result = ordered_json::array();
    for (const auto& [id, asset] : assets) {
        if (id != asset.id)
            throw std::invalid_argument("Asset map key does not match its stable ID");
        result.push_back({{"id", asset.id}, {"media_type", asset.media_type},
                          {"bytes", byte_hex(asset.bytes)}, {"sha256", asset.sha256},
                          {"metadata", ordered_json(asset.metadata)}});
    }
    return result;
}

std::string digest_json(const ordered_json& value) {
    const auto encoded = value.dump();
    return sha256_hex(std::as_bytes(std::span(encoded.data(), encoded.size())));
}
} // namespace

std::string entity_map_digest(const std::map<std::string, Entity, std::less<>>& entities) {
    return digest_json(ordered_json{{"entities", entities_json(entities)}});
}

namespace {
ordered_json snapshot_json(const DocumentSnapshot& snapshot,
                           std::optional<Revision> through = std::nullopt) {
    if (through && (*through >= snapshot.history().size() ||
                    snapshot.history()[static_cast<std::size_t>(*through)].revision != *through)) {
        throw std::invalid_argument("Historical source revision is not retained");
    }
    auto history = ordered_json::array();
    const auto count = through ? static_cast<std::size_t>(*through) + 1 : snapshot.history().size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = snapshot.history()[index];
        if (through && record.revision != index)
            throw std::invalid_argument("Historical source prefix is not contiguous");
        ordered_json item{{"revision", record.revision}, {"action", record.action},
                          {"entities", entities_json(record.entities)},
                          {"assets", assets_json(record.assets)},
                          {"undo_stack", record.undo_stack}, {"redo_stack", record.redo_stack}};
        item["parent_revision"] = record.parent_revision.has_value()
            ? ordered_json(*record.parent_revision) : ordered_json(nullptr);
        item["source_revision"] = record.source_revision.has_value()
            ? ordered_json(*record.source_revision) : ordered_json(nullptr);
        item["name"] = record.name.has_value() ? ordered_json(*record.name) : ordered_json(nullptr);
        // Omit absent proofs to preserve the frozen v1 digest vectors.
        if (record.boundary_translation)
            item["boundary_translation"] = encode_boundary_translation(*record.boundary_translation);
        if (record.boundary_transform)
            item["boundary_transform"] = encode_boundary_transform(*record.boundary_transform);
        history.push_back(std::move(item));
    }
    auto names = ordered_json::array();
    for (const auto& [name, revision] : snapshot.named_revisions()) {
        if (!through || revision <= *through)
            names.push_back({{"name", name}, {"revision", revision}});
    }
    ordered_json value{{"document_id", snapshot.document_id()}, {"revision", through.value_or(snapshot.revision())},
                       {"editable", snapshot.is_editable()}, {"read_only_reason", snapshot.read_only_reason()},
                       {"history", std::move(history)}, {"named_revisions", std::move(names)}};
    value["saved_revision"] = snapshot.saved_revision_optional().has_value()
        ? ordered_json(*snapshot.saved_revision_optional()) : ordered_json(nullptr);
    return value;
}
std::string authoring_digest(const DocumentSnapshot& snapshot, std::optional<Revision> through) {
    auto source = snapshot_json(snapshot, through);
    // Frozen v1 hash domain: this is not a displayed or serialized product name.
    // Renaming it would invalidate existing recovery receipts and history fences.
    ordered_json value{{"domain", "property-studio.authoring-source"}, {"version", 1},
                       {"document_id", snapshot.document_id()}, {"revision", through.value_or(snapshot.revision())},
                       {"history", std::move(source["history"])},
                       {"named_revisions", std::move(source["named_revisions"])}};
    return digest_json(value);
}
} // namespace

std::string document_snapshot_digest(const DocumentSnapshot& snapshot) {
    return digest_json(snapshot_json(snapshot));
}

std::string document_authoring_source_digest_v1(const DocumentSnapshot& snapshot) {
    return authoring_digest(snapshot, std::nullopt);
}

std::string document_authoring_source_digest_v1_at_revision(
    const DocumentSnapshot& snapshot, Revision revision) {
    return authoring_digest(snapshot, revision);
}
} // namespace sketch
