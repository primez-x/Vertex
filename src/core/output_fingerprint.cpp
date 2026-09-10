#include "sketch/output_fingerprint.hpp"

#include "sketch/document.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 6> kDependencyGroups{
    "profiles", "fonts", "views", "crs", "processing_components", "application_build"};
constexpr std::array<std::string_view, 4> kProcessingRoles{"kernel", "solver", "renderer",
                                                            "adapters"};

[[noreturn]] void fail(OutputFingerprintErrorCode code, std::string message) {
    throw OutputFingerprintError(code, std::move(message));
}

bool is_lower_hex(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool is_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), is_lower_hex);
}

std::string digest_of(std::string_view value) {
    const auto* data = reinterpret_cast<const std::byte*>(value.data());
    return sha256_hex(std::span<const std::byte>(data, value.size()));
}

std::string digest_bytes(std::span<const std::byte> bytes) { return sha256_hex(bytes); }

bool is_valid_utf8(std::string_view value) noexcept {
    const auto continuation = [](unsigned char byte) noexcept {
        return (byte & 0xc0U) == 0x80U;
    };

    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= value.size() ||
                !continuation(static_cast<unsigned char>(value[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first == 0xe0U) {
            if (index + 2 >= value.size() ||
                !(static_cast<unsigned char>(value[index + 1]) >= 0xa0U &&
                  static_cast<unsigned char>(value[index + 1]) <= 0xbfU) ||
                !continuation(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if ((first >= 0xe1U && first <= 0xecU) || first == 0xeeU || first == 0xefU) {
            if (index + 2 >= value.size() ||
                !continuation(static_cast<unsigned char>(value[index + 1])) ||
                !continuation(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first == 0xedU) {
            if (index + 2 >= value.size() ||
                !(static_cast<unsigned char>(value[index + 1]) >= 0x80U &&
                  static_cast<unsigned char>(value[index + 1]) <= 0x9fU) ||
                !continuation(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first == 0xf0U) {
            if (index + 3 >= value.size() ||
                !(static_cast<unsigned char>(value[index + 1]) >= 0x90U &&
                  static_cast<unsigned char>(value[index + 1]) <= 0xbfU) ||
                !continuation(static_cast<unsigned char>(value[index + 2])) ||
                !continuation(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }
        if (first >= 0xf1U && first <= 0xf3U) {
            if (index + 3 >= value.size() ||
                !continuation(static_cast<unsigned char>(value[index + 1])) ||
                !continuation(static_cast<unsigned char>(value[index + 2])) ||
                !continuation(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }
        if (first == 0xf4U) {
            if (index + 3 >= value.size() ||
                !(static_cast<unsigned char>(value[index + 1]) >= 0x80U &&
                  static_cast<unsigned char>(value[index + 1]) <= 0x8fU) ||
                !continuation(static_cast<unsigned char>(value[index + 2])) ||
                !continuation(static_cast<unsigned char>(value[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

void validate_utf8_string(std::string_view value, std::string_view context,
                          OutputFingerprintErrorCode code) {
    if (!is_valid_utf8(value)) {
        fail(code, std::string(context) + " must contain valid UTF-8");
    }
}

std::string canonical_dump(const Json& value, OutputFingerprintErrorCode code,
                           std::string_view context) {
    try {
        return value.dump();
    } catch (const Json::exception& failure) {
        fail(code, std::string(context) + " cannot be serialized: " + failure.what());
    }
}

void require_object(const Json& value, std::string_view context,
                    OutputFingerprintErrorCode code) {
    if (!value.is_object()) {
        fail(code, std::string(context) + " must be an object");
    }
}

void require_string(const Json& value, std::string_view context,
                    OutputFingerprintErrorCode code, bool non_empty = true) {
    if (!value.is_string() || (non_empty && value.get_ref<const std::string&>().empty())) {
        fail(code, std::string(context) + " must be a non-empty string");
    }
}

void require_only_keys(const Json& object, std::initializer_list<std::string_view> allowed,
                       std::string_view context, OutputFingerprintErrorCode code) {
    for (const auto& [key, value] : object.items()) {
        (void)value;
        const auto found = std::find(allowed.begin(), allowed.end(), key);
        if (found == allowed.end()) {
            fail(code, std::string(context) + " contains unsupported field '" + key + "'");
        }
    }
}

void validate_json_tree(const Json& value, std::string_view context,
                        OutputFingerprintErrorCode code) {
    if (value.is_discarded() || value.is_binary()) {
        fail(code, std::string(context) + " contains a non-portable JSON value");
    }
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        fail(code, std::string(context) + " contains a non-finite number");
    }
    if (value.is_string()) {
        validate_utf8_string(value.get_ref<const std::string&>(), context, code);
    }
    if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            validate_json_tree(value.at(index),
                               std::string(context) + "[" + std::to_string(index) + "]", code);
        }
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            validate_utf8_string(key, std::string(context) + " object key", code);
            validate_json_tree(child, std::string(context) + "." + key, code);
        }
    }
}

const Json& required_field(const Json& object, std::string_view key, std::string_view context,
                           OutputFingerprintErrorCode code) {
    const auto found = object.find(std::string(key));
    if (found == object.end()) {
        fail(code, std::string(context) + " is missing '" + std::string(key) + "'");
    }
    return *found;
}

std::uint32_t schema_value(const Json& value, std::string_view context,
                           OutputFingerprintErrorCode code) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
        fail(code, std::string(context) + " must be a non-negative integer");
    }
    try {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            fail(code, std::string(context) + " is out of range");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const Json::exception&) {
        fail(code, std::string(context) + " must be a non-negative integer");
    }
}

FingerprintGroupState state_from_name(std::string_view value, std::string_view context,
                                      OutputFingerprintErrorCode code) {
    if (value == "resources") {
        return FingerprintGroupState::resources;
    }
    if (value == "no_resource") {
        return FingerprintGroupState::no_resource;
    }
    if (value == "not_applicable") {
        return FingerprintGroupState::not_applicable;
    }
    fail(code, std::string(context) + " has an unknown dependency state");
}

void validate_resource(const FingerprintResource& resource, std::string_view context) {
    if (resource.id.empty()) {
        fail(OutputFingerprintErrorCode::invalid_dependency,
             std::string(context) + " has an empty resource ID");
    }
    validate_utf8_string(resource.id, std::string(context) + ".id",
                         OutputFingerprintErrorCode::invalid_dependency);
    validate_utf8_string(resource.sha256, std::string(context) + ".sha256",
                         OutputFingerprintErrorCode::invalid_dependency);
    if (!is_sha256(resource.sha256)) {
        fail(OutputFingerprintErrorCode::invalid_dependency,
             std::string(context) + " requires a lower-case SHA-256 resource digest");
    }
    if (!resource.metadata.is_object()) {
        fail(OutputFingerprintErrorCode::invalid_dependency,
             std::string(context) + " metadata must be an object");
    }
    validate_json_tree(resource.metadata, std::string(context) + ".metadata",
                       OutputFingerprintErrorCode::invalid_dependency);
}

std::vector<FingerprintResource> sorted_resources(
    const std::vector<FingerprintResource>& resources, std::string_view context) {
    std::vector<FingerprintResource> result = resources;
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    for (std::size_t index = 0; index < result.size(); ++index) {
        validate_resource(result[index], context);
        if (index > 0 && result[index - 1].id == result[index].id) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 std::string(context) + " contains duplicate resource ID '" + result[index].id +
                     "'");
        }
    }
    return result;
}

void validate_role(const FingerprintRole& role, std::string_view role_name) {
    const auto context = std::string("processing_components role '") + std::string(role_name) + "'";
    validate_utf8_string(role.reason, context + ".reason",
                         OutputFingerprintErrorCode::invalid_dependency);
    if (role.state == FingerprintGroupState::resources) {
        if (!role.reason.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 context + " cannot include a not-applicable reason");
        }
        if (role.resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 context + " requires at least one resource digest");
        }
        (void)sorted_resources(role.resources, context);
        return;
    }
    if (role.state == FingerprintGroupState::not_applicable) {
        if (!role.resources.empty() || role.reason.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 context + " requires an explicit not-applicable reason and no resources");
        }
        return;
    }
    fail(OutputFingerprintErrorCode::invalid_dependency,
         context + " must provide resources or an explicit not-applicable reason");
}

void validate_group(const FingerprintDependencyGroup& group, std::string_view name) {
    const bool is_processing = name == "processing_components";
    const bool is_application_build = name == "application_build";
    validate_utf8_string(group.reason, std::string(name) + ".reason",
                         OutputFingerprintErrorCode::invalid_dependency);
    if (!is_processing && !group.roles.empty()) {
        fail(OutputFingerprintErrorCode::invalid_dependency,
             std::string(name) + " cannot contain processing roles");
    }

    if (is_processing) {
        if (group.state != FingerprintGroupState::resources || !group.reason.empty() ||
            !group.resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 "processing_components must use role-level resource states");
        }
        if (group.roles.size() != kProcessingRoles.size()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 "processing_components must identify kernel, solver, renderer, and adapters");
        }
        for (const auto role_name : kProcessingRoles) {
            const auto found = group.roles.find(role_name);
            if (found == group.roles.end()) {
                fail(OutputFingerprintErrorCode::invalid_dependency,
                     "processing_components is missing role '" + std::string(role_name) + "'");
            }
            validate_role(found->second, role_name);
        }
        for (const auto& [role_name, role] : group.roles) {
            (void)role;
            const auto known = std::find(kProcessingRoles.begin(), kProcessingRoles.end(), role_name);
            if (known == kProcessingRoles.end()) {
                fail(OutputFingerprintErrorCode::invalid_dependency,
                     "processing_components contains unsupported role '" + role_name + "'");
            }
        }
        const auto renderer = group.roles.find("renderer");
        if (renderer == group.roles.end() ||
            renderer->second.state != FingerprintGroupState::resources ||
            renderer->second.resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 "processing_components requires a renderer resource digest");
        }
        return;
    }

    if (!group.reason.empty() && group.state == FingerprintGroupState::resources) {
        fail(OutputFingerprintErrorCode::invalid_dependency,
             std::string(name) + " cannot include a no-resource reason");
    }
    if (group.state == FingerprintGroupState::resources) {
        if (group.resources.empty() && is_application_build) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 "application_build requires at least one actual build digest");
        }
        if (group.resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 std::string(name) + " must state no_resource/not_applicable when empty");
        }
        (void)sorted_resources(group.resources, name);
        return;
    }
    if (group.state == FingerprintGroupState::no_resource ||
        group.state == FingerprintGroupState::not_applicable) {
        if (!group.resources.empty() || group.reason.empty()) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 std::string(name) + " requires an explicit reason and no resources");
        }
        if (is_application_build) {
            fail(OutputFingerprintErrorCode::invalid_dependency,
                 "application_build must contain an actual build digest");
        }
        return;
    }
    fail(OutputFingerprintErrorCode::invalid_dependency,
         std::string(name) + " has an unspecified dependency state");
}

