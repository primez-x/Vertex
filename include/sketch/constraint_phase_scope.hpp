#pragma once

#include "sketch/constraint_entity.hpp"
#include "sketch/physical_wall_phase.hpp"

#include <set>

namespace sketch {

enum class ConstraintPhasePolicy { legacy_all, saved_active };

struct ConstraintPhaseScope {
    // Actual saved choices; no supplied visibility list establishes authority.
    std::vector<PhysicalWallPhaseState> registries;
    std::set<std::string,std::less<>> inactive_owner_ids;
};

[[nodiscard]] ConstraintPhaseScope constraint_phase_scope(
    const std::map<std::string,Entity,std::less<>>& entities);
[[nodiscard]] bool constraint_participates(
    const PersistentConstraint& constraint,const ConstraintPhaseScope& scope);

} // namespace sketch
