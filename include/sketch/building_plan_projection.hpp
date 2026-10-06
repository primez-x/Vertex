#pragma once

#include "sketch/building_entity.hpp"
#include "sketch/geometry.hpp"

namespace sketch {

// Derive the visible, top-down plan edges for one canonical building object.
// The returned segments are in the document's world XY coordinates.  A
// nonzero sweep denotes an exact circular arc; zero sweep denotes a line.
// This is presentation geometry only.  It must not be used as an authoring or
// calculation boundary.
//
// HLR hidden edges are intentionally omitted.  Invalid object geometry,
// unavailable projection data, and projected curve types other than lines or
// circles are reported as std::invalid_argument.
[[nodiscard]] Boundary project_building_plan(const BuildingObject& object);

// `object` is already in its effective world placement (ordinary objects
// resolve_vertical_placement once before decoding; hosted railings decode
// directly). `entities` must be the original authored source map. The shape
// builder resolves a hosted stair from that source exactly once. Never pass
// a map containing previously level-resolved copies.
[[nodiscard]] Boundary project_building_plan(
    const BuildingObject& object,
    const std::map<std::string, Entity, std::less<>>& entities);

}  // namespace sketch
