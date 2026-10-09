#include "sketch/phase_stair_demolition.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/building_objects.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/vertical_levels.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include <Standard_Failure.hxx>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t target_limit = 1000, closure_limit = 65536, proof_limit = 1024 * 1024;
constexpr std::size_t source_node_limit = 4 * 1024 * 1024, source_string_limit = 64 * 1024 * 1024;
constexpr std::size_t phase_work_limit = 2000000, geometry_work_limit = 65536;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Stair/railing demolition: " + reason);
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

struct SourceBudget {
    std::size_t nodes{}, strings{};
    void text(const std::string& value) {
        if (value.size() > source_string_limit - strings) invalid("source string budget exceeded");
        strings += value.size();
    }
    void read(const Json& value) {
        std::vector<std::pair<const Json*, std::size_t>> pending{{&value, 0}};
        while (!pending.empty()) {
            const auto [node, depth] = pending.back(); pending.pop_back();
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
                    text(key); pending.emplace_back(&child, depth + 1);
                }
            } else for (const auto& child : *node) pending.emplace_back(&child, depth + 1);
        }
    }
};
void admit_source(const Entities& source) {
    if (source.size() > closure_limit) invalid("source entity budget exceeded");
    SourceBudget budget;
    std::size_t phase_work = 0;
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
        if (entity.type != "model_phases") continue;
        const auto model = field(entity.properties, "model");
        const auto members = model ? field(*model, "entity_ids") : nullptr;
        const auto alternatives = model ? field(*model, "alternatives") : nullptr;
        if (!members || !members->is_array() || members->size() > closure_limit ||
            !alternatives || !alternatives->is_array() || alternatives->size() > 4096)
            invalid("actual phase registry has no bounded supported inventory: " + id);
        const auto contexts = alternatives->size() + 1;
        if (members->size() > (phase_work_limit - phase_work) / contexts)
            invalid("aggregate phase state work budget exceeded");
        phase_work += members->size() * contexts;
    }
}

bool supported(const Entity& entity) {
    const auto version = field(entity.properties, "version"), form = field(entity.properties, "form");
    if (!version || !version->is_number_integer() || !form || !form->is_string()) return false;
    if (entity.type == "stair")
        return (*version == 1 && *form == "straight_stair_flight") ||
            ((*version == 2 || *version == 3 || *version == 4) && *form == "multi_flight_stair");
    return entity.type == "railing" &&
        ((*version == 1 && *form == "straight_railing") ||
         (*version == 2 && *form == "stair_flight_railing") ||
         (*version == 3 && *form == "stair_landing_railing"));
}
bool stair_or_rail(const Entity& entity) { return entity.type == "stair" || entity.type == "railing"; }
std::optional<std::string> actual_host(const Entity& entity) {
    if (entity.type != "railing" || !supported(entity)) return std::nullopt;
    const auto rail = decode_railing_properties(entity.id, entity.properties);
    if (rail.host) return rail.host->stair_id;
    if (rail.landing_host) return rail.landing_host->stair_id;
    return std::nullopt;
}

