#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace sketch {

struct FootprintVertexEdit {
    // The vertex owns this outgoing segment's start and the preceding end.
    std::size_t vertex_index{};
    Vec2 proposed_position{};
    // Absent selects the outer ring; present selects one authored hole ring.
    std::optional<std::size_t> hole_index;
};

// Stage the actual source payload without document or revision authority.
// Admits analytical geometry and detached native volume when present; an
// exact no-op returns the source unchanged, including numeric representations.
[[nodiscard]] Entity stage_architectural_footprint_vertex_entity(
    const Entity& actual_source, const FootprintVertexEdit& edit);

// Edit only an independent slab or room's captured closed footprint ring.
// Retains signed sweeps, ring order, source field/point representations and
// all unrelated payload. Missing volume measurements remain absent. Returns
// one revision-fenced command after analytical and physical admission; the
// caller retains its complete captured-source fence for publication.
[[nodiscard]] ApplyEntityChanges architectural_footprint_vertex_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    const FootprintVertexEdit& edit, Revision expected_revision);

}  // namespace sketch
