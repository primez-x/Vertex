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
    // Dialect ten selects closed inner version three, adding an optional
    // structural replacement or ordinary transform-only structural edit list.
    // Its exact keys are version, wall_authoring, roof_replacement,
    // slab_replacement, structural_replacement, ordinary_roof_edits,
    // ordinary_slab_geometry, ordinary_structural_edits. Each absent leaf is
    // null and each absent ordinary list is []; at least two families are
    // required. Unlike historical inner one/two, actual-source ordinary lanes
    // may compose without a replacement, retaining typed wall room authority.
    nlohmann::json coordinated_replacements=nullptr;
    // Dialect nine: source-derived column/beam edits in one saved alternative.
    // Shared baseline owners remain exact; explicit structural intent alone
    // creates proposed replacements and their known presentation references.
    nlohmann::json structural_replacement=nullptr;
    // Dialect eleven: registry-only stair/railing demolition. Explicit roots
    // retain their physical records; actual attached baseline rails follow
    // their host into the saved alternative's demolition membership.
    nlohmann::json stair_demolition=nullptr;
    // Dialect twelve: typed stair/railing profile replacement in the actual
    // saved alternative. Baseline owners stay exact; attached baseline rails
    // and hosted parts receive copies, while proposed rails retain identity.
    nlohmann::json stair_replacement=nullptr;
    // Dialect thirteen: retire actual active proposed rail dependents before
    // demolishing their retained baseline stair. No arbitrary erase authority.
    nlohmann::json stair_demolition_retirement=nullptr;
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
// Detached physical preview only for coordinated inner v2/v3 wall replacements.
// Pending room decisions remain pending; this map is not publication authority.
[[nodiscard]] PhaseWallReplacementAuthoringPreview inspect_phase_coordinated_authoring(
    const DocumentSnapshot& source,const PhaseConstraintAuthoringIntent& intent);

// Compose independently replayed ordinary family maps from the same actual
// source. All source owners must remain and no fresh owners are admitted.
// Conflicting physical edits refuse; only codec-known presentation/registry
// rows and distinct hosted instance placements have composition authority.
// Callers still own complete geometry, phase-scope and command admission.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_architectural_family_candidates(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::vector<std::map<std::string,Entity,std::less<>>>& candidates);

// Independently evaluated saved selections, including the empty-registry case.
[[nodiscard]] nlohmann::json phase_constraint_authoring_selections(
    const std::map<std::string,Entity,std::less<>>& source);

} // namespace sketch
