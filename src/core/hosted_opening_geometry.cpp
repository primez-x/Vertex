#include "sketch/hosted_opening_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;
constexpr double full_turn = 2*std::numbers::pi;

bool finite(Vec2 point) { return std::isfinite(point.x) && std::isfinite(point.y); }
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

struct HostGeometry {
    double length{};
    Vec2 along{};
    Vec2 center{};
    Vec2 start_radial{};
    double radius{};
};

HostGeometry host_geometry(const Segment& host) {
    require(finite(host.start) && finite(host.end) && std::isfinite(host.sweep_radians),
        "Hosted opening baseline must be finite");
    require(std::abs(host.sweep_radians) < full_turn,
        "Hosted opening arc sweep must be less than two pi");
    const Vec2 chord{host.end.x-host.start.x,host.end.y-host.start.y};
    const double chord_length = std::hypot(chord.x,chord.y);
    require(finite(chord) && std::isfinite(chord_length) && chord_length > 0,
        "Hosted opening baseline must have a representable chord");
    HostGeometry result;
    result.length = segment_length(host);
    require(std::isfinite(result.length) && result.length > tolerance,
        "Hosted opening baseline must have a positive length");
    result.along = {chord.x/chord_length,chord.y/chord_length};
    if (host.sweep_radians != 0) {
        const double half = host.sweep_radians*.5;
        const double sine = std::sin(std::abs(half));
        const double tangent = std::tan(half);
        require(sine > 0 && tangent != 0 && std::isfinite(tangent),
            "Hosted opening arc sweep cannot be represented");
        const double center_distance = (chord_length*.5)/tangent;
        result.radius = (chord_length*.5)/sine;
        const Vec2 left{-result.along.y,result.along.x};
        result.center = {std::midpoint(host.start.x,host.end.x)+left.x*center_distance,
                         std::midpoint(host.start.y,host.end.y)+left.y*center_distance};
        result.start_radial = {-chord.x*.5-left.x*center_distance,
                               -chord.y*.5-left.y*center_distance};
        require(std::isfinite(center_distance) && std::isfinite(result.radius) &&
            result.radius > tolerance && finite(result.center) && finite(result.start_radial),
            "Hosted opening arc geometry exceeds numeric range");
    }
    // The same analytical bounds check used by core geometry also rejects
    // nonrepresentable extrema, even when both endpoints happen to be finite.
    (void)segment_bounds(host);
    return result;
}

double station_angle(const Segment& host, const HostGeometry& geometry, double station) {
    const double angle = (host.sweep_radians > 0 ? station : -station)/geometry.radius;
    require(std::isfinite(angle), "Hosted opening station exceeds numeric range");
    return angle;
}

Vec2 point_at(const Segment& host, const HostGeometry& geometry, double station) {
    require(std::isfinite(station), "Hosted opening station must be finite");
    if (station == 0) return host.start;
    if (station == geometry.length) return host.end;
    Vec2 result;
    if (host.sweep_radians == 0) {
        result = {host.start.x+geometry.along.x*station,host.start.y+geometry.along.y*station};
    } else {
        const double angle = station_angle(host,geometry,station);
        const double sine = std::sin(angle);
        const double half_sine = std::sin(angle*.5);
        const double cosine_delta = -2*half_sine*half_sine;
        // Start-relative rotation avoids subtracting the large nearly equal
        // centre/radius terms of a shallow arc.
        result = {host.start.x+geometry.start_radial.x*cosine_delta-geometry.start_radial.y*sine,
                  host.start.y+geometry.start_radial.x*sine+geometry.start_radial.y*cosine_delta};
    }
    require(finite(result), "Hosted opening point exceeds numeric range");
    return result;
}

Segment span(const Segment& host, const HostGeometry& geometry, double offset, double width) {
    const double end = offset+width;
    require(std::isfinite(offset) && offset >= 0 && std::isfinite(width) && width > tolerance &&
        std::isfinite(end) && offset < geometry.length && end <= geometry.length+tolerance,
        "Hosted opening dimensions must fit the host baseline");
    const double end_station = std::min(end,geometry.length);
    Segment result{point_at(host,geometry,offset),point_at(host,geometry,end_station),
        host.sweep_radians*((end_station-offset)/geometry.length)};
    require(segment_length(result) > tolerance, "Hosted opening span is not representable");
    (void)segment_bounds(result);
    return result;
}

Vec2 left_at(const Segment& host, const HostGeometry& geometry, double station) {
    if (host.sweep_radians == 0) return {-geometry.along.y,geometry.along.x};
    const double angle = station_angle(host,geometry,station);
    const double x = geometry.start_radial.x/geometry.radius;
    const double y = geometry.start_radial.y/geometry.radius;
    const double sign = host.sweep_radians > 0 ? -1 : 1;
    return {sign*(x*std::cos(angle)-y*std::sin(angle)),
            sign*(x*std::sin(angle)+y*std::cos(angle))};
}
} // namespace

Vec2 point_at_host_station(const Segment& host, double station_metres) {
    return point_at(host,host_geometry(host),station_metres);
}

