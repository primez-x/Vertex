#pragma once

#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_wall_replacement_command.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// Detached value snapshots for canvas preview. Every authority field in intent
// describes the actual input document; physical.edited_entities is never a
// substitute DocumentSnapshot and must not be used to invent source authority.
struct PhaseWallCanvasProposal {
    PhaseConstraintAuthoringIntent intent;
    PhaseWallReplacementAuthoringPreview physical;
};

// Discover semantic roots before any original-geometry solve. No replacement
// roots returns nullopt so the caller can use its ordinary authoring route.
// Multiple actual registries and unsupported affected dependencies throw
// std::invalid_argument instead of falling back to editing shared originals.
//
// For an admitted plan, allocate_fresh_identity receives each exact original
// entity/owned-child ID once. Returned identities must be fresh and injective;
// strict replacement replay checks them against the complete actual source.
// Retained-history reservations remain the final Document admission's duty.
// This helper retains no allocations and mutates no document or history.
//
// The returned complete entity map is a PHYSICAL PREVIEW, not a completed edit.
// When physical.needs_room_review is true, the command obtained from
// phase_wall_replacement_authoring_command(proposal.intent) is PROVISIONAL:
// complete source-bound room and individual relationship decisions first.
// Do not publish or call Document prepare with that incomplete stage. Final
// authoring replay and Document admission remain required before publication.
[[nodiscard]] std::optional<PhaseWallCanvasProposal> prepare_phase_wall_canvas_proposal(
    const DocumentSnapshot& source,
    const ConstraintAuthoringIntent& semantic,
    const std::function<std::string(std::string_view original_id)>& allocate_fresh_identity);

// Discover the actual saved-active shared-baseline hosts before ordinary
// opening authoring or Document preview. Every profile must retain an existing
// active opening and its exact active host. No shared-baseline host returns
// nullopt; mixed shared/nonshared targets, several registries and unsupported
// replacement dependencies throw instead of mutating any shared original.
//
// Allocation, actual-source authority and provisional room-review/publication
// lifetime are the same as prepare_phase_wall_canvas_proposal above. Profiles
// replay only onto independently copied openings in the complete physical map.
[[nodiscard]] std::optional<PhaseWallCanvasProposal> prepare_phase_hosted_opening_canvas_proposal(
    const DocumentSnapshot& source,
    const std::vector<HostedOpeningProfileEditIntent>& profiles,
    const std::function<std::string(std::string_view original_id)>& allocate_fresh_identity);

} // namespace sketch
