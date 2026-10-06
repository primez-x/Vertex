#include "sketch/stair_semantics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
constexpr double eps=1e-7, max_dimension=1e6, max_coordinate=1e9;
constexpr std::size_t max_risers=10000, max_flights=256, max_posts=10000;
[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
void positive(double x) { if(!std::isfinite(x)||x<=eps||x>max_dimension) invalid("Stair/railing dimension must be positive and bounded"); }
void coordinate(Vec3 p) { for(double x:{p.x,p.y,p.z}) if(!std::isfinite(x)||std::abs(x)>max_coordinate) invalid("Stair coordinate must be finite and bounded"); }
void angle(double x) { if(!std::isfinite(x)||std::abs(x)>1e6) invalid("Stair angle must be finite and bounded"); }
void identity(std::string_view id) {
    if(id.empty()||id.size()>128||!std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':';})) invalid("Stair/railing identity must be portable and bounded");
}
Vec3 local(Vec3 base,double yaw,double x,double y,double z=0) { Vec3 p{base.x+std::cos(yaw)*x-std::sin(yaw)*y,base.y+std::sin(yaw)*x+std::cos(yaw)*y,base.z+z}; coordinate(p); return p; }
StairPolygon rectangle(Vec3 base,double yaw,double x0,double y0,double x1,double y1,double z) {return {local(base,yaw,x0,y0,z),local(base,yaw,x1,y0,z),local(base,yaw,x1,y1,z),local(base,yaw,x0,y1,z)};}
bool overlap(const StairPolygon& a,const StairPolygon& b) {
    for(const auto* poly:{&a,&b}) for(std::size_t i=0;i<4;++i) {
        const auto& p=(*poly)[i]; const auto& q=(*poly)[(i+1)%4]; const auto nx=q.y-p.y, ny=p.x-q.x, norm=std::hypot(nx,ny);
        double amin=1e300,amax=-1e300,bmin=1e300,bmax=-1e300;
        for(auto v:a){const auto t=(v.x*nx+v.y*ny)/norm;amin=std::min(amin,t);amax=std::max(amax,t);}
        for(auto v:b){const auto t=(v.x*nx+v.y*ny)/norm;bmin=std::min(bmin,t);bmax=std::max(bmax,t);}
        if(std::min(amax,bmax)-std::max(amin,bmin)<=eps) return false;
    }
    return true;
}
const Json& field(const Json& j,const char* key) {if(!j.is_object()||!j.contains(key)) invalid("Stair/railing required field missing");return j.at(key);}
double number(const Json& j,const char* key) {const auto& v=field(j,key);if(!v.is_number())invalid("Stair/railing field must be numeric");const auto x=v.get<double>();if(!std::isfinite(x))invalid("Stair/railing field must be finite");return x;}
std::string text(const Json& j,const char* key) {const auto& v=field(j,key);if(!v.is_string())invalid("Stair/railing field must be a string");return v.get<std::string>();}
std::size_t count(const Json& j,const char* key) {const auto& v=field(j,key);if(!v.is_number_integer())invalid("Stair count must be an integer");if(v.is_number_unsigned()){const auto n=v.get<std::uint64_t>();if(n==0||n>max_risers)invalid("Stair count outside range");return static_cast<std::size_t>(n);}const auto n=v.get<std::int64_t>();if(n<=0||n>static_cast<std::int64_t>(max_risers))invalid("Stair count outside range");return static_cast<std::size_t>(n);}
Vec3 point(const Json& j,const char* key) {const auto& p=field(j,key);if(!p.is_array()||p.size()!=3)invalid("Stair point must have three coordinates"); Json wrapped={{"x",p[0]},{"y",p[1]},{"z",p[2]}};return {number(wrapped,"x"),number(wrapped,"y"),number(wrapped,"z")};}
Json json_point(Vec3 p){return Json::array({p.x,p.y,p.z});}
void version_form(const Json& p,int version,const char* form) {const auto& v=field(p,"version");if(!v.is_number_integer()||v!=version||text(p,"form")!=form)invalid("Unsupported stair/railing version or form");}
std::string turn_name(StairTurn turn) {switch(turn){case StairTurn::straight:return "straight";case StairTurn::left_quarter:return "left_quarter";case StairTurn::right_quarter:return "right_quarter";case StairTurn::left_half:return "left_half";case StairTurn::right_half:return "right_half";}invalid("Invalid stair turn");}
StairTurn turn_value(const std::string& turn) {for(auto t:{StairTurn::straight,StairTurn::left_quarter,StairTurn::right_quarter,StairTurn::left_half,StairTurn::right_half})if(turn_name(t)==turn)return t;invalid("Invalid stair turn");}
void validate_connection(const StairLevelConnection& c) {
    identity(c.graph_entity_id);
    for(const auto* id:{&c.link_id,&c.lower_level_id,&c.upper_level_id}){
        if(id->empty()||id->size()>256||id->find('\0')!=std::string::npos)invalid("Invalid stair level identity");
        try {(void)Json(*id).dump();}catch(const Json::exception&){invalid("Invalid UTF-8 stair level identity");}
    }
    if(c.lower_level_id==c.upper_level_id)invalid("Stair levels must differ");
}
void basic_stair(const StairFlight& s) {
    coordinate(s.base_position);angle(s.orientation_radians);positive(s.total_rise);positive(s.going);positive(s.width);
    if(s.riser_count==0||s.riser_count>max_risers)invalid("Stair riser count outside range");
    positive(s.total_rise/static_cast<double>(s.riser_count));positive(s.going*static_cast<double>(s.riser_count));
    if(s.level_connection)validate_connection(*s.level_connection);
    if(s.top_landing){positive(s.top_landing->depth);positive(s.top_landing->thickness);if(s.top_landing->thickness>s.total_rise)invalid("Top landing extends below stair base");}
    if(s.flights.empty()){if(!s.landings.empty())invalid("Connecting landings require flights");return;}
    if(s.flights.size()>max_flights||s.landings.size()!=s.flights.size()-1)invalid("Stair topology size invalid");
    std::set<std::string> ids;std::size_t sum=0;
    for(const auto& f:s.flights){identity(f.id);if(!ids.insert(f.id).second||f.riser_count==0||f.riser_count>max_risers-sum)invalid("Invalid flight identity/count");sum+=f.riser_count;}
    if(sum!=s.riser_count)invalid("Aggregate riser count differs from ordered flight sum");
    double rise=0;const auto h=s.total_rise/static_cast<double>(sum);
    for(std::size_t i=0;i<s.landings.size();++i){const auto& l=s.landings[i];identity(l.id);if(!ids.insert(l.id).second)invalid("Duplicate stair child identity");positive(l.depth);positive(l.thickness);rise+=static_cast<double>(s.flights[i].riser_count)*h;if(l.thickness>rise)invalid("Connecting landing extends below stair base");(void)turn_name(l.turn);if(l.turn!=StairTurn::straight&&l.depth+eps<s.width)invalid("Turning landing depth must accommodate full flight width");if(!std::isfinite(l.return_gap)||l.return_gap<0||l.return_gap>max_dimension)invalid("Invalid stair return gap");if(l.return_gap!=0&&l.turn!=StairTurn::left_half&&l.turn!=StairTurn::right_half)invalid("Return gap requires a half turn");}
}
}

