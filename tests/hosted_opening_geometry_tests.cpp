#include "sketch/hosted_opening_geometry.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a-b) < 1e-9; }
bool near(Vec2 a, Vec2 b) { return near(a.x,b.x) && near(a.y,b.y); }
bool same_segment_points(const Segment& a, const Segment& b) {
    return (near(a.start,b.start) && near(a.end,b.end)) ||
        (near(a.start,b.end) && near(a.end,b.start));
}
bool same_boundary(const Boundary& a, const Boundary& b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0; i<a.size(); ++i) {
        if (!near(a[i].start,b[i].start) || !near(a[i].end,b[i].end) ||
            !near(a[i].sweep_radians,b[i].sweep_radians)) return false;
    }
    return true;
}
bool contains_segment(const Boundary& boundary, const Segment& segment) {
    for (const auto& edge : boundary)
        if (same_segment_points(edge,segment)) return true;
    return false;
}
void require_closed_wall_polygons(const Boundary& boundary, const char* message) {
    require(boundary.size()%4==0, message);
    for (std::size_t first=0; first<boundary.size(); first+=4) {
        for (std::size_t i=0; i<4; ++i)
            require(near(boundary[first+i].end,boundary[first+(i+1)%4].start),message);
    }
}
template<class F> void rejects(F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid hosted geometry was accepted");
}

void test_line_geometry() {
    const Segment host{{4,-3},{10,5},0};
    require(near(point_at_host_station(host,3),{5.8,-.6}), "line station uses metres");
    require(near(point_at_host_station(host,-2),{2.8,-4.6}), "line station extrapolates");
    require(near(project_host_station(host,{4.2,7.6},3),8.6), "line projection is signed and unclamped");
    require(near(project_host_station(host,{2.8,-4.6},3),-2), "line projection retains a station behind the host");
    const auto span = hosted_opening_span(host,3,2);
    require(near(span.start,{5.8,-.6}) && near(span.end,{7,1}) && span.sweep_radians == 0,
            "line span preserves exact opening positions");
    const auto symbol = window_plan_symbol(host,3,2,.3);
    require(symbol.size() == 4 && near(symbol[0].start,{5.752,-.564}) &&
        near(symbol[0].end,{6.952,1.036}) && near(symbol[1].start,{5.848,-.636}) &&
        near(symbol[1].end,{7.048,.964}), "line glazing retains the old inset geometry");
    require(near(symbol[2].start,symbol[1].start) && near(symbol[2].end,symbol[0].start) &&
        near(symbol[3].start,symbol[1].end) && near(symbol[3].end,symbol[0].end),
        "window jambs connect the glazing outlines");
    require(near(window_plan_symbol({{0,0},{10,0},0},1,2,.01)[0].start,{1,.015}) &&
        near(window_plan_symbol({{0,0},{10,0},0},1,2,1)[0].start,{1,.06}),
        "window inset keeps its minimum and maximum clamp");
}

void test_arc_geometry() {
    for (double sweep : {std::numbers::pi/2,-std::numbers::pi/2,
                         3*std::numbers::pi/2,-3*std::numbers::pi/2}) {
        // This hand-constructed host has centre (4,-3), radius 10, start angle .7.
        const auto expected = [](double angle, double radius = 10.) {
            return Vec2{4+radius*std::cos(angle),-3+radius*std::sin(angle)};
        };
        const Segment host{expected(.7),expected(.7+sweep),sweep};
        const double sign = sweep > 0 ? 1 : -1;
        const auto span = hosted_opening_span(host,3,2);
        require(near(span.start,expected(.7+sign*.3)) && near(span.end,expected(.7+sign*.5)) &&
            near(span.sweep_radians,sign*.2) && near(segment_length(span),2),
            "curved span width must measure arc length rather than chord");
        require(near(point_at_host_station(host,-2),expected(.7-sign*.2)),
            "curved station extrapolates behind the host start");
        require(near(project_host_station(host,expected(.7+sign*.5,15),5),5),
            "radial drag displacement must not change arc station");
        require(near(project_host_station(host,expected(.7-sign*.01),.1),-.1),
            "projection near start must preserve the nearby negative branch");
        const double length = 10*std::abs(sweep);
        require(near(project_host_station(host,expected(.7+sweep+sign*.01),length),length+.1),
            "projection near end must preserve the nearby branch on major arcs");
        const auto symbol = window_plan_symbol(host,3,2,.3);
        require(symbol.size() == 4 && near(symbol[0].sweep_radians,sign*.2) &&
            near(symbol[1].sweep_radians,sign*.2) &&
            near(symbol[0].start,expected(.7+sign*.3,10-sign*.06)) &&
            near(symbol[0].end,expected(.7+sign*.5,10-sign*.06)) &&
            near(symbol[1].start,expected(.7+sign*.3,10+sign*.06)),
            "curved glazing must use concentric arcs and tangent-left inset");
        require(near(symbol[2].start,symbol[1].start) && near(symbol[2].end,symbol[0].start) &&
            near(symbol[3].start,symbol[1].end) && near(symbol[3].end,symbol[0].end),
            "curved window jambs must join the arc endpoints");
        rejects([&] { (void)project_host_station(host,{4,-3},5); });
    }
}

