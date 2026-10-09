#include "sketch/roof_join_phase_ownership.hpp"

#include "sketch/document.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/roof_join_semantics.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_alternatives = 1024;
constexpr std::size_t maximum_references = 1024 * 1024;
constexpr std::size_t maximum_string_bytes = 64 * 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Roof join ownership: " + reason);
}

bool reference_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        });
}

std::optional<std::string> qualifier(const Entity& entity) {
    if (entity.type != "roof_join") return std::nullopt;
    const auto found = entity.extensions.find(roof_join_phase_ownership_extension_key);
    if (found == entity.extensions.end()) return std::nullopt;
    const auto& value = *found;
    if (!value.is_object() || !value.contains("version") ||
        !value.at("version").is_number_integer())
        reject("phase qualifier requires an integer version");
    // Future envelopes are deliberately not decoded or granted authority.
    if (value.at("version") > 1) return std::nullopt;
    if (value.at("version") != 1 || value.size() != 2 ||
        !value.contains("registry_id") || !value.at("registry_id").is_string())
        reject("phase qualifier must contain exactly version 1 and registry_id");
    const auto& id = value.at("registry_id").get_ref<const std::string&>();
    if (!reference_id(id)) reject("phase qualifier registry_id must be a bounded ASCII identifier");
    return id;
}

struct PhaseBudget {
    std::size_t references{};
    std::size_t alternatives{};
    std::size_t string_bytes{};

    void string(const Json& value) {
        if (!value.is_string()) reject("phase registry strings must be strings");
        const auto size = value.get_ref<const std::string&>().size();
        if (size > maximum_string_bytes - string_bytes)
            reject("phase registry string budget exceeded");
        string_bytes += size;
    }

    void ids(const Json& value, std::size_t& registry_references) {
        if (!value.is_array() || value.size() > maximum_entities)
            reject("phase registry roster budget exceeded");
        if (value.size() > maximum_references - registry_references ||
            value.size() > maximum_references - references)
            reject("phase registry reference budget exceeded");
        registry_references += value.size();
        references += value.size();
        for (const auto& id : value) string(id);
    }

    void preflight(const Json& value) {
        if (!value.is_object() || !value.contains("entity_ids") ||
            !value.contains("baseline_ids") || !value.contains("alternatives") ||
            !value.at("alternatives").is_array())
            reject("invalid phase registry rosters");
        const auto& entries = value.at("alternatives");
        if (entries.size() > maximum_alternatives ||
            entries.size() > maximum_entities - alternatives)
            reject("phase registry alternative budget exceeded");
        alternatives += entries.size();
        std::size_t registry_references{};
        ids(value.at("entity_ids"), registry_references);
        ids(value.at("baseline_ids"), registry_references);
        for (const auto& entry : entries) {
            if (!entry.is_object() || !entry.contains("id") || !entry.contains("name") ||
                !entry.contains("demolished_ids") || !entry.contains("proposed_ids"))
                reject("invalid phase registry alternative");
            string(entry.at("id"));
            string(entry.at("name"));
            ids(entry.at("demolished_ids"), registry_references);
            ids(entry.at("proposed_ids"), registry_references);
        }
        if (value.contains("active_alternative") && !value.at("active_alternative").is_null())
            string(value.at("active_alternative"));
    }
};

struct Membership {
    std::string registry_id;
    std::size_t count{};
    bool baseline{};
    std::optional<std::size_t> proposal;
    std::set<std::size_t> demolition_alternatives;
};
using Memberships = std::map<std::string, Membership, std::less<>>;

Memberships registry_memberships(const std::map<std::string, Entity, std::less<>>& entities) {
    Memberships memberships;
    PhaseBudget budget;
    for (const auto& [registry_id, registry] : entities) {
        if (registry.type != "model_phases") continue;
        if (!registry.properties.is_object() || !registry.properties.contains("model"))
            reject("phase registry must contain an actual model");
        const auto& encoded = registry.properties.at("model");
        budget.preflight(encoded);
        const auto model = ModelPhases::from_json(encoded);
        for (const auto& id : model.entity_ids()) {
            const auto actual = entities.find(id);
            if (actual == entities.end() || !is_model_phase_entity_type(actual->second.type))
                reject("phase registry member must be an actual architectural entity: " + id);
            if (actual->second.type != "roof_join") continue;
            auto& membership = memberships[id];
            if (membership.count++ == 0) membership.registry_id = registry_id;
        }
        const auto own = [&](const std::string& id) -> Membership* {
            const auto found = memberships.find(id);
            if (found == memberships.end() || found->second.count != 1 ||
                found->second.registry_id != registry_id) return nullptr;
            return &found->second;
        };
        for (const auto& id : model.baseline_ids())
            if (auto* membership = own(id)) membership->baseline = true;
        for (std::size_t index = 0; index < model.alternatives().size(); ++index) {
            const auto& alternative = model.alternatives()[index];
            for (const auto& id : alternative.demolished_ids)
                if (auto* membership = own(id)) membership->demolition_alternatives.insert(index);
            for (const auto& id : alternative.proposed_ids)
                if (auto* membership = own(id)) membership->proposal = index;
        }
    }
    return memberships;
}

