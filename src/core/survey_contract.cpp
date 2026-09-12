#include "sketch/survey_contract.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
double finite(double value) { check(std::isfinite(value), "Nonfinite survey calculation"); return value == 0 ? 0 : value; }
void text(const std::string& value) {
    check(!value.empty() && value.size() <= 4096, "Invalid survey text");
    try { (void)nlohmann::json(value).dump(); } catch (const nlohmann::json::exception&) { throw std::invalid_argument("Invalid survey UTF-8"); }
}
const char* quadrant(BearingQuadrant q) {
    switch(q) {
    case BearingQuadrant::north_east: return "NE";
    case BearingQuadrant::south_east: return "SE";
    case BearingQuadrant::south_west: return "SW";
    case BearingQuadrant::north_west: return "NW";
    }
    throw std::invalid_argument("Unsupported bearing quadrant");
}
double cross(SurveyVertex a, SurveyVertex b, SurveyVertex c) {
    return finite(finite((b.east_m-a.east_m)*(c.north_m-a.north_m)) - finite((b.north_m-a.north_m)*(c.east_m-a.east_m)));
}
bool on(SurveyVertex a, SurveyVertex b, SurveyVertex p) {
    return p.east_m >= std::min(a.east_m,b.east_m) && p.east_m <= std::max(a.east_m,b.east_m) && p.north_m >= std::min(a.north_m,b.north_m) && p.north_m <= std::max(a.north_m,b.north_m);
}
bool intersects(SurveyVertex a, SurveyVertex b, SurveyVertex c, SurveyVertex d) {
    const double x=cross(a,b,c), y=cross(a,b,d), z=cross(c,d,a), w=cross(c,d,b);
    return ((x>0 && y<0 || x<0 && y>0) && (z>0 && w<0 || z<0 && w>0)) ||
        (x==0 && on(a,b,c)) || (y==0 && on(a,b,d)) || (z==0 && on(c,d,a)) || (w==0 && on(c,d,b));
}
}
double parse_survey_angle(std::string_view expression) {
    check(expression.size() <= 128, "Bearing angle is too long");
    const auto trim = [](std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return value;
    };
    const auto number = [&](std::string_view value, bool integer) {
        value = trim(value);
        check(!value.empty(), "Missing bearing angle component");
        check(std::all_of(value.begin(), value.end(), [integer](char c) {
            return (c >= '0' && c <= '9') || (!integer && c == '.');
        }), "Use unsigned decimal numbers in a bearing angle");
        double result{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result),
              "Invalid bearing angle number");
        return result;
    };
    expression = trim(expression);
    if (expression.find(':') == std::string_view::npos) {
        const auto degrees = number(expression, false);
        check(degrees <= 90, "Angle must be between 0 and 90 degrees");
        return degrees;
    }
    std::array<std::string_view, 3> parts;
    for (std::size_t index = 0; index < 2; ++index) {
        const auto separator = expression.find(':');
        check(separator != std::string_view::npos, "Use degrees:minutes:seconds for a DMS bearing");
        parts[index] = expression.substr(0, separator);
        expression.remove_prefix(separator + 1);
    }
    parts[2] = expression;
    const auto degrees = number(parts[0], true);
    const auto minutes = number(parts[1], true);
    const auto seconds = number(parts[2], false);
    check(degrees <= 90 && minutes < 60 && seconds < 60 &&
              (degrees < 90 || (minutes == 0 && seconds == 0)),
          "DMS bearing requires degrees 0–90 and minutes/seconds below 60");
    return degrees + minutes / 60.0 + seconds / 3600.0;
}