void test_wall_footprints() {
    const Segment line{{0,0},{10,0},0};
    const auto whole = wall_plan_footprint(line,{},.4);
    require(whole.size()==4 && near(whole[0].start,Vec2{0,.2}), "wall faces use half thickness");
    const std::vector<HostedOpening> cuts{{"a",2,1,0,2},{"b",6,2,0,2}};
    require(wall_plan_footprint(line,cuts,.4).size()==12, "two openings leave three wall intervals");
    require(wall_plan_footprint(line,{{"all",0,10,0,2}},.4).empty(), "full cut must not restore wall");
    rejects([&]{ (void)wall_plan_footprint(line,{{"bad",9,2,0,2}},.4); });
    rejects([&]{ (void)wall_plan_footprint(line,{},0); });
    for (const double sweep : {std::numbers::pi/2,-std::numbers::pi/2}) {
        const Segment arc{{10,0},{0,sweep>0?10.:-10.},sweep};
        const auto result=wall_plan_footprint(arc,{{"cut",3,2,0,2}},.4);
        require(result.size()==8, "curved cut leaves two exact arc footprints");
        require(result[0].sweep_radians*sweep>0 && result[2].sweep_radians*sweep<0,
            "opposite wall faces preserve reversed arc winding");
    }
}

void test_joined_l_corner_and_reversed_orientation() {
    const Segment host{{0,0},{4,0},0};
    const Segment previous{{0,-3},{0,0},0};
    const auto host_join=joined_wall_plan_geometry(host,{},.4,{{true,previous,.6}});
    const auto previous_join=joined_wall_plan_geometry(previous,{},.6,{{false,host,.4}});
    require(host_join.footprint.size()==4 && previous_join.footprint.size()==4,
        "a joined L corner retains two complete wall footprints");
    require_closed_wall_polygons(host_join.footprint,"joined host footprint must remain closed");
    require_closed_wall_polygons(previous_join.footprint,"joined neighbor footprint must remain closed");
    const Segment expected_diagonal{{-.3,.2},{.3,-.2},0};
    require(same_segment_points(host_join.footprint[3],expected_diagonal),
        "unequal wall thicknesses meet at the analytical face intersections");
    require(same_segment_points(host_join.footprint[3],previous_join.footprint[1]),
        "both wall footprints share the same diagonal corner cap");
    require(host_join.strokes.size()==3 && previous_join.strokes.size()==3 &&
        !contains_segment(host_join.strokes,expected_diagonal) &&
        !contains_segment(previous_join.strokes,expected_diagonal),
        "joined plan strokes omit only the internal diagonal endpoint caps");

    const Segment reversed_host{{4,0},{0,0},0};
    const Segment reversed_previous{{0,0},{0,-3},0};
    const auto reversed_host_join=joined_wall_plan_geometry(
        reversed_host,{},.4,{{false,reversed_previous,.6}});
    const auto reversed_previous_join=joined_wall_plan_geometry(
        reversed_previous,{},.6,{{true,reversed_host,.4}});
    require(same_segment_points(host_join.footprint[3],reversed_host_join.footprint[1]) &&
        same_segment_points(previous_join.footprint[1],reversed_previous_join.footprint[3]),
        "reversing both wall directions preserves the shared diagonal point set");
}

