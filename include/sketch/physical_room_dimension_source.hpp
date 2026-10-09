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

// Current-value qualification against an actual captured snapshot. Phase
// bookkeeping alone may differ after both captured lineages are admitted and
// the complete physical inventory, context/plane and exact clear geometry
// still match. The retained owner supplies its stable IDs and is not rewritten.
// This is not historical command/review replay authority; those callers retain
// the strict retained-map API above and its exact source-lineage requirement.
[[nodiscard]] PhysicalRoomDimensionSource resolve_current_physical_room_dimension_source(
    const Entity& room, const DocumentSnapshot& snapshot);
} // namespace sketch
