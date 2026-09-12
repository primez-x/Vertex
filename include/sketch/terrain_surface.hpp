#pragma once

#include "sketch/geometry.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sketch {

// A terrain point is an authored model-space coordinate in metres.  The
// stable point ID is retained so a future editor can update a survey without
// replacing unrelated triangles.
struct TerrainPoint {
    std::string id;
    double x_m{};
    double y_m{};
    double elevation_m{};

    bool operator==(const TerrainPoint&) const noexcept = default;
};

struct TerrainTriangle {
    std::array<std::uint32_t, 3> point_indices{};

    bool operator==(const TerrainTriangle&) const noexcept = default;
};

struct TerrainContourSegment {
    Vec2 start{};
    Vec2 end{};
    double elevation_m{};

    bool operator==(const TerrainContourSegment&) const noexcept = default;
};

// Versioned bounded triangulated terrain surface.  This is a deterministic
// local TIN model: plan edges and contour segments are derived from the same
// points/triangles used by the native 3D surface.  It carries no hosted
// service or external elevation dependency.
class TerrainSurface final {
public:
    static constexpr std::size_t maximum_points = 4096;
    static constexpr std::size_t maximum_triangles = 8192;
    static constexpr std::size_t maximum_contour_segments = 1'000'000;

    TerrainSurface(std::string provenance, std::vector<TerrainPoint> points,
                   std::vector<TerrainTriangle> triangles,
                   double contour_interval_m = 1.0, bool visible = true);

    [[nodiscard]] const std::string& provenance() const noexcept { return provenance_; }
    [[nodiscard]] const std::vector<TerrainPoint>& points() const noexcept { return points_; }
    [[nodiscard]] const std::vector<TerrainTriangle>& triangles() const noexcept {
        return triangles_;
    }
    [[nodiscard]] double contour_interval_m() const noexcept { return contour_interval_m_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }

    [[nodiscard]] std::pair<double, double> elevation_bounds() const noexcept;
    // Deduplicated triangulation edges for plan/canvas presentation.
    [[nodiscard]] Boundary plan_edges() const;
    // Linear contour intersections at the configured interval.  A contour
    // touching only one triangle vertex is omitted rather than duplicated.
    [[nodiscard]] std::vector<TerrainContourSegment> contours() const;

    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static TerrainSurface from_json(const nlohmann::json& value);

private:
    std::string provenance_;
    std::vector<TerrainPoint> points_;
    std::vector<TerrainTriangle> triangles_;
    double contour_interval_m_{};
    bool visible_{};
};

}  // namespace sketch
