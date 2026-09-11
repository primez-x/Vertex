#include "sketch/georeferencing_contract.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
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
std::string GeoreferencingContract::serialize() const {
    nlohmann::json points=nlohmann::json::array(), residuals=nlohmann::json::array(), resources=nlohmann::json::array();
    for (const auto& p: points_) points.push_back({{"id",p.id},{"local_x_m",p.local_x},{"local_y_m",p.local_y},{"target_easting_m",p.target_x},{"target_northing_m",p.target_y}});
    for (const auto& r: residuals_) residuals.push_back({{"id",r.id},{"dx_m",r.dx},{"dy_m",r.dy},{"magnitude_m",r.magnitude_m}});
    for (const auto& r: resources_.files) resources.push_back({{"relative_path",r.relative_path},{"sha256",r.sha256}});
    const auto& t=transform_;
    return nlohmann::json{{"version",1},{"crs",{{"identifier",crs_.identifier},{"definition",crs_.definition},{"unit","metre"},{"axis_order","easting_northing"}}},
        {"transform",{{"kind","supplied_affine_2d"},{"a",t.a},{"b",t.b},{"tx",t.tx},{"c",t.c},{"d",t.d},{"ty",t.ty}}},
        {"control_points",points},{"residuals",residuals},{"rms_residual_m",rms_},{"maximum_residual_m",maximum_},
        {"network_enabled",false},{"offline_resources",resources},{"resource_verification","declaration_only"}}.dump();
}
}
