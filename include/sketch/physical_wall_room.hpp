#pragma once
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/boundary_entity.hpp"

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

struct PhysicalWallRoomRepairReferences {
    nlohmann::json child_mapping=nlohmann::json::object();
    std::vector<std::string> removed_reference_ids;
    std::vector<std::string> replacement_dimension_ids;
    bool allow_automatic_angle_removal{};
};
// Fresh child identities and explicit reference decisions belong to the
// reviewed reassignment. Preparation never chooses geometric correspondence.
[[nodiscard]] EditBoundaryGeometry prepare_physical_wall_room_repair(
    const DocumentSnapshot& source,std::string_view room_id,std::string_view selected_wall_id,
    Vec2 interior_witness,const nlohmann::json& reviewed_source_lineage,
    std::string expected_descriptor_digest,const LegacyBoundaryIdentityOptions& fresh_ids,
    const PhysicalWallRoomRepairReferences& references);
} // namespace sketch