StairLayout derive_stair_layout(const StairFlight& s) {
    basic_stair(s); StairLayout result; Vec3 base=s.base_position; double yaw=s.orientation_radians;
    const auto h=s.total_rise/static_cast<double>(s.riser_count);
    const std::vector<StairFlightRecord> legacy={{s.id,s.riser_count}};
    const auto& flights=s.flights.empty()?legacy:s.flights;
    for(std::size_t i=0;i<flights.size();++i){const auto& record=flights[i];
        StairFlightLayout f;f.id=record.id;f.base_position=base;f.orientation_radians=yaw;f.run=static_cast<double>(record.riser_count)*s.going;f.rise=static_cast<double>(record.riser_count)*h;f.riser_height=h;f.end_position=local(base,yaw,f.run,0,f.rise);f.footprint=rectangle(base,yaw,0,0,f.run,s.width,0);
        f.treads.reserve(record.riser_count);
        for(std::size_t t=0;t<record.riser_count;++t){const double x=static_cast<double>(t)*s.going,z=static_cast<double>(t+1)*h;const auto polygon=rectangle(base,yaw,x,0,x+s.going,s.width,z);f.treads.push_back({polygon,polygon[0],polygon[3],base.z+z});}
        result.flights.push_back(std::move(f));base=result.flights.back().end_position;
        if(i<s.landings.size()){const auto& l=s.landings[i]; double ymin=0,ymax=s.width,nx=l.depth,ny=0,turn=0;
            switch(l.turn){case StairTurn::straight:break;case StairTurn::left_quarter:nx=l.depth;ny=s.width;turn=std::numbers::pi/2;break;case StairTurn::right_quarter:nx=l.depth-s.width;turn=-std::numbers::pi/2;break;case StairTurn::left_half:ymax=2*s.width+l.return_gap;nx=0;ny=ymax;turn=std::numbers::pi;break;case StairTurn::right_half:ymin=-s.width-l.return_gap;nx=0;ny=-l.return_gap;turn=-std::numbers::pi;break;}
            result.landings.push_back({l.id,rectangle(base,yaw,0,ymin,l.depth,ymax,0),base.z,l.thickness});base=local(base,yaw,nx,ny);
            // This is a derived frame, not a rewrite of the authored angle.
            // Keep accumulated turns bounded even at the authored yaw limit.
            yaw=std::remainder(std::remainder(yaw,2*std::numbers::pi)+turn,
                               2*std::numbers::pi);
        }
    }
    if(s.top_landing)result.landings.push_back({{},rectangle(base,yaw,0,0,s.top_landing->depth,s.width,0),base.z,s.top_landing->thickness});
    // Check actual tread volumes rather than flight bounding boxes, which
    // would wrongly reject a landing safely above an earlier lower tread.
    struct Cell {StairPolygon p;double low,high;}; std::vector<Cell> cells;
    for(const auto& f:result.flights)for(const auto& t:f.treads)cells.push_back({t.footprint,f.base_position.z,t.elevation});
    for(const auto& l:result.landings){for(const auto& c:cells)if(std::min(c.high,l.elevation)-std::max(c.low,l.elevation-l.thickness)>eps&&overlap(c.p,l.footprint))invalid("Stair landing overlaps stair solid");cells.push_back({l.footprint,l.elevation-l.thickness,l.elevation});}
    // Ordered flights have disjoint vertical intervals, so their positive
    // volumes cannot overlap; landing slabs are the remaining overlap risk.
    return result;
}
void validate_stair(const StairFlight& s){(void)derive_stair_layout(s);}
std::vector<std::string> stair_child_ids(const StairFlight& s){validate_stair(s);std::vector<std::string> ids;for(const auto& f:s.flights)ids.push_back(f.id);for(const auto& l:s.landings)ids.push_back(l.id);return ids;}
void validate_railing(const Railing& r) {
    positive(r.height);
    positive(r.thickness);
    positive(r.post_spacing);
    if (r.thickness >= r.height - eps) invalid("Railing thickness must be below height");
    if (!r.host) {
        coordinate(r.base_position);
        angle(r.orientation_radians);
        positive(r.length);
        // Retain the v1 builder's interior-post convention exactly, while
        // checking the floating result before any bounded integer conversion.
        const auto interior = std::floor((r.length - eps) / r.post_spacing);
        if (!std::isfinite(interior) || interior > static_cast<double>(max_posts - 2))
            invalid("Too many railing posts");
        return;
    }
    const auto& host = *r.host;
    identity(host.stair_id);
    identity(host.flight_id);
    if (host.side != StairRailingSide::left && host.side != StairRailingSide::right)
        invalid("Invalid railing side");
    if (!std::isfinite(host.start_fraction) || !std::isfinite(host.end_fraction) ||
        host.start_fraction < 0 || host.end_fraction > 1 ||
        host.start_fraction >= host.end_fraction)
        invalid("Railing stations must be ordered normalized fractions");
}

