#pragma once

#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/roof_opening_group_edit.hpp"
#include "sketch/selection_geometry_transform.hpp"
#include "sketch/selection_presentation_property_edit.hpp"
#include "sketch/selection_property_edit.hpp"
#include "sketch/stair_property_review.hpp"

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
// Each lane reads the same
// complete immutable original snapshot. No supplied entity map, transform,
// selected-object identity generation or active-baseline replacement is
// accepted. Historical dialects five through eight retain their no-deletion
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
private:
    MixedSelectionPropertyEditPreparation(const DocumentSnapshot&, nlohmann::json,
        std::map<std::string, Entity, std::less<>>,
        std::vector<SelectionGeometryRoomReviewRequirement>);
    nlohmann::json intent_;
    DocumentSnapshot preview_;
    std::string source_digest_;
    std::vector<SelectionGeometryRoomReviewRequirement> room_reviews_;
    friend MixedSelectionPropertyEditPreparation replay_mixed_selection_property_edit_preparation(
        const DocumentSnapshot&, const nlohmann::json&);
    friend DocumentSnapshot preview_mixed_selection_property_room_stage(
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
// Independently rederive all lanes and every mandatory review against the
// captured source, returning canonical history intent without publication.
[[nodiscard]] nlohmann::json complete_mixed_selection_property_edit(
    const DocumentSnapshot&, const MixedSelectionPropertyEditPreparation&,
    const std::vector<nlohmann::json>& room_reviews);
// Complete derived map and source-replayed typed room transitions. Full final
// physical/constraint admission, room/fresh-ID policies and publication remain
// owned by Document. Scalar Properties grant no dimension movement authority.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_mixed_selection_property_edit(
    const DocumentSnapshot&, const nlohmann::json&);
[[nodiscard]] bool mixed_selection_property_edit_has_geometry(const nlohmann::json&);
[[nodiscard]] bool mixed_selection_property_edit_active_phase_policy(const nlohmann::json&);
[[nodiscard]] std::vector<nlohmann::json> mixed_selection_property_edit_room_reviews(const nlohmann::json&);
[[nodiscard]] std::optional<Revision> mixed_selection_property_edit_source_saved_revision(const nlohmann::json&);

} // namespace sketch
