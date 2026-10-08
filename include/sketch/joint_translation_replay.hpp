#pragma once

#include "sketch/document.hpp"

namespace sketch {

struct JointTranslationOffsets {
    std::map<std::string, Vec2, std::less<>> owner_offsets;
    std::map<std::string, Vec2, std::less<>> dimension_offsets;
};

// Validate bounded exact target coverage and captured source owner types.
// Legacy versions resolve every selection to their historical shared offset.
// Version three never falls back to that shared coordinate lane.
[[nodiscard]] JointTranslationOffsets resolve_joint_translation_offsets(
    const std::map<std::string, Entity, std::less<>>& source,
    const JointTranslationIntent& intent);

// Reconstruct from source entities only. The returned lower-lane proof has no
// joint marker; replay must independently retain and validate the joint intent.
// No Document, history, assets or snapshot hash participates in this solve.
[[nodiscard]] ApplyBoundaryConstraintChanges reconstruct_joint_translation(
    const std::map<std::string, Entity, std::less<>>& source,
    const JointTranslationIntent& intent);

} // namespace sketch
