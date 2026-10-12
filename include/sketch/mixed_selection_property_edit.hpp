#pragma once

#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/roof_opening_group_edit.hpp"
#include "sketch/selection_geometry_transform.hpp"
#include "sketch/selection_presentation_property_edit.hpp"
#include "sketch/selection_property_edit.hpp"
#include "sketch/stair_property_review.hpp"
#include "sketch/phase_selection_property_edit.hpp"
#include "sketch/phase_assembly_instance_property_edit.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace sketch {

// Dialect five is scalar Properties authority only; dialect six additionally
// requires presentation request two for saved-frame placement and mirrors.
// Dialect seven requires ordinary request two for column/beam scalar profiles
// and may compose either explicit presentation request version.
// Dialect eight requires ordinary request three for stair/railing scalar
// profiles and may compose either explicit presentation request version.
// Dialect nine separates a mandatory authenticated stair review request from
// ordinary request one/two and admits only its independently derived removal
// closure. Selected scalar owners remain present except explicitly retired
// dependent rails; retirement grants no other lane deletion or replacement
// authority.
// Dialect ten adds authenticated qualified assembly instance/path Properties.
// Its stair review field is always present and may be null; a nonnull review
// retains dialect nine's exact scalar/removal closure authority.
// Dialect eleven adds sealed source-derived active-baseline replacements and
// explicit final-stage phase room completion. Its phase request also owns the
// complete scalar/skylight composite, including ordinary/proposed members.
// Dialect twelve retains eleven's exact fields and requires phase request three
// with scalar request four. Nullable presentation/assembly/skylight lanes remain
// explicit. Zero allocation cohorts permit ordinary elevation edits with the
// same source-bound replay and mandatory source/destination room-plane review.
// Historical dialects five through eleven reject the elevation authority.
// Dialect thirteen retains twelve's exact fields and requires phase request
// four with scalar request five for original-source hosted slab elevation
// carry. Wall elevation remains optional; any affected walls retain twelve's
// mandatory source/destination room-plane review. Historical dialects five
// through twelve reject the new slab carry authority.
// Dialect fourteen composes a sealed actual-source component alternative
// contribution with an optional independently replayed historical five-through-
// thirteen intent. Its component request is the sole assembly Properties lane;
// final room decisions belong to the complete composed stage.
// Dialect fifteen is a separate detached phase request five envelope, with
// eleven through thirteen's exact fields and explicit qualified wall-hosted
// copy authority. Scalar requests one through five retain their own exact
// authority; a null scalar retains child-only selection. Elevation and hosted
// slab carry require the underlying scalar four/five codec. Qualified wall
// room decisions use explicit leaf eleven on the final composed stage.
// Dialect sixteen explicitly coordinates component operations with actual
// base-carried hosts. It requires an unreviewed five-through-thirteen/fifteen
// base and coordinate_component_hosts:true; entities and correspondence are
// reconstructed together. Fourteen retains its historical composition meaning.
// This preparation does not grant Document publication or a format floor.
// Each lane reads the same
// complete immutable original snapshot. No supplied entity map, transform,
// or caller-derived selected-object identity map is accepted. Historical
// dialects five through eight retain their no-deletion
// authority. Historical dialect five only admits presentation
// request one and never acquires the newer pose authority.
// Room decisions retain their existing typed topology
// authority and require Document's final identity-lifetime admission.
class MixedSelectionPropertyEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& intent() const noexcept;
    [[nodiscard]] const DocumentSnapshot& preview_snapshot() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;
    [[nodiscard]] const std::vector<SelectionGeometryRoomReviewRequirement>& required_room_reviews() const noexcept;
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& selected_original_to_proposed() const noexcept;
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& original_to_proposed() const noexcept;
    using ComponentIdentityMap = std::map<AssemblyInstancePropertyEditTarget, AssemblyInstancePropertyEditTarget>;
    [[nodiscard]] const ComponentIdentityMap& component_original_to_proposed() const noexcept;
    // Coordinated component dialects' independently reconstructed allocation inventory;
    // historical dialects leave these detached coordination inventories empty.
    [[nodiscard]] const std::vector<std::string>& reserved_component_identity_ids() const noexcept;
    [[nodiscard]] const std::vector<std::string>& elided_component_identity_ids() const noexcept;
    [[nodiscard]] const PhaseStairReplacementChildIdentityMap& stair_child_original_to_proposed() const noexcept;
    using HostedBindingMap = std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>>;
    [[nodiscard]] const HostedBindingMap& hosted_binding_original_to_proposed() const noexcept;
    [[nodiscard]] const std::map<std::pair<std::string, std::string>, RoofOpeningGroupMember>& roof_child_original_to_proposed() const noexcept;
    [[nodiscard]] const std::vector<PhysicalWallRoomPhaseReviewInventory>& required_phase_room_reviews() const noexcept;
    [[nodiscard]] const std::vector<std::size_t>& required_phase_room_review_indices() const noexcept;
    [[nodiscard]] const std::vector<PhaseWallReplacementAuthoringPreview>& phase_wall_previews() const noexcept;
    [[nodiscard]] const std::vector<nlohmann::json>& phase_authorings() const noexcept;
