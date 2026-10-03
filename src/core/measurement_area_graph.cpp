#include "sketch/measurement_area_graph.hpp"
#include "sketch/geometry_operations.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
constexpr double full_turn = 2 * std::numbers::pi;
constexpr double roundoff = 128 * std::numeric_limits<double>::epsilon();
constexpr std::size_t maximum_sources = 2048;
constexpr std::size_t maximum_edges = 16384;
constexpr std::size_t maximum_contacts = maximum_edges * 4;

[[noreturn]] void fail(const std::string& message) {
    throw std::invalid_argument("Measurement area graph: " + message);
}
Vec2 minus(Vec2 a, Vec2 b) { return {a.x-b.x,a.y-b.y}; }
double distance(Vec2 a, Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y); }
bool same(Vec2 a, Vec2 b) { return a.x==b.x && a.y==b.y; }
bool point_less(Vec2 a, Vec2 b) { return std::tie(a.x,a.y)<std::tie(b.x,b.y); }
bool same(const Segment& a, const Segment& b) {
    return same(a.start,b.start) && same(a.end,b.end) && a.sweep_radians==b.sweep_radians;
}
Segment reverse(Segment segment) {
    std::swap(segment.start,segment.end);
    segment.sweep_radians=-segment.sweep_radians;
    return segment;
}
bool geometry_less(const Segment& a, const Segment& b) {
    return std::tie(a.start.x,a.start.y,a.end.x,a.end.y,a.sweep_radians)<
        std::tie(b.start.x,b.start.y,b.end.x,b.end.y,b.sweep_radians);
}
double cross(Vec2 a, Vec2 b) {
    // Keep the product tail so exactly parallel represented vectors remain
    // distinguishable from nearby, genuinely intersecting measurements.
    const double product=a.y*b.x;
    return std::fma(a.x,b.y,-product)+std::fma(-a.y,b.x,product);
}
double positive_angle(double angle) {
    double result=std::fmod(angle,full_turn);
    if(result<0) result+=full_turn;
    return result;
}
struct Circle { Vec2 offset; double radius; double start_angle; };
Circle circle(const Segment& segment) {
    const auto chord=minus(segment.end,segment.start);
    const double length=std::hypot(chord.x,chord.y);
    const bool half_turn=std::abs(segment.sweep_radians)==std::numbers::pi;
    const double offset=half_turn?0:length/(2*std::tan(segment.sweep_radians/2));
    Circle result{{chord.x/2-chord.y/length*offset,chord.y/2+chord.x/length*offset},
        length/(2*std::sin(std::abs(segment.sweep_radians)/2)),0};
    result.start_angle=std::atan2(-result.offset.y,-result.offset.x);
    if(!std::isfinite(result.offset.x)||!std::isfinite(result.offset.y)||
       !std::isfinite(result.radius)||result.radius>length*1e8)
        fail("arc center is too ill-conditioned for reliable analytical noding");
    return result;
}
double numerical_tolerance(const Segment& a, const Segment& b) {
    return roundoff*std::max({1.0,segment_length(a),segment_length(b)});
}
bool coincident_circles(const Segment& a, const Segment& b, double epsilon) {
    const auto ca=circle(a), cb=circle(b);
    const auto offset=minus(b.start,a.start);
    const Vec2 centers{offset.x+cb.offset.x-ca.offset.x,offset.y+cb.offset.y-ca.offset.y};
    return std::hypot(centers.x,centers.y)<=epsilon && std::abs(ca.radius-cb.radius)<=epsilon;
}
// This is an arithmetic error band, never the caller's metre tolerance.
bool parameter_on(const Segment& segment, Vec2 point, double epsilon, double& parameter) {
    if(same(point,segment.start)) { parameter=0; return true; }
    if(same(point,segment.end)) { parameter=1; return true; }
    const auto chord=minus(segment.end,segment.start);
    if(segment.sweep_radians==0) {
        const double length=std::hypot(chord.x,chord.y);
        const auto delta=minus(point,segment.start);
        const Vec2 unit{chord.x/length,chord.y/length};
        parameter=std::fma(delta.x,unit.x,delta.y*unit.y)/length;
        if(std::abs(cross(delta,unit))>epsilon) return false;
    } else {
        const auto c=circle(segment);
        const auto delta=minus(point,segment.start);
        const Vec2 radial{delta.x-c.offset.x,delta.y-c.offset.y};
        if(std::abs(std::hypot(radial.x,radial.y)-c.radius)>epsilon) return false;
        const double angle=std::atan2(radial.y,radial.x);
        const double travel=segment.sweep_radians>0?positive_angle(angle-c.start_angle):
            positive_angle(c.start_angle-angle);
        parameter=travel/std::abs(segment.sweep_radians);
    }
    if(distance(point,segment.start)<=epsilon) { parameter=0; return true; }
    if(distance(point,segment.end)<=epsilon) { parameter=1; return true; }
    const double band=epsilon/segment_length(segment);
    if(!std::isfinite(parameter)||parameter < -band || parameter > 1+band) return false;
    parameter=std::clamp(parameter,0.0,1.0);
    return true;
}
Vec2 point_at(const Segment& segment, double parameter) {
    if(parameter==0) return segment.start;
    if(parameter==1) return segment.end;
    if(segment.sweep_radians==0)
        return {std::lerp(segment.start.x,segment.end.x,parameter),
                std::lerp(segment.start.y,segment.end.y,parameter)};
    const auto c=circle(segment);
    const double angle=segment.sweep_radians*parameter;
    const double sine=std::sin(angle), cosine=std::cos(angle);
    return {segment.start.x+c.offset.x-c.offset.x*cosine+c.offset.y*sine,
            segment.start.y+c.offset.y-c.offset.x*sine-c.offset.y*cosine};
}
struct Cut { double parameter; Vec2 point; };

