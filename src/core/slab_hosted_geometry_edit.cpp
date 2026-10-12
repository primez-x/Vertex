#include "sketch/slab_hosted_geometry_edit.hpp"

#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_directional_resize.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/site_frame.hpp"

#include <cmath>
#include <algorithm>
#include <iterator>
#include <numbers>
#include <set>
#include <stdexcept>

#include <Standard_Failure.hxx>

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
         *schema == "sketch.assemblies.v5" || *schema == "sketch.assemblies.v6" ||
         *schema == "sketch.assemblies.v7" || *schema == "sketch.assemblies.v8" ||
         *schema == "sketch.assemblies.v9");
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
    double theta = 0, scale = 1, vertical_scale = 1;
    bool flip_x = false, flip_y = false;
    if (intent.kind == SlabGeometryEditKind::transform_plan) {
        const auto& t = *intent.transform;
        pivot = {t.pivot.x, t.pivot.y, 0}; offset = {t.offset.x, t.offset.y, 0};
        // The actual slab plan producer changes XY only. Hosted profiles must
        // follow that same map while retaining their physical Z dimensions.
        scale = intent.uniform_scale; vertical_scale = 1 / scale;
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
        (flip_x ? std::numbers::pi : 0), 2 * std::numbers::pi), scale, odd, vertical_scale};
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
void charge_native_segments(const Boundary& outer, const std::vector<Boundary>& holes,
    AssemblyExpansionBudget& budget) {
    auto count = outer.size();
    for (const auto& hole : holes) count += hole.size();
    if (count > budget.max_profile_segments - budget.consumed_profile_segments)
        reject("hosted native profile segment budget exceeded");
    budget.consumed_profile_segments += count;
}
bool admit_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const Entities& entities, AssemblyExpansionBudget& budget,
    AssemblyExpansionBudget* native_budget, bool directional,
    AssemblyExpansion* admitted = nullptr) try {
    if (!instance.placement) reject("affected instance has no actual hosted placement: " + instance.id);
    const auto& placement = *instance.placement;
    const auto host = entities.find(placement.host_entity_id);
    if (host == entities.end() || host->second.type != "slab")
        reject("affected instance requires its actual slab host: " + instance.id);
    const auto expansion = model.expand(instance, budget);
    if (native_budget) {
        if (native_budget->consumed_nodes == native_budget->max_nodes) reject("hosted native instance budget exceeded");
        ++native_budget->consumed_nodes;
    }
    if (!expansion.profiles.empty()) {
        if (native_budget) for (const auto& profile : expansion.profiles)
            charge_native_segments(profile.profile.outer, profile.profile.holes, *native_budget);
        (void)make_assembly_geometry(expansion);
        if (admitted) *admitted = expansion;
        return true;
    }
    if (directional && (instance.profile_overrides || std::any_of(instance.nested_overrides.begin(),
        instance.nested_overrides.end(), [](const auto& node) { return node.profile_overrides.has_value(); })))
        reject("explicit hosted geometry has no native source profiles: " + instance.id);
    Slab slab; std::string error;
    if (!read_document_slab(resolve_vertical_placement(entities, host->second), slab, error))
        reject("cannot admit actual hosted slab " + host->first + ": " + error);
    if (native_budget) charge_native_segments(slab.boundary, slab.holes, *native_budget);
    (void)transform_assembly_shape(make_slab(slab),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
            placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
    if (admitted) *admitted = expansion;
    return false;
} catch (const Standard_Failure& error) {
    reject(std::string("affected hosted native admission failed: ") + error.what());
}

using Path = std::vector<std::string>;
template<class Model> auto& raw_instance(Model& model, const std::string& id) {
    for (auto& row : model.at("instances")) if (row.at("id") == id) return row;
    reject("actual hosted instance row is missing: " + id);
}
const Json& raw_type_profiles(const Json& model, const std::string& id) {
    for (const auto& row : model.at("types")) if (row.at("id") == id) {
        static const Json empty = Json::array();
        return row.contains("profiles") ? row.at("profiles") : empty;
    }
    reject("actual hosted profile type is missing: " + id);
}
const Json* raw_path(const Json& instance, const Path& path) {
    if (const auto rows = field(instance, "nested_overrides"))
        for (const auto& row : *rows) if (row.at("part_path").get<Path>() == path) return &row;
    return nullptr;
}
// Directional resize leaves the source primitives, extrusion, material and
// topology exact. Materialization borrows the actual inherited raw roster,
// including its order and numeric forms; only its effective affine changes.
Json retained_profiles(const Json& original, const std::vector<AssemblyProfile>& candidate) {
    const auto encoded = encode_assembly_profiles(candidate);
    if (original.size() != encoded.size()) reject("directional profile correspondence is incomplete");
    auto result = Json::array();
    for (const auto& source : original) {
        const auto found = std::find_if(encoded.begin(), encoded.end(),
            [&](const Json& row) { return row.at("id") == source.at("id"); });
        if (found == encoded.end()) reject("directional profile correspondence is missing");
        auto row = source;
        if (!found->contains("planar_affine")) { result.push_back(std::move(row)); continue; }
        // A mapped profile's closed envelope replaces the historical version
        // field while retaining the authored topology payload verbatim.
        row.erase("version");
        row["profile_version"] = found->at("profile_version");
        if (!row.contains("topology")) row["topology"] = found->at("topology");
        auto affine = found->at("planar_affine");
        if (const auto saved = field(source, "planar_affine")) {
            affine["version"] = saved->at("version");
            for (const auto key : {"source_origin_m", "target_origin_m", "matrix"})
                for (std::size_t i = 0; i < affine.at(key).size(); ++i)
                    if (affine.at(key)[i] == saved->at(key)[i]) affine[key][i] = saved->at(key)[i];
            row["profile_version"] = source.at("profile_version");
        }
        row["planar_affine"] = std::move(affine);
        result.push_back(std::move(row));
    }
    return result;
}
void patch_directional_profiles(Json& candidate, const Json& original,
    const AssemblyDirectionalResizeDraft& draft, const Json* borrowed_profiles = nullptr) {
    if (!draft.changed) return;
    const auto& source_row = raw_instance(original, draft.source_instance.id);
    auto& row = raw_instance(candidate, draft.source_instance.id);
    for (const auto& node : draft.nodes) {
        if (node.source_profiles == node.candidate_profiles) continue;
        const Json* old = node.part_path.empty() ? &source_row : raw_path(source_row, node.part_path);
        const auto& profiles = node.part_path.empty() && borrowed_profiles ? *borrowed_profiles :
            old && old->contains("profile_overrides") ? old->at("profile_overrides") :
                raw_type_profiles(original, node.type_id);
        auto retained = retained_profiles(profiles, node.candidate_profiles);
        if (node.part_path.empty()) row["profile_overrides"] = std::move(retained);
        else {
            if (!row.contains("root_transform")) row["root_transform"] = nullptr;
            if (!row.contains("nested_overrides")) row["nested_overrides"] = Json::array();
            auto& paths = row.at("nested_overrides");
            auto found = std::find_if(paths.begin(), paths.end(),
                [&](const Json& value) { return value.at("part_path").get<Path>() == node.part_path; });
            if (found == paths.end()) {
                paths.push_back(Json{{"part_path", node.part_path}, {"transform", nullptr},
                    {"property_overrides", Json::object()}, {"material_overrides", Json::object()},
                    {"quantity_overrides", Json::object()}});
                found = std::prev(paths.end());
            }
            (*found)["profile_overrides"] = std::move(retained);
        }
    }
    candidate["schema"] = "sketch.assemblies.v9";
}

AssemblyInstance materialize_borrowed_host(const AssemblyModel& model, const AssemblyInstance& instance,
    const Entities& source, Json& source_profiles) {
    if (instance.profile_overrides || std::any_of(instance.nested_overrides.begin(),
        instance.nested_overrides.end(), [](const auto& node) { return node.profile_overrides.has_value(); }))
        reject("suppressed hosted geometry cannot acquire a borrowed host profile");
    Slab slab; std::string error;
    const auto& host = source.at(instance.placement->host_entity_id);
    const auto resolved_host = resolve_vertical_placement(source, host);
    if (!read_document_slab(resolved_host, slab, error))
        reject("cannot materialize actual borrowed slab: " + error);
    AssemblyProfile profile{"host-slab", slab.boundary, slab.holes, slab.elevation, slab.thickness};
    profile.planar_affine = slab.planar_affine;
    // Native fallback colors the entire host copy by the first resolved slot
    // with an actual catalog color; it does not publish host layer regions.
    const auto resolved = model.resolve(instance.id);
    for (const auto& [slot, id] : resolved.materials) {
        const auto material = std::find_if(model.materials().begin(), model.materials().end(),
            [&](const auto& row) { return row.id == id; });
        if (material != model.materials().end() && material->color_srgb) {
            profile.material_slot = slot; break;
        }
    }
    auto result = instance;
    result.profile_overrides = std::vector<AssemblyProfile>{std::move(profile)};
    source_profiles = encode_assembly_profiles(*result.profile_overrides);
    auto& raw_profile = source_profiles.at(0);
    raw_profile["outer"] = host.properties.at("boundary");
    raw_profile["holes"] = host.properties.at("holes");
    if (slab.planar_affine) raw_profile["planar_affine"] = host.properties.at("planar_affine");
    const auto& p = resolved_host.properties;
    raw_profile["elevation_m"] = p.at(p.contains("elevation_m") ? "elevation_m" : "elevation");
    raw_profile["height_m"] = p.at(p.contains("thickness_m") ? "thickness_m" : "thickness");
    return result;
}
} // namespace

