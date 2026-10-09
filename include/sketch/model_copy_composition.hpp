#pragma once

#include "sketch/document.hpp"

#include <map>
#include <string>
#include <vector>

namespace sketch {

using ModelCopyEntities = std::map<std::string, Entity, std::less<>>;

// Compose 1..4 complete, independently admitted additive copy candidates,
// each derived from this same actual source. Existing model owners, catalogs,
// phase registries and opaque envelopes remain exact. Only existing codec-known
// view presentation arrays and object annotation overrides may append rows.
// Candidate order determines suffix order; identities are never regenerated.
// This is not a Document command or admission substitute. The caller owns the
// captured-source binding, history/assets/fresh allocation, final Document
// admission, and any subsequent scope enrollment.
// Throws std::invalid_argument for incompatible or over-budget candidates.
[[nodiscard]] ModelCopyEntities compose_independent_model_copy_candidates(
    const ModelCopyEntities& actual, const std::vector<ModelCopyEntities>& candidates);

} // namespace sketch
