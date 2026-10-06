#pragma once

#include "sketch/document.hpp"
#include "sketch/stair_identity_history.hpp"

namespace sketch {

// Pure admission over the ORIGINAL current entity map. Throws invalid_argument;
// never changes entities, performs native geometry work, or recurses into Document.
// Known v1 straight and v2 multi-flight/hosted forms are decoded strictly. Generic
// legacy descriptors without form/version and unsupported future forms/versions
// remain opaque. Known forms with missing/malformed versions are invalid; v1
// cannot carry v2 topology/host authority. Opaque forms cannot host known rails.
// A hosted rail needs complete valid organization matching its host's property,
// building and floor; another valid layer on that floor is permitted. Visibility
// checkboxes do not affect authority. Hosted vertical_placement is forbidden.
// Explicit phase_id must match (including presence). Each ModelPhases registry
// involving either participant must include both, and at most one may own them.
// In baseline and EVERY alternative, a participating rail needs a participating
// host: existing->existing; proposed->existing/proposed; demolished->existing/
// demolished. Thus surviving rails cannot refer to demolished hosts. Independent
// or multiply-owned phase membership is refused rather than guessed.
void validate_stair_attachment_state(
    const std::map<std::string, Entity, std::less<>>& entities);

// For authored changes ONLY. Root validates navigation/history copies separately.
// A surviving child keeps its owner AND typed role (flight vs landing), through
// reorder/edits. Introduced children cannot reuse any retained child/entity ID,
// including retired IDs and abandoned redo branches. Retained owner identity
// cannot be resurrected as a newly authored stair. No history bypass flags.
// Typed topology and mapped identity are checked before admission; full geometry
// admission remains validate_stair_attachment_state's separate responsibility.
// Finite relevant child/topology bounds do not cap unrelated history size.
void validate_stair_identity_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    std::span<const RevisionRecord> retained_history);

} // namespace sketch
