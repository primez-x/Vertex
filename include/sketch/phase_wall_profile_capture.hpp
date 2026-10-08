#pragma once

#include "sketch/phase_wall_profile_edit.hpp"

namespace sketch {

// Pure current-authoring admission: validates logical wall semantics, scalar
// aliases and every known profile receipt against its actual existing field.
// Dangling/stale known authority throws; unknown opaque receipt paths remain
// untouched. Unreceipted dimensions need no manufactured exact quantity.
void validate_wall_profile_source_entity(const Entity& source);

// Captures supported dimensions of this exact existing wall, including the
// complete retained layer inventory and signed top rise. Authored quantities
// retain their exact expressions. Unsupported properties or receipt changes
// throw; equivalent source encodings return nullopt and preserve the source.
// Publish independent typed replay, never the raw candidate.
[[nodiscard]] std::optional<WallProfileEditIntent> capture_wall_profile_edit(
    const Entity& original, const Entity& candidate,
    const std::optional<WallProfileEditIntent>& authored = std::nullopt);

} // namespace sketch
