#include "sketch/assembly_phase_membership_transport.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = AssemblyPhaseTransportEntities;
constexpr std::size_t max_entities = 100000;
constexpr std::size_t max_bindings = 10000;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void document_id(std::string_view id) {
    require(!id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "assembly phase transport requires actual document identities");
}
void root_id(const AssemblyPhaseRoot& root) {
    document_id(root.first);
    require(!root.second.empty() && root.second.size() <= 4096 &&
        !std::all_of(root.second.begin(), root.second.end(), [](unsigned char c) { return std::isspace(c); }),
        "assembly phase transport requires a nonblank bounded local identity");
}
struct Budget {
    std::size_t bytes{}, nodes{};
    void text(std::string_view value) {
        require(value.size() <= 64 * 1024 * 1024 - bytes, "assembly phase transport byte budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64 && ++nodes <= 4 * 1024 * 1024,
            "assembly phase transport node/depth budget exceeded");
        require(!value.is_binary() && !value.is_discarded(), "assembly phase transport requires ordinary JSON");
        text(std::string_view("                                "));
        if (value.is_number_float())
            require(std::isfinite(value.get<double>()), "assembly phase transport requires finite JSON numbers");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
    void entities(const Entities& values) {
        require(values.size() <= max_entities, "assembly phase transport entity budget exceeded");
        for (const auto& [id, entity] : values) {
            document_id(id);
            require(entity.id == id, "assembly phase transport entity key differs from actual identity");
            text(id); text(entity.type); read(entity.properties); read(entity.extensions);
        }
    }
};
bool exact(const Json& left, const Json& right) {
    if (left.type() != right.type() || left.size() != right.size()) return false;
    if (left.is_object()) {
        for (auto row = left.begin(); row != left.end(); ++row) {
            const auto other = right.find(row.key());
            if (other == right.end() || !exact(row.value(), *other)) return false;
        }
        return true;
    }
    if (left.is_array()) {
        for (std::size_t i = 0; i < left.size(); ++i) if (!exact(left.at(i), right.at(i))) return false;
        return true;
    }
    return left == right;
}
bool exact(const Entity& left, const Entity& right) {
    return left.id == right.id && left.type == right.type && left.required == right.required &&
        exact(left.properties, right.properties) && exact(left.extensions, right.extensions);
}
const Json& model(const Entity& entity, std::string_view type) {
    require(entity.type == type && entity.properties.is_object() &&
        entity.properties.contains("model") && entity.properties.at("model").is_object(),
        "assembly phase transport requires an actual typed model carrier");
    return entity.properties.at("model");
}
struct Phases {
    std::map<std::string, ModelPhases, std::less<>> registries;
    std::map<std::string, std::string, std::less<>> owners;
    std::map<std::string, ModelPhase, std::less<>> states;
    std::set<std::string, std::less<>> inactive;
    explicit Phases(const Entities& entities) {
        for (const auto& [id, entity] : entities) {
            if (entity.type != "model_phases") continue;
            auto registry = ModelPhases::from_json(model(entity, "model_phases"));
            const auto active = registry.active_state();
            for (const auto& owner : registry.entity_ids()) {
                const auto actual = entities.find(owner);
                require(actual != entities.end() && is_model_phase_entity_type(actual->second.type),
                    "assembly phase transport registry references a missing or mistyped actual owner");
                require(owners.emplace(owner, id).second,
                    "assembly phase transport actual owner belongs to overlapping registries");
                const auto phase = active.find(owner);
                if (phase != active.end()) states.emplace(owner, phase->second);
                if (phase == active.end() || phase->second == ModelPhase::demolished) inactive.insert(owner);
            }
            registries.emplace(id, std::move(registry));
        }
    }
    const ModelPhases& selected(std::string_view id, std::string_view alternative) const {
        const auto found = registries.find(id);
        require(found != registries.end() && found->second.active_alternative() &&
            *found->second.active_alternative() == alternative,
            "assembly phase transport requires the actual saved active alternative");
        return found->second;
    }
    void owned(const std::string& host, std::string_view registry) const {
        const auto found = owners.find(host);
        require(found != owners.end() && found->second == registry,
            "assembly phase transport cannot encode cross-registry or unregistered host coupling");
    }
};
struct Catalog {
    std::map<std::string, AssemblyInstance, std::less<>> instances;
    std::map<std::string, const Json*, std::less<>> raw;
};
using Catalogs = std::map<std::string, Catalog, std::less<>>;
void admit_catalogs(const Entities& entities, const std::set<std::string, std::less<>>& ids,
    AssemblyCatalogTransferBudget& budget) {
    // Reserve every reached catalog once before the first expensive decoder.
    for (const auto& id : ids) {
        const auto found = entities.find(id);
        require(found != entities.end(), "assembly phase transport actual catalog is missing");
        (void)model(found->second, "assembly_model");
        admit_existing_assembly_catalog_work(found->second, budget);
    }
}
Catalogs catalogs(const Entities& entities, const std::set<std::string, std::less<>>& ids) {
    Catalogs result;
    for (const auto& id : ids) {
        const auto& raw = model(entities.at(id), "assembly_model");
        const auto decoded = AssemblyModel::from_json(raw);
        Catalog entry;
        for (const auto& instance : decoded.instances()) entry.instances.emplace(instance.id, instance);
        for (const auto& row : raw.at("instances"))
            entry.raw.emplace(row.at("id").get<std::string>(), &row);
        result.emplace(id, std::move(entry));
    }
    return result;
}
const AssemblyInstance& instance(const Catalogs& values, const AssemblyPhaseRoot& root) {
    const auto catalog = values.find(root.first);
    require(catalog != values.end(), "assembly phase transport source catalog was not admitted");
    const auto row = catalog->second.instances.find(root.second);
    require(row != catalog->second.instances.end(), "assembly phase transport actual instance row is missing");
    return row->second;
}
void companions_exact(const Entities& source, const Entities& candidate, bool permit_absent) {
    for (const auto& [id, entity] : source) {
        if (entity.type != assembly_phase_membership_entity_type) continue;
        const auto found = candidate.find(id);
        require((permit_absent && found == candidate.end()) ||
            (found != candidate.end() && exact(entity, found->second)),
            "assembly phase transport input changed or removed an actual source companion");
    }
}
void actual_host(const Entities& entities, const std::string& id) {
    const auto found = entities.find(id);
    require(found != entities.end() && is_complete_assembly_catalog_host_type(found->second.type),
        "assembly phase transport placement needs its actual physical host");
}
void inherited_host(const Entities& entities, const Phases& phases,
    const std::string& host, std::string_view registry) {
    require(!assembly_host_inactive_in_phase(entities, phases.inactive, host),
        "assembly phase transport source placement host is inactive");
    const auto& entity = entities.at(host);
    if (entity.type != "opening") return;
    require(entity.properties.is_object() && entity.properties.contains("wall_id") &&
        entity.properties.at("wall_id").is_string(), "assembly phase transport opening host has no actual wall");
    const auto& parent = entity.properties.at("wall_id").get_ref<const std::string&>();
    const auto wall = entities.find(parent);
    require(wall != entities.end() && wall->second.type == "wall",
        "assembly phase transport opening host references a missing actual wall");
    const auto owner = phases.owners.find(parent);
    require(owner == phases.owners.end() || owner->second == registry,
        "assembly phase transport cannot encode cross-registry inherited host coupling");
}
} // namespace

