#include "sketch/architectural_object_removal.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/opening_host_geometry.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/slab_geometry_edit.hpp"
#include "sketch/slab_layer_stack_edit.hpp"
#include "sketch/stair_attachment_integrity.hpp"
#include "sketch/structural_object_edit.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using Keys = std::set<std::pair<std::string, std::string>>;
using OverlayChildren = std::map<std::pair<std::string, std::string>, Ids>;
using OpeningWalls = std::map<std::string, Wall, std::less<>>;
struct OpeningAdmission {
    OpeningWalls walls;
    std::map<std::string, std::vector<const Entity*>, std::less<>> siblings;
    bool indexed{};
};
constexpr std::size_t entity_limit = 65536, selection_limit = 1000, closure_limit = 4096;
constexpr std::size_t node_limit = 4 * 1024 * 1024, byte_limit = 64 * 1024 * 1024;
constexpr std::size_t phase_limit = 2000000, geometry_limit = 65536;
constexpr std::size_t opening_native_limit = 262144;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Architectural object removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters: " + id);
}
const Json* field(const Json& value, const char* name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name); return found == value.end() ? nullptr : &*found;
}
struct Budget {
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) reject("source string/key budget exceeded");
        bytes += value.size();
    }
    void read(const Json& root) {
        std::vector<std::pair<const Json*, std::size_t>> pending{{&root, 0}};
        while (!pending.empty()) {
            const auto [value, depth] = pending.back(); pending.pop_back();
            if (depth > 64 || ++nodes > node_limit) reject("source JSON node/nesting budget exceeded");
            if (value->is_number_float() && !std::isfinite(value->get<double>())) reject("source has nonfinite scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size() > byte_limit - bytes) reject("source binary budget exceeded");
                bytes += value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size() > node_limit - nodes || pending.size() > node_limit - nodes - value->size())
                reject("source JSON pending-node budget exceeded");
            if (value->is_object()) for (const auto& [key, child] : value->items()) {
                text(key); pending.emplace_back(&child, depth + 1);
            } else for (const auto& child : *value) pending.emplace_back(&child, depth + 1);
        }
    }
};
void bounds(const Entities& source) {
    if (source.size() > entity_limit) reject("entity budget exceeded");
    Budget budget; std::size_t phase_work{}, catalog_rows{};
    for (const auto& [id, entity] : source) {
        identity(id);
        if (id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type); budget.read(entity.properties); budget.read(entity.extensions);
        if (entity.type == "model_phases") {
            const auto model = field(entity.properties, "model");
            const auto members = model ? field(*model, "entity_ids") : nullptr;
            const auto alternatives = model ? field(*model, "alternatives") : nullptr;
            if (!members || !members->is_array() || members->size() > entity_limit ||
                !alternatives || !alternatives->is_array() || alternatives->size() > closure_limit)
                reject("phase registry lacks bounded actual inventory: " + id);
            if (members->size() > (phase_limit - phase_work) / (alternatives->size() + 1))
                reject("phase state work budget exceeded");
            phase_work += members->size() * (alternatives->size() + 1);
        } else if (entity.type == "assembly_model") {
            const auto model = field(entity.properties, "model");
            for (const auto* key : {"materials", "types", "instances"}) {
                const auto rows = model ? field(*model, key) : nullptr;
                if (!rows || !rows->is_array() || rows->size() > entity_limit - catalog_rows)
                    reject("catalog lacks bounded actual inventory: " + id);
                catalog_rows += rows->size();
            }
        }
    }
}
bool touches(const Json& root, const Ids& ids) {
    if (ids.empty()) return false;
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto* value = pending.back(); pending.pop_back();
        if (value->is_string() && ids.contains(value->get_ref<const std::string&>())) return true;
        if (value->is_object()) for (const auto& [key, child] : value->items()) {
            if (ids.contains(key)) return true;
            pending.push_back(&child);
        } else if (value->is_array()) for (const auto& child : *value) pending.push_back(&child);
    }
    return false;
}
bool qualified_instance_reference(const Json& root, const Keys& instances) {
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto* value = pending.back(); pending.pop_back();
        if (value->is_object()) {
            const auto local = field(*value, "instance_id");
            for (const auto* key : {"catalog_id", "assembly_catalog_id"}) {
                const auto catalog = field(*value, key);
                if (catalog && catalog->is_string() && local && local->is_string() &&
                    instances.contains({catalog->get<std::string>(), local->get<std::string>()})) return true;
            }
            for (const auto& child : *value) pending.push_back(&child);
        } else if (value->is_array()) for (const auto& child : *value) pending.push_back(&child);
    }
    return false;
}
bool qualified_overlay_reference(const Json& root, const std::string& current_owner, const OverlayChildren& overlays) {
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto* value = pending.back(); pending.pop_back();
        if (value->is_object()) {
            const auto view = field(*value, "view_id"), overlay = field(*value, "overlay_id");
            if (view && view->is_string() && overlay && overlay->is_string()) {
                std::string owner = current_owner;
                for (const auto* key : {"sheet_view_entity_id", "sheet_view_id", "entity_id"}) {
                    const auto explicit_owner = field(*value, key);
                    if (explicit_owner && explicit_owner->is_string()) { owner = explicit_owner->get<std::string>(); break; }
                }
                const auto found = overlays.find({owner, view->get<std::string>()});
                if (found != overlays.end() && found->second.contains(overlay->get<std::string>())) return true;
            }
            for (const auto& child : *value) pending.push_back(&child);
        } else if (value->is_array()) for (const auto& child : *value) pending.push_back(&child);
    }
    return false;
}
template<class Predicate> void filter(Json& rows, Predicate keep) {
    auto retained = Json::array();
    for (const auto& row : rows) if (keep(row)) retained.push_back(row);
    rows = std::move(retained);
}
void remove_ids(Json& rows, const Ids& retired) {
    filter(rows, [&](const Json& row) { return !retired.contains(row.get<std::string>()); });
}
bool physical(const Entity& entity) {
    return entity.type == "stair" || entity.type == "railing" || entity.type == "column" ||
        entity.type == "beam" || entity.type == "slab";
}
bool component_host(const Entity& entity, bool allow_manufactured_opening_hosts) {
    return physical(entity) || entity.type == "wall" || entity.type == "roof" ||
        (allow_manufactured_opening_hosts && entity.type == "opening");
}
bool known_rail(const Entity& entity) {
    const auto version = field(entity.properties, "version"), form = field(entity.properties, "form");
    return entity.type == "railing" && version && version->is_number_integer() && form && form->is_string() &&
        ((*version == 1 && *form == "straight_railing") || (*version == 2 && *form == "stair_flight_railing") ||
            (*version == 3 && *form == "stair_landing_railing"));
}
std::optional<std::string> host(const Entity& entity) {
    if (!known_rail(entity)) return std::nullopt;
    const auto rail = decode_railing_properties(entity.id, entity.properties);
    if (rail.host) return rail.host->stair_id;
    if (rail.landing_host) return rail.landing_host->stair_id;
    return std::nullopt;
}
bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}
struct Phases {
    ConstraintPhaseScope scope;
    std::map<std::string, ModelPhases, std::less<>> models;
    std::map<std::string, std::string, std::less<>> owners;
};
Phases phases(const Entities& source) {
    Phases result; result.scope = constraint_phase_scope(source);
    for (const auto& registry : result.scope.registries) {
        result.models.emplace(registry.registry_id, ModelPhases::from_json(source.at(registry.registry_id).properties.at("model")));
        for (const auto& id : registry.registered_entity_ids)
            if (!result.owners.emplace(id, registry.registry_id).second) reject("overlapping actual phase ownership: " + id);
    }
    return result;
}
void removable(const Phases& phase, const std::string& id) {
    if (phase.scope.inactive_owner_ids.contains(id)) reject("owner is inactive in saved design: " + id);
    const auto owner = phase.owners.find(id); if (owner == phase.owners.end()) return;
    const auto& model = phase.models.at(owner->second);
    if (contains(model.baseline_ids(), id)) {
        if (model.active_alternative()) reject("shared baseline owner requires typed demolition in active alternative: " + id);
        if (!model.alternatives().empty()) reject("baseline owner is protected by retained alternatives: " + id);
        return;
    }
    if (!model.active_alternative()) reject("owner does not participate in saved baseline: " + id);
    const auto state = model.active_state();
    const auto row = state.find(id);
    if (row == state.end() || row->second != ModelPhase::proposed) reject("owner is not active proposed: " + id);
    std::size_t proposals{};
    for (const auto& alternative : model.alternatives()) {
        if (contains(alternative.demolished_ids, id)) reject("owner has protected demolition membership: " + id);
        if (!contains(alternative.proposed_ids, id)) continue;
        ++proposals;
        if (alternative.id != *model.active_alternative()) reject("owner is shared with another alternative: " + id);
    }
    if (proposals != 1) reject("owner lacks sole actual active proposal membership: " + id);
}
void retire_phases(Entities& result, const Entities& source, const Phases& phase, const Ids& retired) {
    for (const auto& [registry, model] : phase.models) {
        Ids members;
        for (const auto& id : retired) if (phase.owners.contains(id) && phase.owners.at(id) == registry) members.insert(id);
        if (members.empty()) continue;
        auto all = model.entity_ids(), baseline = model.baseline_ids(); auto alternatives = model.alternatives();
        std::erase_if(all, [&](const auto& id) { return members.contains(id); });
        if (!model.active_alternative()) std::erase_if(baseline, [&](const auto& id) { return members.contains(id); });
        else for (auto& row : alternatives) if (row.id == *model.active_alternative())
            std::erase_if(row.proposed_ids, [&](const auto& id) { return members.contains(id); });
        const auto expected = ModelPhases::create(all, baseline, alternatives, model.active_alternative()).to_json();
        auto raw = source.at(registry).properties.at("model"); remove_ids(raw.at("entity_ids"), members);
        if (!model.active_alternative()) remove_ids(raw.at("baseline_ids"), members);
        else for (auto& row : raw.at("alternatives")) if (row.at("id") == *model.active_alternative())
            remove_ids(row.at("proposed_ids"), members);
        if (ModelPhases::from_json(raw).to_json() != expected) reject("raw registry retirement differs from typed preservation: " + registry);
        result.at(registry).properties.at("model") = std::move(raw);
    }
}
bool supported_catalog(const Json& model) {
    const auto schema = field(model, "schema"); if (!schema || !schema->is_string()) return false;
    for (int v = 1; v <= 7; ++v) if (*schema == "sketch.assemblies.v" + std::to_string(v)) return true;
    return false;
}
bool sole_active_proposal(const ModelPhases& model, const std::string& id) {
    if (!model.active_alternative() || contains(model.baseline_ids(), id)) return false;
    std::size_t proposals{};
    for (const auto& alternative : model.alternatives()) {
        if (contains(alternative.demolished_ids, id)) return false;
        if (!contains(alternative.proposed_ids, id)) continue;
        if (alternative.id != *model.active_alternative()) return false;
        ++proposals;
    }
    return proposals == 1;
}
bool unprotected_baseline(const ModelPhases& model, const std::string& id) {
    if (!contains(model.baseline_ids(), id)) return false;
    for (const auto& alternative : model.alternatives())
        if (contains(alternative.proposed_ids, id) || contains(alternative.demolished_ids, id)) return false;
    return true;
}
// Same actual-source rule for production admission and the coordinator's
// analytical protected-row guard. No geometry factory or inferred choice.
std::optional<std::string> complete_proposed_opening_wall(const Entities& source,
    const Phases& phase, const Entity& catalog, const std::string& opening_id,
    bool require_baseline_carrier = true) {
    const auto carrier = phase.owners.find(catalog.id), opening_member = phase.owners.find(opening_id);
    const auto opening = source.find(opening_id);
    if (catalog.type != "assembly_model" || catalog.required || (require_baseline_carrier && carrier == phase.owners.end()) ||
        opening == source.end() || opening->second.type != "opening" || opening->second.required ||
        opening_member == phase.owners.end() || (carrier != phase.owners.end() && opening_member->second != carrier->second) ||
        phase.scope.inactive_owner_ids.contains(catalog.id) || phase.scope.inactive_owner_ids.contains(opening_id) ||
        !supported_catalog(catalog.properties.at("model"))) return std::nullopt;
    const auto& model = phase.models.at(opening_member->second);
    if (!model.active_alternative() ||
        (require_baseline_carrier && !unprotected_baseline(model, catalog.id)) ||
        (carrier != phase.owners.end() && !unprotected_baseline(model, catalog.id) && !sole_active_proposal(model, catalog.id)) ||
        !sole_active_proposal(model, opening_id)) return std::nullopt;
    std::string wall_id, error;
    if (!read_document_wall_id(opening->second, wall_id, error)) return std::nullopt;
    const auto wall = source.find(wall_id);
    const auto wall_member = phase.owners.find(wall_id);
    if (wall == source.end() || wall->second.type != "wall" || wall->second.required ||
        wall_member == phase.owners.end() || wall_member->second != opening_member->second ||
        phase.scope.inactive_owner_ids.contains(wall_id) ||
        (!unprotected_baseline(model, wall_id) && !sole_active_proposal(model, wall_id))) return std::nullopt;
    return wall_id;
}
Json retained_phase_membership(Json raw, const Ids& retained) {
    const auto keep = [&](Json& values) {
        filter(values, [&](const Json& value) { return retained.contains(value.get<std::string>()); });
    };
    keep(raw.at("entity_ids")); keep(raw.at("baseline_ids"));
    for (auto& alternative : raw.at("alternatives")) {
        keep(alternative.at("proposed_ids")); keep(alternative.at("demolished_ids"));
    }
    return raw;
}
void context(const Entities& source, const ProjectOrganization& organization, const Entity& entity) {
    if (entity.type == "assembly_model") {
        for (const auto& [key, type] :
            {std::pair{"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"},
                {"layer_id", "layer"}, {"wall_id", "wall"}}) {
            const auto value = field(entity.properties, key); if (!value) continue;
            if (!value->is_string()) reject("catalog context is malformed: " + entity.id);
            const auto found = source.find(value->get<std::string>());
            if (found == source.end() || found->second.type != type) reject("catalog context is unresolved: " + entity.id);
        }
        return;
    }
    const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
        entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
        entity.properties.contains("level_id") || entity.properties.contains("wall_id");
    const auto node = organization.nodes.find(entity.id);
    if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
        reject("owner has unresolved actual drawing context: " + entity.id);
}
void material(const Entities& source, const std::string& catalog_id, const std::string& material_id) {
    identity(catalog_id); const auto found = source.find(catalog_id);
    if (found == source.end() || found->second.type != "assembly_model") reject("material requires actual catalog: " + catalog_id);
    const auto& raw = found->second.properties.at("model");
    if (!supported_catalog(raw)) reject("material catalog has unsupported schema: " + catalog_id);
    const auto model = AssemblyModel::from_json(raw);
    if (std::none_of(model.materials().begin(), model.materials().end(), [&](const auto& row) { return row.id == material_id; }))
        reject("material is absent from actual catalog: " + material_id);
}
void material_assignment(const Entities& source, const Entity& entity) {
    const auto value = field(entity.properties, "material_assignment"); if (!value) return;
    const auto version = field(*value, "version"), catalog = field(*value, "catalog_id"), id = field(*value, "material_id");
    if (!version || !version->is_number_integer() || *version != 1 || !catalog || !catalog->is_string() || !id || !id->is_string())
        reject("owner has unsupported material assignment: " + entity.id);
    material(source, catalog->get<std::string>(), id->get<std::string>());
}
// Known child declarations do not acquire global entity-reference meaning when
// their local spelling happens to equal a removed physical/render identity.
Entity declaration_remainder(Entity entity, bool validate = true) {
    auto& p = entity.properties;
    if (entity.type == "stair") {
        const auto form = field(p, "form"), version = field(p, "version");
        if (form && version && version->is_number_integer() && *form == "multi_flight_stair" &&
            (*version == 2 || *version == 3 || *version == 4)) {
            if (validate) (void)decode_stair_properties(entity.id, p);
            for (const auto* key : {"flights", "landings"}) for (auto& row : p.at(key)) row.erase("id");
        }
    } else if (entity.type == "slab") {
        // This is the analytical slab reader; it constructs no native body.
        Slab slab; std::string error;
        if (read_document_slab(entity, slab, error) && p.contains("layers"))
            for (auto& row : p.at("layers")) row.erase("id");
    } else if (entity.type == kSheetViewEntityType) {
        if (validate) (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) row.erase("id");
        }
    } else if (entity.type == "assembly_model" && p.contains("model") && supported_catalog(p.at("model"))) {
        // Definition and row identities have catalog-local meaning too.
        if (validate) (void)AssemblyModel::from_json(p.at("model"));
        auto& model = p.at("model");
        for (auto& row : model.at("materials")) row.erase("id");
        for (auto& row : model.at("types")) {
            row.erase("id");
            if (row.contains("profiles")) for (auto& child : row.at("profiles")) child.erase("id");
            if (row.contains("parts")) for (auto& child : row.at("parts")) child.erase("id");
        }
        for (auto& row : model.at("instances")) row.erase("id");
    }
    return entity;
}
bool bound_overlay(const Json& row, const Ids& retired) {
    const auto object = field(row, "object_id"), binding = field(row, "dimension_binding");
    const auto bound = binding ? field(*binding, "object_id") : nullptr;
    return (object && object->is_string() && retired.contains(object->get<std::string>())) ||
        (bound && bound->is_string() && retired.contains(bound->get<std::string>()));
}
// This inspection copy removes only codec-owned reference slots. It never
// supplies deletion authority or replaces an authoritative raw envelope.
Entity reference_remainder(Entity entity) {
    auto& p = entity.properties;
    if (entity.type == "assembly_model") {
        if (!supported_catalog(p.at("model"))) reject("affected catalog has unsupported schema: " + entity.id);
        (void)AssemblyModel::from_json(p.at("model"));
        for (auto& row : p.at("model").at("instances"))
            if (row.contains("placement") && !row.at("placement").is_null()) row.at("placement").erase("host_entity_id");
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model")); auto& model = p.at("model");
        model.erase("entity_ids"); model.erase("baseline_ids");
        for (auto& row : model.at("alternatives")) { row.erase("proposed_ids"); row.erase("demolished_ids"); }
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : p.at("model").at("views")) {
            view.erase("object_ids"); auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) row.at("dimension_binding").erase("object_id");
            }
        }
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        for (auto& row : p.at("state").at("overrides")) if (row.at("target_kind") == "object") row.erase("target_id");
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) reject("affected associative dimension has unsupported schema: " + entity.id);
        p.at("target").erase("entity_id");
    }
    return declaration_remainder(std::move(entity), false);
}
Entity catalog_local_remainder(Entity entity) {
    auto& model = entity.properties.at("model");
    for (auto& row : model.at("materials")) row.erase("id");
    for (auto& row : model.at("types")) {
        row.erase("id"); row.erase("materials");
        if (row.contains("profiles")) for (auto& child : row.at("profiles")) { child.erase("id"); child.erase("material_slot"); }
        if (row.contains("parts")) for (auto& child : row.at("parts")) {
            child.erase("id"); child.erase("type_id"); child.erase("material_overrides");
        }
    }
    for (auto& row : model.at("instances")) {
        row.erase("id"); row.erase("type_id"); row.erase("material_overrides");
        if (row.contains("nested_overrides")) for (auto& child : row.at("nested_overrides")) {
            child.erase("part_path"); child.erase("material_overrides");
        }
    }
    return entity;
}

