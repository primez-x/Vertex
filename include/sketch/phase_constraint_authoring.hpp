#pragma once

#include "sketch/constraint_authoring.hpp"
#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/assembly_directional_resize.hpp"
#include "sketch/roof_opening_group_edit.hpp"
#include "sketch/phase_roof_resize.hpp"

#include <vector>
#include <set>
#include <optional>
#include <span>

namespace sketch {

struct PhaseWallReplacementAuthoringPreview;
struct ReplayedPhysicalWallRoomReview;
class StairPropertyReviewPreparation;
class AssemblyInstancePropertyEditPreparation;
class PhaseAssemblyInstancePropertyEditPreparation;
struct PhaseAssemblyInstancePropertyEditAuthoring;
class MixedSelectionPropertyEditPreparation;
struct ComponentSlabDirectionalResizeAllocation;
struct ComponentStructuralDirectionalResizeAllocation;
struct StructuralDirectionalResizeRequest;
struct StairDirectionalResizeRequest;
struct SlabGeometryEditIntent;

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
    // version-one semantics and exact wire keys. Dialect twenty-six admits
    // only complete wall leaf ten, with explicit signed raw elevation authority;
    // dialect twenty-three remains paired exclusively with scalar leaf nine.
    nlohmann::json wall_replacement=nullptr;
    // Dialect three only: registry-only opening demolition. It cannot borrow
    // wall replacements, geometry edits, relationships or arbitrary payloads.
    nlohmann::json opening_demolition=nullptr;
    // Dialect four only: source-derived roof replacement. No ordinary entity
    // payload, wall edit or relationship authority accompanies this operation.
    nlohmann::json roof_replacement=nullptr;
    // Dialect five: source-derived horizontal assembly replacement. The
    // baseline and its actual entered dimensions remain retained verbatim.
    // Dialect twenty-seven pairs exclusive leaf seven for explicit coordinated
    // slab elevation and actual hosted component carry; five refuses that lane.
    // Dialect forty-two alone pairs slab child nine for explicit directional
    // plan resize and actual-source hosted carry. Forty-three alone pairs its
    // complete dimension child ten with slab_edit_version forty-two. Historical
    // five/twenty-seven/thirty-nine and coordinated envelopes refuse this lane.
    // Dialect forty-four pairs slab child eleven for source-preserving mapped
    // plan geometry. Forty-five pairs dimension child twelve and exactly
    // slab_edit_version forty-four. Earlier dialects retain their own children.
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
    // Dialect twenty-nine selects historical inner five, retaining qualified
    // hosted wall replacement authority and its exact baseline protections.
    // Dialect thirty-one alone pairs closed inner six: {version:6, authoring:
    // <exact inner one through five>, complete_hosted_component_dimensions:true}.
    // Wrappers cannot nest or add fields. The authored inner version keeps all
    // historical family and replacement semantics. Only actual changed hosts
    // from independently replayed ordinary roof/slab/structural/stair families
    // (including producer-discovered attached rails) admit one final carry of
    // existing supported component dimensions. Active/baseline dimension policy
    // still applies; no replacement dimension copies or fresh IDs are admitted.
    // Dialect thirty-four alone pairs closed inner seven: inner six's fields
    // plus complete_stair_component_dimensions:true. Its nonnested body is
    // inner four/five with an actual canonical stair child8. That leaf copies
    // its source-derived component dimensions to fresh proposed identities;
    // historical enclosures cannot borrow this authority. The existing
    // ordinary hosted carry remains a single final composition contribution.
    // Dialect thirty-seven alone pairs closed inner eight: {version:8,
    // authoring:<exact inner five>, complete_hosted_component_dimensions:true,
    // complete_wall_component_dimensions:true}. Its actual typed move-only
    // wall child is phase thirty-six/wall_edit_version two with canonical leaf
    // twelve, retaining exact original component dimensions and reserving their
    // explicit fresh copies. The optional literal-true stair completion key is
    // present exactly when an actual canonical stair child8 supplies its complete
    // dimension authority. The other families keep inner five's semantics;
    // ordinary hosted carry still runs once after composition, excluding walls.
    // Dialect forty-one alone pairs closed inner nine: version, authoring,
    // complete_hosted_component_dimensions:true and optional literal-true
    // complete_{wall,stair,roof,slab,structural}_component_dimensions keys.
    // Each key matches an actual wall12/stair8/roof11/slab8/structural5 child;
    // at least one of roof11/slab8/structural5 is required. The nonnested exact
    // body is four, or five exactly when an actual qualified wall replacement
    // supplies phase36/wall_edit_version two. Roof11 alone may enclose roof
    // rotation body ten; historical wrappers retain their original admission.
    // Real complete leaves preserve original dimensions and reserve fresh
    // destinations together. The source-derived shared copied roster and one
    // final ordinary hosted contribution share the 1000-association budget;
    // final copied frames are independently verified against the original source.
    // Dialect forty-six alone pairs direct inner ten, with inner four/five's
    // family restrictions plus explicit directional/mapped slab geometry.
    // It has the family body keys (no authoring wrapper), literal-true complete
    // hosted dimensions, and optional completion flags matching actual children.
    // Independent same-source replay authenticates units and changed hosts;
    // disjoint directional catalog composition precedes one shared ordinary/
    // clone dimension batch. At least two physical families must actually change.
    // Historical coordinated dialects retain their exact geometry authority.
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
    // Dialect thirty-three alone admits child7 component dimension copies;
    // its historical child body retains existing scalar/dependency authority.
    // Dialect thirty-five alone admits child8's explicit ordinary dimension
    // carry after the complete child7 copies. Historical33 retains clone only.
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
    // Dialect thirty-six encloses the historical wall edit lane separately
    // in wall_edit_version (two/nineteen/twenty-three/twenty-six) and requires
    // wall replacement leaf twelve. Its explicit dimension mapping clones
    // qualified embedded extents before the final host/frame carry. Original
    // baseline dimensions retain their exact source envelopes; historical
    // twenty-eight/leaf-eleven cannot acquire this authority.
    // Dialects thirty-eight/thirty-nine/forty add standalone roof/slab/
    // structural embedded dimension copies. Their separate roof_edit_version
    // (four/twenty-one), slab_edit_version (five/twenty-seven), and
    // structural_edit_version (nine/twenty-four) retain the exact historical
    // physical lane; the leaf is respectively eleven/eight/five. Historical
    // coordinated and Properties envelopes cannot borrow these new wrappers.
    // Dialect twenty-two retains a pure per-owner similarity joint intent.
    // It keeps the ordinary eight outer keys and grants no replacement lane;
    // historical wrappers cannot carry the new nested operator dialect.
    // Dialect thirty: registry-preserving demolition of actual qualified
    // baseline component rows in their saved alternative. The closed leaf
    // changes only companion demolition membership; the inner intent is
    // message-only and no sibling edit or replacement authority is admitted.
    nlohmann::json component_demolition=nullptr;
    // Dialect thirty-two: standalone ordinary roof edits retain their exact
    // historical authority and additionally carry existing supported hosted
    // component dimensions from the complete actual-source typed candidate.
    // Its tenth outer key is this literal-true flag; no replacement copies,
    // retirement, fresh dimension IDs or coordinated wrapper are admitted.
    bool complete_ordinary_roof_component_dimensions=false;
};

