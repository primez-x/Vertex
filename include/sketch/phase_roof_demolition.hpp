#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

using RoofDemolitionEntities = std::map<std::string, Entity, std::less<>>;
using RoofDemolitionIdentityMap = std::map<std::string, std::string, std::less<>>;

// Only explicit selected baseline roofs confer demolition authority. The full
// joined cohort and its children are independently discovered from actual source.
struct RoofDemolitionIntent {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_roof_ids;
    RoofDemolitionIdentityMap identities;
    // Only additional actual >=2 join copies and their bound overlays. Slot
    // order follows component order after the first actual copied component.
    std::map<std::string, std::vector<std::string>, std::less<>> additional_identities;
    // Version two retains actual ordinary/proposed join members with their IDs.
    bool phase_qualified_joins{};
    bool operator==(const RoofDemolitionIntent&) const = default;
};

struct RoofDemolitionResult {
    RoofDemolitionEntities entities;
    // Complete primary mapping, including identities whose copies are omitted.
    RoofDemolitionIdentityMap original_to_proposed;
    // Every declared destination remains reserved across Undo by the Document.
    std::vector<std::string> fresh_identity_ids;
    // Actual newly created proposed roofs and joins, excluding children/overlays.
    std::vector<std::string> copied_owner_ids;
    // Complete original baseline roof/join cohort, retained exact. Actual
    // ordinary/proposed survivors are excluded from demolition and copying.
    std::vector<std::string> demolished_owner_ids;
};

struct RoofDemolitionRequest {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_roof_ids;
    bool phase_qualified_joins{};
    bool operator==(const RoofDemolitionRequest&) const = default;
};

// Actual active ordinary/proposed targets return nullopt for the ordinary delete
// path. Shared-baseline targets require one saved active alternative; mixed,
// inactive, dangling, duplicate or cross-registry targets refuse before allocation.
[[nodiscard]] std::optional<RoofDemolitionRequest> roof_demolition_request(
    const RoofDemolitionEntities& source, const std::vector<std::string>& selected_roof_ids,
    bool phase_qualified_joins = false);

// Actual additional >=2 join components and their bound overlays require fresh
// slots beyond the complete primary mapping. Keys are actual join/overlay IDs;
// only nonzero counts are returned. Replay independently rederives all counts.
[[nodiscard]] std::map<std::string, std::size_t, std::less<>> roof_demolition_additional_identity_counts(
    const RoofDemolitionEntities& source, const RoofDemolitionRequest& request);

// Strict version 1: exactly version, registry_id, alternative_id, seed_roof_ids,
// identities, additional_identities. No entity payload establishes authority.
// Version two adds only phase_qualified_joins:true; version one stays closed.
[[nodiscard]] nlohmann::json encode_roof_demolition_intent(const RoofDemolitionIntent& intent);
[[nodiscard]] RoofDemolitionIntent decode_roof_demolition_intent(const nlohmann::json& value);

// Reads the complete actual retained map and saved active alternative. Selected
// roofs have no proposed copies; surviving neighbors retain source geometry and
// opaque data. A singleton inherits its actual source join's admitted effective
// material override, preserving compatible raw roof assignment extras. Original
// roofs remain exact. Qualified presentation extends only actual copies.
// Opt-in ordinary/proposed survivors retain exact owners and roles. A singleton
// whose source join material cannot be represented on its exact roof refuses.
[[nodiscard]] RoofDemolitionResult replay_roof_demolition(
    const RoofDemolitionEntities& source, const RoofDemolitionIntent& intent);

} // namespace sketch
