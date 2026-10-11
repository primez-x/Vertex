#include "sketch/assembly_component_phase_demolition.hpp"

#include "sketch/assembly_document_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = AssemblyComponentPhaseDemolitionEntities;
constexpr std::size_t max_components = 1000;
constexpr std::size_t max_intent_bytes = 1024 * 1024;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void document_id(std::string_view id) {
    require(!id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        }), "component phase demolition requires an actual document identity");
}
void root_id(const AssemblyPhaseRoot& root) {
    document_id(root.first);
    require(!root.second.empty() && root.second.size() <= 4096 &&
        !std::all_of(root.second.begin(), root.second.end(), [](unsigned char c) { return std::isspace(c); }),
        "component phase demolition requires a bounded nonblank local identity");
}
struct Budget {
    std::size_t byte_limit{64 * 1024 * 1024};
    std::size_t node_limit{4 * 1024 * 1024};
    std::size_t bytes{}, nodes{};
    void charge(std::size_t amount) {
        require(amount <= byte_limit - bytes, "component phase demolition byte budget exceeded");
        bytes += amount;
    }
    void text(std::string_view value) {
        // Bound escaped JSON size without allocating a serialized copy.
        charge(value.size()); charge(2);
        for (const unsigned char c : value) {
            if (c < 0x20) charge(5);
            else if (c == '"' || c == '\\') charge(1);
        }
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64 && ++nodes <= node_limit,
            "component phase demolition node/depth budget exceeded");
        require(!value.is_binary() && !value.is_discarded(),
            "component phase demolition requires ordinary JSON");
        charge(32);
        if (value.is_number_float())
            require(std::isfinite(value.get<double>()), "component phase demolition requires finite JSON numbers");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
    void entities(const Entities& values) {
        require(values.size() <= 100000, "component phase demolition entity budget exceeded");
        for (const auto& [id, entity] : values) {
            document_id(id);
            require(entity.id == id, "component phase demolition entity key differs from actual identity");
            text(id); text(entity.type); read(entity.properties); read(entity.extensions);
        }
    }
};
void fields(const Json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "invalid component phase demolition fields");
    for (const auto* key : keys) require(value.contains(key), "missing component phase demolition field");
}
void admit_intent(const AssemblyComponentPhaseDemolitionIntent& intent) {
    require(!intent.components.empty() && intent.components.size() <= max_components,
        "component phase demolition requires 1 to 1000 components");
    Budget budget{max_intent_bytes, 32768};
    budget.charge(256);
    for (const auto& root : intent.components) {
        root_id(root);
        budget.charge(160); budget.text(root.first); budget.text(root.second);
    }
    require(std::is_sorted(intent.components.begin(), intent.components.end()) &&
        std::adjacent_find(intent.components.begin(), intent.components.end()) == intent.components.end(),
        "component phase demolition components must be sorted and unique");
}
const Json& model(const Entity& entity, std::string_view type) {
    require(entity.type == type && entity.properties.is_object() &&
        entity.properties.contains("model") && entity.properties.at("model").is_object(),
        "component phase demolition requires an actual typed model carrier");
    return entity.properties.at("model");
}
void admit_catalogs(const Entities& entities, AssemblyCatalogTransferBudget& budget) {
    // Include unused catalogs before any resolver or semantic model decoder.
    for (const auto& [id, entity] : entities) {
        (void)id;
        if (entity.type != "assembly_model") continue;
        (void)model(entity, "assembly_model");
        admit_existing_assembly_catalog_work(entity, budget);
    }
}
} // namespace

