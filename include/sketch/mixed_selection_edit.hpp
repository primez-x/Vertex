#pragma once

#include "sketch/document.hpp"

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
// Version four pairs ordinary dialect four with explicit text-baseline and invariant-child
// proof. A nonnull roof_identity_requests array selects this version, including
// an empty array. Its sorted unique rows are {roof_id, transform, opening_ids};
// transform uses the canonical rigid selection-geometry root encoding, and
// opening_ids is the sorted complete roster of that roof's selected identity
// children. Those children use coverage:"identity", have no ordinary parent,
// replay their own actual-source operator to an exactly unchanged full map,
// and require their entire owner unchanged in the final composed result.
// This conveys neither changed-child nor phase replacement authority.
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
