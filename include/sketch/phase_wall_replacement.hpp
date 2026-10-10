#pragma once

#include "sketch/document.hpp"

#include <map>
#include <string>
#include <vector>

namespace sketch {

using PhaseWallReplacementEntities = std::map<std::string, Entity, std::less<>>;
using PhaseWallReplacementIdentityMap = std::map<std::string, std::string, std::less<>>;

struct PhaseWallReplacementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhaseWallReplacementDiagnostic&) const = default;
};

// Derived inventory only. This is not authority for supplied entity payloads.
// Sorted identities include hosted objects, complete canonical measured-stroke
// cohorts, qualified analytical consumers, retained proof children and active
// typed relations. Copied model owners require actual target baseline registry
// membership. Physical room owners are retained for a subsequent
// reviewed source-derived room completion, never cloned as current room facts.
struct PhaseWallReplacementPlan {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_wall_ids;
    std::vector<std::string> required_entity_ids;
    std::vector<std::string> required_child_ids;
    std::vector<std::string> affected_original_room_ids;
    // Copied active relations still bind preserved room owners. The enclosing
    // room review must explicitly retain or remap these endpoints; superseding
    // a baseline room otherwise suspends the relation in the proposed design.
    std::vector<std::string> room_constraint_ids_requiring_review;
    std::vector<PhaseWallReplacementDiagnostic> diagnostics;
    // Opt-in additive typed presentation completion; legacy plans retain their
    // historical refusal of affected saved views and annotation overrides.
    bool complete_presentations{false};
    // Additive owner/two-cut completion. Historical plans continue to refuse
    // affected corner aggregates unless their command explicitly opts in.
    bool complete_corner_windows{false};

    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhaseWallReplacementPlan&) const = default;
};

struct PhaseWallReplacementResult {
    PhaseWallReplacementEntities entities;
    // Includes entity and typed owned-child identities, so enclosing semantic
    // edit intent can be remapped without inspecting copied opaque payloads.
    PhaseWallReplacementIdentityMap original_to_proposed;
    std::vector<std::string> fresh_identity_ids;
    std::vector<std::string> affected_original_room_ids;
    std::vector<std::string> room_constraint_ids_requiring_review;
    // Typed copies independently reconstructed from source, withheld from the
    // intermediate solve until the enclosing room review supplies individual
    // retained/remapped/omitted relation dispositions. Fresh IDs stay reserved.
    PhaseWallReplacementEntities deferred_room_constraints;
};

// Requires the actual complete map and the registry's actual saved active
// alternative. Invalid envelopes/selectors throw; unsupported affected
// dependencies produce blocking diagnostics. No source is modified.
[[nodiscard]] PhaseWallReplacementPlan inspect_phase_wall_replacement_plan(
    const PhaseWallReplacementEntities& source,
    const std::vector<std::string>& seed_wall_ids,
    const std::string& registry_id,
    const std::string& alternative_id,
    bool complete_presentations = false,
    bool complete_corner_windows = false);

// Reinspects source and requires exactly the derived plan and a complete,
// injective fresh mapping. Never generates identities or accepts clone JSON.
// Originals are byte-for-byte retained except the selected registry's model
// field and, when opted in, additive typed presentation rows on retained owners.
// Retained-history reservations and the enclosing source snapshot fence
// remain the Document's responsibility. Result is an intermediate physical
// source: affected rooms still require reviewed room completion before commit.
[[nodiscard]] PhaseWallReplacementResult replay_phase_wall_replacement(
    const PhaseWallReplacementEntities& source,
    const PhaseWallReplacementPlan& plan,
    const PhaseWallReplacementIdentityMap& identities);

} // namespace sketch
