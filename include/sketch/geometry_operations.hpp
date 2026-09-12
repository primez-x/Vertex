#pragma once

#include "sketch/boundary_entity.hpp"
#include <string_view>

namespace sketch {

struct ShortcutBinding { std::string shortcut; std::string command_id; };
// Logical Apex operation names; physical key compatibility is a UI responsibility.
[[nodiscard]] std::vector<ShortcutBinding> apex_operation_preset();
[[nodiscard]] std::string_view apex_command_id(std::string_view operation_name);
// Returns duplicate shortcuts (including identical repeated bindings), sorted.
[[nodiscard]] std::vector<std::string> shortcut_conflicts(const std::vector<ShortcutBinding>& bindings);

enum class BoundaryFlipAxis { horizontal, vertical };
[[nodiscard]] IdentifiedBoundary rotate_boundary(const IdentifiedBoundary&, Vec2 pivot, double radians);
// horizontal reflects y about pivot.y; vertical reflects x about pivot.x.
[[nodiscard]] IdentifiedBoundary flip_boundary(const IdentifiedBoundary&, Vec2 pivot, BoundaryFlipAxis);
// Fraction is strictly inside (0,1), measured along the selected line or arc.
// Retains original segment ID for the first piece; caller supplies the second.
[[nodiscard]] IdentifiedBoundary insert_boundary_vertex(const IdentifiedBoundary&, std::string_view segment_id,
    double fraction, std::string vertex_id, std::string second_segment_id);
[[nodiscard]] IdentifiedBoundary clone_boundary(const IdentifiedBoundary&, std::string new_id,
    const LegacyBoundaryIdentityOptions& new_ids, Vec2 translation);

[[nodiscard]] Vec2 jump_to_boundary_vertex(const IdentifiedBoundary&, std::string_view vertex_id);
// Helpers operate on geometry only. Closure never snaps or repairs existing points.
[[nodiscard]] Boundary automatically_close_boundary(const Boundary& open_chain);
// Completes start -> shoulder1 -> shoulder2 -> end with three straight segments.
[[nodiscard]] Boundary complete_bay_window(Vec2 start, Vec2 shoulder1, Vec2 shoulder2, Vec2 end);
// Orders an unordered set of existing analytical segments into one closed
// boundary without changing their coordinates or curve sweeps. Every endpoint
// must join exactly, every vertex must have degree two, and all supplied
// segments must belong to the same simple cycle; disconnected, branched, open,
// or duplicate topology is rejected.
[[nodiscard]] Boundary assemble_boundary_from_segments(
    const std::vector<Segment>& segments, std::size_t seed_index = 0);

} // namespace sketch