Json resource_json(const FingerprintResource& resource) {
    return Json{{"id", resource.id}, {"metadata", resource.metadata}, {"sha256", resource.sha256}};
}

Json role_json(const FingerprintRole& role) {
    std::vector<FingerprintResource> resources = role.resources;
    std::sort(resources.begin(), resources.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    Json encoded_resources = Json::array();
    for (const auto& resource : resources) {
        encoded_resources.push_back(resource_json(resource));
    }
    return Json{{"reason", role.reason},
                {"resources", std::move(encoded_resources)},
                {"state", fingerprint_group_state_name(role.state)}};
}

Json group_json(const FingerprintDependencyGroup& group, std::string_view name) {
    validate_group(group, name);
    Json encoded_resources = Json::array();
    auto resources = sorted_resources(group.resources, name);
    for (const auto& resource : resources) {
        encoded_resources.push_back(resource_json(resource));
    }
    Json result{{"reason", group.reason},
                {"resources", std::move(encoded_resources)},
                {"state", fingerprint_group_state_name(group.state)}};
    if (name == "processing_components") {
        Json roles = Json::object();
        for (const auto role_name : kProcessingRoles) {
            roles[std::string(role_name)] = role_json(group.roles.at(std::string(role_name)));
        }
        result["roles"] = std::move(roles);
    }
    return result;
}

void validate_document_entity(const std::string& map_id, const Entity& entity) {
    if (map_id.empty() || entity.id.empty() || map_id != entity.id || entity.type.empty()) {
        fail(OutputFingerprintErrorCode::invalid_document,
             "head entity IDs and types must be non-empty and map-consistent");
    }
    validate_utf8_string(map_id, "head entity map ID",
                         OutputFingerprintErrorCode::invalid_document);
    validate_utf8_string(entity.id, "head entity ID",
                         OutputFingerprintErrorCode::invalid_document);
    validate_utf8_string(entity.type, "head entity type",
                         OutputFingerprintErrorCode::invalid_document);
    if (!entity.properties.is_object() || !entity.extensions.is_object()) {
        fail(OutputFingerprintErrorCode::invalid_document,
             "head entity properties and extensions must be objects");
    }
    validate_json_tree(entity.properties, "head entity properties",
                       OutputFingerprintErrorCode::invalid_document);
    validate_json_tree(entity.extensions, "head entity extensions",
                       OutputFingerprintErrorCode::invalid_document);
}

// Builds snapshot-derived material that is hashed for the document head. It
// is intentionally not retained in OutputFingerprint::manifest: output
// stamps must not duplicate document data or raw asset payloads.
Json document_content_json(const DocumentSnapshot& snapshot) {
    if (snapshot.document_id().empty()) {
        fail(OutputFingerprintErrorCode::invalid_document, "document ID must be non-empty");
    }
    validate_utf8_string(snapshot.document_id(), "document ID",
                         OutputFingerprintErrorCode::invalid_document);

    Json entities = Json::array();
    for (const auto& [id, entity] : snapshot.entities()) {
        validate_document_entity(id, entity);
        entities.push_back(Json{{"extensions", entity.extensions},
                                {"id", entity.id},
                                {"properties", entity.properties},
                                {"required", entity.required},
                                {"type", entity.type}});
    }

    Json assets = Json::array();
    for (const auto& [id, asset] : snapshot.assets()) {
        if (id.empty() || asset.id.empty() || id != asset.id || asset.media_type.empty() ||
            !asset.metadata.is_object()) {
            fail(OutputFingerprintErrorCode::invalid_asset,
                 "head assets require non-empty map-consistent IDs, media types, and metadata");
        }
        validate_utf8_string(id, "head asset map ID",
                             OutputFingerprintErrorCode::invalid_asset);
        validate_utf8_string(asset.id, "head asset ID",
                             OutputFingerprintErrorCode::invalid_asset);
        validate_utf8_string(asset.media_type, "head asset media type",
                             OutputFingerprintErrorCode::invalid_asset);
        validate_json_tree(asset.metadata, "head asset metadata",
                           OutputFingerprintErrorCode::invalid_asset);
        if (!is_sha256(asset.sha256)) {
            fail(OutputFingerprintErrorCode::invalid_asset,
                 "asset '" + id + "' is missing a lower-case SHA-256 digest");
        }
        const auto actual = digest_bytes(
            std::span<const std::byte>(asset.bytes.data(), asset.bytes.size()));
        // Asset.sha256 is metadata, but it is still validated against a fresh
        // digest so a stale or forged declaration cannot enter the manifest.
        if (actual != asset.sha256) {
            fail(OutputFingerprintErrorCode::invalid_asset,
                 "asset '" + id + "' SHA-256 does not match its bytes");
        }
        assets.push_back(Json{{"id", asset.id},
                             {"media_type", asset.media_type},
                             {"metadata", asset.metadata},
                             {"sha256", actual}});
    }

    return Json{{"assets", std::move(assets)},
                {"document_id", snapshot.document_id()},
                {"entities", std::move(entities)},
                {"revision", snapshot.revision()}};
}

Json make_manifest(const DocumentSnapshot& snapshot, const OutputFingerprintInputs& inputs) {
    const auto document_content = document_content_json(snapshot);
    const auto document_content_digest =
        digest_of(canonical_dump(document_content, OutputFingerprintErrorCode::invalid_document,
                                 "document head"));
    const Json document{{"asset_count", document_content.at("assets").size()},
                        {"document_id", document_content.at("document_id")},
                        {"entity_count", document_content.at("entities").size()},
                        {"head_content_sha256", document_content_digest},
                        {"revision", document_content.at("revision")}};
    Json dependencies{{"application_build", group_json(inputs.application_build, "application_build")},
                      {"crs", group_json(inputs.crs, "crs")},
                      {"fonts", group_json(inputs.fonts, "fonts")},
                      {"processing_components",
                       group_json(inputs.processing_components, "processing_components")},
                      {"profiles", group_json(inputs.profiles, "profiles")},
                      {"views", group_json(inputs.views, "views")}};
    return Json{{"dependencies", std::move(dependencies)},
                {"document", document},
                {"schema_version", kOutputFingerprintSchemaVersion}};
}

void validate_resource_json(const Json& encoded, std::string_view context) {
    require_object(encoded, context, OutputFingerprintErrorCode::invalid_manifest);
    require_only_keys(encoded, {"id", "metadata", "sha256"}, context,
                      OutputFingerprintErrorCode::invalid_manifest);
    const auto& id = required_field(encoded, "id", context,
                                    OutputFingerprintErrorCode::invalid_manifest);
    const auto& metadata = required_field(encoded, "metadata", context,
                                          OutputFingerprintErrorCode::invalid_manifest);
    const auto& sha = required_field(encoded, "sha256", context,
                                     OutputFingerprintErrorCode::invalid_manifest);
    require_string(id, std::string(context) + ".id", OutputFingerprintErrorCode::invalid_manifest);
    require_object(metadata, std::string(context) + ".metadata",
                   OutputFingerprintErrorCode::invalid_manifest);
    validate_json_tree(metadata, std::string(context) + ".metadata",
                       OutputFingerprintErrorCode::invalid_manifest);
    require_string(sha, std::string(context) + ".sha256",
                   OutputFingerprintErrorCode::invalid_manifest);
    if (!is_sha256(sha.get_ref<const std::string&>())) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             std::string(context) + ".sha256 must be a lower-case SHA-256 digest");
    }
}

