#include "sketch/hosted_opening_removal.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/architecture.hpp"
#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_wall_profile_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/stair_semantics.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using Keys = std::set<std::pair<std::string, std::string>>;
using Overlays = std::map<std::pair<std::string, std::string>, Ids>;
constexpr std::size_t entity_limit = 65536, selection_limit = 1000, closure_limit = 4096;
constexpr std::size_t node_limit = 4 * 1024 * 1024, byte_limit = 64 * 1024 * 1024;
constexpr std::size_t phase_limit = 2000000, native_limit = 262144;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Hosted opening removal: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) reject("invalid actual identity: " + id);
}
const Json* field(const Json& value, const char* name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
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
            if (value->is_number_float() && !std::isfinite(value->get<double>())) reject("nonfinite source scalar");
            if (value->is_string()) text(value->get_ref<const std::string&>());
            if (value->is_binary()) {
                if (value->get_binary().size() > byte_limit - bytes) reject("source binary budget exceeded");
                bytes += value->get_binary().size();
            }
            if (!value->is_structured()) continue;
            if (value->size() > node_limit - nodes || pending.size() > node_limit - nodes - value->size())
                reject("source pending-node budget exceeded");
            if (value->is_object()) for (const auto& [key, child] : value->items()) {
                text(key); pending.emplace_back(&child, depth + 1);
            } else for (const auto& child : *value) pending.emplace_back(&child, depth + 1);
        }
    }
};
// Bound the full source before recursive codecs, phase resolution, expansions
// or native work. No unrelated records are truncated to fit an allowance.
void bounds(const Entities& source) {
    if (source.size() > entity_limit) reject("source entity budget exceeded");
    Budget budget; std::size_t phases{}, rows{};
    for (const auto& [id, entity] : source) {
        identity(id);
        if (id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("inconsistent actual entity envelope: " + id);
        budget.text(id); budget.text(entity.type); budget.read(entity.properties); budget.read(entity.extensions);
        if (entity.type == "model_phases") {
            const auto model = field(entity.properties, "model");
            const auto members = model ? field(*model, "entity_ids") : nullptr;
            const auto alternatives = model ? field(*model, "alternatives") : nullptr;
            if (!members || !members->is_array() || members->size() > entity_limit ||
                !alternatives || !alternatives->is_array() || alternatives->size() > closure_limit)
                reject("phase registry lacks bounded actual inventory: " + id);
            if (members->size() > (phase_limit - phases) / (alternatives->size() + 1))
                reject("phase resolution work budget exceeded");
            phases += members->size() * (alternatives->size() + 1);
        } else if (entity.type == "assembly_model") {
            const auto model = field(entity.properties, "model");
            for (const auto* key : {"materials", "types", "instances"}) {
                const auto values = model ? field(*model, key) : nullptr;
                if (!values || !values->is_array() || values->size() > entity_limit - rows)
                    reject("catalog lacks bounded actual inventory: " + id);
                rows += values->size();
            }
        } else if (entity.type == "wall") {
            const auto layers = field(entity.properties, "layers");
            if (layers && (!layers->is_array() || layers->size() > 1024)) reject("wall layer budget exceeded: " + id);
        }
    }
}
struct NativeBudget {
    std::size_t used{};
    void add(std::size_t count) {
        if (count > native_limit - used) reject("aggregate wall/opening/component native work budget exceeded");
        used += count;
    }
    static std::size_t product(std::size_t first, std::size_t second) {
        if (first && second > native_limit / first)
            reject("wall/opening/component native work product exceeds budget");
        return first * second;
    }
};
struct WallWork {
    std::size_t build{}, manufactured{};
};
struct OpeningWork {
    std::size_t cuts{}, manufactured{};
};
using WallWorks = std::map<std::string, WallWork, std::less<>>;
WallWork wall_work(const Entities& source, const ConstraintPhaseScope& scope, const std::string& id,
    const std::map<std::string, OpeningWork, std::less<>>& openings) {
    const auto wall = source.find(id);
    if (wall == source.end() || wall->second.type != "wall" || scope.inactive_owner_ids.contains(id))
        reject("native dependency requires an actual active wall: " + id);
    const auto layers = field(wall->second.properties, "layers");
    if (layers && (!layers->is_array() || layers->size() > 1024)) reject("affected wall layer budget exceeded: " + id);
    const auto found = openings.find(id);
    const auto work = found == openings.end() ? OpeningWork{} : found->second;
    // make_wall constructs each layer and cuts every active sibling from each
    // one. A wall without authored layers still builds one complete layer.
    return {NativeBudget::product(std::max<std::size_t>(1, layers ? layers->size() : 0), work.cuts + 1), work.manufactured};
}
WallWorks charge_wall_dependencies(const Entities& source, const Ids& targets,
    const ConstraintPhaseScope& scope, std::size_t lanes, NativeBudget& budget) {
    std::vector<WallJoin> joins;
    std::map<std::string, OpeningWork, std::less<>> openings;
    for (const auto& [id, entity] : source) {
        if (entity.type == "opening" && !scope.inactive_owner_ids.contains(id)) {
            const auto host = field(entity.properties, "wall_id");
            if (host && host->is_string()) {
                auto& work = openings[host->get_ref<const std::string&>()]; ++work.cuts;
                const auto kind = field(entity.properties, "opening_kind");
                if (entity.properties.contains("opening_assembly") || (kind && kind->is_string() &&
                        parse_opening_assembly_kind(kind->get_ref<const std::string&>()))) ++work.manufactured;
            }
        }
        if (entity.type != "wall_join" || scope.inactive_owner_ids.contains(id)) continue;
        const auto members = field(entity.properties, "wall_ids");
        if (!members || !members->is_array() || members->size() > 128) reject("wall join member budget exceeded: " + id);
        auto join = parse_wall_join(entity.properties, id);
        if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(),
                [&](const auto& member) { return scope.inactive_owner_ids.contains(member); })) continue;
        joins.push_back(std::move(join));
    }
    Ids affected = targets;
    bool changed = true;
    std::size_t closure_work{};
    while (changed) {
        changed = false;
        for (const auto& join : joins) {
            if (++closure_work > phase_limit) reject("actual wall join dependency closure work budget exceeded");
            if (std::none_of(join.wall_ids.begin(), join.wall_ids.end(),
                    [&](const auto& member) { return affected.contains(member); })) continue;
            for (const auto& member : join.wall_ids) changed = affected.insert(member).second || changed;
        }
    }
    WallWorks walls;
    for (const auto& id : affected) {
        const auto work = wall_work(source, scope, id, openings); walls.emplace(id, work);
        // Strict target and joined-member admission can manufacture the same
        // sibling twice. Reserve 32 complete wall builds/part operations per
        // assembly, including double doors and moving/bay window families.
        const auto manufacture = NativeBudget::product(32, work.manufactured);
        const auto cohort = NativeBudget::product(work.build, manufacture + 1);
        budget.add(NativeBudget::product(cohort, NativeBudget::product(2, lanes)));
    }
    for (const auto& join : joins) {
        if (std::none_of(join.wall_ids.begin(), join.wall_ids.end(),
                [&](const auto& member) { return affected.contains(member); })) continue;
        // make_wall_join rebuilds every full member and performs pair contact
        // admission before fusion. Count each actual admitted relationship.
        budget.add(NativeBudget::product(NativeBudget::product(join.wall_ids.size(), join.wall_ids.size()), lanes));
        for (const auto& member : join.wall_ids) budget.add(NativeBudget::product(walls.at(member).build, lanes));
    }
    return walls;
}
bool touches(const Json& root, const Ids& names) {
    if (names.empty()) return false;
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
template<class Predicate> void filter(Json& rows, Predicate keep) {
    auto retained = Json::array();
    for (const auto& row : rows) if (keep(row)) retained.push_back(row);
    rows = std::move(retained);
}
void remove_ids(Json& rows, const Ids& retired) {
    filter(rows, [&](const Json& row) { return !retired.contains(row.get<std::string>()); });
}
bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}
bool exact(const Entity& before, const Entity& after) {
    return before == after && before.properties.dump() == after.properties.dump() &&
        before.extensions.dump() == after.extensions.dump();
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
            if (!result.owners.emplace(id, registry.registry_id).second) reject("overlapping phase ownership: " + id);
    }
    return result;
}
void removable(const Entities& source, const Phases& phase, const std::string& id) {
    const auto found = source.find(id);
    if (found == source.end() || found->second.required) reject("required or absent affected owner: " + id);
    if (phase.scope.inactive_owner_ids.contains(id)) reject("affected owner is inactive: " + id);
    const auto owner = phase.owners.find(id); if (owner == phase.owners.end()) return;
    const auto& model = phase.models.at(owner->second);
    if (contains(model.baseline_ids(), id)) {
        if (model.active_alternative()) reject("shared baseline opening/owner requires typed phase demolition: " + id);
        if (!model.alternatives().empty()) reject("baseline owner is protected by retained alternatives: " + id);
        return;
    }
    if (!model.active_alternative()) reject("affected owner is absent from saved baseline: " + id);
    const auto state = model.active_state();
    if (!state.contains(id) || state.at(id) != ModelPhase::proposed) reject("affected owner is not active proposed: " + id);
    std::size_t proposals{};
    for (const auto& alternative : model.alternatives()) {
        if (contains(alternative.demolished_ids, id)) reject("affected owner has protected demolition membership: " + id);
        if (!contains(alternative.proposed_ids, id)) continue;
        ++proposals;
        if (alternative.id != *model.active_alternative()) reject("affected owner belongs to another alternative: " + id);
    }
    if (proposals != 1) reject("affected owner lacks sole active proposal membership: " + id);
}
void context(const Entities& source, const ProjectOrganization& organization, const Entity& entity) {
    if (entity.type == "assembly_model") {
        for (const auto& [key, type] : {std::pair{"property_id", "property"}, {"building_id", "building"},
                {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}}) {
            const auto value = field(entity.properties, key); if (!value) continue;
            if (!value->is_string()) reject("malformed catalog drawing context: " + entity.id);
            const auto target = source.find(value->get<std::string>());
            if (target == source.end() || target->second.type != type) reject("unresolved catalog drawing context: " + entity.id);
        }
        return;
    }
    const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
        entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
        entity.properties.contains("level_id") || entity.properties.contains("wall_id");
    const auto node = organization.nodes.find(entity.id);
    if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
        reject("affected owner has unresolved drawing context: " + entity.id);
}
void retire_phases(Entities& candidate, const Entities& source, const Phases& phase, const Ids& retired) {
    for (const auto& [registry, model] : phase.models) {
        Ids members;
        for (const auto& id : retired) if (phase.owners.contains(id) && phase.owners.at(id) == registry) members.insert(id);
        if (members.empty()) continue;
        auto all = model.entity_ids(), baseline = model.baseline_ids(); auto alternatives = model.alternatives();
        std::erase_if(all, [&](const auto& id) { return members.contains(id); });
        std::erase_if(baseline, [&](const auto& id) { return members.contains(id); });
        for (auto& row : alternatives) if (model.active_alternative() && row.id == *model.active_alternative())
            std::erase_if(row.proposed_ids, [&](const auto& id) { return members.contains(id); });
        const auto expected = ModelPhases::create(all, baseline, alternatives, model.active_alternative()).to_json();
        auto raw = source.at(registry).properties.at("model"); remove_ids(raw.at("entity_ids"), members);
        remove_ids(raw.at("baseline_ids"), members);
        for (auto& row : raw.at("alternatives")) if (model.active_alternative() && row.at("id") == *model.active_alternative())
            remove_ids(row.at("proposed_ids"), members);
        if (ModelPhases::from_json(raw).to_json() != expected) reject("raw phase retirement differs from typed update: " + registry);
        candidate.at(registry).properties.at("model") = std::move(raw);
    }
}
bool supported_catalog(const Json& model) {
    const auto schema = field(model, "schema");
    if (!schema || !schema->is_string()) return false;
    for (int v = 1; v <= 7; ++v) if (*schema == "sketch.assemblies.v" + std::to_string(v)) return true;
    return false;
}
// A complete typed operation may retire an actual row on its selected proposed
// opening while retaining the shared catalog. This grants no carrier retirement
// or authority over baseline openings, unrelated rows, or another saved choice.
bool retained_baseline_catalog_row(const Entities& source, const Phases& phase,
    const Entity& catalog, const std::string& opening_id, bool complete_consequences) {
    if (!complete_consequences) return false;
    const auto catalog_owner = phase.owners.find(catalog.id);
    if (catalog_owner == phase.owners.end()) return false;
    const auto& model = phase.models.at(catalog_owner->second);
    if (!contains(model.baseline_ids(), catalog.id) || !model.active_alternative()) return false;
    if (catalog.required || phase.scope.inactive_owner_ids.contains(catalog.id) ||
        !supported_catalog(catalog.properties.at("model")))
        reject("complete opening consequence requires an active nonrequired supported catalog: " + catalog.id);
    const auto& opening = source.at(opening_id);
    // The selection already passed removable(), including sole active proposal
    // membership. An unregistered opening can inherit only a proposed wall.
    std::string wall_id, error;
    if (!read_document_wall_id(opening, wall_id, error)) reject(error);
    const auto& wall = source.at(wall_id);
    if (wall.required || phase.scope.inactive_owner_ids.contains(wall_id))
        reject("complete opening consequence requires an active nonrequired wall host: " + opening_id);
    const auto opening_owner = phase.owners.find(opening_id), wall_owner = phase.owners.find(wall_id);
    const auto authority = opening_owner != phase.owners.end() ? opening_owner : wall_owner;
    if (authority == phase.owners.end() || authority->second != catalog_owner->second ||
        (wall_owner != phase.owners.end() && wall_owner->second != catalog_owner->second))
        reject("complete opening/catalog consequence has foreign or absent saved authority: " + catalog.id + "/" + opening_id);
    const auto& proposed_id = opening_owner != phase.owners.end() ? opening_id : wall_id;
    const auto state = model.active_state();
    if (contains(model.baseline_ids(), proposed_id) || !state.contains(proposed_id) ||
        state.at(proposed_id) != ModelPhase::proposed)
        reject("complete opening/catalog consequence requires actual proposed ownership: " + opening_id);
    for (const auto& alternative : model.alternatives()) {
        // A retained baseline carrier may not be protected by any demolition
        // membership; affected hosts may not be shared with a different choice.
        if (contains(alternative.demolished_ids, catalog.id) ||
            (alternative.id != *model.active_alternative() &&
                (contains(alternative.proposed_ids, wall_id) || contains(alternative.demolished_ids, wall_id))))
            reject("complete opening/catalog consequence touches a protected carrier or wall: " + catalog.id + "/" + opening_id);
    }
    return true;
}
// Only known catalog-local declarations/references are masked in this scan
// copy. The authoritative catalog is patched solely by removing actual rows.
void mask_catalog_locals(Json& model) {
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
}
Ids output_view_ids(const Entities& source) {
    Ids ids;
    for (const auto& [owner, entity] : source) {
        (void)owner;
        if (entity.type != kSheetViewEntityType) continue;
        const auto model = decode_sheet_view_entity(entity);
        for (const auto& view : model.views()) ids.insert(view.id);
    }
    return ids;
}
// Call only after admitting the complete sheet/view graph. These declarations
// and references resolve within that graph, never the document owner map.
// Global schedule/object references and adjacent opaque bytes remain scanned.
void mask_sheet_locals(Json& model) {
    for (auto& view : model.at("views")) {
        view.erase("id");
        if (view.contains("overlays")) for (auto& row : view.at("overlays")) row.erase("id");
    }
    model.erase("sheet_order");
    for (auto& sheet : model.at("sheets")) {
        sheet.erase("id");
        if (sheet.contains("revisions")) for (auto& revision : sheet.at("revisions")) revision.erase("id");
        if (sheet.contains("viewports")) for (auto& viewport : sheet.at("viewports")) {
            viewport.erase("id"); viewport.erase("view_id");
        }
        if (sheet.contains("callouts")) for (auto& callout : sheet.at("callouts")) {
            callout.erase("id"); callout.erase("target_sheet_id"); callout.erase("target_viewport_id");
        }
        if (sheet.contains("schedules")) for (auto& schedule : sheet.at("schedules")) schedule.erase("id");
    }
}
bool document_presentation_target(std::string_view kind) {
    return kind == "object" || kind == "area" || kind == "wall_dimension" ||
        kind == "area_name" || kind == "area_calculation";
}
void mask_retained_output_views(Json& state, const Ids& retained_views) {
    if (!state.contains("overrides")) return;
    for (auto& row : state.at("overrides"))
        if (row.at("target_kind") == "output_view" && retained_views.contains(row.at("target_id").get<std::string>()))
            row.erase("target_id");
}
// Validate on complete envelopes first. Mask only declarations, never incoming
// document references. Unknown bytes that mention a retired owner still refuse.
Entity reference_remainder(Entity entity, const Ids& retained_views) {
    auto& p = entity.properties;
    if (entity.type == "assembly_model" && p.contains("model") && supported_catalog(p.at("model"))) {
        (void)AssemblyModel::from_json(p.at("model")); mask_catalog_locals(p.at("model"));
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(p.at("model"));
        p.at("model").erase("active_alternative");
        for (auto& row : p.at("model").at("alternatives")) row.erase("id");
    } else if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        mask_sheet_locals(p.at("model"));
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        mask_retained_output_views(p.at("state"), retained_views);
    } else if (entity.type == "wall" && p.contains("layers")) {
        Wall wall; std::string error;
        if (!read_document_wall(entity, {}, wall, error)) reject("retained wall local declarations are invalid: " + entity.id + ": " + error);
        for (auto& row : p.at("layers")) {
            row.erase("id");
            if (row.contains("material_assignment") && !row.at("material_assignment").is_null())
                row.at("material_assignment").erase("material_id");
        }
    } else if (entity.type == "slab" && p.contains("layers")) {
        Slab slab; std::string error;
        if (!read_document_slab(entity, slab, error)) reject("retained slab local declarations are invalid: " + entity.id + ": " + error);
        for (auto& row : p.at("layers")) {
            row.erase("id");
            if (row.contains("material_assignment") && !row.at("material_assignment").is_null())
                row.at("material_assignment").erase("material_id");
        }
    } else if (entity.type == "stair") {
        const auto form = field(p, "form"), version = field(p, "version");
        if (form && version && version->is_number_integer() && *form == "multi_flight_stair" &&
            (*version == 2 || *version == 3 || *version == 4)) {
            (void)decode_stair_properties(entity.id, p);
            for (const auto* key : {"flights", "landings"}) for (auto& row : p.at(key)) row.erase("id");
        }
    } else if (entity.type == "railing") {
        const auto rail = decode_railing_properties(entity.id, p);
        if (rail.host) p.at("host").erase("flight_id");
        else if (rail.landing_host) {
            p.at("host").erase("incoming_flight_id");
            if (rail.landing_host->role == StairLandingRole::connecting) {
                p.at("host").erase("landing_id"); p.at("host").erase("outgoing_flight_id");
            }
        }
    } else if (can_recognize_boundary_dimension_entity_type(entity.type)) {
        const auto decoded = decode_boundary_dimension_entity(entity);
        if (decoded.supported()) {
            for (const auto* key : {"segment_id", "segment_ids", "second_segment_id", "vertex_id"}) p.at("target").erase(key);
        }
    } else if (entity.type == "constraint") {
        const auto decoded = decode_constraint_entity(entity);
        if (decoded.supported()) for (auto& binding : p.at("bindings")) {
            binding.erase("segment_id"); binding.erase("vertex_id");
        }
    }
    if (const auto assignment = field(p, "material_assignment"); assignment && assignment->is_object()) {
        const auto version = field(*assignment, "version"), catalog = field(*assignment, "catalog_id"), local = field(*assignment, "material_id");
        if (version && version->is_number_integer() && *version == 1 && catalog && catalog->is_string() && local && local->is_string())
            p.at("material_assignment").erase("material_id");
    }
    return entity;
}
// Before retiring a whole presentation row, retain its opaque data in a scan
// copy and mask only admitted reference slots. Deleting a known binding must
// not silently delete an unsupported second reference embedded in that row.
Entity cleanup_remainder(Entity entity, const Keys& instances, const Ids& names, const Ids& retained_views) {
    if (entity.type == kSheetViewEntityType) {
        (void)decode_sheet_view_entity(entity);
        for (auto& view : entity.properties.at("model").at("views")) {
            remove_ids(view.at("object_ids"), names); auto& presentation = view.at("presentation");
            if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                for (auto& row : presentation.at("appearance").at("objects"))
                    if (names.contains(row.at("object_id").get<std::string>())) row.erase("object_id");
            if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
                if (row.contains("object_id") && names.contains(row.at("object_id").get<std::string>())) row.erase("object_id");
                if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null() &&
                    names.contains(row.at("dimension_binding").at("object_id").get<std::string>()))
                    row.at("dimension_binding").erase("object_id");
            }
        }
        mask_sheet_locals(entity.properties.at("model"));
        return entity;
    } else if (entity.type == kAnnotationEntityType) {
        validate_annotation_entity(entity);
        if (entity.properties.at("state").contains("overrides"))
            for (auto& row : entity.properties.at("state").at("overrides"))
                if (document_presentation_target(row.at("target_kind").get<std::string>()) &&
                    names.contains(row.at("target_id").get<std::string>())) row.erase("target_id");
        mask_retained_output_views(entity.properties.at("state"), retained_views);
        return entity;
    } else if (entity.type == "model_phases") {
        (void)ModelPhases::from_json(entity.properties.at("model"));
        auto& model = entity.properties.at("model"); model.erase("entity_ids"); model.erase("baseline_ids");
        for (auto& row : model.at("alternatives")) { row.erase("proposed_ids"); row.erase("demolished_ids"); }
        // reference_remainder expects the complete phase codec envelope.
        model.erase("active_alternative");
        for (auto& row : model.at("alternatives")) row.erase("id");
        return entity;
    } else if (entity.type == "assembly_model") {
        (void)AssemblyModel::from_json(entity.properties.at("model"));
        for (auto& row : entity.properties.at("model").at("instances"))
            if (instances.contains({entity.id, row.at("id").get<std::string>()}))
                row.at("placement").erase("host_entity_id");
        // The catalog has already been decoded on its full actual envelope.
        mask_catalog_locals(entity.properties.at("model"));
        return entity;
    }
    return reference_remainder(std::move(entity), retained_views);
}
bool bound_overlay(const Json& row, const Ids& names) {
    const auto object = field(row, "object_id"), binding = field(row, "dimension_binding");
    const auto bound = binding ? field(*binding, "object_id") : nullptr;
    return (object && object->is_string() && names.contains(object->get<std::string>())) ||
        (bound && bound->is_string() && names.contains(bound->get<std::string>()));
}
void retire_presentations(Entities& candidate, const Ids& names, Overlays& overlays) {
    for (auto& [id, entity] : candidate) {
        if (!touches(entity.properties, names) && !touches(entity.extensions, names)) continue;
        if (entity.type == kSheetViewEntityType) {
            (void)decode_sheet_view_entity(entity);
            for (auto& view : entity.properties.at("model").at("views")) {
                auto& ids = view.at("object_ids");
                const bool restricted = !ids.empty() || view.value("restrict_to_objects", false);
                const auto count = ids.size(); remove_ids(ids, names);
                if (count != ids.size() && restricted && ids.empty()) view["restrict_to_objects"] = true;
                auto& presentation = view.at("presentation");
                if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
                    filter(presentation.at("appearance").at("objects"), [&](const Json& row) {
                        return !names.contains(row.at("object_id").get<std::string>());
                    });
                if (view.contains("overlays")) filter(view.at("overlays"), [&](const Json& row) {
                    if (!bound_overlay(row, names)) return true;
                    overlays[{id, view.at("id").get<std::string>()}].insert(row.at("id").get<std::string>()); return false;
                });
            }
            validate_sheet_view_entity(entity);
        } else if (entity.type == kAnnotationEntityType) {
            validate_annotation_entity(entity);
            if (entity.properties.at("state").contains("overrides"))
                filter(entity.properties.at("state").at("overrides"), [&](const Json& row) {
                    return !document_presentation_target(row.at("target_kind").get<std::string>()) ||
                        !names.contains(row.at("target_id").get<std::string>());
                });
            validate_annotation_entity(entity);
        }
    }
}
// Qualified rows and local overlays have independent namespaces. A local ID
// must never accidentally acquire global retirement authority.
void refuse_qualified_references(const Entity& entity, const Keys& instances, const Ids& locals, const Overlays& overlays) {
    for (const auto* root : {&entity.properties, &entity.extensions}) {
        std::vector<const Json*> pending{root};
        while (!pending.empty()) {
            const auto* value = pending.back(); pending.pop_back();
            if (value->is_object()) {
                const auto instance = field(*value, "instance_id");
                bool qualified = false;
                for (const auto* key : {"catalog_id", "assembly_catalog_id"}) {
                    const auto catalog = field(*value, key);
                    qualified = qualified || (catalog && catalog->is_string());
                    if (catalog && catalog->is_string() && instance && instance->is_string() &&
                        instances.contains({catalog->get<std::string>(), instance->get<std::string>()}))
                        reject("retained qualified catalog/instance reference has no retirement codec: " + entity.id);
                }
                if (!qualified && instance && instance->is_string() && locals.contains(instance->get<std::string>()))
                    reject("unqualified retired instance_id reference has no catalog retirement codec: " + entity.id);
                const auto view = field(*value, "view_id"), overlay = field(*value, "overlay_id");
                if (view && view->is_string() && overlay && overlay->is_string()) {
                    std::string owner = entity.id;
                    for (const auto* key : {"sheet_view_entity_id", "sheet_view_id", "entity_id"}) {
                        const auto explicit_owner = field(*value, key);
                        if (explicit_owner && explicit_owner->is_string()) { owner = explicit_owner->get<std::string>(); break; }
                    }
                    const auto children = overlays.find({owner, view->get<std::string>()});
                    if (children != overlays.end() && children->second.contains(overlay->get<std::string>()))
                        reject("retained qualified sheet/view/overlay reference has no retirement codec: " + entity.id);
                }
                for (const auto& child : *value) pending.push_back(&child);
            } else if (value->is_array()) for (const auto& child : *value) pending.push_back(&child);
        }
    }
    if (entity.type == kSheetViewEntityType) for (auto view : entity.properties.at("model").at("views")) {
        const auto children = overlays.find({entity.id, view.at("id").get<std::string>()});
        if (children == overlays.end()) continue;
        view.erase("id"); view.erase("object_ids");
        auto& presentation = view.at("presentation");
        if (presentation.contains("appearance") && !presentation.at("appearance").is_null())
            for (auto& row : presentation.at("appearance").at("objects")) row.erase("object_id");
        if (view.contains("overlays")) for (auto& row : view.at("overlays")) {
            row.erase("id"); row.erase("object_id");
            if (row.contains("dimension_binding") && !row.at("dimension_binding").is_null()) row.at("dimension_binding").erase("object_id");
        }
        if (touches(view, children->second)) reject("retained saved-view metadata refers to retired local overlay: " + entity.id);
    }
}

