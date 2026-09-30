#pragma once

#include "sketch/architecture.hpp"
#include "sketch/document_wall.hpp"

namespace sketch {

[[nodiscard]] bool read_document_slab(const Entity& entity, Slab& output, std::string& error);
// Decode an architectural room volume from its analytical boundary, optional
// holes, and explicit height/elevation fields.  The returned semantic value is
// validated by the same solid kernel used by native and projected views.
[[nodiscard]] bool read_document_room(const Entity& entity, RoomVolume& output,
                                      std::string& error);

} // namespace sketch
