#include "sketch/stair_semantics.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(double a, double b) { require(std::abs(a-b)<1e-9, "independent layout expectation"); }
template<class F> void reject(F f) { try { f(); } catch(const std::invalid_argument&) { return; } throw std::runtime_error("invalid input accepted"); }
sketch::StairFlight stair() {
    sketch::StairFlight s{"stair", {0,0,0}, 0, 8, 1.6, 0.3, 1.0};
    s.flights={{"lower",4},{"upper",4}};
    s.landings={{"turn",1.2,0.15,sketch::StairTurn::left_quarter,0}};
    return s;
}
void layouts() {
    using namespace sketch;
    auto s=stair(); auto l=derive_stair_layout(s);
    near(l.flights[0].run,1.2); near(l.flights[0].rise,0.8);
    near(l.flights[1].base_position.x,2.4); near(l.flights[1].base_position.y,1.0);
    near(l.flights[1].base_position.z,0.8); near(l.flights[1].end_position.y,2.2);
    near(l.flights[0].treads[0].elevation,0.2); near(l.flights[0].treads.back().footprint[1].x,1.2);
    near(l.landings[0].footprint[2].x,2.4); near(l.landings[0].footprint[2].y,1.0);
    s.landings[0].turn=StairTurn::right_quarter; l=derive_stair_layout(s);
    near(l.flights[1].base_position.x,1.4); near(l.flights[1].end_position.y,-1.2);
    s.landings[0].turn=StairTurn::left_half; s.landings[0].return_gap=0.2; l=derive_stair_layout(s);
    near(l.flights[1].base_position.x,1.2); near(l.flights[1].base_position.y,2.2);
    near(l.landings[0].footprint[2].y,2.2); near(l.flights[1].end_position.x,0.0);
    s.landings[0].turn=StairTurn::right_half; l=derive_stair_layout(s);
    near(l.flights[1].base_position.y,-0.2); near(l.landings[0].footprint[0].y,-1.2);
    s.landings[0].turn=StairTurn::straight; s.landings[0].return_gap=0; l=derive_stair_layout(s);
    near(l.flights[1].base_position.x,2.4); near(l.flights[1].base_position.y,0);
    s=stair(); s.orientation_radians=1'000'000.0; l=derive_stair_layout(s);
    near(l.flights[0].orientation_radians,s.orientation_radians);
    require(std::abs(l.flights[1].orientation_radians)<4,"outgoing derived yaw stays bounded");
    near(std::cos(l.flights[1].orientation_radians),-std::sin(s.orientation_radians));
    near(std::sin(l.flights[1].orientation_radians),std::cos(s.orientation_radians));
    require(encode_stair_properties(s).at("orientation_rad")==1'000'000.0,
            "extreme authored yaw remains unchanged in persistence");
}
void failures_and_codecs() {
    using namespace sketch; auto s=stair();
    const auto p=encode_stair_properties(s); require(p.at("version")==2,"v2 stair schema");
    require(encode_stair_properties(decode_stair_properties("stair",p))==p,"pure canonical roundtrip");
    auto bad=p; bad["riser_count"]=9; reject([&]{decode_stair_properties("stair",bad);});
    bad=p; bad["flights"][0]["riser_count"]=-1; reject([&]{decode_stair_properties("stair",bad);});
    bad=p; bad["version"]=1; reject([&]{decode_stair_properties("stair",bad);});
    s.riser_count=9; reject([&]{derive_stair_layout(s);}); s=stair();
    s.landings[0].id="lower"; reject([&]{derive_stair_layout(s);}); s=stair();
    s.landings[0].depth=0.5; reject([&]{derive_stair_layout(s);}); s=stair();
    s.landings[0].thickness=0.9; reject([&]{derive_stair_layout(s);}); s=stair();
    s.going=std::numeric_limits<double>::infinity(); reject([&]{derive_stair_layout(s);});
    StairFlight legacy{"old",{},0,3,0.6,0.3,1.0}; auto old=encode_stair_properties(legacy);
    require(old.at("version")==1,"legacy version retained");
    near(derive_stair_layout(decode_stair_properties("old",old)).flights[0].run,0.9);
    old["landings"]=nlohmann::json::array(); reject([&]{decode_stair_properties("old",old);});
    // Four left turns return to the first footprint. A thick top landing
    // would intersect the first connecting slab despite valid local seams.
    StairFlight loop{"loop",{},0,10,2.0,0.3,1.0};
    loop.flights={{"a",2},{"b",2},{"c",2},{"d",2},{"e",2}};
    loop.landings={{"ab",1.2,0.15,StairTurn::left_quarter,0},
                   {"bc",1.2,0.15,StairTurn::left_quarter,0},
                   {"cd",1.2,0.15,StairTurn::left_quarter,0},
                   {"de",1.2,0.15,StairTurn::left_quarter,0}};
    require(derive_stair_layout(loop).flights.size()==5,"stacked flights with disjoint volumes are allowed");
    loop.top_landing=StairLanding{1.2,2.0}; reject([&]{derive_stair_layout(loop);});
}
void rail_support() {
    using namespace sketch; auto s=stair();
    Railing r{"rail",{},0,0,0.9,0.04,0.45,StairRailingHost{"stair","lower",StairRailingSide::left,0,1}};
    const auto l=derive_hosted_railing_layout(r,s);
    near(l.rail_start.x,0.02); near(l.rail_end.x,1.18); near(l.rail_start.y,0.98);
    near(l.posts.front().base.z,0.2); near(l.posts.back().base.z,0.8);
    for(std::size_t i=0;i<l.posts.size();++i) {
        const auto& post=l.posts[i]; const auto index=static_cast<std::size_t>(std::floor(post.base.x/s.going));
        require(post.base.x-0.02>=index*s.going-1e-9 && post.base.x+0.02<=(index+1)*s.going+1e-9,"full post tread support");
        if(i) {const auto& prior=l.posts[i-1].top; require(std::hypot(post.top.x-prior.x,post.top.z-prior.z)<=r.post_spacing+1e-9,"3D spacing limit");}
    }
    auto encoded=encode_railing_properties(r); require(!encoded.contains("length_m") && !encoded.contains("base_position_m"),"host coordinates are derived");
    require(encode_railing_properties(decode_railing_properties("rail",encoded))==encoded,"hosted rail roundtrip");
    r.post_spacing=0.01; reject([&]{derive_hosted_railing_layout(r,s);}); r.post_spacing=0.45;
    r.post_spacing=1e-6; reject([&]{derive_hosted_railing_layout(r,s);}); r.post_spacing=0.45;
    r.host->end_fraction=1e-18; reject([&]{derive_hosted_railing_layout(r,s);});
    r.host->end_fraction=1e-9; reject([&]{derive_hosted_railing_layout(r,s);});
    r.host->end_fraction=1;
    r.host->start_fraction=0.24; reject([&]{derive_hosted_railing_layout(r,s);});
    r.host->start_fraction=0; r.host->flight_id="missing"; reject([&]{derive_hosted_railing_layout(r,s);});
    StairFlight one{"single",{},0,1,0.2,0.3,1.0}; one.flights={{"one",1}};
    r.host=StairRailingHost{"single","one",StairRailingSide::right,0,1};
    const auto horizontal=derive_hosted_railing_layout(r,one); near(horizontal.rail_start.z,1.1); near(horizontal.rail_end.z,1.1);
    encoded["version"]=1; reject([&]{decode_railing_properties("rail",encoded);});
}
void rail_spacing_at_exact_support_boundaries() {
    using namespace sketch;
    StairFlight s{"boundary-stair",{},0,4,0.6,0.2,1.0};
    s.flights={{"boundary-flight",4}};
    Railing r{"boundary-rail",{},0,0,0.9,0.04,0.05,
        StairRailingHost{"boundary-stair","boundary-flight",StairRailingSide::right,0,1}};
    const auto layout=derive_hosted_railing_layout(r,s);
    require(layout.posts.size()==20,"exact minimum bridge spacing has a feasible bounded post chain");
    near(layout.posts.front().base.x,0.02); near(layout.posts.back().base.x,0.78);
    // Horizontal gap .04 and pitch slope .75 require exactly .05 in 3D.
    // Every riser boundary must have posts on both actual tread support edges.
    for(double boundary:{0.2,0.4,0.6}) {
        bool before=false,after=false;
        for(const auto& post:layout.posts) {
            before=before||std::abs(post.base.x-(boundary-0.02))<1e-12;
            after=after||std::abs(post.base.x-(boundary+0.02))<1e-12;
        }
        require(before&&after,"boundary posts occupy both full-support edges");
    }
    for(std::size_t i=0;i<layout.posts.size();++i) {
        const auto& post=layout.posts[i];
        const auto index=static_cast<std::size_t>(std::floor(post.base.x/0.2));
        require(post.base.x-0.02>=index*0.2-1e-12 &&
                post.base.x+0.02<=(index+1)*0.2+1e-12,"boundary chain retains full square post support");
        if(i) {
            const auto& prior=layout.posts[i-1].top;
            require(std::hypot(post.top.x-prior.x,post.top.z-prior.z)<=0.05+1e-12,
                    "actual 3D pitch spacing never exceeds authored maximum");
        }
    }
    r.post_spacing=0.05-1e-9;
    reject([&]{derive_hosted_railing_layout(r,s);});
}
}
int main() { try {layouts();failures_and_codecs();rail_support();rail_spacing_at_exact_support_boundaries(); std::cout<<"Stair semantics tests passed\n"; return 0;} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
