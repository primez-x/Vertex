#pragma once

#include "sketch/document.hpp"
#include <string>
#include <string_view>

namespace sketch {
// TYPE is the owning workflow's explicit classification or declared category.
// This query needs context and facts, but no geometry (a new draft may be empty).
[[nodiscard]] std::string area_subtraction_type(const DocumentSnapshot&, const Entity&);
// Pure calculation adjustment. The subtractor may be newly encoded and absent
// from the snapshot. Only the returned target's deduction_ids can change.
// Additions require active phase members; removals can repair unavailable links.
[[nodiscard]] Entity prepare_area_subtraction_target(const DocumentSnapshot&,
    const Entity& subtractor, std::string_view target_id, bool remove = false);
// Validate a saved Finish target against its historical reconstruction rules.
// This only accepts an exact saved target and cannot prepare a new adjustment.
void validate_historical_area_subtraction_target(const DocumentSnapshot&,
    const Entity& subtractor, const Entity& expected_target);
}
