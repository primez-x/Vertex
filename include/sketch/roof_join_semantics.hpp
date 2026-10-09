#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// A roof join is a document relationship between source roof entities.  The
// fused style keeps each source roof's semantic identity and openings while
// allowing coordinated views to present one derived union. V1 retains the
// original closed three-field record. V2 optionally binds one material override
// using the established closed version-1 assignment record. V3 requires that
// same assignment and exactly one roof, preserving an explicit material scope.
enum class RoofJoinStyle { fused };

struct RoofJoinMaterialAssignment {
    std::string catalog_id;
    std::string material_id;
    bool operator==(const RoofJoinMaterialAssignment&) const = default;
};

struct RoofJoin {
    std::string id;
    std::vector<std::string> roof_ids;
    RoofJoinStyle style{RoofJoinStyle::fused};
    // Authored roof_ids order determines overlap ownership: earlier roofs own
    // shared volume. V2 assignment intentionally overrides all region bindings.
    std::optional<RoofJoinMaterialAssignment> material_assignment;
    // V3 is an explicit material relationship to exactly one source roof.
    bool singleton_material_scope{};

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
