#pragma once

#include <cstddef>

namespace sketch {

// Engineering accounting ceilings, not measured RSS or a hardware SLA.
// One policy travels with the session and applies to live edits and recovery.
struct BoundaryAuthoringResourcePolicy {
    std::size_t max_encoded_bytes{16U * 1024U * 1024U};
    // Portable hard ceiling is 64; callers may lower it.
    std::size_t max_json_depth{64};
    std::size_t max_json_values{250'000};
    std::size_t max_string_bytes{1U * 1024U * 1024U};
    std::size_t max_actions{100'000};
    std::size_t max_generated_ids_per_action{64};
    std::size_t max_total_generated_ids{500'000};
    std::size_t max_chain_edges{100'000};
    std::size_t max_dimensions_per_chain{100'000};
    std::size_t max_replay_work{20'000'000};
    std::size_t max_retained_history_bytes{256U * 1024U * 1024U};
    std::size_t max_operation_bytes{512U * 1024U * 1024U};
    std::size_t max_materialization_bytes{512U * 1024U * 1024U};
    std::size_t max_cumulative_replay_copy_bytes{1024U * 1024U * 1024U};
    bool operator==(const BoundaryAuthoringResourcePolicy&) const = default;
};

inline constexpr BoundaryAuthoringResourcePolicy boundary_authoring_default_resource_policy{};
using BoundaryAuthoringRecoveryLimits = BoundaryAuthoringResourcePolicy;
inline constexpr BoundaryAuthoringRecoveryLimits boundary_authoring_recovery_default_limits{};

// Cumulative prefix counters; branch publication starts from the cursor's
// prefix. Retained bytes reserve every possible undo zipper node in advance.
struct BoundaryAuthoringResourceUsage {
    std::size_t action_count{};
    std::size_t generated_ids{};
    std::size_t encoded_bytes{};
    std::size_t json_values{};
    std::size_t string_bytes{};
    std::size_t json_depth{};
    std::size_t replay_work{};
    std::size_t retained_history_bytes{};
    // Canonical retained state plus materialization reservation. Replacement
    // also checks old reachable history plus candidate materialization as an
    // ephemeral operation charge, without storing discarded-redo-dependent data.
    std::size_t operation_bytes{};
    std::size_t materialization_bytes{};
    std::size_t cumulative_replay_copy_bytes{};
    bool operator==(const BoundaryAuthoringResourceUsage&) const = default;
};

} // namespace sketch
