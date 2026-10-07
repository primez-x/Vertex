#include "sketch/wall_measurement.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/wall_merge.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <limits>
#include <numbers>
#include <numeric>
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

std::vector<SourceWall> read_source_walls(const std::map<std::string, Entity, std::less<>>& entities,
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
        const auto found = entities.find(id);
        if (found == entities.end() || found->second.type != "wall")
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
                    // Known endpoint cuts already have an authoritative station.
                    // Recomputing an arc station from a rounded intersection can
                    // wrap a tiny negative start-angle delta into a full turn.
                    if (std::any_of(cuts[wall].begin(), cuts[wall].end(), [&](const Cut& cut) {
                            return cut.node == node;
                        })) continue;
                    const auto fraction = walls[wall].baseline.sweep_radians == 0.0
                        ? dot(subtract(point, walls[wall].baseline.start),
                              subtract(walls[wall].baseline.end, walls[wall].baseline.start)) /
                              std::pow(distance(walls[wall].baseline.start,
                                               walls[wall].baseline.end), 2)
                        : directed_arc_fraction(walls[wall].baseline, point);
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

enum class OffsetKernel { stable, legacy_v1 };

Vec2 unit_vector(Vec2 value) {
    const auto length = std::hypot(value.x, value.y);
    if (!(length > default_geometry_tolerance_metres) || !std::isfinite(length))
        reject("Offset source wall tangent is degenerate");
    return {value.x / length, value.y / length};
}

double wrapped_angle_delta(double from, double to) {
    return std::remainder(to - from, 2.0 * std::numbers::pi);
}

Vec2 offset_join(const OffsetSupport& incoming, const OffsetSupport& outgoing, Vec2 vertex,
                 OffsetKernel kernel) {
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
        double discriminant{};
        if (kernel == OffsetKernel::legacy_v1) {
            // Preserve the original operation order and tolerance exactly:
            // its positive-ulp tangent drift is part of retained v1 bytes.
            discriminant = circle.radius * circle.radius -
                (dot(offset, offset) - projection * projection);
            const auto allowed = default_geometry_tolerance_metres *
                                 (2.0 * circle.radius + default_geometry_tolerance_metres);
            if (discriminant < -allowed)
                reject("Line and circular wall offsets do not meet at a common corner");
            discriminant = std::max(0.0, discriminant);
        } else {
            // The perpendicular distance avoids subtracting two squared lengths.
            // At a tangent that cancellation otherwise leaves a positive ulp,
            // whose square root moves the corner by sqrt(epsilon), not epsilon.
            const auto perpendicular = std::abs(cross(offset, direction));
            const auto radius_gap = circle.radius - perpendicular;
            // Account for the rounded support coordinates, their subtraction,
            // tangent normalization, and the two products in the cross product.
            // This is an input-roundoff envelope, independent of geometry tolerance.
            const auto coordinate_scale = std::abs(line_start.x) + std::abs(line_start.y) +
                std::abs(circle.center.x) + std::abs(circle.center.y);
            const auto roundoff = 8.0 * std::numeric_limits<double>::epsilon() *
                (coordinate_scale + std::hypot(offset.x, offset.y) + circle.radius + perpendicular);
            if (radius_gap < -roundoff)
                reject("Line and circular wall offsets do not meet at a common corner");
            discriminant = std::abs(radius_gap) <= roundoff ? 0.0 :
                radius_gap * (circle.radius + perpendicular);
        }
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

Boundary offset_loop(const Boundary& loop, const std::vector<SourceWall>& walls, OffsetKernel kernel) {
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
        corners[index] = offset_join(edges[previous], edges[index], loop[index].start, kernel);
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
    if (!source.is_object() || !source.contains("version") ||
        !source.contains("basis") || !source.contains("walls"))
        reject("Wall measurement source is incomplete");
    const auto& version = source.at("version");
    const bool translated=version==2;
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || (version != 1 && !translated) ||
        source.size()!=(translated ? 6U : 3U) ||
        !source.at("basis").is_string() || source.at("basis") != "exterior" ||
        !source.at("walls").is_array() || source.at("walls").empty() ||
        source.at("walls").size() > maximum_source_walls)
        reject("Wall measurement source has an unknown or invalid schema");
    if (translated && (!source.contains("kernel") || !source.at("kernel").is_string() ||
        (source.at("kernel")!="stable" && source.at("kernel")!="legacy_v1") ||
        !source.contains("origin_outline") || !source.at("origin_outline").is_array() ||
        source.at("origin_outline").size()!=source.at("walls").size() ||
        !source.contains("translations") || !source.at("translations").is_array() ||
        source.at("translations").empty() || source.at("translations").size()>4096 || source.dump().size()>1024*1024-4096))
        reject("Wall translation lineage is malformed or exceeds its budget");
    if (translated) {
        // Every sequential add must retain its floating-point result and every
        // resulting outline must pass topology validation. Bound their combined
        // quadratic work, including genesis/derived-outline validation, before
        // any materialization. Four-edge owners still retain all 4096 moves.
        constexpr std::size_t maximum_replay_pair_checks=8*1024*1024;
        const auto count=source.at("walls").size();
        const auto pairs=count*(count-1)/2;
        if (pairs && source.at("translations").size()+2>maximum_replay_pair_checks/pairs)
            reject("Wall translation lineage exceeds its combined replay-work budget (8388608 edge-pair checks)");
    }
    std::set<std::string, std::less<>> seen;
    std::string previous_id;
    for (const auto& record : source.at("walls")) {
        if (!record.is_object() || record.size() != (translated ? 4U : 2U) || !record.contains("id") ||
            !record.contains("context") || !record.at("id").is_string() ||
            !record.at("context").is_object())
            reject("Wall measurement source record is malformed");
        const auto id = record.at("id").get<std::string>();
        if (id.empty() || !seen.insert(id).second)
            reject("Wall measurement source IDs must be unique and non-empty");
        if (translated && !previous_id.empty() && id<=previous_id) reject("Wall translation origin records must retain canonical identity order");
        previous_id=id;
        for (const auto& [key, value] : record.at("context").items()) {
            if (std::find(context_fields.begin(), context_fields.end(), key) == context_fields.end() ||
                !value.is_string() || value.get_ref<const std::string&>().empty())
                reject("Wall measurement source context is malformed");
        }
        ids.push_back(id);
        if (translated) {
            if (!record.contains("baseline") || !record.at("baseline").is_object() ||
                record.at("baseline").size()!=3 || !record.at("baseline").contains("sweep_radians") ||
                !record.contains("thickness_m")) reject("Wall translation origin record is incomplete");
            (void)baseline(record); (void)thickness(record);
        }
    }
    if (translated) for (const auto& offset:source.at("translations")) {
        const auto value=point(offset,"Wall translation offset");
        if (value.x==0 && value.y==0) reject("Wall translation lineage contains an empty operation");
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
    auto result=source; result["walls"]=std::move(walls); return result;
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
    auto walls = read_source_walls(document.entities(), candidate_wall_ids);
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
    return derive_exterior_wall_measurement(document.entities(), wall_ids);
}

static WallMeasurementResult derive_exterior_wall_measurement_impl(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& wall_ids, OffsetKernel kernel) {
    auto walls = read_source_walls(entities, wall_ids);
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
    auto boundary = offset_loop(loop, loop_walls, kernel);
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
    std::vector<std::string> ordered_ids;
    ordered_ids.reserve(loop_walls.size());
    for (const auto& wall : loop_walls) ordered_ids.push_back(wall.id);
    return {std::move(boundary), source_for_walls(walls), std::move(ordered_ids)};
}

WallMeasurementResult derive_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& wall_ids) {
    return derive_exterior_wall_measurement_impl(entities, wall_ids, OffsetKernel::stable);
}

namespace {
Json segment_record(const Segment& value) {
    return {{"start",{value.start.x,value.start.y}},{"end",{value.end.x,value.end.y}},
        {"sweep_radians",value.sweep_radians}};
}
Json outline_record(const Boundary& value) {
    auto result=Json::array(); for (const auto& edge:value) result.push_back(segment_record(edge)); return result;
}
Boundary translation_origin_outline(const Json& source) {
    Boundary result;
    for (const auto& edge:source.at("origin_outline")) {
        if (!edge.is_object() || edge.size()!=3 || !edge.contains("sweep_radians"))
            reject("Wall translation origin outline is malformed");
        result.push_back(baseline(Json{{"baseline",edge}}));
    }
    if (!validate_boundary(result).empty()) reject("Wall translation origin outline is invalid");
    return result;
}
std::vector<std::string> aligned_physical_ids(const Boundary& actual,const WallMeasurementResult& derived) {
    std::vector<std::string> result; unsigned matches{};
    if (actual.size()!=derived.boundary.size()) reject("Wall translation origin changed physical topology");
    const auto n=actual.size();
    for (std::size_t shift=0;shift<n;++shift) for (const bool reverse:{false,true}) {
        std::vector<std::string> ids; bool match=true;
        for (std::size_t i=0;i<n;++i) {
            const auto index=(shift+(reverse ? n-i : i))%n;
            auto edge=derived.boundary[index];
            if (reverse) { std::swap(edge.start,edge.end); edge.sweep_radians=-edge.sweep_radians; }
            if (segment_record(actual[i])!=segment_record(edge)) { match=false; break; }
            ids.push_back(derived.ordered_wall_ids[index]);
        }
        if (match) { ++matches; result=std::move(ids); }
    }
    if (matches!=1) reject("Wall translation origin does not uniquely match its independently derived physical outline");
    return result;
}
// A v2 materialization is in the retained owner's winding/order. A fresh v1
// offset is canonical. Carry an edge's direction across both representations
// rather than treating the old correspondence reversal as the new reversal.
bool replacement_edge_reversed(bool old_reversed,const Boundary& old,const Boundary& replacement) {
    return old_reversed != ((signed_area(old)<0.0)!=(signed_area(replacement)<0.0));
}
bool translation_walls_current(const std::map<std::string,Entity,std::less<>>& entities,const Json& source) {
    for (const auto& record:source.at("walls")) {
        const auto found=entities.find(record.at("id").get<std::string>());
        if (found==entities.end() || found->second.type!="wall") return false;
        auto expected=baseline(record);
        for (const auto& offset:source.at("translations")) expected=transform_segment(expected,
            PlanarTransform{{},0,false,false,point(offset,"Wall translation offset")});
        if (segment_record(baseline(found->second.properties))!=segment_record(expected) ||
            thickness(found->second.properties)!=thickness(record) || wall_context(found->second.properties)!=record.at("context")) return false;
    }
    return true;
}
}

WallMeasurementResult materialize_exterior_wall_measurement(const Entity& owner) {
    if (owner.type!="measurement_boundary" || inspect_boundary_entity_version(owner).format!=BoundaryEntityFormat::identified_v1)
        reject("Physical translation lineage requires an identified measured owner");
    const auto& source=owner.properties.at("wall_measurement_source");
    std::vector<std::string> ids; validate_source_schema(source,ids);
    if (source.at("version")!=2) reject("Retained physical translation materialization requires version two lineage");
    std::map<std::string,Entity,std::less<>> captured;
    for (const auto& record:source.at("walls")) {
        auto properties=record.at("context"); properties["baseline"]=record.at("baseline");
        properties["thickness_m"]=record.at("thickness_m"); const auto id=record.at("id").get<std::string>();
        captured.emplace(id,Entity{id,"wall",std::move(properties),false,Json::object()});
    }
    const auto derived=derive_exterior_wall_measurement_impl(captured,ids,
        source.at("kernel")=="stable" ? OffsetKernel::stable : OffsetKernel::legacy_v1);
    auto outline=translation_origin_outline(source);
    auto ordered=aligned_physical_ids(outline,derived);
    for (const auto& offset:source.at("translations")) {
        const PlanarTransform move{{},0,false,false,point(offset,"Wall translation offset")};
        for (auto& edge:outline) {
            edge=transform_segment(edge,move);
            (void)baseline(Json{{"baseline",segment_record(edge)}});
        }
        for (auto& [id,wall]:captured) {
            wall.properties["baseline"]=segment_record(transform_segment(baseline(wall.properties),move));
            (void)baseline(wall.properties);
        }
        if (!validate_boundary(outline).empty()) reject("Wall translation lineage produced an invalid outline");
    }
    return {std::move(outline),source,std::move(ordered)};
}

WallMeasurementResult derive_translated_exterior_wall_measurement(
    const std::map<std::string,Entity,std::less<>>& entities,const Entity& owner,
    const std::vector<std::string>& wall_ids,const Json& proof) {
    if (!proof.is_object() || (proof.size()!=2 && proof.size()!=3) || !proof.contains("version") ||
        !proof.at("version").is_number_integer() || proof.at("version")!=1 || !proof.contains("offset"))
        reject("Typed physical translation proof is malformed");
    const auto offset=point(proof.at("offset"),"Wall translation offset");
    if (offset.x==0 && offset.y==0) reject("Physical translation requires a nonzero offset");
    const auto& previous=owner.properties.at("wall_measurement_source");
    const auto kernel=previous.at("version")==2 ? previous.at("kernel") :
        proof.contains("genesis") && proof.at("genesis").is_object() ? proof.at("genesis").value("kernel",Json(nullptr)) : Json(nullptr);
    if (kernel=="legacy_v1") (void)derive_legacy_replacement_exterior_wall_measurement(entities,owner,wall_ids);
    else (void)derive_replacement_exterior_wall_measurement(entities,owner,wall_ids);
    auto retained_ids=exterior_wall_measurement_source_ids(owner),requested=wall_ids;
    std::sort(retained_ids.begin(),retained_ids.end()); std::sort(requested.begin(),requested.end());
    if (retained_ids!=requested) reject("Physical translation cannot replace source identities");
    Json source;
    if (previous.at("version")==1) {
        if (proof.size()!=3 || !proof.contains("genesis") || !proof.at("genesis").is_object() || proof.at("genesis").size()!=3 ||
            !proof.at("genesis").contains("kernel") || !proof.at("genesis").contains("walls") ||
            !proof.at("genesis").contains("origin_outline")) reject("First physical translation requires its captured genesis");
        source=proof.at("genesis"); source["version"]=2; source["basis"]="exterior";
        source["translations"]=Json::array({proof.at("offset")});
        std::vector<std::string> origin_ids; validate_source_schema(source,origin_ids);
        std::sort(origin_ids.begin(),origin_ids.end());
        if (origin_ids!=retained_ids || outline_record(actual_boundary_geometry(owner))!=source.at("origin_outline"))
            reject("Physical translation genesis differs from the retained owner");
        Json contexts=Json::array();
        for (const auto& record:source.at("walls")) contexts.push_back({{"id",record.at("id")},{"context",record.at("context")}});
        if (normalize_source_order(previous)!=normalize_source_order(Json{{"version",1},{"basis","exterior"},{"walls",contexts}}))
            reject("Physical translation genesis changed source context");
    } else {
        if (proof.size()!=2) reject("Continued physical translation cannot replace its genesis");
        const auto old=materialize_exterior_wall_measurement(owner);
        if (outline_record(old.boundary)!=outline_record(actual_boundary_geometry(owner))) reject("Retained physical translation outline was altered");
        source=previous; source["translations"].push_back(proof.at("offset"));
    }
    auto projected=owner; projected.properties["wall_measurement_source"]=source;
    auto result=materialize_exterior_wall_measurement(projected);
    if (!translation_walls_current(entities,source) || !boundary_context_matches(owner,source))
        reject("Translated physical walls differ from their exact retained source records");
    return result;
}

WallMeasurementResult derive_legacy_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::vector<std::string>& wall_ids) {
    return derive_exterior_wall_measurement_impl(entities, wall_ids, OffsetKernel::legacy_v1);
}

