#include "sketch/constraint_phase_scope.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sketch {

ConstraintPhaseScope constraint_phase_scope(
    const std::map<std::string,Entity,std::less<>>& entities) {
    try {
        ConstraintPhaseScope result;
        result.assembly_memberships = resolve_assembly_phase_memberships(entities);
        const auto selected = std::find_if(entities.begin(),entities.end(),[](const auto& item) {
            return item.second.type == "model_phases";
        });
        if (selected == entities.end()) return result;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
        // The explicit physical inventory evaluates every other registry's saved
        // choice too. Supply this registry's actual saved choice, never visibility
        // hints or an owner's unbound phase_id.
        const auto model = ModelPhases::from_json(selected->second.properties.at("model"));
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

bool assembly_host_inactive_in_phase(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::set<std::string, std::less<>>& inactive_owner_ids,
    const std::string& host_id) {
    std::set<std::string,std::less<>> visited;
    const auto visit=[&](const auto& self,const std::string& id)->bool {
        if (inactive_owner_ids.contains(id)) return true;
        // A valid corner closure has only one owner, two cuts and two walls.
        // Bound malformed backlink chains while leaving their diagnosis to
        // the caller's normal complete-source admission.
        if (visited.size()>=8) return false;
        if (!visited.insert(id).second) return false;
        const auto host=entities.find(id);
        if (host==entities.end() || host->second.id!=id) return false;
        const auto parent=[&](const nlohmann::json& value,const char* type) {
            if (!value.is_string()) return false;
            const auto& parent_id=value.get_ref<const std::string&>();
            const auto found=entities.find(parent_id);
            return found!=entities.end() && found->second.id==parent_id && found->second.type==type &&
                self(self,parent_id);
        };
        const auto& properties=host->second.properties;
        if (host->second.type=="opening") {
            const auto wall=properties.find("wall_id");
            if (wall!=properties.end() && parent(*wall,"wall")) return true;
            const auto corner=properties.find("corner_window_id");
            if (corner!=properties.end() && corner->is_string()) {
                const auto owner=entities.find(corner->get_ref<const std::string&>());
                if (owner!=entities.end() && owner->second.type=="corner_window") {
                    const auto children=owner->second.properties.find("opening_ids");
                    if (children!=owner->second.properties.end() && children->is_array() && children->size()==2 &&
                        std::any_of(children->begin(),children->end(),[&](const auto& child) { return child==id; }) &&
                        parent(*corner,"corner_window")) return true;
                }
            }
        } else if (host->second.type=="corner_window") {
            // One real corner owner and its two cuts/walls form a finite
            // physical closure. Do not follow arbitrary metadata references.
            for (const auto& [field,type]:{std::pair{"wall_ids","wall"},std::pair{"opening_ids","opening"}}) {
                const auto parents=properties.find(field);
                if (parents==properties.end() || !parents->is_array() || parents->size()!=2) continue;
                for (const auto& value:*parents) if (parent(value,type)) return true;
            }
        }
        return false;
    };
    return visit(visit,host_id);
}

} // namespace sketch