[[nodiscard]] PhaseConstraintAuthoringIntent decode_phase_constraint_authoring_intent(
    const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_phase_constraint_authoring_intent(
    const PhaseConstraintAuthoringIntent& intent);
// Canonical encoding of one ordinary constraint child, as embedded in phase intents.
[[nodiscard]] nlohmann::json encode_phase_constraint_authoring_child_intent(
    const ConstraintAuthoringIntent& value);
// Borrow the historical coordinated body from an already validated canonical
// enclosing intent. Inner six/seven/eight/nine's exact closed, literal-true, nonnested
// wrapper is checked before unwrapping once; historical bodies return unchanged.
// Mutation callers must re-encode the enclosing intent to validate completion.
// Direct inner ten returns unchanged after its own closed-shape/flag check.
// This accessor grants no edit, replay, replacement or publication authority.
[[nodiscard]] const nlohmann::json& phase_constraint_coordinated_authoring_body(
    const nlohmann::json& coordinated_replacements);
[[nodiscard]] nlohmann::json& phase_constraint_coordinated_authoring_body(
    nlohmann::json& coordinated_replacements);
// Historical identity/model guards enumerate the actual replacement leaves.
// Standalone baseline scale retains its complete pair as one component.
// Ordinary historical intents return themselves; coordinated ordinary lists
// do not acquire replacement authority through this enumeration.
[[nodiscard]] std::vector<PhaseConstraintAuthoringIntent> phase_constraint_replacement_components(
    const PhaseConstraintAuthoringIntent& intent);
// Pass the component leaf directly to admit all source catalogs before saved
// phase selections are decoded. Complete membership work also admits them for
// a subsequently enclosed coordinated child; this is admission, not authority.
// Null/false preserve the historical make path.
[[nodiscard]] PhaseConstraintAuthoringIntent make_phase_constraint_authoring_intent(
    const DocumentSnapshot& source, const ConstraintAuthoringIntent& intent,
    const nlohmann::json& component_demolition = nullptr,
    bool complete_component_membership_work = false);
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_constraint_authoring(
    const std::map<std::string,Entity,std::less<>>& source, const nlohmann::json& proof);
// Detached physical preview for coordinated authored inner v2/v3/v4/v5 wall
// replacements, including the exclusive v6/v7/v8/v9 dimension wrappers and
// direct inner10 slab authority when present.
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

// Independently replay the closed scalar/explicit dependency review against
// the complete captured source before composing up to five ordinary edit maps.
// Only that exact replay's removal closure and explicitly retired rail hosted
// rows may disappear. Surviving reviewed physical consequences and qualified
// render aliases remain exact; disjoint admitted catalog/annotation rows merge.
// Ordinary maps retain their own admission responsibility. No candidate map or
// projected source supplies review, fresh identity or publication authority.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_stair_property_review_selection_edit_candidates(
    const DocumentSnapshot& actual,
    const StairPropertyReviewPreparation& reviewed,
    const std::vector<std::map<std::string,Entity,std::less<>>>& independently_admitted_ordinary_candidates);