bool mutually_exclusive(const Membership& left, const Membership& right) {
    // ModelPhases assigns every nonbaseline record to exactly one proposal.
    // Baseline records coexist in baseline, regardless of demolition elsewhere.
    if (left.baseline && right.baseline) return false;
    if (!left.baseline && !right.baseline)
        return left.proposal && right.proposal && left.proposal != right.proposal;
    const auto& baseline = left.baseline ? left : right;
    const auto& proposal = left.baseline ? right : left;
    return proposal.proposal && baseline.demolition_alternatives.contains(*proposal.proposal);
}

struct JoinRecord {
    RoofJoin join;
    std::optional<std::string> registry_id;
};
}  // namespace

bool has_phase_qualified_roof_join_ownership(const Entity& entity) {
    return qualifier(entity).has_value();
}

std::set<std::string, std::less<>> phase_qualified_roof_join_cohort_ids(
    const std::map<std::string, Entity, std::less<>>& actual_entities) {
    std::vector<JoinRecord> joins;
    bool qualified{};
    std::size_t references{};
    for (const auto& [id, entity] : actual_entities) {
        if (id != entity.id) reject("actual entity map key differs from its entity ID");
        if (entity.type != "roof_join") continue;
        auto marker = qualifier(entity);
        qualified = qualified || marker.has_value();
        auto join = parse_roof_join(entity.properties, id);
        if (join.roof_ids.size() > maximum_references - references)
            reject("roof ownership reference budget exceeded");
        references += join.roof_ids.size();
        for (const auto& roof_id : join.roof_ids) {
            const auto roof = actual_entities.find(roof_id);
            if (roof == actual_entities.end() || roof->second.type != "roof")
                reject("join member must be an actual roof: " + roof_id);
        }
        joins.push_back({std::move(join), std::move(marker)});
    }
    if (qualified && joins.size() > maximum_entities) reject("qualified join inventory budget exceeded");
    const auto memberships = qualified ? registry_memberships(actual_entities) : Memberships{};
    std::set<std::string, std::less<>> qualified_cohort;
    for (const auto& record : joins) {
        if (!record.registry_id) continue;
        const auto registry = actual_entities.find(*record.registry_id);
        const auto membership = memberships.find(record.join.id);
        if (registry == actual_entities.end() || registry->second.type != "model_phases" ||
            membership == memberships.end() || membership->second.count != 1 ||
            membership->second.registry_id != *record.registry_id)
            reject("qualified join must belong uniquely to its actual phase registry: " + record.join.id);
        qualified_cohort.insert(record.join.id);
    }
    std::map<std::string, std::vector<std::size_t>, std::less<>> owners;
    std::set<std::pair<std::size_t, std::size_t>> checked_pairs;
    std::size_t comparisons{};
    for (std::size_t index = 0; index < joins.size(); ++index) {
        const auto& current = joins[index];
        for (const auto& roof_id : current.join.roof_ids) {
            auto& previous_owners = owners[roof_id];
            for (const auto previous_index : previous_owners) {
                if (++comparisons > maximum_references) reject("roof ownership pair budget exceeded");
                if (!checked_pairs.emplace(previous_index, index).second) continue;
                const auto& previous = joins[previous_index];
                const auto left = memberships.find(previous.join.id);
                const auto right = memberships.find(current.join.id);
                if ((!previous.registry_id && !current.registry_id) ||
                    left == memberships.end() || right == memberships.end() ||
                    left->second.count != 1 || right->second.count != 1 ||
                    left->second.registry_id != right->second.registry_id ||
                    !mutually_exclusive(left->second, right->second))
                    reject("roof " + roof_id + " belongs to coactive or unqualified joins: " +
                           previous.join.id + " and " + current.join.id);
                qualified_cohort.insert(previous.join.id);
                qualified_cohort.insert(current.join.id);
            }
            previous_owners.push_back(index);
        }
    }
    return qualified_cohort;
}

void validate_roof_join_ownership(
    const std::map<std::string, Entity, std::less<>>& actual_entities) {
    (void)phase_qualified_roof_join_cohort_ids(actual_entities);
}

}  // namespace sketch