SurveyTraverse::SurveyTraverse(std::string provenance, std::vector<SurveyLeg> legs, double tolerance)
    : provenance_(std::move(provenance)), legs_(std::move(legs)), tolerance_(finite(tolerance)) {
    text(provenance_);
    check(!legs_.empty() && legs_.size() <= maximum_legs, "Invalid survey leg count");
    check(tolerance_ >= 0, "Negative closure tolerance");
    std::set<std::string> ids;
    vertices_.push_back({});
    for (auto& leg : legs_) {
        text(leg.id); check(ids.insert(leg.id).second, "Duplicate survey leg ID");
        (void)quadrant(leg.quadrant);
        leg.angle_degrees = finite(leg.angle_degrees); leg.distance_m = finite(leg.distance_m);
        check(leg.angle_degrees >= 0 && leg.angle_degrees <= 90 && leg.distance_m > 0, "Invalid bearing or distance");
        const double radians = leg.angle_degrees * std::numbers::pi / 180;
        double east = leg.distance_m * (leg.angle_degrees == 90 ? 1 : std::sin(radians));
        double north = leg.distance_m * (leg.angle_degrees == 90 ? 0 : std::cos(radians));
        if (leg.quadrant == BearingQuadrant::south_east || leg.quadrant == BearingQuadrant::south_west) north = -north;
        if (leg.quadrant == BearingQuadrant::north_west || leg.quadrant == BearingQuadrant::south_west) east = -east;
        vertices_.push_back({finite(vertices_.back().east_m + east), finite(vertices_.back().north_m + north)});
        diagnostics_.perimeter_m = finite(diagnostics_.perimeter_m + leg.distance_m);
    }
    diagnostics_.east_error_m = vertices_.back().east_m; diagnostics_.north_error_m = vertices_.back().north_m;
    diagnostics_.linear_error_m = finite(std::hypot(diagnostics_.east_error_m, diagnostics_.north_error_m));
    diagnostics_.relative_error = finite(diagnostics_.linear_error_m / diagnostics_.perimeter_m);
    diagnostics_.closed = diagnostics_.linear_error_m <= tolerance_;
    if (!diagnostics_.closed || legs_.size() < 3) return;
    // Area includes the explicit closing segment to origin; retain the measured endpoint.
    auto polygon = vertices_;
    if (polygon.back().east_m == 0 && polygon.back().north_m == 0) polygon.pop_back();
    const auto n = polygon.size();
    for (std::size_t i=0; i<n; ++i) {
        const auto prev=polygon[(i+n-1)%n], cur=polygon[i], next=polygon[(i+1)%n];
        check(!(cross(prev,cur,next)==0 && (on(prev,cur,next) || on(cur,next,prev))), "Overlapping adjacent survey segments");
        for (std::size_t j=i+1; j<n; ++j) {
            if (j==i+1 || (i==0 && j==n-1)) continue;
            check(!intersects(polygon[i], polygon[(i+1)%n], polygon[j], polygon[(j+1)%n]), "Self-intersecting survey polygon");
        }
    }
    double area=0;
    for (std::size_t i=0;i<n;++i) area=finite(area+cross({}, polygon[i],polygon[(i+1)%n]));
    area=finite(std::abs(area)/2); check(area>0, "Degenerate survey polygon");
    diagnostics_.area_m2=area; diagnostics_.acres=finite(area/4046.8564224);
}
std::string SurveyTraverse::serialize() const {
    nlohmann::json legs=nlohmann::json::array(), vertices=nlohmann::json::array();
    for (const auto& leg: legs_) legs.push_back({{"id",leg.id},{"quadrant",quadrant(leg.quadrant)},{"angle_degrees",leg.angle_degrees},{"distance_m",leg.distance_m}});
    for (const auto& v: vertices_) vertices.push_back({{"east_m",v.east_m},{"north_m",v.north_m}});
    const auto& d=diagnostics_;
    return nlohmann::json{{"version",1},{"provenance",provenance_},{"legs",legs},{"vertices",vertices},{"closure_tolerance_m",tolerance_},
        {"diagnostics",{{"east_error_m",d.east_error_m},{"north_error_m",d.north_error_m},{"linear_error_m",d.linear_error_m},{"perimeter_m",d.perimeter_m},{"relative_error",d.relative_error},{"closed",d.closed},{"area_m2",d.area_m2 ? nlohmann::json(*d.area_m2) : nlohmann::json(nullptr)},{"acres",d.acres ? nlohmann::json(*d.acres) : nlohmann::json(nullptr)}}}}.dump();
}
}
