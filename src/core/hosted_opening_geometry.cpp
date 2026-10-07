#include "sketch/hosted_opening_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <numeric>
#include <optional>
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

using WallInterval = std::pair<double,double>;

std::vector<WallInterval> remaining_wall_intervals(
    double length, const std::vector<HostedOpening>& openings) {
    std::vector<WallInterval> cuts;
    for (const auto& opening : openings) {
        const auto end=opening.offset+opening.width;
        require(std::isfinite(opening.offset) && std::isfinite(opening.width) &&
            opening.offset>=-tolerance && opening.width>tolerance && std::isfinite(end) &&
            end<=length+tolerance,"Wall plan opening must fit the host");
        cuts.emplace_back(std::max(0.0,opening.offset),std::min(end,length));
    }
    std::sort(cuts.begin(),cuts.end());
    std::vector<WallInterval> result;
    double cursor=0;
    for (const auto& [from,to] : cuts) {
        if (from-cursor>tolerance) result.emplace_back(cursor,from);
        cursor=std::max(cursor,to);
    }
    if (length-cursor>tolerance) result.emplace_back(cursor,length);
    return result;
}

Vec2 add(Vec2 a, Vec2 b) { return {a.x+b.x,a.y+b.y}; }
Vec2 subtract(Vec2 a, Vec2 b) { return {a.x-b.x,a.y-b.y}; }
Vec2 multiply(Vec2 point, double scalar) { return {point.x*scalar,point.y*scalar}; }
double dot(Vec2 a, Vec2 b) { return a.x*b.x+a.y*b.y; }
double cross(Vec2 a, Vec2 b) { return a.x*b.y-a.y*b.x; }
Vec2 left_normal(Vec2 direction) { return {-direction.y,direction.x}; }

struct EndpointJoin {
    Vec2 left_point;
    Vec2 right_point;
    double left_station{};
    double right_station{};
};

bool preserves_local_offset(double world, double origin, double offset) {
    const double represented=world-origin;
    return std::isfinite(world) && std::isfinite(represented) &&
        std::abs(represented-offset)<=tolerance;
}

