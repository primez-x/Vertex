#include "sketch/georeferencing_contract.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
void check(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
double finite(double value) { check(std::isfinite(value), "Nonfinite georeferencing value"); return value == 0 ? 0 : value; }
void text(const std::string& value, std::size_t limit=4096) {
    check(!value.empty() && value.size() <= limit && value.find('\0') == std::string::npos, "Invalid georeferencing text");
    try { (void)nlohmann::json(value).dump(); } catch (const nlohmann::json::exception&) { throw std::invalid_argument("Invalid georeferencing UTF-8"); }
}
void path(const std::string& value) {
    text(value, 512);
    // Portable relative ASCII components only; no Windows aliases, ADS, UNC or URLs.
    std::size_t start=0;
    while (start<value.size()) {
        const auto end=value.find('/',start);
        const auto part=value.substr(start,end==std::string::npos ? end : end-start);
        check(!part.empty() && part!="." && part!=".." && part.back()!='.', "Unsafe offline resource path");
        check(std::all_of(part.begin(),part.end(),[](char c) { return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-' || c=='.'; }), "Unsafe offline resource character");
        auto stem=part.substr(0,part.find('.'));
        std::transform(stem.begin(),stem.end(),stem.begin(),[](char c){return c>='a'&&c<='z' ? static_cast<char>(c-'a'+'A') : c;});
        check(stem!="CON" && stem!="PRN" && stem!="AUX" && stem!="NUL" && !(stem.size()==4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3]>='1' && stem[3]<='9'), "Reserved offline resource path");
        if (end==std::string::npos) break;
        start=end+1; check(start<value.size(), "Trailing resource separator");
    }
}

void exact_object(const Json& value, std::initializer_list<const char*> keys,
                  const char* label) {
    check(value.is_object(), label);
    check(value.size() == keys.size(), label);
    for (const auto* key : keys) check(value.contains(key), label);
}

double number(const Json& value, const char* label) {
    check(value.is_number(), label);
    const auto result = value.get<double>();
    return finite(result);
}

std::string string_value(const Json& value, const char* label) {
    check(value.is_string(), label);
    return value.get<std::string>();
}
}
GeoreferencingContract::GeoreferencingContract(GeoCrs crs, AffineGeoTransform transform, std::vector<GeoControlPoint> points, OfflineGeoResources resources)
    : crs_(std::move(crs)), transform_(transform), points_(std::move(points)), resources_(std::move(resources)) {
    text(crs_.identifier,256); text(crs_.definition,16384);
    check(crs_.unit==GeoCoordinateUnit::metre, "Only projected metre coordinates supported");
    check(!resources_.network_enabled, "Georeferencing networking is forbidden");
    check(!resources_.files.empty() && resources_.files.size()<=256, "Offline resource declaration required");
    std::set<std::string> paths;
    for (auto& resource: resources_.files) {
        path(resource.relative_path);
        auto key=resource.relative_path;
        std::transform(key.begin(),key.end(),key.begin(),[](char c){return c>='A'&&c<='Z' ? static_cast<char>(c-'A'+'a') : c;});
        check(paths.insert(key).second,"Duplicate offline resource path");
        check(resource.sha256.size()==64 && std::all_of(resource.sha256.begin(),resource.sha256.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}), "Expected lowercase SHA256");
    }
    std::sort(resources_.files.begin(),resources_.files.end(),[](const auto& a,const auto& b){return a.relative_path<b.relative_path;});
    auto& t=transform_;
    for (auto* v: {&t.a,&t.b,&t.tx,&t.c,&t.d,&t.ty}) *v=finite(*v);
    const double scale=std::max({std::abs(t.a),std::abs(t.b),std::abs(t.c),std::abs(t.d)});
    check(scale>0,"Singular transform");
    check(std::abs((t.a/scale)*(t.d/scale)-(t.b/scale)*(t.c/scale))>1e-12,"Singular or ill-conditioned transform");
    check(!points_.empty() && points_.size()<=maximum_control_points,"Invalid control point count");
    std::sort(points_.begin(),points_.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::set<std::string> ids;
    double norm=0;
    for (auto& p: points_) {
        text(p.id,256); check(ids.insert(p.id).second,"Duplicate control point ID");
        for (auto* v: {&p.local_x,&p.local_y,&p.target_x,&p.target_y}) *v=finite(*v);
        const auto projected=apply(p.local_x,p.local_y);
        const auto dx=finite(projected.x-p.target_x), dy=finite(projected.y-p.target_y);
        const auto magnitude=finite(std::hypot(dx,dy));
        residuals_.push_back({p.id,dx,dy,magnitude});
        maximum_=std::max(maximum_,magnitude);
        norm=finite(std::hypot(norm,magnitude/std::sqrt(static_cast<double>(points_.size()))));
    }
    rms_=norm;
}
GeoCoordinate GeoreferencingContract::apply(double x,double y) const {
    x=finite(x); y=finite(y); const auto& t=transform_;
    return {finite(finite(finite(t.a*x)+finite(t.b*y))+t.tx), finite(finite(finite(t.c*x)+finite(t.d*y))+t.ty)};
}
Json GeoreferencingContract::to_json() const {
    nlohmann::json points=nlohmann::json::array(), residuals=nlohmann::json::array(), resources=nlohmann::json::array();
    for (const auto& p: points_) points.push_back({{"id",p.id},{"local_x_m",p.local_x},{"local_y_m",p.local_y},{"target_easting_m",p.target_x},{"target_northing_m",p.target_y}});
    for (const auto& r: residuals_) residuals.push_back({{"id",r.id},{"dx_m",r.dx},{"dy_m",r.dy},{"magnitude_m",r.magnitude_m}});
    for (const auto& r: resources_.files) resources.push_back({{"relative_path",r.relative_path},{"sha256",r.sha256}});
    const auto& t=transform_;
    return nlohmann::json{{"version",1},{"crs",{{"identifier",crs_.identifier},{"definition",crs_.definition},{"unit","metre"},{"axis_order","easting_northing"}}},
        {"transform",{{"kind","supplied_affine_2d"},{"a",t.a},{"b",t.b},{"tx",t.tx},{"c",t.c},{"d",t.d},{"ty",t.ty}}},
        {"control_points",points},{"residuals",residuals},{"rms_residual_m",rms_},{"maximum_residual_m",maximum_},
        {"network_enabled",false},{"offline_resources",resources},{"resource_verification","declaration_only"}};
}

std::string GeoreferencingContract::serialize() const {
    return to_json().dump();
}

GeoreferencingContract GeoreferencingContract::from_json(const nlohmann::json& value) {
    try {
        exact_object(value, {"version", "crs", "transform", "control_points", "residuals",
                             "rms_residual_m", "maximum_residual_m", "network_enabled",
                             "offline_resources", "resource_verification"},
                     "Malformed georeferencing envelope");
        check(value.at("version").is_number_integer() && value.at("version") == 1,
              "Unsupported georeferencing version");
        const auto& crs = value.at("crs");
        exact_object(crs, {"identifier", "definition", "unit", "axis_order"},
                     "Malformed georeferencing CRS");
        check(crs.at("unit") == "metre" && crs.at("axis_order") == "easting_northing",
              "Unsupported georeferencing CRS axes");
        GeoCrs decoded_crs{string_value(crs.at("identifier"), "CRS identifier must be text"),
                           string_value(crs.at("definition"), "CRS definition must be text"),
                           GeoCoordinateUnit::metre};

        const auto& transform = value.at("transform");
        exact_object(transform, {"kind", "a", "b", "tx", "c", "d", "ty"},
                     "Malformed georeferencing transform");
        check(transform.at("kind") == "supplied_affine_2d",
              "Unsupported georeferencing transform");
        const AffineGeoTransform decoded_transform{
            number(transform.at("a"), "Transform coefficient must be numeric"),
            number(transform.at("b"), "Transform coefficient must be numeric"),
            number(transform.at("tx"), "Transform coefficient must be numeric"),
            number(transform.at("c"), "Transform coefficient must be numeric"),
            number(transform.at("d"), "Transform coefficient must be numeric"),
            number(transform.at("ty"), "Transform coefficient must be numeric")};

        const auto& points_json = value.at("control_points");
        check(points_json.is_array(), "Control points must be an array");
        std::vector<GeoControlPoint> points;
        points.reserve(points_json.size());
        for (const auto& point : points_json) {
            exact_object(point, {"id", "local_x_m", "local_y_m", "target_easting_m",
                                 "target_northing_m"}, "Malformed georeferencing control point");
            points.push_back({string_value(point.at("id"), "Control point ID must be text"),
                              number(point.at("local_x_m"), "Control point coordinate must be numeric"),
                              number(point.at("local_y_m"), "Control point coordinate must be numeric"),
                              number(point.at("target_easting_m"), "Control point coordinate must be numeric"),
                              number(point.at("target_northing_m"), "Control point coordinate must be numeric")});
        }

        const auto& resources_json = value.at("offline_resources");
        check(resources_json.is_array(), "Offline resources must be an array");
        std::vector<OfflineGeoResource> resources;
        resources.reserve(resources_json.size());
        for (const auto& resource : resources_json) {
            exact_object(resource, {"relative_path", "sha256"},
                         "Malformed offline georeferencing resource");
            resources.push_back({string_value(resource.at("relative_path"),
                                              "Resource path must be text"),
                                 string_value(resource.at("sha256"),
                                              "Resource digest must be text")});
        }
        check(value.at("network_enabled").is_boolean() && !value.at("network_enabled").get<bool>(),
              "Georeferencing networking is forbidden");
        check(value.at("resource_verification") == "declaration_only",
              "Unsupported georeferencing resource verification");
        const auto result = GeoreferencingContract(
            std::move(decoded_crs), decoded_transform, std::move(points),
            OfflineGeoResources{false, std::move(resources)});
        check(value.at("residuals") == result.to_json().at("residuals"),
              "Georeferencing residuals do not match the supplied transform");
        check(value.at("rms_residual_m") == result.to_json().at("rms_residual_m") &&
                  value.at("maximum_residual_m") == result.to_json().at("maximum_residual_m"),
              "Georeferencing residual summary does not match the supplied transform");
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("Malformed georeferencing JSON: ") + error.what());
    }
}
}
