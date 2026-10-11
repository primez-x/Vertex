#include "sketch/phase_slab_profile_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/site_frame.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <Standard_Failure.hxx>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t layer_limit = 1024;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Slab profile edit fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Slab profile edit field is missing");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Slab profile identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Slab profile identity is invalid");
    return result;
}
const Json* field(const Json& object, const std::string& key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}
bool exact(const Json& left, const Json& right) {
    return left == right && left.dump() == right.dump();
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && exact(left.properties, right.properties) && exact(left.extensions, right.extensions);
}
bool version_one(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 1;
}
bool version_three(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 3;
}
const char* unit_name(Unit unit) {
    switch (unit) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Slab profile unit is unsupported");
}
void receipt_budget(const Json& value) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit || value.dump().size() > proof_limit)
        invalid("Slab profile quantity receipt budget exceeded");
}
Quantity quantity(const Json& value, bool positive) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || (positive && result.metres <= default_geometry_tolerance_metres))
        invalid("Slab profile thickness must be finite and positive");
    return result;
}
Json quantity(const Quantity& value, bool positive) {
    // Compose the shared receipt schema because elevation permits signed zero
    // and negative values, unlike the common fixed-length encoder policy.
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)}, {"exact_metres", {
            {"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result, positive);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("Slab profile quantity is internally inconsistent");
    return result;
}
bool known_receipt(const Json& value) {
    if (!value.is_object()) invalid("Slab profile quantity receipt must be an object");
    const auto version = field(value, "version");
    if (!version || (!version->is_number_integer() && !version->is_number_unsigned()) ||
        (version->is_number_integer() && !version->is_number_unsigned() && version->get<std::int64_t>() < 0))
        invalid("Slab profile quantity receipt version is invalid");
    return version_one(*version);
}
Quantity admitted_receipt(const Json& value, double metres) {
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (result.metres != metres) invalid("Slab profile quantity receipt is stale");
    return result;
}
const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && (!result->is_object() || result->size() > collection_limit))
        invalid("Slab profile quantity_entries must be a bounded object");
    return result;
}
double number(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        invalid("Slab profile scalar must be a finite number");
    return value.get<double>();
}
struct Dimension {
    const char* canonical;
    const char* alias;
    bool positive;
    std::optional<Quantity> SlabProfileEditIntent::* intent;
};
constexpr std::array dimensions{
    Dimension{"thickness_m", "thickness", true, &SlabProfileEditIntent::thickness},
    Dimension{"elevation_m", "elevation", false, &SlabProfileEditIntent::elevation}};