std::vector<std::string> exterior_wall_measurement_source_ids(const Entity& owner) {
    if (owner.type != "measurement_boundary" || !owner.properties.is_object() ||
        !owner.properties.contains("wall_measurement_source"))
        reject("Exterior measurement source is missing from its measured owner");
    std::vector<std::string> ids;
    validate_source_schema(owner.properties.at("wall_measurement_source"), ids);
    if (ids.size() < 3) reject("Exterior source requires at least three walls");
    return ids;
}

static WallMeasurementResult derive_replacement_exterior_wall_measurement_impl(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const std::vector<std::string>& wall_ids, OffsetKernel kernel) {
    if (owner.type != "measurement_boundary" ||
        inspect_boundary_entity_version(owner).format != BoundaryEntityFormat::identified_v1 ||
        !owner.properties.contains("wall_measurement_source"))
        reject("Wall source replacement requires an identified exterior measured area");
    (void)exterior_wall_measurement_source_ids(owner);
    const auto& previous_source = owner.properties.at("wall_measurement_source");
    if (previous_source.at("version")==2) {
        const auto retained_translation=materialize_exterior_wall_measurement(owner);
        if (outline_record(retained_translation.boundary)!=outline_record(actual_boundary_geometry(owner)))
            reject("Retained physical translation outline differs from its intrinsic proof");
    }
    const auto organization = organize_project(entities);
    const auto owner_context = organization.drawing_context(owner.id);
    if (!owner_context || !owner_context->complete())
        reject("Exterior measured area requires fully resolved property, building, floor and layer");
    const auto& first_context = previous_source.at("walls").front().at("context");
    const auto phase = [](const Json& context) {
        return context.contains("phase_id") ? context.at("phase_id") : Json(nullptr);
    };
    const auto original_phase = phase(first_context);
    const auto contextual_id = [&](std::string_view field) -> std::string {
        if (field == "property_id") return owner_context->property_id;
        if (field == "building_id") return owner_context->building_id;
        if (field == "floor_id") return owner_context->floor_id;
        return owner_context->layer_id;
    };
    for (const auto& record : previous_source.at("walls")) {
        const auto& context = record.at("context");
        if (!context.contains("layer_id") ||
            !organization.drawing_context(context.at("layer_id").get<std::string>()))
            reject("Original exterior source requires a fully resolved recorded drawing layer");
        if (phase(context) != original_phase)
            reject("Original exterior sources disagree on design phase");
        for (const auto field : {"property_id", "building_id", "floor_id", "layer_id"})
            if (context.contains(field) && context.at(field) != contextual_id(field))
                reject("Original exterior source context disagrees with its measured owner");
    }
    if (owner.properties.contains("phase_id") && owner.properties.at("phase_id") != original_phase)
        reject("Exterior measured owner and original source phases disagree");
    const auto effective_elevation = [&](const Entity& wall) {
        const auto resolved = resolve_vertical_placement(entities, wall);
        auto value = resolved.properties.find("elevation_m");
        if (value == resolved.properties.end()) value = resolved.properties.find("elevation");
        const auto elevation = value == resolved.properties.end() ? 0.0 : number(*value, "Wall elevation");
        if (!bounded(elevation)) reject("Wall elevation exceeds the supported geometry envelope");
        return elevation;
    };
    std::optional<double> plane;
    const auto check_plane = [&](double elevation) {
        if (!plane) plane = elevation;
        else if (std::abs(*plane - elevation) > default_geometry_tolerance_metres)
            reject("Replacement and surviving source walls must share one effective elevation plane");
    };
    const auto walls = read_source_walls(entities, wall_ids);
    for (const auto& [id, entity] : entities) {
        (void)id;
        if (entity.type != "model_phases") continue;
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        const auto active = model.active_state();
        for (const auto& wall : walls)
            if (std::find(model.entity_ids().begin(), model.entity_ids().end(), wall.id) != model.entity_ids().end() &&
                (!active.contains(wall.id) || active.at(wall.id) == ModelPhase::demolished))
                reject("Replacement source wall is unavailable in the active design phase");
    }
    for (const auto& wall : walls) {
        const auto context = organization.drawing_context(wall.id);
        if (!context || !context->complete() || context->property_id != owner_context->property_id ||
            context->building_id != owner_context->building_id || context->floor_id != owner_context->floor_id ||
            context->layer_id != owner_context->layer_id || phase(wall.context) != original_phase)
            reject("Replacement source walls must retain the measured owner's hierarchy and original phase");
        check_plane(effective_elevation(entities.at(wall.id)));
    }
    for (const auto& record : previous_source.at("walls")) {
        const auto found = entities.find(record.at("id").get<std::string>());
        if (found == entities.end()) continue;
        if (found->second.type != "wall" || wall_context(found->second.properties) != record.at("context"))
            reject("A surviving original wall no longer matches its recorded source context");
        const auto context = organization.drawing_context(found->first);
        if (!context || !context->complete() || context->property_id != owner_context->property_id ||
            context->building_id != owner_context->building_id || context->floor_id != owner_context->floor_id ||
            context->layer_id != owner_context->layer_id)
            reject("A surviving original source has unresolved or contradictory hierarchy");
        check_plane(effective_elevation(found->second));
    }
    for (const auto* field : {"elevation_m", "elevation"})
        if (owner.properties.contains(field)) { check_plane(number(owner.properties.at(field), "Measured owner elevation")); break; }
    // Ordinary replacement always derives v1 geometry from the requested IDs.
    // Only the separately typed translation may retain an exact v2 outline.
    return derive_exterior_wall_measurement_impl(entities, wall_ids, kernel);
}

WallMeasurementResult derive_replacement_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const std::vector<std::string>& wall_ids) {
    return derive_replacement_exterior_wall_measurement_impl(entities, owner, wall_ids, OffsetKernel::stable);
}

WallMeasurementResult derive_legacy_replacement_exterior_wall_measurement(
    const std::map<std::string, Entity, std::less<>>& entities, const Entity& owner,
    const std::vector<std::string>& wall_ids) {
    return derive_replacement_exterior_wall_measurement_impl(entities, owner, wall_ids, OffsetKernel::legacy_v1);
}

bool wall_measurement_source_current(const DocumentSnapshot& document, const Entity& boundary) {
    return wall_measurement_source_current(document.entities(), boundary);
}

bool wall_measurement_source_current(const std::map<std::string, Entity, std::less<>>& entities,
    const Entity& boundary) {
    try {
        if (!boundary.properties.is_object()) return false;
        const auto source_property = boundary.properties.find("wall_measurement_source");
        if (source_property == boundary.properties.end()) return true;
        if (boundary.type != "boundary" && boundary.type != "measurement_boundary") return false;
        std::vector<std::string> wall_ids;
        validate_source_schema(*source_property, wall_ids);
        const auto actual = actual_boundary_geometry(boundary);
        if (!validate_boundary(actual).empty()) return false;
        if (source_property->at("version")==2) {
            const auto expected=materialize_exterior_wall_measurement(boundary);
            return boundary_context_matches(boundary,*source_property) &&
                translation_walls_current(entities,*source_property) && outline_record(actual)==outline_record(expected.boundary);
        }
        const auto matches = [&](OffsetKernel kernel) {
            const auto expected = derive_exterior_wall_measurement_impl(entities, wall_ids, kernel);
            return normalize_source_order(*source_property) == expected.source &&
                boundary_context_matches(boundary, expected.source) &&
                edge_keys(actual) == edge_keys(expected.boundary);
        };
        try {
            if (matches(OffsetKernel::stable)) return true;
        } catch (const std::invalid_argument&) {
            // A retained v1 gap may be outside the stable roundoff envelope.
            // The legacy result must still match the complete source/outline.
        }
        return matches(OffsetKernel::legacy_v1);
    } catch (const std::exception&) {
        return false;
    }
}

nlohmann::json encode_exterior_corner_move(const ExteriorCornerMoveIntent& intent) {
    BoundaryGeometryEdit edit;
    edit.boundary_id = intent.boundary_id;
    edit.target_id = intent.vertex_id;
    edit.target_position = intent.target_position;
    validate_boundary_geometry_edit(edit);
    return {{"version", 1}, {"boundary_id", intent.boundary_id}, {"vertex_id", intent.vertex_id},
        {"position", {intent.target_position.x, intent.target_position.y}},
        {"move_connected_objects", intent.move_connected_objects}};
}