std::map<std::string, Entity, std::less<>> replay_slab_geometry_with_hosted_entities(
    const Entities& source, const std::vector<SlabGeometryEditIntent>& intents) {
    // Validates actual targets, mathematical intent, saved activity, bounded
    // payloads and resolved physical/material context before catalog discovery.
    auto result = replay_slab_geometry_entities(source, intents);
    const bool directional_cohort = std::any_of(intents.begin(), intents.end(), [](const auto& intent) {
        return intent.kind == SlabGeometryEditKind::resize_plan && intent.coordinate_world_hosted_geometry;
    });
    Ids owners;
    std::map<std::string, const SlabGeometryEditIntent*, std::less<>> changed_intents;
    for (const auto& intent : intents) if (result.at(intent.slab_id) != source.at(intent.slab_id) ||
        (intent.kind == SlabGeometryEditKind::resize_plan && intent.coordinate_world_hosted_geometry)) {
        owners.insert(intent.slab_id); changed_intents.emplace(intent.slab_id, &intent);
    }
    if (owners.empty()) return result;
    const auto scope = constraint_phase_scope(source);
    AssemblyExpansionBudget source_budget, candidate_budget;
    AssemblyExpansionBudget source_native_budget, candidate_native_budget;
    AssemblyCatalogTransferBudget source_raw_budget, candidate_raw_budget;
    std::size_t inventory = 0;
    Ids catalog_ids;
    std::map<std::string, Ids, std::less<>> affected_instances;
    // Reserve the complete actual raw cohort before its first semantic decode.
    for (const auto& [id, catalog] : source) {
        if (catalog.type != "assembly_model") continue;
        const auto raw = field(catalog.properties, "model");
        if (!affected_catalog(raw ? *raw : catalog.properties, owners)) continue;
        admit_catalog_context(catalog, scope, source);
        if (!raw) reject("affected catalog has no actual model: " + id);
        catalog_bounds(*raw, inventory, id);
        if (directional_cohort) admit_existing_assembly_catalog_work(catalog, source_raw_budget);
        catalog_ids.insert(id);
    }
    for (const auto& id : catalog_ids) {
        const auto& catalog = source.at(id);
        const auto* raw = field(catalog.properties, "model");
        try {
            const auto model = AssemblyModel::from_json(*raw);
            std::map<std::string, AssemblyTransform, std::less<>> historical_transforms, coordinated_transforms;
            Ids instance_ids;
            for (const auto& instance : model.instances()) {
                if (!instance.placement || !owners.contains(instance.placement->host_entity_id)) continue;
                instance_ids.insert(instance.id);
                const auto& intent = *changed_intents.at(instance.placement->host_entity_id);
                const bool directional = intent.kind == SlabGeometryEditKind::resize_plan &&
                    intent.coordinate_world_hosted_geometry;
                if (directional && scope.assembly_memberships.inactive_roots.contains({id, instance.id}))
                    reject("affected directional hosted instance is inactive: " + instance.id);
                AssemblyExpansion actual;
                const bool type_owned = admit_instance(model, instance, source, source_budget,
                    directional_cohort ? &source_native_budget : nullptr, directional, &actual);
                if (directional) {
                    const auto& resize = *intent.resize;
                    AssemblyDirectionalResize operation{resize.anchor_m, resize.scale_x, resize.scale_y,
                        resize.frame_rotation_radians};
                    const auto frame = resolve_site_presentation(source, instance.placement->host_entity_id).forward;
                    // Both native source forms are actual world bodies. The
                    // authored slab operation is local to its Site frame, so
                    // v4 always applies F*D*F^-1, including the first borrowed-
                    // host materialization. Subsequent edits retain that same
                    // operator rather than changing meaning with source form.
                    operation.operation_frame = AssemblyTransform{
                        {frame.translation_m.x, frame.translation_m.y, frame.translation_m.z},
                        frame.rotation_radians, 1.0, false};
                    Json borrowed_profiles;
                    // Anonymous native host copies acquire their own exact
                    // source body without changing the actual host binding/pose.
                    // Unit factors authenticate both snapshots without creating
                    // an override or changing their historical host-copy meaning.
                    if (!type_owned && (resize.scale_x != 1 || resize.scale_y != 1)) {
                        const auto borrowed = materialize_borrowed_host(model, instance, source, borrowed_profiles);
                        actual = model.expand(borrowed, source_budget);
                    }
                    const auto draft = draft_assembly_directional_resize(model, actual, operation, candidate_budget);
                    if ((resize.scale_x != 1 || resize.scale_y != 1) && !draft.changed)
                        reject("hosted directional resize produced no genuine profile change");
                    patch_directional_profiles(result.at(id).properties.at("model"), *raw, draft,
                        borrowed_profiles.is_null() ? nullptr : &borrowed_profiles);
                    continue;
                }
                if (intent.kind == SlabGeometryEditKind::transform_plan ||
                    intent.kind == SlabGeometryEditKind::transform_model) {
                    auto transform = world_transform(intent);
                    if (intent.coordinate_world_hosted_geometry) {
                        if (type_owned) {
                            const auto frame = resolve_site_presentation(source, instance.placement->host_entity_id).forward;
                            transform = conjugate_assembly_transform_through_rigid_frame(transform,
                                {{frame.translation_m.x, frame.translation_m.y, frame.translation_m.z},
                                    frame.rotation_radians, 1.0, false});
                        }
                        coordinated_transforms.emplace(instance.id, transform);
                    } else historical_transforms.emplace(instance.id, transform);
                }
            }
            if (instance_ids.empty()) reject("catalog has an affected host binding outside a supported hosted instance: " + id);
            // Raw patching keeps definitions, materials, overrides and unchanged
            // scalars exact. XYZ envelope upgrade happens only when required.
            // Historical operations retain their whole-catalog dialect upgrade.
            // Coordinated operations patch only their actual affected rows.
            auto& candidate_raw = result.at(id).properties.at("model");
            if (!historical_transforms.empty()) candidate_raw =
                transform_hosted_assembly_model(candidate_raw, historical_transforms, directional_cohort);
            if (!coordinated_transforms.empty()) candidate_raw =
                transform_hosted_assembly_model(candidate_raw, coordinated_transforms, true);
            affected_instances.emplace(id, std::move(instance_ids));
        } catch (const std::exception& error) {
            reject("catalog " + id + ": " + error.what());
        }
    }
    // Candidate raw admission is also one complete ledger, before decoding the
    // first saved candidate. No partially patched or unadmitted result escapes.
    if (directional_cohort) for (const auto& id : catalog_ids)
        admit_existing_assembly_catalog_work(result.at(id), candidate_raw_budget);
    for (const auto& id : catalog_ids) try {
        const auto candidate = AssemblyModel::from_json(result.at(id).properties.at("model"));
        for (const auto& instance : candidate.instances()) if (affected_instances.at(id).contains(instance.id)) {
            const auto& intent = *changed_intents.at(instance.placement->host_entity_id);
            admit_instance(candidate, instance, result, candidate_budget,
                directional_cohort ? &candidate_native_budget : nullptr,
                intent.kind == SlabGeometryEditKind::resize_plan && intent.coordinate_world_hosted_geometry);
        }
    } catch (const std::exception& error) {
        reject("catalog " + id + ": " + error.what());
    }
    return result;
}
} // namespace sketch
