#pragma once

#include "sketch/constraint_authoring.hpp"

namespace sketch {

// Mapping authority comes from the independently reconstructed actual wall
// replacement plan, including its fresh typed children. This pure helper only
// remaps typed semantic identities; source admission and geometry/receipt
// validation remain the responsibility of core authoring replay. Opaque JSON,
// quantity expressions, coordinates and presentation messages are not rewritten.
// Ambiguous identity mappings or colliding operator targets throw
// std::invalid_argument rather than silently replacing another target.
[[nodiscard]] ConstraintAuthoringIntent remap_phase_wall_replacement_authoring_intent(
    const ConstraintAuthoringIntent& intent,
    const std::map<std::string, std::string, std::less<>>& original_to_proposed);

} // namespace sketch
