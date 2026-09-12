#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sketch {
enum class GeoCoordinateUnit { metre };
struct GeoCrs { std::string identifier, definition; GeoCoordinateUnit unit{GeoCoordinateUnit::metre}; };
struct OfflineGeoResource { std::string relative_path, sha256; };
struct OfflineGeoResources { bool network_enabled{}; std::vector<OfflineGeoResource> files; };
struct AffineGeoTransform { double a{1}, b{}, tx{}, c{}, d{1}, ty{}; };
struct GeoControlPoint { std::string id; double local_x{}, local_y{}, target_x{}, target_y{}; };
struct GeoCoordinate { double x{}, y{}; };
struct GeoResidual { std::string id; double dx{}, dy{}, magnitude_m{}; };
// Local planar metre coordinates -> declared projected CRS easting/northing metres.
// Transform is supplied, not fitted. CRS and file declarations are not runtime verification.
class GeoreferencingContract {
public:
    static constexpr std::size_t maximum_control_points = 4096;
    GeoreferencingContract(GeoCrs crs, AffineGeoTransform transform,
                           std::vector<GeoControlPoint> points, OfflineGeoResources resources);
    [[nodiscard]] static GeoreferencingContract from_json(const nlohmann::json& value);
    [[nodiscard]] GeoCoordinate apply(double local_x, double local_y) const;
    [[nodiscard]] const std::vector<GeoResidual>& residuals() const noexcept { return residuals_; }
    [[nodiscard]] double rms_residual_m() const noexcept { return rms_; }
    [[nodiscard]] double maximum_residual_m() const noexcept { return maximum_; }
    [[nodiscard]] const GeoCrs& crs() const noexcept { return crs_; }
    [[nodiscard]] const AffineGeoTransform& transform() const noexcept { return transform_; }
    [[nodiscard]] const std::vector<GeoControlPoint>& control_points() const noexcept {
        return points_;
    }
    [[nodiscard]] const OfflineGeoResources& offline_resources() const noexcept {
        return resources_;
    }
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] std::string serialize() const;
private:
    GeoCrs crs_;
    AffineGeoTransform transform_;
    std::vector<GeoControlPoint> points_;
    OfflineGeoResources resources_;
    std::vector<GeoResidual> residuals_;
    double rms_{}, maximum_{};
};
}