void validate_resource_array_json(const Json& encoded, std::string_view context,
                                  bool require_nonempty) {
    if (!encoded.is_array() || (require_nonempty && encoded.empty())) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             std::string(context) + " must be a non-empty resource array");
    }
    std::vector<std::string> ids;
    ids.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto item_context = std::string(context) + "[" + std::to_string(index) + "]";
        validate_resource_json(encoded.at(index), item_context);
        const auto id = encoded.at(index).at("id").get<std::string>();
        if (!ids.empty() && id < ids.back()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string(context) + " must be sorted by resource ID");
        }
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string(context) + " contains duplicate resource ID '" + id + "'");
        }
        ids.push_back(id);
    }
}

void validate_role_json(const Json& encoded, std::string_view role_name) {
    const auto context = std::string("processing_components.roles.") + std::string(role_name);
    require_object(encoded, context, OutputFingerprintErrorCode::invalid_manifest);
    require_only_keys(encoded, {"reason", "resources", "state"}, context,
                      OutputFingerprintErrorCode::invalid_manifest);
    const auto& reason = required_field(encoded, "reason", context,
                                        OutputFingerprintErrorCode::invalid_manifest);
    const auto& resources = required_field(encoded, "resources", context,
                                           OutputFingerprintErrorCode::invalid_manifest);
    const auto& state = required_field(encoded, "state", context,
                                       OutputFingerprintErrorCode::invalid_manifest);
    require_string(reason, context + ".reason", OutputFingerprintErrorCode::invalid_manifest, false);
    require_string(state, context + ".state", OutputFingerprintErrorCode::invalid_manifest);
    const auto parsed_state = state_from_name(
        state.get_ref<const std::string&>(), context + ".state",
        OutputFingerprintErrorCode::invalid_manifest);
    if (parsed_state == FingerprintGroupState::resources) {
        if (!reason.get_ref<const std::string&>().empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 context + " cannot include a not-applicable reason");
        }
        validate_resource_array_json(resources, context + ".resources", true);
    } else if (parsed_state == FingerprintGroupState::not_applicable) {
        if (reason.get_ref<const std::string&>().empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 context + " requires a not-applicable reason");
        }
        validate_resource_array_json(resources, context + ".resources", false);
        if (!resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 context + " not_applicable state cannot include resources");
        }
    } else {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             context + " must use resources or not_applicable");
    }
}

