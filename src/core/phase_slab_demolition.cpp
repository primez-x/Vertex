#include "sketch/phase_slab_demolition.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t target_limit = 1000, proof_limit = 1024 * 1024;
constexpr std::size_t entity_limit = 65536, source_node_limit = 4 * 1024 * 1024;
constexpr std::size_t source_string_limit = 64 * 1024 * 1024;
constexpr std::size_t slab_segment_limit = 4096, aggregate_segment_limit = 65536;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Slab demolition: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("proof identity must be a string");
    const auto& id = value.get_ref<const std::string&>();
    identity(id);
    return id;
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}

// Bound actual source data before recursive codecs, normalization or native
// geometry admission. Opaque strings are counted, never interpreted as links.
struct SourceBudget {
    std::size_t nodes{}, strings{};
    void text(const std::string& value) {
        if (value.size() > source_string_limit - strings) invalid("source string budget exceeded");
        strings += value.size();
    }
    void read(const Json& value) {
        std::vector<std::pair<const Json*, std::size_t>> pending{{&value, 0}};
        while (!pending.empty()) {
            const auto [node, depth] = pending.back();
            pending.pop_back();
            if (depth > 64 || ++nodes > source_node_limit) invalid("source JSON node/nesting budget exceeded");
            if (node->is_string()) text(node->get_ref<const std::string&>());
            if (node->is_binary()) {
                if (node->get_binary().size() > source_string_limit - strings)
                    invalid("source binary payload budget exceeded");
                strings += node->get_binary().size();
            }
            if (!node->is_structured()) continue;
            if (node->size() > source_node_limit - nodes ||
                pending.size() > source_node_limit - nodes - node->size())
                invalid("source JSON pending-node budget exceeded");
            if (node->is_object()) {
                for (const auto& [key, child] : node->items()) {
                    text(key);
                    pending.emplace_back(&child, depth + 1);
                }
            } else for (const auto& child : *node) pending.emplace_back(&child, depth + 1);
        }
    }
};
void admit_source_bounds(const Entities& source) {
    if (source.size() > entity_limit) invalid("source entity budget exceeded");
    SourceBudget budget;
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
    }
}
void admit_footprint_bounds(const Entity& entity, std::size_t& aggregate_segments) {
    const auto boundary = field(entity.properties, "boundary"), holes = field(entity.properties, "holes");
    if (!boundary || !boundary->is_array() || boundary->empty() || !holes || !holes->is_array() ||
        holes->size() > 1024) invalid("slab has no bounded actual footprint: " + entity.id);
    std::size_t segments = 0;
    const auto count = [&](const Json& edges) {
        if (!edges.is_array() || edges.empty() || edges.size() > slab_segment_limit - segments)
            invalid("slab footprint segment budget exceeded: " + entity.id);
        segments += edges.size();
    };
    count(*boundary);
    for (const auto& hole : *holes) count(hole);
    if (segments > aggregate_segment_limit - aggregate_segments)
        invalid("aggregate slab footprint segment budget exceeded");
    aggregate_segments += segments;
}
Slab actual_slab(const Entity& entity) {
    if (entity.type != "slab") invalid("actual source owner is not a slab: " + entity.id);
    Slab result;
    std::string diagnostic;
    if (!read_document_slab(entity, result, diagnostic)) invalid("slab " + entity.id + ": " + diagnostic);
    return result;
}

void admit_slabs(const Entities& source, const Ids& targets) {
    const auto organization = organize_project(source);
    const std::vector<std::string> ids(targets.begin(), targets.end());
    const auto resolved = resolve_vertical_placements(source, ids);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    const auto material = [&](const std::string& catalog_id, const std::string& material_id) {
        identity(catalog_id); identity(material_id);
        const auto catalog = source.find(catalog_id);
        if (catalog == source.end() || catalog->second.type != "assembly_model")
            invalid("slab material must bind an actual assembly catalog: " + catalog_id);
        if (!catalogs.contains(catalog_id)) catalogs.emplace(catalog_id,
            AssemblyModel::from_json(catalog->second.properties.at("model")));
        const auto& materials = catalogs.at(catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& value) { return value.id == material_id; }))
            invalid("slab material is absent from its actual catalog: " + material_id);
    };
    std::size_t aggregate_segments = 0;
    for (const auto& id : targets) {
        const auto& entity = source.at(id);
        admit_footprint_bounds(entity, aggregate_segments);
        validate_slab_profile_source_entity(entity);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            invalid("slab has unresolved actual drawing context: " + id);
        // The local profile owns scalar aliases and receipts. Resolved elevation
        // is a derived coordinate and intentionally leaves legacy aliases intact.
        const auto slab = actual_slab(resolved.at(id));
        (void)make_slab(slab);
        if (const auto assignment = field(entity.properties, "material_assignment")) {
            if (!assignment->is_object() || !assignment->contains("version") ||
                !assignment->at("version").is_number_integer() || assignment->at("version") != 1)
                invalid("unsupported slab material assignment: " + id);
            material(identity(assignment->at("catalog_id")), identity(assignment->at("material_id")));
        }
        for (const auto& layer : slab.layers) if (layer.material)
            material(layer.material->catalog_id, layer.material->material_id);
    }
}

