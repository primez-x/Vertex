#pragma once
#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace sketch {
struct PhysicalWallRoomDescriptor {
    std::string selected_wall_id;
    nlohmann::json source_lineage;
    std::vector<Boundary> holes;
};
[[nodiscard]] bool is_physical_wall_room(const Entity& entity) noexcept;
// An absent marker returns no reason. Unknown positive descriptor versions
// return a read-only reason; malformed understood descriptors throw.
[[nodiscard]] std::optional<std::string> validate_physical_wall_room_descriptor(const Entity& entity);
[[nodiscard]] PhysicalWallRoomDescriptor decode_physical_wall_room_descriptor(const Entity& entity);
[[nodiscard]] nlohmann::json encode_physical_wall_room_descriptor(const PhysicalWallRoomDescriptor& descriptor);
} // namespace sketch
