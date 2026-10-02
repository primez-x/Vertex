#include "sketch/wall_measurement.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr double geometry_envelope_metres = 1'000'000.0;
constexpr std::size_t maximum_source_walls = 2'048;
constexpr double parallel_direction_tolerance = 1e-12;

constexpr std::array<std::string_view, 5> context_fields{
    "property_id", "building_id", "floor_id", "layer_id", "phase_id"};

[[noreturn]] void reject(std::string message) {
    throw std::invalid_argument(std::move(message));
}

bool bounded(double value) {
    return std::isfinite(value) && std::abs(value) <= geometry_envelope_metres;
}

double number(const Json& value, std::string_view label) {
    if (!value.is_number()) reject(std::string(label) + " must be a finite number");
    double result{};
    try {
        result = value.get<double>();
    } catch (const Json::exception&) {
        reject(std::string(label) + " must be a finite number");
    }
    if (!std::isfinite(result)) reject(std::string(label) + " must be a finite number");
    return result;
}

Vec2 point(const Json& value, std::string_view label) {
    if (!value.is_array() || value.size() != 2)
        reject(std::string(label) + " must contain two coordinates");
    const Vec2 result{number(value[0], label), number(value[1], label)};
    if (!bounded(result.x) || !bounded(result.y))
        reject("Wall coordinates exceed the supported +/-1e6 metre envelope");
    return result;
}

Segment baseline(const Json& properties) {
    const auto found = properties.find("baseline");
    if (found == properties.end() || !found->is_object() ||
        !found->contains("start") || !found->contains("end"))
        reject("Source wall baseline is missing or malformed");
    double sweep = 0.0;
    if (const auto sweep_value = found->find("sweep_radians"); sweep_value != found->end())
        sweep = number(*sweep_value, "Wall baseline sweep");
    const Segment result{point(found->at("start"), "Wall baseline point"),
                         point(found->at("end"), "Wall baseline point"), sweep};
    double length{};
    try {
        length = segment_length(result);
    } catch (const std::invalid_argument&) {
        reject("Source wall baseline is invalid");
    }
    if (!(length > default_geometry_tolerance_metres) || !std::isfinite(length))
        reject("Source wall baseline is degenerate");
    const auto bounds = segment_bounds(result);
    if (!bounded(bounds.minimum.x) || !bounded(bounds.minimum.y) ||
        !bounded(bounds.maximum.x) || !bounded(bounds.maximum.y))
        reject("Source wall baseline exceeds the supported +/-1e6 metre envelope");
    return result;
}

double thickness(const Json& properties) {
    auto found = properties.find("thickness_m");
    if (found == properties.end()) found = properties.find("thickness");
    if (found == properties.end()) reject("Source wall thickness is missing");
    const auto result = number(*found, "Wall thickness");
    if (!(result > default_geometry_tolerance_metres) || !bounded(result))
        reject("Wall thickness is outside the supported positive geometry envelope");
    return result;
}

Json wall_context(const Json& properties) {
    Json context = Json::object();
    for (const auto field : context_fields) {
        const auto found = properties.find(std::string(field));
        if (found == properties.end()) continue;
        if (!found->is_string() || found->get_ref<const std::string&>().empty())
            reject(std::string("Source wall ") + std::string(field) + " must be a non-empty string");
        context[std::string(field)] = *found;
    }
    return context;
}

struct SourceWall {
    std::string id;
    Segment baseline;
    double thickness{};
    Json context;
};

std::pair<double, double> point_key(Vec2 point) {
    return {point.x, point.y};
}

std::vector<SourceWall> read_source_walls(const DocumentSnapshot& document,
                                          const std::vector<std::string>& wall_ids) {
    if (wall_ids.size() < 3)
        reject("At least three source walls are required for an exterior loop");
    if (wall_ids.size() > maximum_source_walls)
        reject("Exterior measurement exceeds the supported source wall count");

    std::set<std::string, std::less<>> unique_ids;
    std::vector<SourceWall> result;
    result.reserve(wall_ids.size());
    std::optional<Json> shared_floor;
    std::optional<Json> shared_layer;
    bool first_wall = true;
    const auto validate_shared_context = [&](const Json& context, std::string_view field,
                                             std::optional<Json>& shared) {
        const auto value = context.find(std::string(field));
        const std::optional<Json> current = value == context.end()
            ? std::nullopt : std::optional<Json>(*value);
        if (first_wall) shared = current;
        else if (shared != current)
            reject("All source walls must belong to the same floor and layer");
    };
    for (const auto& id : wall_ids) {
        if (id.empty() || !unique_ids.insert(id).second)
            reject("Source wall IDs must be unique and non-empty");
        const auto found = document.entities().find(id);
        if (found == document.entities().end() || found->second.type != "wall")
            reject("Exterior measurement source wall is missing or is not a wall: " + id);
        const auto& properties = found->second.properties;
        if (!properties.is_object()) reject("Source wall properties must be an object");
        SourceWall wall{id, baseline(properties), thickness(properties), wall_context(properties)};
        validate_shared_context(wall.context, "floor_id", shared_floor);
        validate_shared_context(wall.context, "layer_id", shared_layer);
        first_wall = false;
        result.push_back(std::move(wall));
    }
    return result;
}

Json source_for_walls(const std::vector<SourceWall>& walls) {
    std::vector<const SourceWall*> ordered;
    ordered.reserve(walls.size());
    for (const auto& wall : walls) ordered.push_back(&wall);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    Json records = Json::array();
    for (const auto* wall : ordered)
        records.push_back({{"id", wall->id}, {"context", wall->context}});
    return {{"version", 1}, {"basis", "exterior"}, {"walls", std::move(records)}};
}