void validate_assembly_phase_retirement_roots(const Entities& actual,
    const std::vector<AssemblyPhaseRoot>& retired_roots,
    std::string_view registry_id, std::string_view alternative_id) {
    try {
        require(retired_roots.size() <= max_bindings, "assembly phase retirement root budget exceeded");
        document_id(registry_id);
        require(!alternative_id.empty() && alternative_id.size() <= 4096 &&
            !std::all_of(alternative_id.begin(), alternative_id.end(), [](unsigned char c) { return std::isspace(c); }),
            "assembly phase retirement requires a bounded nonblank alternative identity");
        Budget raw_budget;
        raw_budget.text(registry_id); raw_budget.text(alternative_id);
        raw_budget.entities(actual);
        std::set<AssemblyPhaseRoot> unique;
        std::set<std::string, std::less<>> reached_catalogs, all_catalogs;
        for (const auto& root : retired_roots) {
            root_id(root);
            raw_budget.text(root.first); raw_budget.text(root.second);
            require(unique.insert(root).second, "assembly phase retirement requires unique actual roots");
            reached_catalogs.insert(root.first);
        }
        for (const auto& [id, entity] : actual)
            if (entity.type == "assembly_model") all_catalogs.insert(id);
        AssemblyCatalogTransferBudget catalog_budget;
        admit_catalogs(actual, all_catalogs, catalog_budget);
        const Phases phases(actual);
        (void)phases.selected(registry_id, alternative_id);
        const auto scope = resolve_assembly_phase_memberships(actual);
        const auto actual_catalogs = catalogs(actual, reached_catalogs);
        std::map<std::string, AssemblyPhaseMembership, std::less<>> companions;
        for (const auto& root : retired_roots) {
            (void)instance(actual_catalogs, root);
            const auto owner = scope.owners.find(root);
            if (owner == scope.owners.end()) continue;
            require(owner->second.registry_id == registry_id,
                "assembly phase retirement row belongs to a foreign registry");
            const auto state = scope.states.find(root);
            require(state != scope.states.end() && state->second == ModelPhase::proposed &&
                !scope.inactive_roots.contains(root),
                "assembly phase retirement requires an actual active proposed row");
            const auto& companion_id = owner->second.membership_entity_id;
            if (!companions.contains(companion_id))
                companions.emplace(companion_id, AssemblyPhaseMembership::from_json(
                    model(actual.at(companion_id), assembly_phase_membership_entity_type)));
            const auto& membership = companions.at(companion_id);
            require(membership.registry_id() == registry_id &&
                std::find(membership.baseline_roots().begin(), membership.baseline_roots().end(), root) ==
                    membership.baseline_roots().end(),
                "assembly phase retirement cannot delete a baseline row");
            bool proposed = false;
            for (const auto& alternative : membership.alternatives()) {
                require(std::find(alternative.demolished_roots.begin(), alternative.demolished_roots.end(), root) ==
                    alternative.demolished_roots.end(),
                    "assembly phase retirement cannot delete a row with demolition usage");
                if (std::find(alternative.proposed_roots.begin(), alternative.proposed_roots.end(), root) ==
                    alternative.proposed_roots.end()) continue;
                require(alternative.alternative_id == alternative_id && !proposed,
                    "assembly phase retirement row has foreign or repeated proposed usage");
                proposed = true;
            }
            require(proposed, "assembly phase retirement row lacks its exact source proposed partition");
        }
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly phase retirement roots: ") + error.what());
    }
}