double scalar(const Entity& entity, const Dimension& dimension) {
    const auto canonical = field(entity.properties, dimension.canonical), alias = field(entity.properties, dimension.alias);
    if (!canonical && !alias) invalid("Slab profile scalar is missing");
    const double result = number(canonical ? *canonical : *alias);
    if (dimension.positive && result <= default_geometry_tolerance_metres)
        invalid("Slab profile thickness must be positive");
    if (alias && number(*alias) != result) invalid("Slab profile scalar aliases disagree");
    return result;
}
Slab native_slab(const Entity& entity) {
    if (entity.type != "slab" || !entity.properties.is_object() || !entity.extensions.is_object())
        invalid("Slab profile source requires an actual slab with object properties and extensions");
    (void)identity(entity.id);
    if (const auto layers = field(entity.properties, "layers"); layers && layers->is_array() && layers->size() > layer_limit)
        invalid("Slab profile source layer inventory budget exceeded");
    // The authoritative reader validates the actual layer envelope, materials,
    // layer order and total. No stripped or normalized projection is admitted.
    Slab result;
    std::string diagnostic;
    if (!read_document_slab(entity, result, diagnostic))
        throw std::invalid_argument("Slab profile source " + entity.id + ": " + diagnostic);
    return result;
}
Slab slab(const Entity& entity) {
    for (const auto& dimension : dimensions) (void)scalar(entity, dimension);
    return native_slab(entity);
}
bool known_pointer(const std::string& pointer) {
    for (const auto& dimension : dimensions)
        for (const auto* name : {dimension.canonical, dimension.alias})
            if (pointer == "/" + std::string(name)) return true;
    constexpr std::string_view prefix = "/layers/";
    const std::string_view path(pointer);
    if (!path.starts_with(prefix)) return false;
    const auto tail = path.substr(prefix.size());
    const auto slash = tail.find('/');
    if (slash == std::string_view::npos) return false;
    const auto name = tail.substr(slash + 1);
    return name == "thickness_m" || name == "thickness";
}
const Json* actual_scalar(const Entity& source, const std::string& pointer) {
    for (const auto& dimension : dimensions)
        for (const auto* name : {dimension.canonical, dimension.alias})
            if (pointer == "/" + std::string(name)) return field(source.properties, name);
    constexpr std::string_view prefix = "/layers/";
    const std::string_view path(pointer);
    if (path.starts_with(prefix)) {
        const auto tail = path.substr(prefix.size());
        const auto slash = tail.find('/');
        if (slash != std::string_view::npos) {
            const auto name = tail.substr(slash + 1);
            if (name == "thickness_m" || name == "thickness") {
                const auto index = tail.substr(0, slash);
                std::size_t position = 0;
                const auto parsed = std::from_chars(index.data(), index.data() + index.size(), position);
                const auto layers = field(source.properties, "layers");
                if (index.empty() || (index.size() > 1 && index.front() == '0') ||
                    parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size() ||
                    !layers || !layers->is_array() || position >= layers->size())
                    invalid("Slab profile quantity receipt has no existing layer index");
                return field(layers->at(position), std::string(name));
            }
        }
    }
    return nullptr;
}
void replace_receipt(Entity& result, const std::string& pointer, const Quantity& value, bool positive) {
    const auto encoded = quantity(value, positive);
    if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
    auto& values = result.properties.at("quantity_entries");
    const auto previous = values.find(pointer);
    if (previous == values.end()) {
        if (values.size() >= collection_limit) invalid("Slab profile quantity_entries budget exceeded");
        values[pointer] = encoded;
        return;
    }
    if (!known_receipt(*previous)) invalid("Slab profile edit cannot replace an opaque future receipt");
    // Source admission binds the receipt to the original field. Update only
    // its known core, including rational core; every opaque sibling survives.
    for (const auto* key : {"version", "original_expression", "entered_unit"}) (*previous)[key] = encoded.at(key);
    for (const auto* key : {"numerator", "denominator"})
        (*previous)["exact_metres"][key] = encoded.at("exact_metres").at(key);
}
void edit_scalar(Entity& result, const Entity& source, const Dimension& dimension, const Quantity& value) {
    if (value.metres == scalar(source, dimension)) return;
    const auto primary = source.properties.contains(dimension.canonical) ? dimension.canonical : dimension.alias;
    result.properties[primary] = value.metres;
    replace_receipt(result, "/" + std::string(primary), value, dimension.positive);
    for (const auto* name : {dimension.canonical, dimension.alias}) {
        if (std::string_view(name) == primary || !source.properties.contains(name)) continue;
        result.properties[name] = value.metres;
        const auto pointer = "/" + std::string(name);
        const auto source_entries = entries(source);
        if (source_entries && source_entries->contains(pointer)) replace_receipt(result, pointer, value, dimension.positive);
    }
}
Quantity captured_quantity(const Entity& original, const Entity& candidate,
    const std::vector<std::string>& pointers, double metres, bool changed) {
    const auto before = entries(original), after = entries(candidate);
    for (const auto& pointer : pointers) {
        const auto old_receipt = before ? field(*before, pointer) : nullptr;
        const auto new_receipt = after ? field(*after, pointer) : nullptr;
        if (!new_receipt || (changed && old_receipt && exact(*new_receipt, *old_receipt))) continue;
        if (!known_receipt(*new_receipt)) invalid("Slab profile capture cannot author a future quantity receipt");
        return admitted_receipt(*new_receipt, metres);
    }
    invalid("Slab profile capture requires exact entered quantities for changed dimensions");
}
bool any(const SlabProfileEditIntent& intent) {
    return intent.thickness || intent.elevation || (intent.layer_thicknesses &&
        std::any_of(intent.layer_thicknesses->begin(), intent.layer_thicknesses->end(),
            [](const auto& layer) { return !layer.retain_source_thickness; }));
}
bool retains_layer_thickness(const SlabProfileEditIntent& intent) {
    return intent.layer_thicknesses &&
        std::any_of(intent.layer_thicknesses->begin(), intent.layer_thicknesses->end(),
            [](const auto& layer) { return layer.retain_source_thickness; });
}
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
struct MapBudget {
    std::size_t nodes{}, bytes{};
    void text(const std::string& value) {
        if (value.size() > 64 * 1024 * 1024 - bytes) invalid("Slab profile source string budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) invalid("Slab profile source node/nesting budget exceeded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) invalid("Slab profile source has a nonfinite scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
};
void bound_map(const Entities& source) {
    if (source.size() > 65536) invalid("Slab profile source entity budget exceeded");
    MapBudget budget;
    for (const auto& [id, entity] : source) {
        (void)identity(id);
        if (entity.id != id) invalid("Slab profile source contains inconsistent live identities");
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
    }
}
bool references_owner(const Json& value, const Ids& owners) {
    if (value.is_string()) return owners.contains(value.get_ref<const std::string&>());
    if (value.is_array()) {
        for (const auto& child : value) if (references_owner(child, owners)) return true;
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items())
            if (owners.contains(key) || references_owner(child, owners)) return true;
    }
    return false;
}
bool has_host_reference(const Json& value, const Ids& owners) {
    if (value.is_array()) {
        for (const auto& child : value) if (has_host_reference(child, owners)) return true;
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items())
            if ((key == "host_entity_id" && references_owner(child, owners)) || has_host_reference(child, owners)) return true;
    }
    return false;
}
bool affected_catalog(const Json& model, const Ids& owners) {
    if (has_host_reference(model, owners)) return true;
    const auto schema = model.is_object() ? field(model, "schema") : nullptr;
    const bool supported = schema && schema->is_string() &&
        (*schema == "sketch.assemblies.v1" || *schema == "sketch.assemblies.v2" ||
         *schema == "sketch.assemblies.v3" || *schema == "sketch.assemblies.v4" ||
         *schema == "sketch.assemblies.v5" || *schema == "sketch.assemblies.v6" || *schema == "sketch.assemblies.v7" ||
         *schema == "sketch.assemblies.v8");
    if (!supported && references_owner(model, owners)) return true;
    const auto rows = model.is_object() ? field(model, "instances") : nullptr;
    if (!rows) return false;
    if (!rows->is_array()) return references_owner(*rows, owners);
    for (const auto& row : *rows) {
        if (!row.is_object()) { if (references_owner(row, owners)) return true; continue; }
        if (const auto placement = field(row, "placement"); placement && references_owner(*placement, owners)) return true;
    }
    return false;
}
void admit_coordinated_hosts(const Entities& entities, const Ids& owners) {
    const auto organization = organize_project(entities);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    const auto bind = [&](const WallLayerMaterial& reference) {
        (void)identity(reference.catalog_id); (void)identity(reference.material_id);
        const auto found = entities.find(reference.catalog_id);
        if (found == entities.end() || found->second.type != "assembly_model" ||
            !found->second.properties.is_object() || !found->second.properties.contains("model"))
            invalid("Slab profile material requires its actual assembly catalog");
        if (!catalogs.contains(reference.catalog_id)) catalogs.emplace(reference.catalog_id,
            AssemblyModel::from_json(found->second.properties.at("model")));
        const auto& materials = catalogs.at(reference.catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& material) { return material.id == reference.material_id; }))
            invalid("Slab profile material is absent from its actual catalog");
    };
    for (const auto& id : owners) {
        const auto& entity = entities.at(id);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            invalid("Slab profile target has unresolved actual drawing context");
        const auto resolved = native_slab(resolve_vertical_placement(entities, entity));
        (void)make_slab(resolved);
        if (const auto reference = field(entity.properties, "material_assignment")) {
            if (!reference->is_object()) invalid("Slab profile material assignment must be an object");
            const auto version = field(*reference, "version"), catalog = field(*reference, "catalog_id"), material = field(*reference, "material_id");
            if (!version || !version_one(*version) || !catalog || !material) invalid("Slab profile material assignment is unsupported");
            bind({identity(*catalog), identity(*material)});
        }
        for (const auto& layer : resolved.layers) if (layer.material) bind(*layer.material);
    }
}
bool admit_hosted_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const Entities& entities, AssemblyExpansionBudget& budget) try {
    if (!instance.placement) invalid("Slab profile hosted instance has no actual placement");
    const auto& placement = *instance.placement;
    const auto host = entities.find(placement.host_entity_id);
    if (host == entities.end() || host->second.type != "slab") invalid("Slab profile hosted instance has no actual slab host");
    const auto expansion = model.expand(instance, budget);
    if (!expansion.profiles.empty()) {
        (void)make_assembly_geometry(expansion);
        return true;
    }
    (void)transform_assembly_shape(make_slab(native_slab(resolve_vertical_placement(entities, host->second))),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
            placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
    return false;
} catch (const Standard_Failure& error) {
    throw std::invalid_argument(std::string("Slab profile hosted native admission failed: ") + error.what());
}
void coordinate_hosted_elevation(const Entities& source, Entities& result, const Ids& owners) {
    if (owners.empty()) return;
    admit_coordinated_hosts(source, owners);
    admit_coordinated_hosts(result, owners);
    const auto scope = constraint_phase_scope(source);
    AssemblyExpansionBudget source_budget, final_budget;
    std::size_t inventory = 0;
    for (const auto& [id, entity] : source) {
        if (entity.type != "assembly_model") continue;
        const auto raw = entity.properties.is_object() ? field(entity.properties, "model") : nullptr;
        if (!affected_catalog(raw ? *raw : entity.properties, owners)) continue;
        if (scope.inactive_owner_ids.contains(id)) invalid("Slab profile affected catalog is inactive");
        if (!raw) invalid("Slab profile affected catalog has no actual model");
        for (const auto& [key, type] : std::array<std::pair<const char*, const char*>, 5>{{
            {"property_id", "property"}, {"building_id", "building"}, {"floor_id", "floor"},
            {"layer_id", "layer"}, {"wall_id", "wall"}}}) {
            const auto binding = field(entity.properties, key);
            if (!binding) continue;
            const auto target = source.find(identity(*binding));
            if (target == source.end() || target->second.type != type) invalid("Slab profile affected catalog context is unresolved");
        }
        for (const auto* key : {"materials", "types", "instances"}) {
            const auto rows = raw->is_object() ? field(*raw, key) : nullptr;
            if (!rows || !rows->is_array() || rows->size() > 65536 - inventory) invalid("Slab profile affected catalog inventory budget exceeded");
            inventory += rows->size();
        }
        const auto model = AssemblyModel::from_json(*raw);
        std::map<std::string, AssemblyTransform, std::less<>> transforms;
        for (const auto& instance : model.instances()) {
            if (!instance.placement || !owners.contains(instance.placement->host_entity_id)) continue;
            const auto& host = instance.placement->host_entity_id;
            // Raw local authority was admitted before replay. Retain vertical
            // binding exactly and derive local displacement before resolved
            // level coordinates can lose small differences through rounding.
            const double delta = scalar(result.at(host), dimensions[1]) - scalar(source.at(host), dimensions[1]);
            if (!std::isfinite(delta)) invalid("Slab profile hosted elevation displacement is not finite");
            AssemblyTransform transform{{0, 0, delta}, 0, 1, false};
            if (admit_hosted_instance(model, instance, source, source_budget)) {
                const auto& frame = resolve_site_presentation(source, host).forward;
                transform = conjugate_assembly_transform_through_rigid_frame(transform,
                    {{frame.translation_m.x, frame.translation_m.y, frame.translation_m.z}, frame.rotation_radians, 1, false});
            }
            transforms.emplace(instance.id, transform);
        }
        if (transforms.empty()) invalid("Slab profile catalog has a host reference outside supported actual instances");
        auto& candidate_raw = result.at(id).properties.at("model");
        candidate_raw = transform_hosted_assembly_model(*raw, transforms, true);
        const auto candidate = AssemblyModel::from_json(candidate_raw);
        for (const auto& instance : candidate.instances()) if (transforms.contains(instance.id))
            (void)admit_hosted_instance(candidate, instance, result, final_budget);
    }
}
} // namespace

