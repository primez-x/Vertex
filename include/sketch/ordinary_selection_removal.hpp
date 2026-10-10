#pragma once

#include "sketch/architectural_drawing_removal.hpp"
#include "sketch/opening_architectural_removal.hpp"

#include <optional>

namespace sketch {

// Exactly one ordinary deletion mode. Wall roots remain explicit even for the
// historical single-wall raw proof; erase rows never supply selection authority.
// These are complete actual-source deletions, not general geometry edit proofs.
struct OrdinarySelectionRemovalIntent {
    std::optional<OpeningArchitecturalRemovalIntent> openings;
    nlohmann::json wall_geometry_proof=nullptr;
    DrawingSelectionRemovalIntent drawing;
    std::vector<std::string> wall_ids;
};

[[nodiscard]] nlohmann::json encode_ordinary_selection_removal_intent(
    const OrdinarySelectionRemovalIntent& intent);
[[nodiscard]] OrdinarySelectionRemovalIntent decode_ordinary_selection_removal_intent(
    const nlohmann::json& value);

// Explicit roots/qualified rows only, including declared roof split slots.
// Hosted skylights never promote their roof merely through host membership.
[[nodiscard]] ArchitecturalDrawingRemovalIntent ordinary_selection_removal_authority(
    const OrdinarySelectionRemovalIntent& intent);

// Independently reconstructs the deletion and previews the complete original
// command, including retained room decisions and optional drawing retirement.
// Publication and the full captured-snapshot fence remain with the controller.
[[nodiscard]] DocumentSnapshot prepare_ordinary_selection_removal_stage(
    const DocumentSnapshot& source, const OrdinarySelectionRemovalIntent& intent,
    const Command& original_command);

// Operation-local reuse of fully admitted source-bound composition results.
// No cache or caller-supplied validated-stage authority escapes this scope.
class OrdinarySelectionRemovalReplayScope final {
public:
    OrdinarySelectionRemovalReplayScope();
    ~OrdinarySelectionRemovalReplayScope();
    OrdinarySelectionRemovalReplayScope(const OrdinarySelectionRemovalReplayScope&)=delete;
    OrdinarySelectionRemovalReplayScope& operator=(const OrdinarySelectionRemovalReplayScope&)=delete;
};

// Closed completion for explicit drawing retirement after one independently
// admitted hosted-opening or wall deletion, including complete room decisions.
[[nodiscard]] nlohmann::json validate_completed_ordinary_selection_removal_intent(
    const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> ordinary_selection_removal_source_saved_revision(
    const nlohmann::json& value);
[[nodiscard]] DrawingSelectionRemovalEntities replay_completed_ordinary_selection_removal(
    const DocumentSnapshot& source, const nlohmann::json& value);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_ordinary_selection_removal(
    const DocumentSnapshot& source, const OrdinarySelectionRemovalIntent& intent,
    const Command& base_command);

} // namespace sketch
