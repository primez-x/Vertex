#pragma once

#include "sketch/phase_hosted_opening_edit.hpp"

#include <string_view>

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

// Returns the exact captured quantity for one existing dimension after
// independently replaying a same-field edit against the unchanged source.
// Accepts only offset_m/offset, width_m/width, sill_m/sill, and height_m/height.
[[nodiscard]] Quantity capture_hosted_opening_dimension_quantity(
    const Entity& original, std::string_view scalar_field);

} // namespace sketch
