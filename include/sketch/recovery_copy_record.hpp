#pragma once

#include "sketch/boundary_authoring_resource_policy.hpp"
#include "sketch/document.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace sketch {

// Recovery provenance and save anchors, not proof of ownership or edit authority.
// Paths are stored text only; the codec never resolves or opens them.
struct RecoveryCopyRecord {
    std::string archive_id;
    std::string owner_token;
    std::string document_id;
    std::optional<std::string> source_path;
    std::optional<std::string> source_sha256;
    std::optional<Revision> explicitly_saved_document_revision;
    std::uint64_t explicit_save_generation{};
    std::uint64_t workspace_epoch{};
    std::uint64_t edited_generation{};
    std::uint64_t checkpoint_generation{};
    std::uint64_t autosaved_checkpoint_generation{};
    std::uint64_t saved_edited_generation{};
    nlohmann::json ownership = nlohmann::json::object();
    nlohmann::json extensions = nlohmann::json::object();
    bool operator==(const RecoveryCopyRecord&) const = default;
};

struct RecoveryCopyDecodeResult {
    std::optional<RecoveryCopyRecord> record;
    std::optional<nlohmann::json> original_envelope;
    std::string diagnostic;

    [[nodiscard]] bool supported() const noexcept { return record.has_value(); }
    [[nodiscard]] bool opaque() const noexcept {
        return !record.has_value() && original_envelope.has_value();
    }
};

// Malformed known schemas and budget violations throw std::invalid_argument.
// Unknown positive outer versions preserve the entire bounded envelope.
[[nodiscard]] nlohmann::json encode_recovery_copy_record(
    const RecoveryCopyRecord& value,
    const BoundaryAuthoringResourcePolicy& policy = boundary_authoring_default_resource_policy);
[[nodiscard]] RecoveryCopyDecodeResult decode_recovery_copy_record(
    const nlohmann::json& envelope,
    const BoundaryAuthoringResourcePolicy& policy = boundary_authoring_default_resource_policy);

// Checks record validity, identity, and the nullable retained explicit-save anchor.
// Does not alter saved markers, dirty state, or read-only protection.
void validate_recovery_copy_document(const RecoveryCopyRecord& record,
                                     const DocumentSnapshot& document,
                                     const BoundaryAuthoringResourcePolicy& policy =
                                         boundary_authoring_default_resource_policy);

}  // namespace sketch