std::optional<EndpointJoin> endpoint_join(const Segment& host,
    const HostGeometry& host_info, double thickness, const WallPlanJunction& junction,
    const std::vector<WallInterval>& intervals) {
    if (host.sweep_radians!=0 || junction.neighbor.sweep_radians!=0 ||
        !std::isfinite(junction.neighbor_thickness) || junction.neighbor_thickness<=tolerance ||
        intervals.empty())
        return std::nullopt;

    HostGeometry neighbor_geometry;
    try {
        neighbor_geometry=host_geometry(junction.neighbor);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    const Vec2 host_endpoint=junction.at_start ? host.start : host.end;
    const double start_distance=std::hypot(junction.neighbor.start.x-host_endpoint.x,
                                           junction.neighbor.start.y-host_endpoint.y);
    const double end_distance=std::hypot(junction.neighbor.end.x-host_endpoint.x,
                                         junction.neighbor.end.y-host_endpoint.y);
    const bool neighbor_at_start=start_distance<=tolerance;
    const bool neighbor_at_end=end_distance<=tolerance;
    if (neighbor_at_start==neighbor_at_end) return std::nullopt;
    const Vec2 neighbor_endpoint=neighbor_at_start ? junction.neighbor.start : junction.neighbor.end;
    const Vec2 endpoint_delta=subtract(neighbor_endpoint,host_endpoint);
    if (!finite(endpoint_delta)) return std::nullopt;
    if (junction.at_start ? intervals.front().first>tolerance
                          : intervals.back().second<host_info.length-tolerance)
        return std::nullopt;

    const Vec2 host_direction=host_info.along;
    const Vec2 neighbor_direction=neighbor_geometry.along;
    const double direction_cross=cross(host_direction,neighbor_direction);
    const double absolute_cross=std::abs(direction_cross);
    constexpr double parallel_sine_tolerance=1e-6;
    constexpr double miter_limit=8.0;

    if (absolute_cross<=parallel_sine_tolerance) {
        const Vec2 host_inward=junction.at_start ? host_direction : multiply(host_direction,-1);
        const Vec2 neighbor_inward=neighbor_at_start ? neighbor_direction : multiply(neighbor_direction,-1);
        const bool continues=dot(host_inward,neighbor_inward)<-1+parallel_sine_tolerance;
        const bool equal_width=std::abs(thickness-junction.neighbor_thickness)<=tolerance;
        const bool collinear=std::abs(cross(endpoint_delta,host_direction))<=tolerance;
        if (!continues || !equal_width || !collinear) return std::nullopt;

        // Pick the midpoint of tolerance-close endpoints so each wall computes
        // the same seam even if the stored endpoints differ by rounding noise.
        const Vec2 joint{std::midpoint(host_endpoint.x,neighbor_endpoint.x),
                         std::midpoint(host_endpoint.y,neighbor_endpoint.y)};
        const Vec2 normal=left_normal(host_direction);
        const Vec2 joint_delta=subtract(joint,host_endpoint);
        const Vec2 left_delta=add(joint_delta,multiply(normal,thickness*.5));
        const Vec2 right_delta=add(joint_delta,multiply(normal,-thickness*.5));
        const Vec2 left_point=add(host_endpoint,left_delta);
        const Vec2 right_point=add(host_endpoint,right_delta);
        const double station_base=junction.at_start ? 0 : host_info.length;
        const double station=station_base+dot(joint_delta,host_direction);
        const Vec2 left_neighbor_delta=subtract(left_delta,endpoint_delta);
        const Vec2 right_neighbor_delta=subtract(right_delta,endpoint_delta);
        if (!finite(left_point) || !finite(right_point) || !std::isfinite(station) ||
            !preserves_local_offset(left_point.x,host_endpoint.x,left_delta.x) ||
            !preserves_local_offset(left_point.y,host_endpoint.y,left_delta.y) ||
            !preserves_local_offset(right_point.x,host_endpoint.x,right_delta.x) ||
            !preserves_local_offset(right_point.y,host_endpoint.y,right_delta.y) ||
            !preserves_local_offset(left_point.x,neighbor_endpoint.x,left_neighbor_delta.x) ||
            !preserves_local_offset(left_point.y,neighbor_endpoint.y,left_neighbor_delta.y) ||
            !preserves_local_offset(right_point.x,neighbor_endpoint.x,right_neighbor_delta.x) ||
            !preserves_local_offset(right_point.y,neighbor_endpoint.y,right_neighbor_delta.y))
            return std::nullopt;
        if (junction.at_start ? station>=intervals.front().second-tolerance
                              : station<=intervals.back().first+tolerance)
            return std::nullopt;
        return EndpointJoin{left_point,right_point,station,station};
    }

    if (intervals.empty()) return std::nullopt;
    const double host_side_relation=junction.at_start==neighbor_at_start ? -1.0 : 1.0;
    const Vec2 host_normal=left_normal(host_direction);
    const Vec2 neighbor_normal=left_normal(neighbor_direction);
    const double host_half=thickness*.5;
    const double neighbor_half=junction.neighbor_thickness*.5;
    const double maximum_width=std::max(thickness,junction.neighbor_thickness);
    const double first_station=junction.at_start ? 0 : host_info.length;
    std::array<Vec2,2> points{};
    std::array<double,2> host_parameters{};
    for (std::size_t i=0; i<2; ++i) {
        const double side=i==0 ? 1.0 : -1.0;
        const Vec2 host_offset=multiply(host_normal,side*host_half);
        const Vec2 neighbor_offset=multiply(neighbor_normal,
            side*host_side_relation*neighbor_half);
        const Vec2 line_delta=subtract(add(endpoint_delta,neighbor_offset),host_offset);
        const double host_parameter=cross(line_delta,neighbor_direction)/direction_cross;
        const double neighbor_parameter=cross(line_delta,host_direction)/direction_cross;
        const Vec2 point_delta=add(host_offset,multiply(host_direction,host_parameter));
        const Vec2 neighbor_point_delta=add(add(endpoint_delta,neighbor_offset),
            multiply(neighbor_direction,neighbor_parameter));
        if (!finite(host_offset) || !finite(neighbor_offset) || !finite(line_delta) ||
            !std::isfinite(host_parameter) || !std::isfinite(neighbor_parameter) ||
            !finite(point_delta) || !finite(neighbor_point_delta) ||
            std::hypot(point_delta.x-neighbor_point_delta.x,
                       point_delta.y-neighbor_point_delta.y)>tolerance)
            return std::nullopt;

        const Vec2 world_point=add(host_endpoint,point_delta);
        const Vec2 expected_neighbor_delta=subtract(point_delta,endpoint_delta);
        if (!finite(world_point) ||
            !preserves_local_offset(world_point.x,host_endpoint.x,point_delta.x) ||
            !preserves_local_offset(world_point.y,host_endpoint.y,point_delta.y) ||
            !preserves_local_offset(world_point.x,neighbor_endpoint.x,expected_neighbor_delta.x) ||
            !preserves_local_offset(world_point.y,neighbor_endpoint.y,expected_neighbor_delta.y))
            return std::nullopt;

        const double host_station=first_station+host_parameter;
        const double ratio=std::max({std::abs(host_parameter)/maximum_width,
            std::abs(neighbor_parameter)/maximum_width,
            std::hypot(point_delta.x,point_delta.y)/maximum_width,
            std::hypot(expected_neighbor_delta.x,expected_neighbor_delta.y)/maximum_width});
        if (!std::isfinite(host_station) || !std::isfinite(ratio) || ratio>miter_limit)
            return std::nullopt;
        points[i]=world_point;
        host_parameters[i]=host_station;
    }
    if (segment_length({points[0],points[1],0})<=tolerance) return std::nullopt;

    const double left_station=host_parameters[0];
    const double right_station=host_parameters[1];
    if (junction.at_start) {
        if (std::max(left_station,right_station)>=intervals.front().second-tolerance)
            return std::nullopt;
    } else {
        if (std::min(left_station,right_station)<=intervals.back().first+tolerance)
            return std::nullopt;
    }
    return EndpointJoin{points[0],points[1],left_station,right_station};
}
} // namespace

