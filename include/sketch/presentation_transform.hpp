#pragma once

#include "sketch/document.hpp"

namespace sketch {

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