// Independently authenticate the assembly contribution against the complete
// source. Only its actual legacy catalog promotions acquire the v1/v2-to-v7
// row-local envelope authority; historical composers retain their rules.
// Optional stair review keeps its exact separately reconstructed retirement.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_assembly_property_selection_edit_candidates(
    const DocumentSnapshot& actual,
    const AssemblyInstancePropertyEditPreparation& assemblies,
    const std::vector<std::map<std::string,Entity,std::less<>>>& independently_admitted_ordinary_candidates,
    const StairPropertyReviewPreparation* stair_review = nullptr);

// Reconstruct the closed component alternative contribution from the complete
// captured source before composing other admitted lanes. Fresh destinations and
// changed rows on retained hosts come only from that replay. This is detached
// composition, not dependency completion or Document publication authority.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_phase_assembly_property_selection_edit_candidates(
    const DocumentSnapshot& actual,
    const PhaseAssemblyInstancePropertyEditPreparation& assemblies,
    const std::vector<std::map<std::string,Entity,std::less<>>>& independently_admitted_ordinary_candidates,
    const StairPropertyReviewPreparation* stair_review = nullptr);

// Both contributions are independently reconstructed from the complete
// original snapshot. The historical base must have empty review decisions and
// no assembly lane. Only its exact replay-derived retirement closure may erase
// source owners/rows. This grants no Document publication authority.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_phase_assembly_with_mixed_property_selection_edit_candidates(
    const DocumentSnapshot& actual,
    const PhaseAssemblyInstancePropertyEditPreparation& assemblies,
    const MixedSelectionPropertyEditPreparation* base);

// Selected unchanged components follow the historical base's authenticated
// copied host bindings, retaining their exact nested part paths. Editing the
// component and replacing that same host requires explicit dependency carry;
// this helper cannot silently discard either contribution or duplicate it.
[[nodiscard]] std::map<AssemblyInstancePropertyEditTarget,AssemblyInstancePropertyEditTarget>
resolve_phase_assembly_with_mixed_property_correspondence(
    const DocumentSnapshot& actual,
    const PhaseAssemblyInstancePropertyEditPreparation& assemblies,
    const MixedSelectionPropertyEditPreparation* base);

