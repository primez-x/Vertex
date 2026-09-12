#include "sketch/door_operation.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sketch {
namespace {
void validate(const DoorOperation& value) {
    if(!std::isfinite(value.angle_degrees) || value.angle_degrees <= 0 || value.angle_degrees > 180)
        throw std::invalid_argument("Door swing angle must be greater than zero and at most 180 degrees");
}
Vec2 point(const Segment& line,double fraction) {
    if(line.sweep_radians==0) return {std::lerp(line.start.x,line.end.x,fraction),std::lerp(line.start.y,line.end.y,fraction)};
    const double dx=line.end.x-line.start.x,dy=line.end.y-line.start.y;
    const double factor=0.5/std::tan(line.sweep_radians/2);
    const Vec2 center{line.start.x+dx/2-dy*factor,line.start.y+dy/2+dx*factor};
    const double angle=line.sweep_radians*fraction,c=std::cos(angle),s=std::sin(angle);
    return {center.x+(line.start.x-center.x)*c-(line.start.y-center.y)*s,
        center.y+(line.start.x-center.x)*s+(line.start.y-center.y)*c};
}
}
DoorOperation decode_door_operation(const nlohmann::json& value) {
    if(!value.is_object() || value.size()!=4 || !value.at("version").is_number_integer() || value.at("version")!=1)
        throw std::invalid_argument("Unsupported door operation");
    const auto hinge=value.at("hinge").get<std::string>(),side=value.at("side").get<std::string>();
    if((hinge!="start" && hinge!="end") || (side!="left" && side!="right") || !value.at("angle_degrees").is_number())
        throw std::invalid_argument("Invalid door hinge or swing side");
    DoorOperation result{hinge=="end",side=="left",value.at("angle_degrees").get<double>()};
    validate(result);
    return result;
}
nlohmann::json encode_door_operation(const DoorOperation& value) {
    validate(value);
    return {{"version",1},{"hinge",value.hinge_at_end?"end":"start"},
        {"side",value.swing_left?"left":"right"},{"angle_degrees",value.angle_degrees}};
}
Boundary door_plan_symbol(const Segment& host,double offset,double width,const DoorOperation& operation) {
    validate(operation);
    const auto length=segment_length(host);
    if(!std::isfinite(host.sweep_radians) || std::abs(host.sweep_radians)>=2*std::numbers::pi ||
        !std::isfinite(length) || length<=default_geometry_tolerance_metres || !std::isfinite(offset) || offset<0 ||
        !std::isfinite(width) || width<=default_geometry_tolerance_metres || !std::isfinite(offset+width) ||
        offset+width>length+default_geometry_tolerance_metres)
        throw std::invalid_argument("Door dimensions must fit the host baseline");
    const auto start=point(host,offset/length),end=point(host,std::min(1.0,(offset+width)/length));
    const auto hinge=operation.hinge_at_end?end:start,closed=operation.hinge_at_end?start:end;
    const double angle=operation.angle_degrees*std::numbers::pi/180*(operation.swing_left?1:-1)*(operation.hinge_at_end?-1:1);
    const double dx=closed.x-hinge.x,dy=closed.y-hinge.y;
    const Vec2 opened{hinge.x+dx*std::cos(angle)-dy*std::sin(angle),hinge.y+dx*std::sin(angle)+dy*std::cos(angle)};
    const auto arc=arc_from_chord_angle(closed,opened,angle);
    return {{hinge,opened,0},arc};
}
} // namespace sketch
