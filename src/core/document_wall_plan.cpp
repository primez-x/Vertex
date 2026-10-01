#include "sketch/document_wall_plan.hpp"

#include "sketch/document_wall.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;

bool same_context(const Entity& first, const Entity& second) {
    constexpr std::array fields{"property_id", "building_id", "floor_id", "layer_id", "phase_id"};
    for (const auto* field : fields) {
        const auto a = first.properties.find(field), b = second.properties.find(field);
        if ((a == first.properties.end()) != (b == second.properties.end())) return false;
        if (a != first.properties.end() && *a != *b) return false;
    }
    return true;
}

bool material_at_endpoint(const Wall& wall, bool start) {
    const auto length = segment_length(wall.baseline);
    for (const auto& opening : wall.openings) {
        if (start && opening.offset <= tolerance) return false;
        if (!start && opening.offset + opening.width >= length - tolerance) return false;
    }
    return true;
}
}

std::map<std::string, WallPlanGeometry, std::less<>> document_wall_plan_geometry(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::map<std::string, Wall, std::less<>>& overrides) {
    std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
    for (const auto& [id, entity] : entities) {
        (void)id;
        if (entity.type != "opening") continue;
        std::string host, error;
        if (read_document_wall_id(entity, host, error)) openings[host].push_back(&entity);
    }
    std::map<std::string, Wall, std::less<>> walls;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "wall") continue;
        try {
            Wall wall;
            std::string error;
            if (const auto replacement = overrides.find(id); replacement != overrides.end())
                wall = replacement->second;
            else if (!read_document_wall(entity, openings[id], wall, error)) continue;
            validate_wall_semantics(wall);
            walls.emplace(id, std::move(wall));
        } catch (const std::exception&) {
            // Existing document projection reports malformed semantic sources.
        }
    }
    struct Endpoint { const Wall* wall; bool start; Vec2 position; };
    std::multimap<double, Endpoint> endpoints;
    for (const auto& [id, wall] : walls) {
        (void)id;
        endpoints.emplace(wall.baseline.start.x, Endpoint{&wall, true, wall.baseline.start});
        endpoints.emplace(wall.baseline.end.x, Endpoint{&wall, false, wall.baseline.end});
    }
    struct Connection { WallPlanJunction junction; std::string neighbor; bool neighbor_start; };
    std::map<std::string, std::vector<Connection>, std::less<>> connections;
    for (const auto& [id, wall] : walls) {
        for (const bool start : {true, false}) {
            if (!material_at_endpoint(wall, start)) continue;
            const auto point = start ? wall.baseline.start : wall.baseline.end;
            const Wall* neighbor = nullptr;
            bool neighbor_start = false;
            bool ambiguous = false;
            for (auto it = endpoints.lower_bound(point.x - tolerance);
                 it != endpoints.end() && it->first <= point.x + tolerance; ++it) {
                const auto& endpoint = it->second;
                if (endpoint.wall->id == id ||
                    !material_at_endpoint(*endpoint.wall, endpoint.start) ||
                    std::hypot(endpoint.position.x - point.x, endpoint.position.y - point.y) > tolerance ||
                    !same_context(entities.at(id), entities.at(endpoint.wall->id)) ||
                    std::abs(wall.elevation - endpoint.wall->elevation) > tolerance) continue;
                if (neighbor) { ambiguous = true; break; }
                neighbor = endpoint.wall;
                neighbor_start = endpoint.start;
            }
            if (neighbor && !ambiguous)
                connections[id].push_back({{start, neighbor->baseline, neighbor->thickness},
                                            neighbor->id, neighbor_start});
        }
    }
    std::map<std::string, WallPlanGeometry, std::less<>> result;
    const auto regenerate = [&] {
        result.clear();
        for (const auto& [id, wall] : walls) {
            std::vector<WallPlanJunction> junctions;
            for (const auto& connection : connections[id]) junctions.push_back(connection.junction);
            result.emplace(id, joined_wall_plan_geometry(wall.baseline, wall.openings,
                                                         wall.thickness, junctions));
        }
    };
    regenerate();
    // A short run or an unstable acute corner can reject a local miter. Keep
    // the partner capped too; otherwise the accepted half leaves a false gap.
    // Each iteration only removes connections, so the process is bounded by
    // the original endpoint count and cannot oscillate.
    for (;;) {
        bool removed = false;
        const auto joined = [](const WallPlanGeometry& plan, bool start) {
            return start ? plan.joined_start : plan.joined_end;
        };
        for (auto& [id, links] : connections) {
            std::erase_if(links, [&](const auto& connection) {
                const bool valid = joined(result.at(id), connection.junction.at_start) &&
                    joined(result.at(connection.neighbor), connection.neighbor_start);
                removed = removed || !valid;
                return !valid;
            });
        }
        if (!removed) break;
        regenerate();
    }
    return result;
}
} // namespace sketch
