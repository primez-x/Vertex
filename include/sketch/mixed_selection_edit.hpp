#pragma once

#include "sketch/document.hpp"

#include <string_view>

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

// Versions one/two compose explicit ordinary selections with actual typed
// skylight children. Both complete leaves replay the original full snapshot.
// The ordinary request must use the matching dialect. Version one is rigid
// presentation only; version two grants explicit presentation uniform scale.
// A carried child requires its explicitly transformed parent; an independent
// child is authenticated by its original qualified ID, even under replacement.
[[nodiscard]] nlohmann::json make_mixed_selection_edit_intent(
    const DocumentSnapshot& source, const nlohmann::json& ordinary_request,
    const nlohmann::json& roof_authoring, const nlohmann::json& roof_members,
    std::string_view message);
[[nodiscard]] nlohmann::json validate_mixed_selection_edit_intent(const nlohmann::json& value);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replay_mixed_selection_edit(
    const DocumentSnapshot& source, const nlohmann::json& value);
[[nodiscard]] bool mixed_selection_edit_active_phase_policy(const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> mixed_selection_edit_source_saved_revision(const nlohmann::json& value);

} // namespace sketch
