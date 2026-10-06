#pragma once

#include "sketch/boundary_entity.hpp"

#include <map>
#include <vector>

namespace sketch {
struct PhysicalRoomDimensionSource {
    IdentifiedBoundary boundary;
    std::vector<Boundary> holes;
    double area_square_metres{};
};

// Pure retained-map qualification. Stable IDs come from the retained owner only
// after its complete physical evidence and exact clear geometry match one fresh
// component. No document restoration, preview, repair or mutation is performed.
[[nodiscard]] PhysicalRoomDimensionSource resolve_physical_room_dimension_source(
    const Entity& room, const std::map<std::string, Entity, std::less<>>& entities);
} // namespace sketch
