#pragma once

#include "sketch/document.hpp"
#include "sketch/selection_geometry_transform.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace sketch {

// Operation-local replay memo only. This conveys no snapshot, leaf admission,
// identity lifetime or publication authority; every replay checks its source.
class MixedSelectionEditReplayScope final {
public:
    MixedSelectionEditReplayScope();
    ~MixedSelectionEditReplayScope();
    MixedSelectionEditReplayScope(const MixedSelectionEditReplayScope&) = delete;
    MixedSelectionEditReplayScope& operator=(const MixedSelectionEditReplayScope&) = delete;
};

// Sealed original-source gesture preparation. The drawing snapshot contains all
// independently derived physical/nongeometry/roof consequences, with retained
// rooms still awaiting the typed geometry review. It is read-only, has no
// Document admission token, and is never a command or publication authority.
class MixedSelectionEditPreparation final {
public:
    [[nodiscard]] const nlohmann::json& intent() const noexcept;
    [[nodiscard]] const std::optional<SelectionGeometryTransformPreparation>&
    geometry_preparation() const noexcept;
    [[nodiscard]] const DocumentSnapshot& preview_snapshot() const noexcept;
    [[nodiscard]] const std::string& source_snapshot_digest() const noexcept;

private:
    MixedSelectionEditPreparation(const DocumentSnapshot& source, nlohmann::json intent,
        std::optional<SelectionGeometryTransformPreparation> geometry,
        std::map<std::string, Entity, std::less<>> drawing_entities);

    nlohmann::json intent_;
    std::optional<SelectionGeometryTransformPreparation> geometry_;
    DocumentSnapshot preview_;
    std::string source_digest_;

    friend MixedSelectionEditPreparation prepare_mixed_selection_edit(
        const DocumentSnapshot&, const nlohmann::json&, const nlohmann::json&,
        const nlohmann::json&, std::string_view, const nlohmann::json&);
};

// Accepts only ordinary/mixed dialect four, with geometry.room_reviews empty.
// Every leaf and the geometry preparation derive the same original snapshot;
// old room state in the drawing preview does not discharge review requirements.
[[nodiscard]] MixedSelectionEditPreparation prepare_mixed_selection_edit(
    const DocumentSnapshot& source, const nlohmann::json& ordinary_request,
    const nlohmann::json& roof_authoring, const nlohmann::json& roof_members,
    std::string_view message, const nlohmann::json& roof_identity_requests = nlohmann::json::array());

// Rechecks the full original source, including save state, and replays every
// mandatory room decision through the complete historical mixed replay. Returns
// the canonical completed intent for existing envelope52 admission; this does
// not publish a command or give the drawing snapshot authoring authority.
[[nodiscard]] nlohmann::json complete_mixed_selection_edit(
    const DocumentSnapshot& source, const MixedSelectionEditPreparation& prepared,
    const std::vector<nlohmann::json>& room_reviews);

// Versions one/two/three compose explicit ordinary selections with actual typed
// skylight children. Both complete leaves replay the original full snapshot.
// The ordinary request must use the matching dialect. Version one is rigid
// presentation only; version two grants explicit presentation uniform scale.
// Version three adds closed actual-source geometry and complete typed room
// decisions. The same original source independently derives every lane;
// ordinary payloads cannot grant wall/room authority.
// Version three permits an empty roof roster, giving complete ordinary geometry
// edits the same typed publication path. Versions one/two still require children.
// A carried child requires its explicitly transformed parent; an independent
// child is authenticated by its original qualified ID, even under replacement.
// Version four pairs ordinary dialect four, including geometry request two's
// explicit positive similarity scales, with text-baseline and invariant-child
// proof. A nonnull roof_identity_requests array selects this version, including
// an empty array. Its sorted unique rows are {roof_id, transform, opening_ids};
// transform uses the canonical rigid selection-geometry root encoding, and
// opening_ids is the sorted complete roster of that roof's selected identity
// children. Those children use coverage:"identity", have no ordinary parent,
// replay their own actual-source operator to an exactly unchanged full map,
// and require their entire owner unchanged in the final composed result.
// This conveys neither changed-child nor phase replacement authority.
// Version five is the separate mixed scalar Properties dialect declared in
// mixed_selection_property_edit.hpp. It rederives physical/presentation/child
// edits together and requires every affected retained-room review. Historical
// gesture requests keep their original versions and meanings.
[[nodiscard]] nlohmann::json make_mixed_selection_edit_intent(
    const DocumentSnapshot& source, const nlohmann::json& ordinary_request,
    const nlohmann::json& roof_authoring, const nlohmann::json& roof_members,
    std::string_view message, const nlohmann::json& roof_identity_requests = nullptr);
[[nodiscard]] nlohmann::json validate_mixed_selection_edit_intent(const nlohmann::json& value);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_mixed_selection_edit(
    const DocumentSnapshot& source, const nlohmann::json& value);
[[nodiscard]] bool mixed_selection_edit_active_phase_policy(const nlohmann::json& value);
[[nodiscard]] bool mixed_selection_edit_has_geometry(const nlohmann::json& value);
[[nodiscard]] std::vector<nlohmann::json> mixed_selection_edit_room_reviews(const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> mixed_selection_edit_source_saved_revision(const nlohmann::json& value);

} // namespace sketch
