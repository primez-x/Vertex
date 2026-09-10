#include "sketch/recovery_copy_record.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument("recovery copy: " + std::string(message));
}
void preflight(const Json& value, const BoundaryAuthoringResourcePolicy& policy) {
    try {
        detail::validate_authoring_recovery_json(value, policy);
    } catch (const Json::exception&) {
        invalid("invalid portable JSON encoding");
    }
}
void exact_keys(const Json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) invalid("unexpected object fields");
    for (const auto* key : keys)
        if (!value.contains(key)) invalid("missing required field");
}
std::uint64_t unsigned_integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0)
        invalid("expected nonnegative integer");
    return static_cast<std::uint64_t>(value.get<std::int64_t>());
}
std::uint64_t positive_version(const Json& envelope, const char* key) {
    if (!envelope.contains(key)) invalid("missing version discriminator");
    const auto result = unsigned_integer(envelope.at(key));
    if (result == 0) invalid("version discriminator must be positive");
    return result;
}
const std::string& string(const Json& value) {
    if (!value.is_string()) invalid("expected string");
    return value.get_ref<const std::string&>();
}
std::optional<std::string> optional_string(const Json& value) {
    if (value.is_null()) return std::nullopt;
    return string(value);
}
void validate_value(const RecoveryCopyRecord& value) {
    for (const auto* id : {&value.archive_id, &value.owner_token, &value.document_id})
        if (id->empty() || id->size() > 128) invalid("identifier must contain 1 to 128 bytes");
    if (value.source_path && (value.source_path->empty() || value.source_path->size() > 131072 ||
                              value.source_path->find('\0') != std::string::npos))
        invalid("source path must contain 1 to 131072 UTF-8 bytes without NUL");
    if (value.source_sha256 && (value.source_sha256->size() != 64 ||
        !std::all_of(value.source_sha256->begin(), value.source_sha256->end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))) invalid("source hash must be 64 lowercase hexadecimal characters");
    if (value.autosaved_checkpoint_generation > value.checkpoint_generation ||
        value.saved_edited_generation > value.edited_generation ||
        value.explicit_save_generation > value.edited_generation)
        invalid("save watermark exceeds its corresponding generation");
    if (!value.ownership.is_object() || !value.extensions.is_object())
        invalid("ownership and extensions must be objects");
}
template<class T> Json nullable(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}
}  // namespace

Json encode_recovery_copy_record(const RecoveryCopyRecord& value,
                                 const BoundaryAuthoringResourcePolicy& policy) {
    validate_value(value);
    // Bound borrowed metadata before copying it into the envelope.
    preflight(value.ownership, policy);
    preflight(value.extensions, policy);
    Json result{{"version", 1}, {"replay_version", 1}, {"role", "recovery_copy"},
                {"archive_id", value.archive_id}, {"owner_token", value.owner_token},
                {"document_id", value.document_id}, {"source_path", nullable(value.source_path)},
                {"source_sha256", nullable(value.source_sha256)},
                {"explicitly_saved_document_revision", nullable(value.explicitly_saved_document_revision)},
                {"explicit_save_generation", value.explicit_save_generation},
                {"workspace_epoch", value.workspace_epoch}, {"edited_generation", value.edited_generation},
                {"checkpoint_generation", value.checkpoint_generation},
                {"autosaved_checkpoint_generation", value.autosaved_checkpoint_generation},
                {"saved_edited_generation", value.saved_edited_generation},
                {"ownership", value.ownership}, {"extensions", value.extensions}};
    preflight(result, policy);
    return result;
}

RecoveryCopyDecodeResult decode_recovery_copy_record(
    const Json& envelope, const BoundaryAuthoringResourcePolicy& policy) {
    preflight(envelope, policy);
    if (!envelope.is_object()) invalid("envelope must be an object");
    const auto version = positive_version(envelope, "version");
    const auto replay_version = positive_version(envelope, "replay_version");
    if (version != 1 || replay_version != 1)
        return {std::nullopt, envelope, "unsupported recovery copy version or replay version"};
    exact_keys(envelope, {"version", "replay_version", "role", "archive_id", "owner_token", "document_id",
                         "source_path", "source_sha256", "explicitly_saved_document_revision",
                         "explicit_save_generation", "workspace_epoch", "edited_generation",
                         "checkpoint_generation", "autosaved_checkpoint_generation", "saved_edited_generation",
                         "ownership", "extensions"});
    if (string(envelope.at("role")) != "recovery_copy") invalid("unexpected archive role");
    RecoveryCopyRecord value;
    value.archive_id = string(envelope.at("archive_id"));
    value.owner_token = string(envelope.at("owner_token"));
    value.document_id = string(envelope.at("document_id"));
    value.source_path = optional_string(envelope.at("source_path"));
    value.source_sha256 = optional_string(envelope.at("source_sha256"));
    if (!envelope.at("explicitly_saved_document_revision").is_null())
        value.explicitly_saved_document_revision = unsigned_integer(envelope.at("explicitly_saved_document_revision"));
    value.explicit_save_generation = unsigned_integer(envelope.at("explicit_save_generation"));
    value.workspace_epoch = unsigned_integer(envelope.at("workspace_epoch"));
    value.edited_generation = unsigned_integer(envelope.at("edited_generation"));
    value.checkpoint_generation = unsigned_integer(envelope.at("checkpoint_generation"));
    value.autosaved_checkpoint_generation = unsigned_integer(envelope.at("autosaved_checkpoint_generation"));
    value.saved_edited_generation = unsigned_integer(envelope.at("saved_edited_generation"));
    value.ownership = envelope.at("ownership");
    value.extensions = envelope.at("extensions");
    validate_value(value);
    return {std::move(value), std::nullopt, {}};
}

void validate_recovery_copy_document(const RecoveryCopyRecord& record,
                                     const DocumentSnapshot& document,
                                     const BoundaryAuthoringResourcePolicy& policy) {
    (void)encode_recovery_copy_record(record, policy);
    if (record.document_id != document.document_id()) invalid("document identity mismatch");
    if (record.explicitly_saved_document_revision &&
        std::none_of(document.history().begin(), document.history().end(), [&](const RevisionRecord& revision) {
            return revision.revision == *record.explicitly_saved_document_revision;
        })) invalid("explicit-save revision is absent from retained document history");
}

}  // namespace sketch
