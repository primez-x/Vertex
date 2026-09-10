#include "sketch/boundary_authoring_recovery_resource.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace sketch {
bool boundary_authoring_recovery_checked_add(std::size_t left, std::size_t right,
                                             std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

bool boundary_authoring_recovery_checked_multiply(std::size_t left, std::size_t right,
                                                  std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) return false;
    result = left * right;
    return true;
}

std::size_t boundary_authoring_recovery_saturating_add(std::size_t left,
                                                       std::size_t right,
                                                       bool& saturated) noexcept {
    std::size_t result = 0;
    if (!boundary_authoring_recovery_checked_add(left, right, result)) {
        saturated = true;
        return std::numeric_limits<std::size_t>::max();
    }
    // Preserve an overflow reported by an earlier composed operation.
    return result;
}

std::size_t boundary_authoring_recovery_saturating_multiply(std::size_t left,
                                                            std::size_t right,
                                                            bool& saturated) noexcept {
    std::size_t result = 0;
    if (!boundary_authoring_recovery_checked_multiply(left, right, result)) {
        saturated = true;
        return std::numeric_limits<std::size_t>::max();
    }
    // Preserve an overflow reported by an earlier composed operation.
    return result;
}

namespace detail {
void validate_authoring_usage(const BoundaryAuthoringResourceUsage& u,
                              const BoundaryAuthoringResourcePolicy& p) {
    if (u.action_count > p.max_actions || u.generated_ids > p.max_total_generated_ids ||
        u.encoded_bytes > p.max_encoded_bytes || u.json_values > p.max_json_values ||
        u.string_bytes > p.max_string_bytes || u.json_depth > p.max_json_depth ||
        u.replay_work > p.max_replay_work || u.retained_history_bytes > p.max_retained_history_bytes ||
        u.operation_bytes > p.max_operation_bytes || u.materialization_bytes > p.max_materialization_bytes ||
        u.cumulative_replay_copy_bytes > p.max_cumulative_replay_copy_bytes)
        throw std::invalid_argument("boundary authoring resource policy exceeded");
}
static std::size_t strings(std::initializer_list<std::string_view> values) {
    std::size_t bytes = 0;
    for (const auto value : values) {
        std::size_t extra;
        if (!boundary_authoring_recovery_checked_add(value.size(), 33, extra) ||
            !boundary_authoring_recovery_checked_multiply(extra, 2, extra) ||
            !boundary_authoring_recovery_checked_add(bytes, extra, bytes))
            throw std::invalid_argument("string accounting overflow");
    }
    return bytes;
}
std::size_t authoring_dynamic_bytes(const ConstructionReceipt& r) {
    auto result = strings({r.segment_id});
    const auto add = [&](std::size_t bytes) {
        if (!boundary_authoring_recovery_checked_add(result, bytes, result))
            throw std::invalid_argument("receipt string accounting overflow");
    };
    for (const auto* q : {&r.distance, &r.rise, &r.run, &r.height, &r.arc_length}) {
        if (*q) add(strings({(*q)->original_expression}));
    }
    for (const auto* a : {&r.heading, &r.turn, &r.angle, &r.tangent, &r.sweep}) {
        if (*a) add(strings({(*a)->original_expression, (*a)->normalized_expression}));
    }
    return result;
}
std::size_t authoring_dynamic_bytes(const IdentifiedSegment& s) {
    return strings({s.segment_id, s.start_vertex_id, s.end_vertex_id});
}
std::size_t authoring_dynamic_bytes(const BoundaryDimension& d) {
    return strings({d.id, d.boundary_id, d.segment_id});
}
std::size_t authoring_dynamic_bytes(const PendingBoundaryDimension& d) {
    return strings({d.boundary_id, d.segment_id});
}
} // namespace detail

BoundaryAuthoringResourceUsage estimate_boundary_authoring_recovery_resources(
    const BoundaryAuthoringCheckpoint& checkpoint, const BoundaryAuthoringResourcePolicy& policy) {
    detail::validate_authoring_checkpoint_raw(checkpoint, policy);
    const auto session = BoundaryAuthoringSession::from_recovery_checkpoint(checkpoint, policy);
    return session.resource_usage();
}
} // namespace sketch