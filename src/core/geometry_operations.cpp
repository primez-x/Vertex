#include "sketch/geometry_operations.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
void valid(const IdentifiedBoundary& boundary) { (void)encode_identified_boundary_entity(boundary); }
void finite(Vec2 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw std::invalid_argument("Point must be finite");
}
bool same(Vec2 a, Vec2 b) { return a.x==b.x && a.y==b.y; }

template<class F> IdentifiedBoundary transform(const IdentifiedBoundary& source, F operation, bool reflect=false) {
    valid(source);
    auto result=source;
    for (auto& edge:result.segments) {
        edge.segment.start=operation(edge.segment.start);
        edge.segment.end=operation(edge.segment.end);
        if (reflect) edge.segment.sweep_radians=-edge.segment.sweep_radians;
    }
    valid(result);
    return result;
}
}

std::vector<ShortcutBinding> apex_operation_preset() {
    return {{"Point Jump","boundary.point_jump"},{"Auto Close","boundary.auto_close"},
        {"Bay Window","boundary.bay_window"},{"Rotate","boundary.rotate"},
        {"Flip Horizontal","boundary.flip_horizontal"},{"Flip Vertical","boundary.flip_vertical"},
        {"Insert Vertex","boundary.insert_vertex"},{"Clone","boundary.clone"}};
}
std::string_view apex_command_id(std::string_view name) {
    static const auto bindings=apex_operation_preset();
    for (const auto& binding:bindings) if (binding.shortcut==name) return binding.command_id;
    return {};
}
std::vector<std::string> shortcut_conflicts(const std::vector<ShortcutBinding>& bindings) {
    std::map<std::string,std::size_t> counts;
    for (const auto& binding:bindings) {
        if (binding.shortcut.empty() || binding.command_id.empty())
            throw std::invalid_argument("Shortcut and command must be nonempty");
        ++counts[binding.shortcut];
    }
    std::vector<std::string> result;
    for (const auto& [key,count]:counts) if(count>1) result.push_back(key);
    return result;
}
IdentifiedBoundary rotate_boundary(const IdentifiedBoundary& source,Vec2 pivot,double radians) {
    finite(pivot);
    if (!std::isfinite(radians)) throw std::invalid_argument("Rotation must be finite");
    const auto c=std::cos(radians),s=std::sin(radians);
    return transform(source,[=](Vec2 p) { const auto x=p.x-pivot.x,y=p.y-pivot.y;
        return Vec2{pivot.x+x*c-y*s,pivot.y+x*s+y*c}; });
}
IdentifiedBoundary flip_boundary(const IdentifiedBoundary& source,Vec2 pivot,BoundaryFlipAxis axis) {
    finite(pivot);
    if (axis!=BoundaryFlipAxis::horizontal && axis!=BoundaryFlipAxis::vertical)
        throw std::invalid_argument("Unsupported flip axis");
    return transform(source,[=](Vec2 p) { return axis==BoundaryFlipAxis::horizontal
        ? Vec2{p.x,pivot.y-(p.y-pivot.y)} : Vec2{pivot.x-(p.x-pivot.x),p.y}; },true);
}
IdentifiedBoundary clone_boundary(const IdentifiedBoundary& source,std::string new_id,
    const LegacyBoundaryIdentityOptions& ids,Vec2 translation) {
    finite(translation);
    valid(source);
    if(ids.segment_ids.size()!=source.segments.size() || ids.vertex_ids.size()!=source.segments.size())
        throw std::invalid_argument("Clone requires one new segment and vertex ID per edge");
    if(new_id==source.id) throw std::invalid_argument("Clone must have a new boundary ID");
    std::set<std::string> old_edges,old_vertices;
    for (const auto& e:source.segments) { old_edges.insert(e.segment_id); old_vertices.insert(e.start_vertex_id); }
    auto result=transform(source,[=](Vec2 p){return Vec2{p.x+translation.x,p.y+translation.y};});
    result.id=std::move(new_id);
    for(std::size_t i=0;i<result.segments.size();++i) {
        if(old_edges.contains(ids.segment_ids[i]) || old_vertices.contains(ids.vertex_ids[i]))
            throw std::invalid_argument("Clone identities must be distinct from the source");
        auto& e=result.segments[i];
        e.segment_id=ids.segment_ids[i]; e.start_vertex_id=ids.vertex_ids[i];
        e.end_vertex_id=ids.vertex_ids[(i+1)%ids.vertex_ids.size()];
    }
    valid(result);
    return result;
}
Vec2 jump_to_boundary_vertex(const IdentifiedBoundary& source,std::string_view id) {
    valid(source);
    for(const auto& e:source.segments) if(e.start_vertex_id==id) return e.segment.start;
    throw std::invalid_argument("Unknown vertex ID");
}
Boundary automatically_close_boundary(const Boundary& source, double tolerance_metres) {
    if(source.empty()) throw std::invalid_argument("Cannot close an empty chain");
    Boundary result=source;
    for(std::size_t i=1;i<result.size();++i)
        if(!same(result[i-1].end,result[i].start)) throw std::invalid_argument("Chain must join exactly");
    if(!same(result.back().end,result.front().start)) result.push_back({result.back().end,result.front().start,0});
    const auto diagnostics=validate_boundary(result,tolerance_metres);
    if(!diagnostics.empty()) throw std::invalid_argument(diagnostics.front().message);
    return result;
}
Boundary complete_bay_window(Vec2 start,Vec2 shoulder1,Vec2 shoulder2,Vec2 end) {
    finite(start);finite(shoulder1);finite(shoulder2);finite(end);
    Boundary result{{start,shoulder1,0},{shoulder1,shoulder2,0},{shoulder2,end,0}};
    (void)automatically_close_boundary(result);
    // Both shoulders must progress along the opening and lie on the same side.
    const auto dx=end.x-start.x,dy=end.y-start.y;
    const auto length2=dx*dx+dy*dy;
    const auto t1=((shoulder1.x-start.x)*dx+(shoulder1.y-start.y)*dy)/length2;
    const auto t2=((shoulder2.x-start.x)*dx+(shoulder2.y-start.y)*dy)/length2;
    const auto h1=dx*(shoulder1.y-start.y)-dy*(shoulder1.x-start.x);
    const auto h2=dx*(shoulder2.y-start.y)-dy*(shoulder2.x-start.x);
    if(!std::isfinite(length2) || !(0<t1 && t1<t2 && t2<1) ||
        !((h1>0 && h2>0)||(h1<0 && h2<0))) throw std::invalid_argument("Invalid bay shoulders");
    return result;
}

