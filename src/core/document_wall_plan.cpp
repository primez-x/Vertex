#include "sketch/document_wall_plan.hpp"

#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/wall_plan_network.hpp"

#include <algorithm>
#include <array>

namespace sketch {
namespace {
nlohmann::json contact_domain(const Entity& entity) {
    constexpr std::array fields{"property_id", "building_id", "floor_id", "layer_id", "phase_id"};
    auto result = nlohmann::json::array();
    for (const auto* field : fields) {
        const auto value = entity.properties.find(field);
        // Presence is distinct from a present null. Keep the JSON value rather
        // than its dump string so numeric equality matches the prior predicate.
        result.push_back(nlohmann::json::array({value != entity.properties.end(),
            value == entity.properties.end() ? nlohmann::json(nullptr) : *value}));
    }
    return result;
}
} // namespace

std::map<std::string, WallPlanGeometry, std::less<>> document_wall_plan_geometry(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::map<std::string, Wall, std::less<>>& overrides) {
    const auto scope = constraint_phase_scope(entities);
    std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "opening" || scope.inactive_owner_ids.contains(id)) continue;
        std::string host, error;
        if (read_document_wall_id(entity, host, error)) openings[host].push_back(&entity);
    }
    std::map<std::string, Wall, std::less<>> walls;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "wall" || scope.inactive_owner_ids.contains(id)) continue;
        try {
            Wall wall;
            std::string error;
            if (const auto replacement = overrides.find(id); replacement != overrides.end()) {
                wall = replacement->second;
                if (wall.id != id) continue;
                // Overrides retain candidate dimensions, but their retained
                // inactive cuts cannot become active through a derived copy.
                std::erase_if(wall.openings, [&](const auto& opening) {
                    return scope.inactive_owner_ids.contains(opening.id);
                });
                std::erase_if(wall.pocket_recesses, [&](const auto& recess) {
                    return scope.inactive_owner_ids.contains(recess.opening_id);
                });
            } else if (!read_document_wall(entity, openings[id], wall, error)) continue;
            validate_wall_semantics(wall);
            walls.emplace(id, std::move(wall));
        } catch (const std::exception&) {
            // Existing document projection reports malformed semantic sources.
        }
    }
    std::map<std::string, nlohmann::json, std::less<>> contact_domains;
    for (const auto& [id, wall] : walls) {
        (void)wall;
        contact_domains.emplace(id, contact_domain(entities.at(id)));
    }
    return wall_plan_network_geometry(walls, contact_domains);
}
} // namespace sketch