nlohmann::json encode_slab_profile_edit_intent(const SlabProfileEditIntent& intent) {
    (void)identity(intent.slab_id);
    if (intent.layer_thicknesses &&
        (intent.layer_thicknesses->empty() || intent.layer_thicknesses->size() > layer_limit))
        invalid("Slab profile layer inventory budget is invalid");
    if (!any(intent)) invalid("Slab profile edit requires an authored dimension");
    if (intent.coordinate_hosted_elevation && !intent.elevation)
        invalid("Coordinated slab profile edit requires elevation");
    Json result{{"version", intent.coordinate_hosted_elevation ? 3 : retains_layer_thickness(intent) ? 2 : 1}, {"slab_id", intent.slab_id},
        {"thickness", intent.thickness ? quantity(*intent.thickness, true) : Json(nullptr)},
        {"elevation", intent.elevation ? quantity(*intent.elevation, false) : Json(nullptr)},
        {"layer_thicknesses", nullptr}};
    if (intent.coordinate_hosted_elevation) result["coordinate_hosted_elevation"] = true;
    if (intent.layer_thicknesses) {
        auto rows = Json::array();
        std::set<std::string, std::less<>> ids;
        for (const auto& layer : *intent.layer_thicknesses) {
            if (!ids.insert(identity(layer.layer_id)).second) invalid("Slab profile layer identities are duplicated");
            rows.push_back({{"layer_id", layer.layer_id}, {"thickness",
                layer.retain_source_thickness ? Json(nullptr) : quantity(layer.thickness, true)}});
        }
        result["layer_thicknesses"] = std::move(rows);
    }
    if (result.dump().size() > proof_limit) invalid("Slab profile edit proof byte budget exceeded");
    return result;
}