// Local material/type references cannot name a document owner merely because
// their spelling matches it. Mask only validated codec-owned local slots in
// this inspection copy; authoritative catalog and assignment bytes stay raw.
Entity global_reference_remainder(Entity entity,bool completed) {
    try {
        wall_scale_quantity_reference_remainder(entity);
    } catch (const std::invalid_argument& error) {
        reject(error.what());
    }
    // Validate on the original envelope before declaration/reference scratch
    // filtering removes fields required by the analytical codecs.
    std::vector<const char*> local_host_fields,local_dimension_fields;
    bool known_layers=false;
    if (entity.type=="railing") {
        const auto rail=decode_railing_properties(entity.id,entity.properties);
        if (rail.host) local_host_fields={"flight_id"};
        else if (rail.landing_host) {
            local_host_fields={"incoming_flight_id"};
            if (rail.landing_host->role==StairLandingRole::connecting)
                local_host_fields.insert(local_host_fields.end(),{"landing_id","outgoing_flight_id"});
        }
    } else if (entity.type=="wall" && entity.properties.contains("layers")) {
        Wall wall; std::string error;
        known_layers=read_document_wall(entity,{},wall,error);
    } else if (entity.type=="slab" && entity.properties.contains("layers")) {
        Slab slab; std::string error;
        known_layers=read_document_slab(entity,slab,error);
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded=decode_boundary_dimension_entity(entity);
        if (decoded.supported()) {
            if (decoded.dimension->kind==BoundaryDimensionKind::segment_length)
                local_dimension_fields=decoded.dimension->segment_chain_ids.empty()
                    ? std::vector<const char*>{"segment_id"} : std::vector<const char*>{"segment_ids"};
            else if (decoded.dimension->kind==BoundaryDimensionKind::angle)
                local_dimension_fields={"segment_id","second_segment_id","vertex_id"};
        }
    }
    auto scratch=completed ? declaration_remainder(std::move(entity)) : reference_remainder(std::move(entity));
    if (scratch.type=="assembly_model" && scratch.properties.contains("model") &&
        supported_catalog(scratch.properties.at("model"))) scratch=catalog_local_remainder(std::move(scratch));
    if (const auto assignment=field(scratch.properties,"material_assignment");assignment && assignment->is_object()) {
        const auto version=field(*assignment,"version"),catalog=field(*assignment,"catalog_id"),local=field(*assignment,"material_id");
        if (version && version->is_number_integer() && *version==1 && catalog && catalog->is_string() && local && local->is_string())
            scratch.properties.at("material_assignment").erase("material_id");
    }
    for (const auto* key:local_host_fields) scratch.properties.at("host").erase(key);
    for (const auto* key:local_dimension_fields) scratch.properties.at("target").erase(key);
    if (known_layers) for (auto& row:scratch.properties.at("layers")) {
        row.erase("id");
        if (row.contains("material") && !row.at("material").is_null()) row.at("material").erase("material_id");
    }
    return scratch;
}

