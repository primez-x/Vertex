#pragma once

#include "sketch/architectural_drawing_removal.hpp"

#include <optional>

namespace sketch {

// Explicit selection only. Derived dependents, room decisions and fresh
// destinations remain exclusively in the complete phase deletion command.
struct PhaseSelectionRemovalIntent {
    ArchitecturalSelectionRemovalIntent architectural;
    DrawingSelectionRemovalIntent drawing;
};

[[nodiscard]] nlohmann::json encode_phase_selection_removal_intent(
    const PhaseSelectionRemovalIntent& intent);
[[nodiscard]] PhaseSelectionRemovalIntent decode_phase_selection_removal_intent(
    const nlohmann::json& value);
[[nodiscard]] ArchitecturalDrawingRemovalIntent phase_selection_removal_authority(
    const PhaseSelectionRemovalIntent& intent);

// Independently admits exact explicit roots against a pure outer33 historical
// wall demolition or outer34 typed demolition. Does not accept wrapper47 or
// derive root authority from raw entity payloads. Complete review admission
// remains the responsibility of the source-owning preview/composition paths.
[[nodiscard]] ArchitecturalSelectionRemovalIntent phase_selection_removal_base_authority(
    const DocumentSnapshot& source, const Command& pure_phase_deletion_command,
    const ArchitecturalSelectionRemovalIntent& explicit_selection);

[[nodiscard]] DocumentSnapshot prepare_phase_selection_removal_stage(
    const DocumentSnapshot& source, const PhaseSelectionRemovalIntent& intent,
    const Command& original_command);

// Thread/operation-local reuse of fully admitted complete-source results.
// No caller-supplied validated stage or reusable authority token is exposed.
class PhaseSelectionRemovalReplayScope final {
public:
    PhaseSelectionRemovalReplayScope();
    ~PhaseSelectionRemovalReplayScope();
    PhaseSelectionRemovalReplayScope(const PhaseSelectionRemovalReplayScope&)=delete;
    PhaseSelectionRemovalReplayScope& operator=(const PhaseSelectionRemovalReplayScope&)=delete;
};

[[nodiscard]] nlohmann::json validate_completed_phase_selection_removal_intent(
    const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> phase_selection_removal_source_saved_revision(
    const nlohmann::json& value);
[[nodiscard]] DrawingSelectionRemovalEntities replay_completed_phase_selection_removal(
    const DocumentSnapshot& source, const nlohmann::json& value);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_phase_selection_removal(
    const DocumentSnapshot& source, const PhaseSelectionRemovalIntent& intent,
    const Command& base_command);

} // namespace sketch