HostedRailingLayout derive_hosted_railing_layout(const Railing& r,const StairFlight& s){
    validate_railing(r);if(!r.host||r.host->stair_id!=s.id)invalid("Railing requires its current host stair");const auto layout=derive_stair_layout(s);const auto found=std::find_if(layout.flights.begin(),layout.flights.end(),[&](const auto& f){return f.id==r.host->flight_id;});if(found==layout.flights.end())invalid("Hosted railing flight no longer exists");const auto& f=*found;
    if(r.thickness>=s.going-eps||r.thickness>=s.width-eps)invalid("Railing posts do not fit the tread");const auto half=r.thickness/2;const auto domain=f.run-r.thickness;const auto start=half+r.host->start_fraction*domain,end=half+r.host->end_fraction*domain;
    if (end - start <= eps)
        invalid("Hosted railing stations must span a positive supported run");
    const auto slope=f.treads.size()==1?0:f.riser_height/s.going;const auto factor=std::hypot(1.0,slope);const auto spacing=r.post_spacing/factor;
    if(!std::isfinite(spacing)||std::ceil((end-start)/spacing)>static_cast<double>(max_posts-1))invalid("Too many hosted railing posts");
    const auto supported=[&](double x){auto t=std::min(static_cast<std::size_t>(std::floor(x/s.going)),f.treads.size()-1);const auto low=static_cast<double>(t)*s.going+half,high=static_cast<double>(t+1)*s.going-half;return x>=low-eps&&x<=high+eps;};
    if(!supported(start)||!supported(end))invalid("Railing endpoints require full tread support");
    HostedRailingLayout result;result.orientation_radians=f.orientation_radians;const auto y=r.host->side==StairRailingSide::left?s.width-half:half;
    const auto rail_point=[&](double x){return local(f.base_position,f.orientation_radians,x,y,f.riser_height+x*slope+r.height);};result.rail_start=rail_point(start);result.rail_end=rail_point(end);
    const auto post=[&](double x){const auto t=std::min(static_cast<std::size_t>(std::floor(x/s.going)),f.treads.size()-1);const auto elevation=static_cast<double>(t+1)*f.riser_height;result.posts.push_back({local(f.base_position,f.orientation_radians,x,y,elevation),rail_point(x)});};post(start);
    // Geometry tolerance identifies a support boundary, but it must not
    // enlarge the authored maximum spacing. Allow only floating roundoff in
    // the actual 3D pitch distance, including the final endpoint interval.
    const auto spacing_roundoff = 32 * std::numeric_limits<double>::epsilon()
        * std::max(1.0, r.post_spacing);
    const auto within_spacing = [&](double from, double to) {
        return (to - from) * factor <= r.post_spacing + spacing_roundoff;
    };
    double x = start;
    while (!within_spacing(x, end)) {
        double next = std::min(end, x + spacing);
        const auto t = std::min(static_cast<std::size_t>(std::floor(next / s.going)),
                                f.treads.size() - 1);
        const auto low = static_cast<double>(t) * s.going + half;
        const auto high = static_cast<double>(t + 1) * s.going - half;
        if (next < low) {
            // An exactly reachable boundary can round a few ULPs below low.
            // Clamp to it before measuring spacing rather than jumping back
            // to the previous tread, which can strand an otherwise valid rail.
            next = low - next <= eps ? low : static_cast<double>(t) * s.going - half;
        } else if (next > high) {
            next = high;
        }
        if (next <= x + eps || next > end || !supported(next) ||
            !within_spacing(x, next) || result.posts.size() >= max_posts - 1)
            invalid("Post spacing cannot bridge unsupported tread boundary");
        post(next);
        x = next;
    }
    post(end);
    return result;
}

