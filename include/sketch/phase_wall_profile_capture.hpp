#pragma once

#include "sketch/phase_wall_profile_edit.hpp"

namespace sketch {

// Pure current-authoring admission: validates logical wall semantics, scalar
// aliases and every known profile receipt against its actual existing field.
// Dangling/stale known authority throws; unknown opaque receipt paths remain
// untouched. Unreceipted dimensions need no manufactured exact quantity.
// Explicit elevation authority additionally admits signed raw elevation aliases
// and their bound receipts. False retains historical profile admission.
void validate_wall_profile_source_entity(const Entity& source,
    bool strict_elevation = false);

// Captures supported dimensions of this exact existing wall, including the
// complete retained layer inventory, signed raw elevation and top rise. Authored quantities
// retain their exact expressions. Unsupported properties or receipt changes
// throw; equivalent source encodings return nullopt and preserve the source.
// Publish independent typed replay, never the raw candidate.
// Historical physical-room profile proofs opt out of elevation admission:
// elevation and its opaque receipts stay exact and grant no authored authority.
[[nodiscard]] std::optional<WallProfileEditIntent> capture_wall_profile_edit(
    const Entity& original, const Entity& candidate,
    const std::optional<WallProfileEditIntent>& authored = std::nullopt,
    bool strict_elevation = true);

} // namespace sketch
