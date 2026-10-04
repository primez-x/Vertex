#include "sketch/area_arithmetic.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace sketch {
AreaArithmetic derive_area_arithmetic(const Boundary& boundary) {
    const auto issues=validate_boundary(boundary);
    if(!issues.empty())throw std::invalid_argument("Area arithmetic requires valid closed geometry: "+issues.front().message);
    const auto directed=signed_area(boundary);
    if(!std::isfinite(directed) || directed==0)throw std::invalid_argument("Area arithmetic requires finite nonzero area");
    const double winding=directed>0 ? 1.0 : -1.0;
    AreaArithmetic result;result.gross_square_metres=std::abs(directed);
    Boundary chords=boundary;bool curved=false,orthogonal=true,exact_closed=true;
    std::vector<double> levels;const auto origin=boundary.front().start;
    for(std::size_t i=0;i<boundary.size();++i) {
        const auto& edge=boundary[i];curved|=edge.sweep_radians!=0;
        orthogonal&=edge.start.x==edge.end.x || edge.start.y==edge.end.y;
        const auto next=boundary[(i+1)%boundary.size()].start;
        exact_closed&=edge.end.x==next.x && edge.end.y==next.y;
        chords[i].sweep_radians=0;levels.push_back(edge.start.y-origin.y);
    }
    result.chord_contribution_square_metres=signed_area(chords)*winding;
    result.curve_adjustment_square_metres=result.gross_square_metres-result.chord_contribution_square_metres;
    if(!std::isfinite(result.chord_contribution_square_metres) || !std::isfinite(result.curve_adjustment_square_metres))
        throw std::invalid_argument("Area chord contribution is not representable");
    if(curved) {result.method=AreaArithmeticMethod::chord_and_arcs;return result;}
    if(exact_closed && boundary.size()==3) {
        const auto base=segment_length(boundary.front());
        const auto dx=static_cast<long double>(boundary.front().end.x)-origin.x;
        const auto dy=static_cast<long double>(boundary.front().end.y)-origin.y;
        const auto tx=static_cast<long double>(boundary[2].start.x)-origin.x;
        const auto ty=static_cast<long double>(boundary[2].start.y)-origin.y;
        const auto height=static_cast<double>(std::abs(dx*ty-dy*tx)/base);
        if(!std::isfinite(height) || std::abs(base*height/2-result.gross_square_metres)>std::max(1e-10,result.gross_square_metres*1e-10))
            throw std::invalid_argument("Triangle multiplication does not reconcile to analytical gross area");
        result.method=AreaArithmeticMethod::triangle;
        result.triangle_base_and_height_metres=Vec2{base,height};
        return result;
    }
    // Do not call a nearly orthogonal or tolerance-closed shape a rectangle.
    if(!orthogonal || !exact_closed)return result;
    std::sort(levels.begin(),levels.end());levels.erase(std::unique(levels.begin(),levels.end()),levels.end());
    for(std::size_t i=1;i<levels.size();++i) {
        const auto depth=levels[i]-levels[i-1];const auto mid=levels[i-1]+depth/2;
        std::vector<double> crossings;
        for(const auto& edge:boundary) {
            const auto y1=edge.start.y-origin.y,y2=edge.end.y-origin.y;
            if(mid>std::min(y1,y2) && mid<std::max(y1,y2))crossings.push_back(edge.start.x-origin.x);
        }
        std::sort(crossings.begin(),crossings.end());
        if(crossings.size()%2)throw std::invalid_argument("Area strip intersections do not pair");
        for(std::size_t j=0;j<crossings.size();j+=2) {
            const auto width=crossings[j+1]-crossings[j];const auto area=width*depth;
            if(!std::isfinite(area) || width<=0 || depth<=0)throw std::invalid_argument("Invalid area rectangle component");
            result.rectangles.push_back({width,depth,area});
        }
    }
    const auto sum=std::accumulate(result.rectangles.begin(),result.rectangles.end(),0.0,
        [](double total,const auto& component){return total+component.area_square_metres;});
    if(result.rectangles.empty() || !std::isfinite(sum) ||
        std::abs(sum-result.gross_square_metres)>std::max(1e-10,result.gross_square_metres*1e-10))
        throw std::invalid_argument("Area rectangles do not reconcile to analytical gross area");
    result.method=AreaArithmeticMethod::rectangular_components;return result;
}
} // namespace sketch
