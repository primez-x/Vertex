#include "sketch/slab_hosted_geometry_edit.hpp"

#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"

#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_inventory = 65536;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Slab hosted geometry edit: " + reason);
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
bool contains_owner(const Json& value, const Ids& owners) {
    if (value.is_string()) return owners.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key,child]:value.items())
            if (owners.contains(key) || contains_owner(child,owners)) return true;
    } else if (value.is_array())
        for (const auto& child : value) if (contains_owner(child, owners)) return true;
    return false;
}
// Raw discovery establishes only which catalogs must be decoded. Search malformed
// host slots too, so a non-string host value or an unexpected row cannot silently
// hide an affected reference. Physical replay has already bounded the full JSON.
bool has_host_binding(const Json& value, const Ids& owners) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key == "host_entity_id" && contains_owner(child, owners)) return true;
            if (has_host_binding(child, owners)) return true;
        }
    } else if (value.is_array()) {
        for (const auto& child : value) if (has_host_binding(child, owners)) return true;
    }
    return false;
}
bool affected_catalog(const Json& model, const Ids& owners) {
    if (has_host_binding(model, owners)) return true;
    const auto schema = field(model, "schema");
    const bool supported = schema && schema->is_string() &&
        (*schema == "sketch.assemblies.v1" || *schema == "sketch.assemblies.v2" ||
         *schema == "sketch.assemblies.v3" || *schema == "sketch.assemblies.v4" ||
         *schema == "sketch.assemblies.v5");
    // Unknown dialects may rename or relocate binding slots, including keyed
    // inventories. Opaque actual-owner references cannot acquire motion rules.
    if (!supported && contains_owner(model,owners)) return true;
    const auto rows = field(model, "instances");
    if (!rows) return false;
    if (!rows->is_array()) return contains_owner(*rows, owners);
    for (const auto& row : *rows) {
        if (!row.is_object() && contains_owner(row, owners)) return true;
        const auto placement = field(row, "placement");
        if (placement && !placement->is_null() && contains_owner(*placement, owners)) return true;
    }
    return false;
}
void admit_catalog_context(const Entity& catalog, const ConstraintPhaseScope& scope,
    const Entities& source) {
    if (scope.inactive_owner_ids.contains(catalog.id))
        reject("affected catalog is inactive in the saved design: " + catalog.id);
    struct Binding { const char* key; const char* type; };
    constexpr Binding bindings[]{{"property_id", "property"}, {"building_id", "building"},
        {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}};
    // Catalogs may legitimately be shared at property/building scope. Match
    // existing hosted discovery: each present relationship needs its actual
    // typed owner, without inventing a complete drawing-placement requirement.
    for (const auto& binding : bindings) {
        const auto value = field(catalog.properties, binding.key);
        if (!value) continue;
        if (!value->is_string()) reject("affected catalog has malformed actual context: " + catalog.id);
        const auto owner = source.find(value->get_ref<const std::string&>());
        if (owner == source.end() || owner->second.type != binding.type)
            reject("affected catalog has unresolved actual context: " + catalog.id);
    }
}
void catalog_bounds(const Json& model, std::size_t& inventory, const std::string& id) {
    for (const auto* key : {"materials", "types", "instances"}) {
        const auto rows = field(model, key);
        if (!rows || !rows->is_array() || rows->size() > maximum_inventory - inventory)
            reject("affected catalog inventory is not bounded: " + id);
        inventory += rows->size();
    }
}
AssemblyTransform world_transform(const SlabGeometryEditIntent& intent) {
    AssemblyPoint3 pivot, offset;
    double theta = 0, scale = 1;
    bool flip_x = false, flip_y = false;
    if (intent.kind == SlabGeometryEditKind::transform_plan) {
        if (intent.uniform_scale != 1)
            reject("hosted plan-only scale cannot scale the physical assembly profile; use a model transform");
        const auto& t = *intent.transform;
        pivot = {t.pivot.x, t.pivot.y, 0}; offset = {t.offset.x, t.offset.y, 0};
        theta = t.rotation_radians; flip_x = t.flip_horizontal; flip_y = t.flip_vertical;
    } else if (intent.kind == SlabGeometryEditKind::transform_model) {
        const auto& t = *intent.model_transform;
        pivot = {t.pivot_m.x, t.pivot_m.y, t.pivot_m.z};
        offset = {t.offset_m.x, t.offset_m.y, t.offset_m.z};
        theta = t.rotation_radians; scale = t.uniform_scale;
        flip_x = t.flip_horizontal; flip_y = t.flip_vertical;
    } else return {};
    // Slab operations rotate then reflect in world X/Y. Assembly operations
    // reflect local Y then rotate. D_world*R(theta) = R(yaw)*D_y^odd.
    const bool odd = flip_x != flip_y;
    AssemblyTransform result{{}, std::remainder((odd ? -theta : theta) +
        (flip_x ? std::numbers::pi : 0), 2 * std::numbers::pi), scale, odd};
    if (result.rotation_radians == 0 && !odd && scale == 1) result.translation_m = offset;
    else {
        const auto moved_pivot = transform_assembly_point(pivot, result);
        result.translation_m = {pivot.x - moved_pivot.x + offset.x,
            pivot.y - moved_pivot.y + offset.y,
            scale == 1 ? offset.z : std::fma(1 - scale, pivot.z, offset.z)};
    }
    (void)transform_assembly_point({}, result);
    return result;
}
// Same authoritative profile/legacy-host split as native assembly rendering.
// Both snapshots are admitted; legacy geometry comes from each actual resolved
// host, including vertex and axis edits that have no rigid placement operator.
void admit_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const Entities& entities, AssemblyExpansionBudget& budget) try {
    if (!instance.placement) reject("affected instance has no actual hosted placement: " + instance.id);
    const auto& placement = *instance.placement;
    const auto host = entities.find(placement.host_entity_id);
    if (host == entities.end() || host->second.type != "slab")
        reject("affected instance requires its actual slab host: " + instance.id);
    const auto expansion = model.expand(instance, budget);
    if (!expansion.profiles.empty()) {
        (void)make_assembly_geometry(expansion);
        return;
    }
    Slab slab; std::string error;
    if (!read_document_slab(resolve_vertical_placement(entities, host->second), slab, error))
        reject("cannot admit actual hosted slab " + host->first + ": " + error);
    auto shape = make_slab(slab);
    const auto apply = [&](const gp_Trsf& transform) {
        BRepBuilderAPI_Transform changed(shape, transform, true);
        if (!changed.IsDone() || changed.Shape().IsNull())
            reject("native placement failed for affected instance: " + instance.id);
        shape = changed.Shape();
    };
    if (placement.mirrored_y) {
        gp_Trsf mirror;
        mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)));
        apply(mirror);
    }
    gp_Trsf scale; scale.SetScale(gp_Pnt(0, 0, 0), placement.scale); apply(scale);
    gp_Trsf rotate;
    rotate.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), placement.rotation_radians);
    apply(rotate);
    gp_Trsf translate;
    translate.SetTranslation(gp_Vec(placement.translation_m.x, placement.translation_m.y, placement.translation_z_m));
    apply(translate);
    const auto volume = solid_volume(shape);
    if (!BRepCheck_Analyzer(shape).IsValid() || !std::isfinite(volume) || volume <= 0)
        reject("affected instance produces an invalid native solid: " + instance.id);
} catch (const Standard_Failure& error) {
    reject(std::string("affected hosted native admission failed: ") + error.what());
}
} // namespace