Json encode_assembly_component_phase_demolition_intent(const AssemblyComponentPhaseDemolitionIntent& intent) {
    admit_intent(intent);
    auto rows = Json::array();
    for (const auto& [catalog, instance] : intent.components)
        rows.push_back({{"catalog_id", catalog}, {"instance_id", instance}});
    Json result{{"version", 1}, {"kind", "component_phase_demolition"}, {"components", std::move(rows)}};
    Budget budget{max_intent_bytes, 32768};
    budget.read(result);
    return result;
}
AssemblyComponentPhaseDemolitionIntent decode_assembly_component_phase_demolition_intent(const Json& value) {
    try {
        Budget budget{max_intent_bytes, 32768};
        budget.read(value);
        fields(value, {"version", "kind", "components"});
        require(value.at("version").is_number_integer() && value.at("version") == 1 &&
            value.at("kind").is_string() && value.at("kind") == "component_phase_demolition",
            "unsupported component phase demolition intent");
        const auto& rows = value.at("components");
        require(rows.is_array() && !rows.empty() && rows.size() <= max_components,
            "component phase demolition requires 1 to 1000 component rows");
        AssemblyComponentPhaseDemolitionIntent result;
        result.components.reserve(rows.size());
        for (const auto& row : rows) {
            fields(row, {"catalog_id", "instance_id"});
            require(row.at("catalog_id").is_string() && row.at("instance_id").is_string(),
                "component phase demolition identities must be strings");
            result.components.emplace_back(row.at("catalog_id").get<std::string>(),
                row.at("instance_id").get<std::string>());
        }
        admit_intent(result);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid component phase demolition intent: ") + error.what());
    }
}
Entities replay_assembly_component_phase_demolition(const Entities& actual,
    const AssemblyComponentPhaseDemolitionIntent& intent) {
    try {
        admit_intent(intent);
        Budget raw_budget;
        raw_budget.entities(actual);
        AssemblyCatalogTransferBudget catalog_budget;
        admit_catalogs(actual, catalog_budget);
        const auto scope = resolve_assembly_phase_memberships(actual);

        std::map<std::string, AssemblyPhaseMembership, std::less<>> memberships;
        std::map<std::string, ModelPhases, std::less<>> registries;
        std::map<std::string, std::vector<AssemblyPhaseRoot>, std::less<>> additions;
        for (const auto& root : intent.components) {
            const auto owner = scope.owners.find(root);
            require(owner != scope.owners.end(),
                "component phase demolition requires actual declared qualified membership");
            const auto state = scope.states.find(root);
            require(state != scope.states.end() && state->second == ModelPhase::existing &&
                !scope.inactive_roots.contains(root),
                "component phase demolition requires an active existing baseline row");
            const auto& membership_id = owner->second.membership_entity_id;
            auto membership = memberships.find(membership_id);
            if (membership == memberships.end()) {
                const auto& entity = actual.at(membership_id);
                require(entity.required, "component phase demolition companion must be required");
                membership = memberships.emplace(membership_id,
                    AssemblyPhaseMembership::from_json(model(entity, assembly_phase_membership_entity_type))).first;
            }
            require(membership->second.registry_id() == owner->second.registry_id &&
                std::binary_search(membership->second.baseline_roots().begin(),
                    membership->second.baseline_roots().end(), root),
                "component phase demolition root must belong to its actual companion baseline");
            const auto& registry_id = membership->second.registry_id();
            auto registry = registries.find(registry_id);
            if (registry == registries.end())
                registry = registries.emplace(registry_id,
                    ModelPhases::from_json(model(actual.at(registry_id), "model_phases"))).first;
            require(registry->second.active_alternative().has_value(),
                "component phase demolition requires an actual saved alternative");
            additions[membership_id].push_back(root);
        }

        auto result = actual;
        for (const auto& [id, roots] : additions) {
            const auto& membership = memberships.at(id);
            const auto& alternative_id = *registries.at(membership.registry_id()).active_alternative();
            auto alternatives = membership.alternatives();
            auto alternative = std::find_if(alternatives.begin(), alternatives.end(),
                [&](const auto& row) { return row.alternative_id == alternative_id; });
            if (alternative == alternatives.end()) {
                require(alternatives.size() < 1000,
                    "component phase demolition resulting alternative budget exceeded");
                alternatives.push_back({alternative_id, {}, {}});
                alternative = std::prev(alternatives.end());
            }
            require(roots.size() <= 10000 - alternative->demolished_roots.size(),
                "component phase demolition resulting root budget exceeded");
            alternative->demolished_roots.insert(alternative->demolished_roots.end(), roots.begin(), roots.end());
            const auto proposed = AssemblyPhaseMembership::create(membership.registry_id(), membership.roots(),
                membership.baseline_roots(), std::move(alternatives));
            result.at(id).properties.at("model") =
                retain_assembly_phase_membership_source(model(actual.at(id), assembly_phase_membership_entity_type), proposed);
        }
        raw_budget.entities(result);
        admit_catalogs(result, catalog_budget);
        (void)resolve_assembly_phase_memberships(result);
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid component phase demolition replay: ") + error.what());
    }
}

} // namespace sketch
