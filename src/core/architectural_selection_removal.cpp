#include "sketch/architectural_selection_removal.hpp"

#include "sketch/architectural_object_removal.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/mixed_wall_removal.hpp"
#include "sketch/phase_constraint_authoring.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = RoofRemovalEntities;
using Keys = std::set<std::pair<std::string, std::string>>;
constexpr std::size_t entity_limit = 65536, selection_limit = 1000, identity_limit = 4096;
constexpr std::size_t byte_limit = 64 * 1024 * 1024, node_limit = 4 * 1024 * 1024;
constexpr std::size_t phase_limit = 2000000;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Architectural selection removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
struct Budget {
    std::size_t nodes{}, bytes{}, serialized{};
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("source string/key budget exceeded");
        bytes += value.size();
    }
    void reserve(std::size_t count) {
        if (count > byte_limit - serialized) reject("serialized source map byte budget exceeded");
        serialized += count;
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) reject("source JSON node/nesting budget exceeded");
        if (value.is_binary() || value.is_discarded()) reject("source JSON cannot be binary or discarded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("source scalar must be finite");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        } else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
};
// These aggregate inventories precede roof inspection, which itself performs
// native admission. No producer receives a shortened or fabricated source.
void bounds(const Entities& source) {
    if (source.size() > entity_limit) reject("source entity budget exceeded");
    Budget budget;
    std::size_t phase_work{}, catalog_rows{};
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
        budget.reserve(2 * id.size() + Json(entity.type).dump().size() + 128);
        budget.reserve(entity.properties.dump().size()); budget.reserve(entity.extensions.dump().size());
        if (entity.type == "model_phases") {
            const auto& model = entity.properties.at("model");
            const auto& members = model.at("entity_ids");
            const auto& alternatives = model.at("alternatives");
            if (!members.is_array() || members.size() > entity_limit ||
                !alternatives.is_array() || alternatives.size() > identity_limit)
                reject("phase registry lacks bounded actual inventory: " + id);
            if (members.size() > (phase_limit - phase_work) / (alternatives.size() + 1))
                reject("phase state work budget exceeded");
            phase_work += members.size() * (alternatives.size() + 1);
        } else if (entity.type == "assembly_model") {
            const auto& model = entity.properties.at("model");
            for (const auto* key : {"materials", "types", "instances"}) {
                const auto& rows = model.at(key);
                if (!rows.is_array() || rows.size() > entity_limit - catalog_rows)
                    reject("catalog lacks bounded actual inventory: " + id);
                catalog_rows += rows.size();
            }
        }
    }
}
bool primitive(const Entity& entity) {
    return entity.type == "stair" || entity.type == "railing" || entity.type == "slab" ||
        entity.type == "column" || entity.type == "beam";
}
void additional_bounds(const ArchitecturalSelectionRemovalIntent& intent) {
    if (intent.roof_additional_identities.size() > identity_limit)
        reject("roof destination key budget exceeded");
    std::size_t count = intent.object_ids.size();
    std::set<std::string, std::less<>> fresh;
    for (const auto& [old_id, destinations] : intent.roof_additional_identities) {
        identity(old_id);
        if (destinations.empty() || destinations.size() > identity_limit - count)
            reject("roof destinations require bounded nonempty fresh identity arrays");
        count += destinations.size();
        for (const auto& id : destinations) {
            identity(id);
            if (!fresh.insert(id).second) reject("duplicate fresh roof destination: " + id);
        }
    }
}
EmbeddedAssemblyPresentationIds expected_aliases(const Entities& source, const std::vector<Entities>& candidates) {
    const auto original = embedded_assembly_presentation_ids(source);
    auto expected = original;
    for (const auto& candidate : candidates) {
        const auto remaining = embedded_assembly_presentation_ids(candidate);
        for (const auto& [key, alias] : remaining) {
            const auto before = original.find(key);
            if (before == original.end() || before->second != alias)
                reject("removal changed a surviving computed component alias: " + key.first + "/" + key.second);
        }
        for (const auto& [key, alias] : original) {
            (void)alias;
            if (!remaining.contains(key)) expected.erase(key);
        }
    }
    return expected;
}
} // namespace

