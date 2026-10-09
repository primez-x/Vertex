#include "sketch/phase_stair_demolition_retirement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/vertical_levels.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using InstanceKeys = std::set<std::pair<std::string, std::string>>;
constexpr std::size_t entity_limit = 65536, target_limit = 1000, retirement_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024, node_limit = 4 * 1024 * 1024;
constexpr std::size_t string_limit = 64 * 1024 * 1024, phase_limit = 2000000;
constexpr std::size_t geometry_limit = 65536;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Stair demolition proposal retirement: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("proof identity must be a string");
    const auto& id = value.get_ref<const std::string&>(); identity(id); return id;
}
const Json* field(const Json& value, const char* name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name); return found == value.end() ? nullptr : &*found;
}
struct Budget {
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > string_limit - bytes) invalid("source string budget exceeded");
        bytes += value.size();
    }
    void read(const Json& root) {
        std::vector<std::pair<const Json*, std::size_t>> pending{{&root, 0}};
        while (!pending.empty()) {
            const auto [value, depth] = pending.back(); pending.pop_back();
            if (depth > 64 || ++nodes > node_limit) invalid("source JSON node/nesting budget exceeded");
            if (value->is_number_float() && !std::isfinite(value->get<double>())) invalid("source has nonfinite scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size() > string_limit - bytes) invalid("source binary budget exceeded");
                bytes += value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size() > node_limit - nodes || pending.size() > node_limit - nodes - value->size())
                invalid("source JSON pending-node budget exceeded");
            if (value->is_object()) for (const auto& [key, child] : value->items()) {
                text(key); pending.emplace_back(&child, depth + 1);
            } else for (const auto& child : *value) pending.emplace_back(&child, depth + 1);
        }
    }
};
void source_bounds(const Entities& source) {
    if (source.size() > entity_limit) invalid("source entity budget exceeded");
    Budget budget; std::size_t phase_work{};
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type); budget.read(entity.properties); budget.read(entity.extensions);
        if (entity.type != "model_phases") continue;
        const auto model = field(entity.properties, "model");
        const auto members = model ? field(*model, "entity_ids") : nullptr;
        const auto alternatives = model ? field(*model, "alternatives") : nullptr;
        if (!members || !members->is_array() || members->size() > entity_limit ||
            !alternatives || !alternatives->is_array() || alternatives->size() > retirement_limit)
            invalid("actual phase registry has no bounded supported inventory: " + id);
        const auto contexts = alternatives->size() + 1;
        if (members->size() > (phase_limit - phase_work) / contexts) invalid("phase state work budget exceeded");
        phase_work += members->size() * contexts;
    }
}
bool supported(const Entity& entity) {
    const auto version = field(entity.properties, "version"), form = field(entity.properties, "form");
    if (!version || !version->is_number_integer() || !form || !form->is_string()) return false;
    if (entity.type == "stair") return (*version == 1 && *form == "straight_stair_flight") ||
        ((*version == 2 || *version == 3 || *version == 4) && *form == "multi_flight_stair");
    return entity.type == "railing" && ((*version == 1 && *form == "straight_railing") ||
        (*version == 2 && *form == "stair_flight_railing") || (*version == 3 && *form == "stair_landing_railing"));
}
std::optional<std::string> host_id(const Entity& entity) {
    if (entity.type != "railing" || !supported(entity)) return std::nullopt;
    const auto rail = decode_railing_properties(entity.id, entity.properties);
    if (rail.host) return rail.host->stair_id;
    if (rail.landing_host) return rail.landing_host->stair_id;
    return std::nullopt;
}
bool baseline(const ModelPhases& model, const std::string& id) {
    return std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id);
}
bool touches(const Json& root, const Ids& names) {
    if (names.empty()) return false;
    // The complete source was bounded before this traversal.
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto* value = pending.back(); pending.pop_back();
        if (value->is_string() && names.contains(value->get_ref<const std::string&>())) return true;
        if (value->is_object()) for (const auto& [key, child] : value->items()) {
            if (names.contains(key)) return true;
            pending.push_back(&child);
        } else if (value->is_array()) for (const auto& child : *value) pending.push_back(&child);
    }
    return false;
}
void diagnostic(StairDemolitionRetirementPlan& plan, const std::string& id, const std::string& reason) {
    // Refusal remains deterministic and bounded for large retained sources.
    // Every retained diagnostic is blocking; omitted repeats cannot admit work.
    constexpr std::size_t maximum_diagnostics = 128;
    if (plan.diagnostics.size() >= maximum_diagnostics) return;
    StairDemolitionRetirementDiagnostic item{id, reason};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(std::move(item));
}
bool supported_catalog(const Json& raw) {
    const auto schema = field(raw, "schema");
    if (!schema || !schema->is_string()) return false;
    for (int version = 1; version <= 7; ++version)
        if (*schema == "sketch.assemblies.v" + std::to_string(version)) return true;
    return false;
}
void catalog_bounds(const Json& model, std::size_t& inventory) {
    for (const auto* key : {"materials", "types", "instances"}) {
        const auto rows = field(model, key);
        if (!rows || !rows->is_array() || rows->size() > entity_limit - inventory)
            invalid("affected catalog inventory is not bounded");
        inventory += rows->size();
    }
}
bool bound_overlay(const Json& row, const Ids& retired) {
    const auto owner = field(row, "object_id"), binding = field(row, "dimension_binding");
    const auto bound = binding ? field(*binding, "object_id") : nullptr;
    return (owner && owner->is_string() && retired.contains(owner->get_ref<const std::string&>())) ||
        (bound && bound->is_string() && retired.contains(bound->get_ref<const std::string&>()));
}
template<class Predicate> void filter(Json& rows, Predicate keep) {
    auto retained = Json::array();
    for (const auto& row : rows) if (keep(row)) retained.push_back(row);
    rows = std::move(retained);
}
void remove_ids(Json& rows, const Ids& ids) {
    filter(rows, [&](const Json& row) { return !ids.contains(row.get<std::string>()); });
}

