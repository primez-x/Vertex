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
[[nodiscard]] bool read_document_wall_id(const Entity& entity, std::string& wall_id, std::string& error);

} // namespace sketch
