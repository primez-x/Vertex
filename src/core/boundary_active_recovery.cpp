#include "sketch/boundary_active_recovery.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument("boundary active recovery: " + std::string(message));
}
void exact_keys(const Json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) invalid("unexpected object fields");
    for (const auto* key : keys) {
        if (!value.contains(key)) invalid("missing required field");
    }
}
std::uint64_t unsigned_integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0)
        invalid("expected nonnegative integer");
    return static_cast<std::uint64_t>(value.get<std::int64_t>());
}
std::uint64_t positive_version(const Json& envelope, const char* key) {
    if (!envelope.contains(key)) invalid("missing version discriminator");
    const auto version = unsigned_integer(envelope.at(key));
    if (version == 0) invalid("version discriminator must be positive");
    return version;
}
void identifier(std::string_view value, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > 128)
        invalid("identifier must contain at most 128 bytes and satisfy emptiness policy");
}
void validate_source(const BoundaryRecoverySource& source) {
    identifier(source.document_id);
    const auto& digest = source.authoring_digest;
    if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) invalid("authoring digest must be 64 lowercase hexadecimal characters");
    identifier(source.context.property_id, true);
    identifier(source.context.building_id, true);
    identifier(source.context.floor_id, true);
    identifier(source.context.layer_id, true);
}
const std::string& string(const Json& value) {
    if (!value.is_string()) invalid("expected string");
    return value.get_ref<const std::string&>();
}
BoundaryRecoverySource read_source(const Json& value) {
    exact_keys(value, {"document_id", "revision", "authoring_digest", "context"});
    const auto& context = value.at("context");
    exact_keys(context, {"property_id", "building_id", "floor_id", "layer_id"});
    BoundaryRecoverySource source{
        string(value.at("document_id")), unsigned_integer(value.at("revision")),
        string(value.at("authoring_digest")),
        {string(context.at("property_id")), string(context.at("building_id")),
         string(context.at("floor_id")), string(context.at("layer_id"))}};
    validate_source(source);
    return source;
}
Json write_source(const BoundaryRecoverySource& source) {
    return {{"document_id", source.document_id}, {"revision", source.revision},
            {"authoring_digest", source.authoring_digest},
            {"context", {{"property_id", source.context.property_id},
                         {"building_id", source.context.building_id},
                         {"floor_id", source.context.floor_id},
                         {"layer_id", source.context.layer_id}}}};
}
}  // namespace

Json encode_boundary_active_recovery(const BoundaryActiveRecovery& value,
                                     const BoundaryAuthoringResourcePolicy& policy) {
    validate_source(value.source);
    if (!value.extensions.is_object()) invalid("extensions must be an object");
    // Preflight caller-owned extension trees before copying them into a record.
    detail::validate_authoring_recovery_json(value.extensions, policy);
    auto checkpoint = encode_boundary_authoring_recovery(value.checkpoint, policy);
    Json result{{"version", 1}, {"replay_version", 1}, {"source", write_source(value.source)},
                {"checkpoint", std::move(checkpoint)}, {"extensions", value.extensions}};
    detail::validate_authoring_recovery_json(result, policy);
    return result;
}

BoundaryActiveRecoveryDecodeResult decode_boundary_active_recovery(
    const Json& envelope, const BoundaryAuthoringResourcePolicy& policy) {
    // Includes future schemas: bound the borrowed whole tree before any copy.
    detail::validate_authoring_recovery_json(envelope, policy);
    if (!envelope.is_object()) invalid("envelope must be an object");
    const auto version = positive_version(envelope, "version");
    const auto replay_version = positive_version(envelope, "replay_version");
    if (version != 1 || replay_version != 1)
        return {std::nullopt, envelope, "unsupported boundary active recovery version or replay version"};
    exact_keys(envelope, {"version", "replay_version", "source", "checkpoint", "extensions"});
    // A future checkpoint dialect makes the enclosing record wholly opaque.
    // Do not interpret provenance or produce a partially recovered record.
    auto checkpoint = decode_boundary_authoring_recovery(envelope.at("checkpoint"), policy);
    if (checkpoint.opaque()) return {std::nullopt, envelope, checkpoint.diagnostic};
    if (!checkpoint.supported()) invalid("checkpoint did not decode");
    auto source = read_source(envelope.at("source"));
    if (!envelope.at("extensions").is_object()) invalid("extensions must be an object");
    return {BoundaryActiveRecovery{std::move(source), std::move(*checkpoint.checkpoint),
                                   envelope.at("extensions")}, std::nullopt, {}};
}

}  // namespace sketch