struct Derivation {
    StairDemolitionRetirementPlan plan;
    Entities retired;
    InstanceKeys instances;
    Ids rails;
};
enum class RetirementMode { demolition_closure, selected_proposals };

std::optional<StairDemolitionRetirementIntent> discover(const Entities& source,
    const std::vector<std::string>& selection) {
    if (selection.size() > target_limit) invalid("selection target budget exceeded");
    if (selection.empty()) return std::nullopt;
    source_bounds(source);
    const auto scope = constraint_phase_scope(source);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> owners;
    for (const auto& registry : scope.registries) for (const auto& id : registry.registered_entity_ids)
        if (!owners.emplace(id, &registry).second) invalid("overlapping actual phase ownership: " + id);
    std::map<std::string, ModelPhases, std::less<>> models;
    Ids selected, baseline_selected; const PhysicalWallPhaseState* destination = nullptr;
    for (const auto& id : selection) {
        identity(id);
        if (!selected.insert(id).second) invalid("duplicate selected identity: " + id);
        const auto found = source.find(id);
        if (found == source.end()) invalid("selected actual owner is missing: " + id);
        if (scope.inactive_owner_ids.contains(id)) invalid("selected owner is inactive in saved design: " + id);
        const auto member = owners.find(id);
        if ((found->second.type != "stair" && found->second.type != "railing") || member == owners.end()) {
            continue;
        }
        const auto* registry = member->second;
        if (!models.contains(registry->registry_id)) models.emplace(registry->registry_id,
            ModelPhases::from_json(source.at(registry->registry_id).properties.at("model")));
        const auto& model = models.at(registry->registry_id);
        if (!registry->alternative_id || !baseline(model, id)) continue;
        const auto state = registry->states.find(id);
        if (!supported(found->second) || model.active_alternative() != registry->alternative_id ||
            state == registry->states.end() || state->second != ModelPhase::existing)
            invalid("selection requires actual active supported shared baseline stairs/railings: " + id);
        if (destination && destination != registry) invalid("selected baseline roots span actual registries");
        destination = registry;
        baseline_selected.insert(id);
    }
    if (!destination) return std::nullopt;
    validate_stair_attachment_state(source);
    const auto& model = models.at(destination->registry_id);
    for (const auto& id : destination->registered_entity_ids) {
        const auto& entity = source.at(id);
        if ((entity.type == "stair" || entity.type == "railing") && !supported(entity))
            invalid("affected registry contains an unsupported stair/railing form: " + id);
    }
    Ids retired;
    for (const auto& [id, entity] : source) {
        const auto host = host_id(entity);
        if (!host || !baseline_selected.contains(*host) || source.at(*host).type != "stair") continue;
        const auto member = owners.find(id);
        if (member == owners.end() || member->second != destination)
            invalid("attached rail lacks its host's unambiguous actual registry: " + id);
        const auto state = destination->states.find(id);
        if (state == destination->states.end() || state->second != ModelPhase::proposed) continue;
        if (baseline(model, id)) invalid("retired rail must not be shared baseline: " + id);
        std::size_t proposals{};
        for (const auto& alternative : model.alternatives()) {
            if (std::binary_search(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), id))
                invalid("retired rail participates in demolition membership: " + id);
            if (!std::binary_search(alternative.proposed_ids.begin(), alternative.proposed_ids.end(), id)) continue;
            ++proposals;
            if (alternative.id != *destination->alternative_id)
                invalid("attached proposed rail is shared with another alternative: " + id);
        }
        if (proposals != 1) invalid("retired rail must be solely proposed in the actual active alternative: " + id);
        retired.insert(id);
    }
    // Window selection may include the host's proposed dependents. Normalize
    // only the exact derived retirement closure; unrelated roots still refuse.
    for (const auto& id : selected)
        if (!baseline_selected.contains(id) && !retired.contains(id))
            invalid("selection includes an owner outside the baseline and derived proposed rail closure: " + id);
    if (retired.empty()) return std::nullopt;
    if (retired.size() > retirement_limit) invalid("derived proposed rail retirement budget exceeded");
    StairDemolitionRetirementIntent intent{destination->registry_id, *destination->alternative_id,
        {baseline_selected.begin(), baseline_selected.end()}, {retired.begin(), retired.end()}};
    (void)encode_stair_demolition_retirement_intent(intent);
    return intent;
}

