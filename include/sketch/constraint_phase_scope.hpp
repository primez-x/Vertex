#pragma once

#include "sketch/constraint_entity.hpp"
#include "sketch/physical_wall_phase.hpp"
#include "sketch/assembly_phase_membership.hpp"

#include <set>

namespace sketch {

enum class ConstraintPhasePolicy { legacy_all, saved_active };

struct ConstraintPhaseScope {
    // Actual saved choices; no supplied visibility list establishes authority.
    std::vector<PhysicalWallPhaseState> registries;
    std::set<std::string,std::less<>> inactive_owner_ids;
    // Embedded roots have qualified source identities, not document owner IDs.
    // Presentation aliases never establish saved alternative membership.
    AssemblyPhaseMembershipScope assembly_memberships;
};

[[nodiscard]] ConstraintPhaseScope constraint_phase_scope(
    const std::map<std::string,Entity,std::less<>>& entities);
[[nodiscard]] bool constraint_participates(
    const PersistentConstraint& constraint,const ConstraintPhaseScope& scope);

// Actual opening hosts inherit their actual wall's activity, even when the
// opening itself is unregistered. Corner cuts also inherit the real corner
// owner's two cuts/walls; visited identities prevent the backlink cycle.
// Visibility and render aliases are not proof.
// Malformed/unresolved parents retain the caller's existing diagnostic path.
[[nodiscard]] bool assembly_host_inactive_in_phase(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::set<std::string, std::less<>>& inactive_owner_ids,
    const std::string& host_id);

} // namespace sketch
