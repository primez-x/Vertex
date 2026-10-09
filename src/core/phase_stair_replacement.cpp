#include "sketch/phase_stair_replacement.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/stair_attachment_integrity.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = PhaseStairReplacementEntities;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t entity_limit = 65536, identity_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024, geometry_limit = 100000;
constexpr std::size_t source_nodes = 4 * 1024 * 1024, source_bytes = 64 * 1024 * 1024;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Phase stair replacement: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
std::string identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    auto result = value.get<std::string>(); identity(result); return result;
}
void local_identity(const std::string& id, std::size_t limit = 128) {
    if (id.empty() || id.size() > limit || std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isspace(c);
    })) reject("qualified source local identity must be bounded and nonblank");
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key); return found == value.end() ? nullptr : &*found;
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
void keys(const Json& value, const Ids& expected) {
    if (!value.is_object() || value.size() != expected.size()) reject("authoring fields are not closed");
    for (const auto& name : expected) if (!value.contains(name)) reject("authoring field is missing: " + name);
}
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    std::size_t node_limit{source_nodes}, byte_limit{source_bytes}, depth_limit{64}, collection_limit{source_nodes};
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("JSON string/key budget exceeded");
        bytes += value.size(); values.insert(value);
    }
    void read(const Json& root) {
        std::vector<std::pair<const Json*, std::size_t>> pending{{&root, 0}};
        while (!pending.empty()) {
            const auto [value, depth] = pending.back(); pending.pop_back();
            if (depth > depth_limit || ++nodes > node_limit) reject("JSON node/nesting budget exceeded");
            if (value->is_number_float() && !std::isfinite(value->get<double>())) reject("nonfinite JSON scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size() > byte_limit - bytes) reject("JSON binary budget exceeded");
                bytes += value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size() > collection_limit || value->size() > node_limit - nodes ||
                pending.size() > node_limit - nodes - value->size()) reject("JSON collection/pending budget exceeded");
            if (value->is_object()) for (const auto& [name, child] : value->items()) {
                text(name); pending.emplace_back(&child, depth + 1);
            } else for (const auto& child : *value) pending.emplace_back(&child, depth + 1);
        }
    }
};
void proof_budget(const Json& value) {
    Strings budget; budget.node_limit = 65536; budget.byte_limit = proof_limit;
    budget.depth_limit = 32; budget.collection_limit = identity_limit; budget.read(value);
    if (value.dump().size() > proof_limit) reject("authoring byte budget exceeded");
}
Strings source_budget(const Entities& actual) {
    if (actual.size() > entity_limit) reject("actual entity budget exceeded");
    Strings result;
    for (const auto* name : {"id", "type", "properties", "required", "extensions"}) result.text(name);
    std::size_t phase_work{};
    for (const auto& [id, entity] : actual) {
        identity(id);
        if (id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source requires actual identified entity envelopes: " + id);
        result.text(id); result.text(entity.type); result.read(entity.properties); result.read(entity.extensions);
        if (entity.type != "model_phases") continue;
        const auto model = field(entity.properties, "model");
        const auto members = model ? field(*model, "entity_ids") : nullptr;
        const auto alternatives = model ? field(*model, "alternatives") : nullptr;
        if (!members || !members->is_array() || members->size() > entity_limit ||
            !alternatives || !alternatives->is_array() || alternatives->size() > identity_limit)
            reject("actual registry inventory is not bounded: " + id);
        if (members->size() > (2000000 - phase_work) / (alternatives->size() + 1))
            reject("aggregate phase state work budget exceeded");
        phase_work += members->size() * (alternatives->size() + 1);
    }
    return result;
}
bool touches(const Json& value, const Ids& ids) {
    if (ids.empty()) return false;
    if (value.is_string()) return ids.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) if (ids.contains(key) || touches(child, ids)) return true;
    } else if (value.is_array()) for (const auto& child : value) if (touches(child, ids)) return true;
    return false;
}
void diagnostic(PhaseStairReplacementPlan& plan, const std::string& id, const std::string& reason) {
    const PhaseStairReplacementDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end())
        plan.diagnostics.push_back(item);
}
bool physical_owner(const Entity& entity) { return entity.type == "stair" || entity.type == "railing"; }
std::optional<std::string> host_id(const Entity& entity) {
    if (entity.type != "railing") return std::nullopt;
    const auto host = field(entity.properties, "host");
    const auto stair = host ? field(*host, "stair_id") : nullptr;
    if (!stair || !stair->is_string()) return std::nullopt;
    // A raw slot selects admission; only the physical codec supplies authority.
    const auto railing = decode_railing_properties(entity.id, entity.properties);
    if (railing.host) return railing.host->stair_id;
    if (railing.landing_host) return railing.landing_host->stair_id;
    reject("host slot has no supported railing binding: " + entity.id);
}
struct Memberships {
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> owners;
};
Memberships memberships(const Entities& actual) {
    Memberships result;
    for (const auto& [id, entity] : actual) {
        if (entity.type != "model_phases") continue;
        auto model = ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& owner_id : model.entity_ids()) {
            const auto owner = actual.find(owner_id);
            if (owner == actual.end() || !is_model_phase_entity_type(owner->second.type))
                reject("registry has a missing or unsupported actual owner: " + owner_id);
            if (!result.owners.emplace(owner_id, id).second) reject("overlapping actual registries: " + owner_id);
        }
        result.models.emplace(id, std::move(model));
    }
    return result;
}
bool baseline(const ModelPhases& model, const std::string& id) {
    return std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id);
}
void static_pose(const Entity& before, const Entity& after) {
    for (const auto* name : {"base_position_m", "orientation_rad", "vertical_placement"}) {
        const auto old = field(before.properties, name), current = field(after.properties, name);
        if ((!old != !current) || (old && *old != *current))
            reject("profile-only edit cannot change pose or vertical placement: " + before.id + "/" + name);
    }
}
Json canonical_profile(const Entity& entity) {
    return entity.type == "stair" ? encode_stair_properties(decode_stair_properties(entity.id, entity.properties))
        : encode_railing_properties(decode_railing_properties(entity.id, entity.properties));
}
bool same_receipts(const Entity& before, const Entity& after) {
    const auto old = field(before.properties, "quantity_entries"), current = field(after.properties, "quantity_entries");
    return (!old && !current) || (old && current && *old == *current && old->dump() == current->dump());
}
std::optional<PhaseStairReplacementRequest> classify(const Entities& actual, const Entities& physical,
    const std::vector<StairObjectEditIntent>& edits, const Memberships& scope) {
    std::optional<PhaseStairReplacementRequest> result;
    Ids registries;
    for (const auto& edit : edits) {
        if (exact(actual.at(edit.object_id), physical.at(edit.object_id))) continue;
        const auto member = scope.owners.find(edit.object_id);
        if (member == scope.owners.end()) continue;
        const auto& model = scope.models.at(member->second);
        const auto state = model.active_state(); const auto owner_state = state.find(edit.object_id);
        if (owner_state == state.end() || owner_state->second == ModelPhase::demolished)
            reject("changed owner is inactive in actual saved design: " + edit.object_id);
        registries.insert(member->second);
        if (!baseline(model, edit.object_id) || !model.active_alternative()) continue;
        if (canonical_profile(actual.at(edit.object_id)) == canonical_profile(physical.at(edit.object_id))) {
            if (!same_receipts(actual.at(edit.object_id), physical.at(edit.object_id)))
                reject("receipt-only baseline edit has no changed physical replacement authority: " + edit.object_id);
            continue; // No mathematical profile/topology/host change grants a copy.
        }
        if (result && result->registry_id != member->second) reject("baseline cohort spans foreign registries");
        if (!result) result = PhaseStairReplacementRequest{member->second, *model.active_alternative(), {}};
        result->seed_object_ids.push_back(edit.object_id);
    }
    if (result) {
        // Ordinary/proposed-only batches retain their existing typed movement
        // authority. A real baseline replacement grants profile authority only,
        // including every ordinary member of a mixed replacement cohort.
        for (const auto& edit : edits) static_pose(actual.at(edit.object_id), physical.at(edit.object_id));
        for (const auto& id : registries) if (id != result->registry_id) reject("mixed cohort includes a foreign registry");
        std::sort(result->seed_object_ids.begin(), result->seed_object_ids.end());
    }
    return result;
}
bool supported_catalog(const Json& raw) {
    const auto schema = field(raw, "schema");
    if (!schema || !schema->is_string()) return false;
    for (int version = 1; version <= 7; ++version)
        if (*schema == "sketch.assemblies.v" + std::to_string(version)) return true;
    return false;
}
void catalog_bounds(const Json& raw, std::size_t& inventory) {
    for (const auto* name : {"materials", "types", "instances"}) {
        const auto rows = field(raw, name);
        if (!rows || !rows->is_array() || rows->size() > entity_limit - inventory)
            reject("affected catalog inventory is not bounded");
        inventory += rows->size();
    }
}
void context(const Entities& actual, const ProjectOrganization& organization, const Entity& entity) {
    if (entity.type == "assembly_model") {
        struct Binding { const char* key; const char* type; };
        constexpr Binding bindings[]{{"property_id", "property"}, {"building_id", "building"},
            {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}};
        // Catalogs are not placeable organization nodes. Validate their actual
        // optional context through the catalog contract instead.
        for (const auto& binding : bindings) {
            const auto value = field(entity.properties, binding.key); if (!value) continue;
            if (!value->is_string()) reject("affected catalog has malformed context: " + entity.id);
            const auto owner = actual.find(value->get_ref<const std::string&>());
            if (owner == actual.end() || owner->second.type != binding.type)
                reject("affected catalog has unresolved actual context: " + entity.id);
        }
        return;
    }
    const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
        entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
        entity.properties.contains("level_id") || entity.properties.contains("wall_id");
    const auto node = organization.nodes.find(entity.id);
    if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
        reject("affected owner has unresolved actual drawing context: " + entity.id);
    if (!entity.properties.contains("material_assignment")) return;
    const auto& assignment = entity.properties.at("material_assignment");
    if (!assignment.is_object() || !assignment.contains("version") || !assignment.at("version").is_number_integer() ||
        assignment.at("version") != 1 || !assignment.contains("catalog_id") || !assignment.at("catalog_id").is_string() ||
        !assignment.contains("material_id") || !assignment.at("material_id").is_string())
        reject("affected owner has unsupported material assignment: " + entity.id);
    const auto catalog_id = assignment.at("catalog_id").get<std::string>();
    const auto catalog = actual.find(catalog_id);
    if (catalog == actual.end() || catalog->second.type != "assembly_model") reject("material requires its actual catalog");
    const auto model = AssemblyModel::from_json(catalog->second.properties.at("model"));
    const auto material_id = assignment.at("material_id").get<std::string>();
    if (std::none_of(model.materials().begin(), model.materials().end(), [&](const auto& row) { return row.id == material_id; }))
        reject("material is absent from its actual catalog: " + entity.id);
}
void admit_instances(const Entities& actual, const std::set<PhaseStairReplacementHostedInstanceKey>& selected) {
    AssemblyExpansionBudget budget;
    std::map<std::string, AssemblyModel, std::less<>> models;
    std::vector<AssemblyExpansion> expansions;
    std::vector<std::pair<BuildingObject, AssemblyTransform>> legacy;
    std::size_t work{};
    for (const auto& key : selected) {
        if (!models.contains(key.first)) models.emplace(key.first, AssemblyModel::from_json(actual.at(key.first).properties.at("model")));
        const auto& model = models.at(key.first);
        const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == key.second; });
        if (row == model.instances().end() || !row->placement) reject("actual selected hosted row is missing");
        const auto& placement = *row->placement;
        const auto host = actual.find(placement.host_entity_id);
        if (host == actual.end() || !physical_owner(host->second)) reject("hosted row requires its actual stair/railing host");
        auto expansion = model.expand(*row, budget);
        if (!expansion.profiles.empty()) expansions.push_back(std::move(expansion));
        else {
            const auto resolved = host_id(host->second) ? host->second : resolve_vertical_placement(actual, host->second);
            auto object = decode_building_entity(resolved);
            std::size_t cost{};
            if (const auto stair = std::get_if<StairFlight>(&object)) cost = stair->riser_count + stair->landings.size() + 1;
            else {
                const auto& rail = std::get<Railing>(object);
                if (const auto rail_host = host_id(host->second))
                    cost = derive_hosted_railing_layout(rail, decode_stair_properties(*rail_host,
                        resolve_vertical_placement(actual, actual.at(*rail_host)).properties)).posts.size() + 1;
                else cost = static_cast<std::size_t>(std::ceil(rail.length / rail.post_spacing)) + 2;
            }
            if (cost > geometry_limit - work) reject("host-derived component native work budget exceeded");
            work += cost;
            legacy.emplace_back(std::move(object), AssemblyTransform{{placement.translation_m.x,
                placement.translation_m.y, placement.translation_z_m}, placement.rotation_radians,
                placement.scale, placement.mirrored_y, placement.vertical_scale});
        }
    }
    // Expansion and aggregate host-derived bounds precede native construction.
    for (const auto& expansion : expansions) (void)make_assembly_geometry(expansion);
    for (const auto& [object, transform] : legacy) (void)transform_assembly_shape(make_building_shape(object, actual), transform);
}
bool affected_overlay(const Json& row, const Ids& owners) {
    const auto owner = field(row, "object_id");
    const auto binding = field(row, "dimension_binding");
    const auto bound_owner = binding ? field(*binding, "object_id") : nullptr;
    return (owner && owner->is_string() && owners.contains(owner->get_ref<const std::string&>())) ||
        (bound_owner && bound_owner->is_string() && owners.contains(bound_owner->get_ref<const std::string&>()));
}
// Strip only codec-admitted identity slots from an admission scratch copy.
// Source JSON and opaque siblings never acquire rewrite authority.
Entity remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "stair") {
        (void)decode_stair_properties(entity.id, p);
        for (const auto* name : {"flights", "landings"})
            if (p.contains(name)) for (auto& child : p.at(name)) child.erase("id");
    } else if (entity.type == "railing") {
        (void)decode_railing_properties(entity.id, p);
        if (p.contains("host")) for (const auto* name : {"stair_id", "flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"})
            p.at("host").erase(name);
    } else if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model"));
        for (auto& row : p.at("model").at("instances")) {
            row.erase("id");
            if (row.contains("placement")) row.at("placement").erase("host_entity_id");
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
                row.erase("id"); row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null())
                    row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) if (row.at("target_kind") == "object") row.erase("target_id");
    }
    if (physical_owner(entity) && p.contains("material_assignment")) p.at("material_assignment").erase("catalog_id");
    return entity;
}
Entity catalog_local_remainder(Entity entity) {
    auto& model = entity.properties.at("model");
    for (auto& material : model.at("materials")) material.erase("id");
    const auto clear_materials = [](Json& row) { row.erase("material_overrides"); };
    for (auto& type : model.at("types")) {
        type.erase("id"); type.erase("materials");
        if (type.contains("profiles")) for (auto& profile : type.at("profiles")) { profile.erase("id"); profile.erase("material_slot"); }
        if (type.contains("parts")) for (auto& part : type.at("parts")) {
            part.erase("id"); part.erase("type_id"); clear_materials(part);
        }
    }
    for (auto& row : model.at("instances")) {
        row.erase("type_id"); clear_materials(row);
        if (row.contains("nested_overrides")) for (auto& change : row.at("nested_overrides")) {
            change.erase("part_path"); clear_materials(change);
        }
    }
    return entity;
}
struct Derivation {
    PhaseStairReplacementPlan plan;
    Entities physical;
    Ids copied_owners;
    Ids catalogs;
    EmbeddedAssemblyPresentationIds original_aliases;
};
void topology_dependencies(const Entities& actual, const std::vector<StairObjectEditIntent>& edits) {
    std::map<std::string, const StairObjectEditIntent*, std::less<>> targets;
    std::size_t bytes{};
    for (const auto& edit : edits) {
        const auto encoded = encode_stair_object_edit_intent(edit);
        const auto size = encoded.dump().size();
        if (size > proof_limit - bytes) reject("typed profile batch byte budget exceeded");
        bytes += size;
        if (!targets.emplace(edit.object_id, &edit).second) reject("duplicate typed profile target");
    }
    for (const auto& [id, entity] : actual) {
        if (entity.type != "railing") continue;
        const Json* binding = field(entity.properties, "host");
        if (const auto rail_edit = targets.find(id); rail_edit != targets.end())
            binding = field(rail_edit->second->profile_fields, "host");
        const auto stair_id = binding ? field(*binding, "stair_id") : nullptr;
        if (!stair_id || !stair_id->is_string()) continue;
        const auto stair_edit = targets.find(stair_id->get_ref<const std::string&>());
        if (stair_edit == targets.end()) continue;
        const auto owner = actual.find(stair_edit->first);
        if (owner == actual.end() || owner->second.type != "stair") continue;
        const auto& profile = stair_edit->second->profile_fields;
        for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"}) {
            const auto child = field(*binding, name); if (!child || !child->is_string()) continue;
            const auto rows = field(profile, std::string_view(name) == "landing_id" ? "landings" : "flights");
            const bool survives = rows && rows->is_array() && std::any_of(rows->begin(), rows->end(), [&](const auto& row) {
                const auto local = field(row, "id"); return local && *local == *child;
            });
            if (!survives) reject("retired topology blocks rail " + id + " dependency " + stair_edit->first +
                "/host/" + name + ":" + child->get<std::string>());
        }
    }
}
Derivation derive(const Entities& actual, const std::vector<StairObjectEditIntent>& edits,
    const std::string& registry_id, const std::string& alternative_id) {
    Derivation result;
    auto& plan = result.plan;
    plan.registry_id = registry_id; plan.alternative_id = alternative_id;
    try {
        (void)source_budget(actual);
        if (edits.empty() || edits.size() > identity_limit) reject("requires a bounded nonempty typed profile batch");
        topology_dependencies(actual, edits);
        result.physical = replay_stair_object_edit_entities(actual, edits);
        const auto scope = memberships(actual);
        const auto request = classify(actual, result.physical, edits, scope);
        if (!request) reject("batch has no actually changed active shared-baseline owner");
        if ((!registry_id.empty() && registry_id != request->registry_id) ||
            (!alternative_id.empty() && alternative_id != request->alternative_id))
            reject("supplied destination differs from actual saved active membership");
        plan.registry_id = request->registry_id; plan.alternative_id = request->alternative_id;
        plan.seed_object_ids = request->seed_object_ids;
        result.copied_owners.insert(plan.seed_object_ids.begin(), plan.seed_object_ids.end());
        const auto& model = scope.models.at(plan.registry_id);
        const auto states = model.active_state();
        for (const auto& [id, entity] : actual) {
            const auto raw_host = field(entity.properties, "host");
            const auto raw_stair = raw_host ? field(*raw_host, "stair_id") : nullptr;
            if (entity.type != "railing" || !raw_stair || !raw_stair->is_string() ||
                !result.copied_owners.contains(raw_stair->get_ref<const std::string&>())) continue;
            const auto host = host_id(entity);
            if (!host || actual.at(*host).type != "stair") reject("affected rail lacks an actual canonical stair host: " + id);
            const auto membership = scope.owners.find(id);
            if (membership == scope.owners.end() || membership->second != plan.registry_id)
                reject("attached rail lacks the stair's unambiguous registry: " + id);
            const auto state = states.find(id);
            if (state == states.end() || state->second == ModelPhase::demolished) continue;
            if (state->second == ModelPhase::proposed) plan.retained_rehost_object_ids.push_back(id);
            else if (baseline(model, id)) result.copied_owners.insert(id);
            else reject("active attached rail has no baseline/proposed authority: " + id);
        }
        const auto organization = organize_project(actual);
        for (const auto& id : result.copied_owners) {
            context(actual, organization, actual.at(id));
            const auto& edited = result.physical.at(id);
            if (edited.type != "stair") continue;
            for (const auto* name : {"flights", "landings"}) if (edited.properties.contains(name))
                for (const auto& child : edited.properties.at(name)) {
                    const auto child_id = child.at("id").get<std::string>(); identity(child_id);
                    plan.required_child_ids.emplace_back(id, child_id);
                }
        }
        const auto active = constraint_phase_scope(actual);
        std::size_t inventory{};
        std::set<PhaseStairReplacementHostedInstanceKey> selected;
        for (const auto& [id, entity] : actual) {
            if (entity.type != "assembly_model" ||
                (!touches(entity.properties, result.copied_owners) && !touches(entity.extensions, result.copied_owners))) continue;
            const auto& raw = entity.properties.at("model");
            if (!supported_catalog(raw)) reject("affected catalog has no supported schema 1..7: " + id);
            catalog_bounds(raw, inventory);
            const auto catalog = AssemblyModel::from_json(raw);
            bool included = false;
            for (const auto& row : catalog.instances()) if (row.placement && result.copied_owners.contains(row.placement->host_entity_id)) {
                selected.emplace(id, row.id); included = true;
            }
            if (!included) continue; // Opaque-only references are refused below.
            if (active.inactive_owner_ids.contains(id)) reject("actual selected hosted catalog is inactive: " + id);
            context(actual, organization, entity);
            result.catalogs.insert(id);
        }
        plan.required_hosted_instance_ids.assign(selected.begin(), selected.end());
        if (!selected.empty()) {
            admit_instances(actual, selected);
            const auto aliases = embedded_assembly_presentation_ids(actual);
            for (const auto& key : selected) result.original_aliases.emplace(key, aliases.at(key));
        }
        Ids presentation = result.copied_owners;
        for (const auto& [key, alias] : result.original_aliases) { (void)key; presentation.insert(alias); }
        std::set<PhaseStairReplacementOverlayKey> overlays;
        Ids overlay_spellings;
        for (const auto& [id, entity] : actual) {
            if (entity.type != kSheetViewEntityType ||
                (!touches(entity.properties, presentation) && !touches(entity.extensions, presentation))) continue;
            const auto views = decode_sheet_view_entity(entity);
            for (const auto& view : views.views()) for (const auto& overlay : view.overlays) {
                if (!presentation.contains(overlay.object_id) &&
                    (!overlay.dimension_binding || !presentation.contains(overlay.dimension_binding->object_id))) continue;
                local_identity(view.id, proof_limit); local_identity(overlay.id);
                if (!overlays.emplace(id, view.id, overlay.id).second) reject("affected overlay has duplicate qualified identity");
                overlay_spellings.insert(overlay.id);
            }
        }
        plan.required_overlay_ids.assign(overlays.begin(), overlays.end());
        Ids children;
        for (const auto& id : result.copied_owners) if (actual.at(id).type == "stair")
            for (const auto* name : {"flights", "landings"}) if (actual.at(id).properties.contains(name))
                for (const auto& row : actual.at(id).properties.at(name)) children.insert(row.at("id").get<std::string>());
        Ids affected = presentation; affected.insert(children.begin(), children.end());
        for (const auto& [id, entity] : actual) {
            if (!result.catalogs.contains(id) && !result.copied_owners.contains(id) &&
                !touches(entity.properties, affected) && !touches(entity.extensions, affected) &&
                !touches(entity.properties, overlay_spellings) && !touches(entity.extensions, overlay_spellings)) continue;
            try {
                auto scratch = remainder(entity);
                auto external = scratch;
                if (entity.type == kSheetViewEntityType) for (auto& view : external.properties.at("model").at("views")) view.erase("id");
                if (touches(external.properties, affected) || touches(external.extensions, affected))
                    diagnostic(plan, id, "affected opaque reference has no qualified stair replacement codec");
                if (result.catalogs.contains(id)) {
                    auto local = catalog_local_remainder(scratch);
                    Ids local_names; auto rows = Json::array();
                    const auto& source_rows = entity.properties.at("model").at("instances");
                    for (std::size_t i = 0; i < source_rows.size(); ++i) {
                        const auto name = source_rows.at(i).at("id").get<std::string>(); local_names.insert(name);
                        if (selected.contains({id, name})) rows.push_back(local.properties.at("model").at("instances").at(i));
                    }
                    local.properties.at("model").at("instances") = std::move(rows);
                    if (touches(local.properties, local_names) || touches(local.extensions, local_names))
                        diagnostic(plan, id, "copied catalog has an opaque reference to an original local instance");
                }
                if (entity.type == kSheetViewEntityType) {
                    for (const auto& view : scratch.properties.at("model").at("views")) {
                        Ids local_names;
                        for (const auto& key : overlays) if (std::get<0>(key) == id && std::get<1>(key) == view.at("id").get<std::string>())
                            local_names.insert(std::get<2>(key));
                        auto local = view; local.erase("id");
                        if (touches(local, local_names)) diagnostic(plan, id, "affected overlay-local opaque reference cannot be copied");
                    }
                    if (touches(scratch.extensions, overlay_spellings)) diagnostic(plan, id, "affected overlay reference in opaque view extensions");
                } else if (touches(scratch.properties, overlay_spellings) || touches(scratch.extensions, overlay_spellings))
                    diagnostic(plan, id, "affected overlay reference outside its qualified saved view");
            } catch (const std::exception& error) { diagnostic(plan, id, error.what()); }
        }
        Ids required = result.copied_owners; required.insert(result.catalogs.begin(), result.catalogs.end());
        plan.required_entity_ids.assign(required.begin(), required.end());
        std::sort(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (required.size() + plan.required_child_ids.size() + selected.size() + overlays.size() > identity_limit)
            reject("combined physical/child/hosted/overlay identity budget exceeded");
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); diagnostic(plan, {}, std::string("native geometry admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason};
    });
    return result;
}
void remap_field(Json& value, const char* name, const PhaseStairReplacementIdentityMap& mapping) {
    const auto old = field(value, name);
    if (!old) return;
    const auto found = mapping.find(old->get<std::string>());
    if (found != mapping.end()) value.at(name) = found->second;
}
void remap_host(Entity& entity, const PhaseStairReplacementAuthoring& authoring) {
    const auto actual_host = host_id(entity);
    if (!actual_host || !authoring.identities.contains(*actual_host)) return;
    auto& host = entity.properties.at("host");
    for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"}) {
        const auto child = field(host, name); if (!child) continue;
        const auto mapped = authoring.child_identities.find({*actual_host, child->get<std::string>()});
        if (mapped == authoring.child_identities.end())
            reject("retired topology prevents valid rail rehost: " + entity.id + "/host/" + name);
        host.at(name) = mapped->second;
    }
    host.at("stair_id") = authoring.identities.at(*actual_host);
}
void admit_rebound_quantities(const Entity& before, const Entity& after) {
    const auto entries = field(before.properties, "quantity_entries"); if (!entries) return;
    for (const auto& [pointer, receipt] : entries->items()) {
        (void)receipt;
        if (pointer == "/host" || pointer.starts_with("/host/")) {
            const auto old = field(before.properties, "host"), current = field(after.properties, "host");
            if ((!old != !current) || (old && *old != *current))
                reject("opaque host quantity binding cannot follow replacement: " + before.id + ":" + pointer);
        }
        for (const auto* name : {"flights", "landings"}) {
            const auto prefix = std::string("/") + name;
            if (pointer != prefix && !pointer.starts_with(prefix + "/")) continue;
            const auto old = field(before.properties, name), current = field(after.properties, name);
            if ((!old && !current) || (old && current && *old == *current)) continue;
            bool known_dimension = false;
            if (old && old->is_array()) for (std::size_t i = 0; i < old->size(); ++i) {
                const Ids dimensions = std::string_view(name) == "flights" ? Ids{"going_m", "width_m"}
                    : Ids{"depth_m", "thickness_m", "return_gap_m"};
                for (const auto& dimension : dimensions)
                    if (pointer == prefix + "/" + std::to_string(i) + "/" + dimension && old->at(i).contains(dimension))
                        known_dimension = true;
            }
            // Known dimensions retain their already admitted index pointers.
            // Other pointers have no codec for changing child identity binding.
            if (!known_dimension) reject("opaque child quantity binding cannot follow replacement: " + before.id + ":" + pointer);
        }
    }
}
void presentation(Entities& candidate, const Entities& actual, const PhaseStairReplacementIdentityMap& mapping,
    const PhaseStairReplacementOverlayIdentityMap& overlay_ids) {
    Ids owners; for (const auto& [old, proposed] : mapping) { (void)proposed; owners.insert(old); }
    for (const auto& [id, original] : actual) {
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            (void)decode_sheet_view_entity(original);
            for (auto& view : candidate.at(id).properties.at("model").at("views")) {
                const auto view_id = view.at("id").get<std::string>();
                if (view.contains("object_ids")) {
                    auto& rows = view.at("object_ids"); const auto retained = rows;
                    for (const auto& row : retained) if (owners.contains(row.get<std::string>())) rows.push_back(mapping.at(row.get<std::string>()));
                }
                auto& p = view.at("presentation");
                if (p.contains("appearance") && !p.at("appearance").is_null()) {
                    auto& rows = p.at("appearance").at("objects"); const auto retained = rows;
                    for (auto row : retained) if (owners.contains(row.at("object_id").get<std::string>())) {
                        remap_field(row, "object_id", mapping); rows.push_back(std::move(row));
                    }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays"); const auto retained = rows;
                    for (auto row : retained) if (affected_overlay(row, owners)) {
                        row.at("id") = overlay_ids.at({id, view_id, row.at("id").get<std::string>()});
                        remap_field(row, "object_id", mapping);
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) remap_field(row.at("dimension_binding"), "object_id", mapping);
                        // Profile-only: unbound view-plane coordinates and bound
                        // dimension axis/offset remain exact source values.
                        rows.push_back(std::move(row));
                    }
                }
            }
            validate_sheet_view_entity(candidate.at(id));
        } else if (original.type == kAnnotationEntityType) {
            auto& rows = candidate.at(id).properties.at("state").at("overrides"); const auto retained = rows;
            for (auto row : retained) if (row.at("target_kind") == "object" && owners.contains(row.at("target_id").get<std::string>())) {
                remap_field(row, "target_id", mapping); rows.push_back(std::move(row));
            }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
void admit_physical(const Entities& candidate, const Ids& owners) {
    validate_stair_attachment_state(candidate);
    const auto organization = organize_project(candidate);
    std::map<std::string, BuildingObject, std::less<>> objects;
    std::size_t work{};
    for (const auto& id : owners) {
        const auto& entity = candidate.at(id); context(candidate, organization, entity);
        const auto resolved = host_id(entity) ? entity : resolve_vertical_placement(candidate, entity);
        auto object = decode_building_entity(resolved);
        std::size_t cost{};
        if (const auto stair = std::get_if<StairFlight>(&object)) cost = stair->riser_count + stair->landings.size() + 1;
        else {
            const auto& rail = std::get<Railing>(object);
            if (const auto host = host_id(entity)) cost = derive_hosted_railing_layout(rail, decode_stair_properties(*host,
                resolve_vertical_placement(candidate, candidate.at(*host)).properties)).posts.size() + 1;
            else cost = static_cast<std::size_t>(std::ceil(rail.length / rail.post_spacing)) + 2;
        }
        if (cost > geometry_limit - work) reject("complete candidate native geometry budget exceeded");
        work += cost; objects.emplace(id, std::move(object));
    }
    for (const auto& [id, object] : objects) { (void)id; (void)make_building_shape(object, candidate); }
}
} // namespace