// Detached dependency composition. Reconstruct both sealed contributions from
// the same original snapshot, keep an actual base-carried root as the survivor,
// and apply component operations in that root's final parent frame. Entities,
// correspondence and losing allocations are derived together. All component
// allocations, including elided copies/aliases, remain receipt reservations.
// This is not historical Properties14 authority, Document publication, or a
// persisted dialect. The base must be unreviewed five-through-thirteen/fifteen
// with no assembly lane; final room decisions remain the enclosing command's job.
struct CoordinatedComponentPropertyComposition {
    std::map<std::string,Entity,std::less<>> entities;
    std::map<AssemblyInstancePropertyEditTarget,AssemblyInstancePropertyEditTarget> original_to_proposed;
    std::vector<std::string> reserved_component_identity_ids;
    std::vector<std::string> elided_component_identity_ids;
};

struct ComponentSlabDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    std::map<std::string,std::string,std::less<>> slab_original_to_proposed;
    std::map<std::string,std::string,std::less<>> slab_selected_original_to_proposed;
    std::map<std::pair<std::string,std::string>,std::pair<std::string,std::string>> slab_hosted_bindings;
    std::vector<nlohmann::json> slab_phase_authorings;
};
// Reconstruct independent component geometry and source-partitioned slab
// geometry. No candidate map, historical base, or completed command is accepted.
// The one final reviewed dimension batch includes ordinary, clone and unit
// associations from both lanes, with the original source envelopes restored.
[[nodiscard]] ComponentSlabDirectionalResizeComposition compose_component_slab_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const SlabGeometryEditIntent>,
    const std::optional<ComponentSlabDirectionalResizeAllocation>&, std::string_view);
struct ComponentRoofOpeningDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    std::map<std::pair<std::string,std::string>,RoofOpeningGroupMember> roof_children;
    std::vector<nlohmann::json> phase_authorings;
};
struct ComponentStructuralDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    std::map<std::string,std::string,std::less<>> structural_original_to_proposed;
    std::map<std::string,std::string,std::less<>> structural_selected_original_to_proposed;
    std::map<std::pair<std::string,std::string>,std::pair<std::string,std::string>> structural_hosted_bindings;
    std::vector<nlohmann::json> structural_phase_authorings;
};
struct ComponentStairDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    std::map<std::string,std::string,std::less<>> stair_selected_original_to_proposed;
};
struct ComponentRoofDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    std::map<std::string,std::string,std::less<>> roof_selected_original_to_proposed;
};
// Complete resource and relationship inventories precede native source/archive
// validation, including units. Both gates require explicit shared-frame roof
// operators; the source-only gate needs no component placeholder.
void require_roof_directional_resize_source_admission(
    const DocumentSnapshot&, std::span<const RoofPlanResizeIntent>);
void preflight_component_roof_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&,
    std::span<const RoofPlanResizeIntent>);
[[nodiscard]] ComponentRoofDirectionalResizeComposition compose_component_roof_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const RoofPlanResizeIntent>, std::string_view);
struct ComponentRoofSkylightDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    std::map<std::string,std::string,std::less<>> roof_selected_original_to_proposed;
    std::map<std::pair<std::string,std::string>,RoofOpeningGroupMember> roof_children;
    std::vector<nlohmann::json> phase_authorings;
};
// Analytical aggregate gates reserve pending allocations, proof and dimension
// growth before any component draft or per-owner native manufacture.
void require_roof_skylight_directional_resize_source_admission(
    const DocumentSnapshot&, std::span<const AssemblyDirectionalResizeTarget>,
    std::span<const RoofPlanResizeIntent>, std::span<const RoofOpeningDirectionalResizeRequest>);
void preflight_component_roof_skylight_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&,
    std::span<const RoofPlanResizeIntent>, std::span<const RoofOpeningDirectionalResizeRequest>);
