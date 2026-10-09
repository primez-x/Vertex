#include "sketch/stair_clone.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/stair_attachment_integrity.hpp"
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

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = StairCloneEntities;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t entity_limit = 65536, identity_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024, geometry_limit = 100000;
constexpr std::size_t source_nodes = 4 * 1024 * 1024, source_bytes = 64 * 1024 * 1024;
[[noreturn]] void reject(const std::string& reason) { throw std::invalid_argument("Stair clone: " + reason); }
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
    if (id.empty() || id.size() > limit || std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }))
        reject("qualified source local identity must be bounded and nonblank");
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
void diagnostic(StairClonePlan& plan, const std::string& id, const std::string& reason) {
    if (plan.diagnostics.size() >= 128) return;
    const StairCloneDiagnostic item{id, reason, true};
    if (std::find(plan.diagnostics.begin(), plan.diagnostics.end(), item) == plan.diagnostics.end()) plan.diagnostics.push_back(item);
}
bool physical_owner(const Entity& entity) { return entity.type == "stair" || entity.type == "railing"; }
std::optional<std::string> host_id(const Entity& entity) {
    if (entity.type != "railing") return std::nullopt;
    const auto host = field(entity.properties, "host");
    const auto stair = host ? field(*host, "stair_id") : nullptr;
    if (!stair || !stair->is_string()) return std::nullopt;
    const auto railing = decode_railing_properties(entity.id, entity.properties);
    if (railing.host) return railing.host->stair_id;
    if (railing.landing_host) return railing.landing_host->stair_id;
    reject("host slot has no supported railing binding: " + entity.id);
}
void context(const Entities& actual, const ProjectOrganization& organization, const Entity& entity) {
    if (entity.type == "assembly_model") {
        struct Binding { const char* key; const char* type; };
        constexpr Binding bindings[]{{"property_id", "property"}, {"building_id", "building"},
            {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}};
        for (const auto& binding : bindings) {
            const auto value = field(entity.properties, binding.key); if (!value) continue;
            if (!value->is_string()) reject("affected catalog has malformed context: " + entity.id);
            const auto owner = actual.find(value->get_ref<const std::string&>());
            if (owner == actual.end() || owner->second.type != binding.type) reject("affected catalog has unresolved actual context: " + entity.id);
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
        !assignment.contains("material_id") || !assignment.at("material_id").is_string()) reject("affected owner has unsupported material assignment: " + entity.id);
    const auto catalog = actual.find(assignment.at("catalog_id").get<std::string>());
    if (catalog == actual.end() || catalog->second.type != "assembly_model") reject("material requires its actual catalog");
    const auto model = AssemblyModel::from_json(catalog->second.properties.at("model"));
    const auto material_id = assignment.at("material_id").get<std::string>();
    if (std::none_of(model.materials().begin(), model.materials().end(), [&](const auto& row) { return row.id == material_id; }))
        reject("material is absent from its actual catalog: " + entity.id);
}
std::size_t physical_cost(const Entities& source, const Entity& entity, const BuildingObject& object) {
    if (const auto stair = std::get_if<StairFlight>(&object)) return stair->riser_count + stair->landings.size() + 1;
    const auto& rail = std::get<Railing>(object);
    if (const auto host = host_id(entity)) return derive_hosted_railing_layout(rail, decode_stair_properties(*host,
        resolve_vertical_placement(source, source.at(*host)).properties)).posts.size() + 1;
    return static_cast<std::size_t>(std::ceil(rail.length / rail.post_spacing)) + 2;
}
void admit_physical(const Entities& source, const Ids& owners) {
    validate_stair_attachment_state(source);
    validate_document_site_frames(source);
    const auto organization = organize_project(source);
    std::map<std::string, BuildingObject, std::less<>> objects;
    std::size_t work{};
    for (const auto& id : owners) {
        const auto& entity = source.at(id); context(source, organization, entity);
        const auto resolved = host_id(entity) ? entity : resolve_vertical_placement(source, entity);
        auto object = decode_building_entity(resolved);
        const auto cost = physical_cost(source, entity, object);
        if (cost > geometry_limit - work) reject("complete physical native geometry budget exceeded");
        work += cost; objects.emplace(id, std::move(object));
    }
    for (const auto& [id, object] : objects) { (void)id; (void)make_building_shape(object, source); }
}
void admit_instances(const Entities& source, const std::set<StairCloneHostedInstanceKey>& selected) {
    AssemblyExpansionBudget budget;
    std::map<std::string, AssemblyModel, std::less<>> models;
    std::vector<AssemblyExpansion> expansions;
    std::vector<std::pair<BuildingObject, AssemblyTransform>> legacy;
    std::size_t work{};
    for (const auto& key : selected) {
        if (!models.contains(key.first)) models.emplace(key.first, AssemblyModel::from_json(source.at(key.first).properties.at("model")));
        const auto& model = models.at(key.first);
        const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == key.second; });
        if (row == model.instances().end() || !row->placement) reject("actual selected hosted row is missing");
        const auto& placement = *row->placement;
        const auto host = source.find(placement.host_entity_id);
        if (host == source.end() || !physical_owner(host->second)) reject("hosted row requires its actual stair/railing host");
        auto expansion = model.expand(*row, budget);
        if (!expansion.profiles.empty()) expansions.push_back(std::move(expansion));
        else {
            const auto resolved = host_id(host->second) ? host->second : resolve_vertical_placement(source, host->second);
            auto object = decode_building_entity(resolved);
            const auto cost = physical_cost(source, host->second, object);
            if (cost > geometry_limit - work) reject("host-derived component native work budget exceeded");
            work += cost;
            legacy.emplace_back(std::move(object), AssemblyTransform{{placement.translation_m.x, placement.translation_m.y,
                placement.translation_z_m}, placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
        }
    }
    for (const auto& expansion : expansions) (void)make_assembly_geometry(expansion);
    for (const auto& [object, transform] : legacy) (void)transform_assembly_shape(make_building_shape(object, source), transform);
}
bool affected_overlay(const Json& row, const Ids& owners) {
    const auto owner = field(row, "object_id");
    const auto binding = field(row, "dimension_binding");
    const auto bound_owner = binding ? field(*binding, "object_id") : nullptr;
    return (owner && owner->is_string() && owners.contains(owner->get_ref<const std::string&>())) ||
        (bound_owner && bound_owner->is_string() && owners.contains(bound_owner->get_ref<const std::string&>()));
}
// Strip only supported reference slots in scratch admission records.
Entity remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "stair") {
        (void)decode_stair_properties(entity.id, p);
        for (const auto* name : {"flights", "landings"}) if (p.contains(name)) for (auto& row : p.at(name)) row.erase("id");
    } else if (entity.type == "railing") {
        (void)decode_railing_properties(entity.id, p);
        if (p.contains("host")) for (const auto* name : {"stair_id", "flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"}) p.at("host").erase(name);
    } else if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(p.at("model"));
        for (auto& row : p.at("model").at("instances")) {
            row.erase("id"); if (row.contains("placement")) row.at("placement").erase("host_entity_id");
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
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) if (row.at("target_kind") == "object") row.erase("target_id");
    }
    // Physical material assignments retain their original, shared catalog.
    // Its supported reference is validated by context, never remapped.
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
        if (type.contains("parts")) for (auto& part : type.at("parts")) { part.erase("id"); part.erase("type_id"); clear_materials(part); }
    }
    for (auto& row : model.at("instances")) {
        row.erase("type_id"); clear_materials(row);
        if (row.contains("nested_overrides")) for (auto& change : row.at("nested_overrides")) { change.erase("part_path"); clear_materials(change); }
    }
    return entity;
}
struct Derivation {
    StairClonePlan plan;
    Entities physical;
    Ids owners, catalogs;
    EmbeddedAssemblyPresentationIds original_aliases;
    std::map<std::string, ArchitecturalGroupTransform, std::less<>> operations;
};
// Bound complete copy and presentation inventories before the typed transform
// producer enters physical codecs or native admission. Bare JSON text grants
// no rewrite authority; discovery below still admits every exact qualified row.
void closure_bounds(const Entities& actual, const Ids& owners) {
    std::size_t identities = owners.size(), inventory{};
    const auto add = [&](std::size_t size) {
        if (size > identity_limit - identities) reject("combined copy identity budget exceeded before codec/native admission");
        identities += size;
    };
    for (const auto& id : owners) if (actual.at(id).type == "stair") {
        for (const auto* name : {"flights", "landings"}) if (const auto rows = field(actual.at(id).properties, name)) {
            if (!rows->is_array()) reject("actual copied child inventory is malformed"); add(rows->size());
        }
    }
    std::set<StairCloneHostedInstanceKey> selected;
    for (const auto& [id, entity] : actual) {
        if (entity.type != "assembly_model" || (!touches(entity.properties, owners) && !touches(entity.extensions, owners))) continue;
        const auto model = field(entity.properties, "model");
        if (!model) reject("affected actual catalog lacks a model");
        for (const auto* name : {"materials", "types", "instances"}) {
            const auto rows = field(*model, name);
            if (!rows || !rows->is_array() || rows->size() > entity_limit - inventory) reject("affected catalog inventory is not bounded");
            inventory += rows->size();
        }
        bool included = false;
        for (const auto& row : model->at("instances")) {
            const auto placement = field(row, "placement"), name = field(row, "id");
            const auto host = placement ? field(*placement, "host_entity_id") : nullptr;
            if (!host || !host->is_string() || !owners.contains(host->get_ref<const std::string&>())) continue;
            if (!name || !name->is_string()) reject("affected actual hosted row has no local identity");
            if (!selected.emplace(id, name->get<std::string>()).second) reject("affected actual hosted row has duplicate local identity");
            included = true; add(1);
        }
        if (included) add(1);
    }
    Ids presentation = owners;
    if (!selected.empty()) {
        const auto aliases = embedded_assembly_presentation_ids(actual);
        for (const auto& key : selected) presentation.insert(aliases.at(key));
    }
    for (const auto& [id, entity] : actual) {
        (void)id;
        if (entity.type != kSheetViewEntityType || (!touches(entity.properties, presentation) && !touches(entity.extensions, presentation))) continue;
        const auto model = field(entity.properties, "model"); const auto views = model ? field(*model, "views") : nullptr;
        if (!views || !views->is_array()) reject("affected actual saved view inventory is malformed");
        for (const auto& view : *views) if (const auto rows = field(view, "overlays")) {
            if (!rows->is_array()) reject("affected actual saved overlay inventory is malformed");
            for (const auto& row : *rows) if (affected_overlay(row, presentation)) add(1);
        }
    }
}
Derivation derive(const Entities& actual, const std::vector<StairTransformIntent>& transforms) {
    Derivation result; auto& plan = result.plan;
    try {
        (void)source_budget(actual);
        if (transforms.empty() || transforms.size() > maximum_architectural_group_targets) reject("requires a bounded nonempty captured transform batch");
        std::size_t bytes{};
        const auto active = constraint_phase_scope(actual);
        for (const auto& transform : transforms) {
            const auto size = encode_stair_transform_intent(transform).dump().size();
            if (size > proof_limit - bytes) reject("captured transform batch byte budget exceeded"); bytes += size;
            if (!result.operations.emplace(transform.object_id, transform.transform).second) reject("duplicate actual selected owner");
            const auto found = actual.find(transform.object_id);
            if (found == actual.end() || !physical_owner(found->second)) reject("selected target requires an actual stair or railing: " + transform.object_id);
            if (active.inactive_owner_ids.contains(transform.object_id)) reject("selected actual owner is inactive: " + transform.object_id);
            result.owners.insert(transform.object_id); plan.selected_object_ids.push_back(transform.object_id);
        }
        for (const auto& [id, entity] : actual) {
            const auto raw = field(entity.properties, "host"); const auto host = raw ? field(*raw, "stair_id") : nullptr;
            if (entity.type != "railing" || !host || !host->is_string() || !result.owners.contains(host->get_ref<const std::string&>())) continue;
            if (active.inactive_owner_ids.contains(id)) continue;
            const auto& raw_host = host->get_ref<const std::string&>();
            if (actual.at(raw_host).type != "stair") reject("attached rail lacks its actual stair host: " + id);
            result.owners.insert(id); result.operations[id] = result.operations.at(raw_host);
        }
        if (result.owners.size() > identity_limit) reject("physical closure identity budget exceeded");
        closure_bounds(actual, result.owners);
        // O enforces selected hosted rails' actual selected host and equivalent
        // affine operator. It admits actual/candidate attachments and levels.
        result.physical = replay_stair_transform_entities(actual, transforms);
        const auto organization = organize_project(actual);
        for (const auto& id : result.owners) {
            context(actual, organization, actual.at(id));
            const auto& edited = result.physical.at(id);
            if (edited.type != "stair") continue;
            for (const auto* name : {"flights", "landings"}) if (edited.properties.contains(name)) for (const auto& child : edited.properties.at(name)) {
                const auto child_id = child.at("id").get<std::string>(); identity(child_id);
                plan.required_child_ids.emplace_back(id, child_id);
            }
        }
        std::set<StairCloneHostedInstanceKey> selected;
        std::size_t inventory{};
        for (const auto& [id, entity] : actual) {
            if (entity.type != "assembly_model" || (!touches(entity.properties, result.owners) && !touches(entity.extensions, result.owners))) continue;
            const auto& raw = entity.properties.at("model");
            const auto schema = field(raw, "schema"); bool supported = false;
            for (int version = 1; version <= 7; ++version) if (schema && *schema == "sketch.assemblies.v" + std::to_string(version)) supported = true;
            if (!supported) reject("affected catalog has no supported schema 1..7: " + id);
            for (const auto* name : {"materials", "types", "instances"}) {
                const auto rows = field(raw, name);
                if (!rows || !rows->is_array() || rows->size() > entity_limit - inventory) reject("affected catalog inventory is not bounded");
                inventory += rows->size();
            }
            const auto catalog = AssemblyModel::from_json(raw); bool included = false;
            for (const auto& row : catalog.instances()) if (row.placement && result.owners.contains(row.placement->host_entity_id)) { selected.emplace(id, row.id); included = true; }
            if (!included) continue;
            if (active.inactive_owner_ids.contains(id)) reject("actual selected hosted catalog is inactive: " + id);
            context(actual, organization, entity); result.catalogs.insert(id);
        }
        plan.required_hosted_instance_ids.assign(selected.begin(), selected.end());
        if (!selected.empty()) {
            admit_instances(actual, selected);
            const auto aliases = embedded_assembly_presentation_ids(actual);
            for (const auto& key : selected) result.original_aliases.emplace(key, aliases.at(key));
        }
        Ids presentation = result.owners;
        for (const auto& [key, alias] : result.original_aliases) { (void)key; presentation.insert(alias); }
        std::set<StairCloneOverlayKey> overlays;
        for (const auto& [id, entity] : actual) {
            if (entity.type != kSheetViewEntityType || (!touches(entity.properties, presentation) && !touches(entity.extensions, presentation))) continue;
            const auto views = decode_sheet_view_entity(entity);
            for (const auto& view : views.views()) for (const auto& overlay : view.overlays) {
                if (!presentation.contains(overlay.object_id) && (!overlay.dimension_binding || !presentation.contains(overlay.dimension_binding->object_id))) continue;
                local_identity(view.id, proof_limit); local_identity(overlay.id);
                if (!overlays.emplace(id, view.id, overlay.id).second) reject("affected overlay has duplicate qualified identity");
            }
        }
        plan.required_overlay_ids.assign(overlays.begin(), overlays.end());
        for (const auto& [id, entity] : actual) {
            if (!result.catalogs.contains(id) && !result.owners.contains(id) && !touches(entity.properties, presentation) && !touches(entity.extensions, presentation)) continue;
            auto scratch = remainder(entity), external = scratch;
            if (entity.type == kSheetViewEntityType) for (auto& view : external.properties.at("model").at("views")) view.erase("id");
            if (touches(external.properties, presentation) || touches(external.extensions, presentation))
                diagnostic(plan, id, "affected opaque reference has no qualified stair clone codec");
            // Bare child/instance/overlay spellings belong only to their actual
            // owner namespace. An unrelated owner's same spelling is inert.
            if (result.owners.contains(id) && entity.type == "stair") {
                Ids children;
                for (const auto& key : plan.required_child_ids) if (key.first == id) children.insert(key.second);
                if (touches(scratch.properties, children) || touches(scratch.extensions, children)) diagnostic(plan, id, "copied owner has an opaque reference to an original child");
            }
            if (result.owners.contains(id) && entity.type == "railing") {
                if (const auto host = host_id(entity)) {
                    Ids children;
                    for (const auto& key : plan.required_child_ids) if (key.first == *host) children.insert(key.second);
                    if (touches(scratch.properties, children) || touches(scratch.extensions, children))
                        diagnostic(plan, id, "copied rail has an opaque reference to an original host child");
                }
            }
            if (result.catalogs.contains(id)) {
                auto local = catalog_local_remainder(scratch); Ids names; auto rows = Json::array();
                const auto& source_rows = entity.properties.at("model").at("instances");
                for (std::size_t i = 0; i < source_rows.size(); ++i) {
                    const auto name = source_rows.at(i).at("id").get<std::string>(); names.insert(name);
                    if (selected.contains({id, name})) rows.push_back(local.properties.at("model").at("instances").at(i));
                }
                local.properties.at("model").at("instances") = std::move(rows);
                if (touches(local.properties, names) || touches(local.extensions, names)) diagnostic(plan, id, "copied catalog has an opaque reference to an original local instance");
            }
            if (entity.type == kSheetViewEntityType) for (const auto& view : scratch.properties.at("model").at("views")) {
                Ids names;
                for (const auto& key : overlays) if (std::get<0>(key) == id && std::get<1>(key) == view.at("id").get<std::string>()) names.insert(std::get<2>(key));
                auto local = view; local.erase("id");
                if (touches(local, names)) diagnostic(plan, id, "affected overlay-local opaque reference cannot be copied");
            }
        }
        Ids required = result.owners; required.insert(result.catalogs.begin(), result.catalogs.end());
        plan.required_entity_ids.assign(required.begin(), required.end());
        std::sort(plan.selected_object_ids.begin(), plan.selected_object_ids.end()); std::sort(plan.required_child_ids.begin(), plan.required_child_ids.end());
        if (required.size() + plan.required_child_ids.size() + selected.size() + overlays.size() > identity_limit) reject("combined physical/child/hosted/overlay identity budget exceeded");
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); diagnostic(plan, {}, std::string("native geometry admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const std::exception& error) { diagnostic(plan, {}, error.what()); }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& a, const auto& b) { return std::pair{a.entity_id, a.reason} < std::pair{b.entity_id, b.reason}; });
    return result;
}
void remap_field(Json& value, const char* name, const StairCloneIdentityMap& mapping) {
    const auto old = field(value, name); if (!old) return;
    const auto found = mapping.find(old->get<std::string>()); if (found != mapping.end()) value.at(name) = found->second;
}
void remap_host(Entity& entity, const StairCloneAuthoring& authoring) {
    const auto host_id_before = host_id(entity); if (!host_id_before) return;
    if (!authoring.identities.contains(*host_id_before)) reject("copied hosted rail requires its qualified copied host");
    auto& host = entity.properties.at("host");
    for (const auto* name : {"flight_id", "landing_id", "incoming_flight_id", "outgoing_flight_id"}) {
        const auto child = field(host, name); if (!child) continue;
        const auto mapped = authoring.child_identities.find({*host_id_before, child->get<std::string>()});
        if (mapped == authoring.child_identities.end()) reject("copied rail lost its actual qualified child binding: " + entity.id + "/host/" + name);
        host.at(name) = mapped->second;
    }
    host.at("stair_id") = authoring.identities.at(*host_id_before);
}
void admit_rebound_quantities(const Entity& before, const Entity& after) {
    const auto entries = field(before.properties, "quantity_entries"); if (!entries) return;
    for (const auto& [pointer, receipt] : entries->items()) {
        (void)receipt;
        if (pointer == "/host" || pointer.starts_with("/host/")) {
            const auto old = field(before.properties, "host"), current = field(after.properties, "host");
            if ((!old != !current) || (old && *old != *current)) reject("opaque host quantity binding cannot follow copy: " + before.id + ":" + pointer);
        }
        for (const auto* name : {"flights", "landings"}) {
            const auto prefix = std::string("/") + name;
            if (pointer != prefix && !pointer.starts_with(prefix + "/")) continue;
            const auto old = field(before.properties, name), current = field(after.properties, name);
            if ((!old && !current) || (old && current && *old == *current)) continue;
            bool known = false;
            if (old && old->is_array()) for (std::size_t i = 0; i < old->size(); ++i) {
                const Ids dimensions = std::string_view(name) == "flights" ? Ids{"going_m", "width_m"} : Ids{"depth_m", "thickness_m", "return_gap_m"};
                for (const auto& dimension : dimensions) if (pointer == prefix + "/" + std::to_string(i) + "/" + dimension && old->at(i).contains(dimension)) known = true;
            }
            if (!known) reject("opaque child quantity binding cannot follow copy: " + before.id + ":" + pointer);
        }
    }
}
using PresentationTransforms = std::map<std::string, AssemblyTransform, std::less<>>;
AssemblyTransform presentation_transform(const Entities& actual, const std::string& owner,
    const ArchitecturalGroupTransform& captured) {
    const auto frame = resolve_site_presentation(actual, owner).forward;
    return conjugate_assembly_transform_through_rigid_frame(architectural_group_assembly_transform(captured),
        {{frame.translation_m.x, frame.translation_m.y, frame.translation_m.z}, frame.rotation_radians, 1.0, false});
}
// Same saved-frame projection as phase stair replacement. The saved origin and
// basis apply to each producer's coordinates, including non-plan saved views.
struct OverlayFrame {
    AssemblyPoint3 origin, right, up, normal;
    explicit OverlayFrame(const CoordinatedView& view)
        : up{view.up[0], view.up[1], view.up[2]},
          normal{-view.direction[0], -view.direction[1], -view.direction[2]} {
        const auto projection = coordinated_view_origins(view).projection_m;
        origin = {projection[0], projection[1], projection[2]};
        right = {up.y * normal.z - up.z * normal.y, up.z * normal.x - up.x * normal.z, up.x * normal.y - up.y * normal.x};
    }
    std::array<double, 2> project(AssemblyPoint3 point) const {
        point = {point.x - origin.x, point.y - origin.y, point.z - origin.z};
        return {point.x * right.x + point.y * right.y + point.z * right.z, point.x * up.x + point.y * up.y + point.z * up.z};
    }
    AssemblyPoint3 lift(std::array<double, 2> point) const {
        return {origin.x + point[0] * right.x + point[1] * up.x, origin.y + point[0] * right.y + point[1] * up.y,
            origin.z + point[0] * right.z + point[1] * up.z};
    }
    std::array<double, 2> move(std::array<double, 2> point, const AssemblyTransform& transform) const {
        return project(transform_assembly_point(lift(point), transform));
    }
    std::array<double, 2> vector(AssemblyPoint3 direction, AssemblyTransform transform) const {
        transform.translation_m = {};
        const auto point = transform_assembly_point(direction, transform);
        return {point.x * right.x + point.y * right.y + point.z * right.z, point.x * up.x + point.y * up.y + point.z * up.z};
    }
};
TopoDS_Shape presentation_shape(const Entities& source, const std::string& owner, const EmbeddedAssemblyPresentationIds& aliases) {
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
    const auto& placement = *row->placement; const auto& host = source.at(placement.host_entity_id);
    const auto resolved = host_id(host) ? host : resolve_vertical_placement(source, host);
    return transform_assembly_shape(make_building_shape(decode_building_entity(resolved), source),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m}, placement.rotation_radians,
            placement.scale, placement.mirrored_y, placement.vertical_scale});
}
Bounds2 presentation_bounds(const TopoDS_Shape& shape, const OverlayFrame& frame) {
    const auto dot_origin = [&](AssemblyPoint3 axis) { return -(axis.x * frame.origin.x + axis.y * frame.origin.y + axis.z * frame.origin.z); };
    gp_Trsf transform;
    transform.SetValues(frame.right.x, frame.right.y, frame.right.z, dot_origin(frame.right),
        frame.up.x, frame.up.y, frame.up.z, dot_origin(frame.up), frame.normal.x, frame.normal.y, frame.normal.z, dot_origin(frame.normal));
    BRepBuilderAPI_Transform operation(shape, transform, true);
    if (!operation.IsDone() || operation.Shape().IsNull()) reject("bound presentation cannot enter actual saved view frame");
    Bnd_Box box; BRepBndLib::AddOptimal(operation.Shape(), box, false, false);
    if (box.IsVoid() || box.IsOpen()) reject("bound presentation has no finite saved-view silhouette");
    double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
    for (const auto value : {x0, y0, x1, y1}) if (!std::isfinite(value) || std::abs(value) > 1e6) reject("bound presentation exceeds saved-view coordinate bounds");
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
std::size_t presentation_cost(const Entities& source, const std::string& owner, const EmbeddedAssemblyPresentationIds& aliases) {
    if (const auto found = source.find(owner); found != source.end() && physical_owner(found->second)) {
        const auto resolved = host_id(found->second) ? found->second : resolve_vertical_placement(source, found->second);
        return physical_cost(source, found->second, decode_building_entity(resolved));
    }
    const auto alias = std::find_if(aliases.begin(), aliases.end(), [&](const auto& row) { return row.second == owner; });
    if (alias == aliases.end()) reject("bound presentation has no actual qualified cost source");
    const auto model = AssemblyModel::from_json(source.at(alias->first.first).properties.at("model"));
    const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == alias->first.second; });
    if (row == model.instances().end() || !row->placement) reject("bound component has no actual cost host");
    AssemblyExpansionBudget budget; const auto expansion = model.expand(*row, budget);
    if (!expansion.profiles.empty()) return budget.consumed_nodes + budget.consumed_profile_segments;
    return presentation_cost(source, row->placement->host_entity_id, aliases);
}
void transform_overlay(Json& row, const CoordinatedView& view, const Entities& actual, const Entities& candidate,
    const StairCloneIdentityMap& mapping, const PresentationTransforms& transforms,
    const EmbeddedAssemblyPresentationIds& source_aliases, const EmbeddedAssemblyPresentationIds& candidate_aliases,
    const std::string& qualified_view, OverlayGeometryCache& source_cache, OverlayGeometryCache& candidate_cache) {
    const auto binding = field(row, "dimension_binding");
    const auto owner = binding && !binding->is_null() ? binding->at("object_id").get<std::string>() : row.value("object_id", std::string{});
    const auto found = transforms.find(owner);
    if (found == transforms.end()) reject("copied overlay has no captured source-qualified transform");
    // Identity copy keeps raw admitted coordinates and dimension receipts.
    if (found->second == AssemblyTransform{}) return;
    const auto& transform = found->second; const OverlayFrame frame(view);
    if (binding && !binding->is_null()) {
        const auto normal = frame.vector(frame.normal, transform);
        if (std::hypot(normal[0], normal[1]) > 1e-10) reject("bound dimension transform depends on source depth in its saved view");
        const bool horizontal = binding->at("axis") == "horizontal";
        const auto axis = frame.vector(horizontal ? frame.right : frame.up, transform);
        const bool next_horizontal = std::abs(axis[1]) <= 1e-10 && std::abs(axis[0]) > 1e-10;
        const bool next_vertical = std::abs(axis[0]) <= 1e-10 && std::abs(axis[1]) > 1e-10;
        if (!next_horizontal && !next_vertical) reject("transformed bound dimension axis is not representable in its saved view");
        const auto before = source_cache.get(actual, owner, qualified_view, frame, source_aliases);
        const auto after = candidate_cache.get(candidate, mapping.at(owner), qualified_view, frame, candidate_aliases);
        const double location = (horizontal ? before.maximum.y : before.maximum.x) + binding->at("line_offset_m").get<double>();
        const auto moved = frame.move(horizontal ? std::array<double, 2>{0, location} : std::array<double, 2>{location, 0}, transform);
        row.at("dimension_binding").at("axis") = next_horizontal ? "horizontal" : "vertical";
        row.at("dimension_binding").at("line_offset_m") = next_horizontal ? moved[1] - after.maximum.y : moved[0] - after.maximum.x;
    } else for (const auto* name : {"start_m", "end_m"}) {
        const auto& old = row.at(name); row.at(name) = frame.move({old.at(0).get<double>(), old.at(1).get<double>()}, transform);
    }
}
void presentation(Entities& candidate, const Entities& actual, const StairCloneIdentityMap& mapping,
    const StairCloneOverlayIdentityMap& overlay_ids, const PresentationTransforms& transforms) {
    Ids owners; for (const auto& [old, proposed] : mapping) { (void)proposed; owners.insert(old); }
    EmbeddedAssemblyPresentationIds source_aliases, candidate_aliases;
    if (std::any_of(mapping.begin(), mapping.end(), [&](const auto& row) { return !actual.contains(row.first); })) {
        source_aliases = embedded_assembly_presentation_ids(actual); candidate_aliases = embedded_assembly_presentation_ids(candidate);
    }
    OverlayGeometryCache source_cache, candidate_cache;
    std::set<StairCloneOverlayKey> projected;
    std::map<std::string, std::size_t, std::less<>> source_costs, candidate_costs;
    std::size_t work{};
    for (const auto& [id, original] : actual) {
        if (original.type != kSheetViewEntityType || !touches(original.properties, owners)) continue;
        const auto views = decode_sheet_view_entity(original);
        for (const auto& view : views.views()) for (const auto& overlay : view.overlays) {
            if (!overlay.dimension_binding || (!owners.contains(overlay.object_id) && !owners.contains(overlay.dimension_binding->object_id))) continue;
            const auto& owner = overlay.dimension_binding->object_id;
            if (!mapping.contains(owner)) reject("copied bound overlay refers to a source outside copied transform cohort");
            if (transforms.at(owner) == AssemblyTransform{}) continue;
            if (!projected.emplace(id, view.id, owner).second) continue;
            const auto& proposed = mapping.at(owner);
            if (!source_costs.contains(owner)) source_costs.emplace(owner, presentation_cost(actual, owner, source_aliases));
            if (!candidate_costs.contains(proposed)) candidate_costs.emplace(proposed, presentation_cost(candidate, proposed, candidate_aliases));
            for (const auto cost : {source_costs.at(owner), candidate_costs.at(proposed)}) {
                if (cost > geometry_limit - work) reject("aggregate bound saved-view native work budget exceeded"); work += cost;
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
                    for (auto row : retained) if (owners.contains(row.at("object_id").get<std::string>())) { remap_field(row, "object_id", mapping); rows.push_back(std::move(row)); }
                }
                if (view.contains("overlays")) {
                    auto& rows = view.at("overlays"); const auto retained = rows;
                    for (auto row : retained) if (affected_overlay(row, owners)) {
                        const auto saved = std::find_if(decoded.views().begin(), decoded.views().end(), [&](const auto& item) { return item.id == view_id; });
                        if (saved == decoded.views().end()) reject("copied overlay lost its actual qualified saved view");
                        const auto qualified = Json::array({id, view_id}).dump();
                        transform_overlay(row, *saved, actual, candidate, mapping, transforms, source_aliases, candidate_aliases, qualified, source_cache, candidate_cache);
                        row.at("id") = overlay_ids.at({id, view_id, row.at("id").get<std::string>()});
                        remap_field(row, "object_id", mapping);
                        if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) remap_field(row.at("dimension_binding"), "object_id", mapping);
                        rows.push_back(std::move(row));
                    }
                }
            }
            validate_sheet_view_entity(candidate.at(id));
        } else if (original.type == kAnnotationEntityType) {
            auto& rows = candidate.at(id).properties.at("state").at("overrides"); const auto retained = rows;
            for (auto row : retained) if (row.at("target_kind") == "object" && owners.contains(row.at("target_id").get<std::string>())) { remap_field(row, "target_id", mapping); rows.push_back(std::move(row)); }
            validate_annotation_entity(candidate.at(id));
        }
    }
}
} // namespace