namespace {
using PhysicalContact = ExteriorCornerPhysicalContact;
std::vector<PhysicalContact> physical_contacts(const std::map<std::string, Entity, std::less<>>& original) {
    struct PhysicalWall { Segment geometry; std::string property, building, floor; Json phase; double low, high; };
    std::map<std::string, PhysicalWall, std::less<>> walls;
    const auto organization = organize_project(original);
    std::set<std::string, std::less<>> unavailable;
    for (const auto& [id,entity] : original) {
        (void)id;
        if (entity.type != "model_phases") continue;
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        const auto active = model.active_state();
        for (const auto& entity_id : model.entity_ids())
            if (!active.contains(entity_id) || active.at(entity_id) == ModelPhase::demolished)
                unavailable.insert(entity_id);
    }
    std::vector<std::string> active_wall_ids;
    for (const auto& [id, entity] : original) {
        if (entity.type != "wall" || !entity.properties.contains("baseline") || unavailable.contains(id)) continue;
        active_wall_ids.push_back(id);
    }
    const auto checked_context = [&](const std::string& id) {
        const auto& entity = original.at(id);
        const auto context = organization.drawing_context(id);
        if (!context && (entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id")))
            reject("Physical wall contact has unresolved drawing context: " + id);
        return context;
    };
    const auto append_wall = [&](const std::string& id, const Entity& placement) {
        const auto& entity = original.at(id);
        const auto context = checked_context(id);
        auto field = placement.properties.find("elevation_m");
        if (field == placement.properties.end()) field = placement.properties.find("elevation");
        const auto low = field == placement.properties.end() ? 0.0 : number(*field, "Wall elevation");
        const auto high = low + number(placement.properties.at("height_m"), "Wall height");
        if (!std::isfinite(high)) reject("Physical wall contact vertical extent exceeds supported range");
        walls.emplace(id, PhysicalWall{baseline(entity.properties), context ? context->property_id : "",
            context ? context->building_id : "", context ? context->floor_id : "",
            entity.properties.value("phase_id", Json(nullptr)), low, high});
    };
    const auto placements = [&] {
        try { return resolve_vertical_placements(original,active_wall_ids); }
        catch (...) {
            // Preserve the original first diagnostic when several walls are
            // malformed. The normal path batches placement; only rejection
            // replays context/placement/geometry validation in owner order.
            for (const auto& id : active_wall_ids) {
                (void)checked_context(id);
                append_wall(id,resolve_vertical_placement(original,original.at(id)));
            }
            throw;
        }
    }();
    for (const auto& id : active_wall_ids) append_wall(id,placements.at(id));
    std::vector<PhysicalContact> contacts;
    for (const auto& [id, wall] : walls)
        for (const auto& [host_id, host] : walls) {
            if (id == host_id || wall.property != host.property || wall.building != host.building ||
                wall.floor != host.floor || wall.phase != host.phase ||
                std::max(wall.low,host.low) >= std::min(wall.high,host.high) + default_geometry_tolerance_metres) continue;
            const auto& before = wall.geometry;
            const auto& old_host = host.geometry;
            for (const bool start : {true, false}) {
                const auto endpoint = start ? before.start : before.end;
                std::optional<double> station;
                if (distance(endpoint, old_host.start) <= default_geometry_tolerance_metres) station = 0;
                else if (distance(endpoint, old_host.end) <= default_geometry_tolerance_metres) station = 1;
                else if (old_host.sweep_radians != 0) station = arc_fraction_if_on(old_host, endpoint);
                else {
                    const auto chord = subtract(old_host.end, old_host.start);
                    const auto fraction = dot(subtract(endpoint, old_host.start), chord) / dot(chord, chord);
                    if (fraction >= 0 && fraction <= 1 &&
                        distance(endpoint, point_at_fraction(old_host, fraction)) <= default_geometry_tolerance_metres) station = fraction;
                }
                if (station) {
                    if (contacts.size() >= maximum_network_edges)
                        reject("Physical wall contact graph exceeds the supported contact count");
                    contacts.push_back({id,start,host_id,*station});
                }
            }
        }
    return contacts;
}
}

std::vector<ExteriorCornerPhysicalContact> exterior_corner_physical_contact_graph(
    const std::map<std::string, Entity, std::less<>>& original) { return physical_contacts(original); }

void validate_exterior_corner_physical_contacts(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& proposed) {
    for (const auto& contact : physical_contacts(original)) {
        if (!proposed.contains(contact.owner) || !proposed.contains(contact.host))
            reject("Exterior corner cannot remove an existing physical wall contact owner");
        const auto after = baseline(proposed.at(contact.owner).properties);
        const auto host = baseline(proposed.at(contact.host).properties);
        const auto target = contact.station == 0 ? host.start : contact.station == 1 ? host.end : point_at_fraction(host,contact.station);
        if (distance(contact.start ? after.start : after.end,target) > default_geometry_tolerance_metres)
            reject("Exterior corner would detach an existing physical wall corner or T station");
    }
}

ExteriorCornerMoveIntent decode_exterior_corner_move(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 5 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("boundary_id") || !value.at("boundary_id").is_string() ||
        !value.contains("vertex_id") || !value.at("vertex_id").is_string() ||
        !value.contains("position") || !value.contains("move_connected_objects") ||
        !value.at("move_connected_objects").is_boolean())
        reject("Exterior corner move proof contains unsupported fields");
    ExteriorCornerMoveIntent result{value.at("boundary_id").get<std::string>(),
        value.at("vertex_id").get<std::string>(), point(value.at("position"), "Exterior target"),
        value.at("move_connected_objects").get<bool>()};
    (void)encode_exterior_corner_move(result);
    return result;
}

std::vector<std::string> exterior_corner_perimeter_ids(
    const std::map<std::string, Entity, std::less<>>& original, const Entity& owner) {
    const auto ids = exterior_wall_measurement_source_ids(owner);
    try { return derive_exterior_wall_measurement(original,ids).ordered_wall_ids; }
    catch (const std::invalid_argument&) { return derive_legacy_exterior_wall_measurement(original,ids).ordered_wall_ids; }
}

// Forward/inverse offset joins use normalized tangents, circular supports and
// line/circle intersections. Account for their coordinate additions, products
// and angle subtraction, with the crossing-angle conditioning at each miter.
// This is an arithmetic envelope, not permission to move a model vertex by the
// ordinary geometry tolerance. Ill-conditioned requests fail closed.
static double measured_arc_roundoff(const Boundary& target) {
    double scale=1.0, conditioning=1.0;
    for (std::size_t i=0;i<target.size();++i) {
        const auto& edge=target[i];
        scale=std::max(scale,std::abs(edge.start.x)+std::abs(edge.start.y)+
            std::abs(edge.end.x)+std::abs(edge.end.y)+segment_length(edge));
        if (edge.sweep_radians!=0) {
            const auto support=arc_support(edge);
            scale=std::max(scale,std::abs(support.center.x)+std::abs(support.center.y)+support.radius);
        }
        const auto& previous=target[(i+target.size()-1)%target.size()];
        const auto entering=std::atan2(previous.end.y-previous.start.y,previous.end.x-previous.start.x)+
            previous.sweep_radians*0.5;
        const auto leaving=std::atan2(edge.end.y-edge.start.y,edge.end.x-edge.start.x)-edge.sweep_radians*0.5;
        const auto crossing=std::abs(std::sin(leaving-entering));
        if (crossing>parallel_direction_tolerance) conditioning=std::max(conditioning,1.0/crossing);
        // At an exact tangent the stable intersection kernel clamps its
        // discriminant inside the support-input roundoff envelope to zero;
        // this avoids the square-root amplification of a rounded positive ulp.
    }
    const auto result=256.0*std::numeric_limits<double>::epsilon()*scale*conditioning;
    if (!std::isfinite(result) || result>default_geometry_tolerance_metres)
        reject("Measured arc inverse cannot establish endpoint fidelity within analytical roundoff");
    return result;
}

static void validate_measured_arc_edge(const Segment& actual,const Segment& target,double tolerance) {
    if ((actual.sweep_radians==0)!=(target.sweep_radians==0) ||
        (actual.sweep_radians!=0 && (std::signbit(actual.sweep_radians)!=std::signbit(target.sweep_radians) ||
            std::max(arc_support(actual).radius,arc_support(target).radius)*
                std::abs(actual.sweep_radians-target.sweep_radians)>tolerance)) ||
        distance(actual.start,target.start)>tolerance || distance(actual.end,target.end)>tolerance ||
        distance(point_at_fraction(actual,0.25),point_at_fraction(target,0.25))>tolerance ||
        distance(point_at_fraction(actual,0.5),point_at_fraction(target,0.5))>tolerance ||
        distance(point_at_fraction(actual,0.75),point_at_fraction(target,0.75))>tolerance)
        reject("Measured arc final forward geometry differs from its complete requested analytical outline");
}

static std::map<std::string, Entity, std::less<>> inverse_exterior_outline(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::string& boundary_id, const Boundary& requested, bool move_connected_objects,
    bool restore_shared_vertices = false, bool measured_arc_authority = false) {
    const auto found = original.find(boundary_id);
    if (found == original.end()) reject("Exterior corner measured owner does not exist");
    const auto& owner = found->second;
    const auto ids = exterior_wall_measurement_source_ids(owner);
    const auto identified = decode_identified_boundary_entity(owner);
    const auto actual = boundary_geometry(identified);
    std::optional<WallMeasurementResult> old;
    std::size_t alignment{};
    bool reversed{};
    unsigned matches{};
    const auto exact = [](const Segment& a, const Segment& b) {
        return a.start.x == b.start.x && a.start.y == b.start.y && a.end.x == b.end.x &&
            a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
    };
    for (const auto kernel : {OffsetKernel::stable, OffsetKernel::legacy_v1}) {
        WallMeasurementResult derived;
        try { derived = owner.properties.at("wall_measurement_source").at("version")==2
            ? materialize_exterior_wall_measurement(owner) : derive_exterior_wall_measurement_impl(original, ids, kernel); }
        catch (const std::invalid_argument&) { continue; }
        if (normalize_source_order(owner.properties.at("wall_measurement_source")) != derived.source ||
            !boundary_context_matches(owner, derived.source) || actual.size() != derived.boundary.size()) continue;
        if (owner.properties.at("wall_measurement_source").at("version")==2 &&
            !wall_measurement_source_current(original,owner)) continue;
        for (std::size_t offset = 0; offset < actual.size(); ++offset)
            for (const bool reverse : {false, true}) {
                bool match = true;
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    auto edge = derived.boundary[(offset + (reverse ? actual.size() - i : i)) % actual.size()];
                    if (reverse) { std::swap(edge.start, edge.end); edge.sweep_radians = -edge.sweep_radians; }
                    if (!exact(actual[i], edge)) { match = false; break; }
                }
                if (match) { ++matches; alignment = offset; reversed = reverse; }
            }
        if (matches) { old = std::move(derived); break; }
    }
    if (!old || matches != 1) reject("Exterior corner requires a current unambiguous physical wall source");
    if (requested.size() != actual.size()) reject("Exterior inverse requested topology differs from its source");
    if (const auto errors = validate_boundary(requested); !errors.empty())
        reject("Exterior corner requested outline is invalid: " + errors.front().message);
    const auto walls = read_source_walls(original, ids);
    std::map<std::string, SourceWall, std::less<>> by_id;
    for (const auto& wall : walls) by_id.emplace(wall.id, wall);
    std::vector<SourceWall> ordered;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        auto wall = by_id.at(old->ordered_wall_ids[(alignment + (reversed ? actual.size() - i : i)) % actual.size()]);
        wall.thickness = -wall.thickness; // Exact analytical inverse, including unequal thicknesses.
        ordered.push_back(std::move(wall));
    }
    auto inverse = offset_loop(requested, ordered, OffsetKernel::stable);
    if (restore_shared_vertices) {
        // A corner is one physical vertex shared by both adjacent walls. Restore
        // a nearly unchanged corner once, before restoring any unchanged whole
        // baseline; independent wall restoration can otherwise leave an exact
        // cycle gap between an old coordinate and its rounded inverse result.
        // Historical corner proofs keep their original restoration arithmetic.
        for (std::size_t i = 0; i < inverse.size(); ++i) {
            const auto previous = (i + inverse.size() - 1) % inverse.size();
            const auto& old_baseline = ordered[i].baseline;
            const auto& old_previous = ordered[previous].baseline;
            const auto old_start = dot(subtract(actual[i].end, actual[i].start),
                                       subtract(old_baseline.end, old_baseline.start)) < 0 ?
                old_baseline.end : old_baseline.start;
            const auto old_end = dot(subtract(actual[previous].end, actual[previous].start),
                                     subtract(old_previous.end, old_previous.start)) < 0 ?
                old_previous.start : old_previous.end;
            if (old_start.x == old_end.x && old_start.y == old_end.y &&
                inverse[i].start.x == inverse[previous].end.x &&
                inverse[i].start.y == inverse[previous].end.y &&
                distance(inverse[i].start, old_start) <= 1e-12) {
                inverse[i].start = old_start;
                inverse[previous].end = old_start;
            }
        }
    }
    auto candidate = original;
    std::map<std::string, Segment, std::less<>> targets;
    for (std::size_t i = 0; i < inverse.size(); ++i) {
        auto next = inverse[i];
        const auto& old_baseline = ordered[i].baseline;
        if (dot(subtract(actual[i].end, actual[i].start), subtract(old_baseline.end, old_baseline.start)) < 0) {
            std::swap(next.start, next.end); next.sweep_radians = -next.sweep_radians;
        }
        if (distance(next.start, old_baseline.start) <= 1e-12 &&
            distance(next.end, old_baseline.end) <= 1e-12 &&
            std::abs(next.sweep_radians - old_baseline.sweep_radians) <= 1e-12 &&
            (!restore_shared_vertices || (next.start.x == old_baseline.start.x &&
                next.start.y == old_baseline.start.y && next.end.x == old_baseline.end.x &&
                next.end.y == old_baseline.end.y)))
            next = old_baseline;
        targets.emplace(ordered[i].id, next);
    }
    // Preserve existing physical endpoint contacts and T stations, including
    // multiple-host agreement. A partition's unattached endpoint stays fixed.
    const auto contacts = physical_contacts(original);
    std::map<std::string, std::vector<PhysicalContact>, std::less<>> contacts_by_owner;
    for (const auto& contact : contacts) contacts_by_owner[contact.owner].push_back(contact);
    const std::set<std::string, std::less<>> source_ids(old->ordered_wall_ids.begin(), old->ordered_wall_ids.end());
    bool converged = false;
    for (std::size_t iteration = 0; iteration <= contacts_by_owner.size(); ++iteration) {
        const auto previous_targets = targets;
    for (const auto& [id, entity] : original) {
        if (entity.type != "wall" || source_ids.contains(id) || !entity.properties.contains("baseline")) continue;
        const auto previous = baseline(entity.properties);
        auto next = previous;
        for (const bool start : {true, false}) {
            const auto endpoint = start ? previous.start : previous.end;
            std::optional<Vec2> destination;
            for (const auto& contact : contacts_by_owner[id]) {
                if (contact.start != start || !previous_targets.contains(contact.host)) continue;
                const auto& host = previous_targets.at(contact.host);
                const auto proposed = contact.station == 0 ? host.start :
                    contact.station == 1 ? host.end : point_at_fraction(host, contact.station);
                if (destination && distance(*destination, proposed) > default_geometry_tolerance_metres) {
                    if (distance(*destination, endpoint) <= default_geometry_tolerance_metres) destination = proposed;
                    else if (distance(proposed, endpoint) > default_geometry_tolerance_metres)
                        reject("Exterior corner attached endpoint has contradictory physical hosts");
                } else destination = proposed;
            }
            if (destination && distance(endpoint, *destination) > 1e-12) {
                if (!move_connected_objects) reject("Exterior corner would detach a frozen connected wall");
                (start ? next.start : next.end) = *destination;
            }
        }
        if (!exact(previous, next)) targets.insert_or_assign(id, next);
    }
        bool changed = targets.size() != previous_targets.size();
        for (const auto& [id, target] : targets)
            changed = changed || !previous_targets.contains(id) || !exact(target, previous_targets.at(id));
        if (!changed) { converged = true; break; }
    }
    if (!converged) reject("Exterior corner physical attachment propagation did not converge");
    for (const auto& [id, target] : targets) {
        if (source_ids.contains(id)) candidate.at(id) = measured_arc_authority ?
            reconstruct_exterior_segment_arc_wall(original.at(id),target) :
            reconstruct_exterior_corner_wall(original.at(id), target);
        else {
            // Provisional attachment coordinates feed the solve. Their final
            // geometry/provenance is replayed from the original typed wall edit.
            auto& recorded = candidate.at(id).properties["baseline"];
            recorded["start"] = {target.start.x,target.start.y};
            recorded["end"] = {target.end.x,target.end.y};
            recorded["sweep_radians"] = target.sweep_radians;
        }
    }
    validate_exterior_corner_physical_contacts(original, candidate);
    for (const auto& id : source_ids) validate_constraint_wall_host(id, candidate);
    // Compare against the actual forward result; never replace these bytes with
    // the user's target coordinates merely to make exact currentness pass.
    const auto forward = derive_replacement_exterior_wall_measurement(candidate, owner, ids);
    const auto forward_reversed=replacement_edge_reversed(reversed,old->boundary,forward.boundary);
    std::map<std::string, Segment, std::less<>> exterior;
    for (std::size_t i = 0; i < forward.boundary.size(); ++i)
        exterior.emplace(forward.ordered_wall_ids[i], forward.boundary[i]);
    const auto arc_tolerance=measured_arc_authority ? measured_arc_roundoff(requested) : 0.0;
    for (std::size_t i = 0; i < requested.size(); ++i) {
        auto edge = exterior.at(ordered[i].id);
        if (forward_reversed) { std::swap(edge.start, edge.end); edge.sweep_radians = -edge.sweep_radians; }
        if (measured_arc_authority) {
            validate_measured_arc_edge(edge,requested[i],arc_tolerance);
            continue;
        }
        if ((edge.sweep_radians == 0) != (requested[i].sweep_radians == 0) ||
            (edge.sweep_radians != 0 &&
                (std::signbit(edge.sweep_radians) != std::signbit(requested[i].sweep_radians) ||
                 (((std::abs(edge.sweep_radians) > std::numbers::pi) != (std::abs(requested[i].sweep_radians) > std::numbers::pi)) &&
                    std::min(std::abs(std::abs(edge.sweep_radians)-std::numbers::pi),
                        std::abs(std::abs(requested[i].sweep_radians)-std::numbers::pi)) *
                    std::max(arc_support(edge).radius,arc_support(requested[i]).radius) > default_geometry_tolerance_metres) ||
                 std::max(arc_support(edge).radius, arc_support(requested[i]).radius) *
                    std::abs(edge.sweep_radians - requested[i].sweep_radians) > default_geometry_tolerance_metres)))
            reject("Exterior corner cannot reproduce the requested circular sweep and branch");
        if (distance(edge.start, requested[i].start) > default_geometry_tolerance_metres ||
            distance(edge.end, requested[i].end) > default_geometry_tolerance_metres ||
            distance(point_at_fraction(edge, 0.25), point_at_fraction(requested[i], 0.25)) > default_geometry_tolerance_metres ||
            distance(point_at_fraction(edge, 0.5), point_at_fraction(requested[i], 0.5)) > default_geometry_tolerance_metres ||
            distance(point_at_fraction(edge, 0.75), point_at_fraction(requested[i], 0.75)) > default_geometry_tolerance_metres)
            reject("Exterior corner cannot reproduce the requested analytical outline within roundoff tolerance");
    }
    return candidate;
}

