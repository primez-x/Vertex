#include "sketch/closed_boundary_detection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
void finite(Vec2 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw std::invalid_argument("Point must be finite");
}
bool same(Vec2 a, Vec2 b) { return a.x==b.x && a.y==b.y; }

constexpr double full_turn = 2.0 * std::numbers::pi;

double positive_angle(double value) {
    auto result = std::fmod(value, full_turn);
    if (result < 0.0) result += full_turn;
    return result;
}

double outgoing_angle(const Segment& segment) {
    if (segment.sweep_radians == 0.0) {
        return positive_angle(std::atan2(segment.end.y - segment.start.y,
                                         segment.end.x - segment.start.x));
    }
    const auto dx = segment.end.x - segment.start.x;
    const auto dy = segment.end.y - segment.start.y;
    const auto chord = std::hypot(dx, dy);
    const auto half_sweep = segment.sweep_radians * 0.5;
    const auto tangent = std::tan(half_sweep);
    if (!(chord > 0.0) || !std::isfinite(chord) || tangent == 0.0 || !std::isfinite(tangent)) {
        throw std::invalid_argument("Segment arc tangent cannot be represented");
    }
    const Vec2 midpoint{(segment.start.x + segment.end.x) * 0.5,
                        (segment.start.y + segment.end.y) * 0.5};
    const auto center_offset = chord / (2.0 * tangent);
    const Vec2 center{midpoint.x - dy / chord * center_offset,
                      midpoint.y + dx / chord * center_offset};
    const Vec2 radial{segment.start.x - center.x, segment.start.y - center.y};
    const auto radial_angle = std::atan2(radial.y, radial.x);
    const auto tangent_angle = radial_angle +
        (segment.sweep_radians > 0.0 ? std::numbers::pi / 2.0 : -std::numbers::pi / 2.0);
    return positive_angle(tangent_angle);
}

Segment reverse_segment(Segment segment) {
    std::swap(segment.start, segment.end);
    segment.sweep_radians = -segment.sweep_radians;
    return segment;
}

struct DetectedFace {
    Boundary boundary;
    std::size_t first_source_edge{};
    double area{};
};

void rotate_boundary_to_stable_start(Boundary& boundary) {
    if (boundary.empty()) return;
    const auto first = std::min_element(boundary.begin(), boundary.end(), [](const auto& left,
                                                                              const auto& right) {
        if (left.start.x != right.start.x) return left.start.x < right.start.x;
        if (left.start.y != right.start.y) return left.start.y < right.start.y;
        if (left.end.x != right.end.x) return left.end.x < right.end.x;
        return left.end.y < right.end.y;
    });
    std::rotate(boundary.begin(), first, boundary.end());
}

} // namespace

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