void admit_supported_dependencies(const Entities& source, const Ids& targets) {
    AssemblyExpansionBudget budget;
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto model = field(entity.properties, "model");
        if (!model) continue;
        const auto schema = field(*model, "schema"), instances = field(*model, "instances");
        // Only actual supported catalog slots establish host authority. Future
        // records and arbitrary opaque values are retained without interpretation.
        if (!schema || !schema->is_string() ||
            (*schema != "sketch.assemblies.v1" && *schema != "sketch.assemblies.v2" &&
             *schema != "sketch.assemblies.v3" && *schema != "sketch.assemblies.v4") ||
            !instances || !instances->is_array()) continue;
        const bool affected = std::any_of(instances->begin(), instances->end(), [&](const Json& instance) {
            const auto placement = field(instance, "placement");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            return host && host->is_string() && targets.contains(host->get_ref<const std::string&>());
        });
        if (!affected) continue;
        try {
            const auto catalog = AssemblyModel::from_json(*model);
            for (const auto& instance : catalog.instances()) {
                if (!instance.placement || !targets.contains(instance.placement->host_entity_id)) continue;
                (void)catalog.expand(instance, budget);
                // Native publication, saved views and schedules scope both
                // host-copy and authored-profile instances by their actual
                // placement host. Retain that qualified placement verbatim.
            }
        } catch (const std::exception& error) {
            invalid("supported slab-hosted assembly " + id + " is invalid in the active source: " + error.what());
        }
    }
    // The supported opening, join and stair attachment codecs bind walls,
    // roofs and stairs. A text field named slab_id/host_id is not physical
    // dependency authority and requires no rewrite here.
}

struct DemolitionDerivation { SlabDemolitionIntent intent; Entities entities; };
std::optional<DemolitionDerivation> derive(const Entities& source, const std::vector<std::string>& selection) {
    if (selection.size() > target_limit) invalid("selection target budget exceeded");
    if (selection.empty()) return std::nullopt;
    admit_source_bounds(source);
    const auto scope = constraint_phase_scope(source);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> memberships;
    for (const auto& registry : scope.registries)
        for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, &registry).second) invalid("overlapping actual phase ownership: " + id);
    std::map<std::string, ModelPhases, std::less<>> models;
    Ids selected;
    const PhysicalWallPhaseState* destination = nullptr;
    std::size_t ordinary = 0;
    for (const auto& id : selection) {
        identity(id);
        if (!selected.insert(id).second) invalid("selection contains duplicate identity: " + id);
        const auto found = source.find(id);
        if (found == source.end()) invalid("selection contains missing actual owner: " + id);
        if (scope.inactive_owner_ids.contains(id)) invalid("selected owner is inactive in the saved design: " + id);
        const auto member = memberships.find(id);
        if (found->second.type != "slab" || member == memberships.end()) { ++ordinary; continue; }
        const auto* registry = member->second;
        if (!models.contains(registry->registry_id)) models.emplace(registry->registry_id,
            ModelPhases::from_json(source.at(registry->registry_id).properties.at("model")));
        const auto& model = models.at(registry->registry_id);
        if (!registry->alternative_id ||
            !std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id)) { ++ordinary; continue; }
        const auto state = registry->states.find(id);
        if (model.active_alternative() != registry->alternative_id || state == registry->states.end() ||
            state->second != ModelPhase::existing) invalid("selected slab is not actual active shared baseline: " + id);
        if (destination && destination != registry) invalid("selected slabs span different actual phase registries");
        destination = registry;
    }
    if (!destination) return std::nullopt;
    if (ordinary) invalid("select only active shared baseline slabs from one saved registry and alternative");
    SlabDemolitionIntent intent{destination->registry_id, *destination->alternative_id,
        std::vector<std::string>(selected.begin(), selected.end())};
    (void)encode_slab_demolition_intent(intent);
    admit_slabs(source, selected);
    admit_supported_dependencies(source, selected);

    const auto& original = source.at(intent.registry_id);
    const auto phases = ModelPhases::from_json(original.properties.at("model"));
    const auto alternative = std::find_if(phases.alternatives().begin(), phases.alternatives().end(),
        [&](const auto& value) { return value.id == intent.alternative_id; });
    if (alternative == phases.alternatives().end()) invalid("actual saved active alternative is missing");
    auto amended = *alternative;
    amended.demolished_ids.insert(amended.demolished_ids.end(), intent.slab_ids.begin(), intent.slab_ids.end());
    const auto expected = phases.with_updated_alternative(std::move(amended)).to_json();
    auto raw = original.properties.at("model");
    bool appended = false;
    for (auto& row : raw.at("alternatives")) if (row.at("id") == intent.alternative_id) {
        for (const auto& id : intent.slab_ids) row.at("demolished_ids").push_back(id);
        appended = true;
    }
    if (!appended || ModelPhases::from_json(raw).to_json() != expected)
        invalid("exact raw demolition append differs from its typed alternative update");
    auto result = source;
    result.at(intent.registry_id).properties.at("model") = std::move(raw);
    const auto after_scope = constraint_phase_scope(result);
    auto expected_inactive = scope.inactive_owner_ids;
    expected_inactive.insert(selected.begin(), selected.end());
    if (after_scope.inactive_owner_ids != expected_inactive)
        invalid("registry append did not exclusively park the selected actual slabs");
    return DemolitionDerivation{std::move(intent), std::move(result)};
}
} // namespace