// Bound analytical physical work before any native building codec/builder.
std::map<std::string, std::size_t, std::less<>> physical_bounds(const Entities& source, const Ids& owners,
    bool complete_native = false) {
    std::size_t work{};
    const auto limit = complete_native ? opening_native_limit : geometry_limit;
    std::map<std::string, std::size_t, std::less<>> costs;
    std::map<std::string, std::size_t, std::less<>> wall_references;
    if (complete_native) for (const auto& [id, entity] : source) {
        (void)id; const auto owner = field(entity.properties, "wall_id");
        if (owner && owner->is_string()) ++wall_references[owner->get<std::string>()];
    }
    const auto add = [&](std::size_t count) {
        if (count > limit - work) reject("aggregate physical geometry budget exceeded");
        work += count;
    };
    for (const auto& id : owners) {
        const auto before = work;
        const auto& e = source.at(id); const auto& p = e.properties;
        if (e.type == "stair") {
            const auto stair = decode_stair_properties(id, p); add(stair.riser_count + stair.landings.size() + 1);
        } else if (e.type == "railing") {
            const auto rail = decode_railing_properties(id, p);
            if (const auto owner = host(e)) {
                const auto stair = decode_stair_properties(*owner, resolve_vertical_placement(source, source.at(*owner)).properties);
                add(derive_hosted_railing_layout(rail, stair).posts.size() + 1);
            } else {
                const double posts = std::ceil(rail.length / rail.post_spacing) + 2;
                if (!std::isfinite(posts) || posts > geometry_limit) reject("railing post work budget exceeded: " + id);
                add(static_cast<std::size_t>(posts));
            }
        } else if (e.type == "slab") {
            const auto boundary = field(p, "boundary"), holes = field(p, "holes");
            if (!boundary || !boundary->is_array() || boundary->empty() || !holes || !holes->is_array() || holes->size() > 1024)
                reject("slab lacks bounded supported footprint: " + id);
            std::size_t segments{};
            const auto ring = [&](const Json& rows) {
                if (!rows.is_array() || rows.empty() || rows.size() > 4096 - segments) reject("slab footprint budget exceeded: " + id);
                segments += rows.size();
            };
            ring(*boundary); for (const auto& hole : *holes) ring(hole);
            const auto layers = field(p, "layers");
            if (layers && (!layers->is_array() || layers->size() > 1024))
                reject("slab layer inventory is unbounded: " + id);
            add(segments * std::max<std::size_t>(1, layers ? layers->size() : 0));
        } else if (e.type == "wall") {
            std::size_t count = 1, layer_count = 1, cuts = 1;
            if (const auto layers = field(p, "layers")) {
                if (!layers->is_array() || layers->size() > 1024) reject("wall host layer inventory is unbounded: " + id);
                count += layers->size();
                layer_count = std::max<std::size_t>(1, layers->size());
            }
            if (complete_native) cuts += wall_references[id];
            else for (const auto& [opening_id, opening] : source) {
                (void)opening_id; const auto owner = field(opening.properties, "wall_id");
                if (owner && owner->is_string() && *owner == id) { ++count; ++cuts; }
            }
            if (complete_native && layer_count > limit / cuts) reject("wall host layered cut budget exceeded: " + id);
            add(complete_native ? layer_count * cuts : count);
        } else if (e.type == "roof") {
            const auto openings = field(p, "roof_openings");
            if (openings && (!openings->is_array() || openings->size() > 1024)) reject("roof host opening inventory is unbounded: " + id);
            add((openings ? openings->size() : 0) + 4);
        } else add(1);
        costs.emplace(id, work - before);
    }
    return costs;
}
Wall actual_wall(const Entities& source, const Entity& entity) {
    std::vector<const Entity*> openings;
    const auto scope = constraint_phase_scope(source);
    for (const auto& [id, opening] : source) {
        if (scope.inactive_owner_ids.contains(id)) continue;
        const auto owner = field(opening.properties, "wall_id");
        if (opening.type == "opening" && owner && owner->is_string() && *owner == entity.id) openings.push_back(&opening);
    }
    Wall wall; std::string error;
    if (!read_document_wall(resolve_vertical_placement(source, entity), openings, wall, error))
        reject("actual wall host codec refused " + entity.id + ": " + error);
    return wall;
}
// Analytical admission only. It retains all actual active sibling cuts and
// the resolved wall frame used by make_document_opening_host_shape.
std::size_t opening_host_work(const Entities& source, const ConstraintPhaseScope& scope,
    const std::string& id, bool require_manufactured, OpeningAdmission& admission) {
    const auto& opening = source.at(id);
    validate_hosted_opening_profile_entity(opening);
    std::string wall_id, error;
    if (!read_document_wall_id(opening, wall_id, error)) reject(error);
    const auto wall = source.find(wall_id);
    if (wall == source.end() || wall->second.type != "wall" ||
        scope.inactive_owner_ids.contains(id) || scope.inactive_owner_ids.contains(wall_id))
        reject("opening component requires its actual active wall: " + id);
    if (!admission.indexed) {
        for (const auto& [sibling_id, sibling] : source) {
            if (sibling.type != "opening" || scope.inactive_owner_ids.contains(sibling_id)) continue;
            const auto owner = field(sibling.properties, "wall_id");
            if (owner && owner->is_string()) admission.siblings[owner->get<std::string>()].push_back(&sibling);
        }
        admission.indexed = true;
    }
    auto cached = admission.walls.find(wall_id);
    if (cached == admission.walls.end()) {
        const auto layers = field(wall->second.properties, "layers");
        if (layers && (!layers->is_array() || layers->size() > 1024)) reject("opening wall layer budget exceeded: " + wall_id);
        const auto& siblings = admission.siblings[wall_id];
        if (siblings.size() > 512) reject("opening component sibling cut budget exceeded: " + id);
        for (const auto* sibling : siblings) validate_hosted_opening_profile_entity(*sibling);
        Wall actual;
        if (!read_document_wall(resolve_vertical_placement(source, wall->second), siblings, actual, error)) reject(error);
        validate_wall_semantics(actual);
        cached = admission.walls.emplace(wall_id, std::move(actual)).first;
    }
    const auto& actual = cached->second;
    if (std::none_of(actual.openings.begin(), actual.openings.end(), [&](const auto& cut) { return cut.id == id; }))
        reject("opening component is absent from its actual wall: " + id);
    const auto kind = field(opening.properties, "opening_kind");
    if (require_manufactured && !opening.properties.contains("opening_assembly") &&
        !(kind && kind->is_string() && parse_opening_assembly_kind(kind->get<std::string>())))
        reject("bare-cut opening has no manufactured body for a legacy host-copy component: " + id);
    const auto layer_count = std::max<std::size_t>(1, actual.layers.size());
    if (layer_count > opening_native_limit / (actual.openings.size() + 1) / 33)
        reject("opening component layered cut geometry budget exceeded: " + id);
    return layer_count * (actual.openings.size() + 1) * 33;
}
void opening_component_context(const Entities& source, const ProjectOrganization& organization, const Entity& opening) {
    std::string wall_id, error;
    if (!read_document_wall_id(opening, wall_id, error)) reject(error);
    const auto wall = source.find(wall_id);
    if (wall == source.end() || wall->second.type != "wall") reject("opening component wall is missing: " + opening.id);
    context(source, organization, wall->second);
    const auto scoped = [](const Entity& entity) {
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "level_id"})
            if (entity.properties.contains(key)) return true;
        return false;
    };
    // Legacy world-default placement needs no fabricated hierarchy enrollment.
    // Explicit context on either owner still uses the actual organization.
    if (scoped(opening) || scoped(wall->second)) context(source, organization, opening);
}
void admit_physical(const Entities& source, const Ids& owners) {
    const auto organization = organize_project(source);
    for (const auto& id : owners) {
        const auto& entity = source.at(id); context(source, organization, entity); material_assignment(source, entity);
        const auto resolved = host(entity) ? entity : resolve_vertical_placement(source, entity);
        if (entity.type == "wall") {
            const auto wall = actual_wall(source, entity);
            for (const auto& layer : wall.layers) if (layer.material) material(source, layer.material->catalog_id, layer.material->material_id);
            (void)make_wall(wall);
        } else if (entity.type == "slab") {
            validate_slab_profile_source_entity(entity); validate_slab_geometry_derivation(entity);
            if (entity.extensions.contains("slab_layer_stack_retirement"))
                validate_slab_layer_stack_retirement(entity.extensions.at("slab_layer_stack_retirement"));
            Slab slab; std::string error;
            if (!read_document_slab(resolved, slab, error)) reject("unsupported actual slab " + id + ": " + error);
            for (const auto& layer : slab.layers) if (layer.material) material(source, layer.material->catalog_id, layer.material->material_id);
            (void)make_slab(slab);
        } else {
            if (entity.type == "column" || entity.type == "beam") validate_structural_object_source_entity(entity);
            const auto object = decode_building_entity(resolved); (void)make_building_shape(object, source);
        }
    }
}
void assembly_integrity(const Entities& source, AssemblyExpansionBudget* admission = nullptr) {
    if (admission) (void)expand_document_assembly_instances(source, *admission);
    else validate_document_assembly_instances(source);
    for (const auto& [id, entity] : source) if (entity.type == "assembly_model") {
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& row : model.instances()) if (row.placement) {
            const auto owner = source.find(row.placement->host_entity_id);
            if (owner == source.end()) reject("hosted assembly row lacks actual owner: " + id + "/" + row.id);
        }
    }
}
struct OpeningNativeBudget {
    std::size_t used{};
    void add(std::size_t count) {
        if (count > opening_native_limit - used) reject("shared source/candidate/component native geometry budget exceeded");
        used += count;
    }
};
// The opt-in lane reserves source and candidate supported host inventories,
// not just removed rows. Analytical expansion is already bounded per map;
// both inventories and local factories share one conservative native allowance.
void charge_candidate_geometry(const Entities& source, const Phases& phase,
    const AssemblyExpansionBudget& assembly, OpeningNativeBudget& native, OpeningAdmission& admission) {
    native.add(assembly.consumed_nodes); native.add(assembly.consumed_profile_segments);
    Ids physical_owners;
    for (const auto& [id, entity] : source)
        if (!phase.scope.inactive_owner_ids.contains(id) && component_host(entity, false)) physical_owners.insert(id);
    const auto costs = physical_bounds(source, physical_owners, true);
    for (const auto& [id, cost] : costs) { (void)id; native.add(cost); }
    Ids legacy_opening_hosts;
    for (const auto& [id, entity] : source) {
        if (entity.type == "opening" && !phase.scope.inactive_owner_ids.contains(id)) {
            const auto wall = field(entity.properties, "wall_id");
            if (wall && wall->is_string() && phase.scope.inactive_owner_ids.contains(wall->get<std::string>())) continue;
            const auto kind = field(entity.properties, "opening_kind");
            if (entity.properties.contains("opening_assembly") ||
                (kind && kind->is_string() && parse_opening_assembly_kind(kind->get<std::string>())))
                native.add(opening_host_work(source, phase.scope, id, true, admission));
        }
        if (entity.type != "assembly_model" || phase.scope.inactive_owner_ids.contains(id)) continue;
        const auto model = AssemblyModel::from_json(entity.properties.at("model"));
        for (const auto& row : model.instances()) if (row.placement) {
            AssemblyExpansionBudget row_budget;
            if (!model.expand(row, row_budget).profiles.empty()) continue;
            const auto& host_id = row.placement->host_entity_id;
            const auto owner = source.find(host_id);
            if (owner == source.end()) reject("candidate legacy component lacks actual host: " + id + "/" + row.id);
            if (phase.scope.inactive_owner_ids.contains(host_id)) continue;
            if (owner->second.type == "opening") {
                const auto wall = field(owner->second.properties, "wall_id");
                if (wall && wall->is_string() && phase.scope.inactive_owner_ids.contains(wall->get<std::string>())) continue;
                if (legacy_opening_hosts.insert(host_id).second)
                    native.add(opening_host_work(source, phase.scope, host_id, true, admission));
            } else if (const auto cost = costs.find(host_id); cost != costs.end()) native.add(cost->second);
            native.add(1); // Native transform and result validation per copy.
        }
    }
}