double cross(Vec2 left, Vec2 right) {
    return left.x * right.y - left.y * right.x;
}

double dot(Vec2 left, Vec2 right) {
    return left.x * right.x + left.y * right.y;
}

Vec2 add(Vec2 left, Vec2 right) {
    return {left.x + right.x, left.y + right.y};
}

Vec2 subtract(Vec2 left, Vec2 right) {
    return {left.x - right.x, left.y - right.y};
}

Vec2 multiply(Vec2 point, double scale) {
    return {point.x * scale, point.y * scale};
}

double distance(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

struct ArcSupport {
    Vec2 center;
    double radius{};
};

ArcSupport arc_support(const Segment& segment) {
    if (segment.sweep_radians == 0.0) reject("A line has no circular support");
    const auto chord = subtract(segment.end, segment.start);
    const auto half_tangent = std::tan(segment.sweep_radians * 0.5);
    if (half_tangent == 0.0 || !std::isfinite(half_tangent))
        reject("Wall arc support is numerically indeterminate");
    const auto offset = 0.5 / half_tangent;
    const Vec2 center{segment.start.x + chord.x * 0.5 - chord.y * offset,
                      segment.start.y + chord.y * 0.5 + chord.x * offset};
    const auto radius = distance(center, segment.start);
    if (!bounded(center.x) || !bounded(center.y) || !std::isfinite(radius) ||
        !(radius > default_geometry_tolerance_metres))
        reject("Wall arc support exceeds the supported geometry envelope");
    return {center, radius};
}

std::optional<double> arc_fraction_if_on(const Segment& arc, Vec2 point) {
    const auto support = arc_support(arc);
    const auto start_angle = std::atan2(arc.start.y - support.center.y,
                                        arc.start.x - support.center.x);
    const auto point_angle = std::atan2(point.y - support.center.y,
                                        point.x - support.center.x);
    const auto turn = 2.0 * std::numbers::pi;
    auto delta = std::remainder(point_angle - start_angle, turn);
    if (arc.sweep_radians > 0.0) {
        if (delta < 0.0) delta += turn;
    } else if (delta > 0.0) {
        delta -= turn;
    }
    const auto fraction = delta / arc.sweep_radians;
    if (!std::isfinite(fraction) || fraction < -1e-8 || fraction > 1.0 + 1e-8 ||
        std::abs(distance(point, support.center) - support.radius) >
            default_geometry_tolerance_metres * 4.0)
        return std::nullopt;
    return std::clamp(fraction, 0.0, 1.0);
}

double directed_arc_fraction(const Segment& arc, Vec2 point) {
    const auto fraction = arc_fraction_if_on(arc, point);
    if (!fraction) reject("Wall intersection could not be ordered along its circular arc");
    return *fraction;
}

Vec2 point_at_fraction(const Segment& segment, double fraction) {
    if (segment.sweep_radians == 0.0)
        return {std::lerp(segment.start.x, segment.end.x, fraction),
                std::lerp(segment.start.y, segment.end.y, fraction)};
    const auto support = arc_support(segment);
    const auto angle = std::atan2(segment.start.y - support.center.y,
                                  segment.start.x - support.center.x) +
                       segment.sweep_radians * fraction;
    return {support.center.x + support.radius * std::cos(angle),
            support.center.y + support.radius * std::sin(angle)};
}

Segment subsegment(const Segment& source, double from, double to, Vec2 start, Vec2 end) {
    return {start, end, source.sweep_radians * (to - from)};
}

// The input count alone does not bound a planar arrangement: pairwise crossings
// can create quadratically many fragments. Keep both storage and face walks
// bounded, and fail closed rather than simplify authoritative geometry.
constexpr std::size_t maximum_network_nodes = 32'768;
constexpr std::size_t maximum_network_edges = 32'768;
constexpr auto no_index = std::numeric_limits<std::size_t>::max();

struct NetworkEdge {
    std::size_t start{};
    std::size_t end{};
    std::size_t wall{};
    Segment segment;
};

struct WallNetwork {
    std::vector<Vec2> nodes;
    std::vector<NetworkEdge> edges;
    std::vector<std::vector<std::size_t>> outgoing;
    std::vector<std::size_t> component;
    std::vector<bool> bridges;

    std::size_t from(std::size_t half_edge) const {
        const auto& edge = edges[half_edge / 2];
        return half_edge % 2 == 0 ? edge.start : edge.end;
    }
    std::size_t to(std::size_t half_edge) const { return from(half_edge ^ 1); }
};

WallNetwork split_wall_network(const std::vector<SourceWall>& walls) {
    WallNetwork graph;
    using Cell = std::pair<long long, long long>;
    std::map<Cell, std::vector<std::size_t>> cells;
    double coordinate_scale = 1.0;
    for (const auto& wall : walls)
        coordinate_scale = std::max({coordinate_scale, std::abs(wall.baseline.start.x),
            std::abs(wall.baseline.start.y), std::abs(wall.baseline.end.x), std::abs(wall.baseline.end.y)});
    const auto cell_for = [](Vec2 point) -> Cell {
        return {static_cast<long long>(std::floor(point.x / default_geometry_tolerance_metres)),
                static_cast<long long>(std::floor(point.y / default_geometry_tolerance_metres))};
    };
    const auto node_for = [&](Vec2 point) {
        if (!bounded(point.x) || !bounded(point.y)) reject("Wall intersection exceeds the geometry envelope");
        const auto cell = cell_for(point);
        std::optional<std::size_t> existing;
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto found = cells.find({cell.first + dx, cell.second + dy});
                if (found == cells.end()) continue;
                for (const auto node : found->second) {
                    const auto separation = distance(graph.nodes[node], point);
                    if (separation > default_geometry_tolerance_metres) continue;
                    // Only absorb arithmetic roundoff. Model-tolerance contacts
                    // that would move distinct geometry are unsupported precision.
                    const auto roundoff = std::min(default_geometry_tolerance_metres * 0.01,
                        64.0 * std::numeric_limits<double>::epsilon() * coordinate_scale);
                    if (separation > roundoff || (existing && *existing != node))
                        reject("Wall network contacts are below supported geometric precision");
                    existing = node;
                }
            }
        }
        if (existing) return *existing;
        if (graph.nodes.size() >= maximum_network_nodes)
            reject("Wall network exceeds the supported intersection node count");
        const auto node = graph.nodes.size();
        graph.nodes.push_back(point);
        cells[cell].push_back(node);
        return node;
    };

    struct Cut { double fraction{}; std::size_t node{}; };
    std::vector<std::vector<Cut>> cuts(walls.size());
    for (std::size_t i = 0; i < walls.size(); ++i) {
        cuts[i].push_back({0.0, node_for(walls[i].baseline.start)});
        cuts[i].push_back({1.0, node_for(walls[i].baseline.end)});
    }
    std::size_t cut_count = walls.size() * 2;
    for (std::size_t i = 0; i < walls.size(); ++i) {
        for (std::size_t j = i + 1; j < walls.size(); ++j) {
            const auto hit = segment_intersection(walls[i].baseline, walls[j].baseline);
            if (hit.kind == SegmentIntersectionKind::overlap)
                reject("Duplicate or overlapping collinear source walls are not supported");
            if (hit.kind == SegmentIntersectionKind::indeterminate)
                reject("Wall network intersection is numerically indeterminate");
            for (const auto point : hit.points) {
                const auto node = node_for(point);
                for (const auto wall : {i, j}) {
                    const auto fraction = walls[wall].baseline.sweep_radians == 0.0
                        ? dot(subtract(point, walls[wall].baseline.start),
                              subtract(walls[wall].baseline.end, walls[wall].baseline.start)) /
                              std::pow(distance(walls[wall].baseline.start,
                                               walls[wall].baseline.end), 2)
                        : directed_arc_fraction(walls[wall].baseline, point);
                    if (std::any_of(cuts[wall].begin(), cuts[wall].end(), [&](const Cut& cut) {
                            return cut.node == node;
                        })) continue;
                    if (++cut_count > maximum_network_edges + walls.size())
                        reject("Wall network exceeds the supported intersection fragment count");
                    cuts[wall].push_back({fraction, node});
                }
            }
        }
    }
    for (std::size_t i = 0; i < walls.size(); ++i) {
        std::sort(cuts[i].begin(), cuts[i].end(), [](const Cut& left, const Cut& right) {
            return left.fraction < right.fraction;
        });
        for (std::size_t j = 1; j < cuts[i].size(); ++j) {
            const auto from = cuts[i][j - 1];
            const auto to = cuts[i][j];
            if (distance(graph.nodes[from.node], graph.nodes[to.node]) <=
                default_geometry_tolerance_metres)
                reject("Wall network fragments are below supported geometric precision");
            if (graph.edges.size() >= maximum_network_edges)
                reject("Wall network exceeds the supported intersection fragment count");
            const auto fragment = subsegment(walls[i].baseline, from.fraction, to.fraction,
                                              graph.nodes[from.node], graph.nodes[to.node]);
            if (!(segment_length(fragment) > default_geometry_tolerance_metres))
                reject("Wall network fragments are below supported geometric precision");
            graph.edges.push_back({from.node, to.node, i, fragment});
        }
    }
    graph.outgoing.resize(graph.nodes.size());
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        graph.outgoing[graph.edges[i].start].push_back(2 * i);
        graph.outgoing[graph.edges[i].end].push_back(2 * i + 1);
    }
    return graph;
}

