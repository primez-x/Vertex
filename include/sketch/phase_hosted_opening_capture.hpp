#pragma once

#include "sketch/phase_hosted_opening_edit.hpp"

namespace sketch {

// Captures only a supported edit of this exact existing opening. Authored input
// is a single-field hint; its exact entered quantity is retained. Unsupported
// changes throw, and an exact unchanged record returns nullopt without copying.
// Supported source-equivalent scalar/profile encodings also return nullopt,
// retaining source representation; an absent door operation is never inferred.
// The caller publishes independent typed replay, never the raw candidate.
[[nodiscard]] std::optional<HostedOpeningProfileEditIntent> capture_hosted_opening_profile_edit(
    const Entity& original, const Entity& candidate,
    std::optional<HostedOpeningProfileEditIntent> authored = std::nullopt);

} // namespace sketch
