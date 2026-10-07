#pragma once

#include "sketch/architecture.hpp"
#include "sketch/document_wall.hpp"

#include <optional>
#include <vector>

namespace sketch {

// A validated analytical room footprint carries no implied volume dimensions.
struct DocumentRoomFootprint {
    Boundary boundary;
    std::vector<Boundary> holes;
};

// Presence only: either canonical or legacy height AND elevation must exist.
// Malformed present fields still require rejection by read_document_room.
[[nodiscard]] bool has_document_room_volume_fields(const Entity& entity);
// On failure, output is unchanged. Accepts boundary or legacy segments and
// optional holes, validated analytically without constructing a room solid.
[[nodiscard]] bool read_document_room_footprint(const Entity& entity,
    DocumentRoomFootprint& output, std::string& error);
// Exact straight closed rectangle eligibility; x is width, y is depth.
// Call on a footprint admitted by read_document_room_footprint.
[[nodiscard]] std::optional<Vec2> room_footprint_rectangle_dimensions(
    const DocumentRoomFootprint& footprint);

[[nodiscard]] bool read_document_slab(const Entity& entity, Slab& output, std::string& error);
// Decode an architectural room volume from its analytical boundary, optional
// holes, and explicit height/elevation fields.  The returned semantic value is
// validated by the same solid kernel used by native and projected views.
[[nodiscard]] bool read_document_room(const Entity& entity, RoomVolume& output,
                                      std::string& error);

} // namespace sketch
