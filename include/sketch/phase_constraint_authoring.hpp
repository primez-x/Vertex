#pragma once

#include "sketch/constraint_authoring.hpp"

#include <vector>

namespace sketch {

struct PhaseWallReplacementAuthoringPreview;

// Versioned semantic authority. Geometry payloads are outputs of replay;
// every source binding and saved phase choice describes the actual source.
struct PhaseConstraintAuthoringIntent {
    Revision expected_revision{};
    std::string source_snapshot_digest;
    std::string source_authoring_digest;
    std::string source_entities_digest;
    std::optional<Revision> source_saved_revision;
    nlohmann::json phase_selections;
    ConstraintAuthoringIntent intent;
    // Dialect two only: a typed wall identity replacement and reviewed room
    // completion, independently replayed before publication. Null keeps the
    // version-one semantics and exact wire keys.
    nlohmann::json wall_replacement=nullptr;
    // Dialect three only: registry-only opening demolition. It cannot borrow
    // wall replacements, geometry edits, relationships or arbitrary payloads.
    nlohmann::json opening_demolition=nullptr;
    // Dialect four only: source-derived roof replacement. No ordinary entity
    // payload, wall edit or relationship authority accompanies this operation.
    nlohmann::json roof_replacement=nullptr;
    // Dialect five: source-derived horizontal assembly replacement. The
    // baseline and its actual entered dimensions remain retained verbatim.
    nlohmann::json slab_replacement=nullptr;
    // Dialect six: registry-only demolition of actual shared baseline slabs.
    // No replacement identity or ordinary geometry authority accompanies it.
    nlohmann::json slab_demolition=nullptr;
    // Dialect seven: one actual-source roof lane and one horizontal geometry
    // lane. Each is either a replacement leaf or a nonempty ordinary typed
    // list; at least one replacement is required. Historical leaves retain
    // their own codecs and replay semantics. Dialect eight selects coordinated
    // inner version two: an optional canonical wall geometry authoring child
    // joins optional roof/horizontal lanes, with at least two families present
    // and at least one actual replacement. Every child binds the same source.
    nlohmann::json coordinated_replacements=nullptr;
};

[[nodiscard]] PhaseConstraintAuthoringIntent decode_phase_constraint_authoring_intent(
    const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_phase_constraint_authoring_intent(
    const PhaseConstraintAuthoringIntent& intent);
// Historical identity/model guards enumerate the actual replacement leaves.
// Ordinary historical intents return themselves; coordinated ordinary lists
// do not acquire replacement authority through this enumeration.
[[nodiscard]] std::vector<PhaseConstraintAuthoringIntent> phase_constraint_replacement_components(
    const PhaseConstraintAuthoringIntent& intent);
[[nodiscard]] PhaseConstraintAuthoringIntent make_phase_constraint_authoring_intent(
    const DocumentSnapshot& source, const ConstraintAuthoringIntent& intent);
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_constraint_authoring(
    const std::map<std::string,Entity,std::less<>>& source, const nlohmann::json& proof);
// Detached physical preview only for coordinated inner v2 wall replacements.
// Pending room decisions remain pending; this map is not publication authority.
[[nodiscard]] PhaseWallReplacementAuthoringPreview inspect_phase_coordinated_authoring(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent);

// Independently evaluated saved selections, including the empty-registry case.
[[nodiscard]] nlohmann::json phase_constraint_authoring_selections(
    const std::map<std::string,Entity,std::less<>>& source);

} // namespace sketch
