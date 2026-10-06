#include "sketch/constraint_entity.hpp"
#include "sketch/constraints.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
using namespace sketch;
constexpr double pi = std::numbers::pi;
void require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
void expect_close(double value,double expected,const char* message) {
    require(std::isfinite(value) && std::abs(value-expected)<1e-7,message);
}
template<class Action> void invalid(Action action,const char* message) {
    bool refused=false;
    try {action();} catch(const std::invalid_argument&) {refused=true;}
    require(refused,message);
}
Entity wall(const char* id,const Segment& segment) {
    return {id,"wall",{{"baseline",{{"start",{segment.start.x,segment.start.y}},
        {"end",{segment.end.x,segment.end.y}},{"sweep_radians",segment.sweep_radians}}},
        {"thickness_m",.1},{"height_m",3.},{"elevation_m",0.}}};
}
PersistentConstraint relation() {
    return {"smooth",ConstraintRelationKind::tangent,
        {{"line",WallEndpointRole::end},{"line",WallEndpointRole::start},
         {"arc",WallEndpointRole::start},{"arc",WallEndpointRole::end}},{},{}};
}
ConstraintSolveRequest line_arc(double sweep,bool reverse=false) {
    // Unit-circle contact at (1,0): the true circular tangent is vertical,
    // although the quarter/major-arc chord is diagonal.
    const double side=sweep>0 ? -1. : 1.;
    ConstraintSolveRequest request;
    request.points={{"ls",1.2,2*side},{"le",1,0},{"as",1,0},
        {"ae",std::cos(sweep),std::sin(sweep)}};
    TangentConstraint tangent{"smooth","ls","le","as","ae",0,sweep,false,true};
    if (reverse) {
        std::swap(request.points[0].x,request.points[1].x);
        std::swap(request.points[0].y,request.points[1].y);
        std::swap(request.points[2].x,request.points[3].x);
        std::swap(request.points[2].y,request.points[3].y);
        tangent.first_at_start=true;tangent.second_at_start=false;
        tangent.second_sweep_radians=-sweep;
    }
    request.constraints={tangent,FixedLengthConstraint{"line-length","ls","le",2},
        FixedAnchorConstraint{"arc-start","as",request.points[2].x,request.points[2].y},
        FixedAnchorConstraint{"arc-end","ae",request.points[3].x,request.points[3].y}};
    return request;
}
void solver_uses_true_endpoint_derivatives() {
    for (const double sweep : {pi/2,-pi/2,3*pi/2,-3*pi/2}) for (const bool reverse : {false,true}) {
        const auto request=line_arc(sweep,reverse);
        const auto solved=solve_planar_constraints(request);
        require(solved.accepted(),"line/arc analytical tangency must solve in either orientation");
        require(request.points[0].x== (reverse ? 1. : 1.2),"solver modified its source request");
        const auto& outer=solved.points[reverse ? 1 : 0];
        const auto& contact=solved.points[reverse ? 0 : 1];
        expect_close(outer.x,1,"circular endpoint derivative must yield a vertical line");
        expect_close(outer.y,sweep>0 ? -2. : 2.,"solver chose a tangent cusp instead of the smooth branch");
        expect_close(contact.x,1,"tangent contacts must coincide in x");
        expect_close(contact.y,0,"tangent contacts must coincide in y");
    }
    auto displaced=line_arc(pi/2);displaced.points[1].x=1.3;displaced.points[1].y=.1;
    const auto reunited=solve_planar_constraints(displaced);
    require(reunited.accepted(),"tangent must solve contact coincidence as well as angular alignment");
    expect_close(reunited.points[1].x,1,"tangent left a contact gap in x");
    expect_close(reunited.points[1].y,0,"tangent left a contact gap in y");
    // Two touching unit semicircles: first ends at (1,0), second starts there.
    // Their outward endpoint derivatives are opposite vertical vectors.
    ConstraintSolveRequest arcs;
    arcs.points={{"a",-1,0},{"j",1,0},{"b",3,0}};
    arcs.constraints={TangentConstraint{"arc-arc","a","j","j","b",pi,-pi,false,true},
        FixedAnchorConstraint{"a-pin","a",-1,0},FixedAnchorConstraint{"j-pin","j",1,0},
        FixedAnchorConstraint{"b-pin","b",3,0}};
    require(solve_planar_constraints(arcs).accepted(),"shared point ID must support true arc/arc tangency");
    std::get<TangentConstraint>(arcs.constraints.front()).second_sweep_radians=pi;
    const auto cusp=solve_planar_constraints(arcs);
    require(!cusp.accepted() && cusp.points==arcs.points,"locked arc/arc cusp must refuse without partial geometry");
    for (const double first_sweep : {pi/2,3*pi/2}) for (const double second_sweep : {-pi/2,-3*pi/2}) {
        // Independent unit-circle construction: centers (0,0) and (2,0),
        // touching at (1,0). Their contact derivatives are vertical.
        ConstraintSolveRequest circular;
        circular.points={{"a",std::cos(-first_sweep),std::sin(-first_sweep)},
            {"j",1,0},{"b",2+std::cos(pi+second_sweep),std::sin(pi+second_sweep)}};
        circular.constraints={TangentConstraint{"arc-arc","a","j","j","b",first_sweep,second_sweep,false,true}};
        for (const auto& point:circular.points)
            circular.constraints.push_back(FixedAnchorConstraint{"pin-"+point.id,point.id,point.x,point.y});
        require(solve_planar_constraints(circular).accepted(),"major and minor circular contacts must use their true endpoint derivatives");
    }
}
void conflicts_and_invalid_input_publish_original_points() {
    auto locked=line_arc(pi/2);
    for (const auto& point:locked.points)
        locked.constraints.push_back(FixedAnchorConstraint{"pin-"+point.id,point.id,point.x,point.y});
    const auto conflict=solve_planar_constraints(locked);
    require(!conflict.accepted() && conflict.points==locked.points,"inconsistent locked tangent published partial geometry");
    for (const double sweep : {std::numeric_limits<double>::infinity(),2*pi,-2*pi}) {
        auto request=line_arc(pi/2);
        std::get<TangentConstraint>(request.constraints.front()).second_sweep_radians=sweep;
        const auto result=solve_planar_constraints(request);
        require(result.status==ConstraintSolveStatus::rejected_invalid_input && result.points==request.points,
            "unsupported tangent sweep must refuse before solving");
    }
    auto lines=line_arc(pi/2);
    std::get<TangentConstraint>(lines.constraints.front()).second_sweep_radians=0;
    require(solve_planar_constraints(lines).status==ConstraintSolveStatus::rejected_invalid_input,
        "line/line must not masquerade as curve tangency");
    auto degenerate=line_arc(pi/2);degenerate.points[3]=degenerate.points[2];degenerate.points[3].id="ae";
    require(solve_planar_constraints(degenerate).status==ConstraintSolveStatus::rejected_invalid_input,
        "degenerate arc chord must refuse before solving");
}
void persisted_semantics_are_exclusive_and_geometric() {
    auto encoded=encode_constraint_entity(relation());
    require(encoded.properties.at("version")==5 && encoded.properties.contains("entity_ids"),"tangent requires its exclusive v5 schema");
    encoded.extensions["vendor"]={{"retain",7}};encoded.properties["bindings"][0]["vendor"]="keep";
    const auto decoded=decode_constraint_entity(encoded);
    require(decoded.supported() && encode_constraint_entity(*decoded.constraint,&encoded)==encoded,"tangent codec lost retained metadata");
    for (const unsigned version : {1U,2U,3U,4U}) {
        auto legacy=encoded;legacy.properties["version"]=version;
        if (version==1) {legacy.properties["wall_ids"]=legacy.properties.at("entity_ids");legacy.properties.erase("entity_ids");}
        require(!decode_constraint_entity(legacy).supported(),"earlier schema reinterpreted tangent semantics");
    }
    auto other=encoded;other.properties["relation"]="parallel";
    require(!decode_constraint_entity(other).supported(),"v5 reinterpreted a chord relation");
    auto malformed=encoded;malformed.properties["bindings"][1]["role"]="end";
    invalid([&]{(void)decode_constraint_entity(malformed);},"tangent admitted duplicate contact roles");
    auto bad=relation();bad.bindings[1].owner_id="other";
    invalid([&]{(void)encode_constraint_entity(bad);},"tangent pair borrowed another owner");
    bad=relation();bad.bindings[2]=bad.bindings[0];bad.bindings[3]=bad.bindings[1];
    invalid([&]{(void)encode_constraint_entity(bad);},"tangent reused the same segment twice");
    const std::map<std::string,Entity,std::less<>> owners{
        {"line",wall("line",{{1,-2},{1,0},0})},{"arc",wall("arc",{{1,0},{0,1},pi/2})}};
    const auto segments=resolve_constraint_tangent_segments(relation(),owners);
    expect_close(constraint_tangent_angular_residual(segments[0],WallEndpointRole::end,segments[1],WallEndpointRole::start),0,
        "analytical derivative must recognize smooth line/arc contact");
    expect_close(constraint_tangent_angular_residual({{1,2},{1,0},0},WallEndpointRole::end,segments[1],WallEndpointRole::start),pi,
        "parallel tangent cusp must not count as smooth");
    for (const double sweep : {0.,2*pi,std::numeric_limits<double>::quiet_NaN()}) {
        auto changed=owners;changed.at("arc").properties["baseline"]["sweep_radians"]=sweep;
        invalid([&]{(void)resolve_constraint_tangent_segments(relation(),changed);},"tangent resolver admitted missing/invalid curve authority");
    }
    auto unsupported=owners;unsupported.at("arc").type="symbol";
    invalid([&]{(void)resolve_constraint_tangent_segments(relation(),unsupported);},"decorative owner admitted as circular authority");
    const IdentifiedBoundary boundary{"arc-boundary","measurement_boundary",
        {{"curve","v1","v2",{{1,0},{0,1},pi/2}},
         {"west","v2","v3",{{0,1},{0,0},0}},
         {"south","v3","v1",{{0,0},{1,0},0}}}};
    auto stable_owners=owners;stable_owners.erase("arc");
    stable_owners.emplace(boundary.id,encode_identified_boundary_entity(boundary));
    auto stable=relation();stable.bindings[2]={boundary.id,WallEndpointRole::start,"curve","v1"};
    stable.bindings[3]={boundary.id,WallEndpointRole::end,"curve","v2"};
    require(decode_constraint_entity(encode_constraint_entity(stable)).supported(),"stable boundary tangent bindings must use v5");
    const auto bound=resolve_constraint_tangent_segments(stable,stable_owners);
    expect_close(bound[1].sweep_radians,pi/2,"tangent lost the stable segment's signed sweep");
    stable.bindings[2].vertex_id="v3";
    invalid([&]{(void)resolve_constraint_tangent_segments(stable,stable_owners);},"tangent binding accepted the wrong stable contact vertex");
}
}
int main() {
    testing::noninteractive_errors();
    try {
        solver_uses_true_endpoint_derivatives();conflicts_and_invalid_input_publish_original_points();
        persisted_semantics_are_exclusive_and_geometric();
        std::cout<<"Tangent constraint tests passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"tangent_constraint_tests: "<<error.what()<<'\n';return 1;}
}
