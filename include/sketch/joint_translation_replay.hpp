#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct JointTranslationOffsets {
    std::map<std::string, Vec2, std::less<>> owner_offsets;
    std::map<std::string, Vec2, std::less<>> dimension_offsets;
    std::map<std::string, PlanarTransform, std::less<>> owner_transforms;
};

// Validate bounded exact target coverage and captured source owner types.
// Legacy versions resolve every selection to their historical shared offset.
// Versions three/four never fall back to that shared coordinate lane. Version
// four fills owner_transforms; owner_offsets has no rigid geometry authority.
[[nodiscard]] JointTranslationOffsets resolve_joint_translation_offsets(
    const std::map<std::string, Entity, std::less<>>& source,
    const JointTranslationIntent& intent);

// Source-only preparation/completion for nested v4. Ordinary selected boundary
// receipts replay their rigid operator; physical and stroke-derived consumers
// remain in the original map until final source redraw. No raw payload authority.
[[nodiscard]] std::map<std::string, Entity, std::less<>> joint_rigid_replay_source(
    const std::map<std::string, Entity, std::less<>>& source, const JointTranslationIntent& intent);
void complete_joint_rigid_consequences(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool complete_area_callouts = true);
// Source-bound consumers retain IDs and exact current producer lineage. Their
// final coordinates use the unique machine-precision face correspondence to
// the captured rigid target; ordinary boundaries/walls/strokes remain exact.
// False prepares only selected stroke-derived consumers before physical redraw
// validates mixed-producer deductions; final source admission is deferred.
void complete_joint_rigid_sources(const std::map<std::string, Entity, std::less<>>& source,
    std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent,
    bool physical_sources_ready = true);
void validate_joint_rigid_topology(const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& candidate, const JointTranslationIntent& intent);

// Reconstruct from source entities only. The returned lower-lane proof has no
// joint marker; replay must independently retain and validate the joint intent.
// No Document, history, assets or snapshot hash participates in this solve.
[[nodiscard]] ApplyBoundaryConstraintChanges reconstruct_joint_translation(
    const std::map<std::string, Entity, std::less<>>& source,
    const JointTranslationIntent& intent);

} // namespace sketch
