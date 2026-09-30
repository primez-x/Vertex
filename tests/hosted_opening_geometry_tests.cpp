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
        std::cout << "hosted opening geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
