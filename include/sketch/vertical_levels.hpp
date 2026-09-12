#pragma once

#include "sketch/typed_relationships.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace sketch {

enum class VerticalLevelErrorCode {
    invalid_input, limit_exceeded, duplicate_id, dangling_reference,
    duplicate_link, cycle, non_monotonic, missing_level, missing_link,
    invalid_transition, frozen_height_changed
};
class VerticalLevelError final : public std::invalid_argument {
public:
    VerticalLevelError(VerticalLevelErrorCode code, std::string message);
    [[nodiscard]] VerticalLevelErrorCode code() const noexcept { return code_; }
private:
    VerticalLevelErrorCode code_;
};
struct VerticalLevel {
    std::string id;
    double elevation_m{};
    bool operator==(const VerticalLevel&) const = default;
};
struct FloorToFloorLink {
    std::string id;
    std::string lower_level_id;
    std::string upper_level_id;
    RelationshipState state{RelationshipState::connected};
    // Required for frozen/disconnected links, absent for connected links.
    std::optional<double> retained_height_m;
    bool operator==(const FloorToFloorLink&) const = default;
};

// Immutable validated value snapshots. Elevations and heights use metres.
class VerticalLevelGraph {
public:
    static constexpr std::size_t maximum_levels = 4096;
    static constexpr std::size_t maximum_links = 8192;
    static constexpr double height_tolerance_m = 1e-9;
    explicit VerticalLevelGraph(std::vector<VerticalLevel> levels = {},
                                std::vector<FloorToFloorLink> links = {});
    [[nodiscard]] static VerticalLevelGraph from_json(const nlohmann::json& value);
    [[nodiscard]] const std::vector<VerticalLevel>& levels() const noexcept { return levels_; }
    [[nodiscard]] const std::vector<FloorToFloorLink>& links() const noexcept { return links_; }
    [[nodiscard]] VerticalLevelGraph create_level(VerticalLevel level) const;
    [[nodiscard]] VerticalLevelGraph create_link(FloorToFloorLink link) const;
    [[nodiscard]] VerticalLevelGraph with_elevation(std::string_view level_id, double elevation_m) const;
    [[nodiscard]] VerticalLevelGraph freeze(std::string_view link_id) const;
    [[nodiscard]] VerticalLevelGraph disconnect(std::string_view link_id) const;
    [[nodiscard]] double floor_to_floor_height(std::string_view link_id) const;
    [[nodiscard]] std::string serialize() const;
private:
    std::vector<VerticalLevel> levels_;
    std::vector<FloorToFloorLink> links_;
    [[nodiscard]] VerticalLevelGraph transition(std::string_view id, RelationshipState state) const;
};
} // namespace sketch
