#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct PhaseWallDemolitionIntent {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> wall_ids;
    bool operator==(const PhaseWallDemolitionIntent&) const = default;
};

// Closed semantic selection: the actual saved registry/alternative and an
// ascending unique inventory of retained baseline walls. No entity payloads
// or unrelated membership changes can borrow this authority.
[[nodiscard]] nlohmann::json encode_phase_wall_demolition_intent(const PhaseWallDemolitionIntent& intent);
[[nodiscard]] PhaseWallDemolitionIntent decode_phase_wall_demolition_intent(const nlohmann::json& value);
// Analytical complete actual-source replay. Only the admitted active registry
// changes; baseline bodies, cuts, hosted rows, metadata and aliases remain exact.
// The enclosing producer still owns source/history/constraint admission and
// explicit physical-room disposition review before publication.
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_wall_demolition_entities(
    const std::map<std::string,Entity,std::less<>>& actual,const PhaseWallDemolitionIntent& intent);

// Prepare one raw, asset-free registry upsert for the existing phase-room
// review. No shared-baseline selection returns nullopt. Once any selected wall
// qualifies, every selected root must be an actual, nonrequired, active baseline
// wall in the same saved registry/alternative (at most 128 roots).
// Retain all physical entities, membership rosters, saved metadata, other
// alternatives and raw row order. Registered baseline openings are demolished
// with their host. Unregistered actual openings retain their exact membership
// and become inactive through the admitted wall-host visibility contract.
// Unsupported or protected affected hosting refuses instead of deleting originals.
// The caller must review room dispositions and apply the resulting completion
// once, retaining the captured source/selection fence. This is not a deletion
// stage and performs no Document preview or native geometry work.
[[nodiscard]] std::optional<ApplyEntityChanges> prepare_phase_wall_demolition(
    const DocumentSnapshot& source, const std::vector<std::string>& selected_wall_ids,
    const std::string& message);

} // namespace sketch