void validate_group_json(const Json& encoded, std::string_view name) {
    const bool is_processing = name == "processing_components";
    require_object(encoded, name, OutputFingerprintErrorCode::invalid_manifest);
    if (is_processing) {
        require_only_keys(encoded, {"reason", "resources", "roles", "state"}, name,
                          OutputFingerprintErrorCode::invalid_manifest);
    } else {
        require_only_keys(encoded, {"reason", "resources", "state"}, name,
                          OutputFingerprintErrorCode::invalid_manifest);
    }
    const auto& reason = required_field(encoded, "reason", name,
                                       OutputFingerprintErrorCode::invalid_manifest);
    const auto& resources = required_field(encoded, "resources", name,
                                           OutputFingerprintErrorCode::invalid_manifest);
    const auto& state = required_field(encoded, "state", name,
                                      OutputFingerprintErrorCode::invalid_manifest);
    require_string(reason, std::string(name) + ".reason",
                   OutputFingerprintErrorCode::invalid_manifest, false);
    require_string(state, std::string(name) + ".state",
                   OutputFingerprintErrorCode::invalid_manifest);
    const auto parsed_state = state_from_name(
        state.get_ref<const std::string&>(), std::string(name) + ".state",
        OutputFingerprintErrorCode::invalid_manifest);

    if (is_processing) {
        if (parsed_state != FingerprintGroupState::resources ||
            !reason.get_ref<const std::string&>().empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 "processing_components must use role-level resource states");
        }
        validate_resource_array_json(resources, "processing_components.resources", false);
        if (!resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 "processing_components cannot contain unscoped resources");
        }
        const auto& roles = required_field(encoded, "roles", name,
                                           OutputFingerprintErrorCode::invalid_manifest);
        require_object(roles, "processing_components.roles",
                       OutputFingerprintErrorCode::invalid_manifest);
        require_only_keys(roles, {"adapters", "kernel", "renderer", "solver"},
                          "processing_components.roles",
                          OutputFingerprintErrorCode::invalid_manifest);
        for (const auto role_name : kProcessingRoles) {
            const auto found = roles.find(std::string(role_name));
            if (found == roles.end()) {
                fail(OutputFingerprintErrorCode::invalid_manifest,
                     "processing_components is missing role '" + std::string(role_name) + "'");
            }
            validate_role_json(*found, role_name);
        }
        const auto& renderer = roles.at("renderer");
        if (renderer.at("state").get<std::string>() != "resources" ||
            renderer.at("resources").empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 "processing_components requires a renderer resource digest");
        }
        return;
    }

    if (parsed_state == FingerprintGroupState::resources) {
        if (!reason.get_ref<const std::string&>().empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string(name) + " resources state cannot include a reason");
        }
        validate_resource_array_json(resources, std::string(name) + ".resources", true);
        return;
    }
    if (parsed_state == FingerprintGroupState::no_resource ||
        parsed_state == FingerprintGroupState::not_applicable) {
        if (reason.get_ref<const std::string&>().empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string(name) + " requires an explicit reason");
        }
        validate_resource_array_json(resources, std::string(name) + ".resources", false);
        if (!resources.empty()) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string(name) + " empty state cannot include resources");
        }
        if (name == "application_build") {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 "application_build must contain an actual build digest");
        }
        return;
    }
}