private:
    MixedSelectionPropertyEditPreparation(const DocumentSnapshot&, nlohmann::json,
        std::map<std::string, Entity, std::less<>>,
        std::vector<SelectionGeometryRoomReviewRequirement>,
        std::optional<PhaseSelectionPropertyEditPreparation> = std::nullopt,
        std::vector<PhysicalWallRoomPhaseReviewInventory> = {},
        std::optional<PhaseAssemblyInstancePropertyEditPreparation> = std::nullopt,
        ComponentIdentityMap = {},
        std::vector<std::string> reserved_component_identity_ids = {},
        std::vector<std::string> elided_component_identity_ids = {});
    nlohmann::json intent_;
    DocumentSnapshot preview_;
    std::string source_digest_;
    std::vector<SelectionGeometryRoomReviewRequirement> room_reviews_;
    std::optional<PhaseSelectionPropertyEditPreparation> phase_;
    std::optional<PhaseAssemblyInstancePropertyEditPreparation> component_;
    ComponentIdentityMap component_identity_map_;
    std::vector<std::string> reserved_component_identity_ids_;
    std::vector<std::string> elided_component_identity_ids_;
    std::map<std::string, std::string, std::less<>> selected_identity_map_;
    std::map<std::string, std::string, std::less<>> identity_map_;
    HostedBindingMap hosted_bindings_;
    std::map<std::pair<std::string, std::string>, RoofOpeningGroupMember> roof_children_;
    std::vector<PhysicalWallRoomPhaseReviewInventory> phase_room_reviews_;
    std::vector<std::size_t> phase_room_review_indices_;
    friend MixedSelectionPropertyEditPreparation replay_mixed_selection_property_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
    friend DocumentSnapshot preview_mixed_selection_property_room_stage(
        const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
        const std::vector<nlohmann::json>&);
    friend DocumentSnapshot preview_mixed_selection_property_room_stage(
        const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
        const std::vector<nlohmann::json>&, const std::vector<nlohmann::json>&);
    friend DocumentSnapshot preview_mixed_selection_property_phase_room_stage(
        const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
        const std::vector<nlohmann::json>&);
};

// Closed dialect-five/six/seven/eight envelope: version, expected_revision, source_snapshot_digest,
// source_authoring_digest, source_entities_digest, source_saved_revision,
// ordinary_request, presentation_request, skylight_properties,
// roof_opening_members, room_reviews, message. Optional lanes are null; at
// least one lane and 1..1000 unique qualified members are required. Skylight
// Properties have exactly version:1,width,depth,frame_width,curb_height,
// glazing_thickness,rotation_radians,retain_invariant_members; quantities use
// the existing {quantity,default_unit} exact receipt. The selected child roster
// is sorted {roof_id,opening_id} and is nonempty iff that lane is present.
// An all-null skylight patch requires retain_invariant_members:true and still
// independently proves every actual selected row; it grants no parent changes.
// Room decisions use canonical existing codecs, up to 32 disjoint planes.
// Wire/complexity limits: 4 MiB, 100000 values, depth 64, portable UTF-8.
// Dialect nine adds exactly stair_review_request (mandatory nonnull canonical
// v1). Its profile IDs join the complete selected roster; ordinary_request is
// null or v1/v2 and presentation_request is null or v1/v2. All lanes bind the
// same full source fences, with no duplicated stair scalar authority.
// Dialect ten adds exactly assembly_request (mandatory nonnull canonical v1)
// and always carries stair_review_request (null or canonical v1). With no stair
// review, ordinary_request may be null or v1/v2/v3; with a review it is null or
// v1/v2. Presentation is null or v1/v2. Every assembly catalog/instance/optional
// entity/path target joins the same bounded complete qualified selected roster.
[[nodiscard]] nlohmann::json validate_mixed_selection_property_edit_intent(const nlohmann::json&);
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(
    const DocumentSnapshot&, const std::optional<SelectionPropertyEditRequest>&,
    const std::optional<SelectionPresentationPropertyEditRequest>&,
    const std::optional<RoofOpeningGroupProperties>&, std::string_view message);