void identify_network_bridges(WallNetwork& graph) {
    // Iterative Tarjan traversal avoids a process-stack limit for long walls
    // split by many intersections. A bridge has no enclosing face on either side.
    std::vector<std::size_t> discovery(graph.nodes.size(), 0);
    std::vector<std::size_t> low(graph.nodes.size(), 0);
    std::vector<std::size_t> parent(graph.nodes.size(), no_index);
    std::vector<std::size_t> cursor(graph.nodes.size(), 0);
    graph.component.resize(graph.nodes.size());
    graph.bridges.assign(graph.edges.size(), false);
    std::size_t time = 0;
    std::size_t component = 0;
    std::vector<std::size_t> stack;
    for (std::size_t seed = 0; seed < graph.nodes.size(); ++seed) {
        if (discovery[seed] != 0) continue;
        discovery[seed] = low[seed] = ++time;
        graph.component[seed] = component;
        stack.push_back(seed);
        while (!stack.empty()) {
            const auto node = stack.back();
            if (cursor[node] < graph.outgoing[node].size()) {
                const auto half = graph.outgoing[node][cursor[node]++];
                if (parent[node] != no_index && half == (parent[node] ^ 1)) continue;
                const auto next = graph.to(half);
                if (discovery[next] == 0) {
                    parent[next] = half;
                    graph.component[next] = component;
                    discovery[next] = low[next] = ++time;
                    stack.push_back(next);
                } else {
                    low[node] = std::min(low[node], discovery[next]);
                }
            } else {
                stack.pop_back();
                if (parent[node] != no_index) {
                    const auto previous = graph.from(parent[node]);
                    if (low[node] > discovery[previous]) graph.bridges[parent[node] / 2] = true;
                    low[previous] = std::min(low[previous], low[node]);
                }
            }
        }
        ++component;
    }
}