std::map<std::string, Entity, std::less<>> exterior_corner_physical_entities(
    const std::map<std::string, Entity, std::less<>>& original,
    const ExteriorCornerMoveIntent& intent) {
    (void)encode_exterior_corner_move(intent);
    const auto found = original.find(intent.boundary_id);
    if (found == original.end()) reject("Exterior corner measured owner does not exist");
    const auto identified = decode_identified_boundary_entity(found->second);
    auto requested = boundary_geometry(identified);
    bool vertex_found = false;
    for (std::size_t i = 0; i < identified.segments.size(); ++i)
        if (identified.segments[i].start_vertex_id == intent.vertex_id) {
            requested[i].start = intent.target_position;
            requested[(i + requested.size() - 1) % requested.size()].end = intent.target_position;
            vertex_found = true;
        }
    if (!vertex_found) reject("Exterior corner stable vertex does not exist");
    return inverse_exterior_outline(original, intent.boundary_id, requested, intent.move_connected_objects);
}

nlohmann::json encode_exterior_segment_resize(const ExteriorSegmentResizeIntent& intent) {
    if (intent.fixed_endpoint != BoundaryFixedEndpoint::start && intent.fixed_endpoint != BoundaryFixedEndpoint::end)
        reject("Exterior segment resize fixed endpoint is unsupported");
    BoundaryGeometryEdit edit;
    edit.kind = BoundaryGeometryEditKind::resize_segment;
    edit.boundary_id = intent.boundary_id;
    edit.target_id = intent.segment_id;
    edit.target_length_metres = intent.exact_length.metres;
    edit.fixed_endpoint = intent.fixed_endpoint;
    edit.move_connected = intent.move_boundary_chain;
    validate_boundary_geometry_edit(edit);
    const auto receipt = encode_constraint_quantity_receipt(intent.exact_length);
    // The shared receipt encoder reparses exact input; positive finite geometry
    // validation above also excludes unusable target magnitudes and anchors.
    return {{"version", 1}, {"boundary_id", intent.boundary_id}, {"segment_id", intent.segment_id},
        {"exact_length", receipt}, {"fixed_endpoint", intent.fixed_endpoint == BoundaryFixedEndpoint::start ? "start" : "end"},
        {"move_boundary_chain", intent.move_boundary_chain}, {"move_connected_objects", intent.move_connected_objects}};
}

ExteriorSegmentResizeIntent decode_exterior_segment_resize(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 7 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("boundary_id") || !value.at("boundary_id").is_string() ||
        !value.contains("segment_id") || !value.at("segment_id").is_string() ||
        !value.contains("exact_length") || !value.contains("fixed_endpoint") ||
        !value.at("fixed_endpoint").is_string() ||
        (value.at("fixed_endpoint") != "start" && value.at("fixed_endpoint") != "end") ||
        !value.contains("move_boundary_chain") || !value.at("move_boundary_chain").is_boolean() ||
        !value.contains("move_connected_objects") || !value.at("move_connected_objects").is_boolean())
        reject("Exterior segment resize proof contains unsupported fields");
    ExteriorSegmentResizeIntent result{value.at("boundary_id").get<std::string>(), value.at("segment_id").get<std::string>(),
        decode_constraint_quantity_receipt(value.at("exact_length")),
        value.at("fixed_endpoint") == "start" ? BoundaryFixedEndpoint::start : BoundaryFixedEndpoint::end,
        value.at("move_boundary_chain").get<bool>(), value.at("move_connected_objects").get<bool>()};
    if (value.at("exact_length") != encode_constraint_quantity_receipt(result.exact_length))
        reject("Exterior segment resize exact quantity receipt contains unsupported fields");
    (void)encode_exterior_segment_resize(result);
    return result;
}

std::map<std::string, Entity, std::less<>> exterior_segment_resize_physical_entities(
    const std::map<std::string, Entity, std::less<>>& original,
    const ExteriorSegmentResizeIntent& intent) {
    (void)encode_exterior_segment_resize(intent);
    const auto found = original.find(intent.boundary_id);
    if (found == original.end()) reject("Exterior resize measured owner does not exist");
    const auto identified = decode_identified_boundary_entity(found->second);
    const auto requested = set_boundary_segment_length(identified, intent.segment_id, intent.exact_length.metres,
                                                       intent.fixed_endpoint, intent.move_boundary_chain);
    return inverse_exterior_outline(original, intent.boundary_id, boundary_geometry(requested), intent.move_connected_objects, true);
}

