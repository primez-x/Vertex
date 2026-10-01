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
    if (value.kind != DoorOperationKind::hinged && value.kind != DoorOperationKind::double_hinged &&
        value.kind != DoorOperationKind::sliding)
        throw std::invalid_argument("Unsupported door operation kind");
    if (!std::isfinite(value.slide_fraction) || value.slide_fraction < 0 || value.slide_fraction > 1 ||
        (value.kind != DoorOperationKind::sliding && value.slide_fraction != 0))
        throw std::invalid_argument("Door sliding travel must be in [0,1] and zero for hinged doors");
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
    if(!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version")!=1 && value.at("version")!=2))
        throw std::invalid_argument("Unsupported door operation");
    const bool extended=value.at("version")==2;
    if(value.size()!=(extended?6u:4u) || !value.contains("hinge") || !value.contains("side") ||
        !value.contains("angle_degrees") || !value.at("hinge").is_string() || !value.at("side").is_string() ||
        (extended && (!value.contains("kind") || !value.at("kind").is_string() ||
                      !value.contains("slide_fraction") || !value.at("slide_fraction").is_number())))
        throw std::invalid_argument("Door operation has missing, unknown, or invalid fields");
    const auto hinge=value.at("hinge").get<std::string>(),side=value.at("side").get<std::string>();
    if((hinge!="start" && hinge!="end") || (side!="left" && side!="right") || !value.at("angle_degrees").is_number())
        throw std::invalid_argument("Invalid door hinge or swing side");
    DoorOperation result{hinge=="end",side=="left",value.at("angle_degrees").get<double>()};
    if(extended) {
        const auto kind=value.at("kind").get<std::string>();
        if(kind=="hinged") result.kind=DoorOperationKind::hinged;
        else if(kind=="double_hinged") result.kind=DoorOperationKind::double_hinged;
        else if(kind=="sliding") result.kind=DoorOperationKind::sliding;
        else throw std::invalid_argument("Unsupported door operation kind");
        result.slide_fraction=value.at("slide_fraction").get<double>();
    }
    validate(result);
    return result;
}
nlohmann::json encode_door_operation(const DoorOperation& value) {
    validate(value);
    nlohmann::json result={{"version",1},{"hinge",value.hinge_at_end?"end":"start"},
        {"side",value.swing_left?"left":"right"},{"angle_degrees",value.angle_degrees}};
    if(value.kind!=DoorOperationKind::hinged) {
        result["version"]=2;
        result["kind"]=value.kind==DoorOperationKind::double_hinged?"double_hinged":"sliding";
        result["slide_fraction"]=value.slide_fraction;
    }
    return result;
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
    const Vec2 midpoint{(start.x+end.x)*0.5,(start.y+end.y)*0.5};
    if(operation.kind==DoorOperationKind::sliding) {
        const double dx=end.x-start.x,dy=end.y-start.y;
        const double side=operation.swing_left?1.0:-1.0;
        const Vec2 normal{-dy*0.01*side,dx*0.01*side};
        const auto translate=[](Vec2 p,Vec2 delta){return Vec2{p.x+delta.x,p.y+delta.y};};
        const double travel=(operation.hinge_at_end?-1.0:1.0)*operation.slide_fraction*0.5;
        const Vec2 delta{normal.x+dx*travel,normal.y+dy*travel};
        const auto moving_start=operation.hinge_at_end?midpoint:start;
        const auto moving_end=operation.hinge_at_end?end:midpoint;
        const auto fixed_start=operation.hinge_at_end?start:midpoint;
        const auto fixed_end=operation.hinge_at_end?midpoint:end;
        return {{translate(moving_start,delta),translate(moving_end,delta),0},
                {translate(fixed_start,{-normal.x,-normal.y}),translate(fixed_end,{-normal.x,-normal.y}),0}};
    }
    Boundary result;
    const int count=operation.kind==DoorOperationKind::double_hinged?2:1;
    for(int i=0;i<count;++i) {
        const bool at_end=i==0?operation.hinge_at_end:!operation.hinge_at_end;
        const auto hinge=at_end?end:start;
        const auto closed=count==2?midpoint:(at_end?start:end);
        const double angle=operation.angle_degrees*std::numbers::pi/180*(operation.swing_left?1:-1)*(at_end?-1:1);
        const double dx=closed.x-hinge.x,dy=closed.y-hinge.y;
        const Vec2 opened{hinge.x+dx*std::cos(angle)-dy*std::sin(angle),hinge.y+dx*std::sin(angle)+dy*std::cos(angle)};
        result.push_back({hinge,opened,0});
        result.push_back(arc_from_chord_angle(closed,opened,angle));
    }
    return result;
}
} // namespace sketch