enum class LoopLocation { outside, boundary, inside };

LoopLocation locate_in_analytical_loop(Vec2 point, const Boundary& loop) {
    const auto ray_y = point.y + default_geometry_tolerance_metres * 0.25;
    bool inside = false;
    for (const auto& edge : loop) {
        const auto direction = subtract(edge.end, edge.start);
        const auto offset = subtract(point, edge.start);
        if (edge.sweep_radians == 0.0) {
            const auto parameter = std::clamp(dot(offset, direction) / dot(direction, direction), 0.0, 1.0);
            if (distance(point, add(edge.start, multiply(direction, parameter))) <=
                default_geometry_tolerance_metres)
                return LoopLocation::boundary;
            if (direction.y != 0.0) {
                const auto parameter_y = (ray_y - edge.start.y) / direction.y;
                if (parameter_y > 0.0 && parameter_y < 1.0) {
                    const auto x = edge.start.x + parameter_y * direction.x;
                    if (x > point.x) inside = !inside;
                }
            }
        } else {
            const auto support = arc_support(edge);
            const auto radial_distance = distance(point, support.center);
            if (std::abs(radial_distance - support.radius) <= default_geometry_tolerance_metres &&
                arc_fraction_if_on(edge, point))
                return LoopLocation::boundary;
            if (std::abs(ray_y - support.center.y) > support.radius) continue;
            const auto vertical = ray_y - support.center.y;
            const auto horizontal = std::sqrt(std::max(0.0, support.radius * support.radius -
                                                           vertical * vertical));
            for (const auto x : {support.center.x - horizontal, support.center.x + horizontal}) {
                if (x <= point.x) continue;
                const Vec2 intersection{x, ray_y};
                const auto fraction = arc_fraction_if_on(edge, intersection);
                if (!fraction || *fraction <= 1e-12 || *fraction >= 1.0 - 1e-12) continue;
                const auto angle = std::atan2(vertical, x - support.center.x);
                const auto dy = edge.sweep_radians * support.radius * std::cos(angle);
                if (std::abs(dy) > default_geometry_tolerance_metres * support.radius)
                    inside = !inside;
            }
        }
    }
    return inside ? LoopLocation::inside : LoopLocation::outside;
}

Segment directed_network_segment(const WallNetwork& graph, std::size_t half_edge) {
    const auto& edge = graph.edges[half_edge / 2];
    return half_edge % 2 == 0
        ? edge.segment
        : Segment{edge.segment.end, edge.segment.start, -edge.segment.sweep_radians};
}

double start_tangent(const Segment& segment) {
    const auto angle = std::atan2(segment.end.y - segment.start.y,
                                  segment.end.x - segment.start.x) -
                       segment.sweep_radians * 0.5;
    const auto turn = 2.0 * std::numbers::pi;
    auto normalized = std::fmod(angle, turn);
    if (normalized < 0.0) normalized += turn;
    return normalized;
}

