#include "sketch/document.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/boundary_integrity.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>

namespace sketch {
namespace {

constexpr std::size_t kMaximumIdBytes = 128;
constexpr std::size_t kMaximumTypeBytes = 64;
constexpr std::size_t kMaximumEntityJsonBytes = 1024 * 1024;
constexpr std::size_t kMaximumAssetBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::size_t kMaximumJsonValues = 100'000;

[[noreturn]] void document_error(DocumentErrorCode code, const std::string& message) {
    throw DocumentError(code, message);
}

bool is_valid_identifier(std::string_view value) {
    if (value.empty() || value.size() > kMaximumIdBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.' || character == ':';
    });
}

bool is_valid_utf8_without_nul(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first == 0) {
            return false;
        }
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        unsigned char second_minimum = 0x80U;
        unsigned char second_maximum = 0xbfU;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2;
            if (first == 0xe0U) {
                second_minimum = 0xa0U;
            } else if (first == 0xedU) {
                second_maximum = 0x9fU;
            }
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3;
            if (first == 0xf0U) {
                second_minimum = 0x90U;
            } else if (first == 0xf4U) {
                second_maximum = 0x8fU;
            }
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if (second < second_minimum || second > second_maximum) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if (continuation < 0x80U || continuation > 0xbfU) {
                return false;
            }
        }
        index += continuation_count + 1;
    }
    return true;
}

void validate_json_value(const nlohmann::json& value, std::size_t depth, std::size_t& count,
                         DocumentErrorCode error_code, std::string_view context) {
    if (++count > kMaximumJsonValues || depth > kMaximumJsonDepth) {
        document_error(error_code, std::string(context) + " exceeds JSON complexity limits");
    }
    if (value.is_discarded() || value.is_binary()) {
        document_error(error_code, std::string(context) + " contains a non-portable JSON value");
    }
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        document_error(error_code, std::string(context) + " contains a non-finite number");
    }
    if (value.is_array()) {
        for (const auto& child : value) {
            validate_json_value(child, depth + 1, count, error_code, context);
        }
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key.size() > kMaximumIdBytes) {
                document_error(error_code, std::string(context) + " contains an oversized key");
            }
            validate_json_value(child, depth + 1, count, error_code, context);
        }
    }
}

void validate_json_object(const nlohmann::json& value, DocumentErrorCode error_code,
                          std::string_view context) {
    if (!value.is_object()) {
        document_error(error_code, std::string(context) + " must be a JSON object");
    }
    std::size_t count = 0;
    validate_json_value(value, 0, count, error_code, context);
    try {
        if (value.dump().size() > kMaximumEntityJsonBytes) {
            document_error(error_code, std::string(context) + " exceeds the encoded size limit");
        }
    } catch (const nlohmann::json::exception& error) {
        document_error(error_code, std::string(context) + " cannot be encoded: " + error.what());
    }
}

void validate_entity(const Entity& entity) {
    if (!is_valid_identifier(entity.id)) {
        document_error(DocumentErrorCode::invalid_entity, "entity id is empty or invalid");
    }
    if (entity.type.empty() || entity.type.size() > kMaximumTypeBytes ||
        !std::all_of(entity.type.begin(), entity.type.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '_';
        })) {
        document_error(DocumentErrorCode::invalid_entity, "entity type is empty or invalid");
    }
    validate_json_object(entity.properties, DocumentErrorCode::invalid_entity,
                         "entity properties");
    validate_json_object(entity.extensions, DocumentErrorCode::invalid_entity,
                         "entity extensions");
    static constexpr std::array reserved{"id", "type", "required", "properties"};
    for (const auto* key : reserved) {
        if (entity.extensions.contains(key)) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("entity extension uses reserved field: ") + key);
        }
    }
    if (entity.type == kSheetViewEntityType) {
        try {
            validate_sheet_view_entity(entity);
        } catch (const std::exception& error) {
            document_error(DocumentErrorCode::invalid_entity,
                           std::string("invalid sheet/view entity: ") + error.what());
        }
    }
}

