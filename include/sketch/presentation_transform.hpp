#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace sketch {

// Child identity is scoped by its annotation owner, never by a joined string.
struct PresentationAnnotationTarget {
    std::string owner_id;
    std::string child_id;
};

inline constexpr std::size_t maximum_presentation_group_targets = 1000;

// Atomic rigid edit of selected label/symbol children and reference owners.
// The caller must qualify one compatible stored position AND orientation frame
// for every target and the shared pivot/offset. Plan/Site projection and frame
// handedness belong to the adapter; model_plan and owner frames are preserved.
// Local symbol/reference X flips are retained; a reflection toggles local Y.
// Text follows the transformed anchor/baseline without persisting mirrored
// glyphs. Dimensions, styles, asset bytes and unselected placements are retained.
// A legacy symbol reflection materializes the required axis-format defaults
// for its owner without changing the unselected symbols' effective artwork.
// All targets are validated even for canonical identity intent, which returns
// no changes or legacy upgrades. Refusal never returns a partial command.
[[nodiscard]] ApplyEntityChanges presentation_group_transform_command(
    const DocumentSnapshot& source,
    std::span<const PresentationAnnotationTarget> annotations,
    std::span<const std::string> reference_ids,
    const PlanarTransform& transform, Revision expected_revision);

// Detached presentation edits. The caller supplies source-plan anchors and
// rotation with the source frame's handedness already applied.
[[nodiscard]] ApplyEntityChanges annotation_transform_command(
    const DocumentSnapshot& source, std::string_view owner_id,
    std::string_view child_id, double relative_scale, double rotation_radians,
    Revision expected_revision);
[[nodiscard]] ApplyEntityChanges annotation_axis_resize_command(
    const DocumentSnapshot& source, std::string_view owner_id,
    std::string_view child_id, double scale_x, double scale_y,
    Vec2 source_anchor, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges reference_transform_command(
    const DocumentSnapshot& source, std::string_view reference_id,
    double relative_scale, double rotation_radians, Revision expected_revision);

}  // namespace sketch