void test_joined_rectangle_winding() {
    const std::vector<std::vector<Segment>> loops{
        {{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},0},{{0,3},{0,0},0}},
        {{{0,0},{0,3},0},{{0,3},{4,3},0},{{4,3},{4,0},0},{{4,0},{0,0},0}}
    };
    for (const auto& loop : loops) {
        std::vector<WallPlanGeometry> geometries;
        for (std::size_t i=0; i<loop.size(); ++i) {
            const Segment previous=loop[(i+loop.size()-1)%loop.size()];
            const Segment next=loop[(i+1)%loop.size()];
            geometries.push_back(joined_wall_plan_geometry(loop[i],{},.2,
                {{true,previous,.2},{false,next,.2}}));
        }
        for (const auto& geometry : geometries) {
            require(geometry.footprint.size()==4 && geometry.strokes.size()==2,
                "a wall joined at both ends keeps its closed fill and two face strokes");
            require_closed_wall_polygons(geometry.footprint,
                "each joined rectangle wall footprint must remain closed");
        }
        for (std::size_t i=0; i<loop.size(); ++i) {
            const auto& current=geometries[i].footprint[1];
            const auto& next_start=geometries[(i+1)%loop.size()].footprint[3];
            require(same_segment_points(current,next_start),
                "neighboring rectangle walls share each corner cap in either winding");
        }
    }
}

void test_joined_opening_and_continuation_policies() {
    const Segment host{{0,0},{4,0},0};
    const Segment previous{{0,-3},{0,0},0};
    const std::vector<HostedOpening> opening{{"window",.8,1,0,2}};
    const auto joined=joined_wall_plan_geometry(host,opening,.4,{{true,previous,.6}});
    const auto original=wall_plan_footprint(host,opening,.4);
    require(joined.footprint.size()==8 && same_boundary(
        Boundary(joined.footprint.begin()+4,joined.footprint.end()),
        Boundary(original.begin()+4,original.end())),
        "a corner miter leaves opening station and the far wall interval unchanged");
    require(near(joined.footprint[1].start,{.8,.2}) &&
        near(joined.footprint[1].end,{.8,-.2}),
        "the opening jamb stays at its original measured station");
    require_closed_wall_polygons(joined.footprint,"opened joined wall footprints must remain closed");

    const std::vector<HostedOpening> flush{{"door",0,.5,0,2}};
    const auto flush_join=joined_wall_plan_geometry(host,flush,.4,{{true,previous,.6}});
    const auto flush_original=wall_plan_footprint(host,flush,.4);
    require(same_boundary(flush_join.footprint,flush_original) &&
        same_boundary(flush_join.strokes,flush_original),
        "an opening at the joined endpoint keeps the legacy capped remainder without fabricated wall");
    const std::vector<HostedOpening> full{{"full",0,4,0,2}};
    const auto full_join=joined_wall_plan_geometry(host,full,.4,{{true,previous,.6}});
    require(full_join.footprint.empty() && full_join.strokes.empty(),
        "a fully cut wall stays empty when a junction is supplied");

    const Segment continuation{{4,0},{7,0},0};
    const auto first=joined_wall_plan_geometry(host,{},.4,{{false,continuation,.4}});
    const auto second=joined_wall_plan_geometry(continuation,{},.4,{{true,host,.4}});
    require(first.strokes.size()==3 && second.strokes.size()==3 &&
        same_segment_points(first.footprint[1],second.footprint[3]),
        "an equal-thickness collinear continuation removes its coincident caps");
    const auto wide_second=joined_wall_plan_geometry(continuation,{},.6,{{true,host,.4}});
    require(same_boundary(wide_second.footprint,wall_plan_footprint(continuation,{},.6)) &&
        same_boundary(wide_second.strokes,wide_second.footprint),
        "an unequal-width collinear transition retains its endpoint cap");
}

