#pragma once

#include "sketch/boundary_authoring_recovery.hpp"
#include "sketch/boundary_recovery_source.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace sketch {

// Serializable recovery state and provenance. Decoding does not grant authority
// to finalize; inspect the source against the current document separately.
struct BoundaryActiveRecovery {
    BoundaryRecoverySource source;
    BoundaryAuthoringCheckpoint checkpoint;
    nlohmann::json extensions = nlohmann::json::object();
    bool operator==(const BoundaryActiveRecovery&) const = default;
};

struct BoundaryActiveRecoveryDecodeResult {
    std::optional<BoundaryActiveRecovery> active;
    std::optional<nlohmann::json> original_envelope;
    std::string diagnostic;

    [[nodiscard]] bool supported() const noexcept { return active.has_value(); }
    [[nodiscard]] bool opaque() const noexcept {
        return !active.has_value() && original_envelope.has_value();
    }
};

// Invalid known schemas and resource violations throw std::invalid_argument.
// Future positive schema/replay versions preserve the entire bounded envelope.
[[nodiscard]] nlohmann::json encode_boundary_active_recovery(
    const BoundaryActiveRecovery& value,
    const BoundaryAuthoringResourcePolicy& policy = boundary_authoring_default_resource_policy);
[[nodiscard]] BoundaryActiveRecoveryDecodeResult decode_boundary_active_recovery(
    const nlohmann::json& envelope,
    const BoundaryAuthoringResourcePolicy& policy = boundary_authoring_default_resource_policy);

}  // namespace sketch
