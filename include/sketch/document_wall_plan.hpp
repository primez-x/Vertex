#pragma once

#include "sketch/document.hpp"
#include "sketch/hosted_opening_geometry.hpp"

#include <map>

namespace sketch {

// Derived plan geometry; never changes source baselines, quantities, or joins.
// Only unambiguous pairs of straight endpoints in the same placement context
// form mitered plan corners. Straight T/X and multi-way contacts remove internal
// material strokes while preserving complete per-wall footprints. Curved
// junction construction retains its original capped geometry.
// Malformed walls are omitted so the caller can report its existing semantic
// diagnostics. Overrides allow a validated opening-width candidate to retain
// its surrounding corner geometry without changing the source snapshot.
[[nodiscard]] std::map<std::string, WallPlanGeometry, std::less<>>
document_wall_plan_geometry(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::map<std::string, Wall, std::less<>>& overrides = {});

} // namespace sketch
