#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct PhaseWallDemolitionIntent {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> wall_ids;
    bool complete_actual_corner_window_cohorts{false};
    bool operator==(const PhaseWallDemolitionIntent&) const = default;
};

struct PhaseWallDemolitionSelection {
    PhaseWallDemolitionIntent baseline;
    std::vector<std::string> ordinary_wall_ids;
};

// Classify actual wall roots without changing the historical baseline-only
// producer. A shared-baseline cohort retains its exact saved choice; other
// walls need independent complete ordinary admission in the enclosing command.
// Null means the selection contains no active-alternative baseline walls.
[[nodiscard]] std::optional<PhaseWallDemolitionSelection> inspect_phase_wall_demolition_selection(
    const std::map<std::string,Entity,std::less<>>& actual,
    const std::vector<std::string>& selected_wall_ids,
    bool complete_actual_corner_window_cohorts=false);

// Closed semantic selection: the actual saved registry/alternative and an
// ascending unique inventory of retained baseline walls. No entity payloads
// or unrelated membership changes can borrow this authority.
// Closed version two explicitly completes actual corner cohorts touching the
// selected walls. Default false retains the exact version-one wire authority.
[[nodiscard]] nlohmann::json encode_phase_wall_demolition_intent(const PhaseWallDemolitionIntent& intent);
[[nodiscard]] PhaseWallDemolitionIntent decode_phase_wall_demolition_intent(const nlohmann::json& value);
// Historical analytical actual-source replay. Only the admitted active registry
// changes; baseline bodies, cuts, hosted rows, metadata and aliases remain exact.
// The enclosing producer still owns source/history/constraint admission and
// explicit physical-room disposition review before publication.
// Version two first admits complete original-source analytical work, then
// independently replays actual corner removal against that same source. Active
// baseline aggregates are parked; sole active proposed aggregates are physically
// retired. The complete returned candidate validates corner invariants; a
// registry-only interim source never escapes as version-two replay authority.
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
