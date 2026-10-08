#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct SlabDemolitionIntent {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> slab_ids;
    bool operator==(const SlabDemolitionIntent&) const = default;
};

// Returns nullopt for ordinary/proposed-only selections. Once an actual active
// shared baseline slab is selected, every selected owner must be an active
// baseline slab in the same saved registry and alternative.
[[nodiscard]] std::optional<SlabDemolitionIntent> phase_slab_demolition_request(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<std::string>& selection);

// Strict bounded version 1 with canonical sorted, unique actual slab IDs.
[[nodiscard]] nlohmann::json encode_slab_demolition_intent(const SlabDemolitionIntent& intent);
[[nodiscard]] SlabDemolitionIntent decode_slab_demolition_intent(const nlohmann::json& value);

// Independently derives demolition from actual source membership. Only the
// active alternative's demolition list changes; all owners and payloads remain
// exact. The caller retains asset and captured-snapshot publication authority.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_phase_slab_demolition_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const SlabDemolitionIntent& intent);

} // namespace sketch
