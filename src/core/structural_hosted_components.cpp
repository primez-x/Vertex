#include "sketch/structural_hosted_components.hpp"

#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/site_frame.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>

#include <Standard_Failure.hxx>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t maximum_entities = 65536;
constexpr std::size_t maximum_identities = 4096;

[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Structural hosted components: " + reason);
}
void identity(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("identity must contain 1..128 supported ASCII characters");
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
const Json* field(const Json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
// Scan opaque strings only to reserve names and detect unsupported references.
// The scan never supplies rewrite authority. Bounds precede recursive discovery.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > 64 * 1024 * 1024 - bytes) reject("source string budget exceeded");
        bytes += value.size(); values.insert(value);
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) reject("source node/nesting budget exceeded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) reject("source contains a nonfinite scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
};
Strings occupied_strings(const StructuralHostedEntities& actual) {
    if (actual.size() > maximum_entities) reject("source entity budget exceeded");
    Strings result;
    for (const auto* key : {"id", "type", "properties", "required", "extensions"}) result.values.insert(key);
    for (const auto& [id, entity] : actual) {
        identity(id);
        if (id != entity.id || !entity.properties.is_object() || !entity.extensions.is_object())
            reject("source requires actual identified entity envelopes: " + id);
        result.text(id); result.text(entity.type);
        result.read(entity.properties); result.read(entity.extensions);
    }
    return result;
}
bool touches(const Json& value, const Ids& owners) {
    if (value.is_string()) return owners.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key, child] : value.items())
            if (owners.contains(key) || touches(child, owners)) return true;
    } else if (value.is_array()) for (const auto& child : value) if (touches(child, owners)) return true;
    return false;
}
bool host_binding(const Json& value, const Ids& owners) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key == "host_entity_id" && touches(child, owners)) return true;
            if (host_binding(child, owners)) return true;
        }
    } else if (value.is_array()) for (const auto& child : value) if (host_binding(child, owners)) return true;
    return false;
}
bool supported_schema(const Json& raw) {
    const auto schema = field(raw, "schema");
    return schema && schema->is_string() && (*schema == "sketch.assemblies.v1" ||
        *schema == "sketch.assemblies.v2" || *schema == "sketch.assemblies.v3" ||
        *schema == "sketch.assemblies.v4" || *schema == "sketch.assemblies.v5" || *schema == "sketch.assemblies.v6" ||
        *schema == "sketch.assemblies.v7");
}
bool affected_catalog(const Entity& entity, const Ids& owners) {
    if (host_binding(entity.extensions, owners)) return true;
    const auto raw = field(entity.properties, "model");
    const auto& value = raw ? *raw : entity.properties;
    if (host_binding(value, owners)) return true;
    if (!supported_schema(value) && touches(value, owners)) return true;
    const auto rows = field(value, "instances");
    if (!rows) return false;
    if (!rows->is_array()) return touches(*rows, owners);
    for (const auto& row : *rows) {
        if (!row.is_object() && touches(row, owners)) return true;
        const auto placement = field(row, "placement");
        if (placement && !placement->is_null() && touches(*placement, owners)) return true;
    }
    return false;
}
void catalog_bounds(const Json& raw, std::size_t& inventory) {
    for (const auto* key : {"materials", "types", "instances"}) {
        const auto rows = field(raw, key);
        if (!rows || !rows->is_array() || rows->size() > maximum_entities - inventory)
            reject("affected catalog inventory is not bounded");
        inventory += rows->size();
    }
}
void catalog_context(const Entity& entity, const StructuralHostedEntities& actual, const Ids& inactive) {
    if (inactive.contains(entity.id)) reject("affected catalog is inactive: " + entity.id);
    struct Binding { const char* key; const char* type; };
    constexpr Binding bindings[]{{"property_id", "property"}, {"building_id", "building"},
        {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}};
    for (const auto& binding : bindings) {
        const auto value = field(entity.properties, binding.key);
        if (!value) continue;
        if (!value->is_string()) reject("affected catalog has malformed context: " + entity.id);
        const auto owner = actual.find(value->get_ref<const std::string&>());
        if (owner == actual.end() || owner->second.type != binding.type)
            reject("affected catalog has unresolved actual context: " + entity.id);
    }
}
void admit_host(const StructuralHostedEntities& actual, const std::string& id) {
    const auto found = actual.find(id);
    if (found == actual.end()) reject("requires an actual structural host: " + id);
    validate_structural_object_source_entity(found->second);
    (void)make_building_shape(decode_building_entity(resolve_vertical_placement(actual, found->second)));
}
void admit_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const StructuralHostedEntities& actual, AssemblyExpansionBudget& budget) try {
    if (!instance.placement) reject("requires an actual hosted placement: " + instance.id);
    const auto& placement = *instance.placement;
    const auto host = actual.find(placement.host_entity_id);
    if (host == actual.end() || (host->second.type != "column" && host->second.type != "beam"))
        reject("requires an actual column/beam host: " + instance.id);
    const auto expansion = model.expand(instance, budget);
    if (!expansion.profiles.empty()) {
        (void)make_assembly_geometry(expansion);
        return;
    }
    (void)transform_assembly_shape(
        make_building_shape(decode_building_entity(resolve_vertical_placement(actual, host->second))),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
            placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
} catch (const Standard_Failure& error) {
    reject(std::string("hosted native geometry admission failed: ") + error.what());
}
// The structural producer's normalization helpers are private. Retain their
// exact whole-turn/double-flip normalization, exact half-turn cosine/sine and
// difference-form pivot translation. D_world*R(angle) = R(yaw)*D_y^odd.
// G acts after local floor placement and before Site presentation. The physical
// producer separately adjusts raw bound Z by (scale-1)*datum; that adjustment
// must not enter a placement operator and apply the level twice.
AssemblyTransform source_transform(const ArchitecturalGroupTransform& t) {
    const auto finite_point = [](Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    if (!finite_point(t.pivot) || !finite_point(t.offset) || !std::isfinite(t.rotation_z_radians) ||
        !std::isfinite(t.scale) || !(t.scale > 0)) reject("world transform must be finite with positive scale");
    const bool reflected = t.flip_horizontal != t.flip_vertical;
    const auto angle = std::remainder(std::remainder(t.rotation_z_radians, 2.0 * std::numbers::pi) +
        (t.flip_horizontal && t.flip_vertical ? std::numbers::pi : 0.0), 2.0 * std::numbers::pi);
    const auto c = std::abs(angle) == std::numbers::pi ? -1.0 : std::cos(angle);
    const auto s = std::abs(angle) == std::numbers::pi ? 0.0 : std::sin(angle);
    const bool horizontal = reflected && t.flip_horizontal;
    const auto hx = horizontal ? -1.0 : 1.0;
    const auto hy = reflected && t.flip_vertical ? -1.0 : 1.0;
    AssemblyTransform result;
    result.translation_m.x = t.offset.x + (1.0 - t.scale * hx) * t.pivot.x +
        t.scale * hx * ((1.0 - c) * t.pivot.x + s * t.pivot.y);
    result.translation_m.y = t.offset.y + (1.0 - t.scale * hy) * t.pivot.y +
        t.scale * hy * ((1.0 - c) * t.pivot.y - s * t.pivot.x);
    result.translation_m.z = t.offset.z + (1.0 - t.scale) * t.pivot.z;
    result.rotation_radians = std::remainder((reflected ? -angle : angle) +
        (horizontal ? std::numbers::pi : 0.0), 2.0 * std::numbers::pi);
    result.scale = t.scale; result.mirrored_y = reflected;
    (void)transform_assembly_point({}, result);
    return result;
}
void require_ready(const StructuralHostedComponentPlan& plan) {
    if (!plan.ready()) reject(plan.diagnostics.front().entity_id + ": " + plan.diagnostics.front().reason);
}
Ids changed_hosts(const StructuralHostedEntities& actual, const StructuralHostedEntities& physical,
    const std::vector<StructuralObjectEditIntent>& edits) {
    Ids changed;
    for (const auto& edit : edits) if (!exact(actual.at(edit.object_id), physical.at(edit.object_id))) changed.insert(edit.object_id);
    return changed;
}
Json replay_catalog(const StructuralHostedEntities& actual, const Entity& actual_catalog,
    const AssemblyModel& model, const Ids& hosts,
    const std::vector<StructuralObjectEditIntent>& edits) {
    std::map<std::string, AssemblyTransform, std::less<>> transforms;
    std::map<std::string, AssemblyTransform, std::less<>> world_by_host;
    AssemblyExpansionBudget expansion_budget;
    for (const auto& instance : model.instances()) {
        if (!instance.placement || !hosts.contains(instance.placement->host_entity_id)) continue;
        const auto edit = std::find_if(edits.begin(), edits.end(), [&](const auto& value) {
            return value.object_id == instance.placement->host_entity_id;
        });
        if (edit == edits.end()) reject("changed host is absent from typed replay");
        if (edit->transform) {
            const auto expansion=model.expand(instance,expansion_budget);
            if (expansion.profiles.empty()) transforms.emplace(instance.id,source_transform(*edit->transform));
            else {
                const auto& host=instance.placement->host_entity_id;
                auto resolved=world_by_host.find(host);
                if (resolved==world_by_host.end()) resolved=world_by_host.emplace(host,
                    structural_world_transform(actual,host,*edit->transform)).first;
                transforms.emplace(instance.id,resolved->second);
            }
        }
    }
    const auto& raw = actual_catalog.properties.at("model");
    return transforms.empty() ? raw : transform_hosted_assembly_model(raw, transforms,true);
}
} // namespace

AssemblyTransform structural_world_transform(const StructuralHostedEntities& actual,
    const std::string& host_id, const ArchitecturalGroupTransform& transform) {
    const auto local=source_transform(transform);
    const auto& host=actual.at(host_id);
    validate_structural_object_source_entity(host);
    const auto placement=resolve_site_presentation(actual,host_id);
    if (local==AssemblyTransform{}) return local;
    const auto& frame=placement.forward;
    if (frame.translation_m.x==0 && frame.translation_m.y==0 && frame.translation_m.z==0 &&
        frame.rotation_radians==0) return local;
    auto result=local;
    // R_phi * R_theta * D * R_-phi keeps theta exactly without reflection;
    // reflected yaw is theta+2*phi. Avoid adding/cancelling a frame angle.
    if (local.mirrored_y) result.rotation_radians=std::remainder(std::fma(2.0,
        std::remainder(frame.rotation_radians,2.0*std::numbers::pi),local.rotation_radians),
        2.0*std::numbers::pi);
    const auto rotated=transform_assembly_point(local.translation_m,
        AssemblyTransform{{},frame.rotation_radians,1.0,false});
    const auto [c,s]=assembly_rotation_components(result.rotation_radians);
    const auto parity=result.mirrored_y ? -1.0 : 1.0;
    // R_phi*t + (I-L_world)*origin. Difference form retains tiny translation
    // at a distant Site origin instead of subtracting and re-adding that origin.
    const auto x_coefficient=(1.0-result.scale)+result.scale*(1.0-c);
    const auto y_coefficient=parity==1.0 ? x_coefficient :
        (1.0+result.scale)-result.scale*(1.0-c);
    result.translation_m={
        rotated.x+x_coefficient*frame.translation_m.x+result.scale*s*parity*frame.translation_m.y,
        rotated.y-result.scale*s*frame.translation_m.x+y_coefficient*frame.translation_m.y,
        rotated.z+(1.0-result.scale)*frame.translation_m.z};
    (void)transform_assembly_point({},result);
    return result;
}

AssemblyTransform structural_hosted_transform(const StructuralHostedEntities& actual,
    const std::string& catalog_id, const std::string& instance_id,
    const ArchitecturalGroupTransform& transform) {
    const auto& entity=actual.at(catalog_id);
    if (entity.type!="assembly_model") reject("hosted transform requires an actual catalog");
    const auto model=AssemblyModel::from_json(entity.properties.at("model"));
    const auto instance=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& row) {
        return row.id==instance_id;
    });
    if (instance==model.instances().end() || !instance->placement)
        reject("hosted transform requires an actual hosted instance");
    validate_structural_object_source_entity(actual.at(instance->placement->host_entity_id));
    AssemblyExpansionBudget budget;
    return model.expand(*instance,budget).profiles.empty() ? source_transform(transform) :
        structural_world_transform(actual,instance->placement->host_entity_id,transform);
}