AssemblyPhaseTransportEntities transport_assembly_phase_memberships_for_host_copies(
    const Entities& source, const Entities& candidate, const AssemblyPhaseCopiedRoots& copied_roots,
    const AssemblyPhaseCopiedHosts& copied_hosts, std::string_view registry_id, std::string_view alternative_id) {
    try {
        require(copied_roots.size() <= max_bindings && copied_hosts.size() <= max_bindings,
            "assembly phase transport copied binding budget exceeded");
        document_id(registry_id);
        require(!alternative_id.empty() && alternative_id.size() <= 4096,
            "assembly phase transport requires a bounded alternative identity");
        Budget budget;
        budget.text(registry_id); budget.text(alternative_id);
        for (const auto& [from, to] : copied_roots) {
            root_id(from); root_id(to);
            budget.text(from.first); budget.text(from.second); budget.text(to.first); budget.text(to.second);
        }
        for (const auto& [from, to] : copied_hosts) {
            document_id(from); document_id(to); budget.text(from); budget.text(to);
        }
        budget.entities(source); budget.entities(candidate);
        const Phases original_phases(source), proposed_phases(candidate);
        const auto& original_registry = original_phases.selected(registry_id, alternative_id);
        const auto& proposed_registry = proposed_phases.selected(registry_id, alternative_id);
        const auto original_scope = resolve_assembly_phase_memberships(source);
        const auto proposed_scope = resolve_assembly_phase_memberships(candidate);
        if (copied_roots.empty()) return candidate;
        companions_exact(source, candidate, false);

        std::set<std::string, std::less<>> original_catalog_ids, proposed_catalog_ids, fresh_hosts;
        std::set<AssemblyPhaseRoot> fresh_roots;
        for (const auto& [from, to] : copied_roots) {
            require(from != to && fresh_roots.insert(to).second && !copied_roots.contains(to),
                "assembly phase transport destination is aliased, duplicated or another source");
            require(!proposed_scope.owners.contains(to),
                "assembly phase transport cannot overwrite or re-enroll destination membership");
            original_catalog_ids.insert(from.first); proposed_catalog_ids.insert(from.first);
            proposed_catalog_ids.insert(to.first);
            if (source.contains(to.first)) original_catalog_ids.insert(to.first);
        }
        for (const auto& [from, to] : copied_hosts) {
            require(from != to && !source.contains(to) && fresh_hosts.insert(to).second && !copied_hosts.contains(to),
                "assembly phase transport copied host must have a fresh injective actual identity");
            actual_host(source, from); actual_host(candidate, from); actual_host(candidate, to);
            require(source.at(from).type == candidate.at(to).type && exact(source.at(from), candidate.at(from)),
                "assembly phase transport copied host changed its retained baseline or physical family");
            original_phases.owned(from, registry_id); proposed_phases.owned(from, registry_id);
            proposed_phases.owned(to, registry_id);
            require(std::binary_search(original_registry.baseline_ids().begin(), original_registry.baseline_ids().end(), from) &&
                original_phases.states.contains(from) && original_phases.states.at(from) == ModelPhase::existing &&
                std::binary_search(proposed_registry.baseline_ids().begin(), proposed_registry.baseline_ids().end(), from) &&
                proposed_phases.states.contains(from) && proposed_phases.states.at(from) == ModelPhase::demolished &&
                proposed_phases.states.contains(to) && proposed_phases.states.at(to) == ModelPhase::proposed,
                "assembly phase transport requires an actual baseline host replacement in the saved alternative");
            inherited_host(source, original_phases, from, registry_id);
            inherited_host(candidate, proposed_phases, to, registry_id);
        }
        AssemblyCatalogTransferBudget catalog_budget;
        admit_catalogs(source, original_catalog_ids, catalog_budget);
        admit_catalogs(candidate, proposed_catalog_ids, catalog_budget);
        const auto original_catalogs = catalogs(source, original_catalog_ids);
        const auto proposed_catalogs = catalogs(candidate, proposed_catalog_ids);
        struct Additions { std::vector<AssemblyPhaseRoot> demolished, proposed; };
        std::map<std::string, Additions, std::less<>> edits;
        for (const auto& [from, to] : copied_roots) {
            const auto& original = instance(original_catalogs, from);
            const auto& destination = instance(proposed_catalogs, to);
            (void)instance(proposed_catalogs, from);
            const auto existing_catalog = original_catalogs.find(to.first);
            require(existing_catalog == original_catalogs.end() || !existing_catalog->second.instances.contains(to.second),
                "assembly phase transport destination qualified identity is not fresh");
            require(exact(*original_catalogs.at(from.first).raw.at(from.second),
                *proposed_catalogs.at(from.first).raw.at(from.second)),
                "assembly phase transport changed its retained actual source row");
            require(original.placement && destination.placement,
                "assembly phase transport copied rows require actual placements");
            const auto host = copied_hosts.find(original.placement->host_entity_id);
            require(host != copied_hosts.end() && host->second == destination.placement->host_entity_id,
                "assembly phase transport destination host is not its exact source-derived copied host");
            const auto owner = original_scope.owners.find(from);
            if (owner == original_scope.owners.end()) continue; // No inferred legacy enrollment.
            require(owner->second.registry_id == registry_id,
                "assembly phase transport cannot encode cross-registry row/host coupling");
            const auto state = original_scope.states.find(from);
            require(state != original_scope.states.end() && state->second != ModelPhase::demolished,
                "assembly phase transport source row is absent or inactive in the saved alternative");
            auto& edit = edits[owner->second.membership_entity_id];
            if (state->second == ModelPhase::existing) edit.demolished.push_back(from);
            edit.proposed.push_back(to);
        }
        // Derive each companion once. Catalog decoding and partition copying
        // are independent of the number of copied rows in that catalog.
        std::map<std::string, Json, std::less<>> replacements;
        for (const auto& [id, edit] : edits) {
            const auto& saved = model(source.at(id), assembly_phase_membership_entity_type);
            const auto membership = AssemblyPhaseMembership::from_json(saved);
            require(edit.proposed.size() <= max_bindings - membership.roots().size(),
                "assembly phase transport resulting root budget exceeded");
            auto roots = membership.roots();
            auto alternatives = membership.alternatives();
            roots.insert(roots.end(), edit.proposed.begin(), edit.proposed.end());
            auto alternative = std::find_if(alternatives.begin(), alternatives.end(),
                [&](const auto& row) { return row.alternative_id == alternative_id; });
            if (alternative == alternatives.end()) {
                require(alternatives.size() < 1000, "assembly phase transport resulting alternative budget exceeded");
                alternatives.push_back({std::string(alternative_id), {}, {}});
                alternative = std::prev(alternatives.end());
            }
            alternative->demolished_roots.insert(alternative->demolished_roots.end(), edit.demolished.begin(), edit.demolished.end());
            alternative->proposed_roots.insert(alternative->proposed_roots.end(), edit.proposed.begin(), edit.proposed.end());
            const auto proposed = AssemblyPhaseMembership::create(membership.registry_id(), std::move(roots),
                membership.baseline_roots(), std::move(alternatives));
            replacements.emplace(id, retain_assembly_phase_membership_source(saved, proposed));
        }
        auto result = candidate;
        for (auto& [id, replacement] : replacements) result.at(id).properties.at("model") = std::move(replacement);
        (void)resolve_assembly_phase_memberships(result);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly phase copy transport: ") + error.what());
    }
}

