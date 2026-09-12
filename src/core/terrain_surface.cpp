#include "sketch/terrain_surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;
constexpr double coordinate_limit = 1.0e9;
constexpr double geometry_tolerance = default_geometry_tolerance_metres;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

void finite_coordinate(double value, const char* name) {
    if (!std::isfinite(value) || std::abs(value) > coordinate_limit) {
        invalid(std::string("Terrain ") + name + " is outside the supported range");
    }
}

void validate_text(const std::string& value, const char* name) {
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos) {
        invalid(std::string("Terrain ") + name + " is invalid");
    }
    try {
        (void)Json(value).dump();
    } catch (const Json::exception&) {
        invalid(std::string("Terrain ") + name + " is not valid UTF-8");
    }
}

void validate_identifier(const std::string& value) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid("Terrain point ID is empty or invalid");
    }
}

void exact_fields(const Json& value, std::initializer_list<const char*> fields,
                  const char* context) {
    if (!value.is_object() || value.size() != fields.size()) {
        invalid(std::string(context) + " fields are invalid");
    }
    for (const auto* field : fields) {
        if (!value.contains(field)) invalid(std::string(context) + " field is missing");
    }
}

double number(const Json& value, const char* context) {
    if (!value.is_number()) invalid(std::string(context) + " must be finite");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string(context) + " must be finite");
    return result;
}

std::uint32_t index_value(const Json& value, const char* context, std::size_t point_count) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) invalid(std::string(context) + " must be a point index");
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        invalid(std::string(context) + " must be a point index");
    }
    if (result >= point_count || result > std::numeric_limits<std::uint32_t>::max()) {
        invalid(std::string(context) + " points outside the surface");
    }
    return static_cast<std::uint32_t>(result);
}

void validate_surface(const std::string& provenance,
                      const std::vector<TerrainPoint>& points,
                      const std::vector<TerrainTriangle>& triangles,
                      double contour_interval_m) {
    validate_text(provenance, "provenance");
    if (points.size() < 3 || points.size() > TerrainSurface::maximum_points) {
        invalid("Terrain point count is outside the supported range");
    }
    if (triangles.empty() || triangles.size() > TerrainSurface::maximum_triangles) {
        invalid("Terrain triangle count is outside the supported range");
    }
    if (!std::isfinite(contour_interval_m) || contour_interval_m <= geometry_tolerance ||
        contour_interval_m > coordinate_limit) {
        invalid("Terrain contour interval is outside the supported range");
    }

    std::set<std::string, std::less<>> point_ids;
    for (const auto& point : points) {
        validate_identifier(point.id);
        if (!point_ids.insert(point.id).second) invalid("Terrain point IDs must be unique");
        finite_coordinate(point.x_m, "point X");
        finite_coordinate(point.y_m, "point Y");
        finite_coordinate(point.elevation_m, "point elevation");
    }

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> edge_counts;
    for (const auto& triangle : triangles) {
        const auto a = triangle.point_indices[0];
        const auto b = triangle.point_indices[1];
        const auto c = triangle.point_indices[2];
        if (a >= points.size() || b >= points.size() || c >= points.size() ||
            a == b || a == c || b == c) {
            invalid("Terrain triangle indices are invalid");
        }
        const auto& first = points[a];
        const auto& second = points[b];
        const auto& third = points[c];
        const auto cross = (second.x_m - first.x_m) * (third.y_m - first.y_m) -
                           (second.y_m - first.y_m) * (third.x_m - first.x_m);
        if (!std::isfinite(cross) || std::abs(cross) <= geometry_tolerance) {
            invalid("Terrain triangle has a degenerate plan footprint");
        }
        for (const auto [left, right] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
            const auto edge = std::minmax(left, right);
            auto& count = edge_counts[edge];
            if (++count > 2) invalid("Terrain surface edge is shared by too many triangles");
        }
    }
}

Json point_json(const TerrainPoint& point) {
    return Json{{"id", point.id}, {"x_m", point.x_m}, {"y_m", point.y_m},
                {"elevation_m", point.elevation_m}};
}

}  // namespace

TerrainSurface::TerrainSurface(std::string provenance, std::vector<TerrainPoint> points,
                               std::vector<TerrainTriangle> triangles,
                               double contour_interval_m, bool visible)
    : provenance_(std::move(provenance)), points_(std::move(points)),
      triangles_(std::move(triangles)), contour_interval_m_(contour_interval_m),
      visible_(visible) {
    for (auto& point : points_) {
        if (point.x_m == 0.0) point.x_m = 0.0;
        if (point.y_m == 0.0) point.y_m = 0.0;
        if (point.elevation_m == 0.0) point.elevation_m = 0.0;
    }
    validate_surface(provenance_, points_, triangles_, contour_interval_m_);
}

std::pair<double, double> TerrainSurface::elevation_bounds() const noexcept {
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (const auto& point : points_) {
        minimum = std::min(minimum, point.elevation_m);
        maximum = std::max(maximum, point.elevation_m);
    }
    return {minimum, maximum};
}

Boundary TerrainSurface::plan_edges() const {
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    Boundary result;
    result.reserve(triangles_.size() * 3);
    for (const auto& triangle : triangles_) {
        for (const auto [left, right] : {std::pair{triangle.point_indices[0], triangle.point_indices[1]},
                                         std::pair{triangle.point_indices[1], triangle.point_indices[2]},
                                         std::pair{triangle.point_indices[2], triangle.point_indices[0]}}) {
            const auto edge = std::minmax(left, right);
            if (!seen.insert(edge).second) continue;
            const auto& start = points_[left];
            const auto& end = points_[right];
            result.push_back({{start.x_m, start.y_m}, {end.x_m, end.y_m}, 0.0});
        }
    }
    return result;
}

