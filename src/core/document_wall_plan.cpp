#include "sketch/document_wall_plan.hpp"

#include "sketch/document_wall.hpp"
#include "sketch/wall_plan_junctions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

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
    std::set<std::pair<std::string, std::string>> material_contacts;
    const auto add_contact = [&](std::string first, std::string second) {
        if (first == second || !same_context(entities.at(first), entities.at(second)) ||
            std::abs(walls.at(first).elevation - walls.at(second).elevation) > tolerance)
            return false;
        if (second < first) std::swap(first, second);
        return material_contacts.emplace(std::move(first), std::move(second)).second;
    };
    for (const auto& [id, wall] : walls) {
        for (const bool start : {true, false}) {
            if (!material_at_endpoint(wall, start)) continue;
            const auto point = start ? wall.baseline.start : wall.baseline.end;
            const Wall* neighbor = nullptr;
            bool neighbor_start = false;
            bool ambiguous = false;
            std::vector<const Wall*> straight_neighbors;
            for (auto it = endpoints.lower_bound(point.x - tolerance);
                 it != endpoints.end() && it->first <= point.x + tolerance; ++it) {
                const auto& endpoint = it->second;
                if (endpoint.wall->id == id ||
                    !material_at_endpoint(*endpoint.wall, endpoint.start) ||
                    std::hypot(endpoint.position.x - point.x, endpoint.position.y - point.y) > tolerance ||
                    !same_context(entities.at(id), entities.at(endpoint.wall->id)) ||
                    std::abs(wall.elevation - endpoint.wall->elevation) > tolerance) continue;
                if (endpoint.wall->baseline.sweep_radians == 0.0)
                    straight_neighbors.push_back(endpoint.wall);
                if (neighbor) ambiguous = true;
                else {
                    neighbor = endpoint.wall;
                    neighbor_start = endpoint.start;
                }
            }
            if (neighbor && !ambiguous)
                connections[id].push_back({{start, neighbor->baseline, neighbor->thickness},
                                            neighbor->id, neighbor_start});
            if (wall.baseline.sweep_radians == 0.0 && straight_neighbors.size() >= 2) {
                for (const auto* member : straight_neighbors) add_contact(id, member->id);
                for (std::size_t first = 0; first < straight_neighbors.size(); ++first)
                    for (std::size_t second = first + 1; second < straight_neighbors.size(); ++second)
                        add_contact(straight_neighbors[first]->id, straight_neighbors[second]->id);
            }
        }
    }
    // Sweep baseline bounds before exact contact tests. Nearby but separate
    // walls, different levels/contexts and ordinary two-endpoint corners do
    // not enter the material union stroke pass.
    struct IndexedWall { const Wall* wall; Bounds2 bounds; };
    std::vector<IndexedWall> indexed;
    for (const auto& [id, wall] : walls) {
        (void)id;
        if (wall.baseline.sweep_radians == 0.0)
            indexed.push_back({&wall, segment_bounds(wall.baseline)});
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& first, const auto& second) {
        if (first.bounds.minimum.x != second.bounds.minimum.x)
            return first.bounds.minimum.x < second.bounds.minimum.x;
        return first.wall->id < second.wall->id;
    });
    for (std::size_t first = 0; first < indexed.size(); ++first) {
        const auto& a = indexed[first];
        for (std::size_t second = first + 1; second < indexed.size(); ++second) {
            const auto& b = indexed[second];
            if (b.bounds.minimum.x > a.bounds.maximum.x + tolerance) break;
            if (b.bounds.minimum.y > a.bounds.maximum.y + tolerance ||
                b.bounds.maximum.y < a.bounds.minimum.y - tolerance ||
                !same_context(entities.at(a.wall->id), entities.at(b.wall->id)) ||
                std::abs(a.wall->elevation - b.wall->elevation) > tolerance) continue;
            if (wall_baselines_have_interior_contact(a.wall->baseline, b.wall->baseline))
                add_contact(a.wall->id, b.wall->id);
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
    // A nearby partition can overlap both members of an admitted miter even
    // when it touches only one source baseline. Include only overlapping
    // corner partners reached from an already qualified material contact.
    std::map<std::string, Bounds2, std::less<>> material_bounds;
    for (const auto& [id, plan] : result)
        if (!plan.footprint.empty()) material_bounds.emplace(id, boundary_bounds(plan.footprint));
    const auto overlaps_material_bounds = [&](const std::string& first, const std::string& second) {
        const auto a = material_bounds.find(first), b = material_bounds.find(second);
        if (a == material_bounds.end() || b == material_bounds.end()) return false;
        return a->second.minimum.x <= b->second.maximum.x + tolerance &&
            b->second.minimum.x <= a->second.maximum.x + tolerance &&
            a->second.minimum.y <= b->second.maximum.y + tolerance &&
            b->second.minimum.y <= a->second.maximum.y + tolerance;
    };
    std::vector<std::pair<std::string, std::string>> pending_contacts(
        material_contacts.begin(), material_contacts.end());
    for (std::size_t index = 0; index < pending_contacts.size(); ++index) {
        const auto [first, second] = pending_contacts[index];
        const auto include_partners = [&](const std::string& owner, const std::string& other) {
            for (const auto& connection : connections[owner]) {
                if (connection.neighbor != other &&
                    overlaps_material_bounds(connection.neighbor, other) &&
                    add_contact(connection.neighbor, other))
                    pending_contacts.emplace_back(connection.neighbor, other);
            }
        };
        include_partners(first, second);
        include_partners(second, first);
    }
    apply_wall_plan_junction_strokes(result, walls,
        {material_contacts.begin(), material_contacts.end()});
    return result;
}
} // namespace sketch