void line_contacts(const Segment& a, const Segment& b, std::vector<Vec2>& points) {
    const auto r=minus(a.end,a.start), s=minus(b.end,b.start), q=minus(b.start,a.start);
    const double denominator=cross(r,s);
    if(!std::isfinite(denominator)) fail("line intersection exceeds numeric range");
    if(denominator==0) {
        // No geometric tolerance here: close parallel lines stay distinct.
        if(cross(q,r)!=0) return;
        const auto within=[](const Segment& segment,Vec2 point) {
            return point.x>=std::min(segment.start.x,segment.end.x)&&
                point.x<=std::max(segment.start.x,segment.end.x)&&
                point.y>=std::min(segment.start.y,segment.end.y)&&
                point.y<=std::max(segment.start.y,segment.end.y);
        };
        for(const auto point:{a.start,a.end,b.start,b.end})
            if(within(a,point)&&within(b,point)) points.push_back(point);
        return;
    }
    const double error=roundoff*(std::abs(r.x*s.y)+std::abs(r.y*s.x));
    if(std::abs(denominator)<=error) fail("nearly parallel line intersection is numerically ambiguous");
    for(const auto point:{a.start,a.end})
        if(same(point,b.start)||same(point,b.end)) { points.push_back(point); return; }
    const double t=cross(q,s)/denominator, u=cross(q,r)/denominator;
    if(!std::isfinite(t)||!std::isfinite(u)) fail("line parameters exceed numeric range");
    if(t<0||t>1||u<0||u>1) return;
    if(t==0) points.push_back(a.start);
    else if(t==1) points.push_back(a.end);
    else if(u==0) points.push_back(b.start);
    else if(u==1) points.push_back(b.end);
    else points.push_back({std::fma(r.x,t,a.start.x),std::fma(r.y,t,a.start.y)});
}

bool equivalent_piece(const Segment& a, const Segment& b) {
    if(!same(a.start,b.start)||!same(a.end,b.end)) return false;
    if(a.sweep_radians==0||b.sweep_radians==0) return a.sweep_radians==b.sweep_radians;
    if (a.sweep_radians != b.sweep_radians && std::abs(a.sweep_radians-b.sweep_radians)<=roundoff)
        fail("distinct near-coincident derived sweeps cannot be merged into one measured edge");
    return std::abs(a.sweep_radians-b.sweep_radians)<=roundoff &&
        coincident_circles(a,b,numerical_tolerance(a,b));
}

