#pragma once

#include "sketch/constraint_authoring.hpp"

#include <vector>
#include <set>

namespace sketch {

struct PhaseWallReplacementAuthoringPreview;
struct ReplayedPhysicalWallRoomReview;

// Collision fence for newly declared global identities against one retained
// state, including known owned topology/children and render aliases. References
// and qualified catalog-local instance IDs are not global ownership authority.
// This validates lifetime only; it cannot admit an edit or create identities.
void validate_selection_edit_fresh_identity_tokens(
    const std::map<std::string,Entity,std::less<>>& retained,
    const std::set<std::string,std::less<>>& fresh);

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
    // Dialect two: a typed wall identity replacement and reviewed room
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
    // Dialect fourteen selects closed inner version four, retaining all inner
    // three keys and adding stair_replacement and ordinary_stair_transforms.
    // The optional stair leaf keeps its own closed v1/v2/v3 codec; the exclusive
    // ordinary list carries canonical StairTransformIntent v1/v2 rows. Every
    // family independently replays the same actual captured source.
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
    // Dialect fifteen: independently replayed demolition children for at least
    // two architectural families in inner one. Inner two combines one or more
    // historical families with actual ordinary/proposed removal. Each child
    // binds the same source; baselines and other alternatives stay protected.
    nlohmann::json coordinated_demolition=nullptr;
    // Dialect sixteen: complete baseline wall demolition with independently
    // replayed architectural removal and explicit phase-room decisions.
    // The analytical stage retains the actual captured source as authority.
    nlohmann::json wall_demolition=nullptr;
    // Dialect seventeen: a standalone nonempty canonical RoofEditIntent list,
    // independently replayed against actual ordinary/proposed membership.
    // It carries explicit opening transfer authority that candidate inference
    // cannot recover. No sibling geometry, relationship, replacement or
    // demolition authority accompanies it. Null preserves historical codecs.
    nlohmann::json ordinary_roof_edits=nullptr;
    // Dialect twenty uses the same nine keys as seventeen and requires at least
    // one RoofEditIntent v9 / opening v4. Seventeen cannot borrow that authority.
    // Dialect twenty-one uses four's exact keys with rotation replacement leaf
    // ten. Historical four/coordinated dialects cannot admit the new leaf.
    // Dialect eighteen: intent.wall_group_scale alone carries canonical
    // uniform physical scale authority for actual ordinary/proposed walls.
    // Its closed intent gains exactly the wall_group_scale field; historical
    // dialects retain their sixteen fields and cannot admit this operation.
    // Dialect nineteen separately pairs the same pure scale with a nonnull
    // canonical wall_replacement leaf eight. It requires complete presentations
    // and complete corner windows, with no profiles, rehosts, family changes,
    // stacks or corner profile edits. Its exact nine outer keys match dialect
    // two; historical dialect two retains its sixteen-field intent. Complete
    // replacement replay derives and copies actual baseline roots before scale.
    // Neither scale dialect grants demolition or coordinated scale authority.
};

[[nodiscard]] PhaseConstraintAuthoringIntent decode_phase_constraint_authoring_intent(
    const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_phase_constraint_authoring_intent(
    const PhaseConstraintAuthoringIntent& intent);
// Historical identity/model guards enumerate the actual replacement leaves.
// Standalone baseline scale retains its complete pair as one component.
// Ordinary historical intents return themselves; coordinated ordinary lists
// do not acquire replacement authority through this enumeration.
[[nodiscard]] std::vector<PhaseConstraintAuthoringIntent> phase_constraint_replacement_components(
    const PhaseConstraintAuthoringIntent& intent);
[[nodiscard]] PhaseConstraintAuthoringIntent make_phase_constraint_authoring_intent(
    const DocumentSnapshot& source, const ConstraintAuthoringIntent& intent);
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_constraint_authoring(
    const std::map<std::string,Entity,std::less<>>& source, const nlohmann::json& proof);
// Detached physical preview only for coordinated inner v2/v3/v4 wall replacements.
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

// Compose one to five complete edit maps independently admitted against the
// same actual source. Exact shared consequences coalesce; fresh destinations
// remain disjoint. Only codec-known source rows and admitted complete assembly
// instance rows/suffixes may merge. Legacy annotation axes may promote to state
// v3 using only required missing symbol defaults, preserving other raw fields.
// Protected baselines and their hosted rows stay exact. This grants neither
// leaf admission, captured-snapshot authority nor publication authority.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_mixed_selection_edit_candidates(
    const std::map<std::string,Entity,std::less<>>& actual,
    const std::vector<std::map<std::string,Entity,std::less<>>>& independently_admitted_candidates);

// The complete typed geometry/review replay is the first composition lane;
// ordinary lanes must be independently admitted against this same actual
// source. Only its explicitly retired supported physical rooms gain owner
// erasure authority. Walls, physical rooms, constraints and wall/room callouts
// retain the exact geometry result. Reviewed reference/room override cleanup
// composes with independent annotation children without restoring removed rows.
// This grants neither replay admission nor captured-source/publication authority;
// the caller must independently rederive the complete typed geometry result.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_reviewed_geometry_selection_edit_candidates(
    const std::map<std::string,Entity,std::less<>>& actual,
    const ReplayedPhysicalWallRoomReview& independently_replayed_geometry,
    const std::vector<std::map<std::string,Entity,std::less<>>>& independently_admitted_ordinary_candidates);

// Internal composition of independently admitted demolition leaves. Complete
// typed leaf replay owns removals and fresh destinations; overlapping changes
// compose only through codec-known phase/presentation rows and catalog removals.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_phase_demolition_candidates(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::vector<std::map<std::string,Entity,std::less<>>>& candidates,
    bool include_ordinary_removal=false,
    bool complete_roof_removal=false);

// Complete ordinary removal leaves may retire baseline-only registries with
// no alternatives. Shared baseline owners stay protected. Roof contact splits
// and hosted retirements still require independent typed leaf admission.
// Explicit shared-reference completion treats identical codec-known reference
// erasures and source-row omissions as one consequence. Physical owner overlap
// and conflicting edits still refuse; the default retains historical rules.
// Dialect sixteen alone opts into complete hosted catalog consequences: exact
// retained source rows/removals plus disjoint raw fresh-instance suffixes, with
// the entire source catalog envelope preserved. Every input must already be a
// complete independently admitted typed primitive replay of this same source;
// this composer grants neither fresh-row nor physical removal admission.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_ordinary_architectural_removal_candidates(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::vector<std::map<std::string,Entity,std::less<>>>& candidates,
    bool allow_shared_reference_retirement=false,
    bool complete_hosted_catalog_consequences=false);

// Independently evaluated saved selections, including the empty-registry case.
[[nodiscard]] nlohmann::json phase_constraint_authoring_selections(
    const std::map<std::string,Entity,std::less<>>& source);

} // namespace sketch