std::vector<std::string> recognize_network_exterior(const std::vector<SourceWall>& walls) {
    auto graph = split_wall_network(walls);
    identify_network_bridges(graph);
    std::vector<std::vector<std::size_t>> angular(graph.nodes.size());
    std::vector<std::size_t> position(graph.edges.size() * 2, no_index);
    for (std::size_t node = 0; node < graph.nodes.size(); ++node) {
        for (const auto half : graph.outgoing[node])
            if (!graph.bridges[half / 2]) angular[node].push_back(half);
        const auto angle = [&](std::size_t half) {
            return start_tangent(directed_network_segment(graph, half));
        };
        std::sort(angular[node].begin(), angular[node].end(), [&](const auto left, const auto right) {
            return angle(left) < angle(right);
        });
        for (std::size_t i = 1; i < angular[node].size(); ++i) {
            if (std::abs(angle(angular[node][i]) - angle(angular[node][i - 1])) <= 1e-12 &&
                graph.edges[angular[node][i] / 2].wall !=
                    graph.edges[angular[node][i - 1] / 2].wall)
                reject("Wall network has ambiguous coincident curved tangents at a contact");
        }
        if (angular[node].size() > 1 &&
            angle(angular[node].front()) + 2.0 * std::numbers::pi -
                angle(angular[node].back()) <= 1e-12 &&
            graph.edges[angular[node].front() / 2].wall !=
                graph.edges[angular[node].back() / 2].wall)
            reject("Wall network has ambiguous coincident curved tangents at a contact");
        for (std::size_t i = 0; i < angular[node].size(); ++i) position[angular[node][i]] = i;
    }

    std::vector<bool> visited(graph.edges.size() * 2, false);
    std::optional<std::vector<std::size_t>> exterior;
    std::size_t containment_work = 0;
    for (std::size_t seed = 0; seed < visited.size(); ++seed) {
        if (visited[seed] || graph.bridges[seed / 2]) continue;
        std::vector<std::size_t> face;
        Boundary loop;
        auto half = seed;
        do {
            if (visited[half]) reject("Wall network face walk is ambiguous; close the perimeter or select one shell");
            visited[half] = true;
            face.push_back(half);
            loop.push_back(directed_network_segment(graph, half));
            const auto node = graph.to(half);
            const auto& incident = angular[node];
            const auto reverse_position = position[half ^ 1];
            if (incident.empty() || reverse_position == no_index)
                reject("Wall network face walk lost its reverse edge");
            half = incident[(reverse_position + incident.size() - 1) % incident.size()];
        } while (half != seed);
        // Keeping the face on the left gives positive bounded faces and a
        // negative unbounded face. We consider only these actual outer walks,
        // never the largest room cycle or a bounding box.
        if (signed_area(loop) >= -default_geometry_tolerance_metres) continue;
        std::set<std::size_t> face_nodes;
        bool simple = true;
        for (const auto edge : face)
            if (!face_nodes.insert(graph.from(edge)).second) simple = false;
        if (!simple) continue;
        // Contacts may subdivide a host wall many times. Coalesce only its
        // recognition segments to keep validation/containment proportional to
        // source geometry; full fragment identity remains in the face record.
        loop.clear();
        std::size_t previous_wall = no_index;
        for (const auto edge : face) {
            const auto wall = graph.edges[edge / 2].wall;
            const auto segment = directed_network_segment(graph, edge);
            if (wall == previous_wall) {
                loop.back().end = segment.end;
                loop.back().sweep_radians += segment.sweep_radians;
            }
            else loop.push_back(segment);
            previous_wall = wall;
        }
        if (loop.size() > 1 && graph.edges[face.front() / 2].wall == previous_wall) {
            loop.front().start = loop.back().start;
            loop.front().sweep_radians += loop.back().sweep_radians;
            loop.pop_back();
        }
        if (!validate_boundary(loop).empty()) continue;
        const auto component = graph.component[graph.from(seed)];
        bool contains_network = true;
        for (std::size_t node = 0; node < graph.nodes.size(); ++node) {
            const bool connected = graph.component[node] == component;
            // Connected open spurs do not enclose area, even outside the loop.
            if (connected && angular[node].empty()) continue;
            if (containment_work > 8'000'000 - loop.size())
                reject("Wall network exceeds the supported containment work limit");
            containment_work += loop.size();
            const auto location = locate_in_analytical_loop(graph.nodes[node], loop);
            if (location == LoopLocation::outside ||
                (!connected && location != LoopLocation::inside)) {
                contains_network = false;
                break;
            }
        }
        if (!contains_network) continue;
        if (exterior) reject("Wall network has multiple incomparable exterior outlines; select one shell");
        exterior = std::move(face);
    }
    if (!exterior)
        reject("Wall network has no unique simple containing exterior outline; close the perimeter or select one shell");

    std::vector<std::size_t> total(walls.size(), 0);
    std::vector<std::size_t> perimeter(walls.size(), 0);
    for (const auto& edge : graph.edges) ++total[edge.wall];
    for (const auto half : *exterior) ++perimeter[graph.edges[half / 2].wall];
    std::vector<std::string> result;
    for (std::size_t wall = 0; wall < walls.size(); ++wall) {
        if (perimeter[wall] == 0) continue;
        if (perimeter[wall] != total[wall])
            reject("Exterior outline uses only part of source wall: " + walls[wall].id +
                   "; split or trim the wall at the perimeter junction");
        result.push_back(walls[wall].id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

struct OffsetSupport {
    bool arc{};
    Segment base;
    ArcSupport circle{};
    Vec2 direction{};
    Vec2 source_chord{};
    double half_thickness{};
};

Vec2 unit_vector(Vec2 value) {
    const auto length = std::hypot(value.x, value.y);
    if (!(length > default_geometry_tolerance_metres) || !std::isfinite(length))
        reject("Offset source wall tangent is degenerate");
    return {value.x / length, value.y / length};
}

double wrapped_angle_delta(double from, double to) {
    return std::remainder(to - from, 2.0 * std::numbers::pi);
}

Vec2 offset_join(const OffsetSupport& incoming, const OffsetSupport& outgoing, Vec2 vertex) {
    std::vector<Vec2> candidates;
    if (!incoming.arc && !outgoing.arc) {
        const auto denominator = cross(incoming.direction, outgoing.direction);
        const auto between = subtract(outgoing.base.start, incoming.base.end);
        if (std::abs(denominator) <= parallel_direction_tolerance) {
            const bool exact_continuation =
                cross(incoming.source_chord, outgoing.source_chord) == 0.0 &&
                dot(incoming.source_chord, outgoing.source_chord) > 0.0;
            if (exact_continuation && incoming.half_thickness == outgoing.half_thickness &&
                distance(incoming.base.end, outgoing.base.start) <= default_geometry_tolerance_metres)
                return multiply(add(incoming.base.end, outgoing.base.start), 0.5);
            reject("A near-parallel wall corner has no bounded unambiguous offset join");
        }
        const auto parameter = cross(between, outgoing.direction) / denominator;
        candidates.push_back(add(incoming.base.end, multiply(incoming.direction, parameter)));
    } else if (incoming.arc && outgoing.arc) {
        const auto centers = subtract(outgoing.circle.center, incoming.circle.center);
        const auto center_distance = std::hypot(centers.x, centers.y);
        const auto r1 = incoming.circle.radius;
        const auto r2 = outgoing.circle.radius;
        if (center_distance <= default_geometry_tolerance_metres) {
            if (std::abs(r1 - r2) > default_geometry_tolerance_metres)
                reject("Concentric wall offsets do not have one common corner");
            const auto radial = subtract(vertex, incoming.circle.center);
            const auto length = std::hypot(radial.x, radial.y);
            if (!(length > default_geometry_tolerance_metres))
                reject("Concentric wall offsets have an indeterminate common corner");
            return add(incoming.circle.center, multiply(radial, r1 / length));
        }
        const auto along = (r1 * r1 - r2 * r2 + center_distance * center_distance) /
                           (2.0 * center_distance);
        auto height_squared = r1 * r1 - along * along;
        const auto height_tolerance = default_geometry_tolerance_metres *
                                      (2.0 * r1 + default_geometry_tolerance_metres);
        if (height_squared < -height_tolerance)
            reject("Circular wall offsets do not meet at a bounded common corner");
        height_squared = std::max(0.0, height_squared);
        const auto base = add(incoming.circle.center, multiply(centers, along / center_distance));
        const Vec2 perpendicular{-centers.y / center_distance, centers.x / center_distance};
        const auto height = std::sqrt(height_squared);
        candidates.push_back(add(base, multiply(perpendicular, height)));
        if (height > default_geometry_tolerance_metres)
            candidates.push_back(add(base, multiply(perpendicular, -height)));
    } else {
        const auto& line = incoming.arc ? outgoing : incoming;
        const auto& circle = incoming.arc ? incoming.circle : outgoing.circle;
        const auto line_start = incoming.arc ? outgoing.base.start : incoming.base.end;
        const auto direction = line.direction;
        const auto offset = subtract(line_start, circle.center);
        const auto projection = dot(offset, direction);
        auto discriminant = circle.radius * circle.radius -
            (dot(offset, offset) - projection * projection);
        const auto allowed = default_geometry_tolerance_metres *
                             (2.0 * circle.radius + default_geometry_tolerance_metres);
        if (discriminant < -allowed)
            reject("Line and circular wall offsets do not meet at a common corner");
        discriminant = std::max(0.0, discriminant);
        const auto root = std::sqrt(discriminant);
        candidates.push_back(add(line_start, multiply(direction, -projection + root)));
        if (root > default_geometry_tolerance_metres)
            candidates.push_back(add(line_start, multiply(direction, -projection - root)));
    }
    if (candidates.empty()) reject("Wall offsets do not have a common corner");
    const auto selected = *std::min_element(candidates.begin(), candidates.end(), [&](Vec2 left, Vec2 right) {
        return distance(left, vertex) < distance(right, vertex);
    });
    if (!bounded(selected.x) || !bounded(selected.y))
        reject("Wall miter exceeds the supported +/-1e6 metre envelope");
    return selected;
}

Boundary offset_loop(const Boundary& loop, const std::vector<SourceWall>& walls) {
    if (loop.size() != walls.size()) reject("Source wall loop assembly was incomplete");
    const auto area = signed_area(loop);
    if (!std::isfinite(area) || std::abs(area) <= default_geometry_tolerance_metres)
        reject("Source wall loop has no stable orientation");
    const double outward_multiplier = area > 0.0 ? -1.0 : 1.0;

    std::vector<OffsetSupport> edges;
    edges.reserve(loop.size());
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const auto& segment = loop[index];
        const auto chord_direction = unit_vector(subtract(segment.end, segment.start));
        const auto chord_heading = std::atan2(chord_direction.y, chord_direction.x);
        const auto start_heading = chord_heading - segment.sweep_radians * 0.5;
        const auto end_heading = chord_heading + segment.sweep_radians * 0.5;
        const Vec2 start_tangent = segment.sweep_radians == 0.0
            ? chord_direction : Vec2{std::cos(start_heading), std::sin(start_heading)};
        const Vec2 end_tangent = segment.sweep_radians == 0.0
            ? chord_direction : Vec2{std::cos(end_heading), std::sin(end_heading)};
        const Vec2 start_outward{-start_tangent.y * outward_multiplier,
                                  start_tangent.x * outward_multiplier};
        const Vec2 end_outward{-end_tangent.y * outward_multiplier,
                                end_tangent.x * outward_multiplier};
        const auto half = walls[index].thickness * 0.5;
        if (segment.sweep_radians == 0.0) {
            const auto shifted_start = add(segment.start, multiply(start_outward, half));
            const auto shifted_end = add(segment.end, multiply(end_outward, half));
            edges.push_back({false, {shifted_start, shifted_end, 0.0}, {}, start_tangent,
                             subtract(segment.end, segment.start), half});
        } else {
            const auto support = arc_support(segment);
            const auto radius_change = -outward_multiplier *
                                       std::copysign(1.0, segment.sweep_radians);
            const auto radius = support.radius + radius_change * half;
            if (!(radius > default_geometry_tolerance_metres) || !std::isfinite(radius))
                reject("Exterior wall offset collapses its circular radius");
            const auto scale = radius / support.radius;
            const auto shifted_start = add(support.center,
                multiply(subtract(segment.start, support.center), scale));
            const auto shifted_end = add(support.center,
                multiply(subtract(segment.end, support.center), scale));
            edges.push_back({true, {shifted_start, shifted_end, segment.sweep_radians},
                             {support.center, radius}, start_tangent,
                             subtract(segment.end, segment.start), half});
        }
    }

    std::vector<Vec2> corners(loop.size());
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const auto previous = (index + loop.size() - 1) % loop.size();
        corners[index] = offset_join(edges[previous], edges[index], loop[index].start);
    }

    Boundary result;
    result.reserve(loop.size());
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const auto next = (index + 1) % loop.size();
        Segment segment{corners[index], corners[next], 0.0};
        if (edges[index].arc) {
            const auto center = edges[index].circle.center;
            const auto start_reference = std::atan2(loop[index].start.y - center.y,
                                                    loop[index].start.x - center.x);
            const auto end_reference = std::atan2(loop[index].end.y - center.y,
                                                  loop[index].end.x - center.x);
            const auto start_angle = std::atan2(corners[index].y - center.y,
                                                corners[index].x - center.x);
            const auto end_angle = std::atan2(corners[next].y - center.y,
                                              corners[next].x - center.x);
            const auto start_delta = wrapped_angle_delta(start_reference, start_angle);
            const auto end_delta = wrapped_angle_delta(end_reference, end_angle);
            segment.sweep_radians = loop[index].sweep_radians + end_delta - start_delta;
            const auto join_start_radius = distance(corners[index], center);
            const auto join_end_radius = distance(corners[next], center);
            if (std::abs(join_start_radius - edges[index].circle.radius) >
                    default_geometry_tolerance_metres * 8.0 ||
                std::abs(join_end_radius - edges[index].circle.radius) >
                    default_geometry_tolerance_metres * 8.0)
                reject("Analytical wall join is not concentric with its offset arc");
        }
        if (!(segment_length(segment) > default_geometry_tolerance_metres))
            reject("Exterior wall offset produced a degenerate segment");
        result.push_back(segment);
    }
    if (const auto diagnostics = validate_boundary(result); !diagnostics.empty())
        reject("Exterior wall offset is not a simple closed boundary: " + diagnostics.front().message);
    return result;
}

