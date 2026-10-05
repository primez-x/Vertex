#pragma once

#include "sketch/geometry.hpp"

namespace sketch {

// Orders an unordered set of existing analytical segments into one closed
// boundary without changing their coordinates or curve sweeps. Every endpoint
// must join exactly, every vertex must have degree two, and all supplied
// segments must belong to the same simple cycle; disconnected, branched, open,
// or duplicate topology is rejected.
[[nodiscard]] Boundary assemble_boundary_from_segments(
    const std::vector<Segment>& segments, std::size_t seed_index = 0);

// Enumerates simple bounded faces in an endpoint-connected analytical segment
// graph. Segments that do not border a bounded face may be present (for
// example, unfinished wall stubs) and are ignored. Returned boundaries use
// counter-clockwise winding, retain each segment's analytical curve, and are
// ordered deterministically by their first source edge and area.
[[nodiscard]] std::vector<Boundary> detect_closed_boundaries(
    const std::vector<Segment>& segments);

} // namespace sketch