SlabProfileEditIntent decode_slab_profile_edit_intent(const nlohmann::json& value) {
    if (value.dump().size() > proof_limit) invalid("Slab profile edit proof byte budget exceeded");
    const bool coordinated = value.is_object() && value.contains("version") && version_three(value.at("version"));
    if (coordinated) {
        keys(value, {"version", "slab_id", "thickness", "elevation", "layer_thicknesses", "coordinate_hosted_elevation"});
        if (!value.at("coordinate_hosted_elevation").is_boolean() || value.at("coordinate_hosted_elevation") != true)
            invalid("Coordinated slab profile flag must be boolean true");
    } else keys(value, {"version", "slab_id", "thickness", "elevation", "layer_thicknesses"});
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || (version != 1 && version != 2 && !coordinated))
        invalid("Slab profile edit version is unsupported");
    const bool retained_layers = version == 2;
    SlabProfileEditIntent result;
    result.coordinate_hosted_elevation = coordinated;
    result.slab_id = identity(value.at("slab_id"));
    if (!value.at("thickness").is_null()) result.thickness = quantity(value.at("thickness"), true);
    if (!value.at("elevation").is_null()) result.elevation = quantity(value.at("elevation"), false);
    const auto& layers = value.at("layer_thicknesses");
    if (retained_layers && layers.is_null()) invalid("Slab profile retention requires a layer inventory");
    if (!layers.is_null()) {
        if (!layers.is_array() || layers.empty() || layers.size() > layer_limit)
            invalid("Slab profile layer inventory budget is invalid");
        result.layer_thicknesses.emplace();
        for (const auto& row : layers) {
            keys(row, {"layer_id", "thickness"});
            const bool retain = (retained_layers || coordinated) && row.at("thickness").is_null();
            result.layer_thicknesses->push_back({identity(row.at("layer_id")),
                retain ? Quantity{} : quantity(row.at("thickness"), true), retain});
        }
    }
    if (retained_layers && !retains_layer_thickness(result))
        invalid("Slab profile version 2 requires retained layer thickness");
    (void)encode_slab_profile_edit_intent(result);
    return result;
}