// The reviewed overload first validates the complete ordinary typed roster,
// then extracts exactly its stair/railing scalar members into the authenticated
// review request. The supplied review must have those exact profiles and actual
// source fences; dependency choices retain the existing ordinary producer's
// closed authority. Other ordinary families retain their exact scalar inputs.
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(
    const DocumentSnapshot&, const std::optional<SelectionPropertyEditRequest>&,
    const std::optional<SelectionPresentationPropertyEditRequest>&,
    const std::optional<RoofOpeningGroupProperties>&, std::string_view message,
    const StairPropertyReviewRequest&);
// The assembly lane is nonempty and is prepared against the same original
// snapshot. A supplied review extracts the exact complete ordinary v3 stair
// profiles just as the reviewed overload above; assembly targets never grant
// host edits, alias authoring, shared type changes or new removal authority.
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(
    const DocumentSnapshot&, const std::optional<SelectionPropertyEditRequest>&,
    const std::optional<SelectionPresentationPropertyEditRequest>&,
    const std::optional<RoofOpeningGroupProperties>&, std::string_view message,
    std::span<const AssemblyInstancePropertyEditIntent>,
    const std::optional<StairPropertyReviewRequest>& reviewed = std::nullopt);
// Dialect eleven replaces ordinary_request with mandatory phase_request v1/v2.
// This overload emits twelve for phase request v3, thirteen for v4 and the
// detached fifteen only for explicit complete_wall_hosted_instances v5;
// twenty-one has exactly fifteen's fields and requires phase request v6, with
// explicit complete_stair_component_dimensions:true. Its wall authority still
// follows phase_request.complete_wall_hosted_instances, which may be false.
// Twenty-four retains twenty-one's exact fields and requires phase request v7,
// with literal complete_wall_component_dimensions:true and hosted completion.
// Its stair dimension permission is an independent explicit Boolean. Wall room
// completion uses canonical leaf twelve with hosted and dimension completion.
// Twenty-seven retains twenty-four's exact fields and requires phase request
// eight, with independent explicit roof/slab/structural dimension Booleans,
// at least one enabled family, wall dimension and hosted completion true, and
// an independent stair dimension Boolean. Each family retains its own sealed
// allocation dimension-map authority; wall room completion remains leaf twelve.
// ordinary overloads also emit thirteen for explicit hosted slab carry.
// Assembly request is nullable; stair review stays solely inside phase_request.
// phase_room_reviews contains ordered {phase_authoring_index,wall_replacement}
// rows: the completed wall leaf must preserve all captured scalar/allocation
// fields and carries only explicit final-stage room/relationship decisions.
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(
    const DocumentSnapshot&, const std::optional<SelectionPropertyEditRequest>&,
    const std::optional<SelectionPresentationPropertyEditRequest>&,
    const std::optional<RoofOpeningGroupProperties>&, std::string_view message,
    std::span<const AssemblyInstancePropertyEditIntent>,
    const std::vector<PhaseSelectionPropertyIdentityAllocation>&,
    const std::vector<PhaseStairReplacementDependencyDisposition>&,
    bool complete_wall_hosted_instances = false,
    bool complete_stair_component_dimensions = false,
    bool complete_wall_component_dimensions = false,
    bool complete_roof_component_dimensions = false,
    bool complete_slab_component_dimensions = false,
    bool complete_structural_component_dimensions = false);