void admit_level_connection(const Entities& source, const ProjectOrganization& organization,
                            const StairFlight& stair) {
    if (!stair.level_connection) return;
    const auto& connection = *stair.level_connection;
    const auto& entity = source.at(stair.id);
    const auto& raw = entity.properties.at("level_connection");
    if (!raw.is_object() || !raw.contains("version") ||
        !raw.at("version").is_number_integer() || raw.at("version") != 1 ||
        !raw.contains("graph_id") || !raw.contains("link_id") ||
        !raw.contains("lower_level_id") || !raw.contains("upper_level_id"))
        invalid("stair level connection must use its closed supported version-one fields: " + stair.id);
    identity(connection.graph_entity_id);
    const auto graph = source.find(connection.graph_entity_id);
    if (graph == source.end() || graph->second.type != "vertical_levels")
        invalid("stair level connection requires its actual vertical-level graph: " + stair.id);
    const auto model = VerticalLevelGraph::from_json(graph->second.properties.at("model"));
    const auto link = std::find_if(model.links().begin(), model.links().end(),
        [&](const auto& value) { return value.id == connection.link_id; });
    if (link == model.links().end() || link->lower_level_id != connection.lower_level_id ||
        link->upper_level_id != connection.upper_level_id || link->state == RelationshipState::disconnected)
        invalid("stair level connection has a missing, disconnected or mismatched actual link: " + stair.id);
    if (std::abs(stair.total_rise - model.floor_to_floor_height(connection.link_id)) >
        VerticalLevelGraph::height_tolerance_m)
        invalid("stair rise differs from its actual floor-to-floor link: " + stair.id);
    const auto placement = field(entity.properties, "vertical_placement");
    if (!placement || placement->at("mode") != "level") return;
    const auto context = organization.drawing_context(stair.id);
    if (!context || context->floor_id.empty()) invalid("connected level stair requires an actual floor: " + stair.id);
    const auto binding = VerticalLevelBinding::from_json(
        source.at(context->floor_id).properties.at("vertical_level_binding"));
    if (binding.graph_entity_id != connection.graph_entity_id || binding.level_id != connection.lower_level_id)
        invalid("connected stair placement differs from its actual lower graph level: " + stair.id);
}

void admit_objects(const Entities& source, const Ids& affected) try {
    auto geometry_ids = affected;
    for (const auto& id : affected) if (const auto host = actual_host(source.at(id))) geometry_ids.insert(*host);
    const auto organization = organize_project(source);
    std::vector<std::string> independent_ids;
    for (const auto& id : geometry_ids) {
        const auto& entity = source.at(id);
        if (!supported(entity)) invalid("owner has no supported canonical physical form: " + id);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            invalid("owner has unresolved actual drawing context: " + id);
        if (!actual_host(entity)) independent_ids.push_back(id);
    }
    const auto resolved = resolve_vertical_placements(source, independent_ids);
    std::size_t geometry_work = 0;
    const auto count = [&](std::size_t work) {
        if (work > geometry_work_limit - geometry_work) invalid("affected native geometry work budget exceeded");
        geometry_work += work;
    };
    std::map<std::string, StairFlight, std::less<>> stairs;
    for (const auto& id : geometry_ids) if (source.at(id).type == "stair") {
        auto stair = decode_stair_properties(id, resolved.at(id).properties);
        count(stair.riser_count + stair.landings.size() + (stair.top_landing ? 1 : 0));
        admit_level_connection(source, organization, stair);
        stairs.emplace(id, std::move(stair));
    }
    // Bound all affected native work before constructing the first solid.
    std::map<std::string, Railing, std::less<>> rails;
    for (const auto& id : geometry_ids) if (source.at(id).type == "railing") {
        const auto host = actual_host(source.at(id));
        auto rail = decode_railing_properties(id, host ? source.at(id).properties : resolved.at(id).properties);
        if (host) count(derive_hosted_railing_layout(rail, stairs.at(*host)).posts.size() + 1);
        else count(static_cast<std::size_t>(std::ceil(rail.length / rail.post_spacing)) + 2);
        rails.emplace(id, std::move(rail));
    }
    for (const auto& [id, stair] : stairs) (void)make_stair_flight(stair);
    for (const auto& [id, rail] : rails) {
        const auto host = rail.host ? std::optional<std::string>{rail.host->stair_id} :
            (rail.landing_host ? std::optional<std::string>{rail.landing_host->stair_id} : std::nullopt);
        if (host) (void)make_hosted_railing(rail, stairs.at(*host));
        else (void)make_railing(rail);
    }
} catch (const Standard_Failure& error) {
    invalid(std::string("affected native geometry admission failed: ") + error.what());
}

