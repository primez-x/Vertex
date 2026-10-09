#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace sketch {
struct Entity;

inline constexpr std::string_view roof_join_phase_ownership_extension_key =
    "roof_join_phase_ownership";

// Only roof_join recognizes this extension. Known version 1 is closed and
// validated; unsupported future versions remain opaque and grant no authority.
// Malformed known metadata throws std::invalid_argument.
[[nodiscard]] bool has_phase_qualified_roof_join_ownership(const Entity& entity);

// Validate actual join members and global ownership. Sharing needs a known
// qualifier on at least one join, unique membership of both joins in the same
// actual ModelPhases registry, and exclusion in baseline and every alternative.
// Unqualified documents retain global roof-member exclusivity without decoding
// their phase registries. Throws std::invalid_argument on invalid ownership.
void validate_roof_join_ownership(
    const std::map<std::string, Entity, std::less<>>& actual_entities);

// Validate the same actual ownership policy and return known qualified joins
// plus their shared-member counterparts. Only this cohort may omit inactive
// preserved joins from current-design native geometry admission.
[[nodiscard]] std::set<std::string, std::less<>> phase_qualified_roof_join_cohort_ids(
    const std::map<std::string, Entity, std::less<>>& actual_entities);

}  // namespace sketch