void validate_asset(const Asset& asset) {
    if (!is_valid_identifier(asset.id)) {
        document_error(DocumentErrorCode::invalid_asset, "asset id is empty or invalid");
    }
    if (asset.media_type.empty() || asset.media_type.size() > 256 ||
        asset.media_type.find_first_of("\r\n") != std::string::npos ||
        !is_valid_utf8_without_nul(asset.media_type)) {
        document_error(DocumentErrorCode::invalid_asset, "asset media type is invalid");
    }
    if (asset.bytes.size() > kMaximumAssetBytes) {
        document_error(DocumentErrorCode::invalid_asset, "asset exceeds the byte limit");
    }
    validate_json_object(asset.metadata, DocumentErrorCode::invalid_asset, "asset metadata");
    if (asset.sha256.size() != 64 ||
        !std::all_of(asset.sha256.begin(), asset.sha256.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }) ||
        sha256_hex(asset.bytes) != asset.sha256) {
        document_error(DocumentErrorCode::invalid_asset, "asset SHA-256 does not match its bytes");
    }
}

struct EntityReference {
    enum class Target { entity, asset };

    std::string id;
    std::optional<std::string_view> expected_type;
    Target target = Target::entity;
};

std::optional<std::optional<std::string_view>> reference_type_for_key(std::string_view key) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 17> typed{{
        {"property_id", "property"},
        {"building_id", "building"},
        {"floor_id", "floor"},
        {"layer_id", "layer"},
        {"boundary_id", "boundary"},
        {"wall_id", "wall"},
        {"opening_id", "opening"},
        {"room_id", "room"},
        {"slab_id", "slab"},
        {"roof_id", "roof"},
        {"stair_id", "stair"},
        {"sheet_id", "sheet"},
        {"view_id", "view"},
        {"constraint_id", "constraint"},
        {"label_id", "label"},
        {"column_id", "column"},
        {"beam_id", "beam"},
    }};
    for (const auto& [candidate, type] : typed) {
        if (key == candidate) {
            return type;
        }
        if (key.size() == candidate.size() + 1 && key.back() == 's' &&
            key.substr(0, candidate.size()) == candidate) {
            return type;
        }
    }
    if (key == "parent_id" || key == "host_id" || key == "target_id" ||
        key == "entity_id" || key == "parent_ids" || key == "host_ids" ||
        key == "target_ids" || key == "entity_ids") {
        return std::optional<std::string_view>{};
    }
    return std::nullopt;
}

void collect_references(const Entity& entity, std::vector<EntityReference>& references) {
    if (!is_known_entity_type(entity.type)) {
        return;
    }
    for (const auto& [key, value] : entity.properties.items()) {
        if (key == "refs" || key == "references") {
            if (!value.is_array()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "reference collection must be a JSON array");
            }
            for (const auto& reference : value) {
                if (!reference.is_string()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "reference collection contains a non-string id");
                }
                references.push_back(
                    {reference.get<std::string>(), std::nullopt, EntityReference::Target::entity});
            }
            continue;
        }
        if (key == "asset_id" || key == "asset_ids") {
            const bool collection = key == "asset_ids";
            if (collection) {
                if (!value.is_array()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "asset_ids must be a JSON array");
                }
                for (const auto& reference : value) {
                    if (!reference.is_string()) {
                        document_error(DocumentErrorCode::invalid_entity,
                                       "asset_ids contains a non-string id");
                    }
                    references.push_back({reference.get<std::string>(), std::nullopt,
                                          EntityReference::Target::asset});
                }
            } else {
                if (!value.is_string()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "asset_id must contain a string id");
                }
                references.push_back({value.get<std::string>(), std::nullopt,
                                      EntityReference::Target::asset});
            }
            continue;
        }
        const auto expected_type = reference_type_for_key(key);
        if (!expected_type.has_value()) {
            continue;
        }
        const bool collection = key.size() >= 4 && key.substr(key.size() - 4) == "_ids";
        if (collection) {
            if (!value.is_array()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "canonical reference collection must be a JSON array");
            }
            for (const auto& reference : value) {
                if (!reference.is_string()) {
                    document_error(DocumentErrorCode::invalid_entity,
                                   "canonical reference collection contains a non-string id");
                }
                references.push_back({reference.get<std::string>(), *expected_type,
                                      EntityReference::Target::entity});
            }
        } else {
            if (!value.is_string()) {
                document_error(DocumentErrorCode::invalid_entity,
                               "canonical reference property must contain a string id");
            }
            references.push_back(
                {value.get<std::string>(), *expected_type, EntityReference::Target::entity});
        }
    }
}