bool PhaseStairReplacementPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& row) { return row.blocking; });
}
std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(const Entities& actual,
    const std::vector<StairObjectEditIntent>& edits) {
    try {
        (void)source_budget(actual);
        if (edits.size() > identity_limit) reject("typed profile target budget exceeded");
        topology_dependencies(actual, edits);
        const auto physical = replay_stair_object_edit_entities(actual, edits);
        return classify(actual, physical, edits, memberships(actual));
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native geometry admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual profile request: ") + error.what()); }
}
PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(const Entities& actual,
    const std::vector<StairObjectEditIntent>& edits, const std::string& registry_id, const std::string& alternative_id) {
    return derive(actual, edits, registry_id, alternative_id).plan;
}

Json encode_phase_stair_replacement_authoring(const PhaseStairReplacementAuthoring& authoring) {
    identity(authoring.registry_id); identity(authoring.alternative_id);
    if (authoring.edits.empty() || authoring.edits.size() > identity_limit || authoring.identities.empty() ||
        authoring.identities.size() > identity_limit || authoring.child_identities.size() > identity_limit - authoring.identities.size() ||
        authoring.hosted_instance_identities.size() > identity_limit - authoring.identities.size() - authoring.child_identities.size() ||
        authoring.overlay_identities.size() > identity_limit - authoring.identities.size() - authoring.child_identities.size() - authoring.hosted_instance_identities.size())
        reject("authoring inventories exceed combined bounds or lack typed edits/owners");
    Ids targets, fresh; Json edits = Json::array(), identities = Json::object();
    std::size_t edit_bytes{};
    for (const auto& edit : authoring.edits) {
        if (!targets.insert(edit.object_id).second) reject("duplicate typed profile target");
        auto value = encode_stair_object_edit_intent(edit);
        const auto size = value.dump().size();
        if (size > proof_limit - edit_bytes) reject("typed profile batch byte budget exceeded");
        edit_bytes += size; edits.push_back(std::move(value));
    }
    const auto reserve = [&](const std::string& destination) {
        identity(destination); if (!fresh.insert(destination).second) reject("fresh destinations overlap across qualified mappings");
    };
    for (const auto& [old, proposed] : authoring.identities) {
        identity(old); reserve(proposed); if (old == proposed) reject("entity destination is not fresh"); identities[old] = proposed;
    }
    Json children = Json::array(), hosted = Json::array(), overlays = Json::array();
    for (const auto& [key, proposed] : authoring.child_identities) {
        identity(key.first); identity(key.second); reserve(proposed);
        if (key.second == proposed) reject("child destination is not fresh");
        children.push_back({{"owner_id", key.first}, {"child_id", key.second}, {"proposed_child_id", proposed}});
    }
    std::size_t hosted_bytes{};
    for (const auto& [key, proposed] : authoring.hosted_instance_identities) {
        identity(key.first); local_identity(key.second, proof_limit); reserve(proposed);
        const auto size = key.first.size() + key.second.size() + proposed.size();
        if (size > proof_limit - hosted_bytes) reject("qualified hosted identity byte budget exceeded");
        hosted_bytes += size;
        if (key.second == proposed) reject("hosted destination is not fresh");
        hosted.push_back({{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_instance_id", proposed}});
    }
    for (const auto& [key, proposed] : authoring.overlay_identities) {
        identity(std::get<0>(key)); local_identity(std::get<1>(key), proof_limit); local_identity(std::get<2>(key)); reserve(proposed);
        if (std::get<2>(key) == proposed) reject("overlay destination is not fresh");
        overlays.push_back({{"view_entity_id", std::get<0>(key)}, {"saved_view_id", std::get<1>(key)},
            {"overlay_id", std::get<2>(key)}, {"proposed_overlay_id", proposed}});
    }
    Json result{{"version", 1}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
        {"edits", std::move(edits)}, {"identities", std::move(identities)}, {"child_identities", std::move(children)},
        {"hosted_instance_identities", std::move(hosted)}, {"overlay_identities", std::move(overlays)}};
    proof_budget(result); return result;
}
PhaseStairReplacementAuthoring decode_phase_stair_replacement_authoring(const Json& value) {
    try {
        proof_budget(value);
        keys(value, {"version", "registry_id", "alternative_id", "edits", "identities", "child_identities", "hosted_instance_identities", "overlay_identities"});
        if (!value.at("version").is_number_integer() || value.at("version") != 1 || !value.at("edits").is_array() ||
            !value.at("identities").is_object() || !value.at("child_identities").is_array() ||
            !value.at("hosted_instance_identities").is_array() || !value.at("overlay_identities").is_array()) reject("unsupported authoring v1 shape");
        PhaseStairReplacementAuthoring result;
        result.registry_id = identity(value.at("registry_id")); result.alternative_id = identity(value.at("alternative_id"));
        for (const auto& edit : value.at("edits")) result.edits.push_back(decode_stair_object_edit_intent(edit));
        for (const auto& [old, proposed] : value.at("identities").items()) {
            identity(old); result.identities.emplace(old, identity(proposed));
        }
        for (const auto& row : value.at("child_identities")) {
            keys(row, {"owner_id", "child_id", "proposed_child_id"});
            if (!result.child_identities.emplace(std::pair{identity(row.at("owner_id")), identity(row.at("child_id"))}, identity(row.at("proposed_child_id"))).second)
                reject("duplicate qualified child source identity");
        }
        for (const auto& row : value.at("hosted_instance_identities")) {
            keys(row, {"catalog_id", "instance_id", "proposed_instance_id"});
            if (!row.at("instance_id").is_string()) reject("hosted local identity must be a string");
            if (!result.hosted_instance_identities.emplace(std::pair{identity(row.at("catalog_id")), row.at("instance_id").get<std::string>()}, identity(row.at("proposed_instance_id"))).second)
                reject("duplicate qualified hosted source identity");
        }
        for (const auto& row : value.at("overlay_identities")) {
            keys(row, {"view_entity_id", "saved_view_id", "overlay_id", "proposed_overlay_id"});
            if (!row.at("saved_view_id").is_string() || !row.at("overlay_id").is_string()) reject("overlay local identities must be strings");
            if (!result.overlay_identities.emplace(PhaseStairReplacementOverlayKey{identity(row.at("view_entity_id")),
                row.at("saved_view_id").get<std::string>(), row.at("overlay_id").get<std::string>()}, identity(row.at("proposed_overlay_id"))).second)
                reject("duplicate qualified overlay source identity");
        }
        if (encode_phase_stair_replacement_authoring(result) != value) reject("qualified authoring rows are not canonical");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed authoring: ") + error.what()); }
}

Entities replay_phase_stair_replacement_authoring(const Entities& actual, const PhaseStairReplacementAuthoring& authoring) {
    try {
        const auto proof = encode_phase_stair_replacement_authoring(authoring);
        const auto derived = derive(actual, authoring.edits, authoring.registry_id, authoring.alternative_id);
        const auto& plan = derived.plan;
        if (!plan.ready()) reject(plan.diagnostics.front().entity_id + ": " + plan.diagnostics.front().reason);
        const Ids entities(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        const std::set<PhaseStairReplacementChildKey> children(plan.required_child_ids.begin(), plan.required_child_ids.end());
        const std::set<PhaseStairReplacementHostedInstanceKey> hosted(plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        const std::set<PhaseStairReplacementOverlayKey> overlays(plan.required_overlay_ids.begin(), plan.required_overlay_ids.end());
        if (authoring.identities.size() != entities.size() || authoring.child_identities.size() != children.size() ||
            authoring.hosted_instance_identities.size() != hosted.size() || authoring.overlay_identities.size() != overlays.size())
            reject("requires complete exact actual-source qualified mapping inventories");
        auto occupied = source_budget(actual);
        EmbeddedAssemblyPresentationIds original_aliases;
        if (!hosted.empty()) {
            original_aliases = embedded_assembly_presentation_ids(actual);
            for (const auto& [key, alias] : original_aliases) { (void)key; occupied.text(alias); }
        }
        Strings authored; authored.read(proof);
        Strings source_frame;
        for (const auto& edit : authoring.edits) source_frame.read(encode_stair_object_edit_intent(edit));
        Ids fresh;
        const auto reserve = [&](const std::string& id) {
            if (occupied.values.contains(id) || source_frame.values.contains(id) || !fresh.insert(id).second)
                reject("fresh identity collides with actual source, typed source-frame vocabulary or another destination: " + id);
        };
        for (const auto& [old, proposed] : authoring.identities) {
            if (!entities.contains(old)) reject("unrequested entity mapping"); reserve(proposed);
        }
        for (const auto& [key, proposed] : authoring.child_identities) {
            if (!children.contains(key)) reject("unrequested qualified child mapping"); reserve(proposed);
        }
        for (const auto& [key, proposed] : authoring.hosted_instance_identities) {
            if (!hosted.contains(key)) reject("unrequested qualified hosted mapping"); reserve(proposed);
        }
        for (const auto& [key, proposed] : authoring.overlay_identities) {
            if (!overlays.contains(key)) reject("unrequested qualified overlay mapping"); reserve(proposed);
        }
        auto candidate = derived.physical;
        // Independently edited ordinary/proposed owners survive typed replay.
        // Every shared-baseline closure original is restored raw-exact first.
        for (const auto& id : derived.copied_owners) candidate.at(id) = actual.at(id);
        const auto original_phases = ModelPhases::from_json(actual.at(plan.registry_id).properties.at("model"));
        for (const auto& edit : authoring.edits)
            if (baseline(original_phases, edit.object_id)) candidate.at(edit.object_id) = actual.at(edit.object_id);
        Ids physical_copies;
        for (const auto& id : derived.copied_owners) {
            auto copy = derived.physical.at(id); copy.id = authoring.identities.at(id);
            if (copy.type == "stair") {
                for (const auto* name : {"flights", "landings"}) if (copy.properties.contains(name))
                    for (auto& row : copy.properties.at(name))
                        row.at("id") = authoring.child_identities.at({id, row.at("id").get<std::string>()});
            } else if (copy.type == "railing") remap_host(copy, authoring);
            admit_rebound_quantities(derived.physical.at(id), copy);
            const auto copy_id = copy.id;
            if (!candidate.emplace(copy_id, std::move(copy)).second) reject("fresh physical owner insertion collided");
            physical_copies.insert(copy_id);
        }
        for (const auto& id : plan.retained_rehost_object_ids) {
            const auto before = candidate.at(id); remap_host(candidate.at(id), authoring);
            admit_rebound_quantities(before, candidate.at(id)); physical_copies.insert(id);
        }
        for (const auto& edit : authoring.edits) if (!derived.copied_owners.contains(edit.object_id)) physical_copies.insert(edit.object_id);
        for (const auto& id : derived.catalogs) {
            auto copy = actual.at(id); copy.id = authoring.identities.at(id);
            auto& copied_rows = copy.properties.at("model").at("instances"); const auto retained = copied_rows; copied_rows = Json::array();
            for (auto row : retained) {
                const PhaseStairReplacementHostedInstanceKey key{id, row.at("id").get<std::string>()};
                if (!hosted.contains(key)) continue;
                row.at("id") = authoring.hosted_instance_identities.at(key);
                remap_field(row.at("placement"), "host_entity_id", authoring.identities);
                copied_rows.push_back(std::move(row));
            }
            const auto copy_id = copy.id;
            if (!candidate.emplace(copy_id, std::move(copy)).second) reject("fresh private catalog insertion collided");
        }
        const auto phases = ModelPhases::from_json(actual.at(plan.registry_id).properties.at("model"));
        auto model_ids = phases.entity_ids(); auto alternatives = phases.alternatives();
        const auto target = std::find_if(alternatives.begin(), alternatives.end(), [&](const auto& row) { return row.id == plan.alternative_id; });
        if (target == alternatives.end()) reject("saved active alternative disappeared");
        for (const auto& id : plan.required_entity_ids) {
            model_ids.push_back(authoring.identities.at(id)); target->proposed_ids.push_back(authoring.identities.at(id));
            if (derived.copied_owners.contains(id)) target->demolished_ids.push_back(id);
        }
        const auto final_phases = ModelPhases::create(model_ids, phases.baseline_ids(), alternatives, phases.active_alternative());
        auto raw = actual.at(plan.registry_id).properties.at("model");
        for (const auto& id : plan.required_entity_ids) raw.at("entity_ids").push_back(authoring.identities.at(id));
        for (auto& row : raw.at("alternatives")) if (row.at("id") == plan.alternative_id)
            for (const auto& id : plan.required_entity_ids) {
                row.at("proposed_ids").push_back(authoring.identities.at(id));
                if (derived.copied_owners.contains(id)) row.at("demolished_ids").push_back(id);
            }
        if (ModelPhases::from_json(raw).to_json() != final_phases.to_json()) reject("raw append differs from validated phase update");
        candidate.at(plan.registry_id).properties.at("model") = std::move(raw);
        PhaseStairReplacementIdentityMap presentation_ids;
        for (const auto& id : derived.copied_owners) presentation_ids.emplace(id, authoring.identities.at(id));
        std::set<PhaseStairReplacementHostedInstanceKey> copied_hosted;
        EmbeddedAssemblyPresentationIds before_presentation;
        if (!hosted.empty()) {
            before_presentation = embedded_assembly_presentation_ids(candidate);
            Ids fresh_aliases;
            for (const auto& [key, alias] : original_aliases)
                if (before_presentation.at(key) != alias) reject("copy insertion changed an original qualified component alias");
            for (const auto& key : hosted) {
                const PhaseStairReplacementHostedInstanceKey proposed{authoring.identities.at(key.first), authoring.hosted_instance_identities.at(key)};
                copied_hosted.insert(proposed);
                const auto& alias = before_presentation.at(proposed);
                if (occupied.values.contains(alias) || authored.values.contains(alias) || fresh.contains(alias) || !fresh_aliases.insert(alias).second)
                    reject("fresh component alias collides with actual/proof/fresh namespace");
                if (!presentation_ids.emplace(original_aliases.at(key), alias).second) reject("computed alias overlaps actual physical mapping");
            }
        }
        presentation(candidate, actual, presentation_ids, authoring.overlay_identities);
        if (!hosted.empty() && embedded_assembly_presentation_ids(candidate) != before_presentation)
            reject("presentation append changed a qualified source or proposed component alias");
        (void)source_budget(candidate);
        validate_stair_identity_transition(actual, candidate, std::span<const RevisionRecord>{});
        admit_physical(candidate, physical_copies);
        admit_instances(candidate, copied_hosted);
        (void)constraint_phase_scope(candidate);
        for (const auto& id : derived.copied_owners) if (!exact(candidate.at(id), actual.at(id))) reject("baseline physical source changed");
        for (const auto& id : derived.catalogs) if (!exact(candidate.at(id), actual.at(id))) reject("actual catalog source changed");
        Ids ordinary; for (const auto& edit : authoring.edits) ordinary.insert(edit.object_id);
        ordinary.insert(plan.retained_rehost_object_ids.begin(), plan.retained_rehost_object_ids.end());
        for (const auto& [id, entity] : actual) {
            if (id == plan.registry_id || ordinary.contains(id) || entity.type == kSheetViewEntityType || entity.type == kAnnotationEntityType) continue;
            if (!exact(candidate.at(id), entity)) reject("replacement changed an unrelated retained owner: " + id);
        }
        return candidate;
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native candidate admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual-source replay: ") + error.what()); }
}
} // namespace sketch
