#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// A roof join is a document relationship between source roof entities.  The
// v1 fused style keeps each source roof's semantic identity and openings while
// allowing coordinated views to present one derived union.
enum class RoofJoinStyle { fused };

struct RoofJoin {
    std::string id;
    std::vector<std::string> roof_ids;
    RoofJoinStyle style{RoofJoinStyle::fused};

    bool operator==(const RoofJoin&) const = default;
};

void validate_roof_join_semantics(const RoofJoin& join);

[[nodiscard]] std::string_view roof_join_style_name(RoofJoinStyle style) noexcept;
[[nodiscard]] std::optional<RoofJoinStyle>
parse_roof_join_style(std::string_view value) noexcept;
[[nodiscard]] RoofJoin parse_roof_join(const nlohmann::json& value,
                                       std::string_view id);
[[nodiscard]] nlohmann::json roof_join_json(const RoofJoin& join);

}  // namespace sketch
