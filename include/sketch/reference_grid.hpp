#pragma once

#include "sketch/geometry.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sketch {

enum class ReferenceGridAxis { x, y };

struct ReferenceGridLine {
    Vec2 start{};
    Vec2 end{};
    ReferenceGridAxis axis{ReferenceGridAxis::x};
    std::int32_t index{};
    bool major{};
    bool operator==(const ReferenceGridLine& other) const noexcept {
        return start.x == other.start.x && start.y == other.start.y &&
               end.x == other.end.x && end.y == other.end.y && axis == other.axis &&
               index == other.index && major == other.major;
    }
};

// A finite orthogonal grid in model metres. The grid is presentation data,
// but its origin, rotation, spacing, and extents are authoritative project
// properties and are therefore validated and persisted like other models.
struct ReferenceGridModel {
    static constexpr std::int32_t maximum_lines_from_origin = 512;
    static constexpr std::int32_t maximum_major_interval = 100;

    Vec2 origin_m{};
    double rotation_radians{};
    double spacing_x_m{1.0};
    double spacing_y_m{1.0};
    std::int32_t count_x{10};
    std::int32_t count_y{10};
    std::int32_t major_every{5};
    std::string x_label{"A"};
    std::string y_label{"1"};
    bool visible{true};

    bool operator==(const ReferenceGridModel& other) const noexcept {
        return origin_m.x == other.origin_m.x && origin_m.y == other.origin_m.y &&
               rotation_radians == other.rotation_radians &&
               spacing_x_m == other.spacing_x_m && spacing_y_m == other.spacing_y_m &&
               count_x == other.count_x && count_y == other.count_y &&
               major_every == other.major_every && x_label == other.x_label &&
               y_label == other.y_label && visible == other.visible;
    }
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static ReferenceGridModel from_json(const nlohmann::json& value);
    [[nodiscard]] std::vector<ReferenceGridLine> lines() const;
};

}  // namespace sketch
