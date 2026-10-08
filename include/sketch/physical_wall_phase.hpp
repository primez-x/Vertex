#pragma once

#include "sketch/document.hpp"
#include "sketch/model_phases.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

// nullopt explicitly evaluates the registry's shared baseline, independently
// of its saved active alternative. The document and registry remain unchanged.
struct PhysicalWallPhaseSelection {
    std::string registry_id;
    std::optional<std::string> alternative_id;
    bool operator==(const PhysicalWallPhaseSelection&) const = default;
};

struct PhysicalWallPhaseRoomRoster {
    std::vector<std::string> active_room_ids;
    std::vector<std::string> inactive_room_ids;
};

// Complete, admitted semantic inventory for explicit analytical discovery.
// Registered IDs resolve to actual architectural model entities; actual walls
// and physical room owners cannot belong to more than one phase registry.
// Other registries retain their saved selection. An absent evaluated state or
// demolished state makes a registered owner inactive; unregistered owners stay
// active. Registry and member order is deterministic.
struct PhysicalWallPhaseState {
    std::string registry_id;
    std::optional<std::string> alternative_id;
    std::vector<std::string> registered_entity_ids;
    std::map<std::string, ModelPhase, std::less<>> states;
};
[[nodiscard]] std::vector<PhysicalWallPhaseState> physical_wall_phase_states(
    const std::map<std::string, Entity, std::less<>>& entities,
    const PhysicalWallPhaseSelection& selection);

// Enumerates only actual is_physical_wall_room owners, including unregistered
// active owners. This is semantic admission only: no geometry, classification,
// descriptor, document state or saved phase selection is modified.
[[nodiscard]] PhysicalWallPhaseRoomRoster physical_wall_phase_room_roster(
    const std::map<std::string, Entity, std::less<>>& entities,
    const PhysicalWallPhaseSelection& selection);

} // namespace sketch
