#pragma once

#include "sketch/phase_constraint_authoring.hpp"

namespace sketch {

// Closed inner v1: version and five nullable full historical phase-authoring
// envelopes. At least two demolition families bind the same captured source,
// saved registry and alternative. Existing leaf dialects keep their meaning.
[[nodiscard]] nlohmann::json encode_phase_coordinated_demolition(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Validated full leaf envelopes in opening, roof, slab, structural, stair order.
[[nodiscard]] std::vector<PhaseConstraintAuthoringIntent> phase_coordinated_demolition_components(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Every leaf independently replays the SAME actual source. Only the exported
// semantic demolition composer may combine their known changed/retired rows.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_phase_coordinated_demolition(
    const std::map<std::string, Entity, std::less<>>& source,
    const PhaseConstraintAuthoringIntent& enclosing);

} // namespace sketch