Json encode_stair_properties(const StairFlight& s){validate_stair(s);Json p={{"version",s.flights.empty()?1:2},{"form",s.flights.empty()?"straight_stair_flight":"multi_flight_stair"},{"base_position_m",json_point(s.base_position)},{"orientation_rad",s.orientation_radians},{"riser_count",s.riser_count},{"total_rise_m",s.total_rise},{"going_m",s.going},{"width_m",s.width},{"top_landing",nullptr}};
    if(s.top_landing)p["top_landing"]={{"depth_m",s.top_landing->depth},{"thickness_m",s.top_landing->thickness}};
    if(s.level_connection){const auto& c=*s.level_connection;p["level_connection"]={{"version",1},{"graph_id",c.graph_entity_id},{"link_id",c.link_id},{"lower_level_id",c.lower_level_id},{"upper_level_id",c.upper_level_id}};}
    if(!s.flights.empty()){p["flights"]=Json::array();for(const auto& f:s.flights)p["flights"].push_back({{"id",f.id},{"riser_count",f.riser_count}});p["landings"]=Json::array();for(const auto& l:s.landings)p["landings"].push_back({{"id",l.id},{"depth_m",l.depth},{"thickness_m",l.thickness},{"turn",turn_name(l.turn)},{"return_gap_m",l.return_gap}});}return p;
}
StairFlight decode_stair_properties(std::string_view id,const Json& p){identity(id);const auto& version=field(p,"version");const bool v2=version.is_number_integer()&&version==2;version_form(p,v2?2:1,v2?"multi_flight_stair":"straight_stair_flight");if(!v2&&(p.contains("flights")||p.contains("landings")))invalid("Stair topology requires version 2");StairFlight s{std::string(id),point(p,"base_position_m"),number(p,"orientation_rad"),count(p,"riser_count"),number(p,"total_rise_m"),number(p,"going_m"),number(p,"width_m")};
    const auto& top=field(p,"top_landing");if(!top.is_null())s.top_landing=StairLanding{number(top,"depth_m"),number(top,"thickness_m")};
    if(p.contains("level_connection")){const auto& c=p.at("level_connection");const auto& v=field(c,"version");if(!v.is_number_integer()||v!=1)invalid("Invalid stair level connection version");s.level_connection=StairLevelConnection{text(c,"graph_id"),text(c,"link_id"),text(c,"lower_level_id"),text(c,"upper_level_id")};}
    if(v2){const auto& fs=field(p,"flights");const auto& ls=field(p,"landings");if(!fs.is_array()||fs.empty()||fs.size()>max_flights||!ls.is_array()||ls.size()!=fs.size()-1)invalid("Invalid stair topology arrays");for(const auto& f:fs)s.flights.push_back({text(f,"id"),count(f,"riser_count")});for(const auto& l:ls)s.landings.push_back({text(l,"id"),number(l,"depth_m"),number(l,"thickness_m"),turn_value(text(l,"turn")),number(l,"return_gap_m")});}validate_stair(s);return s;
}
Json encode_railing_properties(const Railing& r){validate_railing(r);Json p={{"version",r.host?2:1},{"form",r.host?"stair_flight_railing":"straight_railing"},{"height_m",r.height},{"thickness_m",r.thickness},{"post_spacing_m",r.post_spacing}};if(r.host){const auto& h=*r.host;p["host"]={{"stair_id",h.stair_id},{"flight_id",h.flight_id},{"side",h.side==StairRailingSide::left?"left":"right"},{"start_fraction",h.start_fraction},{"end_fraction",h.end_fraction}};}else{p["base_position_m"]=json_point(r.base_position);p["orientation_rad"]=r.orientation_radians;p["length_m"]=r.length;}return p;}
Railing decode_railing_properties(std::string_view id,const Json& p){identity(id);const auto& v=field(p,"version");const bool v2=v.is_number_integer()&&v==2;version_form(p,v2?2:1,v2?"stair_flight_railing":"straight_railing");Railing r;r.id=id;r.height=number(p,"height_m");r.thickness=number(p,"thickness_m");r.post_spacing=number(p,"post_spacing_m");if(v2){if(p.contains("base_position_m")||p.contains("orientation_rad")||p.contains("length_m"))invalid("Hosted rail cannot persist independent coordinates");const auto& h=field(p,"host");const auto side=text(h,"side");if(side!="left"&&side!="right")invalid("Invalid railing host side");r.host=StairRailingHost{text(h,"stair_id"),text(h,"flight_id"),side=="left"?StairRailingSide::left:StairRailingSide::right,number(h,"start_fraction"),number(h,"end_fraction")};}else{if(p.contains("host"))invalid("Railing host requires version 2");r.base_position=point(p,"base_position_m");r.orientation_radians=number(p,"orientation_rad");r.length=number(p,"length_m");}validate_railing(r);return r;}
} // namespace sketch
