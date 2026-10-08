#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct PhaseOpeningDemolitionIntent {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> opening_ids;
    bool operator==(const PhaseOpeningDemolitionIntent&) const = default;
};

[[nodiscard]] nlohmann::json encode_phase_opening_demolition_intent(
    const PhaseOpeningDemolitionIntent& intent);
[[nodiscard]] PhaseOpeningDemolitionIntent decode_phase_opening_demolition_intent(
    const nlohmann::json& value);
// Independently derives the sole registry patch from actual saved memberships.
// All retained owners/dependents and all other registry records remain exact.
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_opening_demolition_entities(
    const std::map<std::string,Entity,std::less<>>& source,
    const PhaseOpeningDemolitionIntent& intent);

// Marks actual shared-baseline openings demolished in their saved active
// alternative. Only that registry changes; owners and all dependents survive.
// No qualified selection returns nullopt. Mixed selections, multiple target
// registries and unregistered openings inheriting a shared host refuse.
// Publication must retain the caller's full captured-source/selection fence.
// Exclusive source-bound phase authoring establishes saved-active validation
// even when the original project's history still uses legacy validation.
[[nodiscard]] std::optional<ApplyBoundaryConstraintChanges> phase_opening_demolition_command(
    const DocumentSnapshot& source, const std::vector<std::string>& selected_opening_ids);

} // namespace sketch
