#pragma once
#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sketch {
struct PhysicalWallPhaseSelection;
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
// Exact digest of the retained descriptor, including stale source evidence.
[[nodiscard]] std::string physical_wall_room_descriptor_digest(const Entity& entity);
// Document's source adapter independently rederives the reviewed destination
// from the preceding state. No caller-supplied geometry or validator authority.
[[nodiscard]] PhysicalWallRoomDescriptor validate_physical_wall_room_repair(
    const std::map<std::string,Entity,std::less<>>& source, const BoundaryGeometryEdit& edit,
    const std::set<std::string>& reviewed_owners = {});
// The same source proof for a staged explicit-phase room review. Evaluated
// inactive owners do not reserve destinations; active owners still do.
[[nodiscard]] PhysicalWallRoomDescriptor validate_physical_wall_room_repair(
    const std::map<std::string,Entity,std::less<>>& source, const BoundaryGeometryEdit& edit,
    const std::set<std::string>& reviewed_owners, const PhysicalWallPhaseSelection& selection);
} // namespace sketch
