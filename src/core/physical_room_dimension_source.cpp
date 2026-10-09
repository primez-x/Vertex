#include "sketch/physical_room_dimension_source.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Physical room dimension: " + reason);
}
bool same_boundary(const Boundary& a, const Boundary& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const Segment& x, const Segment& y) {
            return x.start.x == y.start.x && x.start.y == y.start.y &&
                   x.end.x == y.end.x && x.end.y == y.end.y &&
                   x.sweep_radians == y.sweep_radians;
        });
}
double elevation(const Entity& entity) {
    const auto value = entity.properties.find("elevation_m");
    if (value == entity.properties.end() || !value->is_number())
        reject("effective source plane is unresolved");
    const auto result = value->get<double>();
    if (!std::isfinite(result)) reject("effective source plane is not finite");
    return result;
}
bool same_physical_inventory(const nlohmann::json& retained, const nlohmann::json& fresh) {
    if (!retained.is_object() || !fresh.is_object()) return false;
    auto retained_inventory = retained;
    auto fresh_inventory = fresh;
    retained_inventory.erase("semantic_phases");
    fresh_inventory.erase("semantic_phases");
    return retained_inventory == fresh_inventory;
}

PhysicalRoomDimensionSource resolve_source(
    const Entity& room, const std::map<std::string, Entity, std::less<>>& entities,
    bool allow_current_phase_bookkeeping) {
    try {
        if (!is_physical_wall_room(room)) reject("owner is not a source-bound physical room");
        const auto owner = entities.find(room.id);
        if (owner == entities.end() || !(owner->second == room))
            reject("owner does not match the authoritative source map");
        for (const auto& [id, entity] : entities) {
            (void)id;
            if (entity.type != "model_phases") continue;
            const auto phases = ModelPhases::from_json(entity.properties.at("model"));
            if (std::find(phases.entity_ids().begin(), phases.entity_ids().end(), room.id) == phases.entity_ids().end()) continue;
            const auto active = phases.active_state();
            const auto state = active.find(room.id);
            if (state == active.end() || state->second == ModelPhase::demolished)
                reject("room owner is inactive in the semantic phase");
        }
        const auto descriptor = decode_physical_wall_room_descriptor(room);
        const auto detection = detect_physical_wall_spaces(entities, descriptor.selected_wall_id);
        const auto context = organize_project(entities).drawing_context(room.id);
        if (!context || !context->complete() || *context != detection.context)
            reject("room drawing context differs from its current physical walls");
        const PhysicalWallSpace* current = nullptr;
        for (const auto& space : detection.spaces) {
            // Exact fresh evidence admission also rejects unknown, malformed or
            // incomplete retained lineage without a second permissive parser.
            if (space.source_lineage != descriptor.source_lineage) continue;
            if (current) reject("source lineage matches multiple physical clear components");
            current = &space;
        }
        if (!current && allow_current_phase_bookkeeping) {
            for (const auto& space : detection.spaces) {
                // This comparison only bounds candidate work. The shared
                // current-room predicate independently admits both complete
                // captured lineages and their analytical clear regions.
                if (!same_physical_inventory(descriptor.source_lineage, space.source_lineage) ||
                    !physical_wall_room_lineage_matches_current_inventory(room, *context, space)) continue;
                if (current) reject("source inventory matches multiple physical clear components");
                current = &space;
            }
        }
        if (!current) reject("physical wall source evidence or effective plane changed; repair the room explicitly");
        auto identified = decode_identified_boundary_entity(room);
        if (!same_boundary(boundary_geometry(identified), current->boundary))
            reject("identified room outer differs from current physical clear geometry");
        if (descriptor.holes.size() != current->holes.size() ||
            !std::equal(descriptor.holes.begin(), descriptor.holes.end(), current->holes.begin(), same_boundary))
            reject("room holes differ from current physical clear geometry");
        // Most room boundaries inherit the plane from their captured physical
        // sources. An explicit placement must still agree with that plane.
        if (room.properties.contains("elevation_m") || room.properties.contains("vertical_placement")) {
            const auto placed_room = resolve_vertical_placement(entities, room);
            const auto placed_wall = resolve_vertical_placement(entities, entities.at(descriptor.selected_wall_id));
            if (std::abs(elevation(placed_room) - elevation(placed_wall)) > default_geometry_tolerance_metres)
                reject("explicit room plane differs from its current physical walls");
        }
        if (const auto error = validate_boundary_holes(current->boundary, current->holes)) reject(*error);
        double area = std::abs(signed_area(current->boundary));
        for (const auto& hole : current->holes) area -= std::abs(signed_area(hole));
        if (!std::isfinite(area) || area <= default_geometry_tolerance_metres * default_geometry_tolerance_metres)
            reject("current physical clear room has no measurable net area");
        return {std::move(identified), current->holes, area};
    } catch (const nlohmann::json::exception& error) {
        reject(std::string("malformed source evidence: ") + error.what());
    }
}
}

PhysicalRoomDimensionSource resolve_physical_room_dimension_source(
    const Entity& room, const std::map<std::string, Entity, std::less<>>& entities) {
    return resolve_source(room, entities, false);
}

PhysicalRoomDimensionSource resolve_current_physical_room_dimension_source(
    const Entity& room, const DocumentSnapshot& snapshot) {
    return resolve_source(room, snapshot.entities(), true);
}
} // namespace sketch
