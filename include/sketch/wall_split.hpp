#pragma once
#include "sketch/document.hpp"

namespace sketch {
[[nodiscard]] Command make_wall_split_command(const DocumentSnapshot& source, const WallSplitIntent& intent);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replayed_wall_split_entities(
    const std::map<std::string, Entity, std::less<>>& source, const WallSplitIntent& intent);
[[nodiscard]] nlohmann::json encode_wall_split(const WallSplitIntent& intent);
[[nodiscard]] WallSplitIntent decode_wall_split(const nlohmann::json& value);
// Detached normalization is allowed only after exact complete reconstruction.
[[nodiscard]] std::map<std::string, Entity, std::less<>> wall_split_validation_source(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::map<std::string, Entity, std::less<>>& reconstructed, const WallSplitIntent& intent);
void validate_wall_split_archive(const Entity& wall);
}
