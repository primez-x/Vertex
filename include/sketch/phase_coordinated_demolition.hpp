#pragma once

#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/roof_removal.hpp"

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
// Closed inner v3 has the same seven fields as v2 and requires ordinary child
// v2: exactly version, object_ids, components and roof_additional_identities.
// Its source-derived roof lane cannot be borrowed by any historical dialect.
// Closed inner v4 adds complete_hosted_catalog_consequences:true to the seven
// v2/v3 fields. Ordinary removal is nullable; when present its exact child v1
// or v2 codec remains closed. At least two historical families are required
// without ordinary removal, otherwise one. V4 admits complete independently
// replayed hosted catalog consequences and proven historical selection closure.
// Closed inner v5 adds exactly ordinary_opening_ids: 1..1000 ascending unique
// ASCII identities for independent actual ordinary semantic openings. When no
// historical family is present, replay derives the saved choice from actual
// opening/wall membership. At least one actual active registry is required;
// other unregistered owners remain ordinary without fabricated membership.
// Historical roots, ordinary roots/components and openings total at most 4096;
// opening identities are disjoint from all historical and ordinary roots.
// Closed inner v6 has the same nine fields as v5, permits an empty opening list,
// and requires ordinary child v2 with an actual selected roof. It admits only
// source-derived proposed-roof component retirement from a retained baseline
// catalog in the same actual saved registry. No historical family is required;
// replay derives the actual saved choice from opening/host and roof membership.
// Closed inner v7 has the same nine fields as v5/v6, permits empty openings,
// and requires a nonnull ordinary child v1 or v2 with explicit qualified rows.
// Child v2 still requires an actual roof. Without historical families, actual
// roots, carriers, placement hosts and opening walls bind one active saved choice.
// It admits complete placed-row retirement while preserving physical hosts.
[[nodiscard]] nlohmann::json encode_phase_coordinated_demolition(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Validated full leaf envelopes in opening, roof, slab, structural, stair order.
[[nodiscard]] std::vector<PhaseConstraintAuthoringIntent> phase_coordinated_demolition_components(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

struct PhaseCoordinatedOrdinaryRemoval {
    std::vector<std::string> object_ids;
    // Actual qualified catalog/instance keys; never presentation aliases.
    std::vector<std::pair<std::string, std::string>> components;
    RoofRemovalAdditionalIdentities roof_additional_identities;
    // One is the historical primitive/component producer; two selects the new
    // mixed roof producer. Old wire bytes and replay semantics remain exact.
    int version{1};
};

// Validates the complete enclosure, including historical children and source
// bindings. V1 and v4/v5 with null ordinary_removal return nullopt. V2 selections
// are canonical ascending unique ASCII identities with aggregate size <=1000.
// Inner v3/v4/v5/v6/v7 child v2 destinations are bounded nonempty arrays of fresh IDs; slot
// order is authored join/overlay order rather than lexical identity order.
[[nodiscard]] std::optional<PhaseCoordinatedOrdinaryRemoval> phase_coordinated_demolition_ordinary_removal(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Validates the entire enclosure; empty for closed inner v1..v4. V5 identities
// grant no wall or room authoring authority and replay the same actual source.
// V6/v7 return the same validated list, which may be empty.
[[nodiscard]] std::vector<std::string> phase_coordinated_demolition_ordinary_openings(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Validates the entire enclosure; true exclusively for closed inner v4..v7.
[[nodiscard]] bool phase_coordinated_demolition_complete_hosted_catalog_consequences(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// True exclusively for closed inner v6/v7. Historical/default roof producers keep
// their refusal semantics; controller capture and Site admission use this flag.
[[nodiscard]] bool phase_coordinated_demolition_complete_roof_hosted_catalog_consequences(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// True exclusively for closed inner v7; qualified rows grant no physical host
// removal authority. Older complete producers retain their existing policies.
[[nodiscard]] bool phase_coordinated_demolition_complete_placed_catalog_consequences(
    const nlohmann::json& value, const PhaseConstraintAuthoringIntent& enclosing);

// Every leaf independently replays the SAME actual source. Only the exported
// semantic demolition composer may combine their known changed/retired rows.
// V2's ordinary removal producer also replays that source independently; actual
// owner/catalog/host phase authority must match the historical saved choice.
// V3 additionally binds affected actual roofs/joins and retained roof registry
// transitions to that same actual registry/alternative.
// V4 retains actual registry ID order and all baseline physical envelopes,
// protects foreign/inactive hosts, and composes only complete typed consequences.
// V5 authenticates independent openings and their actual active wall hosts to
// the same saved choice before native replay. V5 additionally retires admitted
// proposed-opening rows from baseline carriers without changing their envelope,
// other rows, baseline openings or unchanged original wall bodies.
// V6 extends those consequences to actual proposed roofs, preserving the raw
// baseline catalog envelope, surviving instance order and retained membership.
// V7 authenticates every explicit row/root/host before historical replay and
// permits only source-produced requested row retirement on an exact retained
// proposed opening and its exact baseline wall in the same actual saved choice.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_phase_coordinated_demolition(
    const std::map<std::string, Entity, std::less<>>& source,
    const PhaseConstraintAuthoringIntent& enclosing);

} // namespace sketch
