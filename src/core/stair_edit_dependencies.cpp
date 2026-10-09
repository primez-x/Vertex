#include "sketch/stair_edit_dependencies.hpp"

#include "sketch/architectural_object_removal.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/stair_attachment_integrity.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t geometry_limit = 262144, dependency_limit = 128;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Ordinary stair dependency edit: " + reason);
}
const Json* field(const Json& value, const char* name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
// Aggregate source and all supplied editor envelopes before nested codecs or
// repeated complete-source scans. Metadata is bounded, never edit authority.
struct JsonBudget {
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > 64 * 1024 * 1024 - bytes) reject("aggregate JSON string budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("aggregate JSON node/nesting budget exceeded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("nonfinite source scalar");
        if (value.is_discarded()) reject("discarded source JSON");
        if (value.is_binary()) {
            if (value.get_binary().size() > 64 * 1024 * 1024 - bytes) reject("aggregate binary budget exceeded");
            bytes += value.get_binary().size();
        }
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (value.is_object()) for (const auto& [key, child] : value.items()) { text(key); read(child, depth + 1); }
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
    }
    void entity(const Entity& value) {
        text(value.id); text(value.type); read(value.properties); read(value.extensions);
    }
};
struct Work {
    std::size_t used{};
    void add(std::size_t count) {
        if (count > geometry_limit - used) reject("aggregate analytical/native geometry budget exceeded");
        used += count;
    }
};
std::optional<std::string> host_id(const Entity& entity) {
    if (entity.type != "railing") return std::nullopt;
    const auto host = field(entity.properties, "host");
    const auto id = host ? field(*host, "stair_id") : nullptr;
    if (!id || !id->is_string()) return std::nullopt;
    return id->get<std::string>();
}
bool known_stair(const Entity& entity) {
    const auto version = field(entity.properties, "version"), form = field(entity.properties, "form");
    return entity.type == "stair" && version && version->is_number_integer() && form && form->is_string() &&
        ((*version == 1 && *form == "straight_stair_flight") ||
         ((*version == 2 || *version == 3 || *version == 4) && *form == "multi_flight_stair"));
}
bool known_rail(const Entity& entity) {
    const auto version = field(entity.properties, "version"), form = field(entity.properties, "form");
    return entity.type == "railing" && version && version->is_number_integer() && form && form->is_string() &&
        ((*version == 1 && *form == "straight_railing") || (*version == 2 && *form == "stair_flight_railing") ||
         (*version == 3 && *form == "stair_landing_railing"));
}
struct Ownership {
    ConstraintPhaseScope scope;
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> owners;
};
Ownership ownership(const Entities& actual) {
    Ownership result; result.scope = constraint_phase_scope(actual);
    for (const auto& registry : result.scope.registries) {
        result.models.emplace(registry.registry_id, ModelPhases::from_json(actual.at(registry.registry_id).properties.at("model")));
        for (const auto& id : registry.registered_entity_ids)
            if (!result.owners.emplace(id, registry.registry_id).second) reject("overlapping actual phase ownership: " + id);
    }
    return result;
}
void ordinary_owner(const Ownership& phase, const std::string& id) {
    if (phase.scope.inactive_owner_ids.contains(id)) reject("owner is inactive in the saved design: " + id);
    const auto found = phase.owners.find(id);
    if (found == phase.owners.end()) return;
    const auto& model = phase.models.at(found->second);
    if (std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id)) {
        if (model.active_alternative() || !model.alternatives().empty()) reject("shared baseline requires phase authoring: " + id);
        return;
    }
    if (!model.active_alternative()) reject("owner has no active proposal: " + id);
    const auto state = model.active_state();
    const auto row = state.find(id);
    if (row == state.end() || row->second != ModelPhase::proposed) reject("owner is not an active proposal: " + id);
    std::size_t count{};
    for (const auto& alternative : model.alternatives()) {
        if (std::binary_search(alternative.demolished_ids.begin(), alternative.demolished_ids.end(), id))
            reject("owner has protected demolition membership: " + id);
        if (!std::binary_search(alternative.proposed_ids.begin(), alternative.proposed_ids.end(), id)) continue;
        if (alternative.id != *model.active_alternative()) reject("owner is shared with another alternative: " + id);
        ++count;
    }
    if (count != 1) reject("owner lacks sole active proposal membership: " + id);
}
bool touches(const Json& value, const Ids& ids) {
    if (value.is_string()) return ids.contains(value.get_ref<const std::string&>());
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        if (ids.contains(key) || touches(child, ids)) return true;
    }
    else if (value.is_array()) for (const auto& child : value) if (touches(child, ids)) return true;
    return false;
}
Ids children(const Entity& entity) {
    Ids result;
    if (known_stair(entity)) for (const auto* name : {"flights", "landings"})
        if (const auto rows = field(entity.properties, name); rows && rows->is_array())
            for (const auto& row : *rows) result.insert(row.at("id").get<std::string>());
    return result;
}
void opaque_child_references(const Entities& actual, const Entities& descriptors) {
    Ids removed;
    for (const auto& [id, entity] : actual) {
        const auto before = children(entity), after = children(descriptors.at(id));
        for (const auto& child : before) if (!after.contains(child)) removed.insert(child);
    }
    if (removed.empty()) return;
    for (const auto& [id, entity] : actual) {
        auto raw = entity.properties;
        // Only actual typed identity/binding slots have a known disposition.
        if (known_stair(entity)) for (const auto* name : {"flights", "landings"})
            if (raw.contains(name)) for (auto& row : raw.at(name)) row.erase("id");
        if (known_rail(entity) && host_id(entity)) for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"})
            raw.at("host").erase(name);
        if (touches(raw, removed) || touches(entity.extensions, removed))
            reject("affected opaque child reference lacks a topology codec: " + id);
    }
}
struct Inspection {
    PhaseStairReplacementDependencyPlan plan;
    // Analytical descriptors only; never passed as source authority to replay.
    Entities descriptors;
    Ownership phase;
    Work work;
};
Inspection inspect(const Entities& actual, const std::vector<Entity>& edited) {
    if (actual.size() > 65536 || edited.empty() || edited.size() > maximum_architectural_group_targets)
        reject("requires a bounded nonempty actual editor cohort");
    JsonBudget budget;
    for (const auto& [id, entity] : actual) { (void)id; budget.entity(entity); }
    for (const auto& entity : edited) budget.entity(entity);
    stair_transform_detail::source_bounds(actual);
    validate_stair_attachment_state(actual);
    Inspection result; result.descriptors = actual; result.phase = ownership(actual);
    Ids seen; std::size_t profile_bytes{};
    std::map<std::string, StairFlight, std::less<>> stairs;
    for (const auto& entity : edited) {
        if (!seen.insert(entity.id).second) reject("duplicate editor owner");
        const auto found = actual.find(entity.id);
        if (found == actual.end() || (found->second.type != "stair" && found->second.type != "railing"))
            reject("editor requires an actual stair or railing owner");
        ordinary_owner(result.phase, entity.id);
        const auto captured = capture_stair_object_edit(found->second, entity);
        if (!captured) continue;
        const auto bytes = encode_stair_object_edit_intent(*captured).dump().size();
        if (bytes > 1024 * 1024 - profile_bytes) reject("aggregate typed profile byte budget exceeded");
        profile_bytes += bytes;
        result.descriptors.at(entity.id) = entity;
        if (entity.type == "stair") {
            auto stair = decode_stair_properties(entity.id, resolve_vertical_placement(actual, entity).properties);
            result.work.add(stair.riser_count + stair.landings.size() + 1);
            stairs.emplace(entity.id, std::move(stair));
        }
    }
    validate_stair_identity_transition(actual, result.descriptors, std::span<const RevisionRecord>{});
    opaque_child_references(actual, result.descriptors);
    for (const auto& [id, entity] : actual) {
        const auto host = host_id(entity);
        if (!host || !stairs.contains(*host)) continue;
        // Even unchanged bindings cannot authorize edits of historical geometry.
        ordinary_owner(result.phase, id);
        const auto rail = decode_railing_properties(id, entity.properties);
        const auto& stair = stairs.at(*host);
        std::optional<HostedRailingLayout> retained;
        try { retained = derive_hosted_railing_layout(rail, stair); }
        catch (const std::invalid_argument&) {}
        if (retained) { result.work.add(retained->posts.size() + 1); continue; }
        if (result.plan.dependencies.size() == dependency_limit ||
            actual.size() > 2000000 / (result.plan.dependencies.size() + 1))
            reject("complete-source dependency inspection work budget exceeded");
        PhaseStairReplacementDependency row;
        row.rail_id = id; row.stair_id = *host; row.current_host = entity.properties.at("host");
        if (const auto name = field(entity.properties, "name"); name && name->is_string()) row.rail_name = name->get<std::string>();
        row.host_role = rail.host ? "flight" : (rail.landing_host->role == StairLandingRole::top ? "top" : "landing");
        row.current_child_id = rail.host ? rail.host->flight_id : rail.landing_host->landing_id;
        const auto admit = [&](const Railing& changed, PhaseStairReplacementDependencyTarget target) {
            std::optional<HostedRailingLayout> layout;
            try { layout = derive_hosted_railing_layout(changed, stair); }
            catch (const std::invalid_argument&) { return; }
            result.work.add(layout->posts.size() + 1);
            target.host = encode_railing_properties(changed).at("host");
            if (target.role == "top") target.display_name = "Stair top";
            const auto rows = field(actual.at(*host).properties, target.role == "flight" ? "flights" : "landings");
            if (rows && rows->is_array()) for (const auto& child : *rows) if (child.at("id") == target.child_id) {
                const auto name = field(child, "name");
                if (name && name->is_string()) target.display_name = name->get<std::string>();
            }
            if (target.display_name.empty()) {
                std::size_t ordinal{};
                if (target.role == "flight") for (const auto& child : stair.flights) {
                    ++ordinal; if (child.id == target.child_id) break;
                }
                else for (const auto& child : stair.landings) {
                    ++ordinal; if (child.id == target.child_id) break;
                }
                target.display_name = (target.role == "flight" ? "Flight " : "Landing ") + std::to_string(ordinal);
            }
            row.valid_targets.push_back(std::move(target));
        };
        if (rail.host) for (const auto& child : stair.flights) {
            auto changed = rail; changed.host->flight_id = child.id;
            admit(changed, {"flight:" + child.id, "flight", child.id, {}, {}, Json::object()});
        }
        else if (rail.landing_host->role == StairLandingRole::connecting) {
            for (std::size_t i = 0; i < stair.landings.size(); ++i) {
                auto changed = rail; auto& binding = *changed.landing_host;
                binding.landing_id = stair.landings[i].id;
                binding.incoming_flight_id = stair.flights.at(i).id;
                binding.outgoing_flight_id = stair.flights.at(i + 1).id;
                admit(changed, {"landing:" + binding.landing_id, "landing", binding.landing_id,
                    binding.incoming_flight_id, binding.outgoing_flight_id, Json::object()});
            }
        } else if (!stair.flights.empty() && stair.top_landing) {
            auto changed = rail; changed.landing_host->incoming_flight_id = stair.flights.back().id;
            admit(changed, {"top", "top", {}, changed.landing_host->incoming_flight_id, {}, Json::object()});
        }
        std::sort(row.valid_targets.begin(), row.valid_targets.end(), [](const auto& a, const auto& b) { return a.target_key < b.target_key; });
        try {
            preflight_architectural_object_removal(actual, {id}, {});
            row.retirement_eligible = true;
        } catch (const std::exception& error) { row.retirement_reason = error.what(); }
        result.plan.dependencies.push_back(std::move(row));
    }
    return result;
}
void preserve_rooms(const Entities& actual, const Entities& candidate) {
    for (const auto& [id, entity] : actual) if (entity.type == "room" || entity.type == "boundary") {
        const auto found = candidate.find(id);
        if (found == candidate.end() || !exact(entity, found->second)) reject("room/boundary lineage changed: " + id);
    }
}
EmbeddedAssemblyPresentationIds surviving_aliases(const Entities& actual,
    const std::vector<std::string>& retired) {
    auto expected = embedded_assembly_presentation_ids(actual);
    const Ids removed(retired.begin(), retired.end());
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model") {
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& instance : model.instances()) if (instance.placement &&
            removed.contains(instance.placement->host_entity_id)) {
            if (expected.erase({id, instance.id}) != 1) reject("actual retired hosted row lacks qualified presentation identity");
        }
    }
    return expected;
}
// Reserve every repeated typed geometry pass conservatively before the first
// factory. Hosted catalogs keep their graph/row count under rigid placement.
// The retirement preflight additionally accounts for its complete scene pass.
std::size_t native_reservation(const Entities& actual, const Entities& descriptors, Work work) {
    std::map<std::string, std::size_t, std::less<>> host_costs;
    for (const auto* map : {&actual, &descriptors}) {
        for (const auto& [id, entity] : *map) {
            if (known_stair(entity)) {
                const auto stair = decode_stair_properties(id, resolve_vertical_placement(*map, entity).properties);
                const auto cost = stair.riser_count + stair.landings.size() + 1;
                if (cost > geometry_limit / 32) reject("stair repeated native work exceeded");
                work.add(cost * 32);
                host_costs[id] = std::max(host_costs[id], cost);
            } else if (known_rail(entity)) {
                const auto rail = decode_railing_properties(id, entity.properties);
                std::size_t cost{};
                if (const auto host = host_id(entity)) cost = derive_hosted_railing_layout(rail,
                    decode_stair_properties(*host, resolve_vertical_placement(*map, map->at(*host)).properties)).posts.size() + 1;
                else {
                    const auto posts = std::ceil(rail.length / rail.post_spacing) + 2;
                    if (!std::isfinite(posts) || posts > geometry_limit / 32) reject("railing repeated native work exceeded");
                    cost = static_cast<std::size_t>(posts);
                }
                if (cost > geometry_limit / 32) reject("hosted railing repeated native work exceeded");
                work.add(cost * 32);
                host_costs[id] = std::max(host_costs[id], cost);
            }
        }
    }
    // Original catalogs bound their surviving rigid copies. Do not admit the
    // analytical retired-rail map as a catalog source: its rows have deliberately
    // not yet undergone the complete qualified retirement producer.
    AssemblyExpansionBudget assembly;
    assembly.max_profile_segments = geometry_limit / 64;
    (void)expand_document_assembly_instances(actual, assembly);
    work.add(assembly.consumed_nodes * 64); work.add(assembly.consumed_profile_segments * 64);
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model") {
        (void)id;
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& instance : model.instances()) if (instance.placement &&
            host_costs.contains(instance.placement->host_entity_id)) {
            // Conservative for profile-bearing rows too: legacy rows repeatedly
            // reconstruct a complete actual host before transforming its copy.
            work.add(host_costs.at(instance.placement->host_entity_id) * 32);
        }
    }
    return work.used;
}
} // namespace