Entities derive(const Entities& source, const std::vector<std::string>& selection,
    const std::vector<std::pair<std::string, std::string>>& components,
    bool allow_manufactured_opening_hosts, std::size_t reserved_native_work = 0, bool analytical_only = false,
    bool complete_hosted_catalog_consequences = false,
    bool complete_proposed_opening_catalog_consequences = false) {
    if (!allow_manufactured_opening_hosts && reserved_native_work)
        reject("external native reservation requires opening-host admission");
    bounds(source);
    if ((selection.empty() && components.empty()) || selection.size() > selection_limit || components.size() > closure_limit)
        reject("selection requires bounded actual physical roots or qualified components");
    const auto phase = phases(source); const auto organization = organize_project(source);
    Ids sole_active_proposals, protected_carriers;
    if (complete_hosted_catalog_consequences) for (const auto& [registry, model] : phase.models) {
        (void)registry;
        if (!model.active_alternative()) continue;
        Ids protected_proposals;
        for (const auto& alternative : model.alternatives()) {
            protected_carriers.insert(alternative.proposed_ids.begin(), alternative.proposed_ids.end());
            protected_carriers.insert(alternative.demolished_ids.begin(), alternative.demolished_ids.end());
            protected_proposals.insert(alternative.demolished_ids.begin(), alternative.demolished_ids.end());
            if (alternative.id == *model.active_alternative())
                sole_active_proposals.insert(alternative.proposed_ids.begin(), alternative.proposed_ids.end());
            else protected_proposals.insert(alternative.proposed_ids.begin(), alternative.proposed_ids.end());
        }
        for (const auto& id : model.baseline_ids()) sole_active_proposals.erase(id);
        for (const auto& id : protected_proposals) sole_active_proposals.erase(id);
    }
    Keys explicit_keys;
    for (const auto& key : components) {
        identity(key.first);
        if (key.second.empty() || key.second.size() > byte_limit) reject("component local identity must be bounded and nonempty");
        if (!explicit_keys.insert(key).second) reject("duplicate qualified selected component: " + key.first + "/" + key.second);
        const auto catalog = source.find(key.first);
        if (catalog == source.end() || catalog->second.type != "assembly_model") reject("selected component requires actual catalog: " + key.first);
    }
    Ids retired; std::optional<std::string> selected_registry;
    for (const auto& id : selection) {
        identity(id);
        if (!retired.insert(id).second) reject("duplicate selected owner: " + id);
        const auto owner = source.find(id);
        if (owner == source.end()) reject("selected actual owner is missing: " + id);
        if (!physical(owner->second)) reject("selected family requires a dedicated removal path: " + id);
        if (owner->second.required) reject("selected physical owner is required: " + id);
        removable(phase, id); context(source, organization, owner->second);
        const auto member = phase.owners.find(id);
        if (member != phase.owners.end()) {
            if (selected_registry && *selected_registry != member->second) reject("selected owners span foreign phase registries: " + id);
            selected_registry = member->second;
        }
    }
    // The host relation is decoded from actual rows. Unsupported hosted forms
    // retain no rewrite authority and will be refused by the reference scan.
    for (const auto& [id, entity] : source) if (const auto owner = host(entity);
        owner && retired.contains(*owner) && source.at(*owner).type == "stair") {
        if (entity.required) reject("attached physical owner is required: " + id);
        removable(phase, id); retired.insert(id);
        if (retired.size() > closure_limit) reject("attached railing closure budget exceeded");
    }
    validate_stair_attachment_state(source);
    AssemblyExpansionBudget source_assembly_admission, candidate_assembly_admission;
    assembly_integrity(source, allow_manufactured_opening_hosts ? &source_assembly_admission : nullptr);
    const auto aliases = embedded_assembly_presentation_ids(source);
    Entities result = source; retire_phases(result, source, phase, retired);
    Ids retained_baseline_catalogs, retained_opening_bodies;
    const auto retained_baseline_catalog_row = [&](const Entity& catalog, const std::string& host_id) {
        if (!complete_hosted_catalog_consequences) return false;
        const auto host = source.find(host_id);
        if (host != source.end() && host->second.type != "wall" && host->second.type != "opening")
            return false; // Other host families retain their existing admission.
        const auto carrier = phase.owners.find(catalog.id);
        if (carrier == phase.owners.end()) return false;
        const auto& model = phase.models.at(carrier->second);
        if (!model.active_alternative() || !contains(model.baseline_ids(), catalog.id)) return false;
        if (catalog.required || phase.scope.inactive_owner_ids.contains(catalog.id) ||
            protected_carriers.contains(catalog.id) || !supported_catalog(catalog.properties.at("model")))
            reject("complete wall consequence requires an active nonrequired unprotected supported catalog: " + catalog.id);
        if (host == source.end() || host->second.required || phase.scope.inactive_owner_ids.contains(host_id))
            reject("complete wall catalog consequence has an absent or protected actual host: " + host_id);
        auto wall_id = host_id;
        if (host->second.type == "opening") {
            if (!allow_manufactured_opening_hosts) reject("complete wall catalog consequence requires opening-host admission");
            std::string error;
            if (!read_document_wall_id(host->second, wall_id, error)) reject(error);
            const auto opening_member = phase.owners.find(host_id);
            if (opening_member != phase.owners.end() &&
                (opening_member->second != carrier->second || !sole_active_proposals.contains(host_id)))
                reject("complete wall catalog consequence has foreign or protected actual opening ownership: " + host_id);
        } else if (host->second.type != "wall")
            reject("complete retained catalog consequence requires an actual wall or semantic opening host: " + host_id);
        const auto wall = source.find(wall_id);
        const auto wall_member = phase.owners.find(wall_id);
        if (host->second.type == "opening" && complete_proposed_opening_catalog_consequences) {
            const auto admitted_wall = complete_proposed_opening_wall(source, phase, catalog, host_id);
            if (!admitted_wall || *admitted_wall != wall_id)
                reject("complete placed consequence requires an unprotected same-registry proposed opening and active wall: " + host_id);
            retained_opening_bodies.insert(host_id); retained_opening_bodies.insert(wall_id);
            return true;
        }
        if (wall == source.end() || wall->second.type != "wall" || wall->second.required ||
            phase.scope.inactive_owner_ids.contains(wall_id) || wall_member == phase.owners.end() ||
            wall_member->second != carrier->second || !sole_active_proposals.contains(wall_id))
            reject("complete wall catalog consequence requires same-registry sole active proposed wall ownership: " + wall_id);
        return true;
    };
    Keys instances; Ids names = retired, geometry_owners = retired, opening_hosts;
    OpeningAdmission source_opening_admission, candidate_opening_admission;
    for (const auto& [id, entity] : source) if (entity.type == "assembly_model") {
        const auto& raw = entity.properties.at("model");
        if (!supported_catalog(raw)) reject("actual catalog has unsupported schema: " + id);
        const auto model = AssemblyModel::from_json(raw); Ids local;
        for (const auto& row : model.instances()) if (explicit_keys.contains({id, row.id}) ||
            (row.placement && retired.contains(row.placement->host_entity_id))) {
            if (instances.size() >= closure_limit) reject("hosted instance retirement budget exceeded");
            if (phase.scope.inactive_owner_ids.contains(id)) reject("selected component catalog is inactive: " + id);
            context(source, organization, entity);
            const bool retained_carrier = row.placement && retained_baseline_catalog_row(entity, row.placement->host_entity_id);
            if (retained_carrier) retained_baseline_catalogs.insert(id);
            if (row.placement) {
                const auto& owner_id = row.placement->host_entity_id;
                const auto owner = source.find(owner_id);
                if (owner == source.end() || !component_host(owner->second, allow_manufactured_opening_hosts)) reject("selected hosted component lacks a supported actual physical host: " + id + "/" + row.id);
                removable(phase, owner_id);
                if (owner->second.type == "opening") {
                    if (!explicit_keys.contains({id, row.id})) reject("opening host admission requires a qualified selected component: " + id + "/" + row.id);
                    if (entity.required || owner->second.required) reject("opening component has a required catalog or opening owner: " + id + "/" + row.id);
                    if (!retained_carrier) removable(phase, id);
                    opening_component_context(source, organization, owner->second);
                    std::string wall_id, error;
                    if (!read_document_wall_id(owner->second, wall_id, error)) reject(error);
                    if (source.at(wall_id).required) reject("opening component has a required wall owner: " + wall_id);
                    // The carrier can also be ordinary or a sole active
                    // proposal. Its existing removable check above still
                    // applies; the unchanged existing wall is never retired.
                    const auto retained_wall = complete_proposed_opening_catalog_consequences ?
                        complete_proposed_opening_wall(source, phase, entity, owner_id, false) : std::nullopt;
                    if (retained_wall) {
                        if (*retained_wall != wall_id) reject("placed opening consequence has an inconsistent actual wall host");
                        retained_opening_bodies.insert(owner_id); retained_opening_bodies.insert(wall_id);
                    } else removable(phase, wall_id);
                    const auto wall_registry = phase.owners.find(wall_id), opening_registry = phase.owners.find(owner_id);
                    if (wall_registry != phase.owners.end() && selected_registry && wall_registry->second != *selected_registry)
                        reject("opening component wall spans foreign phase registry: " + id + "/" + row.id);
                    if (wall_registry != phase.owners.end() && opening_registry != phase.owners.end() && wall_registry->second != opening_registry->second)
                        reject("opening component and wall have foreign phase ownership: " + id + "/" + row.id);
                    if (wall_registry != phase.owners.end()) selected_registry = wall_registry->second;
                    if (opening_hosts.insert(owner_id).second) {
                        (void)opening_host_work(source, phase.scope, owner_id, false, source_opening_admission);
                        material_assignment(source, owner->second); material_assignment(source, source.at(wall_id));
                        for (const auto& layer : source_opening_admission.walls.at(wall_id).layers)
                            if (layer.material) material(source, layer.material->catalog_id, layer.material->material_id);
                    }
                } else { context(source, organization, owner->second); geometry_owners.insert(owner_id); }
                const auto member = phase.owners.find(owner_id);
                if (member != phase.owners.end()) {
                    if (selected_registry && *selected_registry != member->second) reject("selected hosted component spans foreign phase registry: " + id + "/" + row.id);
                    selected_registry = member->second;
                }
                const auto catalog_registry = phase.owners.find(id);
                if (catalog_registry != phase.owners.end() && selected_registry && catalog_registry->second != *selected_registry)
                    reject("hosted catalog carrier belongs to a foreign phase registry: " + id + "/" + row.id);
                if (catalog_registry != phase.owners.end()) selected_registry = catalog_registry->second;
            } else {
                removable(phase, id);
                const auto member = phase.owners.find(id);
                if (member != phase.owners.end()) {
                    if (selected_registry && *selected_registry != member->second) reject("unhosted component carrier spans foreign phase registry: " + id);
                    selected_registry = member->second;
                }
            }
            instances.emplace(id, row.id); local.insert(row.id); names.insert(aliases.at({id, row.id}));
        }
        if (local.empty()) continue;
        const auto scratch = catalog_local_remainder(reference_remainder(entity));
        if (touches(scratch.properties, local) || touches(scratch.extensions, local))
            reject("affected catalog has opaque retired local-instance reference: " + id);
        filter(result.at(id).properties.at("model").at("instances"), [&](const Json& row) { return !local.contains(row.at("id").get<std::string>()); });
        (void)AssemblyModel::from_json(result.at(id).properties.at("model"));
    }
    for (const auto& key : explicit_keys) if (!instances.contains(key)) reject("selected qualified actual component row is missing: " + key.first + "/" + key.second);
    const auto costs = physical_bounds(source, geometry_owners, allow_manufactured_opening_hosts);
    Ids dimension_ids;
    for (const auto& [id, entity] : source) if (can_recognize_boundary_dimension_entity_type(entity.type) &&
        (touches(entity.properties, names) || touches(entity.extensions, names))) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (!decoded.supported()) reject("affected associative dimension has unsupported schema: " + id);
        if (names.contains(decoded.dimension->boundary_id)) {
            if (entity.required) reject("associative dimension owner is required: " + id);
            dimension_ids.insert(id);
        }
    }
    names.insert(dimension_ids.begin(), dimension_ids.end());
    // Deleted overlay identities stay local to the actual saved view. They are
    // never appended to the global physical/presentation retirement namespace.
    OverlayChildren overlays;
    for (const auto& [id, original] : source) {
        if (retired.contains(id) || (!touches(original.properties, names) && !touches(original.extensions, names))) continue;
        const auto scratch = global_reference_remainder(original,false);
        if (touches(scratch.properties, names) || touches(scratch.extensions, names))
            reject("affected opaque/reference data lacks qualified retirement codec: " + id);
        auto& changed = result.at(id);
        if (original.type == kSheetViewEntityType) {
            for (auto& view : changed.properties.at("model").at("views")) {
                auto& ids = view.at("object_ids"); const bool restricted = !ids.empty() || view.value("restrict_to_objects", false);
                remove_ids(ids, names); if (restricted && ids.empty()) view["restrict_to_objects"] = true;
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    filter(presentation.at("appearance").at("objects"), [&](const Json& row) { return !names.contains(row.at("object_id").get<std::string>()); });
                if (view.contains("overlays")) filter(view.at("overlays"), [&](const Json& row) {
                    if (!bound_overlay(row, names)) return true;
                    overlays[{id, view.at("id").get<std::string>()}].insert(row.at("id").get<std::string>()); return false;
                });
            }
            validate_sheet_view_entity(changed);
        } else if (original.type == kAnnotationEntityType) {
            filter(changed.properties.at("state").at("overrides"), [&](const Json& row) {
                return row.at("target_kind") != "object" || !names.contains(row.at("target_id").get<std::string>());
            });
            validate_annotation_entity(changed);
        }
    }
    for (const auto& id : retired) result.erase(id);
    for (const auto& id : dimension_ids) result.erase(id);
    bounds(result);
    const auto remaining_aliases = embedded_assembly_presentation_ids(result);
    for (const auto& [key, alias] : aliases) if (!instances.contains(key)) {
        const auto remaining = remaining_aliases.find(key);
        if (remaining == remaining_aliases.end() || remaining->second != alias)
            reject("removal would change surviving computed presentation alias: " + key.first + "/" + key.second);
    }
    validate_completed_architectural_retirement_references(result,names);
    for (const auto& [id, entity] : result) {
        if (qualified_instance_reference(entity.properties, instances) || qualified_instance_reference(entity.extensions, instances))
            reject("retained owner/component reference requires qualified retirement codec: " + id);
        if (qualified_overlay_reference(entity.properties, id, overlays) || qualified_overlay_reference(entity.extensions, id, overlays))
            reject("retained qualified overlay reference has no retirement codec: " + id);
        const auto local_overlay = overlays.lower_bound({id, ""});
        if (entity.type == kSheetViewEntityType && local_overlay != overlays.end() && local_overlay->first.first == id) {
            for (const auto& view : entity.properties.at("model").at("views")) {
                const auto children = overlays.find({id, view.at("id").get<std::string>()});
                if (children == overlays.end()) continue;
                auto scratch = view; scratch.erase("id"); scratch.erase("object_ids");
                auto& presentation = scratch.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
                if (scratch.contains("overlays")) for (auto& row : scratch.at("overlays")) {
                    row.erase("id"); row.erase("object_id");
                    if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) row.at("dimension_binding").erase("object_id");
                }
                if (touches(scratch, children->second)) reject("saved view retains opaque reference to retired local overlay: " + id);
            }
        }
    }
    validate_stair_attachment_state(result);
    assembly_integrity(result, allow_manufactured_opening_hosts ? &candidate_assembly_admission : nullptr);
    const auto after = phases(result);
    if (after.scope.inactive_owner_ids != phase.scope.inactive_owner_ids)
        reject("removal changed inactive baseline/other-alternative ownership");
    for (const auto& id : retained_opening_bodies) {
        const auto retained = result.find(id);
        if (retained == result.end() || retained->second != source.at(id) ||
            retained->second.properties.dump() != source.at(id).properties.dump() ||
            retained->second.extensions.dump() != source.at(id).extensions.dump())
            reject("complete placed consequence changed its actual opening or wall body: " + id);
        const auto member = after.owners.find(id);
        if (member == after.owners.end() || member->second != phase.owners.at(id))
            reject("complete placed consequence changed its opening or wall registry: " + id);
    }
    for (const auto& id : retained_baseline_catalogs) {
        const auto retained = result.find(id);
        if (retained == result.end()) reject("complete wall consequence erased its retained catalog: " + id);
        auto expected = source.at(id);
        filter(expected.properties.at("model").at("instances"), [&](const Json& row) {
            return !instances.contains({id, row.at("id").get<std::string>()});
        });
        if (retained->second != expected || retained->second.properties.dump() != expected.properties.dump() ||
            retained->second.extensions.dump() != expected.extensions.dump())
            reject("complete wall consequence changed its raw retained catalog beyond admitted rows: " + id);
        const auto owner = after.owners.find(id);
        if (owner == after.owners.end() || owner->second != phase.owners.at(id))
            reject("complete wall consequence changed its retained catalog registry: " + id);
    }
    if (!retained_baseline_catalogs.empty() || !retained_opening_bodies.empty()) {
        auto retained_members = retained_baseline_catalogs;
        retained_members.insert(retained_opening_bodies.begin(), retained_opening_bodies.end());
        for (const auto& [registry, model] : phase.models) {
            (void)model;
            const auto retained = result.find(registry);
            if (retained == result.end() || retained->second.type != "model_phases" ||
                retained_phase_membership(source.at(registry).properties.at("model"), retained_members).dump() !=
                    retained_phase_membership(retained->second.properties.at("model"), retained_members).dump())
                reject("complete catalog consequence changed retained carrier/host phase membership: " + registry);
        }
    }
    // Complete bounded candidates and typed relationships precede native source
    // geometry admission. Removed owners remain available in the actual map.
    AssemblyExpansionBudget budget; std::vector<AssemblyExpansion> expansions;
    std::vector<std::pair<std::string, AssemblyTransform>> legacy;
    std::size_t native_work{};
    for (const auto& [id, cost] : costs) { (void)id; native_work += cost; }
    OpeningNativeBudget shared_native;
    if (allow_manufactured_opening_hosts) {
        shared_native.add(reserved_native_work);
        charge_candidate_geometry(source, phase, source_assembly_admission, shared_native, source_opening_admission);
        charge_candidate_geometry(result, after, candidate_assembly_admission, shared_native, candidate_opening_admission);
        shared_native.add(native_work);
    }
    Ids legacy_opening_hosts;
    for (const auto& [catalog, local] : instances) {
        const auto model = AssemblyModel::from_json(source.at(catalog).properties.at("model"));
        const auto row = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& value) { return value.id == local; });
        auto expansion = model.expand(*row, budget);
        if (!expansion.profiles.empty()) expansions.push_back(std::move(expansion));
        else if (row->placement) {
            const auto& placement = *row->placement;
            const auto& owner_id = placement.host_entity_id;
            if (allow_manufactured_opening_hosts) {
                if (source.at(owner_id).type == "opening") {
                    if (legacy_opening_hosts.insert(owner_id).second)
                        shared_native.add(opening_host_work(source, phase.scope, owner_id, true, source_opening_admission));
                } else shared_native.add(costs.at(owner_id));
                shared_native.add(1);
            } else {
                const auto cost = costs.at(owner_id);
                if (cost > geometry_limit - native_work) reject("aggregate host-derived component work budget exceeded");
                native_work += cost;
            }
            legacy.emplace_back(placement.host_entity_id, AssemblyTransform{
                {placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
                placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
        }
        // A typed metadata-only unhosted row has no native body to admit.
    }
    if (allow_manufactured_opening_hosts) {
        shared_native.add(budget.consumed_nodes); shared_native.add(budget.consumed_profile_segments);
    }
    if (analytical_only) return result;
    admit_physical(source, geometry_owners);
    for (const auto& expansion : expansions) (void)make_assembly_geometry(expansion);
    std::map<std::string, TopoDS_Shape, std::less<>> opening_shapes;
    std::size_t opening_factory_work{};
    for (const auto& [id, transform] : legacy) {
        const auto& owner = source.at(id);
        if (owner.type == "opening" && allow_manufactured_opening_hosts) {
            auto found = opening_shapes.find(id);
            if (found == opening_shapes.end())
                found = opening_shapes.emplace(id, make_document_opening_host_shape(source, id, &opening_factory_work)).first;
            (void)transform_assembly_shape(found->second, transform);
            continue;
        }
        const auto resolved = host(owner) ? owner : resolve_vertical_placement(source, owner);
        if (owner.type == "wall") (void)transform_assembly_shape(make_wall(actual_wall(source, owner)), transform);
        else if (owner.type == "slab") {
            Slab slab; std::string error;
            if (!read_document_slab(resolved, slab, error)) reject("host-derived slab codec refused: " + error);
            (void)transform_assembly_shape(make_slab(slab), transform);
        } else (void)transform_assembly_shape(make_building_shape(decode_building_entity(resolved), source), transform);
    }
    return result;
}
} // namespace

