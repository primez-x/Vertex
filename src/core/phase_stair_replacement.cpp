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
#include "sketch/stair_clone.hpp"
#include "sketch/site_frame.hpp"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

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
template<class Intent>
const std::string& intent_owner(const Intent& intent) { return intent.object_id; }
const std::string& intent_owner(const StairCompoundEditIntent& intent) { return intent.profile_edit.object_id; }
std::vector<StairObjectEditIntent> compound_profiles(const std::vector<StairCompoundEditIntent>& intents) {
    if (intents.size() > identity_limit) reject("typed compound target budget exceeded");
    std::size_t bytes{};
    // Bound both lanes, including final coordinate receipts, before allocating
    // a copied profile batch for topology or intermediate silhouette replay.
    for (const auto& intent : intents) {
        const auto size = encode_stair_compound_edit_intent(intent).dump().size();
        if (size > proof_limit - bytes) reject("typed compound batch byte budget exceeded");
        bytes += size;
    }
    std::vector<StairObjectEditIntent> result;
    result.reserve(intents.size());
    for (const auto& intent : intents) result.push_back(intent.profile_edit);
    return result;
}
template<class Intent>
std::optional<PhaseStairReplacementRequest> classify(const Entities& actual, const Entities& physical,
    const std::vector<Intent>& edits, const Memberships& scope) {
    std::optional<PhaseStairReplacementRequest> result;
    Ids registries;
    for (const auto& edit : edits) {
        const auto& owner = intent_owner(edit);
        if (exact(actual.at(owner), physical.at(owner))) continue;
        const auto member = scope.owners.find(owner);
        if (member == scope.owners.end()) continue;
        const auto& model = scope.models.at(member->second);
        const auto state = model.active_state(); const auto owner_state = state.find(owner);
        if (owner_state == state.end() || owner_state->second == ModelPhase::demolished)
            reject("changed owner is inactive in actual saved design: " + owner);
        registries.insert(member->second);
        if (!baseline(model, owner) || !model.active_alternative()) continue;
        if (canonical_profile(actual.at(owner)) == canonical_profile(physical.at(owner))) {
            if (!same_receipts(actual.at(owner), physical.at(owner)))
                reject("receipt-only baseline edit has no changed physical replacement authority: " + owner);
            continue; // No mathematical profile/topology/host change grants a copy.
        }
        if (result && result->registry_id != member->second) reject("baseline cohort spans foreign registries");
        if (!result) result = PhaseStairReplacementRequest{member->second, *model.active_alternative(), {}};
        result->seed_object_ids.push_back(owner);
    }
    if (result) {
        // Ordinary/proposed-only batches retain their existing typed movement
        // authority. Child one grants profile authority only, including every
        // ordinary member of a mixed baseline replacement cohort.
        if constexpr (std::is_same_v<Intent, StairObjectEditIntent>)
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
struct ProfilePresentationSource {
    // A complete, admitted authored map. V4 retains the additive copied owners
    // and original historical attachments rather than reversing their IDs.
    Entities entities;
    // Actual owner/render spelling -> qualified owner/render in entities.
    PhaseStairReplacementIdentityMap identities;
};
struct Derivation {
    PhaseStairReplacementPlan plan;
    // Typed edited descriptors used to assemble the eventual full candidate;
    // v4 reverses qualification here without claiming physical-map admission.
    Entities physical;
    // Reversed typed descriptors are only for replacement/placement bookkeeping;
    // they are never a physical source map for silhouette/host derivation.
    Entities profile_descriptors;
    ProfilePresentationSource profile_source;
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
void remap_field(Json& value, const char* name, const PhaseStairReplacementIdentityMap& mapping);
std::vector<std::string> inactive_attached_rails(const Entities& actual, const Ids& stairs) {
    const auto active = constraint_phase_scope(actual);
    std::vector<std::string> result;
    for (const auto& [id, entity] : actual) {
        if (!active.inactive_owner_ids.contains(id)) continue;
        const auto host = host_id(entity);
        if (host && stairs.contains(*host)) result.push_back(id);
    }
    if (result.size() > identity_limit) reject("retained inactive rail witness budget exceeded");
    return result;
}
template<class Intent>
Json intent_wire(const Intent& intent) {
    if constexpr (std::is_same_v<Intent, StairObjectEditIntent>) return encode_stair_object_edit_intent(intent);
    else if constexpr (std::is_same_v<Intent, StairCompoundEditIntent>) return encode_stair_compound_edit_intent(intent);
    else return encode_stair_transform_intent(intent);
}
// These descriptors are private typed results, never an admitted entity map or
// a publication candidate. Every physical stage below uses the full additive
// actual-source clone candidate, keeping historical attachment owners intact.
template<class Intent>
bool stage_retained_topology(const Entities& actual, const std::vector<Intent>& intents,
    const Memberships& scope, Entities& physical, Entities& profile_descriptors,
    std::vector<StairCompoundEditIntent>* captured = nullptr,
    ProfilePresentationSource* presentation_source = nullptr) {
    Ids baseline_stairs;
    Strings occupied = source_budget(actual);
    std::size_t bytes{};
    for (const auto& intent : intents) {
        const auto wire = intent_wire(intent); const auto size = wire.dump().size();
        if (size > proof_limit - bytes) reject("typed staging batch byte budget exceeded");
        bytes += size; occupied.read(wire);
        const auto& id = intent_owner(intent);
        const auto owner = actual.find(id);
        if (owner == actual.end() || !physical_owner(owner->second)) reject("typed staging target requires its actual physical owner: " + id);
        const auto member = scope.owners.find(id);
        if (owner->second.type != "stair" || member == scope.owners.end()) continue;
        const auto& model = scope.models.at(member->second);
        const auto states = model.active_state(); const auto state = states.find(id);
        if (model.active_alternative() && baseline(model, id) && state != states.end() && state->second != ModelPhase::demolished)
            baseline_stairs.insert(id);
    }
    if (inactive_attached_rails(actual, baseline_stairs).empty()) return false;
    const auto aliases = embedded_assembly_presentation_ids(actual);
    for (const auto& [key, alias] : aliases) { (void)key; occupied.text(alias); }
    Ids roots;
    for (const auto& intent : intents) {
        const auto& id = intent_owner(intent); roots.insert(id);
        // Identity-copy selection still needs the actual host of an explicitly
        // selected hosted rail. It gives no placement authority to that host.
        if (const auto host = host_id(actual.at(id))) roots.insert(*host);
        if constexpr (!std::is_same_v<Intent, StairTransformIntent>) {
            const auto& profile = [&]() -> const StairObjectEditIntent& {
                if constexpr (std::is_same_v<Intent, StairCompoundEditIntent>) return intent.profile_edit;
                else return intent;
            }();
            // An explicit typed rehost also needs a copied actual destination:
            // an unregistered scratch rail cannot borrow a registered host.
            if (actual.at(id).type == "railing") {
                const auto host = field(profile.profile_fields, "host");
                const auto stair = host ? field(*host, "stair_id") : nullptr;
                if (stair && stair->is_string()) roots.insert(stair->get<std::string>());
            }
        }
    }
    StairCloneAuthoring clone;
    for (const auto& id : roots) clone.transforms.push_back({id, ArchitecturalGroupTransform{}, nullptr});
    const auto plan = inspect_stair_clone_plan(actual, clone.transforms);
    if (!plan.ready()) reject("additive staging: " + plan.diagnostics.front().entity_id + ": " + plan.diagnostics.front().reason);
    std::size_t serial{};
    const auto fresh = [&]() {
        std::string id;
        do { id = "phase-stair-stage-" + std::to_string(++serial); } while (occupied.values.contains(id));
        occupied.text(id); return id;
    };
    for (const auto& id : plan.required_entity_ids) clone.identities.emplace(id, fresh());
    for (const auto& key : plan.required_child_ids) clone.child_identities.emplace(key, fresh());
    for (const auto& key : plan.required_hosted_instance_ids) clone.hosted_instance_identities.emplace(key, fresh());
    for (const auto& key : plan.required_overlay_ids) clone.overlay_identities.emplace(key, fresh());
    // Copy authority is derived only from the actual source. Introduced typed
    // topology gets its own qualified temporary identity, never a borrowed
    // source child. The original spelling is checked again on extraction.
    auto children = clone.child_identities;
    std::size_t temporary_inventory = plan.required_entity_ids.size() + plan.required_child_ids.size() +
        plan.required_hosted_instance_ids.size() + plan.required_overlay_ids.size();
    if constexpr (!std::is_same_v<Intent, StairTransformIntent>) {
        for (const auto& intent : intents) {
            const auto& profile = [&]() -> const StairObjectEditIntent& {
                if constexpr (std::is_same_v<Intent, StairCompoundEditIntent>) return intent.profile_edit;
                else return intent;
            }();
            if (actual.at(profile.object_id).type != "stair") continue;
            for (const auto* name : {"flights", "landings"}) {
                const auto rows = field(profile.profile_fields, name);
                if (!rows || !rows->is_array()) continue;
                for (const auto& row : *rows) {
                    const PhaseStairReplacementChildKey key{profile.object_id, identity(row.at("id"))};
                    if (!children.contains(key)) {
                        if (temporary_inventory == identity_limit) reject("combined temporary topology identity budget exceeded");
                        ++temporary_inventory; children.emplace(key, fresh());
                    }
                }
            }
        }
    }
    const auto rebound_profile = [&](StairObjectEditIntent profile) {
        const auto owner = profile.object_id;
        profile.object_id = clone.identities.at(owner);
        if (actual.at(owner).type == "stair") {
            for (const auto* name : {"flights", "landings"}) {
                auto rows = profile.profile_fields.find(name);
                if (rows == profile.profile_fields.end() || !rows->is_array()) continue;
                for (auto& row : *rows) row.at("id") = children.at({owner, row.at("id").get<std::string>()});
            }
        } else {
            auto host = profile.profile_fields.find("host");
            if (host != profile.profile_fields.end() && host->is_object()) {
                const auto stair = host->at("stair_id").get<std::string>();
                if (clone.identities.contains(stair)) {
                    for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"})
                        if (host->contains(name)) host->at(name) = children.at({stair, host->at(name).get<std::string>()});
                    host->at("stair_id") = clone.identities.at(stair);
                }
            }
        }
        return profile;
    };
    const auto copied_source = replay_stair_clone_authoring(actual, clone);
    Entities edited, profiled;
    if constexpr (std::is_same_v<Intent, StairObjectEditIntent>) {
        std::vector<StairObjectEditIntent> rebound;
        for (const auto& intent : intents) rebound.push_back(rebound_profile(intent));
        topology_dependencies(copied_source, rebound);
        edited = replay_stair_object_edit_entities(copied_source, rebound);
    } else if constexpr (std::is_same_v<Intent, StairCompoundEditIntent>) {
        std::vector<StairCompoundEditIntent> rebound;
        for (auto intent : intents) {
            intent.profile_edit = rebound_profile(std::move(intent.profile_edit));
            intent.placement_edit.object_id = intent.profile_edit.object_id;
            rebound.push_back(std::move(intent));
        }
        const auto profiles = compound_profiles(rebound);
        topology_dependencies(copied_source, profiles);
        profiled = replay_stair_object_edit_entities(copied_source, profiles);
        edited = replay_stair_compound_edit_entities(copied_source, rebound);
    } else {
        std::vector<StairTransformIntent> rebound;
        for (auto intent : intents) { intent.object_id = clone.identities.at(intent.object_id); rebound.push_back(std::move(intent)); }
        edited = replay_stair_transform_entities(copied_source, rebound);
    }
    PhaseStairReplacementIdentityMap inverse;
    for (const auto& [id, temporary] : clone.identities) inverse.emplace(temporary, id);
    std::map<PhaseStairReplacementChildKey, std::string> inverse_children;
    for (const auto& [key, temporary] : children) inverse_children.emplace(std::pair{clone.identities.at(key.first), temporary}, key.second);
    const auto extract = [&](const Entities& staged) {
        auto descriptors = actual;
        for (const auto& [id, temporary] : clone.identities) {
            if (!exact(staged.at(id), copied_source.at(id))) reject("typed staging changed an original owner: " + id);
            const auto& original = actual.at(id);
            if (!physical_owner(original)) continue;
            auto copy = staged.at(temporary); copy.id = id;
            if (copy.type == "stair") {
                for (const auto* name : {"flights", "landings"}) if (copy.properties.contains(name))
                    for (auto& row : copy.properties.at(name)) row.at("id") = inverse_children.at({temporary, row.at("id").get<std::string>()});
            } else if (const auto host = host_id(copy); host && inverse.contains(*host)) {
                auto& binding = copy.properties.at("host");
                for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"})
                    if (binding.contains(name)) binding.at(name) = inverse_children.at({*host, binding.at(name).get<std::string>()});
                binding.at("stair_id") = inverse.at(*host);
            }
            // Independently check envelopes/entered inputs against the actual
            // owner after reversing only codec-owned qualification slots.
            (void)capture_stair_object_edit(original, copy);
            descriptors.at(id) = std::move(copy);
        }
        for (const auto& [key, temporary] : clone.hosted_instance_identities) {
            auto& raw = descriptors.at(key.first).properties.at("model");
            const auto& typed = staged.at(clone.identities.at(key.first)).properties.at("model");
            const auto row = std::find_if(typed.at("instances").begin(), typed.at("instances").end(), [&](const auto& item) { return item.at("id") == temporary; });
            if (row == typed.at("instances").end()) reject("typed staged hosted row disappeared");
            auto replacement = *row; replacement.at("id") = key.second;
            remap_field(replacement.at("placement"), "host_entity_id", inverse);
            auto& rows = raw.at("instances");
            const auto target = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.at("id") == key.second; });
            if (target == rows.end()) reject("actual qualified hosted row disappeared");
            *target = std::move(replacement); raw.at("schema") = typed.at("schema");
        }
        validate_stair_identity_transition(actual, descriptors, std::span<const RevisionRecord>{});
        return descriptors;
    };
    physical = extract(edited);
    if constexpr (std::is_same_v<Intent, StairCompoundEditIntent>) {
        profile_descriptors = extract(profiled);
        if (presentation_source) {
            // Resolve aliases only against the valid full additive map. Original
            // aliases stay exact; copied hosted rows get qualified render names.
            const auto profiled_aliases = embedded_assembly_presentation_ids(profiled);
            for (const auto& [key, alias] : aliases)
                if (profiled_aliases.at(key) != alias) reject("typed additive profiles changed an original component alias");
            presentation_source->identities = clone.identities;
            for (const auto& [key, temporary] : clone.hosted_instance_identities) {
                const PhaseStairReplacementHostedInstanceKey copied{clone.identities.at(key.first), temporary};
                if (!presentation_source->identities.emplace(aliases.at(key), profiled_aliases.at(copied)).second)
                    reject("qualified profile presentation overlaps an actual owner");
            }
            presentation_source->entities = std::move(profiled);
        }
    }
    if constexpr (std::is_same_v<Intent, StairObjectEditIntent>) {
        if (captured) {
            std::vector<Entity> edited_owners;
            for (const auto& intent : intents) edited_owners.push_back(edited.at(clone.identities.at(intent.object_id)));
            auto temporary = capture_stair_compound_edits(copied_source, edited_owners);
            for (auto& intent : temporary) {
                auto& profile = intent.profile_edit;
                const auto temporary_owner = profile.object_id;
                const auto& owner = inverse.at(temporary_owner);
                if (actual.at(owner).type == "stair") {
                    for (const auto* name : {"flights", "landings"}) {
                        auto rows = profile.profile_fields.find(name);
                        if (rows == profile.profile_fields.end() || !rows->is_array()) continue;
                        for (auto& row : *rows) row.at("id") = inverse_children.at({temporary_owner, row.at("id").get<std::string>()});
                    }
                } else {
                    auto host = profile.profile_fields.find("host");
                    if (host != profile.profile_fields.end() && host->is_object()) {
                        const auto stair = host->at("stair_id").get<std::string>();
                        if (inverse.contains(stair)) {
                            for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"})
                                if (host->contains(name)) host->at(name) = inverse_children.at({stair, host->at(name).get<std::string>()});
                            host->at("stair_id") = inverse.at(stair);
                        }
                    }
                }
                profile.object_id = owner; intent.placement_edit.object_id = owner;
                (void)encode_stair_compound_edit_intent(intent);
            }
            *captured = std::move(temporary);
        }
    }
    return true;
}
template<class Intent>
Derivation derive(const Entities& actual, const std::vector<Intent>& edits,
    const std::string& registry_id, const std::string& alternative_id, bool retained_staging = true) {
    Derivation result;
    auto& plan = result.plan;
    plan.registry_id = registry_id; plan.alternative_id = alternative_id;
    try {
        (void)source_budget(actual);
        if (edits.empty() || edits.size() > identity_limit) reject("requires a bounded nonempty typed profile batch");
        const auto scope = memberships(actual);
        const bool staged = retained_staging && stage_retained_topology(actual, edits, scope, result.physical,
            result.profile_descriptors, nullptr, &result.profile_source);
        if (!staged) {
            if constexpr (std::is_same_v<Intent, StairObjectEditIntent>) {
                topology_dependencies(actual, edits);
                result.physical = replay_stair_object_edit_entities(actual, edits);
            } else if constexpr (std::is_same_v<Intent, StairCompoundEditIntent>) {
                const auto profiles = compound_profiles(edits);
                topology_dependencies(actual, profiles);
                result.physical = replay_stair_compound_edit_entities(actual, edits);
                result.profile_descriptors = replay_stair_object_edit_entities(actual, profiles);
                result.profile_source.entities = result.profile_descriptors;
            } else result.physical = replay_stair_transform_entities(actual, edits);
        }
        const auto request = classify(actual, result.physical, edits, scope);
        if (!request) reject("batch has no actually changed active shared-baseline owner");
        if ((!registry_id.empty() && registry_id != request->registry_id) ||
            (!alternative_id.empty() && alternative_id != request->alternative_id))
            reject("supplied destination differs from actual saved active membership");
        plan.registry_id = request->registry_id; plan.alternative_id = request->alternative_id;
        plan.seed_object_ids = request->seed_object_ids;
        if (staged) plan.preserved_inactive_rail_ids = inactive_attached_rails(actual, Ids(plan.seed_object_ids.begin(), plan.seed_object_ids.end()));
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
using PresentationTransforms = std::map<std::string, AssemblyTransform, std::less<>>;
AssemblyTransform presentation_transform(const Entities& actual, const std::string& owner,
    const ArchitecturalGroupTransform& captured, bool world_authored = false) {
    const auto transform = architectural_group_assembly_transform(captured);
    if (!world_authored) return transform;
    const auto frame = resolve_site_presentation(actual, owner).forward;
    return conjugate_assembly_transform_through_rigid_frame(transform,
        {{frame.translation_m.x, frame.translation_m.y, frame.translation_m.z}, frame.rotation_radians, 1.0, false});
}
// The saved frame, including its authored origin, owns overlay coordinates.
// Never substitute the live editor's default plan or a reconstructed pivot.
struct OverlayFrame {
    AssemblyPoint3 origin, right, up, normal;
    explicit OverlayFrame(const CoordinatedView& view)
        : up{view.up[0], view.up[1], view.up[2]},
          normal{-view.direction[0], -view.direction[1], -view.direction[2]} {
        const auto projection = coordinated_view_origins(view).projection_m;
        origin = {projection[0], projection[1], projection[2]};
        right = {up.y * normal.z - up.z * normal.y, up.z * normal.x - up.x * normal.z,
            up.x * normal.y - up.y * normal.x};
    }
    std::array<double, 2> project(AssemblyPoint3 point) const {
        point = {point.x - origin.x, point.y - origin.y, point.z - origin.z};
        return {point.x * right.x + point.y * right.y + point.z * right.z,
            point.x * up.x + point.y * up.y + point.z * up.z};
    }
    AssemblyPoint3 lift(std::array<double, 2> point) const {
        return {origin.x + point[0] * right.x + point[1] * up.x,
            origin.y + point[0] * right.y + point[1] * up.y,
            origin.z + point[0] * right.z + point[1] * up.z};
    }
    std::array<double, 2> move(std::array<double, 2> point, const AssemblyTransform& transform) const {
        return project(transform_assembly_point(lift(point), transform));
    }
    std::array<double, 2> vector(AssemblyPoint3 direction, AssemblyTransform transform) const {
        transform.translation_m = {};
        const auto point = transform_assembly_point(direction, transform);
        return {point.x * right.x + point.y * right.y + point.z * right.z,
            point.x * up.x + point.y * up.y + point.z * up.z};
    }
};
TopoDS_Shape presentation_shape(const Entities& source, const std::string& owner,
    const EmbeddedAssemblyPresentationIds& aliases) {
    if (const auto found = source.find(owner); found != source.end() && physical_owner(found->second)) {
        const auto resolved = host_id(found->second) ? found->second : resolve_vertical_placement(source, found->second);
        return make_building_shape(decode_building_entity(resolved), source);
    }
    const auto alias = std::find_if(aliases.begin(), aliases.end(), [&](const auto& row) { return row.second == owner; });
    if (alias == aliases.end()) reject("bound copied presentation has no qualified physical/component source: " + owner);
    const auto model = AssemblyModel::from_json(source.at(alias->first.first).properties.at("model"));
    const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == alias->first.second; });
    if (row == model.instances().end() || !row->placement) reject("bound component presentation lost its actual hosted row");
    AssemblyExpansionBudget budget;
    const auto expansion = model.expand(*row, budget);
    if (!expansion.profiles.empty()) return make_assembly_geometry(expansion).shape;
    const auto& placement = *row->placement;
    const auto& host = source.at(placement.host_entity_id);
    const auto resolved = host_id(host) ? host : resolve_vertical_placement(source, host);
    return transform_assembly_shape(make_building_shape(decode_building_entity(resolved), source),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
            placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
}
Bounds2 presentation_bounds(const TopoDS_Shape& shape, const OverlayFrame& frame) {
    const auto dot_origin = [&](AssemblyPoint3 axis) {
        return -(axis.x * frame.origin.x + axis.y * frame.origin.y + axis.z * frame.origin.z);
    };
    gp_Trsf transform;
    transform.SetValues(frame.right.x, frame.right.y, frame.right.z, dot_origin(frame.right),
        frame.up.x, frame.up.y, frame.up.z, dot_origin(frame.up),
        frame.normal.x, frame.normal.y, frame.normal.z, dot_origin(frame.normal));
    BRepBuilderAPI_Transform operation(shape, transform, true);
    if (!operation.IsDone() || operation.Shape().IsNull()) reject("bound presentation cannot enter actual saved view frame");
    Bnd_Box box; BRepBndLib::AddOptimal(operation.Shape(), box, false, false);
    if (box.IsVoid() || box.IsOpen()) reject("bound presentation has no finite saved-view silhouette");
    double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
    for (const auto value : {x0, y0, x1, y1})
        if (!std::isfinite(value) || std::abs(value) > 1e6) reject("bound presentation exceeds saved-view coordinate bounds");
    return {{x0, y0}, {x1, y1}};
}
struct OverlayGeometryCache {
    std::map<std::string, TopoDS_Shape, std::less<>> shapes;
    std::map<std::pair<std::string, std::string>, Bounds2> bounds;
    Bounds2 get(const Entities& source, const std::string& owner, const std::string& qualified_view,
        const OverlayFrame& frame, const EmbeddedAssemblyPresentationIds& aliases) {
        const auto key = std::pair{owner, qualified_view};
        if (const auto saved = bounds.find(key); saved != bounds.end()) return saved->second;
        if (!shapes.contains(owner)) shapes.emplace(owner, presentation_shape(source, owner, aliases));
        return bounds.emplace(key, presentation_bounds(shapes.at(owner), frame)).first->second;
    }
};
std::size_t presentation_cost(const Entities& source, const std::string& owner,
    const EmbeddedAssemblyPresentationIds& aliases) {
    if (const auto found = source.find(owner); found != source.end() && physical_owner(found->second)) {
        const auto resolved = host_id(found->second) ? found->second : resolve_vertical_placement(source, found->second);
        const auto object = decode_building_entity(resolved);
        if (const auto stair = std::get_if<StairFlight>(&object)) return stair->riser_count + stair->landings.size() + 1;
        const auto& rail = std::get<Railing>(object);
        if (const auto host = host_id(found->second))
            return derive_hosted_railing_layout(rail, decode_stair_properties(*host,
                resolve_vertical_placement(source, source.at(*host)).properties)).posts.size() + 1;
        return static_cast<std::size_t>(std::ceil(rail.length / rail.post_spacing)) + 2;
    }
    const auto alias = std::find_if(aliases.begin(), aliases.end(), [&](const auto& row) { return row.second == owner; });
    if (alias == aliases.end()) reject("bound presentation has no actual qualified cost source");
    const auto model = AssemblyModel::from_json(source.at(alias->first.first).properties.at("model"));
    const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == alias->first.second; });
    if (row == model.instances().end() || !row->placement) reject("bound component has no actual cost host");
    AssemblyExpansionBudget budget;
    const auto expansion = model.expand(*row, budget);
    if (!expansion.profiles.empty()) return budget.consumed_nodes + budget.consumed_profile_segments;
    return presentation_cost(source, row->placement->host_entity_id, aliases);
}
const std::string& qualified_profile_presentation_owner(
    const PhaseStairReplacementIdentityMap& identities, const std::string& owner) {
    if (identities.empty()) return owner; // Original v1/v2/v3 source vocabulary.
    const auto found = identities.find(owner);
    if (found == identities.end()) reject("additive profile presentation lacks its qualified actual owner: " + owner);
    return found->second;
}
void transform_overlay(Json& row, const CoordinatedView& view, const Entities& actual, const Entities& candidate,
    const PhaseStairReplacementIdentityMap& mapping, const PresentationTransforms& transforms,
    const EmbeddedAssemblyPresentationIds& source_aliases, const EmbeddedAssemblyPresentationIds& candidate_aliases,
    const PhaseStairReplacementIdentityMap& source_identities, const std::string& qualified_view,
    OverlayGeometryCache& source_cache, OverlayGeometryCache& candidate_cache) {
    const auto binding = field(row, "dimension_binding");
    const auto owner = binding && !binding->is_null() ? binding->at("object_id").get<std::string>()
        : row.value("object_id", std::string{});
    const auto found = transforms.find(owner);
    if (found == transforms.end()) reject("copied overlay has no captured source-qualified transform");
    const auto& transform = found->second; const OverlayFrame frame(view);
    if (binding && !binding->is_null()) {
        const auto normal = frame.vector(frame.normal, transform);
        if (std::hypot(normal[0], normal[1]) > 1e-10)
            reject("bound dimension transform depends on source depth in its saved view");
        const bool horizontal = binding->at("axis") == "horizontal";
        const auto axis = frame.vector(horizontal ? frame.right : frame.up, transform);
        const double dx = axis[0], dy = axis[1];
        const bool next_horizontal = std::abs(dy) <= 1e-10 && std::abs(dx) > 1e-10;
        const bool next_vertical = std::abs(dx) <= 1e-10 && std::abs(dy) > 1e-10;
        if (!next_horizontal && !next_vertical) reject("transformed bound dimension axis is not representable in its saved view");
        const auto before = source_cache.get(actual, qualified_profile_presentation_owner(source_identities, owner),
            qualified_view, frame, source_aliases);
        const auto after = candidate_cache.get(candidate, mapping.at(owner), qualified_view, frame, candidate_aliases);
        const double location = (horizontal ? before.maximum.y : before.maximum.x) + binding->at("line_offset_m").get<double>();
        const auto moved = frame.move(horizontal ? std::array<double, 2>{0, location} : std::array<double, 2>{location, 0}, transform);
        row.at("dimension_binding").at("axis") = next_horizontal ? "horizontal" : "vertical";
        row.at("dimension_binding").at("line_offset_m") = (next_horizontal ? moved[1] - after.maximum.y : moved[0] - after.maximum.x);
    } else for (const auto* name : {"start_m", "end_m"}) {
        const auto& old = row.at(name);
        row.at(name) = frame.move({old.at(0).get<double>(), old.at(1).get<double>()}, transform);
    }
}
void presentation(Entities& candidate, const Entities& actual, const PhaseStairReplacementIdentityMap& mapping,
    const PhaseStairReplacementOverlayIdentityMap& overlay_ids, const PresentationTransforms& transforms = {},
    const ProfilePresentationSource* profile_source = nullptr) {
    const auto& silhouette_source = profile_source ? profile_source->entities : actual;
    const PhaseStairReplacementIdentityMap identity_source;
    const auto& source_identities = profile_source ? profile_source->identities : identity_source;
    Ids owners; for (const auto& [old, proposed] : mapping) { (void)proposed; owners.insert(old); }
    EmbeddedAssemblyPresentationIds source_aliases, candidate_aliases;
    OverlayGeometryCache source_cache, candidate_cache;
    if (!transforms.empty() && std::any_of(mapping.begin(), mapping.end(), [&](const auto& row) { return !actual.contains(row.first); })) {
        const auto original_aliases = embedded_assembly_presentation_ids(actual);
        source_aliases = embedded_assembly_presentation_ids(silhouette_source);
        candidate_aliases = embedded_assembly_presentation_ids(candidate);
        if (profile_source && source_identities.empty() && source_aliases != original_aliases)
            reject("typed intermediate profiles changed actual qualified component aliases");
        if (profile_source && !source_identities.empty()) for (const auto& [key, alias] : original_aliases)
            if (source_aliases.at(key) != alias) reject("additive profile presentation changed an original component alias");
    }
    if (!transforms.empty()) {
        std::set<PhaseStairReplacementOverlayKey> projected;
        std::map<std::string, std::size_t, std::less<>> source_costs, candidate_costs;
        std::size_t work{};
        for (const auto& [id, original] : actual) {
            if (original.type != kSheetViewEntityType || !touches(original.properties, owners)) continue;
            const auto views = decode_sheet_view_entity(original);
            for (const auto& view : views.views()) for (const auto& overlay : view.overlays) {
                if (!overlay.dimension_binding || (!owners.contains(overlay.object_id) &&
                    !owners.contains(overlay.dimension_binding->object_id))) continue;
                const auto& owner = overlay.dimension_binding->object_id;
                if (!mapping.contains(owner)) reject("copied bound overlay refers to a source outside the copied transform cohort");
                if (!projected.emplace(id, view.id, owner).second) continue;
                const auto& proposed = mapping.at(owner);
                if (!source_costs.contains(owner)) source_costs.emplace(owner, presentation_cost(silhouette_source,
                    qualified_profile_presentation_owner(source_identities, owner), source_aliases));
                if (!candidate_costs.contains(proposed)) candidate_costs.emplace(proposed, presentation_cost(candidate, proposed, candidate_aliases));
                for (const auto cost : {source_costs.at(owner), candidate_costs.at(proposed)}) {
                    if (cost > geometry_limit - work) reject("aggregate bound saved-view native work budget exceeded");
                    work += cost;
                }
            }
        }
    }
    for (const auto& [id, original] : actual) {
        if (!touches(original.properties, owners) && !touches(original.extensions, owners)) continue;
        if (original.type == kSheetViewEntityType) {
            const auto decoded = decode_sheet_view_entity(original);
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
                        if (!transforms.empty()) {
                            const auto saved = std::find_if(decoded.views().begin(), decoded.views().end(), [&](const auto& item) { return item.id == view_id; });
                            if (saved == decoded.views().end()) reject("copied overlay lost its actual qualified saved view");
                            // JSON framing keeps long/local view IDs qualified
                            // without inventing an ambiguous concatenated alias.
                            const auto qualified = Json::array({id, view_id}).dump();
                            transform_overlay(row, *saved, silhouette_source, candidate, mapping, transforms, source_aliases, candidate_aliases,
                                source_identities, qualified, source_cache, candidate_cache);
                        }
                        row.at("id") = overlay_ids.at({id, view_id, row.at("id").get<std::string>()});
                        remap_field(row, "object_id", mapping);
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) remap_field(row.at("dimension_binding"), "object_id", mapping);
                        // The v1 profile variant retains source coordinates;
                        // closed v2/v3 captured operators move copied rows.
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
std::vector<StairCompoundEditIntent> capture_phase_stair_replacement_compound_edits(
    const Entities& actual, const std::vector<Entity>& edited_entities) {
    (void)source_budget(actual);
    if (edited_entities.size() > maximum_architectural_group_targets) reject("compound capture target budget exceeded");
    std::vector<StairObjectEditIntent> profiles;
    Ids targets; Strings edited_budget;
    for (const auto& edited : edited_entities) {
        identity(edited.id);
        edited_budget.text(edited.id); edited_budget.text(edited.type);
        edited_budget.read(edited.properties); edited_budget.read(edited.extensions);
        if (!targets.insert(edited.id).second) reject("duplicate compound capture owner");
        const auto original = actual.find(edited.id);
        if (original == actual.end() || !physical_owner(original->second)) reject("compound capture requires an actual stair or railing: " + edited.id);
        if (const auto intent = capture_stair_object_edit(original->second, edited)) profiles.push_back(*intent);
    }
    std::vector<StairCompoundEditIntent> result;
    Entities physical, intermediate;
    if (!stage_retained_topology(actual, profiles, memberships(actual), physical, intermediate, &result))
        return capture_stair_compound_edits(actual, edited_entities);
    return result;
}
std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(const Entities& actual,
    const std::vector<StairObjectEditIntent>& edits) {
    try {
        (void)source_budget(actual);
        if (edits.size() > identity_limit) reject("typed profile target budget exceeded");
        const auto scope = memberships(actual);
        Entities physical, profiles;
        if (!stage_retained_topology(actual, edits, scope, physical, profiles)) {
            topology_dependencies(actual, edits);
            physical = replay_stair_object_edit_entities(actual, edits);
        }
        return classify(actual, physical, edits, scope);
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native geometry admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual profile request: ") + error.what()); }
}
PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(const Entities& actual,
    const std::vector<StairObjectEditIntent>& edits, const std::string& registry_id, const std::string& alternative_id) {
    return derive(actual, edits, registry_id, alternative_id).plan;
}
std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(const Entities& actual,
    const std::vector<StairTransformIntent>& transforms) {
    try {
        (void)source_budget(actual);
        if (transforms.size() > identity_limit) reject("typed transform target budget exceeded");
        const auto scope = memberships(actual);
        Entities physical, profiles;
        if (!stage_retained_topology(actual, transforms, scope, physical, profiles)) physical = replay_stair_transform_entities(actual, transforms);
        return classify(actual, physical, transforms, scope);
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native geometry admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual transform request: ") + error.what()); }
}
PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(const Entities& actual,
    const std::vector<StairTransformIntent>& transforms, const std::string& registry_id, const std::string& alternative_id) {
    return derive(actual, transforms, registry_id, alternative_id).plan;
}
std::optional<PhaseStairReplacementRequest> phase_stair_replacement_request(const Entities& actual,
    const std::vector<StairCompoundEditIntent>& compound_edits) {
    try {
        (void)source_budget(actual);
        if (compound_edits.size() > identity_limit) reject("typed compound target budget exceeded");
        const auto scope = memberships(actual);
        Entities physical, profiles;
        if (!stage_retained_topology(actual, compound_edits, scope, physical, profiles)) {
            topology_dependencies(actual, compound_profiles(compound_edits));
            physical = replay_stair_compound_edit_entities(actual, compound_edits);
        }
        return classify(actual, physical, compound_edits, scope);
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native geometry admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual compound request: ") + error.what()); }
}
PhaseStairReplacementPlan inspect_phase_stair_replacement_plan(const Entities& actual,
    const std::vector<StairCompoundEditIntent>& compound_edits, const std::string& registry_id, const std::string& alternative_id) {
    return derive(actual, compound_edits, registry_id, alternative_id).plan;
}

Json encode_phase_stair_replacement_authoring(const PhaseStairReplacementAuthoring& authoring) {
    identity(authoring.registry_id); identity(authoring.alternative_id);
    const bool transformed = !authoring.transforms.empty();
    const bool compound = !authoring.compound_edits.empty();
    const bool retained = !authoring.preserved_inactive_rail_ids.empty();
    if (authoring.preserved_inactive_rail_ids.size() > identity_limit) reject("inactive rail witness budget exceeded");
    std::string previous;
    for (const auto& id : authoring.preserved_inactive_rail_ids) {
        identity(id);
        if (!previous.empty() && id <= previous) reject("inactive rail witness must be ascending and unique");
        previous = id;
    }
    const auto lanes = static_cast<unsigned>(!authoring.edits.empty()) + static_cast<unsigned>(transformed) + static_cast<unsigned>(compound);
    if (lanes != 1 || authoring.edits.size() > identity_limit || authoring.transforms.size() > identity_limit ||
        authoring.compound_edits.size() > identity_limit || authoring.identities.empty() ||
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
    for (const auto& transform : authoring.transforms) {
        if (!targets.insert(transform.object_id).second) reject("duplicate typed transform target");
        auto value = encode_stair_transform_intent(transform);
        const auto size = value.dump().size();
        if (size > proof_limit - edit_bytes) reject("typed transform batch byte budget exceeded");
        edit_bytes += size; edits.push_back(std::move(value));
    }
    for (const auto& edit : authoring.compound_edits) {
        if (!targets.insert(intent_owner(edit)).second) reject("duplicate typed compound target");
        auto value = encode_stair_compound_edit_intent(edit);
        const auto size = value.dump().size();
        if (size > proof_limit - edit_bytes) reject("typed compound batch byte budget exceeded");
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
    Json result{{"version", retained ? 4 : (compound ? 3 : (transformed ? 2 : 1))}, {"registry_id", authoring.registry_id}, {"alternative_id", authoring.alternative_id},
        {compound ? "compound_edits" : (transformed ? "transforms" : "edits"), std::move(edits)}, {"identities", std::move(identities)}, {"child_identities", std::move(children)},
        {"hosted_instance_identities", std::move(hosted)}, {"overlay_identities", std::move(overlays)}};
    if (retained) result["preserved_inactive_rail_ids"] = authoring.preserved_inactive_rail_ids;
    proof_budget(result); return result;
}
PhaseStairReplacementAuthoring decode_phase_stair_replacement_authoring(const Json& value) {
    try {
        proof_budget(value);
        if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
            (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4)) reject("unsupported authoring version");
        const bool retained = value.at("version") == 4;
        const bool transformed = value.at("version") == 2 || (retained && value.contains("transforms"));
        const bool compound = value.at("version") == 3 || (retained && value.contains("compound_edits"));
        const auto* operations = compound ? "compound_edits" : (transformed ? "transforms" : "edits");
        Ids expected{"version", "registry_id", "alternative_id", operations, "identities", "child_identities", "hosted_instance_identities", "overlay_identities"};
        if (retained) expected.insert("preserved_inactive_rail_ids");
        keys(value, expected);
        if (!value.at(operations).is_array() ||
            !value.at("identities").is_object() || !value.at("child_identities").is_array() ||
            !value.at("hosted_instance_identities").is_array() || !value.at("overlay_identities").is_array()) reject("unsupported authoring shape");
        PhaseStairReplacementAuthoring result;
        result.registry_id = identity(value.at("registry_id")); result.alternative_id = identity(value.at("alternative_id"));
        if (retained) {
            const auto& witness = value.at("preserved_inactive_rail_ids");
            if (!witness.is_array() || witness.empty() || witness.size() > identity_limit) reject("v4 requires a bounded nonempty inactive rail witness");
            for (const auto& id : witness) result.preserved_inactive_rail_ids.push_back(identity(id));
        }
        if (compound) for (const auto& edit : value.at(operations)) result.compound_edits.push_back(decode_stair_compound_edit_intent(edit));
        else if (transformed) for (const auto& transform : value.at(operations)) result.transforms.push_back(decode_stair_transform_intent(transform));
        else for (const auto& edit : value.at(operations)) result.edits.push_back(decode_stair_object_edit_intent(edit));
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
        const bool compound = !authoring.compound_edits.empty();
        const bool transformed = !authoring.transforms.empty() || compound;
        const bool retained = !authoring.preserved_inactive_rail_ids.empty();
        const auto derived = compound ? derive(actual, authoring.compound_edits, authoring.registry_id, authoring.alternative_id, retained)
            : (!authoring.transforms.empty() ? derive(actual, authoring.transforms, authoring.registry_id, authoring.alternative_id, retained)
                : derive(actual, authoring.edits, authoring.registry_id, authoring.alternative_id, retained));
        const auto& plan = derived.plan;
        if (!plan.ready()) reject(plan.diagnostics.front().entity_id + ": " + plan.diagnostics.front().reason);
        if (authoring.preserved_inactive_rail_ids != plan.preserved_inactive_rail_ids) reject("inactive rail witness differs from actual-source discovery");
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
        for (const auto& transform : authoring.transforms) source_frame.read(encode_stair_transform_intent(transform));
        for (const auto& edit : authoring.compound_edits) source_frame.read(encode_stair_compound_edit_intent(edit));
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
        Ids ordinary;
        for (const auto& edit : authoring.edits) ordinary.insert(edit.object_id);
        for (const auto& transform : authoring.transforms) ordinary.insert(transform.object_id);
        for (const auto& edit : authoring.compound_edits) ordinary.insert(intent_owner(edit));
        if (transformed) {
            const auto active = constraint_phase_scope(actual);
            const auto& placement_source = compound ? derived.profile_descriptors : actual;
            // A host operator may propagate to active attached rails. Inactive
            // alternatives never acquire authority from scratch geometry.
            for (const auto& [id, entity] : placement_source) if (const auto host = host_id(entity); host && ordinary.contains(*host)) {
                if (active.inactive_owner_ids.contains(id)) candidate.at(id) = actual.at(id);
                else ordinary.insert(id);
            }
        }
        // Independently edited ordinary/proposed owners survive typed replay.
        // Every shared-baseline closure original is restored raw-exact first.
        for (const auto& id : derived.copied_owners) candidate.at(id) = actual.at(id);
        const auto original_phases = ModelPhases::from_json(actual.at(plan.registry_id).properties.at("model"));
        for (const auto& edit : authoring.edits)
            if (baseline(original_phases, edit.object_id)) candidate.at(edit.object_id) = actual.at(edit.object_id);
        for (const auto& transform : authoring.transforms)
            if (baseline(original_phases, transform.object_id)) candidate.at(transform.object_id) = actual.at(transform.object_id);
        for (const auto& edit : authoring.compound_edits)
            if (baseline(original_phases, intent_owner(edit))) candidate.at(intent_owner(edit)) = actual.at(intent_owner(edit));
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
        for (const auto& id : ordinary) if (!derived.copied_owners.contains(id)) physical_copies.insert(id);
        std::set<PhaseStairReplacementHostedInstanceKey> changed_original_hosted;
        if (transformed) for (const auto& [id, original] : actual) {
            if (original.type != "assembly_model" || exact(original, candidate.at(id))) continue;
            const auto& source_model = original.properties.at("model");
            const auto& transformed_model = derived.physical.at(id).properties.at("model");
            auto composed = original;
            auto& rows = composed.properties.at("model").at("instances");
            const auto& transformed_rows = transformed_model.at("instances");
            if (rows.size() != transformed_rows.size()) reject("typed transform changed actual catalog inventory: " + id);
            bool changed = false;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const auto& row = rows.at(index); const auto& replacement = transformed_rows.at(index);
                if (row.at("id") != replacement.at("id")) reject("typed transform reordered actual catalog rows: " + id);
                const auto placement = field(row, "placement");
                const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
                if (!host || !host->is_string() || !ordinary.contains(host->get_ref<const std::string&>()) ||
                    derived.copied_owners.contains(host->get_ref<const std::string&>())) continue;
                if (row != replacement || row.dump() != replacement.dump()) {
                    rows.at(index) = replacement; changed = true;
                    changed_original_hosted.emplace(id, row.at("id").get<std::string>());
                }
            }
            if (changed) {
                // A private baseline copy cannot grant a schema rewrite to its
                // original catalog. Only independently authorized rows compose.
                if (derived.catalogs.contains(id) && source_model.at("schema") != transformed_model.at("schema"))
                    reject("preserving shared baseline catalog conflicts with the ordinary hosted row schema upgrade: " + id);
                if (!derived.catalogs.contains(id)) composed.properties.at("model").at("schema") = transformed_model.at("schema");
                (void)AssemblyModel::from_json(composed.properties.at("model"));
            }
            candidate.at(id) = std::move(composed);
        }
        for (const auto& id : derived.catalogs) {
            auto copy = actual.at(id); copy.id = authoring.identities.at(id);
            if (transformed) copy.properties.at("model").at("schema") = derived.physical.at(id).properties.at("model").at("schema");
            auto& copied_rows = copy.properties.at("model").at("instances"); const auto retained = copied_rows; copied_rows = Json::array();
            for (auto row : retained) {
                const PhaseStairReplacementHostedInstanceKey key{id, row.at("id").get<std::string>()};
                if (!hosted.contains(key)) continue;
                if (transformed) {
                    const auto& typed_rows = derived.physical.at(id).properties.at("model").at("instances");
                    const auto typed = std::find_if(typed_rows.begin(), typed_rows.end(), [&](const auto& item) { return item.at("id") == key.second; });
                    if (typed == typed_rows.end()) reject("selected actual typed transformed hosted row disappeared");
                    row = *typed;
                }
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
        // Complete candidate/source envelopes precede any new alias codec or
        // saved-view native derivation. Typed physical/host admission precedes
        // native silhouette construction used only for presentation placement.
        (void)source_budget(candidate);
        validate_stair_identity_transition(actual, candidate, std::span<const RevisionRecord>{});
        admit_physical(candidate, physical_copies);
        admit_instances(candidate, changed_original_hosted);
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
        admit_instances(candidate, copied_hosted);
        PresentationTransforms presentation_operations;
        if (transformed) {
            std::map<std::string, ArchitecturalGroupTransform, std::less<>> captured;
            for (const auto& transform : authoring.transforms) captured.emplace(transform.object_id, transform.transform);
            for (const auto& edit : authoring.compound_edits)
                captured.emplace(intent_owner(edit), edit.placement_edit.transform);
            const auto active = constraint_phase_scope(actual);
            const auto& placement_source = compound ? derived.profile_descriptors : actual;
            for (const auto& [id, entity] : placement_source) if (const auto host = host_id(entity); host && captured.contains(*host)) {
                if (compound && active.inactive_owner_ids.contains(id)) continue;
                // A dependent rail uses its selected admitted host G once. Child
                // three follows typed rehosting in the profile stage, matching O.
                captured[id] = captured.at(*host);
            }
            for (const auto& id : derived.copied_owners)
                presentation_operations.emplace(id, presentation_transform(actual, id, captured.at(id)));
            for (const auto& key : hosted) {
                const auto model = AssemblyModel::from_json(actual.at(key.first).properties.at("model"));
                const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == key.second; });
                if (row == model.instances().end() || !row->placement) reject("copied component lost its source-qualified captured host");
                AssemblyExpansionBudget budget;
                const bool world_authored = !model.expand(*row, budget).profiles.empty();
                const auto& host = row->placement->host_entity_id;
                presentation_operations.emplace(original_aliases.at(key),
                    presentation_transform(actual, host, captured.at(host), world_authored));
            }
        }
        presentation(candidate, actual, presentation_ids, authoring.overlay_identities, presentation_operations,
            compound ? &derived.profile_source : nullptr);
        if (!hosted.empty() && embedded_assembly_presentation_ids(candidate) != before_presentation)
            reject("presentation append changed a qualified source or proposed component alias");
        (void)source_budget(candidate);
        (void)constraint_phase_scope(candidate);
        for (const auto& id : derived.copied_owners) if (!exact(candidate.at(id), actual.at(id))) reject("baseline physical source changed");
        for (const auto& id : plan.preserved_inactive_rail_ids)
            if (!exact(candidate.at(id), actual.at(id))) reject("retained inactive railing changed: " + id);
        for (const auto& id : derived.catalogs) if (!transformed && !exact(candidate.at(id), actual.at(id))) reject("actual catalog source changed");
        ordinary.insert(plan.retained_rehost_object_ids.begin(), plan.retained_rehost_object_ids.end());
        for (const auto& [id, entity] : actual) {
            const bool changed_catalog = transformed && std::any_of(changed_original_hosted.begin(), changed_original_hosted.end(),
                [&](const auto& key) { return key.first == id; });
            if (id == plan.registry_id || ordinary.contains(id) || changed_catalog || entity.type == kSheetViewEntityType || entity.type == kAnnotationEntityType) continue;
            if (!exact(candidate.at(id), entity)) reject("replacement changed an unrelated retained owner: " + id);
        }
        return candidate;
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native candidate admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual-source replay: ") + error.what()); }
}
} // namespace sketch
