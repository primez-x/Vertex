#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

class DocumentSnapshot;

inline constexpr std::uint32_t kOutputFingerprintSchemaVersion = 1;

// Every dependency group must state why it has no entries. An unspecified
// group is rejected so that an omitted input cannot be mistaken for a valid
// no-resource output.
enum class FingerprintGroupState {
    unspecified,
    resources,
    no_resource,
    not_applicable,
};

struct FingerprintResource {
    std::string id;
    std::string sha256;
    nlohmann::json metadata = nlohmann::json::object();

    bool operator==(const FingerprintResource&) const = default;
};

struct FingerprintRole {
    FingerprintGroupState state = FingerprintGroupState::unspecified;
    std::vector<FingerprintResource> resources;
    // Required for not_applicable. It is serialized as part of the manifest
    // so the sentinel remains explicit and deterministic.
    std::string reason;

    bool operator==(const FingerprintRole&) const = default;
};

struct FingerprintDependencyGroup {
    FingerprintGroupState state = FingerprintGroupState::unspecified;
    std::vector<FingerprintResource> resources;
    // Required for no_resource and not_applicable; forbidden for resources.
    std::string reason;
    // Used only by processing_components. Its required roles are kernel,
    // solver, renderer, and adapters.
    std::map<std::string, FingerprintRole, std::less<>> roles;

    bool operator==(const FingerprintDependencyGroup&) const = default;
};

struct OutputFingerprintInputs {
    FingerprintDependencyGroup profiles;
    FingerprintDependencyGroup fonts;
    FingerprintDependencyGroup views;
    FingerprintDependencyGroup crs;
    FingerprintDependencyGroup processing_components;
    FingerprintDependencyGroup application_build;
};

struct OutputFingerprint {
    std::uint32_t schema_version = kOutputFingerprintSchemaVersion;
    // Compact canonical manifest covered by digest_sha256. The document
    // section stores identity, counts, and a head digest; it deliberately
    // does not retain entity data or raw asset bytes.
    nlohmann::json manifest = nlohmann::json::object();
    std::string digest_sha256;
};

enum class OutputFingerprintErrorCode {
    invalid_document,
    invalid_asset,
    invalid_dependency,
    invalid_manifest,
};

class OutputFingerprintError final : public std::runtime_error {
public:
    OutputFingerprintError(OutputFingerprintErrorCode code, std::string message);

    [[nodiscard]] OutputFingerprintErrorCode code() const noexcept;

private:
    OutputFingerprintErrorCode code_;
};

struct OutputFingerprintCurrentness {
    bool valid = false;
    bool current = false;
    std::vector<std::string> changed_groups;
    std::string error;
};

// Builds a versioned, canonical manifest for the immutable head snapshot.
// Throws OutputFingerprintError when any required document, asset, or
// dependency input is incomplete or malformed.
OutputFingerprint make_output_fingerprint(const DocumentSnapshot& snapshot,
                                           const OutputFingerprintInputs& inputs);

// Emits {schema_version, manifest, digest_sha256}. The digest covers the
// compact canonical JSON serialization of manifest, excluding the envelope.
nlohmann::json serialize_output_fingerprint(const OutputFingerprint& fingerprint);

// Validates the complete envelope, including the digest and dependency
// resource hashes. Asset bytes are intentionally absent and are therefore
// validated only while making or recomputing a fingerprint from a snapshot.
std::optional<OutputFingerprint> deserialize_output_fingerprint(
    const nlohmann::json& encoded, std::string* error = nullptr);

// Rebuilds a candidate fingerprint and identifies changed top-level groups.
// Invalid stored manifests or new inputs produce valid=false and an error.
OutputFingerprintCurrentness check_output_fingerprint_current(
    const OutputFingerprint& fingerprint, const DocumentSnapshot& snapshot,
    const OutputFingerprintInputs& inputs);

[[nodiscard]] std::string fingerprint_group_state_name(FingerprintGroupState state);

}  // namespace sketch
