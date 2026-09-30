#pragma once

#include "sketch/document.hpp"
#include "sketch/wall_semantics.hpp"

namespace sketch {

// Shared document decoding for native rendering and quantity projection.
// Hosted openings are supplied from the complete source snapshot, independently
// of display visibility. Solid builders validate the decoded geometry.
[[nodiscard]] bool read_document_wall(const Entity& entity,
    const std::vector<const Entity*>& openings, Wall& output, std::string& error);
[[nodiscard]] bool read_document_wall_id(const Entity& entity, std::string& wall_id,
                                         std::string& error);

} // namespace sketch