Boundary actual_boundary_geometry(const Entity& boundary) {
    if (!boundary.properties.is_object()) reject("Boundary properties must be an object");
    const auto format = inspect_boundary_entity_version(boundary);
    if (format.format == BoundaryEntityFormat::identified_v1)
        return boundary_geometry(decode_identified_boundary_entity(boundary));
    if (format.format == BoundaryEntityFormat::unsupported_version)
        reject(format.diagnostic);
    const Json* values = nullptr;
    if (const auto boundary_value = boundary.properties.find("boundary");
        boundary_value != boundary.properties.end())
        values = &*boundary_value;
    else if (const auto segments_value = boundary.properties.find("segments");
             segments_value != boundary.properties.end())
        values = &*segments_value;
    if (values == nullptr || !values->is_array() || values->empty())
        reject("Boundary geometry is missing");
    Boundary result;
    result.reserve(values->size());
    for (const auto& value : *values) {
        if (!value.is_object() || !value.contains("start") || !value.contains("end"))
            reject("Boundary segment is incomplete");
        double sweep = 0.0;
        if (const auto field = value.find("sweep_radians"); field != value.end())
            sweep = number(*field, "Boundary sweep");
        result.push_back({point(value.at("start"), "Boundary point"),
                          point(value.at("end"), "Boundary point"), sweep});
    }
    return result;
}

