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

// Current-value qualification against the actual captured authoritative entity
// map. New versioned current-command authoring/replay callers must supply their
// actual captured command source map, never a fabricated or restored snapshot.
// Historical/default command and review replay retain the strict API above.
// Phase bookkeeping alone may differ after both captured lineages are admitted and
// the complete physical inventory, context/plane and exact clear geometry
// still match. The retained owner supplies its stable IDs and is not rewritten.
[[nodiscard]] PhysicalRoomDimensionSource resolve_current_physical_room_dimension_source(
    const Entity& room, const std::map<std::string, Entity, std::less<>>& entities);

// Snapshot callers supply their actual captured snapshot under the same
// current-value qualification and caller obligations as the overload above.
[[nodiscard]] PhysicalRoomDimensionSource resolve_current_physical_room_dimension_source(
    const Entity& room, const DocumentSnapshot& snapshot);
} // namespace sketch
