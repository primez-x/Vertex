#pragma once

#include "sketch/constraint_authoring.hpp"

namespace sketch {

struct PhaseWallReplacementRequest {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_wall_ids;
    bool operator==(const PhaseWallReplacementRequest&) const = default;
};

// Discover active baseline roots from a normalized semantic intent, without
// solving geometry or changing saved choices. Requests and seeds are sorted.
// Invalid owners or uninterpretable persistent relations throw invalid_argument.
[[nodiscard]] std::vector<PhaseWallReplacementRequest> phase_wall_replacement_requests(
    const std::map<std::string, Entity, std::less<>>& entities,
    const ConstraintAuthoringIntent& intent);

} // namespace sketch