Entities replay_architectural_selection_removal(const Entities& actual,
    const ArchitecturalSelectionRemovalIntent& intent, bool allow_manufactured_opening_hosts,
    bool complete_roof_hosted_catalog_consequences, bool complete_wall_hosted_catalog_consequences,
    bool complete_placed_catalog_consequences) {
    try {
        bounds(actual);
        if (intent.object_ids.size() > selection_limit ||
            intent.components.size() > selection_limit - intent.object_ids.size() ||
            (intent.object_ids.empty() && intent.components.empty()))
            reject("requires 1..1000 aggregate actual roots/components");
        additional_bounds(intent);
        std::set<std::string, std::less<>> unique;
        std::vector<std::string> roofs, objects;
        for (const auto& id : intent.object_ids) {
            identity(id);
            if (!unique.insert(id).second) reject("duplicate selected actual owner: " + id);
            const auto found = actual.find(id);
            if (found == actual.end()) reject("selected actual owner is missing: " + id);
            if (found->second.type == "roof") roofs.push_back(id);
            else if (primitive(found->second)) objects.push_back(id);
            else reject("selected root requires a dedicated removal path: " + id);
        }
        Keys selected_components;
        for (const auto& key : intent.components) {
            identity(key.first); identity(key.second);
            if (!selected_components.insert(key).second) reject("duplicate qualified component selection");
        }
        std::sort(roofs.begin(), roofs.end()); std::sort(objects.begin(), objects.end());
        if (roofs.empty() && !intent.roof_additional_identities.empty())
            reject("roof destinations require an actual selected roof");
        const bool complete_generic_catalog = complete_wall_hosted_catalog_consequences || complete_placed_catalog_consequences;
        const bool complete_composition_catalog = complete_roof_hosted_catalog_consequences || complete_generic_catalog;

        if (allow_manufactured_opening_hosts) {
            // Reserve the complete source's conservative shared 16-pass roof,
            // host and component inventory before any native inspector/factory.
            validate_mixed_wall_removal_source_admission(actual, true);
            if (!objects.empty() || !selected_components.empty())
                preflight_architectural_object_removal(actual, objects, intent.components, 0,
                    complete_generic_catalog,
                    complete_placed_catalog_consequences);
        }
        std::vector<Entities> candidates;
        if (!roofs.empty()) {
            const auto plan = inspect_roof_removal_plan(actual, roofs, true, true, true,
                complete_roof_hosted_catalog_consequences);
            if (!plan.ready()) {
                for (const auto& row : plan.diagnostics) if (row.blocking)
                    reject(row.entity_id + ": " + row.reason);
                reject("actual roof removal plan is not ready");
            }
            for (const auto& key : plan.retired_hosted_component_keys) selected_components.erase(key);
            auto roof = replay_roof_removal(actual, roofs, intent.roof_additional_identities, true, true, true,
                complete_roof_hosted_catalog_consequences);
            bounds(roof.entities);
            candidates.push_back(std::move(roof.entities));
        }
        if (!objects.empty() || !selected_components.empty()) {
            const std::vector<std::pair<std::string, std::string>> components(
                selected_components.begin(), selected_components.end());
            auto ordinary = allow_manufactured_opening_hosts ?
                replay_architectural_object_removal(actual, objects, components, true, 0,
                    complete_generic_catalog,
                    complete_placed_catalog_consequences) :
                replay_architectural_object_removal(actual, objects, components, false, 0,
                    complete_generic_catalog,
                    complete_placed_catalog_consequences);
            bounds(ordinary);
            candidates.push_back(std::move(ordinary));
        }
        if (candidates.empty()) reject("selection has no admitted removal lane");
        const auto aliases = expected_aliases(actual, candidates);
        // Derive the union before moving the sole admitted lane. Every original
        // inactive owner must remain inactive, and only admitted lane changes
        // may add inactive owners through exact physical roof retention.
        const auto before_scope = constraint_phase_scope(actual);
        auto expected_inactive = before_scope.inactive_owner_ids;
        for (const auto& candidate : candidates) {
            const auto scope = constraint_phase_scope(candidate);
            expected_inactive.insert(scope.inactive_owner_ids.begin(), scope.inactive_owner_ids.end());
        }
        auto result = candidates.size() == 1 ? std::move(candidates.front()) :
            compose_ordinary_architectural_removal_candidates(actual, candidates,
                complete_composition_catalog, complete_composition_catalog);
        bounds(result);
        validate_document_assembly_instances(result);
        if (embedded_assembly_presentation_ids(result) != aliases)
            reject("composition lost, resurrected or changed a computed component alias");
        const auto after_scope = constraint_phase_scope(result);
        if (after_scope.inactive_owner_ids != expected_inactive)
            reject("composition changed protected inactive ownership");
        return result;
    } catch (const Json::exception& error) {
        reject(std::string("malformed actual source/selection: ") + error.what());
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        reject(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
} // namespace sketch