[[nodiscard]] ComponentRoofSkylightDirectionalResizeComposition compose_component_roof_skylight_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const RoofPlanResizeIntent>, std::span<const RoofOpeningDirectionalResizeRequest>, std::string_view);
// Pure analytical preflight, including units. Charged before component/native
// manufacture; no supplied candidate or completed command becomes authority.
void require_stair_directional_resize_source_admission(
    const DocumentSnapshot&, std::span<const StairDirectionalResizeRequest>);
void preflight_component_stair_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&,
    std::span<const StairDirectionalResizeRequest>);
[[nodiscard]] ComponentStairDirectionalResizeComposition compose_component_stair_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const StairDirectionalResizeRequest>, std::string_view);
struct ComponentPhysicalDirectionalResizeComposition {
    CoordinatedComponentPropertyComposition components;
    // Catalog copies remain family-local in the phase proofs. Only selected
    // owners, dimensions and presentation aliases have global destinations.
    std::map<std::string,std::string,std::less<>> original_to_proposed;
    std::map<std::string,std::string,std::less<>> selected_original_to_proposed;
    std::map<std::pair<std::string,std::string>,std::pair<std::string,std::string>> hosted_bindings;
    std::map<std::pair<std::string,std::string>,RoofOpeningGroupMember> roof_children;
    std::vector<nlohmann::json> phase_authorings;
};
[[nodiscard]] ComponentPhysicalDirectionalResizeComposition compose_component_physical_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const SlabGeometryEditIntent>,
    const std::optional<ComponentSlabDirectionalResizeAllocation>&,
    std::span<const StructuralDirectionalResizeRequest>,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>&,
    std::span<const RoofOpeningDirectionalResizeRequest>, std::string_view);
// Both lanes reconstruct the complete original source before the single shared
// dimension finalizer. Callers supply operators and exact inspected allocations.
[[nodiscard]] ComponentStructuralDirectionalResizeComposition compose_component_structural_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const StructuralDirectionalResizeRequest>,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>&, std::string_view);
// Reconstruct existing ordinary roof-child leaves and their complete phase32
// proof internally. Both lanes share the private original-to-final carry.
[[nodiscard]] ComponentRoofOpeningDirectionalResizeComposition compose_component_roof_opening_directional_resize(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditPreparation&,
    std::span<const RoofOpeningDirectionalResizeRequest>, std::string_view);
[[nodiscard]] CoordinatedComponentPropertyComposition compose_coordinated_component_property_dependencies(
    const DocumentSnapshot& actual,
    const PhaseAssemblyInstancePropertyEditPreparation& assemblies,
    const MixedSelectionPropertyEditPreparation& base);

// Reconstruct bounded scalar replacement leaves against the full original
// snapshot before composing ordinary contributions. Pending wall room review
// remains a detached stage. Only independently replayed explicit stair
// retirement admits physical erasure; this grants no publication authority.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_phase_property_selection_edit_candidates(
    const DocumentSnapshot& actual,
    const std::vector<nlohmann::json>& phase_authorings,
    const std::vector<std::map<std::string,Entity,std::less<>>>& independently_admitted_ordinary_candidates,
    const StairPropertyReviewPreparation* ordinary_stair_review = nullptr,
    const AssemblyInstancePropertyEditPreparation* assemblies = nullptr);

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
// Component phase membership completion additionally requires complete hosted
// catalog consequences. Each leaf must equal the exact typed pruning of source
// companions against its actual catalog absences. Composition keeps those raw
// source carriers until the final union is pruned; it grants no fresh enrollment.
[[nodiscard]] std::map<std::string,Entity,std::less<>> compose_ordinary_architectural_removal_candidates(
    const std::map<std::string,Entity,std::less<>>& source,
    const std::vector<std::map<std::string,Entity,std::less<>>>& candidates,
    bool allow_shared_reference_retirement=false,
    bool complete_hosted_catalog_consequences=false,
    bool complete_component_phase_memberships=false);

// Independently evaluated saved selections, including the empty-registry case.
[[nodiscard]] nlohmann::json phase_constraint_authoring_selections(
    const std::map<std::string,Entity,std::less<>>& source);

} // namespace sketch