std::vector<Boundary> detect_closed_boundaries(const std::vector<Segment>& segments) {
    if (segments.empty()) return {};

    struct IndexedSegment {
        Segment segment;
        std::size_t start_node{};
        std::size_t end_node{};
    };
    std::vector<Vec2> nodes;
    nodes.reserve(segments.size() * 2);
    const auto node_for = [&nodes](Vec2 point) {
        finite(point);
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](Vec2 candidate) {
            return same(candidate, point);
        });
        if (found != nodes.end()) return static_cast<std::size_t>(found - nodes.begin());
        nodes.push_back(point);
        return nodes.size() - 1;
    };

    std::vector<IndexedSegment> indexed;
    indexed.reserve(segments.size());
    std::set<std::pair<std::size_t, std::size_t>> undirected_edges;
    for (const auto& segment : segments) {
        if (!std::isfinite(segment.sweep_radians)) {
            throw std::invalid_argument("Detected segment sweep must be finite");
        }
        const auto start_node = node_for(segment.start);
        const auto end_node = node_for(segment.end);
        if (start_node == end_node) {
            throw std::invalid_argument("Detected segment endpoints must be distinct");
        }
        if (!(segment_length(segment) > default_geometry_tolerance_metres)) {
            throw std::invalid_argument("Detected segment length is too small");
        }
        const auto edge_key = std::minmax(start_node, end_node);
        if (!undirected_edges.insert(edge_key).second) {
            throw std::invalid_argument("Detected segment graph contains duplicate geometry");
        }
        indexed.push_back({segment, start_node, end_node});
    }

    struct DirectedEdge {
        std::size_t source_index{};
        std::size_t from_node{};
        std::size_t to_node{};
        bool reversed{};
        double angle{};
        std::size_t reverse_index{};
    };
    std::vector<DirectedEdge> directed;
    directed.reserve(indexed.size() * 2);
    std::vector<std::vector<std::size_t>> outgoing(nodes.size());
    for (std::size_t index = 0; index < indexed.size(); ++index) {
        const auto& item = indexed[index];
        const auto forward = directed.size();
        directed.push_back({index, item.start_node, item.end_node, false,
                            outgoing_angle(item.segment), forward + 1});
        directed.push_back({index, item.end_node, item.start_node, true,
                            outgoing_angle(reverse_segment(item.segment)), forward});
        outgoing[item.start_node].push_back(forward);
        outgoing[item.end_node].push_back(forward + 1);
    }
    for (auto& incident : outgoing) {
        std::sort(incident.begin(), incident.end(), [&](std::size_t left, std::size_t right) {
            const auto& a = directed[left];
            const auto& b = directed[right];
            if (a.angle != b.angle) return a.angle < b.angle;
            if (a.source_index != b.source_index) return a.source_index < b.source_index;
            return a.reversed < b.reversed;
        });
    }

    std::vector<std::size_t> next(directed.size());
    for (std::size_t index = 0; index < directed.size(); ++index) {
        const auto& edge = directed[index];
        const auto& incident = outgoing[edge.to_node];
        const auto reverse = std::find(incident.begin(), incident.end(), edge.reverse_index);
        if (reverse == incident.end()) {
            throw std::invalid_argument("Detected segment graph adjacency is inconsistent");
        }
        const auto position = static_cast<std::size_t>(reverse - incident.begin());
        next[index] = incident[(position + incident.size() - 1) % incident.size()];
    }

    std::vector<bool> visited(directed.size(), false);
    std::vector<DetectedFace> faces;
    for (std::size_t start = 0; start < directed.size(); ++start) {
        if (visited[start]) continue;
        Boundary boundary;
        std::size_t first_source_edge = std::numeric_limits<std::size_t>::max();
        auto current = start;
        while (!visited[current]) {
            visited[current] = true;
            const auto& edge = directed[current];
            first_source_edge = std::min(first_source_edge, edge.source_index);
            boundary.push_back(edge.reversed ? reverse_segment(indexed[edge.source_index].segment)
                                             : indexed[edge.source_index].segment);
            current = next[current];
            if (boundary.size() > directed.size()) {
                throw std::invalid_argument("Detected segment graph face walk exceeded its bounds");
            }
        }
        if (current != start || boundary.size() < 3) continue;
        const auto diagnostics = validate_boundary(boundary);
        if (!diagnostics.empty()) continue;
        const auto area = signed_area(boundary);
        if (!std::isfinite(area) || area <= default_geometry_tolerance_metres) continue;
        rotate_boundary_to_stable_start(boundary);
        faces.push_back({std::move(boundary), first_source_edge, area});
    }
    std::sort(faces.begin(), faces.end(), [](const auto& left, const auto& right) {
        if (left.first_source_edge != right.first_source_edge)
            return left.first_source_edge < right.first_source_edge;
        if (left.area != right.area) return left.area < right.area;
        return left.boundary.size() < right.boundary.size();
    });
    std::vector<Boundary> result;
    result.reserve(faces.size());
    for (auto& face : faces) result.push_back(std::move(face.boundary));
    return result;
}

} // namespace sketch
