#pragma once

#include "sketch/document.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace sketch {
struct AppraisalPartitionAssignment {
    std::string parent_id;
    // Complete replacement, including retained deductions. Empty removes all.
    std::vector<std::string> deduction_ids;
};

// Resolves the owning property and validates an explicitly declared ANSI v1/v2
// policy. Missing eligibility observations do not block geometric authoring.
// Malformed declarations throw; other supported workflows return false.
[[nodiscard]] bool ansi_appraisal_partition_context(
    const DocumentSnapshot& source, std::string_view boundary_id);

// Pure preparation against a candidate containing any provisional new areas.
// All replacements are overlaid before validating reachable dependency graphs.
// Immediate child gross geometry is deducted once at each node; overlapping
// voids retain calculate_area's union behavior. Shared descendants are allowed.
// Returns copies in assignment order with only deduction_ids changed. Empty
// replacements erase that property and do not validate removed dependencies.
// Does not infer appraisal facts or alter legacy Auto-Subtract/history rules.
[[nodiscard]] std::vector<Entity> prepare_ansi_appraisal_partition_targets(
    const DocumentSnapshot& source,
    const std::vector<AppraisalPartitionAssignment>& assignments);
} // namespace sketch