void admit_supported_catalogs(const Entities& source, const Ids& affected) {
    AssemblyExpansionBudget budget;
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto model = field(entity.properties, "model");
        const auto schema = model ? field(*model, "schema") : nullptr;
        const auto instances = model ? field(*model, "instances") : nullptr;
        // Opaque/future envelopes never establish host authority from matching
        // text. Only supported catalog placement slots are interpreted.
        if (!schema || !schema->is_string() ||
            (*schema != "sketch.assemblies.v1" && *schema != "sketch.assemblies.v2" &&
             *schema != "sketch.assemblies.v3" && *schema != "sketch.assemblies.v4" &&
             *schema != "sketch.assemblies.v5" && *schema != "sketch.assemblies.v6" &&
             *schema != "sketch.assemblies.v7") || !instances || !instances->is_array()) continue;
        const bool participating = std::any_of(instances->begin(), instances->end(), [&](const Json& instance) {
            const auto placement = field(instance, "placement");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            return host && host->is_string() && affected.contains(host->get_ref<const std::string&>());
        });
        if (!participating) continue;
        try {
            const auto catalog = AssemblyModel::from_json(*model);
            for (const auto& instance : catalog.instances())
                if (instance.placement && affected.contains(instance.placement->host_entity_id))
                    (void)catalog.expand(instance, budget);
        } catch (const std::exception& error) {
            invalid("supported hosted assembly " + id + " is invalid in the actual source: " + error.what());
        }
    }
}

struct Derivation { StairDemolitionIntent intent; Entities entities; };
std::optional<Derivation> derive(const Entities& source, const std::vector<std::string>& selection) {
    if (selection.size() > target_limit) invalid("selection target budget exceeded");
    if (selection.empty()) return std::nullopt;
    admit_source(source);
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
        if (!stair_or_rail(found->second) || member == memberships.end()) { ++ordinary; continue; }
        const auto* registry = member->second;
        if (!models.contains(registry->registry_id)) models.emplace(registry->registry_id,
            ModelPhases::from_json(source.at(registry->registry_id).properties.at("model")));
        const auto& model = models.at(registry->registry_id);
        if (!registry->alternative_id ||
            !std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id)) { ++ordinary; continue; }
        if (!supported(found->second)) invalid("selected baseline owner has an unsupported physical form: " + id);
        const auto state = registry->states.find(id);
        if (model.active_alternative() != registry->alternative_id || state == registry->states.end() ||
            state->second != ModelPhase::existing) invalid("selected owner is not actual active shared baseline: " + id);
        if (destination && destination != registry) invalid("selected owners span different actual phase registries");
        destination = registry;
    }
    if (!destination) return std::nullopt;
    if (ordinary) invalid("select only active shared baseline stairs/railings from one saved registry and alternative");
    StairDemolitionIntent intent{destination->registry_id, *destination->alternative_id,
        std::vector<std::string>(selected.begin(), selected.end())};
    (void)encode_stair_demolition_intent(intent);
    validate_stair_attachment_state(source);
    const auto& phases = models.at(intent.registry_id);
    // Refuse unsupported physical members in the affected registry rather than
    // guessing whether an opaque future object carries attachment authority.
    for (const auto& id : destination->registered_entity_ids)
        if (stair_or_rail(source.at(id)) && !supported(source.at(id)))
            invalid("affected registry contains an unsupported stair/railing form: " + id);
    Ids affected = selected, append = selected;
    for (const auto& [id, entity] : source) {
        const auto host = actual_host(entity);
        if (!host || !selected.contains(*host) || source.at(*host).type != "stair") continue;
        const auto member = memberships.find(id);
        if (member == memberships.end() || member->second != destination)
            invalid("hosted railing lacks its stair's unambiguous actual registry: " + id);
        const auto state = destination->states.find(id);
        if (state == destination->states.end()) continue; // Proposed only in another saved alternative.
        if (state->second == ModelPhase::proposed)
            invalid("proposed hosted railing " + id + " must be retired before demolishing baseline stair " + *host);
        if (!std::binary_search(phases.baseline_ids().begin(), phases.baseline_ids().end(), id))
            invalid("participating hosted railing has no actual shared baseline membership: " + id);
        affected.insert(id);
        if (state->second == ModelPhase::existing) append.insert(id);
    }
    if (affected.size() > closure_limit) invalid("actual hosted closure budget exceeded");
    admit_objects(source, affected);
    admit_supported_catalogs(source, affected);

    const auto alternative = std::find_if(phases.alternatives().begin(), phases.alternatives().end(),
        [&](const auto& value) { return value.id == intent.alternative_id; });
    if (alternative == phases.alternatives().end()) invalid("actual saved active alternative is missing");
    auto amended = *alternative;
    amended.demolished_ids.insert(amended.demolished_ids.end(), append.begin(), append.end());
    const auto expected = phases.with_updated_alternative(std::move(amended)).to_json();
    const auto& original = source.at(intent.registry_id);
    auto raw = original.properties.at("model");
    bool appended = false;
    for (auto& row : raw.at("alternatives")) if (row.at("id") == intent.alternative_id) {
        for (const auto& id : append) row.at("demolished_ids").push_back(id);
        appended = true;
    }
    if (!appended || ModelPhases::from_json(raw).to_json() != expected)
        invalid("exact raw demolition append differs from its typed alternative update");
    auto result = source;
    result.at(intent.registry_id).properties.at("model") = std::move(raw);
    validate_stair_attachment_state(result);
    const auto after_scope = constraint_phase_scope(result);
    auto expected_inactive = scope.inactive_owner_ids;
    expected_inactive.insert(append.begin(), append.end());
    if (after_scope.inactive_owner_ids != expected_inactive)
        invalid("registry append did not exclusively park the actual stair/railing closure");
    return Derivation{std::move(intent), std::move(result)};
}
} // namespace

