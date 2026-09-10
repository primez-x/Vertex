#pragma once

#include "sketch/boundary_authoring_session.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace sketch {

namespace detail {
// Shared portable-JSON preflight for enclosing recovery records. Checks the
// borrowed tree before callers copy it or interpret a known schema.
void validate_authoring_recovery_json(const nlohmann::json&,
                                      const BoundaryAuthoringResourcePolicy&);
// Conservative raw wire accounting for an enclosing recovery JSON tree. The
// returned usage has no live-history memory guarantees.
[[nodiscard]] BoundaryAuthoringResourceUsage measure_authoring_recovery_json(
    const nlohmann::json&, const BoundaryAuthoringResourcePolicy&);
[[nodiscard]] BoundaryAuthoringResourceUsage authoring_context_usage(
    const BoundaryAuthoringOptions&, BoundaryAuthoringMode, std::string_view,
    const nlohmann::json&, const BoundaryAuthoringResourcePolicy&);
[[nodiscard]] BoundaryAuthoringResourceUsage authoring_action_usage(
    const BoundaryAuthoringAction&, const BoundaryAuthoringResourcePolicy&);
void validate_authoring_usage(const BoundaryAuthoringResourceUsage&,
                              const BoundaryAuthoringResourcePolicy&);
void validate_authoring_checkpoint_raw(const BoundaryAuthoringCheckpoint&,
                                      const BoundaryAuthoringResourcePolicy&);
// Conservative raw wire accounting for a typed checkpoint. Only encoded
// bytes, JSON values/string bytes/depth, generated IDs, replay work, and
// action count are reported; live-history memory is not measured or guaranteed.
[[nodiscard]] BoundaryAuthoringResourceUsage measure_authoring_checkpoint_raw(
    const BoundaryAuthoringCheckpoint&, const BoundaryAuthoringResourcePolicy&);
[[nodiscard]] std::size_t authoring_dynamic_bytes(const ConstructionReceipt&);
[[nodiscard]] std::size_t authoring_dynamic_bytes(const IdentifiedSegment&);
[[nodiscard]] std::size_t authoring_dynamic_bytes(const BoundaryDimension&);
[[nodiscard]] std::size_t authoring_dynamic_bytes(const PendingBoundaryDimension&);
}

// Checked arithmetic is available to callers that need to compose this
// estimator with an enclosing budget. false means result was not written.
[[nodiscard]] bool boundary_authoring_recovery_checked_add(
    std::size_t left, std::size_t right, std::size_t& result) noexcept;
[[nodiscard]] bool boundary_authoring_recovery_checked_multiply(
    std::size_t left, std::size_t right, std::size_t& result) noexcept;

// Saturating forms never wrap. saturated is set when the mathematical result
// does not fit in size_t; estimator validation treats that condition as an
// invalid overflow rather than silently truncating the report. A true flag is sticky.
[[nodiscard]] std::size_t boundary_authoring_recovery_saturating_add(
    std::size_t left, std::size_t right, bool& saturated) noexcept;
[[nodiscard]] std::size_t boundary_authoring_recovery_saturating_multiply(
    std::size_t left, std::size_t right, bool& saturated) noexcept;

// Explicit bounded canonical replay. Raw limits are checked against the
// caller-owned checkpoint before replay allocations; live edits use stored
// incremental usage and never call this estimator.
[[nodiscard]] BoundaryAuthoringResourceUsage estimate_boundary_authoring_recovery_resources(
    const BoundaryAuthoringCheckpoint& checkpoint,
    const BoundaryAuthoringResourcePolicy& policy = boundary_authoring_default_resource_policy);

}  // namespace sketch
