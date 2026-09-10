#pragma once

#include "sketch/boundary_authoring_session.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace sketch {

enum class BoundaryAuthoringRecoveryFormat {
    supported_v1,
    unsupported_version,
};

struct BoundaryAuthoringRecoveryVersion {
    BoundaryAuthoringRecoveryFormat format{};
    std::optional<std::uint64_t> version;
    std::string diagnostic;
};

struct BoundaryAuthoringRecoveryDecodeResult {
    std::optional<BoundaryAuthoringCheckpoint> checkpoint;
    std::optional<nlohmann::json> original_envelope;
    std::optional<std::uint64_t> version;
    std::optional<std::uint64_t> replay_version;
    std::string diagnostic;

    [[nodiscard]] bool supported() const noexcept { return checkpoint.has_value(); }
    [[nodiscard]] bool opaque() const noexcept {
        return !checkpoint.has_value() && original_envelope.has_value();
    }
};

[[nodiscard]] BoundaryAuthoringRecoveryVersion inspect_boundary_authoring_recovery(
    const nlohmann::json& envelope,
    const BoundaryAuthoringRecoveryLimits& limits =
        boundary_authoring_recovery_default_limits);

[[nodiscard]] BoundaryAuthoringRecoveryDecodeResult decode_boundary_authoring_recovery(
    const nlohmann::json& envelope,
    const BoundaryAuthoringRecoveryLimits& limits =
        boundary_authoring_recovery_default_limits);

[[nodiscard]] nlohmann::json encode_boundary_authoring_recovery(
    const BoundaryAuthoringCheckpoint& checkpoint,
    const BoundaryAuthoringRecoveryLimits& limits =
        boundary_authoring_recovery_default_limits);

// Naming aliases keep the typed checkpoint terminology convenient for callers
// that do not need to mention the envelope layer.
[[nodiscard]] inline nlohmann::json encode_boundary_authoring_checkpoint(
    const BoundaryAuthoringCheckpoint& checkpoint,
    const BoundaryAuthoringRecoveryLimits& limits =
        boundary_authoring_recovery_default_limits) {
    return encode_boundary_authoring_recovery(checkpoint, limits);
}

[[nodiscard]] inline BoundaryAuthoringRecoveryDecodeResult decode_boundary_authoring_checkpoint(
    const nlohmann::json& envelope,
    const BoundaryAuthoringRecoveryLimits& limits =
        boundary_authoring_recovery_default_limits) {
    return decode_boundary_authoring_recovery(envelope, limits);
}

}  // namespace sketch