nlohmann::json encode_slab_demolition_intent(const SlabDemolitionIntent& intent) {
    identity(intent.registry_id); identity(intent.alternative_id);
    if (intent.slab_ids.empty() || intent.slab_ids.size() > target_limit ||
        !std::is_sorted(intent.slab_ids.begin(), intent.slab_ids.end()) ||
        std::adjacent_find(intent.slab_ids.begin(), intent.slab_ids.end()) != intent.slab_ids.end())
        invalid("proof slab inventory must be nonempty, unique and sorted");
    for (const auto& id : intent.slab_ids) identity(id);
    Json result{{"version", 1}, {"registry_id", intent.registry_id},
        {"alternative_id", intent.alternative_id}, {"slab_ids", intent.slab_ids}};
    if (result.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    return result;
}

SlabDemolitionIntent decode_slab_demolition_intent(const nlohmann::json& value) {
    try {
        if (!value.is_object() || value.size() != 4 || !value.contains("version") ||
            !value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.contains("registry_id") || !value.contains("alternative_id") ||
            !value.contains("slab_ids") || !value.at("slab_ids").is_array() ||
            value.at("slab_ids").empty() || value.at("slab_ids").size() > target_limit)
            invalid("proof must contain exactly the four bounded version-one fields");
        SlabDemolitionIntent intent{identity(value.at("registry_id")), identity(value.at("alternative_id")), {}};
        for (const auto& id : value.at("slab_ids")) intent.slab_ids.push_back(identity(id));
        // Shape/string bounds precede serialization, so hostile nesting cannot
        // bypass the strict envelope or cause unbounded proof serialization.
        if (value.dump().size() > proof_limit || encode_slab_demolition_intent(intent) != value)
            invalid("proof is not canonical or exceeds its byte budget");
        return intent;
    } catch (const Json::exception& error) { invalid(std::string("malformed proof: ") + error.what()); }
}

std::optional<SlabDemolitionIntent> phase_slab_demolition_request(
    const Entities& source, const std::vector<std::string>& selection) {
    try {
        const auto derived = derive(source, selection);
        return derived ? std::optional<SlabDemolitionIntent>{derived->intent} : std::nullopt;
    } catch (const Json::exception& error) { invalid(std::string("malformed actual source: ") + error.what()); }
}

Entities replay_phase_slab_demolition_entities(const Entities& source, const SlabDemolitionIntent& intent) {
    try {
        (void)encode_slab_demolition_intent(intent);
        auto derived = derive(source, intent.slab_ids);
        if (!derived || derived->intent != intent) invalid("proof differs from actual saved active baseline membership");
        return std::move(derived->entities);
    } catch (const Json::exception& error) { invalid(std::string("malformed actual replay source: ") + error.what()); }
}

} // namespace sketch
