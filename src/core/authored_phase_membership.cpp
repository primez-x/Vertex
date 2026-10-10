#include "sketch/authored_phase_membership.hpp"

#include "sketch/corner_window.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/stair_semantics.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
bool hosted_stair_railing(const Entity& entity) {
    const auto& p = entity.properties;
    return entity.type == "railing" && p.is_object() && p.contains("version") &&
        p.at("version").is_number_integer() && p.contains("form") &&
        ((p.at("version") == 2 && p.at("form") == "stair_flight_railing") ||
         (p.at("version") == 3 && p.at("form") == "stair_landing_railing"));
}
const std::string& stair_railing_host_id(const Railing& rail) {
    if (rail.host) return rail.host->stair_id;
    if (rail.landing_host) return rail.landing_host->stair_id;
    throw std::invalid_argument("The railing has no supported stair host.");
}
std::optional<std::string> read_string(const nlohmann::json& object, std::string_view key) {
    const auto key_string = std::string(key);
    if (!object.contains(key_string) || !object.at(key_string).is_string()) return std::nullopt;
    return object.at(key_string).get<std::string>();
}
} // namespace

void register_new_phase_memberships(
    const std::map<std::string, Entity, std::less<>>& actual, ApplyEntityChanges& command,
    const std::set<std::string, std::less<>>& admitted_destinations, const std::string& registry_id) {
    auto* changes = &command;
    // Evaluate the actual post-command registries, including an explicit
    // imported/replacement cohort. Never re-enroll its owners into another set.
    std::map<std::string, Entity, std::less<>> registry_entities;
    for (const auto& [id, entity] : actual)
        if (entity.type == "model_phases") registry_entities.emplace(id, entity);
    std::set<std::string, std::less<>> removed;
    for (const auto& change : changes->entity_changes) {
        if (change.kind == EntityChangeKind::erase) {
            removed.insert(change.entity_id);
            registry_entities.erase(change.entity_id);
        } else if (change.entity.type == "model_phases") {
            registry_entities.insert_or_assign(change.entity.id, change.entity);
        } else if (registry_entities.contains(change.entity.id)) {
            throw std::invalid_argument("A design set cannot change its entity type.");
        }
    }
    struct RegistryEdit {
        Entity entity;
        ModelPhases model;
        std::vector<std::string> ids;
        std::vector<std::string> baseline;
        std::vector<RemodelingAlternative> alternatives;
        bool changed{};
    };
    std::vector<RegistryEdit> registries;
    std::map<std::string, std::size_t, std::less<>> memberships;
    std::optional<std::size_t> selected;
    for (const auto& [id, entity] : registry_entities) {
        auto model = ModelPhases::from_json(entity.properties.at("model"));
        RegistryEdit edit{entity, model, model.entity_ids(), model.baseline_ids(), model.alternatives()};
        const auto count = edit.ids.size();
        std::erase_if(edit.ids, [&](const auto& owner) { return removed.contains(owner); });
        std::erase_if(edit.baseline, [&](const auto& owner) { return removed.contains(owner); });
        for (auto& alternative : edit.alternatives) {
            std::erase_if(alternative.demolished_ids, [&](const auto& owner) { return removed.contains(owner); });
            std::erase_if(alternative.proposed_ids, [&](const auto& owner) { return removed.contains(owner); });
        }
        edit.changed = count != edit.ids.size();
        const auto index = registries.size();
        if (id == registry_id) selected = index;
        for (const auto& owner : edit.ids)
            if (!memberships.emplace(owner, index).second)
                throw std::invalid_argument("An object belongs to overlapping design sets: " + owner);
        registries.push_back(std::move(edit));
    }
    const auto fresh = [&](const EntityChange& change) {
        return change.kind == EntityChangeKind::upsert && !actual.contains(change.entity.id) &&
            !admitted_destinations.contains(change.entity.id) && is_model_phase_entity_type(change.entity.type);
    };
    const auto enroll_current = [&](std::size_t index, const std::string& owner) {
        auto& edit = registries.at(index);
        edit.ids.push_back(owner);
        if (edit.model.active_alternative()) {
            for (auto& alternative : edit.alternatives)
                if (alternative.id == *edit.model.active_alternative()) alternative.proposed_ids.push_back(owner);
        } else edit.baseline.push_back(owner);
        memberships.emplace(owner, index);
        edit.changed = true;
    };
    const auto effective_entity = [&](const std::string& id) -> const Entity* {
        for (auto change = changes->entity_changes.rbegin(); change != changes->entity_changes.rend(); ++change) {
            if (change->kind == EntityChangeKind::erase && change->entity_id == id) return nullptr;
            if (change->kind == EntityChangeKind::upsert && change->entity.id == id) return &change->entity;
        }
        const auto found = actual.find(id);
        return found == actual.end() ? nullptr : &found->second;
    };
    // Pure retained transform workers do not have an editing target and
    // cannot introduce independent owners. UI authoring captures one explicitly.
    for (const auto& change : changes->entity_changes) {
        if (!fresh(change) || memberships.contains(change.entity.id) ||
            hosted_stair_railing(change.entity) || change.entity.type == "opening" ||
            change.entity.type == "corner_window" ||
            change.entity.type == "building" || change.entity.type == "floor") continue;
        if (!selected) {
            if (!registry_entities.empty() || !registry_id.empty())
                throw std::invalid_argument("Choose an existing design set before adding model objects.");
            continue;
        }
        enroll_current(*selected, change.entity.id);
    }
    for (const auto& change : changes->entity_changes) {
        if (!fresh(change) || change.entity.type != "corner_window") continue;
        const auto corner = parse_corner_window(change.entity);
        const auto first = memberships.find(corner.wall_ids[0]);
        const auto second = memberships.find(corner.wall_ids[1]);
        if ((first == memberships.end()) != (second == memberships.end()) ||
            (first != memberships.end() && first->second != second->second))
            throw std::invalid_argument("Both corner-window hosts must share their actual design set.");
        const auto own = memberships.find(corner.id);
        if (own != memberships.end()) {
            if (first == memberships.end() || own->second != first->second)
                throw std::invalid_argument("A corner window cannot belong to another design set.");
            continue;
        }
        if (first == memberships.end()) continue; // Preserve legacy unregistered host ownership.
        const auto& edit = registries.at(first->second);
        const auto state = ModelPhases::create(edit.ids, edit.baseline, edit.alternatives,
            edit.model.active_alternative()).active_state();
        for (const auto& host : corner.wall_ids)
            if (!state.contains(host) || state.at(host) == ModelPhase::demolished)
                throw std::invalid_argument("Choose two active walls before adding a corner window.");
        enroll_current(first->second, corner.id);
    }
    for (const auto& change : changes->entity_changes) {
        if (!fresh(change)) continue;
        const auto railing = hosted_stair_railing(change.entity);
        if (!railing && change.entity.type != "opening") continue;
        const auto host = railing
            ? stair_railing_host_id(decode_railing_properties(change.entity.id, change.entity.properties))
            : read_string(change.entity.properties, "wall_id").value_or("");
        const auto* host_entity = effective_entity(host);
        if (!host_entity || host_entity->type != (railing ? "stair" : "wall"))
            throw std::invalid_argument("The hosted component needs its actual current host.");
        const auto host_membership = memberships.find(host);
        const auto own_membership = memberships.find(change.entity.id);
        if (own_membership != memberships.end()) {
            if (host_membership == memberships.end() || own_membership->second != host_membership->second)
                throw std::invalid_argument("A hosted component cannot belong to a different design set from its host.");
            // Explicit source-derived cohorts already carry their lifecycle,
            // including inactive alternatives. Do not reinterpret that intent.
            continue;
        }
        // Legacy unregistered hosts keep their legacy ownership. A selected
        // unrelated set cannot acquire their opening or hosted railing.
        if (host_membership == memberships.end()) {
            if (railing && !registry_entities.empty())
                throw std::invalid_argument("A new stair railing needs a host in a design set.");
            continue;
        }
        auto& edit = registries.at(host_membership->second);
        if (!railing) {
            const auto state = ModelPhases::create(edit.ids, edit.baseline, edit.alternatives,
                edit.model.active_alternative()).active_state();
            const auto found = state.find(host);
            if (found == state.end() || found->second == ModelPhase::demolished)
                throw std::invalid_argument("Choose an active wall before adding a door or window.");
            enroll_current(host_membership->second, change.entity.id);
            continue;
        }
        // Rails inherit the stair's complete lifecycle, including alternatives
        // that are not displayed, so a rail cannot survive demolition of its host.
        edit.ids.push_back(change.entity.id);
        if (std::find(edit.baseline.begin(), edit.baseline.end(), host) != edit.baseline.end())
            edit.baseline.push_back(change.entity.id);
        for (auto& alternative : edit.alternatives) {
            if (std::find(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), host) != alternative.demolished_ids.end())
                alternative.demolished_ids.push_back(change.entity.id);
            if (std::find(alternative.proposed_ids.begin(), alternative.proposed_ids.end(), host) != alternative.proposed_ids.end())
                alternative.proposed_ids.push_back(change.entity.id);
        }
        memberships.emplace(change.entity.id, host_membership->second);
        edit.changed = true;
    }
    for (const auto& change : changes->entity_changes) {
        if (!fresh(change) || change.entity.type != "corner_window") continue;
        const auto corner = parse_corner_window(change.entity);
        const auto own = memberships.find(corner.id);
        if (own == memberships.end()) continue;
        auto& edit = registries.at(own->second);
        if (std::find(edit.baseline.begin(), edit.baseline.end(), corner.id) == edit.baseline.end()) continue;
        // A baseline assembly follows future demolition of either host.
        // Retire its owner and both cuts together in every saved alternative.
        for (auto& alternative : edit.alternatives) {
            if (std::none_of(corner.wall_ids.begin(), corner.wall_ids.end(), [&](const auto& host) {
                return std::find(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), host) != alternative.demolished_ids.end();
            })) continue;
            for (const auto& id : {corner.id, corner.opening_ids[0], corner.opening_ids[1]})
                if (std::find(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), id) == alternative.demolished_ids.end())
                    alternative.demolished_ids.push_back(id);
            edit.changed = true;
        }
    }
    for (auto& edit : registries) {
        if (!edit.changed) continue;
        const auto model = ModelPhases::create(std::move(edit.ids), std::move(edit.baseline),
            std::move(edit.alternatives), edit.model.active_alternative());
        edit.entity.properties["model"] = retain_model_phase_source(edit.entity.properties.at("model"), model);
        std::erase_if(changes->entity_changes, [&](const auto& change) {
            return change.kind == EntityChangeKind::upsert && change.entity.id == edit.entity.id;
        });
        changes->entity_changes.push_back(EntityChange::upsert(std::move(edit.entity)));
    }
}

} // namespace sketch

