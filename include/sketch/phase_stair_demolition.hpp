#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct StairDemolitionIntent {
    std::string registry_id;
    std::string alternative_id;
    // Explicit selected roots only. Hosted baseline railings are derived from
    // the actual current source, never supplied as closure authority.
    std::vector<std::string> selected_object_ids;
    bool operator==(const StairDemolitionIntent&) const = default;
};

// Ordinary/proposed-only selections return nullopt. An actual active baseline
// canonical stair/railing requires every selected root to have that same saved
// registry/alternative authority. A proposed hosted dependent blocks demolition.
[[nodiscard]] std::optional<StairDemolitionIntent> phase_stair_demolition_request(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<std::string>& selection);

// Closed, bounded version one; selected roots must be sorted and unique.
[[nodiscard]] nlohmann::json encode_stair_demolition_intent(const StairDemolitionIntent& intent);
[[nodiscard]] StairDemolitionIntent decode_stair_demolition_intent(const nlohmann::json& value);

// Re-derive actual hosted baseline closure and append demolition membership in
// only the saved active alternative. Physical owners, catalog hosts and opaque
// envelopes are retained exactly; selecting a rail does not demolish its stair.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_phase_stair_demolition_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const StairDemolitionIntent& intent);

} // namespace sketch