void validate_document_json(const Json& document) {
    require_object(document, "manifest.document", OutputFingerprintErrorCode::invalid_manifest);
    require_only_keys(document,
                      {"asset_count", "document_id", "entity_count", "head_content_sha256",
                       "revision"},
                      "manifest.document", OutputFingerprintErrorCode::invalid_manifest);
    const auto& document_id = required_field(document, "document_id", "manifest.document",
                                             OutputFingerprintErrorCode::invalid_manifest);
    const auto& head_content_sha256 =
        required_field(document, "head_content_sha256", "manifest.document",
                       OutputFingerprintErrorCode::invalid_manifest);
    const auto& revision = required_field(document, "revision", "manifest.document",
                                          OutputFingerprintErrorCode::invalid_manifest);
    require_string(document_id, "manifest.document.document_id",
                   OutputFingerprintErrorCode::invalid_manifest);
    require_string(head_content_sha256, "manifest.document.head_content_sha256",
                   OutputFingerprintErrorCode::invalid_manifest);
    if (!is_sha256(head_content_sha256.get_ref<const std::string&>())) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "manifest.document.head_content_sha256 must be a lower-case SHA-256 digest");
    }
    for (const auto* count_name : {"asset_count", "entity_count"}) {
        const auto& count = required_field(document, count_name, "manifest.document",
                                            OutputFingerprintErrorCode::invalid_manifest);
        if ((!count.is_number_unsigned() && !count.is_number_integer())) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string("manifest.document.") + count_name +
                     " must be a non-negative integer");
        }
        if (count.is_number_integer() && !count.is_number_unsigned() &&
            count.get<std::int64_t>() < 0) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string("manifest.document.") + count_name +
                     " must be a non-negative integer");
        }
        try {
            (void)count.get<std::uint64_t>();
        } catch (const Json::exception&) {
            fail(OutputFingerprintErrorCode::invalid_manifest,
                 std::string("manifest.document.") + count_name +
                     " must be a non-negative integer");
        }
    }
    if (!revision.is_number_unsigned() && !revision.is_number_integer()) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "manifest.document.revision must be a non-negative integer");
    }
    if (revision.is_number_integer() && !revision.is_number_unsigned() &&
        revision.get<std::int64_t>() < 0) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "manifest.document.revision must be a non-negative integer");
    }
    try {
        (void)revision.get<std::uint64_t>();
    } catch (const Json::exception&) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "manifest.document.revision must be a non-negative integer");
    }
}