std::map<std::string, Entity, std::less<>> replay_slab_geometry_with_hosted_entities(
    const Entities& source, const std::vector<SlabGeometryEditIntent>& intents) {
    // Validates actual targets, mathematical intent, saved activity, bounded
    // payloads and resolved physical/material context before catalog discovery.
    auto result = replay_slab_geometry_entities(source, intents);
    Ids owners;
    std::map<std::string, const SlabGeometryEditIntent*, std::less<>> changed_intents;
    for (const auto& intent : intents) if (result.at(intent.slab_id) != source.at(intent.slab_id)) {
        owners.insert(intent.slab_id); changed_intents.emplace(intent.slab_id, &intent);
    }
    if (owners.empty()) return result;
    const auto scope = constraint_phase_scope(source);
    AssemblyExpansionBudget source_budget, candidate_budget;
    std::size_t inventory = 0;
    for (const auto& [id, catalog] : source) {
        if (catalog.type != "assembly_model") continue;
        const auto raw = field(catalog.properties, "model");
        if (!affected_catalog(raw ? *raw : catalog.properties, owners)) continue;
        admit_catalog_context(catalog, scope, source);
        if (!raw) reject("affected catalog has no actual model: " + id);
        catalog_bounds(*raw, inventory, id);
        try {
            const auto model = AssemblyModel::from_json(*raw);
            std::map<std::string, AssemblyTransform, std::less<>> transforms;
            Ids instance_ids;
            for (const auto& instance : model.instances()) {
                if (!instance.placement || !owners.contains(instance.placement->host_entity_id)) continue;
                instance_ids.insert(instance.id);
                admit_instance(model, instance, source, source_budget);
                const auto& intent = *changed_intents.at(instance.placement->host_entity_id);
                if (intent.kind == SlabGeometryEditKind::transform_plan ||
                    intent.kind == SlabGeometryEditKind::transform_model)
                    transforms.emplace(instance.id, world_transform(intent));
            }
            if (instance_ids.empty()) reject("catalog has an affected host binding outside a supported hosted instance: " + id);
            // Raw patching keeps definitions, materials, overrides and unchanged
            // scalars exact. XYZ envelope upgrade happens only when required.
            if (!transforms.empty()) result.at(id).properties.at("model") =
                transform_hosted_assembly_model(*raw, transforms);
            const auto candidate = AssemblyModel::from_json(result.at(id).properties.at("model"));
            for (const auto& instance : candidate.instances()) if (instance_ids.contains(instance.id))
                admit_instance(candidate, instance, result, candidate_budget);
        } catch (const std::exception& error) {
            reject("catalog " + id + ": " + error.what());
        }
    }
    return result;
}
} // namespace sketch
