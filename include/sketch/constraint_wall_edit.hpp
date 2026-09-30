#pragma once
#include "sketch/document.hpp"

namespace sketch {
// Reconstructs only the straight baseline and its exact entry receipt. All
// unrelated wall and receipt metadata remains owned by the original entity.
[[nodiscard]] Entity replay_constraint_wall_edit(
    const Entity& source, const ConstraintWallGeometryEdit& edit);
[[nodiscard]] nlohmann::json encode_constraint_wall_edit(const ConstraintWallGeometryEdit& edit);
[[nodiscard]] ConstraintWallGeometryEdit decode_constraint_wall_edit(const nlohmann::json& value);
void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline);
void validate_constraint_wall_host(const std::string& wall_id,
    const std::map<std::string, Entity, std::less<>>& entities);
}