void validate_slab_profile_source_entity(const Entity& source) {
    const auto admitted = slab(source);
    if (const auto values = entries(source)) {
        std::size_t proof_bytes = 0;
        for (const auto& [pointer, raw] : values->items()) {
            // Unknown paths retain opaque authority; known paths have strict
            // envelopes, while a declared future version remains opaque.
            if (!known_pointer(pointer) || !known_receipt(raw)) continue;
            const auto actual = actual_scalar(source, pointer);
            if (!actual) invalid("Slab profile quantity receipt is dangling");
            const auto bytes = raw.dump().size();
            if (bytes > proof_limit - proof_bytes) invalid("Slab profile source receipt byte budget exceeded");
            proof_bytes += bytes;
            (void)admitted_receipt(raw, number(*actual));
        }
    }
    (void)make_slab(admitted);
}

Entity replay_slab_profile_entity(const Entity& source, const SlabProfileEditIntent& intent) {
    (void)encode_slab_profile_edit_intent(intent);
    if (source.id != intent.slab_id) invalid("Slab profile target differs from its actual source identity");
    validate_slab_profile_source_entity(source);
    const auto original = slab(source);
    auto result = source;
    for (const auto& dimension : dimensions)
        if (const auto& value = intent.*(dimension.intent)) edit_scalar(result, source, dimension, *value);
    if (intent.layer_thicknesses) {
        if (intent.layer_thicknesses->size() != original.layers.size())
            invalid("Slab profile edit requires every existing layer");
        for (std::size_t i = 0; i < original.layers.size(); ++i) {
            const auto& replacement = intent.layer_thicknesses->at(i);
            const auto& retained = original.layers[i];
            if (replacement.layer_id != retained.id) invalid("Slab profile edit must retain layer identities and order");
            if (replacement.retain_source_thickness) continue;
            if (replacement.thickness.metres == retained.thickness) continue;
            result.properties.at("layers").at(i).at("thickness_m") = replacement.thickness.metres;
            replace_receipt(result, "/layers/" + std::to_string(i) + "/thickness_m", replacement.thickness, true);
        }
    }
    // Native admission enforces the explicit/result total. A total-only edit
    // never rescales retained layers, and a layer edit never invents a total.
    validate_slab_profile_source_entity(result);
    return result;
}