nlohmann::json encode_stair_demolition_intent(const StairDemolitionIntent& intent) {
    identity(intent.registry_id); identity(intent.alternative_id);
    const auto& ids = intent.selected_object_ids;
    if (ids.empty() || ids.size() > target_limit || !std::is_sorted(ids.begin(), ids.end()) ||
        std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        invalid("proof selected inventory must be nonempty, unique and sorted");
    for (const auto& id : ids) identity(id);
    Json result{{"version", 1}, {"registry_id", intent.registry_id},
        {"alternative_id", intent.alternative_id}, {"selected_object_ids", ids}};
    if (result.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    return result;
}
StairDemolitionIntent decode_stair_demolition_intent(const nlohmann::json& value) {
    try {
        if (!value.is_object() || value.size() != 4 || !value.contains("version") ||
            !value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.contains("registry_id") || !value.contains("alternative_id") ||
            !value.contains("selected_object_ids") || !value.at("selected_object_ids").is_array() ||
            value.at("selected_object_ids").empty() || value.at("selected_object_ids").size() > target_limit)
            invalid("proof must contain exactly the four bounded version-one fields");
        StairDemolitionIntent intent{identity(value.at("registry_id")), identity(value.at("alternative_id")), {}};
        for (const auto& id : value.at("selected_object_ids")) intent.selected_object_ids.push_back(identity(id));
        if (value.dump().size() > proof_limit || encode_stair_demolition_intent(intent) != value)
            invalid("proof is not canonical or exceeds its byte budget");
        return intent;
    } catch (const Json::exception& error) { invalid(std::string("malformed proof: ") + error.what()); }
}
std::optional<StairDemolitionIntent> phase_stair_demolition_request(
    const Entities& source, const std::vector<std::string>& selection) {
    try {
        const auto derived = derive(source, selection);
        return derived ? std::optional<StairDemolitionIntent>{derived->intent} : std::nullopt;
    } catch (const Json::exception& error) { invalid(std::string("malformed actual source: ") + error.what()); }
}
Entities replay_phase_stair_demolition_entities(const Entities& source, const StairDemolitionIntent& intent) {
    try {
        (void)encode_stair_demolition_intent(intent);
        auto derived = derive(source, intent.selected_object_ids);
        if (!derived || derived->intent != intent) invalid("proof differs from actual saved active baseline membership");
        return std::move(derived->entities);
    } catch (const Json::exception& error) { invalid(std::string("malformed actual replay source: ") + error.what()); }
}
} // namespace sketch