AssemblyPhaseTransportEntities transport_assembly_phase_memberships_for_independent_host_copies(
    const Entities& source, const Entities& candidate, const AssemblyPhaseCopiedRoots& copied_roots,
    const AssemblyPhaseCopiedHosts& copied_hosts) {
    try {
        require(copied_roots.size() <= max_bindings && copied_hosts.size() <= max_bindings,
            "independent assembly phase transport copied binding budget exceeded");
        Budget budget;
        for (const auto& [from, to] : copied_roots) {
            root_id(from); root_id(to);
            budget.text(from.first); budget.text(from.second); budget.text(to.first); budget.text(to.second);
        }
        for (const auto& [from, to] : copied_hosts) {
            document_id(from); document_id(to); budget.text(from); budget.text(to);
        }
        budget.entities(source); budget.entities(candidate);
        // Admit the complete carriers and cumulative nonlinear work before any
        // scope or catalog decoder, including catalogs not reached by a copy.
        std::set<std::string, std::less<>> original_catalog_ids, proposed_catalog_ids;
        for (const auto& [id, entity] : source)
            if (entity.type == "assembly_model") original_catalog_ids.insert(id);
        for (const auto& [id, entity] : candidate)
            if (entity.type == "assembly_model") proposed_catalog_ids.insert(id);
        AssemblyCatalogTransferBudget catalog_budget;
        admit_catalogs(source, original_catalog_ids, catalog_budget);
        admit_catalogs(candidate, proposed_catalog_ids, catalog_budget);
        const Phases original_phases(source), proposed_phases(candidate);
        const auto original_scope = resolve_assembly_phase_memberships(source);
        const auto proposed_scope = resolve_assembly_phase_memberships(candidate);
        const auto original_catalogs = catalogs(source, original_catalog_ids);
        const auto proposed_catalogs = catalogs(candidate, proposed_catalog_ids);
        companions_exact(source, candidate, false);

        // Project away only genuinely fresh physical owners. Every source
        // registry role, alternative, saved choice and raw representation must
        // survive ordinary enrollment, including inactive alternative roles.
        for (const auto& [id, registry] : original_phases.registries) {
            (void)registry;
            const auto found = candidate.find(id);
            require(found != candidate.end() && proposed_phases.registries.contains(id),
                "independent assembly phase transport removed an actual source registry");
            auto retained = found->second;
            auto& saved_roles = retained.properties.at("model");
            const auto remove_fresh = [&](Json& rows) {
                std::erase_if(rows.get_ref<Json::array_t&>(), [&](const Json& row) {
                    return !source.contains(row.get_ref<const std::string&>());
                });
            };
            remove_fresh(saved_roles.at("entity_ids"));
            remove_fresh(saved_roles.at("baseline_ids"));
            for (auto& alternative : saved_roles.at("alternatives")) {
                remove_fresh(alternative.at("demolished_ids"));
                remove_fresh(alternative.at("proposed_ids"));
            }
            require(exact(source.at(id), retained),
                "independent assembly phase transport changed source registry roles or saved selection");
        }
        for (const auto& [id, registry_id] : proposed_phases.owners) {
            if (!source.contains(id)) continue;
            const auto owner = original_phases.owners.find(id);
            require(owner != original_phases.owners.end() && owner->second == registry_id,
                "independent assembly phase transport changed an original physical owner's registry");
        }

        std::set<std::string, std::less<>> fresh_hosts;
        for (const auto& [from, to] : copied_hosts) {
            require(from != to && !source.contains(to) && fresh_hosts.insert(to).second && !copied_hosts.contains(to),
                "independent assembly phase transport copied host must have a fresh injective actual identity");
            actual_host(source, from); actual_host(candidate, from); actual_host(candidate, to);
            require(source.at(from).type == candidate.at(to).type && exact(source.at(from), candidate.at(from)),
                "independent assembly phase transport changed its retained source host or physical family");
            require(!assembly_host_inactive_in_phase(source, original_phases.inactive, from) &&
                !assembly_host_inactive_in_phase(candidate, proposed_phases.inactive, to),
                "independent assembly phase transport copied placement host is inactive");
            const auto destination_owner = proposed_phases.owners.find(to);
            if (destination_owner == proposed_phases.owners.end()) continue;
            const auto& registry = proposed_phases.registries.at(destination_owner->second);
            const auto state = proposed_phases.states.find(to);
            require(state != proposed_phases.states.end() && state->second ==
                (registry.active_alternative() ? ModelPhase::proposed : ModelPhase::existing),
                "independent assembly phase transport requires actual ordinary fresh host enrollment");
        }

        struct Additions {
            std::vector<AssemblyPhaseRoot> baseline;
            std::map<std::string, std::vector<AssemblyPhaseRoot>, std::less<>> proposed;
            std::size_t count{};
        };
        std::map<std::string, Additions, std::less<>> edits;
        std::set<AssemblyPhaseRoot> fresh_roots;
        std::set<std::string, std::less<>> checked_source_catalogs;
        for (const auto& [from, to] : copied_roots) {
            require(from != to && fresh_roots.insert(to).second && !copied_roots.contains(to),
                "independent assembly phase transport destination is aliased, duplicated or another source");
            require(!original_scope.owners.contains(to) && !proposed_scope.owners.contains(to),
                "independent assembly phase transport cannot overwrite or re-enroll destination membership");
            const auto& original = instance(original_catalogs, from);
            const auto& destination = instance(proposed_catalogs, to);
            const auto existing_catalog = original_catalogs.find(to.first);
            require(existing_catalog == original_catalogs.end() || !existing_catalog->second.instances.contains(to.second),
                "independent assembly phase transport destination qualified identity is not fresh");
            if (checked_source_catalogs.insert(from.first).second) {
                const auto retained = candidate.find(from.first);
                require(retained != candidate.end() && exact(source.at(from.first), retained->second),
                    "independent assembly phase transport changed its retained actual source catalog");
            }
            require(original.placement && destination.placement,
                "independent assembly phase transport copied rows require actual placements");
            const auto host = copied_hosts.find(original.placement->host_entity_id);
            require(host != copied_hosts.end() && host->second == destination.placement->host_entity_id,
                "independent assembly phase transport destination host is not its actual source-derived copied host");
            // Placement has no independent host-kind field: the actual typed
            // host is its authority, and its kind must survive the row mapping.
            require(source.at(original.placement->host_entity_id).type ==
                candidate.at(destination.placement->host_entity_id).type,
                "independent assembly phase transport changed the actual placement host kind");
            const auto owner = original_scope.owners.find(from);
            if (owner == original_scope.owners.end()) continue; // No inferred legacy enrollment.
            const auto state = original_scope.states.find(from);
            require(state != original_scope.states.end() && state->second != ModelPhase::demolished,
                "independent assembly phase transport source row is absent or inactive in its saved design");
            const auto& registry_id = owner->second.registry_id;
            const auto source_host_owner = original_phases.owners.find(original.placement->host_entity_id);
            require(source_host_owner == original_phases.owners.end() || source_host_owner->second == registry_id,
                "independent assembly phase transport cannot encode cross-registry source row/host coupling");
            proposed_phases.owned(destination.placement->host_entity_id, registry_id);
            inherited_host(source, original_phases, original.placement->host_entity_id, registry_id);
            inherited_host(candidate, proposed_phases, destination.placement->host_entity_id, registry_id);
            const auto& registry = proposed_phases.registries.at(registry_id);
            require(original_phases.registries.at(registry_id).active_alternative() == registry.active_alternative(),
                "independent assembly phase transport source and destination must retain the same saved selection");
            auto& edit = edits[owner->second.membership_entity_id];
            ++edit.count;
            if (registry.active_alternative()) edit.proposed[*registry.active_alternative()].push_back(to);
            else edit.baseline.push_back(to);
        }
        if (copied_roots.empty()) return candidate;

        std::map<std::string, Json, std::less<>> replacements;
        for (const auto& [id, edit] : edits) {
            const auto& saved = model(source.at(id), assembly_phase_membership_entity_type);
            const auto membership = AssemblyPhaseMembership::from_json(saved);
            require(edit.count <= max_bindings - membership.roots().size(),
                "independent assembly phase transport resulting root budget exceeded");
            auto roots = membership.roots();
            auto baseline = membership.baseline_roots();
            auto alternatives = membership.alternatives();
            roots.insert(roots.end(), edit.baseline.begin(), edit.baseline.end());
            baseline.insert(baseline.end(), edit.baseline.begin(), edit.baseline.end());
            for (const auto& [alternative_id, additions] : edit.proposed) {
                roots.insert(roots.end(), additions.begin(), additions.end());
                auto alternative = std::find_if(alternatives.begin(), alternatives.end(),
                    [&](const auto& row) { return row.alternative_id == alternative_id; });
                if (alternative == alternatives.end()) {
                    require(alternatives.size() < 1000,
                        "independent assembly phase transport resulting alternative budget exceeded");
                    alternatives.push_back({alternative_id, {}, {}});
                    alternative = std::prev(alternatives.end());
                }
                alternative->proposed_roots.insert(alternative->proposed_roots.end(), additions.begin(), additions.end());
            }
            const auto proposed = AssemblyPhaseMembership::create(membership.registry_id(), std::move(roots),
                std::move(baseline), std::move(alternatives));
            replacements.emplace(id, retain_assembly_phase_membership_source(saved, proposed));
        }
        auto result = candidate;
        for (auto& [id, replacement] : replacements) result.at(id).properties.at("model") = std::move(replacement);
        (void)resolve_assembly_phase_memberships(result);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid independent assembly phase copy transport: ") + error.what());
    }
}