std::optional<std::string> validate_state(const std::map<std::string, Entity, std::less<>>& entities,
                    const std::map<std::string, Asset, std::less<>>& assets) {
    for (const auto& [id, entity] : entities) {
        validate_entity(entity);
        if (id != entity.id) {
            document_error(DocumentErrorCode::invalid_entity,
                           "entity map key does not match its stable id");
        }
    }
    for (const auto& [id, asset] : assets) {
        validate_asset(asset);
        if (id != asset.id) {
            document_error(DocumentErrorCode::invalid_asset,
                           "asset map key does not match its stable id");
        }
    }
    for (const auto& [id, entity] : entities) {
        std::vector<EntityReference> references;
        collect_references(entity, references);
        for (const auto& reference : references) {
            if (reference.target == EntityReference::Target::asset) {
                if (!assets.contains(reference.id)) {
                    document_error(DocumentErrorCode::dangling_reference,
                                   "entity " + id + " references missing asset " + reference.id);
                }
                continue;
            }
            const auto target = entities.find(reference.id);
            if (target == entities.end()) {
                document_error(DocumentErrorCode::dangling_reference,
                               "entity " + id + " references missing entity " + reference.id);
            }
            if (reference.expected_type.has_value() &&
                target->second.type != *reference.expected_type) {
                document_error(DocumentErrorCode::invalid_entity,
                               "entity " + id + " reference " + reference.id +
                                   " has type " + target->second.type + ", expected " +
                                   std::string(*reference.expected_type));
            }
        }
    }
    std::optional<std::string> unsupported_boundary;
    try { unsupported_boundary = validate_boundary_integrity(entities); }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
    try {
        const auto unsupported_constraint = validate_constraint_integrity(entities);
        return unsupported_boundary ? unsupported_boundary : unsupported_constraint;
    }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::constraint_violation, error.what());
    }
}

void validate_constraint_change(const std::map<std::string, Entity, std::less<>>& before,
                                const std::map<std::string, Entity, std::less<>>& after) {
    try { validate_constraint_transition(before, after); }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::constraint_violation, error.what());
    }
}

void validate_boundary_change(const BoundaryIdentityHistory& history,
                              const std::map<std::string, Entity, std::less<>>& before,
                              const std::map<std::string, Entity, std::less<>>& after) {
    try {
        validate_boundary_transition(before, after);
        validate_boundary_identity_transition(history, before, after);
    }
    catch (const std::exception& error) {
        document_error(DocumentErrorCode::invalid_entity, error.what());
    }
}

void validate_expected_revision(Revision actual, Revision expected) {
    if (actual != expected) {
        document_error(DocumentErrorCode::stale_revision,
                       "command expected revision " + std::to_string(expected) +
                           " but document is at revision " + std::to_string(actual));
    }
}

std::string command_message(const ApplyEntityChanges& command) {
    return command.message.empty() ? "apply entity changes" : command.message;
}

void validate_action(std::string_view action) {
    if (action.empty() || action.size() > 1024 || !is_valid_utf8_without_nul(action)) {
        document_error(DocumentErrorCode::invalid_entity,
                       "revision action must contain 1 to 1024 bytes of valid UTF-8");
    }
}

void validate_revision_name(std::string_view name) {
    if (name.empty() || name.size() > 256 || !is_valid_utf8_without_nul(name)) {
        document_error(DocumentErrorCode::duplicate_revision_name,
                       "revision name must contain 1 to 256 bytes of valid UTF-8");
    }
}