bool has_selected_opening(const Entities& actual, const std::vector<std::string>& selection) {
    return !selection.empty() && std::any_of(selection.begin(), selection.end(), [&](const auto& id) {
            const auto found = actual.find(id); return found != actual.end() && found->second.type == "opening";
        });
}
struct OpeningRemoval {
    Entities entities;
    Ids hosts;
};
// Keep history-policy resolution at its original source/candidate admission
// points. Actual-map callers supply a fixed captured mode; the snapshot path
// reads the real history only after the preceding source preflight succeeds.
template<class ConstraintPolicy>
std::optional<OpeningRemoval> derive(const Entities& actual,
    const std::vector<std::string>& selection, ConstraintPolicy active_phase_constraints,
    bool complete_hosted_catalog_consequences = false) {
    if (!has_selected_opening(actual, selection)) return std::nullopt;
    if (selection.size() > selection_limit) reject("opening selection budget exceeded");
    bounds(actual);
    const auto source_output_views = output_view_ids(actual);
    const auto phase = phases(actual); const auto organization = organize_project(actual);
    Ids retired, hosts;
    for (const auto& id : selection) {
        identity(id);
        if (!retired.insert(id).second) reject("duplicate selected opening identity: " + id);
        const auto found = actual.find(id);
        if (found == actual.end() || found->second.type != "opening")
            reject("mixed selection must remove semantic openings separately; unsupported selected identity: " + id);
        removable(actual, phase, id); context(actual, organization, found->second);
        validate_hosted_opening_profile_entity(found->second);
        std::string host, error;
        if (!read_document_wall_id(found->second, host, error)) reject("selected opening host is invalid: " + id + ": " + error);
        const auto wall = actual.find(host);
        if (wall == actual.end() || wall->second.type != "wall" || phase.scope.inactive_owner_ids.contains(host))
            reject("selected opening requires an actual active wall host: " + id);
        context(actual, organization, wall->second);
        const auto opening_owner = phase.owners.find(id), host_owner = phase.owners.find(host);
        if (opening_owner != phase.owners.end() && host_owner != phase.owners.end() && opening_owner->second != host_owner->second)
            reject("opening and wall have foreign phase ownership: " + id);
        if (opening_owner == phase.owners.end() && host_owner != phase.owners.end()) {
            const auto& model = phase.models.at(host_owner->second);
            if (contains(model.baseline_ids(), host) && !model.alternatives().empty())
                reject("unregistered opening inherits shared baseline wall ownership; register it before typed demolition: " + id);
            if (!contains(model.baseline_ids(), host)) removable(actual, phase, host);
        }
        hosts.insert(host);
    }
    if (hosts.size() > 1000) reject("affected wall target budget exceeded");
    NativeBudget native_budget;
    const auto source_wall_work = charge_wall_dependencies(actual, hosts, phase.scope, 1, native_budget);
    // Admit source constraints before dependent deletion could hide malformed
    // bindings. The caller's captured validation policy remains authoritative.
    const auto unsupported = active_phase_constraints()
        ? validate_active_phase_constraint_integrity(actual) : validate_constraint_integrity(actual);
    if (unsupported) reject(*unsupported);

    AssemblyExpansionBudget document_budget;
    (void)expand_document_assembly_instances(actual, document_budget);
    const auto aliases = embedded_assembly_presentation_ids(actual);
    Entities candidate = actual; Keys instances; Ids names = retired, retired_locals, retained_baseline_catalogs;
    std::vector<AssemblyExpansion> removed_expansions;
    struct OpeningComponent {
        Wall wall;
        HostedOpening cut;
        OpeningAssembly assembly;
        std::optional<DoorOperation> operation;
        AssemblyTransform transform;
    };
    std::vector<OpeningComponent> manufactured_components;
    AssemblyExpansionBudget removed_budget;
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model")
        catalogs.emplace(id, AssemblyModel::from_json(entity.properties.at("model")));
    const auto material = [&](const Json& assignment, const std::string& owner) {
        const auto version = field(assignment, "version"), catalog = field(assignment, "catalog_id"), local = field(assignment, "material_id");
        if (!version || !version->is_number_integer() || *version != 1 || !catalog || !catalog->is_string() ||
            !local || !local->is_string()) reject("unsupported removed opening material assignment: " + owner);
        const auto model = catalogs.find(catalog->get<std::string>());
        if (model == catalogs.end() || std::none_of(model->second.materials().begin(), model->second.materials().end(),
                [&](const auto& row) { return row.id == local->get<std::string>(); }))
            reject("removed opening material lacks actual catalog definition: " + owner);
    };
    for (const auto& id : retired) if (actual.at(id).properties.contains("material_assignment"))
        material(actual.at(id).properties.at("material_assignment"), id);
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model") {
        const auto& raw = entity.properties.at("model");
        if (!supported_catalog(raw)) reject("actual catalog has unsupported schema: " + id);
        const auto& model = catalogs.at(id); Ids locals;
        for (const auto& row : model.instances()) if (row.placement && retired.contains(row.placement->host_entity_id)) {
            if (instances.size() >= closure_limit) reject("opening-hosted catalog closure budget exceeded");
            const bool retained_carrier = retained_baseline_catalog_row(actual, phase, entity,
                row.placement->host_entity_id, complete_hosted_catalog_consequences);
            if (retained_carrier) retained_baseline_catalogs.insert(id);
            else removable(actual, phase, id);
            context(actual, organization, entity);
            const auto catalog_owner = phase.owners.find(id), opening_owner = phase.owners.find(row.placement->host_entity_id);
            if (!retained_carrier && catalog_owner != phase.owners.end() &&
                (opening_owner == phase.owners.end() || catalog_owner->second != opening_owner->second))
                reject("opening-hosted catalog carrier has foreign phase ownership: " + id + "/" + row.id);
            auto expansion = model.expand(row, removed_budget);
            if (expansion.profiles.empty()) {
                const auto& opening = actual.at(row.placement->host_entity_id);
                const auto& p = opening.properties;
                std::optional<OpeningAssembly> assembly;
                if (p.contains("opening_assembly")) assembly = parse_opening_assembly(p.at("opening_assembly"));
                else if (p.contains("opening_kind")) {
                    const auto family = parse_opening_assembly_kind(p.at("opening_kind").get<std::string>());
                    if (family) assembly = default_opening_assembly(*family);
                }
                if (!assembly) reject("bare-cut opening-hosted legacy component has no native host solid codec: " + id + "/" + row.id);
                std::string wall_id, error;
                if (!read_document_wall_id(opening, wall_id, error)) reject(error);
                // Reserve this row's repeated full-wall manufacture before
                // retaining a complete source Wall for its later native call.
                native_budget.add(NativeBudget::product(source_wall_work.at(wall_id).build, 32));
                native_budget.add(1);
                std::vector<const Entity*> siblings;
                for (const auto& [sibling_id, sibling] : actual) {
                    if (sibling.type != "opening" || phase.scope.inactive_owner_ids.contains(sibling_id)) continue;
                    const auto host = field(sibling.properties, "wall_id");
                    if (host && host->is_string() && *host == wall_id) siblings.push_back(&sibling);
                }
                Wall wall;
                if (!read_document_wall(resolve_vertical_placement(actual, actual.at(wall_id)), siblings, wall, error)) reject(error);
                validate_wall_semantics(wall);
                const auto cut = std::find_if(wall.openings.begin(), wall.openings.end(),
                    [&](const auto& value) { return value.id == opening.id; });
                if (cut == wall.openings.end()) reject("actual opening is absent from its complete native host source: " + opening.id);
                std::optional<DoorOperation> operation;
                if (p.contains("door_operation")) operation = decode_door_operation(p.at("door_operation"));
                const auto& placement = *row.placement;
                manufactured_components.push_back({wall, *cut, *assembly, operation, {
                    {placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
                    placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale}});
            } else removed_expansions.push_back(std::move(expansion));
            instances.emplace(id, row.id); locals.insert(row.id); retired_locals.insert(row.id); names.insert(aliases.at({id, row.id}));
        }
        if (locals.empty()) continue;
        filter(candidate.at(id).properties.at("model").at("instances"), [&](const Json& row) {
            return !locals.contains(row.at("id").get<std::string>());
        });
        (void)AssemblyModel::from_json(candidate.at(id).properties.at("model"));
        const auto remainder = reference_remainder(candidate.at(id), source_output_views);
        if (touches(remainder.properties, locals) || touches(remainder.extensions, locals))
            reject("retained catalog metadata has unsupported retired local-instance reference: " + id);
    }
    // Known dependents form a bounded closure. Unsupported affected records
    // remain in the candidate and are refused by the final reference scan.
    bool changed = true;
    std::size_t closure_work{};
    while (changed) {
        changed = false;
        for (const auto& [id, entity] : actual) {
            if (++closure_work > phase_limit) reject("dimension/constraint closure scan work budget exceeded");
            if (retired.contains(id)) continue;
            bool attached = false;
            if (can_recognize_boundary_dimension_entity_type(entity.type) &&
                (touches(entity.properties, names) || touches(entity.extensions, names))) {
                const auto decoded = decode_boundary_dimension_entity(entity);
                if (!decoded.supported()) reject("affected dimension has unsupported target/schema: " + id);
                attached = names.contains(decoded.dimension->boundary_id);
            } else if (entity.type == "constraint" &&
                (touches(entity.properties, names) || touches(entity.extensions, names))) {
                const auto decoded = decode_constraint_entity(entity);
                if (!decoded.supported()) reject("affected constraint has unsupported bindings/schema: " + id);
                attached = std::any_of(decoded.constraint->bindings.begin(), decoded.constraint->bindings.end(),
                    [&](const auto& binding) { return names.contains(binding.owner_id); });
                if (attached && !constraint_participates(*decoded.constraint, phase.scope))
                    reject("affected constraint also belongs to retained inactive geometry: " + id);
            }
            if (!attached) continue;
            removable(actual, phase, id); context(actual, organization, entity);
            retired.insert(id); names.insert(id); changed = true;
            if (retired.size() > closure_limit) reject("opening dimension/constraint closure budget exceeded");
        }
    }
    // Supported cleanup fields do not exempt adjacent opaque references. Scan
    // the original survivors before whole rows disappear from presentations.
    const auto retained_output_views = output_view_ids(candidate);
    if (retained_output_views != source_output_views) reject("removal changes retained decoded output-view identities");
    for (const auto& [id, entity] : actual) {
        if (retired.contains(id)) continue;
        refuse_qualified_references(entity, instances, retired_locals, {});
        if (!touches(entity.properties, names) && !touches(entity.extensions, names)) continue;
        const auto remainder = cleanup_remainder(entity, instances, names, retained_output_views);
        if (touches(remainder.properties, names) || touches(remainder.extensions, names))
            reject("affected " + entity.type + " " + id + " has an unsupported reference outside known cleanup fields");
    }
    for (const auto& id : retired) candidate.erase(id);
    retire_phases(candidate, actual, phase, retired);
    Overlays overlays; retire_presentations(candidate, names, overlays);
    const auto final_output_views = output_view_ids(candidate);
    if (final_output_views != retained_output_views) reject("presentation retirement changes retained decoded output-view identities");
    for (const auto& [id, entity] : candidate) {
        if (touches(entity.properties, names) || touches(entity.extensions, names)) {
            const auto remainder = reference_remainder(entity, final_output_views);
            if (touches(remainder.properties, names) || touches(remainder.extensions, names))
                reject("retained " + entity.type + " " + id + " has an unsupported reference to a retired opening/dependent");
        }
        refuse_qualified_references(entity, instances, retired_locals, overlays);
    }
    bounds(candidate);
    const auto remaining_aliases = embedded_assembly_presentation_ids(candidate);
    for (const auto& [key, alias] : aliases) if (!instances.contains(key)) {
        const auto remaining = remaining_aliases.find(key);
        if (remaining == remaining_aliases.end() || remaining->second != alias)
            reject("removal changes surviving component presentation alias: " + key.first + "/" + key.second);
    }
    if (phases(candidate).scope.inactive_owner_ids != phase.scope.inactive_owner_ids)
        reject("removal changes retained inactive ownership");
    for (const auto& [id, entity] : actual) {
        const auto after = candidate.find(id);
        if (after != candidate.end() && exact(entity, after->second)) continue;
        if (retained_baseline_catalogs.contains(id)) {
            if (after == candidate.end()) reject("complete opening consequence erased its retained catalog: " + id);
            auto expected = entity;
            filter(expected.properties.at("model").at("instances"), [&](const Json& row) {
                return !instances.contains({id, row.at("id").get<std::string>()});
            });
            if (!exact(expected, after->second))
                reject("complete opening consequence changed its raw retained catalog beyond admitted rows: " + id);
            context(actual, organization, entity);
            continue;
        }
        removable(actual, phase, id); context(actual, organization, entity);
    }
    // Only after complete analytical preflight do native factories see the
    // actual full source. Removed descriptors can never escape this admission.
    // Candidate physical dependency admission and final architectural geometry
    // admission independently rebuild the affected full wall/join cohort.
    const auto candidate_scope = constraint_phase_scope(candidate);
    (void)charge_wall_dependencies(candidate, hosts, candidate_scope, 2, native_budget);
    native_budget.add(removed_budget.consumed_nodes);
    native_budget.add(removed_budget.consumed_profile_segments);
    validate_active_wall_physical_dependencies(actual, hosts, true);
    for (const auto& expansion : removed_expansions) (void)make_assembly_geometry(expansion);
    for (const auto& component : manufactured_components)
        (void)transform_assembly_shape(make_opening_assembly_geometry(component.wall, component.cut,
            component.assembly, component.operation).shape, component.transform);
    validate_active_wall_physical_dependencies(candidate, hosts, true);
    const auto final_constraints = active_phase_constraints()
        ? validate_active_phase_constraint_integrity(candidate) : validate_constraint_integrity(candidate);
    if (final_constraints) reject(*final_constraints);
    return OpeningRemoval{std::move(candidate), std::move(hosts)};
}

