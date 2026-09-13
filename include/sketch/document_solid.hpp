#pragma once

#include "sketch/architecture.hpp"
#include "sketch/document.hpp"

namespace sketch {

// Shared document decoding for native rendering and quantity projection.
// Hosted openings are supplied from the complete source snapshot, independently
// of display visibility. Solid builders validate the decoded geometry.
[[nodiscard]] bool read_document_wall(const Entity& entity,
    const std::vector<const Entity*>& openings, Wall& output, std::string& error);
[[nodiscard]] bool read_document_slab(const Entity& entity, Slab& output, std::string& error);
// Decode an architectural room volume from its analytical boundary, optional
// holes, and explicit height/elevation fields.  The returned semantic value is
// validated by the same solid kernel used by native and projected views.
[[nodiscard]] bool read_document_room(const Entity& entity, RoomVolume& output,
                                      std::string& error);
[[nodiscard]] bool read_document_wall_id(const Entity& entity, std::string& wall_id, std::string& error);

} // namespace sketch
