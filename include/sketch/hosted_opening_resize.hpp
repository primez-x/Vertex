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

// Slide one real opening along its unchanged host baseline. offset_metres is
// the exact distance from the directed baseline start to the start jamb,
// including arc length on curved hosts. Width, height, sill, host, manufactured
// assembly, identity and unrelated metadata remain unchanged. Updates offset_m
// and an existing offset alias; invalidates only changed quantity receipts.
// Rejects non-finite/negative stations, cuts beyond the host, sibling overlaps
// and invalid complete host/assembly geometry without clamping or modifying
// source. Even an exact unchanged station validates source editability and
// complete native geometry, then returns an empty command. Admission uses a
// detached Document preview; the command is fenced to source.revision() and
// applying it through Document is atomic and undoable.
[[nodiscard]] ApplyEntityChanges hosted_opening_offset_command(
    const DocumentSnapshot& source, const std::string& opening_id,
    double offset_metres);

} // namespace sketch
