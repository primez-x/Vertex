#include "sketch/constraint_phase_scope.hpp"

#include <algorithm>
#include <stdexcept>

namespace sketch {

ConstraintPhaseScope constraint_phase_scope(
    const std::map<std::string,Entity,std::less<>>& entities) {
    try {
        const auto selected = std::find_if(entities.begin(),entities.end(),[](const auto& item) {
            return item.second.type == "model_phases";
        });
        if (selected == entities.end()) return {};
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
        // The explicit physical inventory evaluates every other registry's saved
        // choice too. Supply this registry's actual saved choice, never visibility
        // hints or an owner's unbound phase_id.
        const auto model = ModelPhases::from_json(selected->second.properties.at("model"));
        ConstraintPhaseScope result;
        result.registries = physical_wall_phase_states(entities,
            {selected->first,model.active_alternative()});
        std::map<std::string,std::string,std::less<>> memberships;
        for (const auto& registry : result.registries) {
            for (const auto& id : registry.registered_entity_ids) {
                // Constraint owners also include identified boundaries and measured
                // strokes, beyond the physical inventory's wall/room exclusivity.
                const auto [previous,inserted] = memberships.emplace(id,registry.registry_id);
                if (!inserted)
                    throw std::invalid_argument("Constraint phase owner " + id +
                        " belongs to overlapping registries " + previous->second +
                        " and " + registry.registry_id);
                const auto state = registry.states.find(id);
                if (state == registry.states.end() || state->second == ModelPhase::demolished)
                    result.inactive_owner_ids.insert(id);
            }
        }
        return result;
#else
        throw std::invalid_argument("Active design constraints require the physical-wall engine");
#endif
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("Constraint phase source is malformed: ") + error.what());
    }
}

bool constraint_participates(const PersistentConstraint& constraint,
                            const ConstraintPhaseScope& scope) {
    return std::none_of(constraint.bindings.begin(),constraint.bindings.end(),
        [&](const auto& binding) { return scope.inactive_owner_ids.contains(binding.owner_id); });
}

} // namespace sketch