bool same_state(const RevisionRecord& left, const RevisionRecord& right) {
    if (left.entities != right.entities || left.assets != right.assets) return false;
    // JSON numeric equality treats 1 and 1.0 (and signed zero) alike. Exact
    // navigation must also preserve their serialized form, including opaque
    // metadata; signed/unsigned integer storage types both serialize as JSON 1.
    for (const auto& [id, entity] : left.entities) {
        const auto& other = right.entities.at(id);
        if (entity.properties.dump() != other.properties.dump() ||
            entity.extensions.dump() != other.extensions.dump()) return false;
    }
    for (const auto& [id, asset] : left.assets) {
        if (asset.metadata.dump() != right.assets.at(id).metadata.dump()) return false;
    }
    return true;
}

std::vector<Revision> appended(std::vector<Revision> revisions, Revision revision) {
    revisions.push_back(revision);
    return revisions;
}

}  // namespace

DocumentError::DocumentError(DocumentErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

DocumentErrorCode DocumentError::code() const noexcept { return code_; }

std::string make_stable_id() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(random());
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result << '-';
        }
        result << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return result.str();
}

std::string sha256_hex(std::span<const std::byte> bytes) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;

    auto check = [](NTSTATUS status, std::string_view operation) {
        if (status < 0) {
            throw std::runtime_error(std::string("BCrypt SHA-256 ") + std::string(operation) +
                                     " failed");
        }
    };
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
              "initialization");
        check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                &received, 0),
              "object-size query");
        check(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &received,
                                0),
              "digest-size query");
        object.resize(object_size);
        digest.resize(hash_size);
        check(BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0),
              "hash creation");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(
                remaining, static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));
            check(BCryptHashData(hash,
                                 reinterpret_cast<PUCHAR>(
                                     const_cast<std::byte*>(bytes.data() + offset)),
                                 chunk, 0),
                  "update");
            offset += chunk;
        }
        check(BCryptFinishHash(hash, digest.data(), hash_size, 0), "finalization");
    } catch (...) {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = hex[digest[index] >> 4U];
        result[index * 2 + 1] = hex[digest[index] & 0x0fU];
    }
    return result;
#else
    (void)bytes;
    throw std::runtime_error("SHA-256 requires Windows BCrypt in this build");
#endif
}

bool is_known_entity_type(std::string_view type) noexcept {
    static constexpr std::array<std::string_view, 21> known{
        "property",             "building", "floor",  "layer", "boundary",
        "measurement_boundary", "room_boundary", "wall", "opening", "room",
        "slab",                 "roof",     "stair",  "column", "beam",
        "label",                "sheet",    "view",   "constraint", "dimension",
        "sheet_view_model"};
    return std::find(known.begin(), known.end(), type) != known.end();
}

Entity Entity::create(std::string type, nlohmann::json properties, bool required,
                      nlohmann::json extensions) {
    return Entity{make_stable_id(), std::move(type), std::move(properties), required,
                  std::move(extensions)};
}

Asset Asset::create(std::string id, std::string media_type, std::vector<std::byte> bytes,
                    nlohmann::json metadata) {
    const auto digest = sha256_hex(bytes);
    return Asset{std::move(id), std::move(media_type), std::move(bytes), digest,
                 std::move(metadata)};
}

Asset Asset::create(std::string media_type, std::vector<std::byte> bytes,
                    nlohmann::json metadata) {
    return create(make_stable_id(), std::move(media_type), std::move(bytes), std::move(metadata));
}

EntityChange EntityChange::upsert(Entity entity) {
    const auto id = entity.id;
    return EntityChange{EntityChangeKind::upsert, std::move(entity), id};
}

EntityChange EntityChange::erase(std::string entity_id) {
    return EntityChange{EntityChangeKind::erase, {}, std::move(entity_id)};
}

AssetChange AssetChange::upsert(Asset asset) {
    const auto id = asset.id;
    return AssetChange{AssetChangeKind::upsert, std::move(asset), id};
}

AssetChange AssetChange::erase(std::string asset_id) {
    return AssetChange{AssetChangeKind::erase, {}, std::move(asset_id)};
}