AssemblyPhaseTransportEntities prune_removed_assembly_phase_memberships(const Entities& source, const Entities& candidate) {
    try {
        Budget budget;
        budget.entities(source); budget.entities(candidate);
        companions_exact(source, candidate, true);
        const Phases original_phases(source), proposed_phases(candidate);
        (void)proposed_phases;
        const auto original_scope = resolve_assembly_phase_memberships(source);
        std::set<std::string, std::less<>> surviving_catalog_ids;
        for (const auto& [root, owner] : original_scope.owners) {
            (void)owner;
            if (candidate.contains(root.first)) surviving_catalog_ids.insert(root.first);
        }
        AssemblyCatalogTransferBudget catalog_budget;
        admit_catalogs(candidate, surviving_catalog_ids, catalog_budget);
        const auto surviving_catalogs = catalogs(candidate, surviving_catalog_ids);
        const auto absent = [&](const AssemblyPhaseRoot& root) {
            const auto catalog = surviving_catalogs.find(root.first);
            return catalog == surviving_catalogs.end() || !catalog->second.instances.contains(root.second);
        };
        std::map<std::string, Json, std::less<>> replacements;
        std::set<std::string, std::less<>> removed_companions;
        for (const auto& [id, entity] : source) {
            if (entity.type != assembly_phase_membership_entity_type) continue;
            const auto& saved = model(entity, assembly_phase_membership_entity_type);
            const auto membership = AssemblyPhaseMembership::from_json(saved);
            const auto& original_registry = original_phases.registries.at(membership.registry_id());
            if (original_registry.active_alternative())
                require(std::none_of(membership.baseline_roots().begin(), membership.baseline_roots().end(), absent),
                    "assembly phase removal cannot delete a source baseline row in an active alternative");
            if (!candidate.contains(membership.registry_id())) {
                require(std::all_of(membership.roots().begin(), membership.roots().end(), absent),
                    "assembly phase registry removal retains a bound actual catalog row");
                removed_companions.insert(id);
                continue;
            }
            require(candidate.contains(id), "assembly phase removal cannot drop a surviving registry's source companion");
            auto roots = membership.roots();
            std::erase_if(roots, absent);
            if (roots.size() == membership.roots().size()) continue;
            auto baseline = membership.baseline_roots();
            auto alternatives = membership.alternatives();
            std::erase_if(baseline, absent);
            for (auto& alternative : alternatives) {
                std::erase_if(alternative.demolished_roots, absent);
                std::erase_if(alternative.proposed_roots, absent);
            }
            const auto proposed = AssemblyPhaseMembership::create(membership.registry_id(), std::move(roots),
                std::move(baseline), std::move(alternatives));
            replacements.emplace(id, retain_assembly_phase_membership_source(saved, proposed));
        }
        auto result = candidate;
        for (const auto& id : removed_companions) result.erase(id);
        for (auto& [id, replacement] : replacements) result.at(id).properties.at("model") = std::move(replacement);
        (void)resolve_assembly_phase_memberships(result);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid assembly phase removal transport: ") + error.what());
    }
}

} // namespace sketch
