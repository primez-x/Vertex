#pragma once

#include "sketch/architectural_drawing_removal.hpp"
#include "sketch/roof_opening_group_edit.hpp"

namespace sketch {

// Operation/thread-local memoization of independently admitted replay results.
// This token grants no source-validation authority and never survives an
// operation. Public preview/restore still validate every supplied history.
class MixedSelectionRemovalReplayScope final {
public:
    MixedSelectionRemovalReplayScope();
    ~MixedSelectionRemovalReplayScope();
    MixedSelectionRemovalReplayScope(const MixedSelectionRemovalReplayScope&)=delete;
    MixedSelectionRemovalReplayScope& operator=(const MixedSelectionRemovalReplayScope&)=delete;
};

// Closed removal-only composition. The ordinary map is independently derived
// from actual roots/qualified rows; typed roof children never select their host.
// The staged snapshots are detached proofs, not intermediate live history.
[[nodiscard]] nlohmann::json validate_mixed_selection_removal_intent(const nlohmann::json& value);
[[nodiscard]] bool mixed_selection_removal_active_phase_policy(const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> mixed_selection_removal_source_saved_revision(const nlohmann::json& value);
[[nodiscard]] DrawingSelectionRemovalEntities replay_mixed_selection_removal(
    const DocumentSnapshot& source, const nlohmann::json& value);

[[nodiscard]] DocumentSnapshot prepare_mixed_selection_removal_stage(
    const DocumentSnapshot& source, const ArchitecturalDrawingRemovalIntent& ordinary,
    const Command& ordinary_command);
[[nodiscard]] std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(
    const DocumentSnapshot& source, const ArchitecturalDrawingRemovalIntent& ordinary,
    const std::vector<RoofOpeningGroupMember>& members);
[[nodiscard]] nlohmann::json make_mixed_selection_removal_intent(
    const DocumentSnapshot& source, const ArchitecturalDrawingRemovalIntent& ordinary,
    const Command& ordinary_command, const std::vector<RoofOpeningGroupMember>& members,
    const std::optional<Command>& child_command);

} // namespace sketch