Segment complete_bay_window_return(const Segment& entering, const Segment& front) {
    finite(entering.start); finite(entering.end);
    finite(front.start); finite(front.end);
    if (!std::isfinite(entering.sweep_radians) || !std::isfinite(front.sweep_radians) ||
        entering.sweep_radians != 0.0 || front.sweep_radians != 0.0) {
        throw std::invalid_argument("Bay completion requires straight edges");
    }
    if (!same(entering.end, front.start)) {
        throw std::invalid_argument("Bay edges must join exactly");
    }
    const Vec2 entering_direction{entering.end.x - entering.start.x,
                                  entering.end.y - entering.start.y};
    const Vec2 front_direction{front.end.x - front.start.x,
                               front.end.y - front.start.y};
    finite(entering_direction); finite(front_direction);
    const auto entering_length = std::hypot(entering_direction.x, entering_direction.y);
    const auto front_length = std::hypot(front_direction.x, front_direction.y);
    if (!std::isfinite(entering_length) || !std::isfinite(front_length) ||
        !(entering_length > default_geometry_tolerance_metres) ||
        !(front_length > default_geometry_tolerance_metres)) {
        throw std::invalid_argument("Bay edge length cannot be represented or is too small");
    }
    const Vec2 front_unit{front_direction.x / front_length, front_direction.y / front_length};
    // Check progression before normalization. Rounding a perpendicular pair's
    // normalized dot can otherwise create a tiny positive shoulder projection
    // and admit a rectangle as an angled bay. Ambiguous cancellation is not
    // sufficient evidence of positive progression.
    const auto along_x = entering_direction.x * front_direction.x;
    const auto along_y = entering_direction.y * front_direction.y;
    const auto progress = along_x + along_y;
    const auto progress_error = 8.0 * std::numeric_limits<double>::epsilon() *
        (std::abs(along_x) + std::abs(along_y));
    if (!std::isfinite(progress) || !std::isfinite(progress_error) ||
        !(progress > progress_error)) {
        throw std::invalid_argument("Bay sides must progress along the front");
    }
    const auto doubled_projection = 2.0 *
        (entering_direction.x * front_unit.x + entering_direction.y * front_unit.y);
    if (!std::isfinite(doubled_projection)) {
        throw std::invalid_argument("Bay return direction cannot be represented");
    }
    const Vec2 return_direction{doubled_projection * front_unit.x - entering_direction.x,
                                doubled_projection * front_unit.y - entering_direction.y};
    finite(return_direction);
    const Vec2 end{front.end.x + return_direction.x, front.end.y + return_direction.y};
    finite(end);
    const auto bay = complete_bay_window(entering.start, entering.end, front.end, end);
    return bay.back();
}


} // namespace sketch
