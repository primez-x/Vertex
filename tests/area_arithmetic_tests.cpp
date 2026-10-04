#include "sketch/area_arithmetic.hpp"
#include "support/noninteractive_errors.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void expectNear(double actual,double expected){require(std::abs(actual-expected)<1e-8,"Independent area arithmetic answer differs");}
Boundary polygon(std::vector<Vec2> points){Boundary result;for(std::size_t i=0;i<points.size();++i)result.push_back({points[i],points[(i+1)%points.size()],0});return result;}
Boundary reverse(Boundary value){std::reverse(value.begin(),value.end());for(auto& edge:value){std::swap(edge.start,edge.end);edge.sweep_radians=-edge.sweep_radians;}return value;}
void cases() {
    const auto rectangle=polygon({{-2,-3},{2,-3},{2,0},{-2,0}});
    auto result=derive_area_arithmetic(rectangle);expectNear(result.gross_square_metres,12);
    require(result.method==AreaArithmeticMethod::rectangular_components && result.rectangles.size()==1,"Rectangle exposes one multiplication");
    expectNear(result.rectangles[0].width_metres,4);expectNear(result.rectangles[0].depth_metres,3);
    const auto ell=polygon({{0,0},{4,0},{4,1},{2,1},{2,3},{0,3}});
    for(const auto& boundary:{ell,reverse(ell)}) {
        result=derive_area_arithmetic(boundary);expectNear(result.gross_square_metres,8);
        require(result.rectangles.size()==2,"L shape exposes two rectangular components");
        expectNear(result.rectangles[0].area_square_metres,4);expectNear(result.rectangles[1].area_square_metres,4);
    }
    const auto triangle=polygon({{0,0},{3,0},{0,4}});result=derive_area_arithmetic(triangle);expectNear(result.gross_square_metres,6);
    require(result.method==AreaArithmeticMethod::triangle && result.rectangles.empty() && result.triangle_base_and_height_metres,
        "Triangle exposes base and perpendicular height instead of rectangles");
    expectNear(result.triangle_base_and_height_metres->x,3);expectNear(result.triangle_base_and_height_metres->y,4);
    Boundary rotated;for(const auto& edge:rectangle)rotated.push_back(transform_segment(edge,{{},std::numbers::pi/4,false,false,{}}));
    result=derive_area_arithmetic(rotated);expectNear(result.gross_square_metres,12);require(result.rectangles.empty(),"Rotated geometry does not invent orthogonal component dimensions");
    const Boundary half{{{-1,0},{1,0},std::numbers::pi},{{1,0},{-1,0},0}};
    const Boundary quarter{{{1,0},{0,1},std::numbers::pi/2},{{0,1},{0,0},0},{{0,0},{1,0},0}};
    for(const auto& boundary:{half,reverse(half),quarter,reverse(quarter)}) {
        result=derive_area_arithmetic(boundary);const auto expected=boundary.size()==2 ? std::numbers::pi/2 : std::numbers::pi/4;
        expectNear(result.gross_square_metres,expected);expectNear(result.chord_contribution_square_metres,boundary.size()==2 ? 0 : 0.5);
        expectNear(result.chord_contribution_square_metres+result.curve_adjustment_square_metres,expected);
        require(result.method==AreaArithmeticMethod::chord_and_arcs,"Curves expose analytical adjustment");
    }
    Boundary translated;for(const auto& edge:ell)translated.push_back(transform_segment(edge,{{},0,false,false,{1e9,-1e9}}));
    expectNear(derive_area_arithmetic(translated).gross_square_metres,8);
    for(auto invalid:{polygon({{0,0},{3,3},{0,3},{3,0}}),Boundary{{{0,0},{1,0},0}}}) {
        bool refused{};try{(void)derive_area_arithmetic(invalid);}catch(const std::invalid_argument&){refused=true;}
        require(refused,"Invalid geometry must not receive a fabricated explanation");
    }
}
}
int main(){sketch::testing::noninteractive_errors();try{cases();std::cout<<"Area arithmetic tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
