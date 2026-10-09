#pragma once

#include "sketch/phase_constraint_authoring.hpp"

#include <optional>
#include <utility>

namespace sketch {

// Closed inner v1: version and five nullable full historical phase-authoring
// envelopes. At least two demolition families bind the same captured source,
// saved registry and alternative. Existing leaf dialects keep their meaning.
// Closed inner v2 adds exactly ordinary_removal: one or more historical families
// plus nonempty actual ordinary/proposed roots or qualified catalog instances.
// It grants no entity/geometry payload authority and retains the exclusive outer
// v15 enclosure. V1 continues to preserve its exact canonical bytes.
[[nodiscard]] nlohmann::json encode_phase_coordinated_demolition(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Validated full leaf envelopes in opening, roof, slab, structural, stair order.
[[nodiscard]] std::vector<PhaseConstraintAuthoringIntent> phase_coordinated_demolition_components(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

struct PhaseCoordinatedOrdinaryRemoval {
    std::vector<std::string> object_ids;
    // Actual qualified catalog/instance keys; never presentation aliases.
    std::vector<std::pair<std::string, std::string>> components;
};

// Validates the complete enclosure, including historical children and source
// bindings. V1 has no ordinary selection and returns nullopt. V2 selections
// are canonical ascending unique ASCII identities with aggregate size <=1000.
[[nodiscard]] std::optional<PhaseCoordinatedOrdinaryRemoval> phase_coordinated_demolition_ordinary_removal(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Every leaf independently replays the SAME actual source. Only the exported
// semantic demolition composer may combine their known changed/retired rows.
// V2's ordinary removal producer also replays that source independently; actual
// owner/catalog/host phase authority must match the historical saved choice.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_phase_coordinated_demolition(
    const std::map<std::string, Entity, std::less<>>& source,
    const PhaseConstraintAuthoringIntent& enclosing);

} // namespace sketch