void validate_completed_architectural_retirement_references(
    const std::map<std::string, Entity, std::less<>>& candidate,
    const std::set<std::string, std::less<>>& retired_ids) {
    for (const auto& [id,entity]:candidate) {
        if (!touches(entity.properties,retired_ids) && !touches(entity.extensions,retired_ids)) continue;
        const auto scratch=global_reference_remainder(entity,true);
        if (touches(scratch.properties,retired_ids) || touches(scratch.extensions,retired_ids))
            reject("retained owner/component reference requires qualified retirement codec: "+id);
    }
}

std::map<std::string, Entity, std::less<>> replay_architectural_object_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<std::string>& selected_object_ids,
    const std::vector<std::pair<std::string, std::string>>& explicit_components,
    bool allow_manufactured_opening_hosts, std::size_t reserved_native_work, bool complete_hosted_catalog_consequences,
    bool complete_proposed_opening_catalog_consequences) {
    try { return derive(actual, selected_object_ids, explicit_components, allow_manufactured_opening_hosts,
        reserved_native_work, false, complete_hosted_catalog_consequences, complete_proposed_opening_catalog_consequences); }
    catch (const Json::exception& error) { reject(std::string("malformed actual source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        reject(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
void preflight_architectural_object_removal(const Entities& actual,
    const std::vector<std::string>& selected_object_ids,
    const std::vector<std::pair<std::string, std::string>>& explicit_components,
    std::size_t reserved_native_work, bool complete_hosted_catalog_consequences,
    bool complete_proposed_opening_catalog_consequences) {
    try { (void)derive(actual, selected_object_ids, explicit_components, true, reserved_native_work, true,
        complete_hosted_catalog_consequences, complete_proposed_opening_catalog_consequences); }
    catch (const Json::exception& error) { reject(std::string("malformed actual source: ") + error.what()); }
}
std::vector<std::pair<std::string, std::string>> complete_proposed_opening_catalog_consequence_keys(
    const Entities& actual, const Entities& candidate,
    const std::vector<std::pair<std::string, std::string>>& requested_components,
    const std::string& registry_id, const std::string& alternative_id) {
    try {
        bounds(actual); bounds(candidate);
        identity(registry_id); identity(alternative_id);
        if (requested_components.size() > closure_limit) return {};
        const auto phase = phases(actual), after = phases(candidate);
        if (!phase.models.contains(registry_id)) return {};
        const auto& model = phase.models.at(registry_id);
        if (!model.active_alternative() || *model.active_alternative() != alternative_id) return {};
        const auto organization = organize_project(actual);
        const auto remaining_aliases = embedded_assembly_presentation_ids(candidate);
        std::map<std::string, AssemblyModel, std::less<>> catalogs;
        std::map<std::string, bool, std::less<>> carrier_admission;
        std::map<std::string, bool, std::less<>> body_admission;
        OpeningAdmission admission;
        Keys eligible;
        Ids retained_members;
        const auto retained_body = [&](const std::string& id) {
            if (const auto cached = body_admission.find(id); cached != body_admission.end()) return cached->second;
            const auto retained = candidate.find(id), original = actual.find(id);
            const bool valid = retained != candidate.end() && original != actual.end() &&
                retained->second == original->second && retained->second.properties.dump() == original->second.properties.dump() &&
                retained->second.extensions.dump() == original->second.extensions.dump();
            body_admission.emplace(id, valid);
            return valid;
        };
        for (const auto& key : requested_components) {
            const auto& [catalog_id, instance_id] = key;
            identity(catalog_id); identity(instance_id);
            if (remaining_aliases.contains(key)) continue;
            const auto catalog = actual.find(catalog_id), retained_catalog = candidate.find(catalog_id);
            if (catalog == actual.end() || catalog->second.type != "assembly_model" ||
                retained_catalog == candidate.end() || retained_catalog->second.type != "assembly_model") continue;
            if (!carrier_admission.contains(catalog_id)) {
                context(actual, organization, catalog->second);
                const auto source_model = AssemblyModel::from_json(catalog->second.properties.at("model"));
                const auto retained_model = AssemblyModel::from_json(retained_catalog->second.properties.at("model"));
                Ids survivors;
                for (const auto& survivor : retained_model.instances()) survivors.insert(survivor.id);
                auto expected = catalog->second;
                filter(expected.properties.at("model").at("instances"), [&](const Json& row) {
                    return survivors.contains(row.at("id").get<std::string>());
                });
                const bool valid = retained_catalog->second == expected &&
                    retained_catalog->second.properties.dump() == expected.properties.dump() &&
                    retained_catalog->second.extensions.dump() == expected.extensions.dump();
                catalogs.emplace(catalog_id, source_model);
                carrier_admission.emplace(catalog_id, valid);
            }
            if (!carrier_admission.at(catalog_id)) continue;
            const auto carrier = phase.owners.find(catalog_id);
            const auto retained_carrier = after.owners.find(catalog_id);
            if ((carrier == phase.owners.end()) != (retained_carrier == after.owners.end()) ||
                (carrier != phase.owners.end() && (carrier->second != registry_id || retained_carrier->second != registry_id))) continue;
            const auto& source_model = catalogs.at(catalog_id);
            const auto row = std::find_if(source_model.instances().begin(), source_model.instances().end(),
                [&](const auto& value) { return value.id == instance_id; });
            if (row == source_model.instances().end() || !row->placement) continue;
            const auto& opening_id = row->placement->host_entity_id;
            const auto opening_member = phase.owners.find(opening_id);
            if (opening_member == phase.owners.end() || opening_member->second != registry_id) continue;
            const auto wall_id = complete_proposed_opening_wall(actual, phase, catalog->second, opening_id, false);
            if (!wall_id || !retained_body(opening_id) || !retained_body(*wall_id)) continue;
            bool same_members = true;
            for (const auto& id : {opening_id, *wall_id}) {
                const auto member = after.owners.find(id);
                same_members = same_members && member != after.owners.end() && member->second == registry_id;
            }
            if (!same_members) continue;
            opening_component_context(actual, organization, actual.at(opening_id));
            (void)opening_host_work(actual, phase.scope, opening_id, false, admission);
            if (carrier != phase.owners.end()) retained_members.insert(catalog_id);
            retained_members.insert(opening_id); retained_members.insert(*wall_id);
            eligible.insert(key);
        }
        if (eligible.empty()) return {};
        const auto registry = candidate.find(registry_id);
        if (registry == candidate.end() || registry->second.type != "model_phases" ||
            retained_phase_membership(actual.at(registry_id).properties.at("model"), retained_members).dump() !=
                retained_phase_membership(registry->second.properties.at("model"), retained_members).dump()) return {};
        return {eligible.begin(), eligible.end()};
    } catch (const Json::exception&) {
        return {};
    } catch (const std::invalid_argument&) {
        return {};
    }
}
} // namespace sketch
