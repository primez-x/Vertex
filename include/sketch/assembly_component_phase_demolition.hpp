#pragma once

#include "sketch/assembly_phase_membership.hpp"
#include "sketch/document.hpp"

namespace sketch {

using AssemblyComponentPhaseDemolitionEntities = std::map<std::string, Entity, std::less<>>;

// Exact catalog/local-row identities, sorted and unique. The actual source
// companion and registry supply ownership and the saved alternative; neither
// an inferred legacy membership nor a caller-supplied phase choice is accepted.
struct AssemblyComponentPhaseDemolitionIntent {
    std::vector<AssemblyPhaseRoot> components;
    bool operator==(const AssemblyComponentPhaseDemolitionIntent&) const = default;
};

// Closed version-one component_phase_demolition envelope. Nonempty requests
// contain at most 1000 rows and pass bounded admission before codec allocation.
[[nodiscard]] nlohmann::json encode_assembly_component_phase_demolition_intent(
    const AssemblyComponentPhaseDemolitionIntent& intent);
[[nodiscard]] AssemblyComponentPhaseDemolitionIntent decode_assembly_component_phase_demolition_intent(
    const nlohmann::json& value);

// Pure complete-map replay. Each selected row must be a declared, active
// baseline-existing root with an actual saved alternative. Only its demolition
// membership is appended in that alternative; its catalog row, baseline, all
// proposals, registry, physical entities and opaque carriers remain raw-exact.
// All catalogs on both sides share cumulative admission before scope decoding.
// No physical deletion, fresh identity, command or historical authority is given.
[[nodiscard]] AssemblyComponentPhaseDemolitionEntities replay_assembly_component_phase_demolition(
    const AssemblyComponentPhaseDemolitionEntities& actual,
    const AssemblyComponentPhaseDemolitionIntent& intent);

} // namespace sketch