const std::string& DocumentSnapshot::document_id() const noexcept { return document_id_; }
Revision DocumentSnapshot::revision() const noexcept { return revision_; }
std::optional<Revision> DocumentSnapshot::saved_revision_optional() const noexcept {
    return saved_revision_;
}
Revision DocumentSnapshot::saved_revision() const noexcept { return saved_revision_.value_or(0); }
bool DocumentSnapshot::dirty() const noexcept {
    return !saved_revision_.has_value() || *saved_revision_ != revision_;
}
bool DocumentSnapshot::is_editable() const noexcept { return editable_; }
const std::string& DocumentSnapshot::read_only_reason() const noexcept { return read_only_reason_; }
const std::map<std::string, Entity, std::less<>>& DocumentSnapshot::entities() const noexcept {
    return history_.at(static_cast<std::size_t>(revision_)).entities;
}
const std::map<std::string, Asset, std::less<>>& DocumentSnapshot::assets() const noexcept {
    return history_.at(static_cast<std::size_t>(revision_)).assets;
}
const std::vector<RevisionRecord>& DocumentSnapshot::history() const noexcept { return history_; }
const std::map<std::string, Revision, std::less<>>& DocumentSnapshot::named_revisions() const noexcept {
    return named_revisions_;
}

Document::Document(std::string document_id) : document_id_(std::move(document_id)) {}

Document Document::create() { return create({}, {}); }

Document Document::create(std::vector<Entity> initial_entities, std::vector<Asset> initial_assets) {
    Document document(make_stable_id());
    RevisionRecord initial;
    initial.revision = 0;
    initial.action = "create";
    for (auto& entity : initial_entities) {
        validate_entity(entity);
        if (!initial.entities.emplace(entity.id, std::move(entity)).second) {
            document_error(DocumentErrorCode::duplicate_change, "duplicate initial entity id");
        }
    }
    for (auto& asset : initial_assets) {
        validate_asset(asset);
        if (!initial.assets.emplace(asset.id, std::move(asset)).second) {
            document_error(DocumentErrorCode::duplicate_change, "duplicate initial asset id");
        }
    }
    document.unsupported_constraint_history_reason_ = validate_state(initial.entities, initial.assets);
    record_boundary_identities(document.boundary_identity_history_, initial.entities);
    document.history_.push_back(std::move(initial));
    document.update_editability();
    return document;
}

Revision Document::revision() const noexcept { return head_revision_; }
std::optional<Revision> Document::saved_revision_optional() const noexcept { return saved_revision_; }
Revision Document::saved_revision() const noexcept { return saved_revision_.value_or(0); }
bool Document::dirty() const noexcept {
    return !saved_revision_.has_value() || *saved_revision_ != head_revision_;
}
bool Document::is_editable() const noexcept { return editable_; }
const std::string& Document::read_only_reason() const noexcept { return read_only_reason_; }
bool Document::can_undo() const noexcept { return !head_record().undo_stack.empty(); }
bool Document::can_redo() const noexcept { return !head_record().redo_stack.empty(); }

const RevisionRecord& Document::head_record() const {
    if (head_revision_ >= history_.size() || history_[static_cast<std::size_t>(head_revision_)].revision !=
                                                    head_revision_) {
        document_error(DocumentErrorCode::invalid_history, "document head is absent from history");
    }
    return history_[static_cast<std::size_t>(head_revision_)];
}

DocumentSnapshot Document::snapshot() const {
    DocumentSnapshot snapshot;
    snapshot.document_id_ = document_id_;
    snapshot.revision_ = head_revision_;
    snapshot.saved_revision_ = saved_revision_;
    snapshot.editable_ = editable_;
    snapshot.read_only_reason_ = read_only_reason_;
    snapshot.history_ = history_;
    snapshot.named_revisions_ = named_revisions_;
    return snapshot;
}

Document Document::fork(const DocumentSnapshot& source) {
    return restore(source);
}

Document Document::fork_at_revision(const DocumentSnapshot& source, Revision revision) {
    (void)fork(source);
    if (revision >= source.history_.size())
        document_error(DocumentErrorCode::invalid_history, "requested revision is not retained");
    auto prefix = source;
    prefix.history_.resize(static_cast<std::size_t>(revision) + 1);
    prefix.revision_ = revision;
    if (prefix.saved_revision_ && *prefix.saved_revision_ > revision) prefix.saved_revision_.reset();
    for (auto it = prefix.named_revisions_.begin(); it != prefix.named_revisions_.end();) {
        if (it->second > revision) it = prefix.named_revisions_.erase(it);
        else ++it;
    }
    return restore(std::move(prefix));
}