StairDemolitionRetirementIntent discover_selected_proposals(const Entities& source,
    const std::string& registry_id, const std::string& alternative_id,
    const std::vector<std::string>& rail_ids) {
    identity(registry_id); identity(alternative_id);
    if (rail_ids.empty() || rail_ids.size() > retirement_limit ||
        !std::is_sorted(rail_ids.begin(), rail_ids.end()) ||
        std::adjacent_find(rail_ids.begin(), rail_ids.end()) != rail_ids.end())
        invalid("selected proposed rails must be nonempty, bounded, sorted and unique");
    for (const auto& id : rail_ids) identity(id);
    source_bounds(source);
    const auto found_registry = source.find(registry_id);
    if (found_registry == source.end() || found_registry->second.type != "model_phases")
        invalid("selective retirement requires its actual phase registry");
    const auto model = ModelPhases::from_json(found_registry->second.properties.at("model"));
    if (!model.active_alternative() || *model.active_alternative() != alternative_id)
        invalid("selective retirement must name the actual saved active alternative");
    // Registry pointers below borrow this complete saved scope, whose lifetime
    // spans every selection/host check. No shortened map or supplied visibility
    // list establishes phase or attachment authority.
    const auto scope = constraint_phase_scope(source);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> owners;
    const PhysicalWallPhaseState* destination = nullptr;
    for (const auto& registry : scope.registries) {
        if (registry.registry_id == registry_id) destination = &registry;
        for (const auto& id : registry.registered_entity_ids)
            if (!owners.emplace(id, &registry).second) invalid("overlapping actual phase ownership: " + id);
    }
    if (!destination || !destination->alternative_id || *destination->alternative_id != alternative_id)
        invalid("selective retirement has no exact actual saved registry/alternative scope");
    validate_stair_attachment_state(source);
    Ids hosts;
    for (const auto& id : rail_ids) {
        const auto found = source.find(id);
        if (found == source.end() || found->second.type != "railing" || !supported(found->second))
            invalid("selected proposal is not an actual supported rail: " + id);
        const auto member = owners.find(id);
        const auto state = destination->states.find(id);
        if (member == owners.end() || member->second != destination ||
            scope.inactive_owner_ids.contains(id) || state == destination->states.end() ||
            state->second != ModelPhase::proposed || baseline(model, id))
            invalid("selected rail is not active-only proposed in the exact saved registry: " + id);
        std::size_t proposals{};
        for (const auto& alternative : model.alternatives()) {
            if (std::binary_search(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), id))
                invalid("selected proposed rail participates in demolition membership: " + id);
            if (!std::binary_search(alternative.proposed_ids.begin(), alternative.proposed_ids.end(), id)) continue;
            ++proposals;
            if (alternative.id != alternative_id)
                invalid("selected proposed rail is shared with another alternative: " + id);
        }
        if (proposals != 1) invalid("selected rail must be solely proposed in the actual active alternative: " + id);
        const auto host = host_id(found->second);
        if (!host) invalid("selected proposed rail has no actual stair attachment: " + id);
        const auto stair = source.find(*host);
        const auto host_member = owners.find(*host);
        const auto host_state = destination->states.find(*host);
        if (stair == source.end() || stair->second.type != "stair" || !supported(stair->second) ||
            host_member == owners.end() || host_member->second != destination || !baseline(model, *host) ||
            scope.inactive_owner_ids.contains(*host) || host_state == destination->states.end() ||
            host_state->second != ModelPhase::existing)
            invalid("selected proposed rail requires an actual active baseline stair in the same saved registry: " + id);
        hosts.insert(*host);
        if (hosts.size() > target_limit) invalid("derived baseline host evidence budget exceeded");
    }
    StairDemolitionRetirementIntent result{registry_id, alternative_id,
        {hosts.begin(), hosts.end()}, rail_ids};
    (void)encode_stair_demolition_retirement_intent(result);
    return result;
}

