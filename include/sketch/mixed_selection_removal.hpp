#pragma once

#include "sketch/architectural_drawing_removal.hpp"
#include "sketch/corner_selection_removal.hpp"
#include "sketch/hosted_opening_removal.hpp"
#include "sketch/ordinary_selection_removal.hpp"
#include "sketch/phase_selection_removal.hpp"
#include "sketch/roof_opening_group_edit.hpp"

namespace sketch {

// Mixed removal14 pairs architectural/drawing removal4 with exact qualified
// component dimension retirement. Historical mixed1/9/10 refuse that capability;
// its independently admitted ordinary and skylight stages remain unchanged.
// Mixed removal15 exclusively pairs architectural/drawing removal5 with actual
// selected owners and complete hosted component dimension retirement.
// Mixed removal16 exclusively pairs ordinary selection removal2 with complete
// hosted-opening component dimensions. Historical mixed3 requires selection1;
// both retain independent ordinary and typed roof-child replay authority.
// Mixed removal18 exclusively pairs architectural/drawing removal6's literal
// ordinary owner dimension membership retirement with independently selected
// skylights. Complete actual-source ordinary replay precedes the exact surviving
// child removal; host IDs and retained allocations never select roof owners.
// Mixed removal17 remains standalone with empty members and no child command.

// Dialect five retains independently reconstructed coordinated corner owners,
// their physical/phase consequences, and explicit ordinary/drawing selections.
// Dialect six alone enables version-two corner catalog-host completion.
// Dialect seven enables qualified corner-copy rows without selected owners.
// Dialect eight retains selected managed cuts. Dialects eleven, twelve and
// thirteen require corner dialects five, six and seven respectively, retaining
// complete component phase memberships for owners, qualified rows and cuts.
[[nodiscard]] DocumentSnapshot prepare_mixed_selection_removal_stage(
    const DocumentSnapshot& source, const CornerSelectionRemovalIntent& ordinary,
    const Command& ordinary_command);
[[nodiscard]] std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(
    const DocumentSnapshot& source, const CornerSelectionRemovalIntent& ordinary,
    const std::vector<RoofOpeningGroupMember>& members);
[[nodiscard]] nlohmann::json make_mixed_selection_removal_intent(
    const DocumentSnapshot& source, const CornerSelectionRemovalIntent& ordinary,
    const Command& ordinary_command, const std::vector<RoofOpeningGroupMember>& members,
    const std::optional<Command>& child_command);

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
// Dialect nine alone accepts architectural/drawing removal two's explicit
// component membership retirement. Historical dialect one requires removal one.
// Dialect ten alone accepts removal three's complete selected-roof memberships.
[[nodiscard]] nlohmann::json validate_mixed_selection_removal_intent(const nlohmann::json& value);
[[nodiscard]] bool mixed_selection_removal_active_phase_policy(const nlohmann::json& value);
[[nodiscard]] std::optional<Revision> mixed_selection_removal_source_saved_revision(const nlohmann::json& value);
[[nodiscard]] DrawingSelectionRemovalEntities replay_mixed_selection_removal(
    const DocumentSnapshot& source, const nlohmann::json& value);

// Closed dialect seventeen completes one standalone actual-source retirement.
// It retains an exact typed selector and canonical preceding command, with no
// selected roof children or child-command authority. Publication uses outer45.
[[nodiscard]] ArchitecturalDrawingRemovalIntent completed_component_selection_removal_authority(
    const nlohmann::json& value);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_component_selection_removal(
    const DocumentSnapshot& source, const ArchitecturalDrawingRemovalIntent& intent,
    const Command& base_command);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_component_selection_removal(
    const DocumentSnapshot& source, const CornerSelectionRemovalIntent& intent,
    const Command& base_command);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_component_selection_removal(
    const DocumentSnapshot& source, const OrdinarySelectionRemovalIntent& intent,
    const Command& base_command);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_component_selection_removal(
    const DocumentSnapshot& source, const HostedOpeningRemovalStageRequest& intent,
    const Command& base_command);
[[nodiscard]] ApplyBoundaryConstraintChanges prepare_completed_component_selection_removal(
    const DocumentSnapshot& source, const OpeningArchitecturalRemovalIntent& intent,
    const Command& base_command);

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

// Dialect two retains a complete phase-demolition command. These are explicit
// ordinary roots/qualified components, never the demolition's derived closure.
// Each preparation independently binds the proof to the actual full snapshot.
// Local component IDs are admitted only by canonical pure phase30 or
// phase15/inner8 or phase16/inner6 complete component membership authority.
// The remaining-child opt-in supplies spelling validation, not command authority.
[[nodiscard]] DocumentSnapshot prepare_mixed_selection_removal_stage(
    const DocumentSnapshot& source, const ArchitecturalSelectionRemovalIntent& ordinary,
    const Command& ordinary_command);
[[nodiscard]] std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(
    const DocumentSnapshot& source, const ArchitecturalSelectionRemovalIntent& ordinary,
    const std::vector<RoofOpeningGroupMember>& members,
    bool complete_component_phase_demolition = false,
    bool complete_coordinated_component_phase_memberships = false,
    bool complete_wall_component_phase_memberships = false);
[[nodiscard]] nlohmann::json make_mixed_selection_removal_intent(
    const DocumentSnapshot& source, const ArchitecturalSelectionRemovalIntent& ordinary,
    const Command& ordinary_command, const std::vector<RoofOpeningGroupMember>& members,
    const std::optional<Command>& child_command);

// Dialect three composes complete hosted-opening and wall deletion/review
// producers. Their explicit selection and original accepted room decisions
// remain independent from actual selected skylight children.
[[nodiscard]] DocumentSnapshot prepare_mixed_selection_removal_stage(
    const DocumentSnapshot& source, const OrdinarySelectionRemovalIntent& ordinary,
    const Command& ordinary_command);
[[nodiscard]] std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(
    const DocumentSnapshot& source, const OrdinarySelectionRemovalIntent& ordinary,
    const std::vector<RoofOpeningGroupMember>& members);
[[nodiscard]] nlohmann::json make_mixed_selection_removal_intent(
    const DocumentSnapshot& source, const OrdinarySelectionRemovalIntent& ordinary,
    const Command& ordinary_command, const std::vector<RoofOpeningGroupMember>& members,
    const std::optional<Command>& child_command);

// Dialect four retains complete phase deletion and accepted room review,
// with independently selected drawing rows. Historical dialect two keeps
// its pure phase34 meaning.
// Selection dialect two additionally retains explicit component demolition;
// its flag must agree with an exact pure phase30 base or completed phase47.
// Selection dialect three separately binds pure phase15/inner8 complete
// component memberships, or completed phase47 retaining that exact selector.
// Selection dialect four separately binds pure phase16/inner6 wall component
// memberships, or completed phase47 retaining that exact selector.
[[nodiscard]] DocumentSnapshot prepare_mixed_selection_removal_stage(
    const DocumentSnapshot& source, const PhaseSelectionRemovalIntent& ordinary,
    const Command& ordinary_command);
[[nodiscard]] std::vector<RoofOpeningGroupMember> mixed_selection_removal_remaining_children(
    const DocumentSnapshot& source, const PhaseSelectionRemovalIntent& ordinary,
    const std::vector<RoofOpeningGroupMember>& members);
[[nodiscard]] nlohmann::json make_mixed_selection_removal_intent(
    const DocumentSnapshot& source, const PhaseSelectionRemovalIntent& ordinary,
    const Command& ordinary_command, const std::vector<RoofOpeningGroupMember>& members,
    const std::optional<Command>& child_command);

} // namespace sketch