std::vector<TerrainContourSegment> TerrainSurface::contours() const {
    const auto [minimum, maximum] = elevation_bounds();
    const auto first_level = std::ceil(minimum / contour_interval_m_) * contour_interval_m_;
    const auto last_level = std::floor(maximum / contour_interval_m_) * contour_interval_m_;
    if (!std::isfinite(first_level) || !std::isfinite(last_level) || first_level > last_level) {
        return {};
    }

    std::vector<TerrainContourSegment> result;
    for (double level = first_level; level <= last_level + geometry_tolerance;
         level += contour_interval_m_) {
        if (result.size() >= maximum_contour_segments) {
            invalid("Terrain contour output exceeds the supported limit");
        }
        const auto canonical_level = level == 0.0 ? 0.0 : level;
        for (const auto& triangle : triangles_) {
            const auto& a = points_[triangle.point_indices[0]];
            const auto& b = points_[triangle.point_indices[1]];
            const auto& c = points_[triangle.point_indices[2]];
            std::vector<Vec2> intersections;
            intersections.reserve(2);
            const auto add_intersection = [&](const TerrainPoint& first,
                                              const TerrainPoint& second) {
                const auto first_delta = first.elevation_m - canonical_level;
                const auto second_delta = second.elevation_m - canonical_level;
                const bool crosses = (first_delta <= 0.0 && second_delta > 0.0) ||
                                     (second_delta <= 0.0 && first_delta > 0.0);
                if (!crosses) return;
                const auto denominator = second.elevation_m - first.elevation_m;
                if (std::abs(denominator) <= geometry_tolerance) return;
                const auto fraction = (canonical_level - first.elevation_m) / denominator;
                if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) return;
                intersections.push_back({std::lerp(first.x_m, second.x_m, fraction),
                                         std::lerp(first.y_m, second.y_m, fraction)});
            };
            add_intersection(a, b);
            add_intersection(b, c);
            add_intersection(c, a);
            if (intersections.size() != 2) continue;
            const auto length = std::hypot(intersections[1].x - intersections[0].x,
                                           intersections[1].y - intersections[0].y);
            if (!(length > geometry_tolerance) || !std::isfinite(length)) continue;
            result.push_back({intersections[0], intersections[1], canonical_level});
            if (result.size() >= maximum_contour_segments) {
                invalid("Terrain contour output exceeds the supported limit");
            }
        }
    }
    return result;
}

nlohmann::json TerrainSurface::to_json() const {
    validate_surface(provenance_, points_, triangles_, contour_interval_m_);
    Json points = Json::array();
    for (const auto& point : points_) points.push_back(point_json(point));
    Json triangles = Json::array();
    for (const auto& triangle : triangles_) {
        triangles.push_back({triangle.point_indices[0], triangle.point_indices[1],
                             triangle.point_indices[2]});
    }
    return Json{{"version", 1}, {"provenance", provenance_}, {"points", points},
                {"triangles", triangles}, {"contour_interval_m", contour_interval_m_},
                {"visible", visible_}};
}

TerrainSurface TerrainSurface::from_json(const nlohmann::json& value) {
    try {
        exact_fields(value, {"version", "provenance", "points", "triangles",
                             "contour_interval_m", "visible"}, "Terrain surface JSON");
        if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.at("provenance").is_string() || !value.at("points").is_array() ||
            !value.at("triangles").is_array() || !value.at("visible").is_boolean()) {
            invalid("Terrain surface JSON values are invalid");
        }
        if (value.at("points").size() > TerrainSurface::maximum_points ||
            value.at("triangles").size() > TerrainSurface::maximum_triangles) {
            invalid("Terrain surface JSON exceeds the supported size");
        }
        std::vector<TerrainPoint> points;
        points.reserve(value.at("points").size());
        for (const auto& entry : value.at("points")) {
            exact_fields(entry, {"id", "x_m", "y_m", "elevation_m"}, "Terrain point");
            if (!entry.at("id").is_string()) invalid("Terrain point ID must be a string");
            points.push_back({entry.at("id").get<std::string>(),
                              number(entry.at("x_m"), "Terrain point X"),
                              number(entry.at("y_m"), "Terrain point Y"),
                              number(entry.at("elevation_m"), "Terrain point elevation")});
        }
        std::vector<TerrainTriangle> triangles;
        triangles.reserve(value.at("triangles").size());
        for (const auto& entry : value.at("triangles")) {
            if (!entry.is_array() || entry.size() != 3) {
                invalid("Terrain triangle must contain exactly three indices");
            }
            triangles.push_back({{index_value(entry[0], "Terrain triangle index", points.size()),
                                  index_value(entry[1], "Terrain triangle index", points.size()),
                                  index_value(entry[2], "Terrain triangle index", points.size())}});
        }
        return TerrainSurface(value.at("provenance").get<std::string>(), std::move(points),
                              std::move(triangles),
                              number(value.at("contour_interval_m"),
                                     "Terrain contour interval"),
                              value.at("visible").get<bool>());
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Invalid terrain surface JSON: ") + error.what());
    }
}

}  // namespace sketch