void validate_manifest(const Json& manifest) {
    require_object(manifest, "manifest", OutputFingerprintErrorCode::invalid_manifest);
    validate_json_tree(manifest, "manifest", OutputFingerprintErrorCode::invalid_manifest);
    require_only_keys(manifest, {"dependencies", "document", "schema_version"}, "manifest",
                      OutputFingerprintErrorCode::invalid_manifest);
    const auto& schema = required_field(manifest, "schema_version", "manifest",
                                        OutputFingerprintErrorCode::invalid_manifest);
    if (schema_value(schema, "manifest.schema_version",
                     OutputFingerprintErrorCode::invalid_manifest) !=
        kOutputFingerprintSchemaVersion) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "manifest schema version is unsupported");
    }
    validate_document_json(required_field(manifest, "document", "manifest",
                                          OutputFingerprintErrorCode::invalid_manifest));
    const auto& dependencies = required_field(manifest, "dependencies", "manifest",
                                              OutputFingerprintErrorCode::invalid_manifest);
    require_object(dependencies, "manifest.dependencies",
                   OutputFingerprintErrorCode::invalid_manifest);
    require_only_keys(dependencies,
                      {"application_build", "crs", "fonts", "processing_components", "profiles",
                       "views"},
                      "manifest.dependencies", OutputFingerprintErrorCode::invalid_manifest);
    for (const auto group_name : kDependencyGroups) {
        validate_group_json(required_field(dependencies, group_name, "manifest.dependencies",
                                           OutputFingerprintErrorCode::invalid_manifest),
                            group_name);
    }
}

