#include "sketch/geometry.hpp"
#include "support/noninteractive_errors.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void require_near(double actual,double expected,double tolerance,const char* message) {
    if(!std::isfinite(actual) || std::abs(actual-expected)>tolerance) throw std::runtime_error(message);
}
template<class F> void rejected(F&& function) {
    try { function(); } catch(const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid measured curve was accepted");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        using namespace sketch;
        constexpr double pi=std::numbers::pi;
        auto arc=arc_from_chord_arc_length({1,0},{0,1},pi/2);
        require_near(arc.sweep_radians,pi/2,1e-12,"Quarter-circle sweep from measured arc length");
        require_near(segment_length(arc),pi/2,1e-12,"Measured arc length roundtrip");
        arc=arc_from_chord_arc_length({1,0},{-1,0},pi,true);
        require_near(arc.sweep_radians,-pi,1e-12,"Clockwise semicircle");
        arc=arc_from_chord_arc_length({1,0},{0,-1},1.5*pi);
        require_near(arc.sweep_radians,1.5*pi,1e-12,"Major arc from chord and length");
        arc=arc_from_chord_arc_length({0,0},{100,0},100.000001);
        require_near(segment_length(arc),100.000001,1e-10,"Shallow arc precision");
        arc=arc_from_start_tangent({1,0},pi/2,pi/2,pi/2);
        require_near(arc.end.x,0,1e-12,"Tangent construction x");
        require_near(arc.end.y,1,1e-12,"Tangent construction y");
        arc=arc_from_start_tangent({0,1},0,pi/2,-pi/2);
        require_near(arc.end.x,1,1e-12,"Clockwise tangent x");
        require_near(arc.end.y,0,1e-12,"Clockwise tangent y");
        rejected([]{(void)arc_from_chord_arc_length({0,0},{2,0},1);});
        rejected([]{(void)arc_from_chord_arc_length({0,0},{2,0},2);});
        rejected([]{(void)arc_from_chord_arc_length({0,0},{0,0},1);});
        rejected([]{(void)arc_from_chord_arc_length({0,0},{1,0},std::numeric_limits<double>::infinity());});
        rejected([]{(void)arc_from_chord_arc_length({0,0},{1,0},1e300);});
        rejected([]{(void)arc_from_start_tangent({0,0},0,1,0);});
        rejected([]{(void)arc_from_start_tangent({0,0},0,-1,1);});
        rejected([]{(void)arc_from_start_tangent({0,0},0,1,2*std::numbers::pi);});
        rejected([]{(void)arc_from_start_tangent({1e100,1e100},0,1,1);});
        std::cout<<"Measured curve construction tests passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
