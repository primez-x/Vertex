#pragma once

#include "sketch/document.hpp"

#include <cstddef>

namespace sketch {

// Natural plan axes for physical dimensions: baseline/run/ridge direction is
// X, its left-hand normal is Y. Rooms/slabs follow their first boundary edge.
// Reads semantic orientation fields without rebuilding a solid. Throws on
// unsupported entity forms or malformed orientation fields.
[[nodiscard]] double plan_axis_resize_frame(const Entity& entity);

// Bounds of the generated solid projected into its natural plan frame, with
// coordinates relative to the world origin. Includes roof overhang, stair
// landings and railing end posts; does not include unrelated display symbols.
[[nodiscard]] Bounds2 plan_axis_resize_bounds(const Entity& entity);

// Resize the generated full plan footprint by the requested factors, about a
// fixed world-space footprint edge/corner anchor in the supplied rotated XY
// frame. Z, height, rise, level connections and material
// identity are retained. The command includes hosted wall openings atomically,
// removes quantity receipts only for changed values, and is fenced to source.
// Circular geometry requires equal factors. Semantic rectangles cannot shear;
// anisotropic frames must align with their natural axes (either axis order).
// Roof core dimensions are solved while overhang/rise/thickness are retained
// and pitch is recalculated. Railing Y edits resize its square post/rail sections
// uniformly, including rail vertical thickness; overall height is retained.
// Its path length compensates for end-post extents to match the requested full
// footprint. Shrinks smaller than retained sections/overhang fail explicitly.
// Unrepresentable edits throw, without modifying the source.
// Admission includes generated solids and a normal Document preview.
[[nodiscard]] ApplyEntityChanges plan_axis_resize_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    double scale_x, double scale_y, Vec2 anchor,
    double frame_rotation_radians = 0.0);

// Resize both plan dimensions of an authored sloped_roof_panel, gable_roof or
// hip_roof by moving one generated-solid footprint corner in world XY metres.
// Corners use plan_axis_resize_bounds in the original natural-axis frame:
// 0=min X/min Y, 1=max X/min Y, 2=max X/max Y, 3=min X/max Y. The opposite
// corner anchors the edit; crossing either of its axes is rejected. Roofs
// retain their parametric family, orientation, overhang, rise and thickness;
// this does not introduce free polygon or skew roof authoring.
// Requires an editable source at expected_revision. The existing axis-resize
// path admits the detached candidate, including native solids and roof joins.
// An exact original world corner returns an empty revision-fenced command.
// Publication must retain the caller's complete captured-source fence.
[[nodiscard]] ApplyEntityChanges roof_plan_corner_resize_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    std::size_t corner_index, Vec2 proposed_position, Revision expected_revision);

} // namespace sketch