// The existing face detector requires a simple endpoint graph. Split distinct
// analytical edges sharing an endpoint pair; this also represents two-edge
// lenses as ordinary four-edge cycles without polygonizing their arcs.
void split_parallel_edges(std::vector<DerivedMeasurementEdge>& edges) {
    std::map<std::tuple<double,double,double,double>,std::size_t> counts;
    for(const auto& edge:edges) ++counts[{edge.geometry.start.x,edge.geometry.start.y,
        edge.geometry.end.x,edge.geometry.end.y}];
    std::vector<DerivedMeasurementEdge> result;
    for(const auto& edge:edges) {
        const auto& g=edge.geometry;
        if(counts[{g.start.x,g.start.y,g.end.x,g.end.y}]==1) { result.push_back(edge); continue; }
        const auto middle=point_at(g,.5);
        for(int piece=0;piece<2;++piece) {
            DerivedMeasurementEdge half{{piece==0?g.start:middle,piece==0?middle:g.end,g.sweep_radians/2},edge.source_uses};
            for(auto& use:half.source_uses) {
                const double mid=std::midpoint(use.parameter_start,use.parameter_end);
                if((piece==0)!=use.reversed) use.parameter_end=mid;
                else use.parameter_start=mid;
            }
            if(point_less(half.geometry.end,half.geometry.start)) {
                half.geometry=reverse(half.geometry);
                for(auto& use:half.source_uses) use.reversed=!use.reversed;
            }
            result.push_back(std::move(half));
        }
    }
    edges=std::move(result);
}

struct Topology {
    std::vector<Vec2> nodes;
    std::vector<std::pair<std::size_t,std::size_t>> ends;
    std::vector<bool> bridges;
    std::size_t bounded_faces{};
};
Topology topology(const std::vector<DerivedMeasurementEdge>& edges) {
    Topology result;
    std::map<std::pair<double,double>,std::size_t> node_ids;
    const auto node=[&](Vec2 point) {
        const auto key=std::pair{point.x,point.y};
        const auto found=node_ids.find(key);
        if(found!=node_ids.end()) return found->second;
        const auto id=result.nodes.size();
        result.nodes.push_back(point); node_ids.emplace(key,id); return id;
    };
    for(const auto& edge:edges) result.ends.push_back({node(edge.geometry.start),node(edge.geometry.end)});
    std::vector<std::vector<std::size_t>> incident(result.nodes.size());
    for(std::size_t i=0;i<edges.size();++i) {
        incident[result.ends[i].first].push_back(i); incident[result.ends[i].second].push_back(i);
    }
    result.bridges.assign(edges.size(),false);
    const auto absent=std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> discovery(result.nodes.size(),absent), low(result.nodes.size()),
        parent_edge(result.nodes.size(),absent), positions(result.nodes.size());
    std::size_t tick=0, components=0;
    for(std::size_t seed=0;seed<result.nodes.size();++seed) {
        if(discovery[seed]!=absent) continue;
        ++components;
        std::vector<std::size_t> stack{seed};
        discovery[seed]=low[seed]=tick++;
        while(!stack.empty()) {
            const auto current=stack.back();
            if(positions[current]<incident[current].size()) {
                const auto edge=incident[current][positions[current]++];
                if(edge==parent_edge[current]) continue;
                const auto [a,b]=result.ends[edge]; const auto next=a==current?b:a;
                if(discovery[next]==absent) {
                    parent_edge[next]=edge; discovery[next]=low[next]=tick++; stack.push_back(next);
                } else low[current]=std::min(low[current],discovery[next]);
            } else {
                stack.pop_back();
                if(parent_edge[current]!=absent) {
                    const auto edge=parent_edge[current]; const auto [a,b]=result.ends[edge];
                    const auto parent=a==current?b:a;
                    if(low[current]>discovery[parent]) result.bridges[edge]=true;
                    low[parent]=std::min(low[parent],low[current]);
                }
            }
        }
    }
    result.bounded_faces=edges.size()+components-result.nodes.size();
    return result;
}
} // namespace