void retire_registry(Derivation& derived, const Entities& source) {
    const auto& intent = derived.plan.intent;
    const auto phases = ModelPhases::from_json(source.at(intent.registry_id).properties.at("model"));
    auto members = phases.entity_ids();
    std::erase_if(members, [&](const auto& id) { return derived.rails.contains(id); });
    auto alternatives = phases.alternatives();
    for (auto& row : alternatives) if (row.id == intent.alternative_id)
        std::erase_if(row.proposed_ids, [&](const auto& id) { return derived.rails.contains(id); });
    const auto expected = ModelPhases::create(std::move(members), phases.baseline_ids(),
        std::move(alternatives), phases.active_alternative()).to_json();
    auto raw = source.at(intent.registry_id).properties.at("model");
    remove_ids(raw.at("entity_ids"), derived.rails);
    for (auto& row : raw.at("alternatives")) if (row.at("id") == intent.alternative_id)
        remove_ids(row.at("proposed_ids"), derived.rails);
    if (ModelPhases::from_json(raw).to_json() != expected)
        invalid("raw proposal retirement differs from typed preserved registry");
    derived.retired.at(intent.registry_id).properties.at("model") = std::move(raw);
}

void catalog_context(const Entities& source, const Entity& entity) {
    constexpr std::pair<const char*, const char*> bindings[]{{"property_id", "property"},
        {"building_id", "building"}, {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}};
    for (const auto& [key, type] : bindings) {
        const auto value = field(entity.properties, key); if (!value) continue;
        if (!value->is_string()) invalid("affected catalog has malformed context: " + entity.id);
        const auto owner = source.find(value->get_ref<const std::string&>());
        if (owner == source.end() || owner->second.type != type)
            invalid("affected catalog has unresolved actual context: " + entity.id);
    }
}

// The inspection scratch removes only admitted slots. It never becomes a
// candidate or grants opaque text a rewrite/deletion meaning.
Entity reference_remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "assembly_model") {
        const auto model = field(p, "model");
        if (model && supported_catalog(*model)) {
            (void)AssemblyModel::from_json(*model);
            for (auto& row : p.at("model").at("instances")) {
                if (row.contains("placement") && !row.at("placement").is_null())
                    row.at("placement").erase("host_entity_id");
            }
        }
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model"));
        auto& model = p.at("model"); model.erase("entity_ids"); model.erase("baseline_ids");
        for (auto& row : model.at("alternatives")) { row.erase("proposed_ids"); row.erase("demolished_ids"); }
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids");
            auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) if (row.at("target_kind") == "object") row.erase("target_id");
    }
    return entity;
}

Entity catalog_local_remainder(Entity entity) {
    auto& model = entity.properties.at("model");
    for (auto& material : model.at("materials")) material.erase("id");
    for (auto& type : model.at("types")) {
        type.erase("id"); type.erase("materials");
        if (type.contains("profiles")) for (auto& profile : type.at("profiles")) {
            profile.erase("id"); profile.erase("material_slot");
        }
        if (type.contains("parts")) for (auto& part : type.at("parts")) {
            part.erase("id"); part.erase("type_id"); part.erase("material_overrides");
        }
    }
    for (auto& row : model.at("instances")) {
        row.erase("id"); row.erase("type_id"); row.erase("material_overrides");
        if (row.contains("nested_overrides")) for (auto& change : row.at("nested_overrides")) {
            change.erase("part_path"); change.erase("material_overrides");
        }
    }
    return entity;
}
bool touches_qualified_instance(const Json& root, const InstanceKeys& instances) {
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto* value = pending.back(); pending.pop_back();
        if (value->is_object()) {
            const auto local = field(*value, "instance_id");
            for (const auto* key : {"catalog_id", "assembly_catalog_id"}) {
                const auto catalog = field(*value, key);
                if (catalog && catalog->is_string() && local && local->is_string() &&
                    instances.contains({catalog->get_ref<const std::string&>(), local->get_ref<const std::string&>()}))
                    return true;
            }
            for (const auto& child : *value) pending.push_back(&child);
        } else if (value->is_array()) for (const auto& child : *value) pending.push_back(&child);
    }
    return false;
}