void validate_exterior_segment_resize_result(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& final,
    const ExteriorSegmentResizeIntent& intent) {
    (void)encode_exterior_segment_resize(intent);
    const auto old_owner = original.find(intent.boundary_id);
    const auto final_owner = final.find(intent.boundary_id);
    if (old_owner == original.end() || final_owner == final.end() ||
        !old_owner->second.properties.contains("wall_measurement_source") ||
        !final_owner->second.properties.contains("wall_measurement_source") ||
        !wall_measurement_source_current(original, old_owner->second) ||
        !wall_measurement_source_current(final, final_owner->second))
        reject("Exterior resize result requires current original and final physical wall sources");
    const auto before = decode_identified_boundary_entity(old_owner->second);
    const auto after = decode_identified_boundary_entity(final_owner->second);
    auto old_sources = exterior_wall_measurement_source_ids(old_owner->second);
    auto final_sources = exterior_wall_measurement_source_ids(final_owner->second);
    std::sort(old_sources.begin(), old_sources.end());
    std::sort(final_sources.begin(), final_sources.end());
    if (old_sources != final_sources)
        reject("Exterior resize result changed its original physical source identities");
    const auto selected = [&](const IdentifiedBoundary& owner) -> const Segment& {
        const auto edge = std::find_if(owner.segments.begin(), owner.segments.end(),
            [&](const auto& value) { return value.segment_id == intent.segment_id; });
        if (edge == owner.segments.end()) reject("Exterior resize selected stable edge does not exist");
        return edge->segment;
    };
    const auto& old_edge = selected(before);
    const auto& edge = selected(after);
    const auto expected = set_boundary_segment_length(before, intent.segment_id, intent.exact_length.metres,
                                                       intent.fixed_endpoint, intent.move_boundary_chain);
    const auto& target = selected(expected);
    const auto anchor = intent.fixed_endpoint == BoundaryFixedEndpoint::start ? old_edge.start : old_edge.end;
    const auto actual_anchor = intent.fixed_endpoint == BoundaryFixedEndpoint::start ? edge.start : edge.end;
    if (std::abs(segment_length(edge) - intent.exact_length.metres) > default_geometry_tolerance_metres ||
        distance(anchor, actual_anchor) > default_geometry_tolerance_metres ||
        (edge.sweep_radians == 0) != (old_edge.sweep_radians == 0) ||
        (edge.sweep_radians != 0 && (std::signbit(edge.sweep_radians) != std::signbit(old_edge.sweep_radians) ||
            std::max(arc_support(edge).radius, arc_support(target).radius) *
                std::abs(edge.sweep_radians - old_edge.sweep_radians) > default_geometry_tolerance_metres)) ||
        distance(edge.start, target.start) > default_geometry_tolerance_metres ||
        distance(edge.end, target.end) > default_geometry_tolerance_metres ||
        distance(point_at_fraction(edge, 0.25), point_at_fraction(target, 0.25)) > default_geometry_tolerance_metres ||
        distance(point_at_fraction(edge, 0.5), point_at_fraction(target, 0.5)) > default_geometry_tolerance_metres ||
        distance(point_at_fraction(edge, 0.75), point_at_fraction(target, 0.75)) > default_geometry_tolerance_metres)
        reject("Exterior resize final forward geometry differs from its requested length, anchor or circular sweep");
}

nlohmann::json encode_exterior_segment_arc(const ExteriorSegmentArcIntent& intent) {
    BoundaryGeometryEdit edit;
    edit.kind=BoundaryGeometryEditKind::reconstruct_arc;
    edit.boundary_id=intent.boundary_id; edit.target_id=intent.segment_id;
    edit.arc_construction=intent.arc_construction;
    validate_boundary_geometry_edit(edit);
    return {{"version",1},{"boundary_id",intent.boundary_id},{"segment_id",intent.segment_id},
        {"arc_construction",encode_construction_receipt(intent.arc_construction)},
        {"move_connected_objects",intent.move_connected_objects}};
}

ExteriorSegmentArcIntent decode_exterior_segment_arc(const nlohmann::json& value) {
    if (!value.is_object() || value.size()!=5 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version")!=1 ||
        !value.contains("boundary_id") || !value.at("boundary_id").is_string() ||
        !value.contains("segment_id") || !value.at("segment_id").is_string() ||
        !value.contains("arc_construction") || !value.contains("move_connected_objects") ||
        !value.at("move_connected_objects").is_boolean())
        reject("Exterior segment arc proof contains unsupported fields");
    ExteriorSegmentArcIntent result{value.at("boundary_id").get<std::string>(),value.at("segment_id").get<std::string>(),
        decode_construction_receipt(value.at("arc_construction")),value.at("move_connected_objects").get<bool>()};
    if (encode_exterior_segment_arc(result)!=value)
        reject("Exterior segment arc proof contains noncanonical construction fields");
    return result;
}

std::map<std::string,Entity,std::less<>> exterior_segment_arc_physical_entities(
    const std::map<std::string,Entity,std::less<>>& original,const ExteriorSegmentArcIntent& intent) {
    (void)encode_exterior_segment_arc(intent);
    const auto found=original.find(intent.boundary_id);
    if (found==original.end()) reject("Exterior arc measured owner does not exist");
    const auto requested=reconstruct_boundary_arc(decode_identified_boundary_entity(found->second),
        intent.segment_id,intent.arc_construction);
    const auto geometry=boundary_geometry(requested);
    (void)measured_arc_roundoff(geometry);
    return inverse_exterior_outline(original,intent.boundary_id,geometry,intent.move_connected_objects,true,true);
}

void validate_exterior_segment_arc_result(const std::map<std::string,Entity,std::less<>>& original,
    const std::map<std::string,Entity,std::less<>>& final,const ExteriorSegmentArcIntent& intent) {
    (void)encode_exterior_segment_arc(intent);
    const auto old=original.find(intent.boundary_id), current=final.find(intent.boundary_id);
    if (old==original.end() || current==final.end() ||
        !old->second.properties.contains("wall_measurement_source") ||
        !current->second.properties.contains("wall_measurement_source") ||
        !wall_measurement_source_current(original,old->second) ||
        !wall_measurement_source_current(final,current->second))
        reject("Exterior arc result requires current original and final physical wall sources");
    auto old_ids=exterior_wall_measurement_source_ids(old->second);
    auto final_ids=exterior_wall_measurement_source_ids(current->second);
    std::sort(old_ids.begin(),old_ids.end()); std::sort(final_ids.begin(),final_ids.end());
    if (old_ids!=final_ids) reject("Exterior arc result changed its physical source identities");
    const auto old_walls=read_source_walls(original,old_ids), final_walls=read_source_walls(final,final_ids);
    for (std::size_t i=0;i<old_walls.size();++i)
        if (old_walls[i].id!=final_walls[i].id || old_walls[i].thickness!=final_walls[i].thickness)
            reject("Exterior arc result changed its physical wall thickness");
    const auto requested=reconstruct_boundary_arc(decode_identified_boundary_entity(old->second),
        intent.segment_id,intent.arc_construction);
    const auto actual=decode_identified_boundary_entity(current->second);
    if (actual.segments.size()!=requested.segments.size()) reject("Exterior arc result changed stable target topology");
    const auto tolerance=measured_arc_roundoff(boundary_geometry(requested));
    for (std::size_t i=0;i<requested.segments.size();++i) {
        const auto& target=requested.segments[i]; const auto& edge=actual.segments[i];
        if (edge.segment_id!=target.segment_id || edge.start_vertex_id!=target.start_vertex_id ||
            edge.end_vertex_id!=target.end_vertex_id)
            reject("Exterior arc result changed stable target edge or vertex identities");
        validate_measured_arc_edge(edge.segment,target.segment,tolerance);
    }
}

