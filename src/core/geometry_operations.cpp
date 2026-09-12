#include "sketch/geometry_operations.hpp"

#include <algorithm>
#include <cmath>
#include <map>
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
IdentifiedBoundary insert_boundary_vertex(const IdentifiedBoundary& source,std::string_view id,
    double fraction,std::string vertex_id,std::string second_id) {
    valid(source);
    if (!std::isfinite(fraction) || fraction<=0 || fraction>=1)
        throw std::invalid_argument("Insertion fraction must be inside (0,1)");
    auto result=source;
    auto found=std::find_if(result.segments.begin(),result.segments.end(),[&](const auto& e){return e.segment_id==id;});
    if(found==result.segments.end()) throw std::invalid_argument("Unknown segment ID");
    const auto original=*found;
    const auto& s=original.segment;
    Vec2 p{std::lerp(s.start.x,s.end.x,fraction),std::lerp(s.start.y,s.end.y,fraction)};
    if(s.sweep_radians!=0) {
        const auto dx=s.end.x-s.start.x,dy=s.end.y-s.start.y;
        const auto k=0.5/std::tan(s.sweep_radians/2);
        const Vec2 center{s.start.x+dx/2-dy*k,s.start.y+dy/2+dx*k};
        const auto a=s.sweep_radians*fraction,c=std::cos(a),sn=std::sin(a);
        const auto x=s.start.x-center.x,y=s.start.y-center.y;
        p={center.x+x*c-y*sn,center.y+x*sn+y*c};
    }
    finite(p);
    found->end_vertex_id=vertex_id;
    found->segment.end=p;
    found->segment.sweep_radians=s.sweep_radians*fraction;
    result.segments.insert(found+1,{std::move(second_id),std::move(vertex_id),original.end_vertex_id,
        {p,s.end,s.sweep_radians*(1-fraction)}});
    valid(result);
    return result;
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
Boundary automatically_close_boundary(const Boundary& source) {
    if(source.empty()) throw std::invalid_argument("Cannot close an empty chain");
    Boundary result=source;
    for(std::size_t i=1;i<result.size();++i)
        if(!same(result[i-1].end,result[i].start)) throw std::invalid_argument("Chain must join exactly");
    if(!same(result.back().end,result.front().start)) result.push_back({result.back().end,result.front().start,0});
    const auto diagnostics=validate_boundary(result);
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

Boundary assemble_boundary_from_segments(const std::vector<Segment>& segments,
                                          std::size_t seed_index) {
    if (segments.size() < 3) {
        throw std::invalid_argument("At least three existing segments are required");
    }
    if (seed_index >= segments.size()) {
        throw std::invalid_argument("Existing-segment seed index is out of range");
    }

    struct IndexedSegment {
        Segment segment;
        std::size_t start_node{};
        std::size_t end_node{};
    };
    std::vector<Vec2> nodes;
    nodes.reserve(segments.size());
    const auto node_for = [&nodes](Vec2 point) {
        finite(point);
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](Vec2 candidate) {
            return same(candidate, point);
        });
        if (found != nodes.end()) {
            return static_cast<std::size_t>(found - nodes.begin());
        }
        nodes.push_back(point);
        return nodes.size() - 1;
    };

    std::vector<IndexedSegment> indexed;
    indexed.reserve(segments.size());
    for (const auto& segment : segments) {
        if (!std::isfinite(segment.sweep_radians)) {
            throw std::invalid_argument("Existing segment sweep must be finite");
        }
        const auto start_node = node_for(segment.start);
        const auto end_node = node_for(segment.end);
        if (start_node == end_node) {
            throw std::invalid_argument("Existing segment endpoints must be distinct");
        }
        try {
            if (!(segment_length(segment) > default_geometry_tolerance_metres)) {
                throw std::invalid_argument("Existing segment length is too small");
            }
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument(std::string("Existing segment is invalid: ") + error.what());
        }
        indexed.push_back({segment, start_node, end_node});
    }

    std::vector<std::vector<std::size_t>> adjacency(nodes.size());
    for (std::size_t index = 0; index < indexed.size(); ++index) {
        const auto& edge = indexed[index];
        adjacency[edge.start_node].push_back(index);
        adjacency[edge.end_node].push_back(index);
    }
    for (const auto& incident : adjacency) {
        if (incident.size() != 2) {
            throw std::invalid_argument(
                "Existing segments must form one closed cycle with exactly two edges at each vertex");
        }
    }

    const auto reverse_segment = [](Segment segment) {
        std::swap(segment.start, segment.end);
        segment.sweep_radians = -segment.sweep_radians;
        return segment;
    };
    Boundary result;
    result.reserve(indexed.size());
    std::vector<bool> visited(indexed.size(), false);
    const auto append = [&](std::size_t edge_index, std::size_t from_node,
                            std::size_t& next_node) {
        if (edge_index >= indexed.size() || visited[edge_index]) {
            throw std::invalid_argument("Existing segments do not form one ordered cycle");
        }
        const auto& edge = indexed[edge_index];
        if (edge.start_node == from_node) {
            result.push_back(edge.segment);
            next_node = edge.end_node;
        } else if (edge.end_node == from_node) {
            result.push_back(reverse_segment(edge.segment));
            next_node = edge.start_node;
        } else {
            throw std::invalid_argument("Existing segment adjacency is inconsistent");
        }
        visited[edge_index] = true;
    };

    const auto start_node = indexed[seed_index].start_node;
    std::size_t current_node = start_node;
    append(seed_index, current_node, current_node);
    while (current_node != start_node) {
        const auto& incident = adjacency[current_node];
        const auto next = std::find_if(incident.begin(), incident.end(),
                                       [&](std::size_t edge) { return !visited[edge]; });
        if (next == incident.end()) {
            throw std::invalid_argument("Existing segments leave an open boundary");
        }
        append(*next, current_node, current_node);
        if (result.size() > indexed.size()) {
            throw std::invalid_argument("Existing segments do not form one ordered cycle");
        }
    }
    if (std::any_of(visited.begin(), visited.end(), [](bool value) { return !value; })) {
        throw std::invalid_argument("Existing segments contain a disconnected cycle");
    }
    const auto diagnostics = validate_boundary(result);
    if (!diagnostics.empty()) {
        throw std::invalid_argument("Existing geometry cannot form a valid boundary: " +
                                    diagnostics.front().message);
    }
    return result;
}
} // namespace sketch