void retire_presentations(Derivation& derived, const Entities& source, const Ids& names, Ids& overlays) {
    for (const auto& [id, original] : source) {
        if (derived.rails.contains(id) || (!touches(original.properties, names) && !touches(original.extensions, names))) continue;
        try {
            const auto scratch = reference_remainder(original);
            if (touches(scratch.properties, names) || touches(scratch.extensions, names))
                diagnostic(derived.plan, id, "affected opaque/reference data has no qualified retirement codec");
            auto& changed = derived.retired.at(id);
            if (original.type == kSheetViewEntityType) {
                for (auto& view : changed.properties.at("model").at("views")) {
                    auto& ids = view.at("object_ids");
                    const bool restricted = !ids.empty() || view.value("restrict_to_objects", false);
                    remove_ids(ids, names);
                    if (restricted && ids.empty()) view["restrict_to_objects"] = true;
                    auto& presentation = view.at("presentation");
                    if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                        filter(presentation.at("appearance").at("objects"), [&](const Json& row) {
                            return !names.contains(row.at("object_id").get<std::string>());
                        });
                    if (view.contains("overlays")) filter(view.at("overlays"), [&](const Json& row) {
                        if (!bound_overlay(row, names)) return true;
                        overlays.insert(row.at("id").get<std::string>()); return false;
                    });
                }
                validate_sheet_view_entity(changed);
            } else if (original.type == kAnnotationEntityType) {
                filter(changed.properties.at("state").at("overrides"), [&](const Json& row) {
                    return row.at("target_kind") != "object" || !names.contains(row.at("target_id").get<std::string>());
                });
                validate_annotation_entity(changed);
            }
        } catch (const std::exception& error) {
            diagnostic(derived.plan, id, std::string("affected reference codec refused: ") + error.what());
        }
    }
}

Derivation derive(const Entities& source, const StairDemolitionRetirementIntent& intent, RetirementMode mode) {
    (void)encode_stair_demolition_retirement_intent(intent);
    if (mode == RetirementMode::demolition_closure) {
        const auto discovered = discover(source, intent.selected_object_ids);
        if (!discovered || *discovered != intent) invalid("proof differs from actual active baseline/proposed attachment inventory");
    } else if (discover_selected_proposals(source, intent.registry_id, intent.alternative_id,
        intent.retired_proposed_rail_ids) != intent)
        invalid("selective host evidence differs from actual selected proposed attachments");
    Derivation result;
    result.plan.intent = intent; result.retired = source;
    result.rails.insert(intent.retired_proposed_rail_ids.begin(), intent.retired_proposed_rail_ids.end());
    retire_registry(result, source);
    const auto aliases = embedded_assembly_presentation_ids(source);
    std::size_t inventory{};
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto raw = field(entity.properties, "model"), rows = raw ? field(*raw, "instances") : nullptr;
        bool affected = false;
        if (rows && rows->is_array()) for (const auto& row : *rows) {
            const auto placement = field(row, "placement"), host = placement ? field(*placement, "host_entity_id") : nullptr;
            if (host && host->is_string() && result.rails.contains(host->get_ref<const std::string&>())) affected = true;
        }
        if (!affected) continue;
        if (!raw || !supported_catalog(*raw)) {
            diagnostic(result.plan, id, "affected catalog has an unsupported placement/instance dialect"); continue;
        }
        catalog_bounds(*raw, inventory); catalog_context(source, entity);
        const auto model = AssemblyModel::from_json(*raw);
        Ids instances;
        for (const auto& row : model.instances()) if (row.placement && result.rails.contains(row.placement->host_entity_id)) {
            if (result.instances.size() >= retirement_limit) invalid("derived hosted instance retirement budget exceeded");
            result.instances.emplace(id, row.id); instances.insert(row.id);
            const auto alias = aliases.find({id, row.id});
            if (alias == aliases.end()) invalid("retired component has no actual qualified presentation alias");
            result.plan.retired_presentation_ids.push_back(alias->second);
        }
        auto& changed = result.retired.at(id).properties.at("model");
        filter(changed.at("instances"), [&](const Json& row) { return !instances.contains(row.at("id").get<std::string>()); });
        // The exact raw envelope, definitions, materials and unrelated rows are
        // retained, including an empty instances array and its catalog owner.
        (void)AssemblyModel::from_json(changed);
        // A local identity has meaning only in its actual catalog. Inspect
        // opaque data there after stripping codec-owned definition references;
        // never treat an unqualified spelling as global erase authority.
        const auto local = catalog_local_remainder(reference_remainder(entity));
        if (touches(local.properties, instances) || touches(local.extensions, instances))
            diagnostic(result.plan, id, "affected catalog contains an opaque reference to a retired local instance");
    }
    result.plan.retired_hosted_instances.assign(result.instances.begin(), result.instances.end());
    std::sort(result.plan.retired_presentation_ids.begin(), result.plan.retired_presentation_ids.end());
    Ids names = result.rails;
    names.insert(result.plan.retired_presentation_ids.begin(), result.plan.retired_presentation_ids.end());
    Ids overlays;
    retire_presentations(result, source, names, overlays);
    for (const auto& id : result.rails) result.retired.erase(id);
    const auto remaining_aliases = embedded_assembly_presentation_ids(result.retired);
    for (const auto& [key, alias] : aliases) if (!result.instances.contains(key)) {
        const auto remaining = remaining_aliases.find(key);
        if (remaining == remaining_aliases.end() || remaining->second != alias)
            diagnostic(result.plan, key.first, "retirement would change a surviving component's original presentation alias");
    }
    // Exact retired overlay child references are unsupported too; do not strand
    // an external/local binding after removing its source-bound overlay.
    names.insert(overlays.begin(), overlays.end());
    for (const auto& [id, entity] : result.retired)
        if (touches(entity.properties, names) || touches(entity.extensions, names) ||
            touches_qualified_instance(entity.properties, result.instances) || touches_qualified_instance(entity.extensions, result.instances))
            diagnostic(result.plan, id, "retained reference to retired owner/component/overlay requires an explicit qualified codec");
    validate_stair_attachment_state(result.retired);
    const auto before = constraint_phase_scope(source), after = constraint_phase_scope(result.retired);
    if (before.inactive_owner_ids != after.inactive_owner_ids)
        invalid("proposal retirement unexpectedly changed inactive baseline/other-alternative ownership");
    std::sort(result.plan.diagnostics.begin(), result.plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return result;
}

