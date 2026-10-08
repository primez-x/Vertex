#include "sketch/physical_wall_phase.hpp"
#include "sketch/physical_wall_room_data.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Physical wall phase: " + reason);
}
void check_identity(const std::string& id, const Entity& entity) {
    if (id.empty() || std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }) ||
        entity.id != id)
        reject("entity key must resolve to its actual nonblank identity: " + id);
}
} // namespace

std::vector<PhysicalWallPhaseState> physical_wall_phase_states(
    const std::map<std::string, Entity, std::less<>>& entities,
    const PhysicalWallPhaseSelection& selection) {
    try {
        const auto selected = entities.find(selection.registry_id);
        if (selected == entities.end() || selected->second.type != "model_phases")
            reject("explicit selection must name an actual model_phases entity");
        check_identity(selected->first, selected->second);
        std::map<std::string, std::string, std::less<>> physical_ownership;
        std::vector<PhysicalWallPhaseState> result;
        for (const auto& [id, entity] : entities) {
            if (entity.type == "wall" || is_physical_wall_room(entity)) check_identity(id, entity);
            if (entity.type != "model_phases") continue;
            check_identity(id, entity);
            const auto model = ModelPhases::from_json(entity.properties.at("model"));
            const auto alternative = id == selection.registry_id ? selection.alternative_id : model.active_alternative();
            auto states = model.state(alternative);
            for (const auto& member_id : model.entity_ids()) {
                const auto member = entities.find(member_id);
                if (member == entities.end())
                    reject("registry " + id + " references missing model entity " + member_id);
                check_identity(member_id, member->second);
                if (!is_model_phase_entity_type(member->second.type))
                    reject("registry " + id + " reference is not an architectural model entity: " + member_id);
                if (member->second.type == "wall" || is_physical_wall_room(member->second)) {
                    const auto [owner, inserted] = physical_ownership.emplace(member_id, id);
                    if (!inserted)
                        reject("physical owner " + member_id + " belongs to overlapping phase registries " +
                            owner->second + " and " + id);
                }
            }
            result.push_back({id, alternative, model.entity_ids(), std::move(states)});
        }
        return result;
    } catch (const nlohmann::json::exception& error) {
        reject(std::string("malformed phase source: ") + error.what());
    }
}

PhysicalWallPhaseRoomRoster physical_wall_phase_room_roster(
    const std::map<std::string, Entity, std::less<>>& entities,
    const PhysicalWallPhaseSelection& selection) {
    const auto inventory = physical_wall_phase_states(entities, selection);
    std::set<std::string, std::less<>> inactive;
    for (const auto& registry : inventory) {
        for (const auto& id : registry.registered_entity_ids) {
            const auto state = registry.states.find(id);
            if (state == registry.states.end() || state->second == ModelPhase::demolished)
                inactive.insert(id);
        }
    }
    PhysicalWallPhaseRoomRoster result;
    for (const auto& [id, entity] : entities) {
        if (!is_physical_wall_room(entity)) continue;
        (inactive.contains(id) ? result.inactive_room_ids : result.active_room_ids).push_back(id);
    }
    return result;
}
} // namespace sketch