using EdgeKey = std::tuple<double, double, double, double, double>;

EdgeKey edge_key(const Segment& segment) {
    auto start = point_key(segment.start);
    auto end = point_key(segment.end);
    auto sweep = segment.sweep_radians;
    if (end < start) {
        std::swap(start, end);
        sweep = -sweep;
    }
    return {start.first, start.second, end.first, end.second, sweep};
}

std::vector<EdgeKey> edge_keys(const Boundary& boundary) {
    std::vector<EdgeKey> result;
    result.reserve(boundary.size());
    for (const auto& segment : boundary) result.push_back(edge_key(segment));
    std::sort(result.begin(), result.end());
    return result;
}

void validate_source_schema(const Json& source, std::vector<std::string>& ids) {
    if (!source.is_object() || source.size() != 3 || !source.contains("version") ||
        !source.contains("basis") || !source.contains("walls"))
        reject("Wall measurement source is incomplete");
    const auto& version = source.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || version != 1 ||
        !source.at("basis").is_string() || source.at("basis") != "exterior" ||
        !source.at("walls").is_array() || source.at("walls").empty() ||
        source.at("walls").size() > maximum_source_walls)
        reject("Wall measurement source has an unknown or invalid schema");
    std::set<std::string, std::less<>> seen;
    for (const auto& record : source.at("walls")) {
        if (!record.is_object() || record.size() != 2 || !record.contains("id") ||
            !record.contains("context") || !record.at("id").is_string() ||
            !record.at("context").is_object())
            reject("Wall measurement source record is malformed");
        const auto id = record.at("id").get<std::string>();
        if (id.empty() || !seen.insert(id).second)
            reject("Wall measurement source IDs must be unique and non-empty");
        for (const auto& [key, value] : record.at("context").items()) {
            if (std::find(context_fields.begin(), context_fields.end(), key) == context_fields.end() ||
                !value.is_string() || value.get_ref<const std::string&>().empty())
                reject("Wall measurement source context is malformed");
        }
        ids.push_back(id);
    }
}

Json normalize_source_order(const Json& source) {
    std::vector<Json> records;
    records.reserve(source.at("walls").size());
    for (const auto& record : source.at("walls")) records.push_back(record);
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.at("id").get_ref<const std::string&>() <
               right.at("id").get_ref<const std::string&>();
    });
    Json walls = Json::array();
    for (auto& record : records) walls.push_back(std::move(record));
    return {{"version", 1}, {"basis", "exterior"}, {"walls", std::move(walls)}};
}