std::vector<BoundaryGeometryEdit> exterior_wall_measurement_source_updates(
    const std::map<std::string, Entity, std::less<>>& original,
    const std::map<std::string, Entity, std::less<>>& proposed,
    bool validate_final_constraints,
    const std::map<std::string,Vec2,std::less<>>& rigid_offsets) {
    for (const auto& [id,offset]:rigid_offsets) {
        const auto owner=original.find(id);
        if (owner==original.end() || owner->second.type!="measurement_boundary" ||
            !owner->second.properties.contains("wall_measurement_source") || !wall_measurement_source_current(original,owner->second) ||
            !bounded(offset.x) || !bounded(offset.y) || (offset.x==0 && offset.y==0))
            reject("Rigid exterior translation requires a current retained source and a bounded nonzero offset");
    }
    const auto same_segment = [](const Segment& a, const Segment& b) {
        return a.start.x == b.start.x && a.start.y == b.start.y &&
            a.end.x == b.end.x && a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
    };
    struct Alignment { std::size_t offset{}; bool reversed{}; unsigned matches{}; };
    const auto align = [&](const Boundary& actual, const Boundary& derived) {
        Alignment result;
        if (actual.size() != derived.size()) return result;
        for (std::size_t offset = 0; offset < derived.size(); ++offset)
            for (const bool reverse : {false, true}) {
                bool matched = true;
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    auto edge = derived[(offset + (reverse ? derived.size() - i : i)) % derived.size()];
                    if (reverse) { std::swap(edge.start, edge.end); edge.sweep_radians = -edge.sweep_radians; }
                    if (!same_segment(actual[i], edge)) { matched = false; break; }
                }
                if (matched) { ++result.matches; result.offset = offset; result.reversed = reverse; }
            }
        return result;
    };
    const auto elevation = [](const auto& entities, const Entity& wall) {
        const auto resolved = resolve_vertical_placement(entities, wall);
        auto value = resolved.properties.find("elevation_m");
        if (value == resolved.properties.end()) value = resolved.properties.find("elevation");
        return value == resolved.properties.end() ? 0.0 : number(*value, "Wall elevation");
    };
    std::map<std::string, BoundaryGeometryEdit, std::less<>> updates;
    for (const auto& [id, owner] : original) {
        // Generic and anonymous imported boundaries retain their existing
        // explicit-upgrade/source-repair contract.
        if (owner.type != "measurement_boundary" ||
            inspect_boundary_entity_version(owner).format != BoundaryEntityFormat::identified_v1 ||
            !owner.properties.contains("wall_measurement_source")) continue;
        std::vector<std::string> ids;
        try { ids = exterior_wall_measurement_source_ids(owner); }
        catch (const std::exception&) { continue; } // Independently stale original source schema.
        bool affected = false;
        for (const auto& wall_id : ids) {
            const auto before = original.find(wall_id), after = proposed.find(wall_id);
            if (before == original.end() || after == proposed.end() || before->second != after->second) {
                affected = true; break;
            }
        }
        if (!affected) continue;
        const auto identified = decode_identified_boundary_entity(owner);
        const auto actual = boundary_geometry(identified);
        std::optional<WallMeasurementResult> old;
        OffsetKernel original_kernel=OffsetKernel::stable;
        Alignment correspondence;
        for (const auto kernel : {OffsetKernel::stable, OffsetKernel::legacy_v1}) {
            std::optional<WallMeasurementResult> derived;
            try {
                derived = owner.properties.at("wall_measurement_source").at("version")==2
                    ? materialize_exterior_wall_measurement(owner) : derive_exterior_wall_measurement_impl(original, ids, kernel);
                if (owner.properties.at("wall_measurement_source").at("version")==2 &&
                    !wall_measurement_source_current(original,owner)) continue;
                if (normalize_source_order(owner.properties.at("wall_measurement_source")) != derived->source ||
                    !boundary_context_matches(owner, derived->source)) continue;
            } catch (const std::invalid_argument&) { continue; }
            const auto candidate = align(actual, derived->boundary);
            if (candidate.matches > 1) reject("Automatic exterior update has ambiguous original edge correspondence");
            if (candidate.matches == 1) { old = std::move(derived); correspondence = candidate; original_kernel=kernel; break; }
        }
        // Only the original snapshot determines this exemption. A current
        // owner cannot become stale as a result of an authored physical edit.
        if (!old) continue;
        const auto retained_owner = proposed.find(id);
        if (retained_owner == proposed.end() || retained_owner->second != owner)
            reject("Automatic exterior update overlaps an edited or removed measured owner");
        for (const auto& wall_id : ids) {
            const auto after = proposed.find(wall_id);
            if (after == proposed.end() || after->second.type != "wall")
                reject("Automatic exterior update requires every original source wall");
            if (std::abs(elevation(original, original.at(wall_id)) - elevation(proposed, after->second)) >
                default_geometry_tolerance_metres)
                reject("Automatic exterior update must retain the original source elevation plane");
        }
        const auto before_context = organize_project(original).drawing_context(id);
        const auto after_context = organize_project(proposed).drawing_context(id);
        if (!before_context || !after_context || !before_context->complete() || !after_context->complete() ||
            before_context->property_id != after_context->property_id ||
            before_context->building_id != after_context->building_id || before_context->floor_id != after_context->floor_id ||
            before_context->layer_id != after_context->layer_id)
            reject("Automatic exterior update must retain the measured owner's resolved hierarchy");
        std::optional<Json> translation_proof;
        if (const auto move=rigid_offsets.find(id);move!=rigid_offsets.end()) {
            translation_proof=Json{{"version",1},{"offset",{move->second.x,move->second.y}}};
            if (owner.properties.at("wall_measurement_source").at("version")==1) {
                auto records=Json::array();
                for (const auto& wall:read_source_walls(original,ids)) records.push_back({{"id",wall.id},{"context",wall.context},
                    {"baseline",segment_record(wall.baseline)},{"thickness_m",wall.thickness}});
                std::sort(records.begin(),records.end(),[](const auto& a,const auto& b) {
                    return a.at("id").template get<std::string>()<b.at("id").template get<std::string>(); });
                const auto kernel=original_kernel==OffsetKernel::stable ? "stable" : "legacy_v1";
                (*translation_proof)["genesis"]={{"kernel",kernel},{"walls",records},{"origin_outline",outline_record(actual)}};
            }
        }
        const auto replacement = translation_proof
            ? derive_translated_exterior_wall_measurement(proposed,owner,ids,*translation_proof)
            : derive_replacement_exterior_wall_measurement(proposed, owner, ids);
        if (replacement.boundary.size() != old->boundary.size() ||
            replacement.ordered_wall_ids.size() != replacement.boundary.size())
            reject("Automatic exterior update changed analytical topology");
        std::map<std::string, std::size_t, std::less<>> new_edges;
        for (std::size_t i = 0; i < replacement.ordered_wall_ids.size(); ++i)
            if (!new_edges.emplace(replacement.ordered_wall_ids[i], i).second)
                reject("Automatic exterior update has duplicate physical edge lineage");
        auto retained = identified;
        const auto count = retained.segments.size();
        const auto replacement_reversed=replacement_edge_reversed(correspondence.reversed,old->boundary,replacement.boundary);
        std::vector<std::size_t> mapped;
        for (std::size_t i = 0; i < count; ++i) {
            const auto old_index = (correspondence.offset + (correspondence.reversed ? count - i : i)) % count;
            const auto target = new_edges.find(old->ordered_wall_ids.at(old_index));
            if (target == new_edges.end()) reject("Automatic exterior update lost physical wall lineage");
            mapped.push_back(target->second);
            auto edge = replacement.boundary[target->second];
            if (replacement_reversed) { std::swap(edge.start, edge.end); edge.sweep_radians = -edge.sweep_radians; }
            retained.segments[i].segment = edge;
        }
        for (std::size_t i = 0; i < count; ++i)
            if (mapped[(i + 1) % count] != (mapped[i] + (replacement_reversed ? count - 1 : 1)) % count)
                reject("Automatic exterior update changed source-wall corner adjacency");
        if (retained == identified && normalize_source_order(owner.properties.at("wall_measurement_source")) == replacement.source)
            continue;
        BoundaryGeometryEdit edit;
        edit.boundary_id = edit.target_id = id;
        edit.kind = BoundaryGeometryEditKind::redefine_boundary;
        edit.replacement_segments = encode_identified_boundary_entity(retained).properties.at("segments");
        std::sort(ids.begin(), ids.end());
        edit.replacement_wall_source_ids = std::move(ids);
        edit.wall_source_translation=std::move(translation_proof);
        updates.emplace(id, std::move(edit));
    }
    // Children must be reconstructed before their updated parents so retained
    // deductions are validated against the complete new child geometry.
    for (const auto& [id,offset]:rigid_offsets)
        if (!updates.contains(id) || !updates.at(id).wall_source_translation)
            reject("Rigid exterior translation did not produce its required physical source completion");
    std::vector<BoundaryGeometryEdit> result;
    std::set<std::string> visiting, visited;
    const auto append = [&](const auto& self, const std::string& id) -> void {
        if (visited.contains(id)) return;
        if (!visiting.insert(id).second) reject("Automatic exterior deductions contain a cycle");
        const auto& owner = original.at(id);
        if (const auto children = owner.properties.find("deduction_ids"); children != owner.properties.end()) {
            if (!children->is_array()) reject("Measured deductions must be an array");
            for (const auto& child : *children) {
                if (!child.is_string()) reject("Measured deduction identifiers must be strings");
                const auto child_id = child.get<std::string>();
                if (updates.contains(child_id)) self(self, child_id);
            }
        }
        visiting.erase(id); visited.insert(id); result.push_back(updates.at(id));
    };
    for (const auto& [id, edit] : updates) { (void)edit; append(append, id); }
    auto completed = edited_boundary_entities_batch(proposed, result);
    if (const auto unsupported = validate_boundary_integrity(completed)) reject(*unsupported);
    if (validate_final_constraints)
        if (const auto unsupported = validate_constraint_integrity(completed)) reject(*unsupported);
    // Also validate unchanged parents of updated deductions against final state.
    for (const auto& [id, owner] : completed) {
        (void)id;
        const auto deductions = owner.properties.find("deduction_ids");
        if (deductions == owner.properties.end() || !deductions->is_array() ||
            !can_recognize_boundary_entity_type(owner.type)) continue;
        bool affected = false;
        std::vector<Boundary> holes;
        for (const auto& child : *deductions) {
            if (!child.is_string()) reject("Measured deduction identifiers must be strings");
            const auto child_id = child.get<std::string>();
            affected = affected || updates.contains(child_id);
            holes.push_back(actual_boundary_geometry(completed.at(child_id)));
        }
        if (affected)
            for (const auto& hole : holes)
                if (const auto diagnostic = validate_boundary_holes(actual_boundary_geometry(owner), {hole}))
                    reject("Updated deduction does not fit its retained parent: " + *diagnostic);
    }
    return result;
}

std::map<std::string,Entity,std::less<>> complete_wall_split_measurement_sources(
    const std::map<std::string,Entity,std::less<>>& original,
    const std::map<std::string,Entity,std::less<>>& physical,const WallSplitIntent& intent) {
    auto result=physical;
    std::set<std::string> consumed;
    const auto close=[](Vec2 a,Vec2 b){return std::hypot(a.x-b.x,a.y-b.y)<=default_geometry_tolerance_metres;};
    for(const auto& [id,owner]:original) {
        if(owner.type!="measurement_boundary" ||
            inspect_boundary_entity_version(owner).format!=BoundaryEntityFormat::identified_v1 ||
            !owner.properties.contains("wall_measurement_source"))continue;
        std::vector<std::string> ids;
        try{ids=exterior_wall_measurement_source_ids(owner);}catch(const std::invalid_argument&){continue;}
        if(std::find(ids.begin(),ids.end(),intent.wall_id)==ids.end() || !wall_measurement_source_current(original,owner))continue;
        const auto mapping=std::find_if(intent.measured_owners.begin(),intent.measured_owners.end(),
            [&](const auto& value){return value.boundary_id==id;});
        if(mapping==intent.measured_owners.end())reject("Wall split requires fresh analytical identities for measured owner: "+id);
        consumed.insert(id);
        if(original.contains(mapping->automatic_dimension_id))reject("Wall split dimension identity is already used: "+mapping->automatic_dimension_id);
        const auto identified=decode_identified_boundary_entity(owner);
        for(const auto& edge:identified.segments)
            if(edge.segment_id==mapping->segment_id || edge.start_vertex_id==mapping->vertex_id ||
                edge.end_vertex_id==mapping->vertex_id)reject("Wall split analytical identities must be fresh: "+id);
        const auto old=owner.properties.at("wall_measurement_source").at("version")==2
            ? materialize_exterior_wall_measurement(owner) : derive_exterior_wall_measurement(original,ids);
        const auto old_index=std::find(old.ordered_wall_ids.begin(),old.ordered_wall_ids.end(),intent.wall_id);
        if(old_index==old.ordered_wall_ids.end())reject("Wall split measured source lost old physical edge: "+id);
        const auto& old_geometry=old.boundary.at(static_cast<std::size_t>(old_index-old.ordered_wall_ids.begin()));
        const IdentifiedSegment* retained=nullptr;bool reversed=false;
        for(const auto& edge:identified.segments) {
            const bool forward=close(edge.segment.start,old_geometry.start) && close(edge.segment.end,old_geometry.end) &&
                std::abs(edge.segment.sweep_radians-old_geometry.sweep_radians)<=1e-9;
            const bool reverse=close(edge.segment.start,old_geometry.end) && close(edge.segment.end,old_geometry.start) &&
                std::abs(edge.segment.sweep_radians+old_geometry.sweep_radians)<=1e-9;
            if(!forward && !reverse)continue;
            if(retained)reject("Wall split analytical correspondence is ambiguous: "+id);
            retained=&edge;reversed=reverse;
        }
        if(!retained)reject("Wall split measured source has no analytical edge correspondence: "+id);
        ids.push_back(intent.second_wall_id);
        const auto replacement=derive_replacement_exterior_wall_measurement(result,owner,ids);
        const auto replacement_reversed=replacement_edge_reversed(reversed,old.boundary,replacement.boundary);
        if(replacement.boundary.size()!=identified.segments.size()+1 ||
            replacement.ordered_wall_ids.size()!=replacement.boundary.size())
            reject("Wall split measured source changed unrelated analytical topology: "+id);
        std::map<std::string,std::size_t,std::less<>> next;
        for(std::size_t i=0;i<replacement.ordered_wall_ids.size();++i)
            if(!next.emplace(replacement.ordered_wall_ids[i],i).second)reject("Wall split source has ambiguous physical lineage: "+id);
        // The physical split's first ID follows its stored baseline direction,
        // which may itself oppose the old source's analytical edge direction.
        const auto original_baseline=baseline(original.at(intent.wall_id).properties);
        const bool baseline_reversed=dot(subtract(retained->segment.end,retained->segment.start),
            subtract(original_baseline.end,original_baseline.start))<0;
        auto first=replacement.boundary.at(next.at(baseline_reversed ? intent.second_wall_id : intent.wall_id));
        auto second=replacement.boundary.at(next.at(baseline_reversed ? intent.wall_id : intent.second_wall_id));
        if(replacement_reversed){std::swap(first.start,first.end);first.sweep_radians=-first.sweep_radians;
            std::swap(second.start,second.end);second.sweep_radians=-second.sweep_radians;}
        if(!close(first.end,second.start) || !close(first.start,retained->segment.start) || !close(second.end,retained->segment.end))
            reject("Wall split offset children do not retain the original outer corners: "+id);
        // Offset mitres move the analytical edge's ends. Its seam ratio must
        // therefore come from the complete derived child lengths.
        BoundaryGeometryEdit insertion;insertion.boundary_id=id;insertion.target_id=retained->segment_id;
        insertion.kind=BoundaryGeometryEditKind::insert_vertex;
        insertion.fraction=segment_length(first)/(segment_length(first)+segment_length(second));
        insertion.new_vertex_id=mapping->vertex_id;insertion.new_segment_id=mapping->segment_id;
        insertion.new_dimension_id=mapping->automatic_dimension_id;
        // Insertion creates a temporary different edge count. The original v2
        // proof was validated above; do not attach it to this intermediate
        // outline. Its v1 context receipt is staged locally until the complete
        // source-derived redraw below replaces it atomically with final v1.
        if(owner.properties.at("wall_measurement_source").at("version")==2) {
            auto contexts=Json::array();
            for(const auto& record:owner.properties.at("wall_measurement_source").at("walls"))
                contexts.push_back({{"id",record.at("id")},{"context",record.at("context")}});
            result.at(id).properties["wall_measurement_source"]={{"version",1},{"basis","exterior"},{"walls",contexts}};
        }
        result=edited_boundary_entities(result,insertion);
        auto split=decode_identified_boundary_entity(result.at(id));
        const auto split_edge=std::find_if(split.segments.begin(),split.segments.end(),
            [&](const auto& edge){return edge.segment_id==retained->segment_id;});
        split_edge->segment=first;std::next(split_edge)->segment=second;
        // Reconstruct the unaffected edges by proven old physical lineage.
        for(auto& edge:split.segments) {
            if(edge.segment_id==retained->segment_id || edge.segment_id==mapping->segment_id)continue;
            const auto prior=std::find_if(identified.segments.begin(),identified.segments.end(),
                [&](const auto& value){return value.segment_id==edge.segment_id;});
            std::optional<std::size_t> lineage;
            for(std::size_t i=0;i<old.boundary.size();++i) {
                const auto& before=old.boundary[i];
                if((close(prior->segment.start,before.start)&&close(prior->segment.end,before.end)) ||
                    (close(prior->segment.start,before.end)&&close(prior->segment.end,before.start))) {
                    if(lineage)reject("Wall split neighbor lineage is ambiguous: "+id);lineage=i;
                }
            }
            if(!lineage)reject("Wall split lost neighboring measured edge: "+id);
            auto after=replacement.boundary.at(next.at(old.ordered_wall_ids.at(*lineage)));
            const auto old_reversed=close(prior->segment.start,old.boundary[*lineage].end);
            if(replacement_edge_reversed(old_reversed,old.boundary,replacement.boundary)) {
                std::swap(after.start,after.end);after.sweep_radians=-after.sweep_radians;
            }
            edge.segment=after;
        }
        BoundaryGeometryEdit redraw;redraw.boundary_id=redraw.target_id=id;
        redraw.kind=BoundaryGeometryEditKind::redefine_boundary;
        redraw.replacement_segments=encode_identified_boundary_entity(split).properties.at("segments");
        std::sort(ids.begin(),ids.end());redraw.replacement_wall_source_ids=ids;
        result=edited_boundary_entities(result,redraw);
        if(!wall_measurement_source_current(result,result.at(id)))reject("Wall split measured owner remains stale: "+id);
    }
    if(consumed.size()!=intent.measured_owners.size())reject("Wall split measured identity mapping includes an unaffected or stale owner");
    return result;
}