DocumentSnapshot Document::preview_command(const DocumentSnapshot& source, const Command& command) {
    auto candidate = fork(source);
    candidate.apply(command);
    return candidate.snapshot();
}

Revision Document::apply(const Command& command) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    return std::visit(
        [this](const auto& typed_command) -> Revision {
            validate_expected_revision(head_revision_, typed_command.expected_revision);
            const auto& current = head_record();
            RevisionRecord next;
            next.revision = static_cast<Revision>(history_.size());
            next.parent_revision = head_revision_;
            next.entities = current.entities;
            next.assets = current.assets;
            next.undo_stack = current.undo_stack;
            next.undo_stack.push_back(head_revision_);
            std::optional<std::string> next_unsupported_constraints;
            auto next_identity_history = boundary_identity_history_;

            using CommandType = std::decay_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<CommandType, ApplyEntityChanges>) {
                next.action = command_message(typed_command);
                validate_action(next.action);
                std::unordered_set<std::string> touched_entities;
                for (const auto& change : typed_command.entity_changes) {
                    const auto& id = change.kind == EntityChangeKind::upsert ? change.entity.id
                                                                            : change.entity_id;
                    if (!touched_entities.insert(id).second) {
                        document_error(DocumentErrorCode::duplicate_change,
                                       "entity is changed more than once in one command: " + id);
                    }
                    if (change.kind == EntityChangeKind::upsert) {
                        validate_entity(change.entity);
                        next.entities.insert_or_assign(change.entity.id, change.entity);
                    } else {
                        if (!is_valid_identifier(change.entity_id)) {
                            document_error(DocumentErrorCode::invalid_entity,
                                           "deleted entity id is invalid");
                        }
                        next.entities.erase(change.entity_id);
                    }
                }
                std::unordered_set<std::string> touched_assets;
                for (const auto& change : typed_command.asset_changes) {
                    const auto& id = change.kind == AssetChangeKind::upsert ? change.asset.id
                                                                           : change.asset_id;
                    if (!touched_assets.insert(id).second) {
                        document_error(DocumentErrorCode::duplicate_change,
                                       "asset is changed more than once in one command: " + id);
                    }
                    if (change.kind == AssetChangeKind::upsert) {
                        validate_asset(change.asset);
                        next.assets.insert_or_assign(change.asset.id, change.asset);
                    } else {
                        if (!is_valid_identifier(change.asset_id)) {
                            document_error(DocumentErrorCode::invalid_asset,
                                           "deleted asset id is invalid");
                        }
                        next.assets.erase(change.asset_id);
                    }
                }
                next_unsupported_constraints = validate_state(next.entities, next.assets);
                validate_constraint_change(current.entities, next.entities);
                validate_boundary_change(boundary_identity_history_, current.entities, next.entities);
                record_boundary_identity_transition(next_identity_history, current.entities, next.entities);
            } else {
                validate_revision_name(typed_command.name);
                if (named_revisions_.contains(typed_command.name)) {
                    document_error(DocumentErrorCode::duplicate_revision_name,
                                   "revision name already exists: " + typed_command.name);
                }
                next.action = "name revision";
                next.name = typed_command.name;
            }

            history_.push_back(std::move(next));
            boundary_identity_history_ = std::move(next_identity_history);
            head_revision_ = history_.back().revision;
            if (next_unsupported_constraints)
                unsupported_constraint_history_reason_ = std::move(next_unsupported_constraints);
            if constexpr (std::is_same_v<CommandType, NameRevision>) {
                named_revisions_.emplace(typed_command.name, head_revision_);
            }
            update_editability();
            return head_revision_;
        },
        command);
}