// Closed dialect fourteen: version, the five complete source fences,
// component_request (sealed v1), base_intent (null or historical five-through-
// thirteen without a nonnull assembly_request), room_reviews,
// phase_room_reviews, message. The nonempty component roster and historical
// roster share a 1000-unique-qualified-target bound. The base retains its exact
// original source fences and message, with empty room decisions; no nested
// fourteen, fabricated snapshot or supplied candidate is admitted.
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&,
    const MixedSelectionPropertyEditPreparation* base, std::string_view message);
// Closed dialect sixteen has fourteen's fields plus mandatory literal
// coordinate_component_hosts:true and component_identity_reservations, a sorted
// unique array of at most 4096 portable UTF-8 strings, each at most 4096 bytes.
// Source replay must reproduce the
// complete reservation receipt, including original computed render aliases.
// This receipt reserves identity lifetime only. Its mandatory base is unreviewed five-through-
// thirteen or fifteen with no assembly lane, the same complete original source
// fences/message and no nested component dialect. Combined qualified selection
// and wire budgets remain identical to fourteen. The sealed detached result
// retains all original component allocation reservations, including elided IDs.
// Dialect twenty-two has exactly sixteen's fields plus literal
// complete_stair_component_dimensions:true and requires an actual nonnull
// base twenty-one. Component request one and host coordination are unchanged;
// fourteen/sixteen cannot accept base twenty-one or any component dialect.
// Dialect twenty-five has twenty-two's exact fields plus literal
// complete_wall_component_dimensions:true. Its mandatory base is twenty-four;
// its explicit Boolean stair dimension permission must equal the base phase
// request's permission. Component request one and host coordination are unchanged.
// Dialect twenty-eight retains twenty-five's exact fields plus explicit Boolean
// complete_roof_component_dimensions, complete_slab_component_dimensions and
// complete_structural_component_dimensions. Its mandatory sealed base is
// twenty-seven; all three family permissions and the stair permission exactly
// match that base's phase request eight, and wall completion is literal true.
// Historical component dialects cannot accept a base twenty-seven.
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_coordinated_component_property_edit(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&,
    const MixedSelectionPropertyEditPreparation& base, std::string_view message);

// Dialect thirty-two adds complete_instance_profile_geometry:true to those
// source/reservation/presentation fields and requires component request three.
// Its base is null or an unreviewed noncomponent five-through-thirteen, fifteen,
// twenty-one, twenty-four or twenty-seven. Family dimension permissions come
// only from that sealed base; the wrapper does not invent or repeat them.
// Historical component requests and wrappers retain their original authority.
// Closed dialect thirty-three uses the same source/reservation/presentation
// fields as thirty-two, with complete_component_dimensions:true replacing
// complete_instance_profile_geometry. It requires nongeometry component request
// four, whose exact dimension allocations and presentation roster derive from
// the complete original source. It accepts the same optional sealed base
// roster as thirty-two; family permissions remain exclusively base-owned.
// Closed dialect thirty-four retains thirty-two's exact fields and requires
// component request five with at least one topology-preserving leaf intent three.
// It inherits the complete geometry dimension/presentation carry, optional sealed
// base roster, host coordination and final room review contract. Thirty-two and
// thirty-three retain their historical component request three/four authority.
// Closed dialect thirty-five retains thirty-four's exact fields and requires
// component request six with at least one initial-topology leaf intent four.
// It retains the same source-derived dimension/presentation carry, optional
// sealed base, host coordination and final room review. Producers prefer it
// whenever initial topology is authored; historical wrappers retain their
// exact component request authority and reject initial topology.
// Closed dialect thirty-six retains thirty-five's exact fields and requires
// component request seven with at least one reviewed-topology leaf intent five.
// Explicit source profile-dimension decisions own remap/retirement authority;
// producers prefer thirty-six whenever profile topology review is authored.
// Closed dialect thirty-seven retains those fields and requires component
// request eight for atomic anonymous affine deformation. Thirty-eight requires
// component request nine for complete reviewed initial own-profile geometry;
// old wrappers cannot acquire either authority. Both retain exact source-bound
// dimension decisions, presentation carry, host coordination and optional base.
//
// Closed dialect seventeen retains Properties16's source/reservation fields plus
// carry_component_presentation:true. It requires component request two, whose
// exact source-derived overlay allocations copy typed presentation references.
// A null base is a component-only edit; otherwise the same sealed noncomponent
// base5..13/15 is composed with host carry. Historical fourteen/sixteen retain
// component request one and their original base contracts.
// Dialect twenty-three has exactly seventeen's fields plus literal
// complete_stair_component_dimensions:true, requires an actual nonnull base
// twenty-one and component request two, and retains the same host/presentation
// coordination. Seventeen cannot accept base twenty-one. No component dialect
// may be used as a base. Producers choose twenty-two/twenty-three only for base twenty-one.
// Dialect twenty-six has twenty-three's exact fields plus literal
// complete_wall_component_dimensions:true and requires an actual base twenty-four.
// It retains component request two and host/presentation coordination, with
// explicit Boolean stair dimension permission matching the base phase request.
// Producers choose twenty-five/twenty-six only for actual wall-authorized base twenty-four.
// Dialect twenty-nine retains twenty-six's exact fields plus the three explicit
// family dimension Booleans from twenty-eight. It requires an actual sealed
// base twenty-seven, with those permissions and the independent stair permission
// matching its phase request eight, and literal wall completion true. Component
// request two and host/presentation coordination remain unchanged. Producers
// choose twenty-eight/twenty-nine only for the actual request-eight base.
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_component_presentation_property_edit(
    const DocumentSnapshot&, const PhaseAssemblyInstancePropertyEditAuthoring&,
    const MixedSelectionPropertyEditPreparation* base, std::string_view message);