bool StairClonePlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& row) { return row.blocking; });
}
StairClonePlan inspect_stair_clone_plan(const Entities& actual, const std::vector<StairTransformIntent>& transforms) {
    return derive(actual, transforms).plan;
}
Json encode_stair_clone_authoring(const StairCloneAuthoring& authoring) {
    if (authoring.transforms.empty() || authoring.transforms.size() > maximum_architectural_group_targets || authoring.identities.empty() ||
        authoring.identities.size() > identity_limit || authoring.child_identities.size() > identity_limit - authoring.identities.size() ||
        authoring.hosted_instance_identities.size() > identity_limit - authoring.identities.size() - authoring.child_identities.size() ||
        authoring.overlay_identities.size() > identity_limit - authoring.identities.size() - authoring.child_identities.size() - authoring.hosted_instance_identities.size())
        reject("authoring inventories exceed combined bounds or lack captured transforms/owners");
    Ids targets, fresh; Json transforms = Json::array(), identities = Json::object(); std::size_t bytes{};
    for (const auto& transform : authoring.transforms) {
        if (!targets.insert(transform.object_id).second) reject("duplicate actual transform root");
        auto value = encode_stair_transform_intent(transform); const auto size = value.dump().size();
        if (size > proof_limit - bytes) reject("captured transform batch byte budget exceeded"); bytes += size; transforms.push_back(std::move(value));
    }
    const auto reserve = [&](const std::string& destination) { identity(destination); if (!fresh.insert(destination).second) reject("fresh destinations overlap across qualified mappings"); };
    for (const auto& [old, proposed] : authoring.identities) {
        identity(old); reserve(proposed); if (old == proposed) reject("entity destination is not fresh"); identities[old] = proposed;
    }
    Json children = Json::array(), hosted = Json::array(), overlays = Json::array();
    for (const auto& [key, proposed] : authoring.child_identities) {
        identity(key.first); identity(key.second); reserve(proposed); if (key.second == proposed) reject("child destination is not fresh");
        children.push_back({{"owner_id", key.first}, {"child_id", key.second}, {"proposed_child_id", proposed}});
    }
    std::size_t local_bytes{};
    for (const auto& [key, proposed] : authoring.hosted_instance_identities) {
        identity(key.first); local_identity(key.second, proof_limit); reserve(proposed); if (key.second == proposed) reject("hosted destination is not fresh");
        const auto size = key.first.size() + key.second.size() + proposed.size();
        if (size > proof_limit - local_bytes) reject("qualified hosted identity byte budget exceeded"); local_bytes += size;
        hosted.push_back({{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_instance_id", proposed}});
    }
    for (const auto& [key, proposed] : authoring.overlay_identities) {
        identity(std::get<0>(key)); local_identity(std::get<1>(key), proof_limit); local_identity(std::get<2>(key)); reserve(proposed);
        if (std::get<2>(key) == proposed) reject("overlay destination is not fresh");
        const auto size = std::get<0>(key).size() + std::get<1>(key).size() + std::get<2>(key).size() + proposed.size();
        if (size > proof_limit - local_bytes) reject("qualified local identity byte budget exceeded"); local_bytes += size;
        overlays.push_back({{"view_entity_id", std::get<0>(key)}, {"saved_view_id", std::get<1>(key)}, {"overlay_id", std::get<2>(key)}, {"proposed_overlay_id", proposed}});
    }
    Json result{{"version", 1}, {"transforms", std::move(transforms)}, {"identities", std::move(identities)},
        {"child_identities", std::move(children)}, {"hosted_instance_identities", std::move(hosted)}, {"overlay_identities", std::move(overlays)}};
    proof_budget(result); return result;
}
StairCloneAuthoring decode_stair_clone_authoring(const Json& value) {
    try {
        proof_budget(value);
        keys(value, {"version", "transforms", "identities", "child_identities", "hosted_instance_identities", "overlay_identities"});
        if (!value.at("version").is_number_integer() || value.at("version") != 1 || !value.at("transforms").is_array() ||
            !value.at("identities").is_object() || !value.at("child_identities").is_array() ||
            !value.at("hosted_instance_identities").is_array() || !value.at("overlay_identities").is_array()) reject("unsupported authoring version or shape");
        StairCloneAuthoring result;
        for (const auto& transform : value.at("transforms")) result.transforms.push_back(decode_stair_transform_intent(transform));
        for (const auto& [old, proposed] : value.at("identities").items()) { identity(old); result.identities.emplace(old, identity(proposed)); }
        for (const auto& row : value.at("child_identities")) {
            keys(row, {"owner_id", "child_id", "proposed_child_id"});
            if (!result.child_identities.emplace(std::pair{identity(row.at("owner_id")), identity(row.at("child_id"))}, identity(row.at("proposed_child_id"))).second) reject("duplicate qualified child source identity");
        }
        for (const auto& row : value.at("hosted_instance_identities")) {
            keys(row, {"catalog_id", "instance_id", "proposed_instance_id"});
            if (!row.at("instance_id").is_string()) reject("hosted local identity must be a string");
            if (!result.hosted_instance_identities.emplace(std::pair{identity(row.at("catalog_id")), row.at("instance_id").get<std::string>()}, identity(row.at("proposed_instance_id"))).second) reject("duplicate qualified hosted source identity");
        }
        for (const auto& row : value.at("overlay_identities")) {
            keys(row, {"view_entity_id", "saved_view_id", "overlay_id", "proposed_overlay_id"});
            if (!row.at("saved_view_id").is_string() || !row.at("overlay_id").is_string()) reject("overlay local identities must be strings");
            if (!result.overlay_identities.emplace(StairCloneOverlayKey{identity(row.at("view_entity_id")), row.at("saved_view_id").get<std::string>(), row.at("overlay_id").get<std::string>()}, identity(row.at("proposed_overlay_id"))).second) reject("duplicate qualified overlay source identity");
        }
        if (encode_stair_clone_authoring(result) != value) reject("qualified authoring rows are not canonical");
        return result;
    } catch (const Json::exception& error) { reject(std::string("malformed authoring: ") + error.what()); }
}
Entities replay_stair_clone_authoring(const Entities& actual, const StairCloneAuthoring& authoring) {
    try {
        const auto proof = encode_stair_clone_authoring(authoring);
        const auto derived = derive(actual, authoring.transforms); const auto& plan = derived.plan;
        if (!plan.ready()) reject(plan.diagnostics.front().entity_id + ": " + plan.diagnostics.front().reason);
        const Ids entities(plan.required_entity_ids.begin(), plan.required_entity_ids.end());
        const std::set<StairCloneChildKey> children(plan.required_child_ids.begin(), plan.required_child_ids.end());
        const std::set<StairCloneHostedInstanceKey> hosted(plan.required_hosted_instance_ids.begin(), plan.required_hosted_instance_ids.end());
        const std::set<StairCloneOverlayKey> overlays(plan.required_overlay_ids.begin(), plan.required_overlay_ids.end());
        if (authoring.identities.size() != entities.size() || authoring.child_identities.size() != children.size() ||
            authoring.hosted_instance_identities.size() != hosted.size() || authoring.overlay_identities.size() != overlays.size()) reject("requires complete exact actual-source qualified mapping inventories");
        auto occupied = source_budget(actual); Strings authored; authored.read(proof);
        // Reserve all original computed aliases, including unselected rows.
        const auto original_aliases = embedded_assembly_presentation_ids(actual);
        for (const auto& [key, alias] : original_aliases) { (void)key; occupied.text(alias); }
        Strings source_frame;
        for (const auto& transform : authoring.transforms) source_frame.read(encode_stair_transform_intent(transform));
        Ids fresh;
        const auto reserve = [&](const std::string& id) {
            if (occupied.values.contains(id) || source_frame.values.contains(id) || !fresh.insert(id).second) reject("fresh identity collides with actual source, captured source vocabulary or another destination: " + id);
        };
        for (const auto& [old, proposed] : authoring.identities) { if (!entities.contains(old)) reject("unrequested entity mapping"); reserve(proposed); }
        for (const auto& [key, proposed] : authoring.child_identities) { if (!children.contains(key)) reject("unrequested qualified child mapping"); reserve(proposed); }
        for (const auto& [key, proposed] : authoring.hosted_instance_identities) { if (!hosted.contains(key)) reject("unrequested qualified hosted mapping"); reserve(proposed); }
        for (const auto& [key, proposed] : authoring.overlay_identities) { if (!overlays.contains(key)) reject("unrequested qualified overlay mapping"); reserve(proposed); }
        // Actual originals, including registries and every catalog, are retained
        // raw-exact. Scratch transform results grant only additive copy authority.
        auto candidate = actual; Ids physical_copies;
        for (const auto& id : derived.owners) {
            auto copy = derived.physical.at(id); copy.id = authoring.identities.at(id);
            if (copy.type == "stair") {
                for (const auto* name : {"flights", "landings"}) if (copy.properties.contains(name)) for (auto& row : copy.properties.at(name)) row.at("id") = authoring.child_identities.at({id, row.at("id").get<std::string>()});
            } else remap_host(copy, authoring);
            admit_rebound_quantities(derived.physical.at(id), copy);
            const auto copy_id = copy.id;
            if (!candidate.emplace(copy_id, std::move(copy)).second) reject("fresh physical insertion collided"); physical_copies.insert(copy_id);
        }
        for (const auto& id : derived.catalogs) {
            auto copy = derived.physical.at(id); copy.id = authoring.identities.at(id);
            auto& copied_rows = copy.properties.at("model").at("instances"); const auto retained = copied_rows; copied_rows = Json::array();
            for (auto row : retained) {
                const StairCloneHostedInstanceKey key{id, row.at("id").get<std::string>()}; if (!hosted.contains(key)) continue;
                row.at("id") = authoring.hosted_instance_identities.at(key); remap_field(row.at("placement"), "host_entity_id", authoring.identities); copied_rows.push_back(std::move(row));
            }
            const auto copy_id = copy.id;
            if (!candidate.emplace(copy_id, std::move(copy)).second) reject("fresh private catalog insertion collided");
        }
        (void)source_budget(candidate);
        validate_stair_identity_transition(actual, candidate, std::span<const RevisionRecord>{});
        admit_physical(candidate, physical_copies);
        StairCloneIdentityMap presentation_ids;
        for (const auto& id : derived.owners) presentation_ids.emplace(id, authoring.identities.at(id));
        std::set<StairCloneHostedInstanceKey> copied_hosted;
        const auto before_presentation = embedded_assembly_presentation_ids(candidate);
        for (const auto& [key, alias] : original_aliases)
            if (before_presentation.at(key) != alias) reject("copy insertion changed an original qualified component alias");
        if (!hosted.empty()) {
            Ids fresh_aliases;
            for (const auto& key : hosted) {
                const StairCloneHostedInstanceKey copied{authoring.identities.at(key.first), authoring.hosted_instance_identities.at(key)}; copied_hosted.insert(copied);
                const auto& alias = before_presentation.at(copied);
                if (occupied.values.contains(alias) || authored.values.contains(alias) || fresh.contains(alias) || !fresh_aliases.insert(alias).second) reject("fresh component alias collides with actual/proof/fresh namespace");
                if (!presentation_ids.emplace(original_aliases.at(key), alias).second) reject("computed alias overlaps actual physical mapping");
            }
        }
        admit_instances(candidate, copied_hosted);
        PresentationTransforms operations;
        for (const auto& id : derived.owners)
            operations.emplace(id, architectural_group_assembly_transform(derived.operations.at(id)));
        std::map<std::string, AssemblyModel, std::less<>> catalogs;
        AssemblyExpansionBudget presentation_expansion;
        for (const auto& key : hosted) {
            if (!catalogs.contains(key.first)) catalogs.emplace(key.first, AssemblyModel::from_json(actual.at(key.first).properties.at("model")));
            const auto& model = catalogs.at(key.first);
            const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& item) { return item.id == key.second; });
            if (row == model.instances().end() || !row->placement) reject("copied component lost its source-qualified captured host");
            const auto& host = row->placement->host_entity_id;
            const auto expansion = model.expand(*row, presentation_expansion);
            operations.emplace(original_aliases.at(key), expansion.profiles.empty() ? operations.at(host)
                : presentation_transform(actual, host, derived.operations.at(host)));
        }
        presentation(candidate, actual, presentation_ids, authoring.overlay_identities, operations);
        if (embedded_assembly_presentation_ids(candidate) != before_presentation) reject("presentation append changed qualified original or copied aliases");
        (void)source_budget(candidate); (void)constraint_phase_scope(candidate);
        for (const auto& [id, entity] : actual) {
            if (entity.type == kSheetViewEntityType || entity.type == kAnnotationEntityType) continue;
            if (!exact(candidate.at(id), entity)) reject("independent copy changed an original owner: " + id);
        }
        return candidate;
    } catch (const Standard_Failure& error) {
        const auto message = error.GetMessageString(); reject(std::string("native candidate admission failed: ") + (message ? message : "Open CASCADE failure"));
    } catch (const Json::exception& error) { reject(std::string("malformed actual-source replay: ") + error.what()); }
}
} // namespace sketch