double project_host_station(const Segment& host, Vec2 point, double reference_station_metres) {
    const auto geometry = host_geometry(host);
    require(finite(point) && std::isfinite(reference_station_metres),
        "Hosted opening projection requires finite point and reference station");
    double station;
    if (host.sweep_radians == 0) {
        const Vec2 delta{point.x-host.start.x,point.y-host.start.y};
        require(finite(delta), "Hosted opening projection exceeds numeric range");
        station = delta.x*geometry.along.x+delta.y*geometry.along.y;
    } else {
        const Vec2 radial{point.x-geometry.center.x,point.y-geometry.center.y};
        const double radial_length = std::hypot(radial.x,radial.y);
        require(finite(radial) && std::isfinite(radial_length) && radial_length > tolerance,
            "Hosted opening arc centre has no unique projection");
        const Vec2 start{geometry.start_radial.x/geometry.radius,geometry.start_radial.y/geometry.radius};
        const Vec2 current{radial.x/radial_length,radial.y/radial_length};
        const double angle = std::atan2(start.x*current.y-start.y*current.x,
                                       start.x*current.x+start.y*current.y);
        const double reference_angle = station_angle(host,geometry,reference_station_metres);
        const double delta = std::remainder(angle-std::remainder(reference_angle,full_turn),full_turn);
        station = reference_station_metres+(host.sweep_radians > 0 ? delta : -delta)*geometry.radius;
    }
    require(std::isfinite(station), "Hosted opening projection exceeds numeric range");
    return station;
}

Segment hosted_opening_span(const Segment& host, double offset_metres, double width_metres) {
    return span(host,host_geometry(host),offset_metres,width_metres);
}

Boundary window_plan_symbol(const Segment& host, double offset_metres, double width_metres,
                            double wall_thickness_metres) {
    const auto geometry = host_geometry(host);
    const auto opening = span(host,geometry,offset_metres,width_metres);
    require(std::isfinite(wall_thickness_metres) && wall_thickness_metres > tolerance,
        "Window host thickness must be finite and positive");
    if (host.sweep_radians != 0)
        require(wall_thickness_metres*.5 < geometry.radius-tolerance,
            "Window host thickness crosses its arc centre");
    const double inset = std::clamp(wall_thickness_metres*.22,.015,.06);
    if (host.sweep_radians != 0)
        require(inset < geometry.radius-tolerance,
            "Window glazing inset crosses its arc centre");
    const Vec2 start_left = left_at(host,geometry,offset_metres);
    const Vec2 end_left = left_at(host,geometry,std::min(offset_metres+width_metres,geometry.length));
    const auto shift = [](Vec2 point, Vec2 left, double distance) {
        return Vec2{point.x+left.x*distance,point.y+left.y*distance};
    };
    // Preserve the old straight symbol's arithmetic as well as its shape:
    // its end is computed from the opening start plus tangent times width.
    const Vec2 end = host.sweep_radians == 0 && offset_metres+width_metres <= geometry.length
        ? Vec2{opening.start.x+geometry.along.x*width_metres,
               opening.start.y+geometry.along.y*width_metres} : opening.end;
    const auto a=shift(opening.start,start_left,inset),b=shift(end,end_left,inset);
    const auto c=shift(opening.start,start_left,-inset),d=shift(end,end_left,-inset);
    Boundary result{{a,b,opening.sweep_radians},{c,d,opening.sweep_radians},{c,a,0},{d,b,0}};
    for (const auto& edge : result) {
        require(segment_length(edge) > tolerance, "Window plan symbol is not representable");
        (void)segment_bounds(edge);
    }
    return result;
}
Boundary wall_plan_footprint(const Segment& host,
    const std::vector<HostedOpening>& openings, double thickness) {
    const auto geometry=host_geometry(host);
    require(std::isfinite(thickness) && thickness>tolerance,
        "Wall plan thickness must be finite and positive");
    if(host.sweep_radians!=0)
        require(thickness*.5<geometry.radius-tolerance,"Wall plan thickness crosses its arc centre");
    std::vector<std::pair<double,double>> cuts;
    for(const auto& opening:openings) {
        const auto end=opening.offset+opening.width;
        require(std::isfinite(opening.offset) && std::isfinite(opening.width) &&
            opening.offset>=-tolerance && opening.width>tolerance && std::isfinite(end) &&
            end<=geometry.length+tolerance,"Wall plan opening must fit the host");
        cuts.emplace_back(std::max(0.0,opening.offset),std::min(end,geometry.length));
    }
    std::sort(cuts.begin(),cuts.end());
    Boundary result;
    const auto append=[&](double from,double to) {
        if(to-from<=tolerance) return;
        const auto part=span(host,geometry,from,to-from);
        const auto first_normal=left_at(host,geometry,from);
        const auto last_normal=left_at(host,geometry,to);
        const auto shift=[](Vec2 p,Vec2 n,double d){return Vec2{p.x+n.x*d,p.y+n.y*d};};
        const Segment left{shift(part.start,first_normal,thickness*.5),
            shift(part.end,last_normal,thickness*.5),part.sweep_radians};
        const Segment right{shift(part.start,first_normal,-thickness*.5),
            shift(part.end,last_normal,-thickness*.5),part.sweep_radians};
        result.insert(result.end(),{left,{left.end,right.end,0},
            {right.end,right.start,-right.sweep_radians},{right.start,left.start,0}});
    };
    double cursor=0;
    for(const auto& [from,to]:cuts) {
        append(cursor,from);
        cursor=std::max(cursor,to);
    }
    append(cursor,geometry.length);
    for(const auto& edge:result) (void)segment_bounds(edge);
    return result;
}
} // namespace sketch