// Actual selected rows and all aggregate analytical bounds are admitted before
// constructing the first native body. Both profile and legacy host-body rows
// use the same assembly/native codecs as stair replacement.
struct RetirementGeometry {
    std::vector<BuildingObject> rails;
    std::vector<AssemblyExpansion> expansions;
    std::vector<std::pair<BuildingObject, AssemblyTransform>> legacy;
};
void admit_material_assignment(const Entities& source, const Entity& entity,
    std::map<std::string, AssemblyModel, std::less<>>& models) {
    const auto assignment = field(entity.properties, "material_assignment");
    if (!assignment) return;
    const auto version = field(*assignment, "version"), catalog_id = field(*assignment, "catalog_id"),
        material_id = field(*assignment, "material_id");
    if (!version || !version->is_number_integer() || *version != 1 ||
        !catalog_id || !catalog_id->is_string() || !material_id || !material_id->is_string())
        invalid("retired rail has an unsupported material assignment: " + entity.id);
    const auto catalog = source.find(catalog_id->get_ref<const std::string&>());
    if (catalog == source.end() || catalog->second.type != "assembly_model")
        invalid("retired rail requires its actual material catalog: " + entity.id);
    const auto raw = field(catalog->second.properties, "model");
    if (!raw || !supported_catalog(*raw)) invalid("retired rail material catalog has an unsupported dialect: " + entity.id);
    std::size_t inventory{}; catalog_bounds(*raw, inventory);
    if (!models.contains(catalog->first)) models.emplace(catalog->first, AssemblyModel::from_json(*raw));
    const auto& model = models.at(catalog->first);
    if (std::none_of(model.materials().begin(), model.materials().end(), [&](const auto& row) {
        return row.id == material_id->get_ref<const std::string&>();
    })) invalid("retired rail material is absent from its actual catalog: " + entity.id);
}
RetirementGeometry prepare_retired_geometry(const Entities& source, const Derivation& derived) {
    const auto organization = organize_project(source);
    RetirementGeometry prepared;
    std::map<std::string, AssemblyModel, std::less<>> models;
    std::map<std::string, StairFlight, std::less<>> stairs;
    const auto stair_for = [&](const std::string& host) -> const StairFlight& {
        if (!stairs.contains(host)) stairs.emplace(host,
            decode_stair_properties(host, resolve_vertical_placement(source, source.at(host)).properties));
        return stairs.at(host);
    };
    std::size_t work{};
    for (const auto& id : derived.rails) {
        const auto node = organization.nodes.find(id);
        if (node == organization.nodes.end() || !node->second.issues.empty())
            invalid("retired rail has unresolved actual organization: " + id);
        admit_material_assignment(source, source.at(id), models);
        const auto host = host_id(source.at(id));
        if (!host) invalid("retired rail lost its actual stair attachment: " + id);
        const auto rail = decode_railing_properties(id, source.at(id).properties);
        const auto cost = derive_hosted_railing_layout(rail, stair_for(*host)).posts.size() + 1;
        if (cost > geometry_limit - work) invalid("retired rail native geometry budget exceeded");
        work += cost; prepared.rails.emplace_back(rail);
    }
    AssemblyExpansionBudget budget;
    for (const auto& [catalog, local_id] : derived.instances) {
        if (!models.contains(catalog)) models.emplace(catalog, AssemblyModel::from_json(source.at(catalog).properties.at("model")));
        const auto& model = models.at(catalog);
        const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == local_id; });
        if (row == model.instances().end() || !row->placement || !derived.rails.contains(row->placement->host_entity_id))
            invalid("retired hosted row differs from actual qualified source");
        auto expansion = model.expand(*row, budget);
        if (!expansion.profiles.empty()) prepared.expansions.push_back(std::move(expansion));
        else {
            const auto& placement = *row->placement;
            const auto host = placement.host_entity_id;
            const auto rail = decode_railing_properties(host, source.at(host).properties);
            const auto stair_id = host_id(source.at(host));
            if (!stair_id) invalid("host-derived retired component requires an actual attached rail");
            const auto cost = derive_hosted_railing_layout(rail, stair_for(*stair_id)).posts.size() + 1;
            if (cost > geometry_limit - work) invalid("retired host-derived component native budget exceeded");
            work += cost;
            prepared.legacy.emplace_back(BuildingObject{rail}, AssemblyTransform{{placement.translation_m.x,
                placement.translation_m.y, placement.translation_z_m}, placement.rotation_radians,
                placement.scale, placement.mirrored_y, placement.vertical_scale});
        }
    }
    return prepared;
}
void admit_retired_geometry(const Entities& source, const RetirementGeometry& prepared) {
    for (const auto& object : prepared.rails) (void)make_building_shape(object, source);
    for (const auto& expansion : prepared.expansions) (void)make_assembly_geometry(expansion);
    for (const auto& [object, transform] : prepared.legacy)
        (void)transform_assembly_shape(make_building_shape(object, source), transform);
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
} // namespace