bool StructuralHostedComponentPlan::ready() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& value) { return value.blocking; });
}

Entity structural_hosted_catalog_opaque_remainder(const Entity& actual_catalog) {
    if (actual_catalog.type != "assembly_model") reject("opaque catalog remainder requires an actual assembly_model");
    Strings bounds; bounds.read(actual_catalog.properties); bounds.read(actual_catalog.extensions);
    (void)AssemblyModel::from_json(actual_catalog.properties.at("model"));
    auto result = actual_catalog;
    for (auto& row : result.properties.at("model").at("instances")) {
        row.erase("id");
        if (row.contains("placement")) row.at("placement").erase("host_entity_id");
    }
    return result;
}

StructuralHostedComponentPlan inspect_structural_hosted_components(
    const StructuralHostedEntities& actual, const std::vector<std::string>& host_ids) {
    StructuralHostedComponentPlan plan;
    plan.host_ids = host_ids;
    try {
        (void)occupied_strings(actual);
        if (host_ids.size() > maximum_identities) reject("host inventory budget exceeded");
        std::sort(plan.host_ids.begin(), plan.host_ids.end());
        if (std::adjacent_find(plan.host_ids.begin(), plan.host_ids.end()) != plan.host_ids.end()) reject("hosts must be unique");
        const Ids hosts(plan.host_ids.begin(), plan.host_ids.end());
        const auto scope = constraint_phase_scope(actual);
        const auto organization = organize_project(actual);
        for (const auto& id : hosts) {
            identity(id);
            if (scope.inactive_owner_ids.contains(id)) reject("structural host is inactive: " + id);
            admit_host(actual, id);
            const auto& p = actual.at(id).properties;
            const bool scoped = p.contains("property_id") || p.contains("building_id") || p.contains("floor_id") ||
                p.contains("layer_id") || p.contains("level_id") || p.contains("wall_id");
            const auto node = organization.nodes.find(id);
            if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
                reject("structural host has unresolved actual drawing context: " + id);
        }
        AssemblyExpansionBudget budget;
        std::size_t inventory = 0;
        for (const auto& [id, entity] : actual) {
            if (entity.type != "assembly_model" || !affected_catalog(entity, hosts)) continue;
            try {
                catalog_context(entity, actual, scope.inactive_owner_ids);
                const auto& raw = entity.properties.at("model");
                catalog_bounds(raw, inventory);
                const auto model = AssemblyModel::from_json(raw);
                const auto remainder = structural_hosted_catalog_opaque_remainder(entity);
                if (touches(remainder.properties, hosts) || touches(remainder.extensions, hosts))
                    reject("affected host reference has no qualified hosted codec");
                bool selected = false;
                for (const auto& instance : model.instances()) {
                    if (!instance.placement || !hosts.contains(instance.placement->host_entity_id)) continue;
                    selected = true;
                    admit_instance(model, instance, actual, budget);
                    plan.instance_ids.emplace_back(id, instance.id);
                }
                if (!selected) reject("affected binding is outside supported hosted instance slots");
                plan.catalog_ids.push_back(id);
            } catch (const std::exception& error) { plan.diagnostics.push_back({id, error.what(), true}); }
        }
        if (plan.host_ids.size() + plan.catalog_ids.size() + plan.instance_ids.size() > maximum_identities)
            reject("combined hosted identity budget exceeded");
        if (!plan.instance_ids.empty()) {
            const auto aliases = embedded_assembly_presentation_ids(actual);
            for (const auto& key : plan.instance_ids) plan.original_presentation_ids.emplace(key, aliases.at(key));
        }
        std::sort(plan.instance_ids.begin(), plan.instance_ids.end());
    } catch (const Standard_Failure& error) {
        plan.diagnostics.push_back({{}, std::string("native host admission failed: ") + error.what(), true});
    } catch (const std::exception& error) { plan.diagnostics.push_back({{}, error.what(), true}); }
    return plan;
}

