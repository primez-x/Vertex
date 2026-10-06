#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Reconstruct from source entities only. The returned lower-lane proof has no
// joint marker; replay must independently retain and validate the joint intent.
// No Document, history, assets or snapshot hash participates in this solve.
[[nodiscard]] ApplyBoundaryConstraintChanges reconstruct_joint_translation(
    const std::map<std::string, Entity, std::less<>>& source,
    const JointTranslationIntent& intent);

} // namespace sketch