Json encode_stair_demolition_retirement_intent(const StairDemolitionRetirementIntent& intent) {
    identity(intent.registry_id); identity(intent.alternative_id);
    const auto admit = [](const auto& ids, std::size_t limit) {
        if (ids.empty() || ids.size() > limit || !std::is_sorted(ids.begin(), ids.end()) ||
            std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            invalid("proof inventories must be nonempty, bounded, sorted and unique");
        for (const auto& id : ids) identity(id);
    };
    admit(intent.selected_object_ids, target_limit); admit(intent.retired_proposed_rail_ids, retirement_limit);
    for (const auto& id : intent.retired_proposed_rail_ids)
        if (std::binary_search(intent.selected_object_ids.begin(), intent.selected_object_ids.end(), id))
            invalid("derived retired proposals cannot be explicit baseline selection roots");
    Json result{{"version", 1}, {"registry_id", intent.registry_id}, {"alternative_id", intent.alternative_id},
        {"selected_object_ids", intent.selected_object_ids}, {"retired_proposed_rail_ids", intent.retired_proposed_rail_ids}};
    if (result.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    return result;
}
StairDemolitionRetirementIntent decode_stair_demolition_retirement_intent(const Json& value) {
    try {
        if (!value.is_object() || value.size() != 5 || !value.contains("version") ||
            !value.at("version").is_number_integer() || value.at("version") != 1 ||
            !value.contains("registry_id") || !value.contains("alternative_id") ||
            !value.contains("selected_object_ids") || !value.at("selected_object_ids").is_array() ||
            value.at("selected_object_ids").empty() || value.at("selected_object_ids").size() > target_limit ||
            !value.contains("retired_proposed_rail_ids") || !value.at("retired_proposed_rail_ids").is_array() ||
            value.at("retired_proposed_rail_ids").empty() || value.at("retired_proposed_rail_ids").size() > retirement_limit)
            invalid("proof requires exactly five bounded version-one fields");
        StairDemolitionRetirementIntent intent{identity(value.at("registry_id")), identity(value.at("alternative_id")), {}, {}};
        for (const auto& id : value.at("selected_object_ids")) intent.selected_object_ids.push_back(identity(id));
        for (const auto& id : value.at("retired_proposed_rail_ids")) intent.retired_proposed_rail_ids.push_back(identity(id));
        if (encode_stair_demolition_retirement_intent(intent) != value) invalid("proof is not canonical");
        return intent;
    } catch (const Json::exception& error) { invalid(std::string("malformed proof: ") + error.what()); }
}
std::optional<StairDemolitionRetirementIntent> phase_stair_demolition_retirement_request(
    const Entities& source, const std::vector<std::string>& selection) {
    try { return discover(source, selection); }
    catch (const Json::exception& error) { invalid(std::string("malformed actual discovery source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString(); invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
StairDemolitionRetirementPlan inspect_phase_stair_demolition_retirement_plan(
    const Entities& source, const StairDemolitionRetirementIntent& intent) {
    try {
        auto derived = derive(source, intent, RetirementMode::demolition_closure);
        if (derived.plan.ready()) {
            try { (void)prepare_retired_geometry(source, derived); }
            catch (const std::exception& error) {
                diagnostic(derived.plan, intent.registry_id, std::string("actual retirement analytical admission refused: ") + error.what());
            }
        }
        return std::move(derived.plan);
    }
    catch (const Json::exception& error) { invalid(std::string("malformed actual inspection source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString(); invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
Entities replay_phase_stair_demolition_retirement_entities(const Entities& source,
    const StairDemolitionRetirementIntent& intent) {
    try {
        auto derived = derive(source, intent, RetirementMode::demolition_closure);
        if (!derived.plan.ready()) {
            const auto& diagnostic = derived.plan.diagnostics.front();
            invalid("blocked at " + diagnostic.entity_id + ": " + diagnostic.reason);
        }
        const auto prepared = prepare_retired_geometry(source, derived);
        auto result = replay_phase_stair_demolition_entities(derived.retired,
            StairDemolitionIntent{intent.registry_id, intent.alternative_id, intent.selected_object_ids});
        admit_retired_geometry(source, prepared);
        validate_stair_attachment_state(result);
        if (result.size() != derived.retired.size()) invalid("baseline demolition changed the retired entity inventory");
        for (const auto& [id, entity] : derived.retired) if (id != intent.registry_id) {
            const auto found = result.find(id);
            if (found == result.end() || !exact(found->second, entity))
                invalid("baseline demolition changed a non-registry retired-candidate envelope: " + id);
        }
        // Exact copies of every surviving non-registry physical envelope are
        // guaranteed by the retirement patches and checked after composition.
        for (const auto& [id, entity] : source) if ((entity.type == "stair" || entity.type == "railing") && !derived.rails.contains(id)) {
            const auto& remaining = result.at(id);
            if (!exact(remaining, entity))
                invalid("baseline/other retained physical envelope changed: " + id);
        }
        for (const auto& [id, entity] : source) if (id != intent.registry_id && !derived.rails.contains(id) &&
            entity.type != "assembly_model" && entity.type != kSheetViewEntityType && entity.type != kAnnotationEntityType)
            if (!exact(result.at(id), entity)) invalid("unrelated organization/property/level envelope changed: " + id);
        return result;
    } catch (const Json::exception& error) { invalid(std::string("malformed actual replay source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString(); invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}

StairDemolitionRetirementPlan inspect_phase_stair_proposed_rail_retirement_plan(
    const Entities& source, const std::string& registry_id, const std::string& alternative_id,
    const std::vector<std::string>& rail_ids) {
    try {
        const auto intent = discover_selected_proposals(source, registry_id, alternative_id, rail_ids);
        auto derived = derive(source, intent, RetirementMode::selected_proposals);
        if (derived.plan.ready()) {
            try { (void)prepare_retired_geometry(source, derived); }
            catch (const std::exception& error) {
                diagnostic(derived.plan, registry_id, std::string("actual retirement analytical admission refused: ") + error.what());
            }
        }
        return std::move(derived.plan);
    } catch (const Json::exception& error) { invalid(std::string("malformed actual selective inspection source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString(); invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
Entities replay_phase_stair_proposed_rail_retirement_entities(
    const Entities& source, const std::string& registry_id, const std::string& alternative_id,
    const std::vector<std::string>& rail_ids) {
    try {
        const auto intent = discover_selected_proposals(source, registry_id, alternative_id, rail_ids);
        auto derived = derive(source, intent, RetirementMode::selected_proposals);
        if (!derived.plan.ready()) {
            const auto& diagnostic = derived.plan.diagnostics.front();
            invalid("blocked at " + diagnostic.entity_id + ": " + diagnostic.reason);
        }
        const auto prepared = prepare_retired_geometry(source, derived);
        // Baseline hosts are evidence only. This candidate deliberately omits
        // the demolition composer used by the historical closure lane.
        admit_retired_geometry(source, prepared);
        validate_stair_attachment_state(derived.retired);
        if (derived.retired.size() != source.size() - derived.rails.size())
            invalid("selective retirement changed another actual entity's lifetime");
        for (const auto& [id, entity] : source) if (!derived.rails.contains(id)) {
            const auto found = derived.retired.find(id);
            if (found == derived.retired.end()) invalid("selective retirement removed another actual owner: " + id);
            if (id != registry_id && entity.type != "assembly_model" &&
                entity.type != kSheetViewEntityType && entity.type != kAnnotationEntityType &&
                !exact(found->second, entity))
                invalid("selective retirement changed a retained physical/organization/property/level envelope: " + id);
        }
        return std::move(derived.retired);
    } catch (const Json::exception& error) { invalid(std::string("malformed actual selective replay source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString(); invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
} // namespace sketch