std::map<std::string, Entity, std::less<>> replay_slab_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<SlabProfileEditIntent>& intents) {
    if (intents.size() > collection_limit) invalid("Slab profile target budget exceeded");
    if (intents.empty()) return source;
    if (std::any_of(intents.begin(), intents.end(), [](const auto& intent) { return intent.coordinate_hosted_elevation; }))
        bound_map(source);
    const auto scope = constraint_phase_scope(source);
    std::set<std::string, std::less<>> targets;
    Ids coordinated_owners;
    auto result = source;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_slab_profile_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Slab profile batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.slab_id).second) invalid("Slab profile edit contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.slab_id)) invalid("Slab profile target is inactive in the saved design");
        const auto found = source.find(intent.slab_id);
        if (found == source.end() || found->second.id != found->first)
            invalid("Slab profile target is missing or has inconsistent identity");
        // Resolved elevation_m is a derived coordinate. Source alias checks
        // apply to the retained local source, not the resolver's transient
        // elevation, whose legacy alias intentionally remains unshifted.
        (void)make_slab(native_slab(resolve_vertical_placement(source, found->second)));
        result.at(intent.slab_id) = replay_slab_profile_entity(found->second, intent);
        if (intent.coordinate_hosted_elevation && scalar(result.at(intent.slab_id), dimensions[1]) != scalar(found->second, dimensions[1]))
            coordinated_owners.insert(intent.slab_id);
    }
    for (const auto& target : targets)
        (void)make_slab(native_slab(resolve_vertical_placement(result, result.at(target))));
    coordinate_hosted_elevation(source, result, coordinated_owners);
    return result;
}

std::optional<SlabProfileEditIntent> capture_slab_profile_edit(
    const Entity& original, const Entity& candidate, const std::optional<SlabProfileEditIntent>& authored) {
    validate_slab_profile_source_entity(original);
    validate_slab_profile_source_entity(candidate);
    if (authored) {
        const auto expected = replay_slab_profile_entity(original, *authored);
        if (!exact(expected, candidate)) invalid("Slab profile candidate differs from independent authored replay");
        return exact(expected, original) ? std::nullopt : authored;
    }
    if (exact(original, candidate)) return std::nullopt;
    const auto before = slab(original), after = slab(candidate);
    if (before.layers.size() != after.layers.size()) invalid("Slab profile capture cannot change layer inventory");
    SlabProfileEditIntent intent;
    intent.slab_id = original.id;
    for (const auto& dimension : dimensions) {
        const auto old_value = scalar(original, dimension), new_value = scalar(candidate, dimension);
        if (old_value == new_value) continue;
        intent.*(dimension.intent) = captured_quantity(original, candidate,
            {"/" + std::string(dimension.canonical), "/" + std::string(dimension.alias)}, new_value, true);
    }
    bool layers_changed = false;
    for (std::size_t i = 0; i < before.layers.size(); ++i) {
        if (before.layers[i].id != after.layers[i].id) invalid("Slab profile capture must retain layer identities and order");
        layers_changed = layers_changed || before.layers[i].thickness != after.layers[i].thickness;
    }
    if (layers_changed) {
        intent.layer_thicknesses.emplace();
        for (std::size_t i = 0; i < before.layers.size(); ++i) {
            const auto& retained = before.layers[i];
            const auto metres = after.layers[i].thickness;
            if (retained.thickness == metres) {
                intent.layer_thicknesses->push_back({retained.id, Quantity{}, true});
                continue;
            }
            intent.layer_thicknesses->push_back({retained.id, captured_quantity(original, candidate,
                {"/layers/" + std::to_string(i) + "/thickness_m"}, metres, true)});
        }
    }
    const auto expected = any(intent) ? replay_slab_profile_entity(original, intent) : original;
    if (!exact(expected, candidate)) invalid("Slab profile candidate differs from independent typed replay");
    if (exact(expected, original)) return std::nullopt;
    return intent;
}
} // namespace sketch