bool boundary_context_matches(const Entity& boundary, const Json& source) {
    if (!boundary.properties.is_object()) return false;
    for (const auto& record : source.at("walls")) {
        const auto& context = record.at("context");
        for (const auto field : {std::string_view{"floor_id"}, std::string_view{"layer_id"}}) {
            const auto source_value = context.find(std::string(field));
            if (source_value == context.end()) continue;
            const auto boundary_value = boundary.properties.find(std::string(field));
            if (boundary_value == boundary.properties.end() || !boundary_value->is_string() ||
                *boundary_value != *source_value)
                return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::string> exterior_wall_measurement_sources(
    const DocumentSnapshot& document, const std::vector<std::string>& candidate_wall_ids) {
    auto walls = read_source_walls(document, candidate_wall_ids);
    // Automatic discovery must never combine separate property, building or
    // phase contexts merely because their projected floor/layer geometry meets.
    for (const auto& wall : walls)
        if (wall.context != walls.front().context)
            reject("All candidate walls must share property, building, floor, layer and phase context");
    std::optional<std::pair<double, double>> elevation_range;
    for (const auto& wall : walls) {
        // Match displayed/project geometry rather than comparing persisted
        // local elevations. The resolver returns a copy and validates opt-in
        // level placement without mutating walls or their provenance records.
        const auto resolved = resolve_vertical_placement(document, document.entities().at(wall.id));
        const auto& properties = resolved.properties;
        auto field = properties.find("elevation_m");
        if (field == properties.end()) field = properties.find("elevation");
        const auto current = field == properties.end() ? 0.0 : number(*field, "Wall elevation");
        if (!bounded(current)) reject("Wall elevation exceeds the supported geometry envelope");
        if (!elevation_range) elevation_range = {current, current};
        else {
            elevation_range->first = std::min(elevation_range->first, current);
            elevation_range->second = std::max(elevation_range->second, current);
            if (elevation_range->second - elevation_range->first > default_geometry_tolerance_metres)
                reject("All candidate walls must share the same elevation plane");
        }
    }
    for (auto& wall : walls)
        if (point_key(wall.baseline.end) < point_key(wall.baseline.start)) {
            std::swap(wall.baseline.start, wall.baseline.end);
            wall.baseline.sweep_radians = -wall.baseline.sweep_radians;
        }
    std::sort(walls.begin(), walls.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    auto ids = recognize_network_exterior(walls);
    // Full source baselines must still satisfy the strict v1 derivation, including
    // bounded offset corners and thickness. The graph is recognition only.
    (void)derive_exterior_wall_measurement(document, ids);
    return ids;
}

WallMeasurementResult derive_exterior_wall_measurement(
    const DocumentSnapshot& document, const std::vector<std::string>& wall_ids) {
    auto walls = read_source_walls(document, wall_ids);
    auto geometry_walls = walls;
    for (auto& wall : geometry_walls) {
        if (point_key(wall.baseline.end) < point_key(wall.baseline.start)) {
            std::swap(wall.baseline.start, wall.baseline.end);
            wall.baseline.sweep_radians = -wall.baseline.sweep_radians;
        }
    }
    std::sort(geometry_walls.begin(), geometry_walls.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    std::vector<Segment> baselines;
    baselines.reserve(geometry_walls.size());
    std::map<EdgeKey, const SourceWall*> wall_by_baseline;
    for (const auto& wall : geometry_walls) {
        baselines.push_back(wall.baseline);
        wall_by_baseline.emplace(edge_key(wall.baseline), &wall);
    }
    const auto loop = assemble_boundary_from_segments(baselines);
    std::vector<SourceWall> loop_walls;
    loop_walls.reserve(loop.size());
    for (const auto& segment : loop) {
        const auto found = wall_by_baseline.find(edge_key(segment));
        if (found == wall_by_baseline.end()) reject("Assembled wall loop lost a source wall");
        loop_walls.push_back(*found->second);
    }
    auto boundary = offset_loop(loop, loop_walls);
    if (signed_area(boundary) < 0.0) {
        std::reverse(boundary.begin(), boundary.end());
        for (auto& segment : boundary) {
            std::swap(segment.start, segment.end);
            segment.sweep_radians = -segment.sweep_radians;
        }
        std::reverse(loop_walls.begin(), loop_walls.end());
    }
    const auto seed = geometry_walls.front().id;
    const auto seed_edge = std::find_if(loop_walls.begin(), loop_walls.end(), [&](const auto& wall) {
        return wall.id == seed;
    });
    if (seed_edge == loop_walls.end()) reject("Exterior boundary lost its stable source wall seed");
    const auto rotation = static_cast<std::size_t>(seed_edge - loop_walls.begin());
    std::rotate(boundary.begin(), boundary.begin() + static_cast<std::ptrdiff_t>(rotation),
                boundary.end());
    std::rotate(loop_walls.begin(), loop_walls.begin() + static_cast<std::ptrdiff_t>(rotation),
                loop_walls.end());
    return {std::move(boundary), source_for_walls(walls)};
}

bool wall_measurement_source_current(const DocumentSnapshot& document, const Entity& boundary) {
    try {
        if (!boundary.properties.is_object()) return false;
        const auto source_property = boundary.properties.find("wall_measurement_source");
        if (source_property == boundary.properties.end()) return true;
        if (boundary.type != "boundary" && boundary.type != "measurement_boundary") return false;
        std::vector<std::string> wall_ids;
        validate_source_schema(*source_property, wall_ids);
        const auto expected = derive_exterior_wall_measurement(document, wall_ids);
        if (normalize_source_order(*source_property) != expected.source ||
            !boundary_context_matches(boundary, expected.source))
            return false;
        const auto actual = actual_boundary_geometry(boundary);
        if (!validate_boundary(actual).empty()) return false;
        return edge_keys(actual) == edge_keys(expected.boundary);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace sketch
