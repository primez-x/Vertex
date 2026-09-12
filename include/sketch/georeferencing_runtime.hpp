#pragma once

#include "sketch/georeferencing_contract.hpp"
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sketch {

struct GeoreferencingRuntimeOptions {
    // The root is deliberately supplied by the caller so the runtime never
    // searches the host or downloads missing coordinate resources.
    std::filesystem::path resource_root;
    std::uint64_t maximum_resource_bytes{128ULL * 1024 * 1024};
};

struct VerifiedGeoreferencingResource {
    std::string relative_path;
    std::string sha256;
    std::uint64_t size_bytes{};
};

struct GeoreferencingRuntimeReport {
    std::string proj_version;
    std::string crs_identifier;
    std::string database_relative_path;
    bool network_enabled{};
    bool crs_identifier_validated{};
    bool crs_definition_validated{};
    bool identity_transform_validated{};
    std::vector<VerifiedGeoreferencingResource> resources;

    [[nodiscard]] nlohmann::json to_json() const;
};

// Verify the complete local PROJ preflight for a contract. This validates
// containment and hashes for every declared resource, binds PROJ to the
// declared database/search tree, disables networking in that context, and
// constructs both CRS declarations and an identity operation. The affine
// mapping in GeoreferencingContract remains the authoritative application
// transform; this function does not infer or fit a transform.
[[nodiscard]] GeoreferencingRuntimeReport verify_georeferencing_runtime(
    const GeoreferencingContract& contract,
    const GeoreferencingRuntimeOptions& options);

}  // namespace sketch