Revision Document::undo(Revision expected_revision) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    validate_expected_revision(head_revision_, expected_revision);
    const auto current = head_record();
    if (current.undo_stack.empty()) {
        document_error(DocumentErrorCode::no_undo, "there is no command to undo");
    }
    const auto target_revision = current.undo_stack.back();
    const auto& target = history_.at(static_cast<std::size_t>(target_revision));
    if (const auto unsupported = validate_state(target.entities, target.assets))
        document_error(DocumentErrorCode::read_only, *unsupported);
    RevisionRecord next;
    next.revision = static_cast<Revision>(history_.size());
    next.parent_revision = head_revision_;
    next.source_revision = target_revision;
    next.action = "undo";
    next.entities = target.entities;
    next.assets = target.assets;
    next.undo_stack = current.undo_stack;
    next.undo_stack.pop_back();
    next.redo_stack = current.redo_stack;
    next.redo_stack.push_back(head_revision_);
    history_.push_back(std::move(next));
    head_revision_ = history_.back().revision;
    update_editability();
    return head_revision_;
}

Revision Document::redo(Revision expected_revision) {
    if (!editable_) {
        document_error(DocumentErrorCode::read_only, read_only_reason_);
    }
    validate_expected_revision(head_revision_, expected_revision);
    const auto current = head_record();
    if (current.redo_stack.empty()) {
        document_error(DocumentErrorCode::no_redo, "there is no command to redo");
    }
    const auto target_revision = current.redo_stack.back();
    const auto& target = history_.at(static_cast<std::size_t>(target_revision));
    if (const auto unsupported = validate_state(target.entities, target.assets))
        document_error(DocumentErrorCode::read_only, *unsupported);
    RevisionRecord next;
    next.revision = static_cast<Revision>(history_.size());
    next.parent_revision = head_revision_;
    next.source_revision = target_revision;
    next.action = "redo";
    next.entities = target.entities;
    next.assets = target.assets;
    next.undo_stack = current.undo_stack;
    next.undo_stack.push_back(head_revision_);
    next.redo_stack = current.redo_stack;
    next.redo_stack.pop_back();
    history_.push_back(std::move(next));
    head_revision_ = history_.back().revision;
    update_editability();
    return head_revision_;
}

void Document::mark_saved(Revision revision) {
    if (revision >= history_.size() || history_[static_cast<std::size_t>(revision)].revision != revision) {
        document_error(DocumentErrorCode::invalid_saved_revision,
                       "saved revision is absent from document history");
    }
    saved_revision_ = revision;
}

void Document::update_editability() {
    editable_ = true;
    read_only_reason_.clear();
    if (unsupported_constraint_history_reason_) {
        editable_ = false;
        read_only_reason_ = *unsupported_constraint_history_reason_;
        return;
    }
    for (const auto& [id, entity] : head_record().entities) {
        if (entity.required && !is_known_entity_type(entity.type)) {
            editable_ = false;
            read_only_reason_ = "required entity type is unsupported: " + entity.type + " (" + id + ")";
            return;
        }
    }
}

