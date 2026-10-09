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

// Stage a canonical v1 rectangular/circular column or straight beam directly
// from its actual source, without Document/transaction admission. Retains raw
// metadata, Z, height, section frame, material, context and binding; invalidates
// only quantity receipts whose physical values changed. Native geometry and
// footprint extents are admitted; exact unit factors return validated source.
[[nodiscard]] Entity stage_structural_plan_axis_resize_entity(
    const Entity& actual_source, double scale_x, double scale_y, Vec2 anchor,
    double frame_rotation_radians = 0.0);

// Stage an actual sloped-panel, gable or hip roof's native plan footprint
// resize without Document/transaction admission. Retains source receipts and
// opaque metadata for the caller's mathematical derivation archive; creates
// no quantity receipts. Uses the supplied factors/frame and opposite-edge
// anchor, retaining Z, rise, overhang, thickness, context and material. Source
// and result native geometry are admitted; exact unit factors return source.
// Sloped panels retain the source run's continuous footprint branch; a fold
// ambiguity or an unreachable width on that branch refuses without a jump.
[[nodiscard]] Entity stage_roof_plan_axis_resize_entity(
    const Entity& actual_source, double scale_x, double scale_y, Vec2 anchor,
    double frame_rotation_radians);

struct RoofPlanCornerResizeParameters {
    double scale_x{1.0}, scale_y{1.0};
    Vec2 anchor;
    double frame_rotation_radians{};
};

// Derive the actual gesture against the original native footprint. This
// carries no candidate, document or transaction authority. The opposite
// corner stays anchored; exact return to the captured grip yields unit factors.
[[nodiscard]] RoofPlanCornerResizeParameters roof_plan_corner_resize_parameters(
    const Entity& actual_source, std::size_t corner_index, Vec2 proposed_position);

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
