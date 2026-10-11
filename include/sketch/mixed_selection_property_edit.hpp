#pragma once

#include "sketch/roof_opening_group_edit.hpp"
#include "sketch/selection_geometry_transform.hpp"
#include "sketch/selection_presentation_property_edit.hpp"
#include "sketch/selection_property_edit.hpp"

#include <optional>
#include <string_view>

namespace sketch {

// Dialect five is scalar Properties authority only; dialect six additionally
// requires presentation request two for saved-frame placement and mirrors.
// Each lane reads the same
// complete immutable original snapshot. No supplied entity map, transform,
// selected-object identity generation, rehosting, deletion or active-baseline
// replacement is accepted. Historical dialect five only admits presentation
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

// Closed dialect-five/six envelope: version, expected_revision, source_snapshot_digest,
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
[[nodiscard]] nlohmann::json validate_mixed_selection_property_edit_intent(const nlohmann::json&);
[[nodiscard]] MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(
    const DocumentSnapshot&, const std::optional<SelectionPropertyEditRequest>&,
    const std::optional<SelectionPresentationPropertyEditRequest>&,
    const std::optional<RoofOpeningGroupProperties>&, std::string_view message);
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