std::map<std::string,Entity,std::less<>> complete_wall_merge_measurement_sources(
    const std::map<std::string,Entity,std::less<>>& original,
    const std::map<std::string,Entity,std::less<>>& physical,const WallMergeIntent& intent) {
    if(const auto diagnostic=validate_boundary_integrity(original))reject(*diagnostic);
    auto result=physical;
    const auto close=[](Vec2 a,Vec2 b){return std::hypot(a.x-b.x,a.y-b.y)<=default_geometry_tolerance_metres;};
    const auto support_point=[](const Segment& segment,double fraction) {
        if(segment.sweep_radians==0)return Vec2{std::lerp(segment.start.x,segment.end.x,fraction),
            std::lerp(segment.start.y,segment.end.y,fraction)};
        const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y,chord=std::hypot(dx,dy);
        const auto angle=segment.sweep_radians*fraction,half_sine=std::sin(angle/2);
        const auto center_offset=std::abs(segment.sweep_radians)==std::numbers::pi?0.0:chord/(2*std::tan(segment.sweep_radians/2));
        const auto along=chord*half_sine*half_sine+center_offset*std::sin(angle);
        const auto normal=-chord*std::sin(angle)/2+center_offset*(2*half_sine*half_sine);
        return Vec2{std::fma(dx/chord,along,std::fma(-dy/chord,normal,segment.start.x)),
            std::fma(dy/chord,along,std::fma(dx/chord,normal,segment.start.y))};
    };
    const auto same=[&](const Segment& a,const Segment& b) {
        if((a.sweep_radians==0)!=(b.sweep_radians==0) ||
            (a.sweep_radians!=0 && std::signbit(a.sweep_radians)!=std::signbit(b.sweep_radians)) ||
            !close(a.start,b.start) || !close(a.end,b.end) ||
            std::max(segment_length(a),segment_length(b))*std::abs(a.sweep_radians-b.sweep_radians)>default_geometry_tolerance_metres)
            return false;
        for(const auto fraction:{0.25,0.5,0.75})if(!close(support_point(a,fraction),support_point(b,fraction)))return false;
        return true;
    };
    const auto flipped=[](Segment edge){std::swap(edge.start,edge.end);edge.sweep_radians=-edge.sweep_radians;return edge;};
    const auto dimension_position=[&](const Segment& segment,double side) {
        const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y;
        const auto midpoint=support_point(segment,0.5);
        const auto tangent=std::atan2(dy,dx);
        const auto offset=std::max(0.25,segment_length(segment)*0.1);
        return Vec2{midpoint.x-std::sin(tangent)*offset*side,midpoint.y+std::cos(tangent)*offset*side};
    };
    for(const auto& [id,owner]:original) {
        if(!owner.properties.contains("wall_measurement_source"))continue;
        auto ids=exterior_wall_measurement_source_ids(owner);
        const bool has_first=std::find(ids.begin(),ids.end(),intent.first_wall_id)!=ids.end();
        const bool has_second=std::find(ids.begin(),ids.end(),intent.second_wall_id)!=ids.end();
        if(!has_first&&!has_second)continue;
        if(!has_first || !has_second)reject("Wall merge measured owner must contain both source walls: "+id);
        if(owner.type!="measurement_boundary" || inspect_boundary_entity_version(owner).format!=BoundaryEntityFormat::identified_v1 ||
            !wall_measurement_source_current(original,owner))reject("Wall merge requires a current identified measured source: "+id);
        if(!result.contains(id) || result.at(id)!=owner)reject("Wall merge overlaps a separately edited measured owner: "+id);
        const auto identified=decode_identified_boundary_entity(owner);
        const auto old=owner.properties.at("wall_measurement_source").at("version")==2
            ? materialize_exterior_wall_measurement(owner) : derive_exterior_wall_measurement(original,ids);
        if(old.ordered_wall_ids.size()!=identified.segments.size())reject("Wall merge measured source has incomplete lineage: "+id);
        std::vector<std::string> lineage;std::vector<bool> reversed;
        for(const auto& edge:identified.segments) {
            std::optional<std::size_t> match;bool reverse=false;
            for(std::size_t i=0;i<old.boundary.size();++i) {
                if(!same(edge.segment,old.boundary[i]) && !same(edge.segment,flipped(old.boundary[i])))continue;
                if(match)reject("Wall merge measured edge lineage is ambiguous: "+id);
                match=i;reverse=!same(edge.segment,old.boundary[i]);
            }
            if(!match)reject("Wall merge lost an analytical source edge: "+id);
            lineage.push_back(old.ordered_wall_ids.at(*match));reversed.push_back(reverse);
        }
        const auto ai=std::find(lineage.begin(),lineage.end(),intent.first_wall_id),bi=std::find(lineage.begin(),lineage.end(),intent.second_wall_id);
        if(ai==lineage.end() || bi==lineage.end())reject("Wall merge sources lack exterior edge correspondence: "+id);
        const auto a=static_cast<std::size_t>(ai-lineage.begin()),b=static_cast<std::size_t>(bi-lineage.begin()),count=lineage.size();
        const auto incoming=(a+1)%count==b?a:b,outgoing=(a+1)%count==b?b:a;
        if((incoming+1)%count!=outgoing || reversed[a]!=reversed[b])reject("Wall merge measured sources are not directed neighbors: "+id);
        const auto& retained=identified.segments[incoming];const auto& retired=identified.segments[outgoing];
        if(retained.end_vertex_id!=retired.start_vertex_id)reject("Wall merge measured seam identity is inconsistent: "+id);
        const auto seam=retained.end_vertex_id;
        // Unknown live edge metadata can collapse only when both pieces agree.
        const auto edge_metadata=[](Json edge){for(const auto* key:{"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"})edge.erase(key);return edge;};
        if(edge_metadata(owner.properties.at("segments").at(incoming))!=edge_metadata(owner.properties.at("segments").at(outgoing)))
            reject("Wall merge measured edge metadata conflicts: "+id);
        ids.erase(std::remove(ids.begin(),ids.end(),intent.second_wall_id),ids.end());std::sort(ids.begin(),ids.end());
        const auto replacement=derive_replacement_exterior_wall_measurement(result,owner,ids);
        if(replacement.boundary.size()+1!=count || replacement.ordered_wall_ids.size()!=replacement.boundary.size())
            reject("Wall merge changed unrelated exterior topology: "+id);
        std::map<std::string,std::size_t,std::less<>> next;
        for(std::size_t i=0;i<replacement.ordered_wall_ids.size();++i)
            if(!next.emplace(replacement.ordered_wall_ids[i],i).second)reject("Wall merge exterior has ambiguous physical lineage: "+id);
        auto merged=remove_boundary_vertex(identified,seam);
        for(auto& edge:merged.segments) {
            const auto prior=std::find_if(identified.segments.begin(),identified.segments.end(),[&](const auto& value){return value.segment_id==edge.segment_id;});
            const auto index=static_cast<std::size_t>(prior-identified.segments.begin());
            auto geometry=replacement.boundary.at(next.at(index==incoming?intent.first_wall_id:lineage.at(index)));
            if(replacement_edge_reversed(reversed.at(index),old.boundary,replacement.boundary))geometry=flipped(geometry);
            if(!same(edge.segment,geometry))
                reject("Wall merge exterior does not preserve original outer corners and sweep: "+id);
            edge.segment=geometry;
        }
        auto metadata=owner;
        if(!metadata.extensions.contains("boundary_geometry_derivation")) {
            if(metadata.properties.contains("boundary_authoring")) {
                metadata.extensions["boundary_geometry_derivation"]={{"version",1},
                    {"source_boundary_authoring",metadata.properties.at("boundary_authoring")},{"operations",Json::array()}};
                metadata.properties.erase("boundary_authoring");
            } else metadata.extensions["boundary_geometry_derivation"]={{"version",2},
                {"source_boundary",{{"boundary_model_version",1},{"segments",owner.properties.at("segments")}}},{"operations",Json::array()}};
        }
        const auto pure=encode_identified_boundary_entity(merged).properties.at("segments");
        metadata.extensions["boundary_geometry_derivation"]["operations"].push_back({{"kind","wall_merge"},
            {"value",{{"version",1},{"vertex_id",seam},{"segments",pure},{"wall_source_ids",ids},{"removed_wall_id",intent.second_wall_id}}}});
        metadata.properties["wall_measurement_source"]=replacement.source;
        auto encoded=encode_identified_boundary_entity(merged,&metadata);
        if(encoded.properties.contains("boundary")) {
            auto geometry=Json::array();for(const auto& edge:merged.segments)geometry.push_back(segment_record(edge.segment));
            encoded.properties["boundary"]=std::move(geometry);
        }
        result.at(id)=std::move(encoded);
        std::set<std::string> erased_dimensions;
        std::map<std::string,std::string,std::less<>> automatic;
        const auto dimension_metadata=[](const Entity& entity) {
            auto properties=entity.properties;properties.erase("text_position");
            for(const auto* key:{"entity_id","segment_id","segment_ids"})properties.at("target").erase(key);
            return Json{{"type",entity.type},{"required",entity.required},{"properties",properties},{"extensions",entity.extensions}};
        };
        for(const auto& [dimension_id,entity]:original)if(can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);if(!decoded.supported())continue;
            const auto& dimension=*decoded.dimension;
            if(dimension.boundary_id==id && dimension.kind==BoundaryDimensionKind::segment_length &&
                dimension.placement==BoundaryDimensionPlacement::automatic && dimension.segment_chain_ids.empty()) {
                if(!automatic.emplace(dimension.segment_id,dimension_id).second)
                    reject("Wall merge has duplicate automatic edge dimensions: "+id);
            }
        }
        const auto retained_dimension=automatic.find(retained.segment_id),retired_dimension=automatic.find(retired.segment_id);
        if(retired_dimension!=automatic.end() && retained_dimension!=automatic.end()) {
            if(dimension_metadata(original.at(retained_dimension->second))!=dimension_metadata(original.at(retired_dimension->second)))
                reject("Wall merge automatic dimension styles or metadata conflict: "+id);
            erased_dimensions.insert(retired_dimension->second);
        }
        const auto collapse_chain=[&](std::vector<std::string> chain,const std::string& dependent) {
            const auto first=std::find(chain.begin(),chain.end(),retained.segment_id),second=std::find(chain.begin(),chain.end(),retired.segment_id);
            if(first==chain.end() && second==chain.end())return chain;
            if(first==chain.end() || second==chain.end() || std::next(first)!=second)
                reject("Wall merge cannot preserve individual measured length target: "+dependent);
            chain.erase(second);return chain;
        };
        for(auto& [dependent,entity]:result) {
            if(entity.type=="constraint") {
                const auto decoded=decode_constraint_entity(entity);if(!decoded.supported())continue;
                if(!std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                    [&](const auto& binding){return binding.owner_id==id;}))continue;
                auto relation=*decoded.constraint;auto raw=entity.properties.at("bindings");
                std::vector<WallEndpointBinding> bindings;Json persisted=Json::array();
                const auto on_pair=[&](const WallEndpointBinding& binding){return binding.owner_id==id &&
                    (binding.segment_id==retained.segment_id || binding.segment_id==retired.segment_id);};
                for(std::size_t i=0;i<relation.bindings.size();) {
                    auto binding=relation.bindings[i];
                    if(relation.relation==ConstraintRelationKind::tangent && on_pair(binding)) {
                        if(i%2!=0 || i+1>=relation.bindings.size() || relation.bindings[i+1].owner_id!=id ||
                            relation.bindings[i+1].segment_id!=binding.segment_id || relation.bindings[i+1].role==binding.role ||
                            binding.vertex_id==seam)reject("Wall merge cannot preserve a measured seam tangent contact: "+dependent);
                        const auto& target=*std::find_if(merged.segments.begin(),merged.segments.end(),
                            [&](const auto& edge){return edge.segment_id==retained.segment_id;});
                        for(std::size_t j=0;j<2;++j) {
                            auto part=relation.bindings[i+j];auto part_json=raw.at(i+j);
                            part.segment_id=retained.segment_id;
                            part.vertex_id=part.role==WallEndpointRole::start?target.start_vertex_id:target.end_vertex_id;
                            part_json["segment_id"]=part.segment_id;part_json["vertex_id"]=part.vertex_id;
                            bindings.push_back(part);persisted.push_back(part_json);
                        }
                        i+=2;continue;
                    }
                    if(relation.relation==ConstraintRelationKind::fixed_arc_length && on_pair(binding)) {
                        if(i+3>=relation.bindings.size())reject("Wall merge cannot preserve individual measured arc lock: "+dependent);
                        const auto c=relation.bindings[i+1],d=relation.bindings[i+2],e=relation.bindings[i+3];
                        const bool forward=binding.segment_id==retained.segment_id && binding.role==WallEndpointRole::start;
                        const bool reverse=binding.segment_id==retired.segment_id && binding.role==WallEndpointRole::end;
                        if((!forward&&!reverse) || c.owner_id!=id || d.owner_id!=id || e.owner_id!=id ||
                            c.segment_id!=binding.segment_id || c.role==binding.role || d.role!=binding.role ||
                            d.segment_id!=(forward?retired.segment_id:retained.segment_id) || e.segment_id!=d.segment_id || e.role==d.role)
                            reject("Wall merge requires a complete directed measured arc chain: "+dependent);
                        // Split duplicates endpoint metadata. Collapse only that
                        // exact duplication, preserving both authored outer records.
                        const auto without_endpoint=[](Json value){value.erase("segment_id");value.erase("vertex_id");return value;};
                        if(without_endpoint(raw.at(i))!=without_endpoint(raw.at(i+2)) ||
                            without_endpoint(raw.at(i+1))!=without_endpoint(raw.at(i+3)))
                            reject("Wall merge measured arc seam binding metadata conflicts: "+dependent);
                        auto start=binding,end=e;start.segment_id=end.segment_id=retained.segment_id;
                        auto sj=raw.at(i),ej=raw.at(i+3);sj["segment_id"]=ej["segment_id"]=retained.segment_id;
                        bindings.push_back(start);bindings.push_back(end);persisted.push_back(sj);persisted.push_back(ej);i+=4;continue;
                    }
                    if(binding.owner_id==id && binding.vertex_id==seam)
                        reject("Wall merge is blocked by a pinned measured seam constraint: "+dependent);
                    auto persisted_binding=raw.at(i);
                    if(binding.owner_id==id && binding.segment_id==retired.segment_id) {
                        binding.segment_id=retained.segment_id;persisted_binding["segment_id"]=retained.segment_id;
                    }
                    if(on_pair(binding)) {
                        const auto& target=*std::find_if(merged.segments.begin(),merged.segments.end(),[&](const auto& edge){return edge.segment_id==retained.segment_id;});
                        binding.role=binding.vertex_id==target.start_vertex_id?WallEndpointRole::start:WallEndpointRole::end;
                        persisted_binding["role"]=binding.role==WallEndpointRole::start?"start":"end";
                    }
                    bindings.push_back(binding);persisted.push_back(persisted_binding);++i;
                }
                relation.bindings=std::move(bindings);
                auto updated=encode_constraint_entity(relation,&entity);updated.properties["bindings"]=std::move(persisted);entity=std::move(updated);
            }
            if(!can_recognize_boundary_dimension_entity_type(entity.type) || erased_dimensions.contains(dependent))continue;
            const auto decoded=decode_boundary_dimension_entity(entity);if(!decoded.supported())continue;
            auto dimension=*decoded.dimension;if(dimension.boundary_id!=id || dimension.kind==BoundaryDimensionKind::area)continue;
            if(dimension.kind==BoundaryDimensionKind::angle) {
                if(dimension.vertex_id==seam)reject("Wall merge is blocked by a measured seam angle dimension: "+dependent);
                if(dimension.segment_id==retired.segment_id)dimension.segment_id=retained.segment_id;
                if(dimension.secondary_segment_id==retired.segment_id)dimension.secondary_segment_id=retained.segment_id;
            } else if(!dimension.segment_chain_ids.empty()) {
                auto chain=collapse_chain(dimension.segment_chain_ids,dependent);dimension.segment_id=chain.front();
                dimension.segment_chain_ids=chain.size()==1?std::vector<std::string>{}:std::move(chain);
            } else if(dimension.segment_id==retained.segment_id || dimension.segment_id==retired.segment_id) {
                if(dimension.placement!=BoundaryDimensionPlacement::automatic)
                    reject("Wall merge cannot reinterpret an individual manual edge dimension: "+dependent);
                dimension.segment_id=retained.segment_id;
            }
            if(dimension.placement==BoundaryDimensionPlacement::automatic && dimension.kind==BoundaryDimensionKind::segment_length) {
                const auto side=dimension.automatic_placement_version.value_or(1)==1?1.0:(signed_area(boundary_geometry(merged))>0?-1.0:1.0);
                dimension.text_position=dimension_position(dimension.resolve(result).segment,side);
            }
            entity=encode_boundary_dimension_entity(dimension,&entity);
        }
        for(const auto& erased:erased_dimensions)result.erase(erased);
        // Do not reinterpret opaque dependent child references. Historical
        // construction/derivation records and the boundary's old child payload
        // are receipts; all other canonical reference locations are checked.
        const auto check_refs=[&](const auto& self,const Json& value,const std::string& dependent,const std::string& path)->void {
            static const std::set<std::string,std::less<>> keys={"segment_id","segment_ids","second_segment_id","vertex_id","vertex_ids",
                "start_vertex_id","end_vertex_id","entity_id","entity_ids","object_id","object_ids","target_id","target_ids",
                "wall_id","wall_ids","host_id","host_ids","source_id","source_ids","source_entity_id","source_entity_ids",
                "parent_id","parent_ids","owner_id","host_entity_id","host_entity_ids","wall_members",
                "wall_join_id","wall_join_ids","join_id","join_ids","constraint_id","constraint_ids","seam_constraint_id",
                "dimension_id","dimension_ids","refs","references"};
            const auto mentions=[&](const auto& recurse,const Json& child,const std::string& target)->bool {
                if(child.is_string())return child==target;
                if(child.is_array())for(const auto& item:child)if(recurse(recurse,item,target))return true;
                return false;
            };
            if(value.is_object())for(const auto& [key,child]:value.items()) {
                if(keys.contains(key)) {
                    const bool segment_reference=key=="segment_id" || key=="segment_ids" || key=="second_segment_id";
                    const bool vertex_reference=key=="vertex_id" || key=="vertex_ids" || key=="start_vertex_id" || key=="end_vertex_id";
                    const bool generic_reference=key=="refs" || key=="references";
                    bool retired_reference=((segment_reference||generic_reference)&&mentions(mentions,child,retired.segment_id)) ||
                        ((vertex_reference||generic_reference)&&mentions(mentions,child,seam));
                    if(!segment_reference&&!vertex_reference)for(const auto& erased:erased_dimensions)
                        retired_reference=retired_reference||mentions(mentions,child,erased);
                    if(retired_reference)reject("Wall merge has unsupported retired-child reference in "+dependent+" at "+path+"/"+key);
                }
                self(self,child,dependent,path+"/"+key);
            } else if(value.is_array())for(std::size_t i=0;i<value.size();++i)self(self,value[i],dependent,path+"/"+std::to_string(i));
        };
        for(const auto& [dependent,entity]:result) {
            auto unchecked=entity;
            if(entity.type=="wall" &&
                (entity.extensions.contains("wall_merge_archive") || entity.extensions.contains("wall_split_archive"))) {
                Wall wall;std::string error;
                if(!read_document_wall(entity,{},wall,error))reject("Wall merge historical wall is invalid: "+dependent+": "+error);
                validate_wall_semantics(wall);validate_wall_split_archive(entity);validate_wall_merge_archive(entity);
                unchecked.extensions.erase("wall_merge_archive");unchecked.extensions.erase("wall_split_archive");
            }
            if(can_recognize_boundary_entity_type(entity.type) &&
                inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1 &&
                (entity.extensions.contains("boundary_geometry_derivation") || entity.properties.contains("boundary_authoring"))) {
                // Only strict owner/operation replay admits historical child
                // inventories; same-named opaque fields remain live references.
                if(const auto diagnostic=validate_boundary_integrity({{dependent,entity}}))reject(*diagnostic);
                unchecked.extensions.erase("boundary_geometry_derivation");unchecked.properties.erase("boundary_authoring");
            }
            check_refs(check_refs,unchecked.properties,dependent,"/properties");check_refs(check_refs,unchecked.extensions,dependent,"/extensions");
        }
        if(!wall_measurement_source_current(result,result.at(id)))reject("Wall merge measured owner remains stale: "+id);
    }
    if(const auto diagnostic=validate_boundary_integrity(result))reject(*diagnostic);
    return result;
}

} // namespace sketch