Vec2 point_at_host_station(const Segment& host, double station_metres) {
    return point_at(host,host_geometry(host),station_metres);
}

double quantize_host_station(double station_metres, double increment_metres) {
    require(std::isfinite(station_metres) && std::isfinite(increment_metres) &&
        increment_metres >= 0, "Hosted opening station quantization requires finite station and nonnegative increment");
    if (increment_metres == 0) return station_metres;
    const double lattice_station = station_metres/increment_metres;
    require(std::isfinite(lattice_station), "Hosted opening station quantization exceeds numeric range");
    const double result = std::round(lattice_station)*increment_metres;
    require(std::isfinite(result), "Hosted opening station quantization exceeds numeric range");
    return result;
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
    const auto intervals=remaining_wall_intervals(geometry.length,openings);
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
    for (const auto& [from,to] : intervals) append(from,to);
    for(const auto& edge:result) (void)segment_bounds(edge);
    return result;
}

WallPlanGeometry joined_wall_plan_geometry(const Segment& host,
    const std::vector<HostedOpening>& openings, double thickness,
    const std::vector<WallPlanJunction>& junctions) {
    WallPlanGeometry result;
    result.footprint=wall_plan_footprint(host,openings,thickness);
    result.strokes=result.footprint;
    if (result.footprint.empty() || host.sweep_radians!=0 || junctions.empty()) return result;

    const auto geometry=host_geometry(host);
    const auto intervals=remaining_wall_intervals(geometry.length,openings);
    if (intervals.empty()) return result;

    std::array<std::vector<const WallPlanJunction*>,2> candidates;
    for (const auto& junction : junctions)
        candidates[junction.at_start ? 0 : 1].push_back(&junction);
    std::array<std::optional<EndpointJoin>,2> joins;
    for (std::size_t endpoint=0; endpoint<candidates.size(); ++endpoint) {
        if (candidates[endpoint].size()!=1) continue;
        const auto& junction=*candidates[endpoint].front();
        joins[endpoint]=endpoint_join(host,geometry,thickness,junction,intervals);
    }

    // A pair of locally safe miters can still reverse a short shared face.
    // If that happens, preserve butt caps at both endpoints of this run.
    if (joins[0] && joins[1] && intervals.size()==1 &&
        (joins[1]->left_station-joins[0]->left_station<=tolerance ||
         joins[1]->right_station-joins[0]->right_station<=tolerance)) {
        joins[0].reset();
        joins[1].reset();
    }
    if (!joins[0] && !joins[1]) return result;

    if (joins[0]) {
        result.footprint[0].start=joins[0]->left_point;
        result.footprint[2].end=joins[0]->right_point;
        result.footprint[3]={joins[0]->right_point,joins[0]->left_point,0};
        result.joined_start=true;
    }
    if (joins[1]) {
        const std::size_t last=(intervals.size()-1)*4;
        result.footprint[last].end=joins[1]->left_point;
        result.footprint[last+2].start=joins[1]->right_point;
        result.footprint[last+1]={joins[1]->left_point,joins[1]->right_point,0};
        result.joined_end=true;
    }

    try {
        for (const auto& edge : result.footprint) (void)segment_bounds(edge);
    } catch (const std::invalid_argument&) {
        // Any nonrepresentable output reverts atomically to the legacy geometry.
        result.footprint=wall_plan_footprint(host,openings,thickness);
        result.strokes=result.footprint;
        result.joined_start=false;
        result.joined_end=false;
        return result;
    }

    result.strokes.clear();
    const std::size_t start_cap=result.joined_start ? 3 : result.footprint.size();
    const std::size_t end_cap=result.joined_end ? (intervals.size()-1)*4+1 : result.footprint.size();
    for (std::size_t i=0; i<result.footprint.size(); ++i) {
        if (i!=start_cap && i!=end_cap) result.strokes.push_back(result.footprint[i]);
    }
    return result;
}
} // namespace sketch