Document Document::restore(DocumentSnapshot snapshot) {
    if (!is_valid_identifier(snapshot.document_id_)) {
        document_error(DocumentErrorCode::invalid_history, "stored document id is invalid");
    }
    if (snapshot.history_.empty() || snapshot.revision_ + 1 != snapshot.history_.size()) {
        document_error(DocumentErrorCode::invalid_history, "stored revision history is incomplete");
    }
    if (snapshot.saved_revision_.has_value() && *snapshot.saved_revision_ > snapshot.revision_) {
        document_error(DocumentErrorCode::invalid_history, "stored saved revision is invalid");
    }
    std::map<std::string, Revision, std::less<>> expected_names;
    std::optional<std::string> unsupported_constraint_history;
    BoundaryIdentityHistory identity_history;
    for (std::size_t index = 0; index < snapshot.history_.size(); ++index) {
        const auto& record = snapshot.history_[index];
        validate_action(record.action);
        if (record.revision != index) {
            document_error(DocumentErrorCode::invalid_history, "stored revisions are not contiguous");
        }
        auto unsupported = validate_state(record.entities, record.assets);
        if (unsupported && !unsupported_constraint_history)
            unsupported_constraint_history = std::move(unsupported);

        if (index == 0) {
            if (record.parent_revision.has_value() || record.source_revision.has_value() ||
                record.name.has_value() || record.action != "create" ||
                !record.undo_stack.empty() || !record.redo_stack.empty()) {
                document_error(DocumentErrorCode::invalid_history,
                               "revision zero is not a valid create record");
            }
            record_boundary_identities(identity_history, record.entities);
            continue;
        }

        const auto& previous = snapshot.history_[index - 1];
        // Unknown locks retain the read-only latch, but must not suppress
        // stable-endpoint checks for known relations in the same history.
        validate_constraint_change(previous.entities, record.entities);
        if (record.parent_revision != Revision{index - 1}) {
            document_error(DocumentErrorCode::invalid_history,
                           "revision parent must be the immediately preceding event");
        }
        if (record.source_revision.has_value() && record.name.has_value()) {
            document_error(DocumentErrorCode::invalid_history,
                           "revision cannot be both named and an undo/redo event");
        }

        if (record.name.has_value()) {
            validate_revision_name(*record.name);
            if (record.action != "name revision" || record.source_revision.has_value() ||
                !same_state(record, previous) ||
                record.undo_stack != appended(previous.undo_stack, Revision{index - 1}) ||
                !record.redo_stack.empty()) {
                document_error(DocumentErrorCode::invalid_history,
                               "named revision transition is impossible");
            }
            if (!expected_names.emplace(*record.name, record.revision).second) {
                document_error(DocumentErrorCode::invalid_history,
                               "stored revision name is duplicated");
            }
        } else if (record.source_revision.has_value()) {
            if (*record.source_revision >= index) {
                document_error(DocumentErrorCode::invalid_history,
                               "undo/redo source revision is not prior history");
            }
            const auto& source = snapshot.history_[static_cast<std::size_t>(*record.source_revision)];
            if (!same_state(record, source)) {
                document_error(DocumentErrorCode::invalid_history,
                               "undo/redo state does not match its source revision");
            }
            if (record.action == "undo") {
                if (previous.undo_stack.empty() ||
                    *record.source_revision != previous.undo_stack.back()) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "undo source is not the prior undo-stack top");
                }
                auto expected_undo = previous.undo_stack;
                expected_undo.pop_back();
                if (record.undo_stack != expected_undo ||
                    record.redo_stack != appended(previous.redo_stack, Revision{index - 1})) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "undo navigation transition is impossible");
                }
            } else if (record.action == "redo") {
                if (previous.redo_stack.empty() ||
                    *record.source_revision != previous.redo_stack.back()) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "redo source is not the prior redo-stack top");
                }
                auto expected_redo = previous.redo_stack;
                expected_redo.pop_back();
                if (record.undo_stack != appended(previous.undo_stack, Revision{index - 1}) ||
                    record.redo_stack != expected_redo) {
                    document_error(DocumentErrorCode::invalid_history,
                                   "redo navigation transition is impossible");
                }
            } else {
                document_error(DocumentErrorCode::invalid_history,
                               "source revision is only valid for undo or redo");
            }
        } else if (record.undo_stack != appended(previous.undo_stack, Revision{index - 1}) ||
                   !record.redo_stack.empty()) {
            document_error(DocumentErrorCode::invalid_history,
                           "apply-command navigation transition is impossible");
        }
        // An exact, validated history navigation may undo an identity upgrade.
        // Ordinary Apply records must never masquerade as that downgrade.
        if (!record.source_revision.has_value()) {
            validate_boundary_change(identity_history, previous.entities, record.entities);
            record_boundary_identity_transition(identity_history, previous.entities, record.entities);
        } else record_boundary_identities(identity_history, record.entities);
    }
    if (expected_names != snapshot.named_revisions_) {
        document_error(DocumentErrorCode::invalid_history,
                       "named revision index is not a bijection with history");
    }
    Document document(std::move(snapshot.document_id_));
    document.head_revision_ = snapshot.revision_;
    document.saved_revision_ = snapshot.saved_revision_;
    document.history_ = std::move(snapshot.history_);
    document.named_revisions_ = std::move(snapshot.named_revisions_);
    document.unsupported_constraint_history_reason_ = std::move(unsupported_constraint_history);
    document.boundary_identity_history_ = std::move(identity_history);
    document.update_editability();
    return document;
}

}  // namespace sketch
