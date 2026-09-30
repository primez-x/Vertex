#pragma once

#include "sketch/document.hpp"

namespace sketch {

// Jambs follow increasing distance along the current host baseline. The frame
// is derived from authoritative wall/opening parameters in model metres.
// Curved hosts use exact arc stations and the tangent at the opening midpoint.
struct HostedOpeningResizeFrame {
    Vec2 start_jamb;
    Vec2 end_jamb;
    double host_thickness_metres{};
    double angle_radians{};
    double width_metres{};
    double height_metres{};
    Segment host_baseline;
    double offset_metres{};
};

[[nodiscard]] HostedOpeningResizeFrame hosted_opening_resize_frame(
    const DocumentSnapshot& source, const std::string& opening_id);

// Resize only the wall cut's width. keep_start_jamb=true pins the start and
// moves the end handle; false pins the end and moves the start handle. Height,
// sill, host, manufactured assembly dimensions and unrelated metadata remain
// authoritative and unchanged. Admission regenerates the complete host with
// every sibling opening and assembly, then validates a detached Document
// preview. The returned command is fenced to source.revision(); applying it
// through Document is atomic and undoable. Throws without modifying source.
[[nodiscard]] ApplyEntityChanges hosted_opening_width_resize_command(
    const DocumentSnapshot& source, const std::string& opening_id,
    double relative_width_scale, bool keep_start_jamb);

} // namespace sketch
