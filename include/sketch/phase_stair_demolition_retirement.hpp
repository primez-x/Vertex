#pragma once

#include "sketch/phase_stair_demolition.hpp"

#include <utility>

namespace sketch {

// Closed v1: explicit baseline selection and the exact proposed attachment
// inventory discovered in the captured source. The inventory is a proof, never
// permission to erase caller-selected objects or catalog owners.
struct StairDemolitionRetirementIntent {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> selected_object_ids;
    std::vector<std::string> retired_proposed_rail_ids;
    bool operator==(const StairDemolitionRetirementIntent&) const = default;
};

struct StairDemolitionRetirementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool operator==(const StairDemolitionRetirementDiagnostic&) const = default;
};
struct StairDemolitionRetirementPlan {
    StairDemolitionRetirementIntent intent;
    // Qualified catalog/local instance keys and actual generated presentation
    // aliases. These are inspection results, not replay inputs.
    std::vector<std::pair<std::string, std::string>> retired_hosted_instances;
    std::vector<std::string> retired_presentation_ids;
    std::vector<StairDemolitionRetirementDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept { return diagnostics.empty(); }
};

// nullopt leaves ordinary/proposed-only and baseline-without-proposals requests
// to their existing lanes. Mixed or ambiguous actual baseline selections refuse.
[[nodiscard]] std::optional<StairDemolitionRetirementIntent>
phase_stair_demolition_retirement_request(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<std::string>& selection);

[[nodiscard]] nlohmann::json encode_stair_demolition_retirement_intent(
    const StairDemolitionRetirementIntent& intent);
[[nodiscard]] StairDemolitionRetirementIntent decode_stair_demolition_retirement_intent(
    const nlohmann::json& value);

[[nodiscard]] StairDemolitionRetirementPlan inspect_phase_stair_demolition_retirement_plan(
    const std::map<std::string, Entity, std::less<>>& source,
    const StairDemolitionRetirementIntent& intent);

// Re-derive every retired owner/row/reference from actual source, retire only
// active-only proposed attached rails, then compose ordinary baseline demolition.
// All baseline physical envelopes, other alternatives and catalog definitions
// survive exactly. A blocking opaque reference or changed surviving alias refuses.
[[nodiscard]] std::map<std::string, Entity, std::less<>>
replay_phase_stair_demolition_retirement_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const StairDemolitionRetirementIntent& intent);

// Explicit selective lane for stair replacement. rail_ids is a nonempty,
// bounded, sorted unique subset of actual active-only proposed attached rails
// in the exact saved registry/alternative. The inspected intent's selected
// object IDs are derived baseline host stair evidence, never demolition roots.
// This lane does not extend the closed v1 demolition-retirement proof grammar.
[[nodiscard]] StairDemolitionRetirementPlan inspect_phase_stair_proposed_rail_retirement_plan(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::string& registry_id, const std::string& alternative_id,
    const std::vector<std::string>& rail_ids);

// Returns the complete source-derived selective retirement candidate without
// demolishing its host stairs. Every other physical envelope and alternative,
// catalog definition, raw survivor order and surviving alias is preserved.
// Actual qualified hosted rows and supported presentation bindings retire with
// the selected rails; opaque/unqualified retained references remain blocking.
[[nodiscard]] std::map<std::string, Entity, std::less<>>
replay_phase_stair_proposed_rail_retirement_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::string& registry_id, const std::string& alternative_id,
    const std::vector<std::string>& rail_ids);

} // namespace sketch