std::optional<ApplyEntityChanges> derive_snapshot(const DocumentSnapshot& source,
    const std::vector<std::string>& selection, const std::string& message) {
    const auto& actual = source.entities();
    if (!has_selected_opening(actual, selection)) return std::nullopt;
    if (selection.size() > selection_limit) reject("opening selection budget exceeded");
    if (!source.is_editable()) throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    if (message.size() > 4096) reject("command message budget exceeded");
    const auto removal = derive(actual, selection, [&source] { return source.uses_active_phase_constraints(); });
    if (!removal) return std::nullopt;
    const auto& candidate = removal->entities;
    ApplyEntityChanges command; command.expected_revision = source.revision(); command.message = message;
    for (const auto& [id, entity] : actual) {
        const auto after = candidate.find(id);
        if (after != candidate.end() && exact(entity, after->second)) continue;
        if (after == candidate.end()) command.entity_changes.push_back(EntityChange::erase(id));
        else command.entity_changes.push_back(EntityChange::upsert(after->second));
    }
    // The real captured snapshot supplies history, assets and policy admission.
    // Never fabricate a snapshot from a filtered/truncated entity map.
    const auto preview = Document::preview_command(source, Command{command});
    if (preview.entities() != candidate || preview.assets() != source.assets() || !preview.is_editable())
        reject("final raw command admission changed unrelated state or made the document read-only");
    for (const auto& [id, entity] : candidate) if (!exact(entity, preview.entities().at(id)))
        reject("final raw command admission changed the exact retained source representation: " + id);
    validate_architectural_geometry_changes(source, preview,
        std::vector<std::string>(removal->hosts.begin(), removal->hosts.end()));
    return command;
}
} // namespace