void test_joined_fallbacks() {
    const Segment host{{0,0},{10,0},0};
    const auto require_capped=[&](const Segment& candidate, double neighbor_thickness,
                                  const char* message) {
        const auto joined=joined_wall_plan_geometry(host,{},.4,
            {{true,candidate,neighbor_thickness}});
        const auto original=wall_plan_footprint(host,{},.4);
        require(same_boundary(joined.footprint,original) &&
            same_boundary(joined.strokes,original),message);
        require_closed_wall_polygons(joined.footprint,
            "a rejected join must keep the original closed wall footprint");
    };
    require_capped({{0,0},{10,1e-8},0},.4,
        "near-parallel wall candidates fall back to butt caps");
    require_capped({{0,0},{10,.35},0},.4,
        "an unbounded acute miter falls back to butt caps");

    const auto ambiguous=joined_wall_plan_geometry(host,{},.4,
        {{true,{{0,0},{0,-3},0},.4},{true,{{0,0},{-3,0},0},.4}});
    require(same_boundary(ambiguous.footprint,wall_plan_footprint(host,{},.4)) &&
        same_boundary(ambiguous.strokes,ambiguous.footprint),
        "multiple neighbors at one endpoint are treated as ambiguous");

    const Segment curved{{10,0},{0,10},std::numbers::pi/2};
    const Segment curved_neighbor{{0,10},{-3,10},0};
    const auto curved_host=joined_wall_plan_geometry(curved,{},.4,
        {{false,curved_neighbor,.4}});
    require(same_boundary(curved_host.footprint,wall_plan_footprint(curved,{},.4)) &&
        same_boundary(curved_host.strokes,curved_host.footprint),
        "curved hosts retain their established exact arc faces and endpoint caps");
    const Segment straight{{0,10},{-3,10},0};
    const Segment curved_neighbor_at_start{{0,10},{10,0},-std::numbers::pi/2};
    const auto curved_neighbor_join=joined_wall_plan_geometry(straight,{},.4,
        {{true,curved_neighbor_at_start,.4}});
    require(same_boundary(curved_neighbor_join.footprint,wall_plan_footprint(straight,{},.4)) &&
        same_boundary(curved_neighbor_join.strokes,curved_neighbor_join.footprint),
        "curved neighbors retain straight-host butt caps as the supported fallback");

    const Segment short_host{{0,0},{.4,0},0};
    const auto short_join=joined_wall_plan_geometry(short_host,{},.4,
        {{true,{{0,0},{0,-3},0},1.2}});
    const auto short_original=wall_plan_footprint(short_host,{},.4);
    require(same_boundary(short_join.footprint,short_original) &&
        same_boundary(short_join.strokes,short_original),
        "a miter that would invert a short wall falls back to its butt cap");

    const double large_origin=1e16;
    const Segment remote_host{{large_origin,0},{large_origin+32,0},0};
    const Segment remote_neighbor{{large_origin,-3},{large_origin,0},0};
    const auto remote_join=joined_wall_plan_geometry(remote_host,{},.4,
        {{true,remote_neighbor,.6}});
    const auto remote_original=wall_plan_footprint(remote_host,{},.4);
    require(same_boundary(remote_join.footprint,remote_original) &&
        same_boundary(remote_join.strokes,remote_original),
        "a miter outside the representable local geometry envelope keeps butt caps");
}

void test_invalid_geometry() {
    const Segment line{{0,0},{10,0},0};
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double large = std::numeric_limits<double>::max();
    for (const Segment host : {Segment{{0,0},{0,0},0},Segment{{0,0},{inf,0},0},
            Segment{{0,0},{10,0},2*std::numbers::pi},Segment{{0,0},{10,0},-2*std::numbers::pi},
            Segment{{0,0},{10,0},nan},
            Segment{{-large,0},{large,0},0},Segment{{0,0},{10,0},1e-320}}) {
        rejects([&] { (void)point_at_host_station(host,1); });
        rejects([&] { (void)project_host_station(host,{1,0},1); });
        rejects([&] { (void)hosted_opening_span(host,1,2); });
        rejects([&] { (void)window_plan_symbol(host,1,2,.3); });
    }
    for (double bad : {inf,nan}) {
        rejects([&] { (void)point_at_host_station(line,bad); });
        rejects([&] { (void)project_host_station(line,{1,0},bad); });
        rejects([&] { (void)project_host_station(line,{bad,0},1); });
        rejects([&] { (void)hosted_opening_span(line,bad,2); });
        rejects([&] { (void)window_plan_symbol(line,1,2,bad); });
    }
    for (double bad : {0.,-1.,inf,nan,large})
        rejects([&] { (void)hosted_opening_span(line,1,bad); });
    rejects([&] { (void)hosted_opening_span(line,-1,2); });
    rejects([&] { (void)hosted_opening_span(line,9,2); });
    rejects([&] { (void)window_plan_symbol(line,1,2,0); });
    rejects([&] { (void)window_plan_symbol(line,1,2,-.3); });
    // Even if glazing would fit, reject a wall whose thickness crosses its centre.
    rejects([&] { (void)window_plan_symbol({{.1,0},{0,.1},std::numbers::pi/2},0,.1,.3); });
    rejects([&] { (void)window_plan_symbol({{.002,0},{0,.002},std::numbers::pi/2},0,.001,.001); });
    rejects([&] { (void)point_at_host_station({{large,0},{large/2,10},0},-large); });
}
} // namespace

int main() {
    try {
        test_line_geometry();
        test_arc_geometry();
        test_invalid_geometry();
        test_wall_footprints();
        test_joined_l_corner_and_reversed_orientation();
        test_joined_rectangle_winding();
        test_joined_opening_and_continuation_policies();
        test_joined_fallbacks();
        std::cout << "hosted opening geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