PhaseStairReplacementDependencyPlan inspect_ordinary_stair_edit_dependencies(
    const Entities& actual, const std::vector<Entity>& edited) {
    try { return inspect(actual, edited).plan; }
    catch (const std::exception& error) {
        PhaseStairReplacementDependencyPlan result; result.diagnostics.push_back({{}, error.what(), true}); return result;
    }
}

ApplyEntityChanges prepare_ordinary_stair_dependency_edit(const DocumentSnapshot& source,
    const std::vector<Entity>& edited, const std::vector<PhaseStairReplacementDependencyDisposition>& choices,
    const std::string& message) try {
    if (!source.is_editable()) reject("captured source is read-only: " + source.read_only_reason());
    if (message.empty() || message.size() > 4096) reject("message requires 1..4096 bytes");
    const auto& actual = source.entities();
    auto inspection = inspect(actual, edited);
    const auto& dependencies = inspection.plan.dependencies;
    if (choices.size() != dependencies.size()) reject("requires exactly one decision per affected rail");
    auto complete_edited = edited;
    std::vector<std::string> retired;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const auto& row = dependencies[i]; const auto& choice = choices[i];
        if (choice.rail_id.size() > 128 || choice.target_key.size() > 256) reject("decision string budget exceeded");
        if (choice.rail_id != row.rail_id) reject("decisions must exactly follow ascending actual affected rails");
        if (choice.action == PhaseStairReplacementDependencyAction::retire) {
            if (!row.retirement_eligible || !choice.target_key.empty()) reject("rail is ineligible for explicit retirement: " + row.rail_id);
            if (std::any_of(edited.begin(), edited.end(), [&](const auto& entity) { return entity.id == row.rail_id; }))
                reject("an edited rail cannot also be retired");
            retired.push_back(row.rail_id); inspection.descriptors.erase(row.rail_id); continue;
        }
        if (choice.action != PhaseStairReplacementDependencyAction::rehost) reject("unsupported dependency action");
        const auto target = std::find_if(row.valid_targets.begin(), row.valid_targets.end(),
            [&](const auto& value) { return value.target_key == choice.target_key; });
        if (target == row.valid_targets.end()) reject("rehost target is not an admitted resulting same-owner binding");
        auto supplied = std::find_if(complete_edited.begin(), complete_edited.end(), [&](const auto& value) { return value.id == row.rail_id; });
        if (supplied == complete_edited.end()) {
            auto changed = actual.at(row.rail_id);
            for (const auto& [key, value] : target->host.items()) changed.properties.at("host")[key] = value;
            complete_edited.push_back(std::move(changed)); supplied = std::prev(complete_edited.end());
        }
        const auto captured = capture_stair_object_edit(actual.at(row.rail_id), *supplied);
        if (!captured || captured->profile_fields.at("host") != target->host)
            reject("editor rail binding conflicts with its explicit rehost choice");
        inspection.descriptors.at(row.rail_id) = *supplied;
    }
    if (complete_edited.size() > maximum_architectural_group_targets) reject("aggregate editor/rehost target budget exceeded");
    JsonBudget complete_budget;
    for (const auto& [id, entity] : actual) { (void)id; complete_budget.entity(entity); }
    std::size_t compound_bytes{};
    for (const auto& entity : complete_edited) {
        complete_budget.entity(entity);
        if (const auto profile = capture_stair_object_edit(actual.at(entity.id), entity)) {
            const auto size = encode_stair_object_edit_intent(*profile).dump().size();
            // Compound capture retains the quantity envelope in both lanes;
            // leave bounded space for the complete rigid operator and wrapper.
            if (compound_bytes > 1024 * 1024 - 4096 || size > (1024 * 1024 - 4096 - compound_bytes) / 2)
                reject("aggregate compound profile/placement byte reservation exceeded");
            compound_bytes += size * 2 + 4096;
        }
    }
    // The analytical map is used only for pre-native reservation. All physical
    // producers below receive original actual source or its derived retirement.
    validate_stair_identity_transition(actual, inspection.descriptors, source.history());
    const auto reservation = native_reservation(actual, inspection.descriptors, inspection.work);
    if (!retired.empty()) preflight_architectural_object_removal(actual, retired, {}, reservation);
    const auto source_constraints = source.uses_active_phase_constraints()
        ? validate_active_phase_constraint_integrity(actual) : validate_constraint_integrity(actual);
    if (source_constraints) reject(*source_constraints);
    const auto expected_aliases = surviving_aliases(actual, retired);
    const auto stage = retired.empty() ? actual : replay_architectural_object_removal(actual, retired, {}, true, reservation);
    for (const auto& entity : complete_edited) if (!stage.contains(entity.id)) reject("edited owner overlaps retired closure");
    const auto stage_aliases = embedded_assembly_presentation_ids(stage);
    if (stage_aliases != expected_aliases) reject("retirement changed aliases outside actual qualified retired rows");
    const auto intents = capture_stair_compound_edits(stage, complete_edited);
    const auto candidate = replay_stair_compound_edit_entities(stage, intents);
    JsonBudget final_budget;
    for (const auto& [id, entity] : candidate) {
        if (!actual.contains(id)) reject("ordinary authoring introduced an unowned entity");
        final_budget.entity(entity);
    }
    validate_stair_identity_transition(actual, candidate, source.history());
    preserve_rooms(actual, candidate);
    if (embedded_assembly_presentation_ids(candidate) != stage_aliases) reject("edit changed surviving qualified presentation aliases");
    const auto constraints = source.uses_active_phase_constraints()
        ? validate_active_phase_constraint_integrity(candidate) : validate_constraint_integrity(candidate);
    if (constraints) reject(*constraints);
    ApplyEntityChanges command; command.expected_revision = source.revision(); command.message = message;
    for (const auto& [id, entity] : actual) {
        const auto found = candidate.find(id);
        if (found == candidate.end()) command.entity_changes.push_back(EntityChange::erase(id));
        else if (!exact(entity, found->second)) command.entity_changes.push_back(EntityChange::upsert(found->second));
    }
    if (command.entity_changes.empty()) reject("ordinary dependency edit has no actual change");
    // Retain and admit the complete existing v1 representation; no replacement
    // proof/state dialect or partial entity-map source is introduced here.
    const auto wire = command_to_json(Command{command});
    JsonBudget command_budget; command_budget.read(wire);
    if (wire.at("version") != 1 || wire.at("kind") != "apply_entity_changes") reject("raw v1 command representation changed");
    const auto retained = command_from_json(wire);
    const auto* raw = std::get_if<ApplyEntityChanges>(&retained);
    if (!raw || command_to_json(retained).dump() != wire.dump()) reject("raw v1 command failed exact retention");
    const auto preview = Document::preview_command(source, Command{command});
    if (preview.entities() != candidate || preview.assets() != source.assets() || !preview.is_editable() ||
        preview.document_id() != source.document_id()) reject("real Document preview changed complete admitted authority");
    if (preview.saved_revision_optional() != source.saved_revision_optional() ||
        preview.named_revisions() != source.named_revisions() || preview.read_only_reason() != source.read_only_reason())
        reject("real Document preview changed retained snapshot metadata");
    for (const auto& [id, entity] : candidate) if (!exact(entity, preview.entities().at(id)))
        reject("preview changed exact admitted raw state: " + id);
    for (const auto& [id, asset] : source.assets()) if (asset.metadata.dump() != preview.assets().at(id).metadata.dump())
        reject("preview changed exact retained asset metadata: " + id);
    std::vector<std::string> required;
    // Stair solids and hosted closure are admitted by the typed compound lane;
    // the generic geometry adapter's explicit required list excludes stairs.
    for (const auto& entity : complete_edited) if (entity.type == "railing") required.push_back(entity.id);
    validate_architectural_geometry_changes(source, preview, required);
    return command;
} catch (const Json::exception& error) {
    reject(std::string("malformed captured source/editor: ") + error.what());
} catch (const Standard_Failure& error) {
    const auto* detail = error.GetMessageString();
    reject(std::string("native admission failed: ") + (detail ? detail : "Open CASCADE failure"));
}
} // namespace sketch
