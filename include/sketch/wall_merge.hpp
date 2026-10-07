#pragma once
#include "sketch/document.hpp"

namespace sketch {
[[nodiscard]] Command make_wall_merge_command(const DocumentSnapshot& source, const WallMergeIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replayed_wall_merge_entities(
    const std::map<std::string, Entity, std::less<>>& source, const WallMergeIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> wall_merge_validation_source(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& reconstructed, const WallMergeIntent& intent);
[[nodiscard]] nlohmann::json encode_wall_merge(const WallMergeIntent& intent);
[[nodiscard]] WallMergeIntent decode_wall_merge(const nlohmann::json& value);
// Strict v1 full-source archive. Historical identities are receipts, not live references.
void validate_wall_merge_archive(const Entity& wall);
}
