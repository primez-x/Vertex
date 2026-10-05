#pragma once
#include "sketch/physical_wall_room_data.hpp"

#include <map>
#include <string_view>

namespace sketch {
struct PhysicalWallRoomCheck {
    bool current{};
    std::string diagnostic;
    Boundary boundary;
    std::vector<Boundary> holes;
    double area_square_metres{};
};
// Current values are rederived from physical walls, never claimed persisted
// areas. Malformed/future/stale owners receive diagnostics and empty geometry.
// Detection is cached within this call; no caller-owned document is modified.
[[nodiscard]] std::map<std::string,PhysicalWallRoomCheck,std::less<>>
physical_wall_room_checks(const DocumentSnapshot& source);
// Captured-snapshot preparation. Indices belong only to freshly detected
// clear components; repeated definitions reuse current owners without changing
// their classification. Inline holes do not create separate deduction tools.
[[nodiscard]] ApplyEntityChanges prepare_physical_wall_rooms(const DocumentSnapshot& source,
    std::string_view selected_wall_id,const std::vector<std::size_t>& indices,std::string classification);
} // namespace sketch
