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
void landing_edges_and_full_section_support() {
    using namespace sketch;
    // Independent expected exposure, with d=1.6,w=1,g=.4,W=2.4.
    for(auto turn:{StairTurn::straight,StairTurn::left_quarter,StairTurn::right_quarter,
                   StairTurn::left_half,StairTurn::right_half}) {
        for(double yaw:{0.0,0.37,1.7}) {
            auto s=stair();s.base_position={3,-2,4};s.orientation_radians=yaw;
            s.landings[0].depth=1.6;s.landings[0].turn=turn;
            s.landings[0].return_gap=(turn==StairTurn::left_half||turn==StairTurn::right_half)?0.4:0;
            for(std::size_t e=0;e<4;++e) {
                std::vector<StairLandingEdgeInterval> expected{{0,1}};
                if(e==3) {
                    expected.clear();
                    if(turn==StairTurn::left_half||turn==StairTurn::right_half) expected={{1.0/2.4,1.4/2.4}};
                }
                if(turn==StairTurn::straight&&e==1) expected.clear();
                if(turn==StairTurn::left_quarter&&e==2) expected={{1.0/1.6,1}};
                if(turn==StairTurn::right_quarter&&e==0) expected={{0,1-1.0/1.6}};
                StairLandingRailingHost h{"stair",StairLandingRole::connecting,"turn","lower","upper",e,0,1};
                const auto edge=derive_stair_landing_edge(s,h);
                require(edge.exposed_intervals.size()==expected.size(),"independent turn contact complement count");
                for(std::size_t i=0;i<expected.size();++i) {
                    near(edge.exposed_intervals[i].start_fraction,expected[i].start_fraction);
                    near(edge.exposed_intervals[i].end_fraction,expected[i].end_fraction);
                    // Stay away from analytic contact boundaries by a fixed
                    // authored margin, independent of reconstruction roundoff.
                    h.start_fraction=expected[i].start_fraction+0.02;
                    h.end_fraction=expected[i].end_fraction-0.02;
                    Railing r{"landing-rail",{},0,0,0.9,0.04,0.13};r.landing_host=h;
                    const auto rail=derive_hosted_railing_layout(r,s);
                    const auto polygon=derive_stair_layout(s).landings[0].footprint;
                    const auto length=std::hypot(edge.edge_end.x-edge.edge_start.x,edge.edge_end.y-edge.edge_start.y);
                    const auto ux=(edge.edge_end.x-edge.edge_start.x)/length,uy=(edge.edge_end.y-edge.edge_start.y)/length;
                    const auto station=[&](Vec3 p){return (p.x-edge.edge_start.x)*ux+(p.y-edge.edge_start.y)*uy;};
                    near(station(rail.posts.front().base),h.start_fraction*length+0.02);
                    near(station(rail.posts.back().base),h.end_fraction*length-0.02);
                    for(std::size_t j=0;j<rail.posts.size();++j) {
                        const auto base=rail.posts[j].base;near(base.z,4.8);
                        for(double along:{-0.02,0.02}) for(double across:{-0.02,0.02}) {
                            const Vec3 corner{base.x+along*ux-across*uy,base.y+along*uy+across*ux,base.z};
                            for(std::size_t side=0;side<4;++side) {
                                const auto a=polygon[side],b=polygon[(side+1)%4];
                                require((b.x-a.x)*(corner.y-a.y)-(b.y-a.y)*(corner.x-a.x)>=-1e-10,
                                    "every square post corner has actual landing surface support");
                            }
                            require(station(corner)>=h.start_fraction*length-1e-10&&
                                station(corner)<=h.end_fraction*length+1e-10,"outer post coverage limits are original-edge fractions");
                        }
                        if(j) {const auto prior=rail.posts[j-1].top;
                            require(std::hypot(prior.x-base.x,prior.y-base.y)<=r.post_spacing+1e-12,"horizontal maximum post spacing");}
                    }
                }
                if(expected.empty()) {
                    Railing r{"blocked",{},0,0,0.9,0.04,0.13};r.landing_host=h;
                    reject([&]{derive_hosted_railing_layout(r,s);});
                }
            }
        }
    }
    for(auto turn:{StairTurn::left_half,StairTurn::right_half}) {
        auto s=stair();s.landings[0].turn=turn;s.landings[0].return_gap=0;s.orientation_radians=.37;
        const StairLandingRailingHost h{"stair",StairLandingRole::connecting,"turn","lower","upper",3,0,1};
        require(derive_stair_landing_edge(s,h).exposed_intervals.empty(),"touching half-turn contacts union to fully blocked edge");
    }
}
void landing_codec_witnesses_and_refusals() {
    using namespace sketch;auto s=stair();s.top_landing=StairLanding{1.2,0.15};
    Railing r{"landing",{},0,0,0.9,0.04,0.3};
    r.landing_host=StairLandingRailingHost{"stair",StairLandingRole::connecting,"turn","lower","upper",0,0,1};
    const auto encoded=encode_railing_properties(r);
    require(encoded.at("version")==3&&encoded.at("form")=="stair_landing_railing","exact v3 landing codec");
    require(encode_railing_properties(decode_railing_properties(r.id,encoded))==encoded,"landing codec roundtrip");
    auto bad=encoded;bad["host"]["edge_index"]=1.5;reject([&]{decode_railing_properties(r.id,bad);});
    bad=encoded;bad["host"]["flight_id"]="lower";reject([&]{decode_railing_properties(r.id,bad);});
    r.host=StairRailingHost{"stair","lower",StairRailingSide::left,0,1};reject([&]{validate_railing(r);});r.host.reset();
    auto changed=s;std::swap(changed.flights[0],changed.flights[1]);reject([&]{derive_hosted_railing_layout(r,changed);});
    changed=s;changed.landings[0].id="replacement";reject([&]{derive_hosted_railing_layout(r,changed);});
    changed=s;changed.flights.resize(1);changed.landings.clear();changed.riser_count=4;reject([&]{derive_hosted_railing_layout(r,changed);});
    r.landing_host->edge_index=2;r.landing_host->start_fraction=0;reject([&]{derive_hosted_railing_layout(r,s);});
    r.landing_host->start_fraction=1/1.2;
    const auto boundary=derive_hosted_railing_layout(r,s);
    changed=s;changed.base_position={3,-2,4};changed.orientation_radians=.37;
    const auto boundary_moved=derive_hosted_railing_layout(r,changed);
    near(boundary_moved.rail_start.z,boundary.rail_start.z+4);
    r.landing_host->start_fraction=1/1.2-1e-9;reject([&]{derive_hosted_railing_layout(r,s);});
    r.landing_host->start_fraction=0;
    r.landing_host->edge_index=0;r.height=2;r.thickness=1.1;reject([&]{derive_hosted_railing_layout(r,s);});r.thickness=.04;r.height=.9;
    r.landing_host->edge_index=0;r.height=2;r.thickness=1;
    const auto full_normal=derive_hosted_railing_layout(r,s);
    near(full_normal.posts.front().base.y,.5);
    r.landing_host->edge_index=0;r.thickness=.04;r.height=.9;
    r.landing_host->end_fraction=.02;reject([&]{derive_hosted_railing_layout(r,s);});r.landing_host->end_fraction=1;
    r.post_spacing=1e-6;reject([&]{derive_hosted_railing_layout(r,s);});r.post_spacing=.3;
    r.landing_host=StairLandingRailingHost{"stair",StairLandingRole::top,"","upper","",0,0,1};
    const auto top=encode_railing_properties(r);
    require(!top.at("host").contains("landing_id")&&!top.at("host").contains("outgoing_flight_id"),"top is explicit with no fictional child");
    const auto rail=derive_hosted_railing_layout(r,s);near(rail.posts.front().base.z,1.6);
    auto h=*r.landing_host;h.edge_index=3;require(derive_stair_landing_edge(s,h).exposed_intervals.empty(),"top incoming rear edge is fully blocked");
    bad=top;bad["host"]["landing_id"]="invented";reject([&]{decode_railing_properties(r.id,bad);});
    changed=s;changed.top_landing.reset();reject([&]{derive_hosted_railing_layout(r,changed);});
    changed=s;changed.base_position={2,3,5};changed.orientation_radians=.5;
    const auto moved=derive_hosted_railing_layout(r,changed);near(moved.posts.front().base.z,6.6);
    changed=s;changed.going*=2;changed.width*=2;changed.total_rise*=2;
    changed.landings[0].depth*=2;changed.landings[0].thickness*=2;changed.top_landing->depth*=2;changed.top_landing->thickness*=2;
    r.height*=2;r.thickness*=2;r.post_spacing*=2;
    const auto scaled=derive_hosted_railing_layout(r,changed);near(scaled.rail_start.x,rail.rail_start.x*2);near(scaled.rail_start.y,rail.rail_start.y*2);near(scaled.rail_start.z,rail.rail_start.z*2);
    s.flights.clear();s.landings.clear();reject([&]{derive_hosted_railing_layout(r,s);});
}
}
int main() { try {layouts();failures_and_codecs();rail_support();rail_spacing_at_exact_support_boundaries();landing_edges_and_full_section_support();landing_codec_witnesses_and_refusals(); std::cout<<"Stair semantics tests passed\n"; return 0;} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