void validate_fingerprint(const OutputFingerprint& fingerprint) {
    if (fingerprint.schema_version != kOutputFingerprintSchemaVersion) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "output fingerprint schema version is unsupported");
    }
    validate_manifest(fingerprint.manifest);
    if (!is_sha256(fingerprint.digest_sha256)) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "output fingerprint is missing a lower-case SHA-256 digest");
    }
    if (digest_of(canonical_dump(fingerprint.manifest,
                                 OutputFingerprintErrorCode::invalid_manifest, "manifest")) !=
        fingerprint.digest_sha256) {
        fail(OutputFingerprintErrorCode::invalid_manifest,
             "output fingerprint digest does not match its canonical manifest");
    }
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

OutputFingerprintError::OutputFingerprintError(OutputFingerprintErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

OutputFingerprintErrorCode OutputFingerprintError::code() const noexcept { return code_; }

std::string fingerprint_group_state_name(FingerprintGroupState state) {
    switch (state) {
        case FingerprintGroupState::unspecified:
            return "unspecified";
        case FingerprintGroupState::resources:
            return "resources";
        case FingerprintGroupState::no_resource:
            return "no_resource";
        case FingerprintGroupState::not_applicable:
            return "not_applicable";
    }
    return "unspecified";
}

OutputFingerprint make_output_fingerprint(const DocumentSnapshot& snapshot,
                                           const OutputFingerprintInputs& inputs) {
    auto manifest = make_manifest(snapshot, inputs);
    return OutputFingerprint{kOutputFingerprintSchemaVersion,
                             manifest,
                             digest_of(canonical_dump(manifest,
                                                      OutputFingerprintErrorCode::invalid_manifest,
                                                      "manifest"))};
}

Json serialize_output_fingerprint(const OutputFingerprint& fingerprint) {
    validate_fingerprint(fingerprint);
    return Json{{"digest_sha256", fingerprint.digest_sha256},
                {"manifest", fingerprint.manifest},
                {"schema_version", fingerprint.schema_version}};
}

std::optional<OutputFingerprint> deserialize_output_fingerprint(const Json& encoded,
                                                                std::string* error) {
    try {
        require_object(encoded, "output fingerprint", OutputFingerprintErrorCode::invalid_manifest);
        require_only_keys(encoded, {"digest_sha256", "manifest", "schema_version"},
                          "output fingerprint", OutputFingerprintErrorCode::invalid_manifest);
        const auto& schema = required_field(encoded, "schema_version", "output fingerprint",
                                            OutputFingerprintErrorCode::invalid_manifest);
        const auto& manifest = required_field(encoded, "manifest", "output fingerprint",
                                              OutputFingerprintErrorCode::invalid_manifest);
        const auto& digest = required_field(encoded, "digest_sha256", "output fingerprint",
                                            OutputFingerprintErrorCode::invalid_manifest);
        const auto parsed_schema = schema_value(schema, "output fingerprint.schema_version",
                                                OutputFingerprintErrorCode::invalid_manifest);
        require_object(manifest, "output fingerprint.manifest",
                       OutputFingerprintErrorCode::invalid_manifest);
        require_string(digest, "output fingerprint.digest_sha256",
                       OutputFingerprintErrorCode::invalid_manifest);
        OutputFingerprint fingerprint{parsed_schema, manifest, digest.get<std::string>()};
        validate_fingerprint(fingerprint);
        set_error(error, "");
        return fingerprint;
    } catch (const OutputFingerprintError& failure) {
        set_error(error, failure.what());
    } catch (const Json::exception& failure) {
        set_error(error, std::string("invalid output fingerprint JSON: ") + failure.what());
    } catch (const std::exception& failure) {
        set_error(error, std::string("invalid output fingerprint: ") + failure.what());
    }
    return std::nullopt;
}

OutputFingerprintCurrentness check_output_fingerprint_current(
    const OutputFingerprint& fingerprint, const DocumentSnapshot& snapshot,
    const OutputFingerprintInputs& inputs) {
    OutputFingerprintCurrentness result;
    try {
        validate_fingerprint(fingerprint);
        const auto candidate = make_output_fingerprint(snapshot, inputs);
        if (fingerprint.manifest.at("document") != candidate.manifest.at("document")) {
            result.changed_groups.emplace_back("document");
        }
        const auto& old_dependencies = fingerprint.manifest.at("dependencies");
        const auto& new_dependencies = candidate.manifest.at("dependencies");
        for (const auto group_name : kDependencyGroups) {
            if (old_dependencies.at(group_name) != new_dependencies.at(group_name)) {
                result.changed_groups.emplace_back(group_name);
            }
        }
        if (result.changed_groups.empty() &&
            fingerprint.digest_sha256 != candidate.digest_sha256) {
            result.changed_groups.emplace_back("manifest");
        }
        result.valid = true;
        result.current = result.changed_groups.empty();
    } catch (const OutputFingerprintError& failure) {
        result.error = failure.what();
    } catch (const std::exception& failure) {
        result.error = failure.what();
    }
    return result;
}

}  // namespace sketch