StructuralHostedEntities replay_structural_hosted_component_geometry(
    const StructuralHostedEntities& actual, const std::vector<StructuralObjectEditIntent>& edits) {
    if (edits.empty()) return actual;
    (void)occupied_strings(actual);
    auto result = replay_structural_object_edit_entities(actual, edits);
    const auto hosts = changed_hosts(actual, result, edits);
    if (hosts.empty()) return result;
    const auto plan = inspect_structural_hosted_components(actual, {hosts.begin(), hosts.end()});
    require_ready(plan);
    for (const auto& id : plan.catalog_ids) {
        const auto model = AssemblyModel::from_json(actual.at(id).properties.at("model"));
        auto& raw = result.at(id).properties.at("model");
        raw = replay_catalog(actual,actual.at(id), model, hosts, edits);
    }
    const auto final_plan = inspect_structural_hosted_components(result, plan.host_ids);
    require_ready(final_plan);
    if (final_plan.catalog_ids != plan.catalog_ids || final_plan.instance_ids != plan.instance_ids ||
        final_plan.original_presentation_ids != plan.original_presentation_ids)
        reject("ordinary replay changed the qualified hosted roster or original aliases");
    return result;
}

StructuralHostedComponentCopyResult copy_structural_hosted_components(
    const StructuralHostedEntities& actual, const std::vector<StructuralObjectEditIntent>& edits,
    const StructuralHostedIdentityMap& object_ids, const StructuralHostedIdentityMap& catalog_ids,
    const StructuralHostedInstanceIdentityMap& hosted_instance_ids, bool include_unchanged_hosts) {
    auto occupied = occupied_strings(actual);
    const auto physical = replay_structural_object_edit_entities(actual, edits);
    auto hosts = changed_hosts(actual, physical, edits);
    if (include_unchanged_hosts) for (const auto& edit : edits) hosts.insert(edit.object_id);
    if (object_ids.size() != hosts.size()) reject("requires exact authored-host mapping");
    for (const auto& [id, proposed] : object_ids) {
        (void)proposed;
        if (!hosts.contains(id)) reject("host mapping contains an unauthorized target: " + id);
    }
    const auto plan = inspect_structural_hosted_components(actual, {hosts.begin(), hosts.end()});
    require_ready(plan);
    const Ids expected_catalogs(plan.catalog_ids.begin(), plan.catalog_ids.end());
    const std::set<StructuralHostedInstanceKey> expected_instances(plan.instance_ids.begin(), plan.instance_ids.end());
    if (catalog_ids.size() != expected_catalogs.size() || hosted_instance_ids.size() != expected_instances.size())
        reject("requires exact catalog and qualified hosted-instance mappings");
    // Even a host with no selected components must not claim an unrelated
    // embedded component's current render identity through its fresh entity ID.
    const auto original_aliases = embedded_assembly_presentation_ids(actual);
    for (const auto& [key, alias] : original_aliases) { (void)key; occupied.text(alias); }
    Ids fresh;
    const auto reserve = [&](const std::string& id) {
        identity(id);
        if (occupied.values.contains(id) || !fresh.insert(id).second) reject("fresh identity collision: " + id);
    };
    for (const auto& [id, proposed] : object_ids) { (void)id; reserve(proposed); }
    for (const auto& [id, proposed] : catalog_ids) {
        if (!expected_catalogs.contains(id)) reject("catalog mapping contains an unrequested source");
        reserve(proposed);
    }
    for (const auto& [key, proposed] : hosted_instance_ids) {
        if (!expected_instances.contains(key)) reject("instance mapping contains an unrequested qualified source");
        reserve(proposed);
    }
    if (object_ids.size() + catalog_ids.size() > maximum_entities - actual.size()) reject("final entity budget exceeded");
    auto candidate = actual;
    for (const auto& [id, proposed] : object_ids) {
        auto copy = physical.at(id); copy.id = proposed;
        if (!candidate.emplace(proposed, std::move(copy)).second) reject("copied host insertion collides");
    }
    StructuralHostedComponentCopyResult result;
    for (const auto& id : plan.catalog_ids) {
        auto copy = actual.at(id); copy.id = catalog_ids.at(id);
        const auto model = AssemblyModel::from_json(actual.at(id).properties.at("model"));
        auto raw = replay_catalog(actual,actual.at(id), model, hosts, edits);
        auto selected = Json::array();
        for (auto row : raw.at("instances")) {
            const StructuralHostedInstanceKey key{id, row.at("id").get<std::string>()};
            if (!expected_instances.contains(key)) continue;
            row.at("id") = hosted_instance_ids.at(key);
            auto& host = row.at("placement").at("host_entity_id");
            host = object_ids.at(host.get<std::string>());
            selected.push_back(std::move(row));
        }
        raw.at("instances") = std::move(selected);
        copy.properties.at("model") = std::move(raw);
        if (!candidate.emplace(copy.id, copy).second) reject("copied catalog insertion collides");
        result.catalogs.push_back(std::move(copy));
    }
    std::vector<std::string> proposed_hosts;
    for (const auto& [id, proposed] : object_ids) { (void)id; proposed_hosts.push_back(proposed); }
    const auto final_plan = inspect_structural_hosted_components(candidate, proposed_hosts);
    require_ready(final_plan);
    std::set<StructuralHostedInstanceKey> proposed_instances;
    Ids proposed_catalogs;
    for (const auto& [id, proposed] : catalog_ids) { (void)id; proposed_catalogs.insert(proposed); }
    for (const auto& [key, proposed] : hosted_instance_ids) proposed_instances.emplace(catalog_ids.at(key.first), proposed);
    if (Ids(final_plan.catalog_ids.begin(), final_plan.catalog_ids.end()) != proposed_catalogs ||
        std::set<StructuralHostedInstanceKey>(final_plan.instance_ids.begin(), final_plan.instance_ids.end()) != proposed_instances)
        reject("final copied hosted roster differs from independently replayed source");
    if (!original_aliases.empty() || !plan.instance_ids.empty()) {
        const auto final_aliases = embedded_assembly_presentation_ids(candidate);
        for (const auto& [key, alias] : original_aliases)
            if (final_aliases.at(key) != alias) reject("copy changed an original presentation identity");
        Ids aliases;
        for (const auto& [key, proposed] : hosted_instance_ids) {
            const auto& alias = final_aliases.at({catalog_ids.at(key.first), proposed});
            if (occupied.values.contains(alias) || fresh.contains(alias) || !aliases.insert(alias).second)
                reject("copied presentation identity collision");
            result.original_to_proposed_presentation.emplace(original_aliases.at(key), alias);
        }
        result.fresh_aliases.assign(aliases.begin(), aliases.end());
    }
    return result;
}

} // namespace sketch