MeasurementAreaGraph build_measurement_area_graph(
    const std::vector<MeasurementGraphSource>& inputs, double tolerance) {
    if(!std::isfinite(tolerance)||tolerance<=0) fail("tolerance must be finite and positive");
    if(inputs.size()>maximum_sources) fail("source limit of 2048 segments exceeded");
    auto sources=inputs;
    std::sort(sources.begin(),sources.end(),[](const auto& a,const auto& b) {
        return std::tie(a.owner_id,a.segment_id)<std::tie(b.owner_id,b.segment_id);
    });
    const double minimum_length=std::max(tolerance,default_geometry_tolerance_metres);
    std::vector<std::vector<Cut>> cuts(sources.size());
    // Two ordered coordinates bound station lookup to a local roundoff box.
    // An all-pairs contact graph must not linearly scan every earlier station.
    std::map<double, std::map<double, Vec2>> nodes;
    std::size_t node_count = 0, contact_count = 0;
    const auto add_node = [&](Vec2 point) {
        auto& column = nodes[point.x];
        if (column.contains(point.y)) return;
        if (++node_count > maximum_edges) fail("represented station limit of 16384 exceeded");
        column.emplace(point.y, point);
    };
    for(std::size_t i=0;i<sources.size();++i) {
        const auto& source=sources[i];
        if(source.owner_id.empty()||source.segment_id.empty()) fail("owner and segment IDs must be nonempty");
        if(i>0&&source.owner_id==sources[i-1].owner_id&&source.segment_id==sources[i-1].segment_id)
            fail("source owner/segment IDs must be unique");
        (void)segment_bounds(source.geometry);
        if(!(segment_length(source.geometry)>minimum_length)) fail("source segment is too small for reliable face extraction");
        if(source.geometry.sweep_radians!=0) (void)circle(source.geometry);
        cuts[i]={{0,source.geometry.start},{1,source.geometry.end}};
        for(const auto point:{source.geometry.start,source.geometry.end}) add_node(point);
    }
    for(std::size_t i=0;i<sources.size();++i) for(std::size_t j=i+1;j<sources.size();++j) {
        const auto& a=sources[i].geometry; const auto& b=sources[j].geometry;
        const double epsilon=numerical_tolerance(a,b);
        const auto directed_b = same(a.start,b.end) && same(a.end,b.start) ? reverse(b) : b;
        if (same(a.start,directed_b.start) && same(a.end,directed_b.end) &&
            a.sweep_radians != directed_b.sweep_radians &&
            std::abs(a.sweep_radians-directed_b.sweep_radians) <= roundoff)
            fail("distinct near-coincident sweeps cannot be merged into one measured edge");
        std::vector<Vec2> contacts;
        if(a.sweep_radians==0&&b.sweep_radians==0) line_contacts(a,b,contacts);
        else if(a.sweep_radians!=0&&b.sweep_radians!=0&&coincident_circles(a,b,epsilon)) {
            double t=0;
            for(const auto point:{a.start,a.end,b.start,b.end})
                if(parameter_on(a,point,epsilon,t)&&parameter_on(b,point,epsilon,t)) contacts.push_back(point);
        } else {
            const auto intersection=segment_intersection(a,b,epsilon);
            if(intersection.kind==SegmentIntersectionKind::indeterminate)
                fail("indeterminate analytical intersection between "+sources[i].segment_id+" and "+sources[j].segment_id);
            if(intersection.kind==SegmentIntersectionKind::overlap)
                fail("near-coincident curves cannot be proven to share their supporting circle");
            contacts=intersection.points;
        }
        for(auto point:contacts) {
            if (++contact_count > maximum_contacts)
                fail("contact work budget of 65536 exceeded; reduce the selected linework graph");
            double ta=0,tb=0;
            if(!parameter_on(a,point,epsilon,ta)||!parameter_on(b,point,epsilon,tb))
                fail("contact does not lie on both analytical source segments");
            // Reuse an already represented station only inside the arithmetic
            // band, after verifying it still lies on both measured supports.
            bool reused = false;
            for (auto column=nodes.lower_bound(point.x-epsilon);
                 column!=nodes.end() && column->first<=point.x+epsilon && !reused; ++column) {
                for (auto station=column->second.lower_bound(point.y-epsilon);
                     station!=column->second.end() && station->first<=point.y+epsilon; ++station) {
                    const auto old=station->second;
                    if(distance(old,point)>epsilon) continue;
                    double old_a=0,old_b=0;
                    if(!parameter_on(a,old,epsilon,old_a)||!parameter_on(b,old,epsilon,old_b))
                        fail("nearby intersection stations are numerically ambiguous");
                    point=old; ta=old_a; tb=old_b; reused=true; break;
                }
            }
            add_node(point);
            cuts[i].push_back({ta,point}); cuts[j].push_back({tb,point});
        }
    }
    MeasurementAreaGraph graph;
    for(std::size_t i=0;i<sources.size();++i) {
        auto& stations=cuts[i];
        std::sort(stations.begin(),stations.end(),[](const auto& a,const auto& b){return a.parameter<b.parameter;});
        std::vector<Cut> unique;
        for(const auto& cut:stations) {
            if(!unique.empty()&&cut.parameter==unique.back().parameter) {
                if(!same(cut.point,unique.back().point)) fail("distinct stations have indistinguishable source parameters");
            } else unique.push_back(cut);
        }
        for(std::size_t k=1;k<unique.size();++k) {
            const auto& lo=unique[k-1]; const auto& hi=unique[k];
            Segment piece{lo.point,hi.point,sources[i].geometry.sweep_radians*(hi.parameter-lo.parameter)};
            if(!(segment_length(piece)>minimum_length)) fail("intersection produced a piece too small for reliable face extraction");
            bool reversed=false;
            if(point_less(piece.end,piece.start)) { piece=reverse(piece); reversed=true; }
            MeasurementSourceUse use{sources[i].owner_id,sources[i].segment_id,lo.parameter,hi.parameter,reversed};
            auto existing=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& edge){return equivalent_piece(edge.geometry,piece);});
            if(existing==graph.edges.end()) graph.edges.push_back({piece,{std::move(use)}});
            else existing->source_uses.push_back(std::move(use));
            if(graph.edges.size()>maximum_edges) fail("derived edge limit of 16384 exceeded");
        }
    }
    split_parallel_edges(graph.edges);
    if(graph.edges.size()>maximum_edges) fail("derived edge limit of 16384 exceeded");
    for(const auto& edge:graph.edges)
        if(!(segment_length(edge.geometry)>minimum_length)) fail("lens subdivision produced an unresolvable small piece");
    std::sort(graph.edges.begin(),graph.edges.end(),[](const auto& a,const auto& b){return geometry_less(a.geometry,b.geometry);});
    const auto indexed=topology(graph.edges);
    std::vector<Segment> face_segments;
    for(std::size_t i=0;i<graph.edges.size();++i) if(!indexed.bridges[i]) face_segments.push_back(graph.edges[i].geometry);
    auto boundaries=detect_closed_boundaries(face_segments);
    if(boundaries.size()!=indexed.bounded_faces)
        fail("bounded faces cannot be reliably extracted; tiny, tangent or ambiguous topology requires correction");
    for(std::size_t i=0;i<boundaries.size();++i) for(std::size_t j=i+1;j<boundaries.size();++j)
        if(!validate_boundary_holes(boundaries[i],{boundaries[j]}).has_value()||
           !validate_boundary_holes(boundaries[j],{boundaries[i]}).has_value())
            fail("nested disconnected cycles require hole topology, which this graph does not support");
    for(auto& boundary:boundaries) {
        DerivedMeasurementFace face;
        face.area_square_metres=signed_area(boundary);
        for(const auto& segment:boundary) {
            bool found=false;
            for(std::size_t i=0;i<graph.edges.size();++i) {
                if(same(segment,graph.edges[i].geometry)) { face.edge_uses.push_back({i,false}); found=true; break; }
                if(same(segment,reverse(graph.edges[i].geometry))) { face.edge_uses.push_back({i,true}); found=true; break; }
            }
            if(!found) fail("face edge lost its derived source lineage");
        }
        face.boundary=std::move(boundary); graph.faces.push_back(std::move(face));
    }
    return graph;
}
} // namespace sketch