std::optional<std::map<std::string, Entity, std::less<>>> replay_hosted_opening_removal(
    const std::map<std::string, Entity, std::less<>>& actual,
    const std::vector<std::string>& selected_opening_ids, bool active_phase_constraints,
    bool complete_hosted_catalog_consequences) {
    try {
        auto removal = derive(actual, selected_opening_ids, [active_phase_constraints] { return active_phase_constraints; },
            complete_hosted_catalog_consequences);
        if (!removal) return std::nullopt;
        return std::move(removal->entities);
    }
    catch (const Json::exception& error) { reject(std::string("malformed actual source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message_text = error.GetMessageString();
        reject(std::string("native source/candidate admission failed: ") +
            (message_text ? message_text : "Open CASCADE failure"));
    }
}

std::optional<ApplyEntityChanges> prepare_hosted_opening_removal(const DocumentSnapshot& source,
    const std::vector<std::string>& selected_opening_ids, const std::string& message) {
    try { return derive_snapshot(source, selected_opening_ids, message); }
    catch (const Json::exception& error) { reject(std::string("malformed actual source: ") + error.what()); }
    catch (const Standard_Failure& error) {
        const auto* message_text = error.GetMessageString();
        reject(std::string("native source/candidate admission failed: ") +
            (message_text ? message_text : "Open CASCADE failure"));
    }
}
} // namespace sketch