// Preparation accepts only empty room_reviews. A copied read-only snapshot
// retains complete history/save/assets/opaque state and has no admission token.
[[nodiscard]] MixedSelectionPropertyEditPreparation replay_mixed_selection_property_edit_preparation(
    const DocumentSnapshot&, const nlohmann::json&);
void require_mixed_selection_property_edit_source_current(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&);
// Replays exactly the required_room_reviews() prefix in that order. Every
// decision retains original full-source metadata and the exact preceding
// source-derived entity digest. The returned copied snapshot is read-only,
// carries no admission token, and is solely a correspondence/drawing input.
[[nodiscard]] DocumentSnapshot preview_mixed_selection_property_room_stage(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
    const std::vector<nlohmann::json>& preceding_room_reviews);
[[nodiscard]] DocumentSnapshot preview_mixed_selection_property_room_stage(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
    const std::vector<nlohmann::json>& preceding_room_reviews,
    const std::vector<nlohmann::json>& completed_phase_room_reviews);
// Independently rederive all lanes and every mandatory review against the
// captured source, returning canonical history intent without publication.
[[nodiscard]] nlohmann::json complete_mixed_selection_property_edit(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
    const std::vector<nlohmann::json>& room_reviews);
[[nodiscard]] nlohmann::json complete_mixed_selection_property_edit(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
    const std::vector<nlohmann::json>& room_reviews,
    const std::vector<nlohmann::json>& phase_room_reviews);
[[nodiscard]] DocumentSnapshot preview_mixed_selection_property_phase_room_stage(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
    const std::vector<nlohmann::json>& preceding_phase_room_reviews);
// Complete derived map and source-replayed typed room transitions. Full final
// physical/constraint admission, room/fresh-ID policies and publication remain
// owned by Document. Scalar Properties grant no dimension movement authority.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_mixed_selection_property_edit(
    const DocumentSnapshot&, const nlohmann::json&);
[[nodiscard]] bool mixed_selection_property_edit_has_geometry(const nlohmann::json&);
[[nodiscard]] bool mixed_selection_property_edit_active_phase_policy(const nlohmann::json&);
[[nodiscard]] std::vector<nlohmann::json> mixed_selection_property_edit_room_reviews(const nlohmann::json&);
[[nodiscard]] std::vector<nlohmann::json> mixed_selection_property_edit_phase_room_reviews(const nlohmann::json&);
[[nodiscard]] bool mixed_selection_property_edit_retires_dependencies(const nlohmann::json&);
[[nodiscard]] bool mixed_selection_property_edit_has_phase_replacements(const nlohmann::json&);
[[nodiscard]] std::optional<Revision> mixed_selection_property_edit_source_saved_revision(const nlohmann::json&);

} // namespace sketch
