#include "sketch/phase_roof_edit.hpp"

#include "sketch/architecture.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_phase_ownership.hpp"
#include "sketch/site_frame.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
const Json* field(const Json& object, const std::string& name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}
bool same(const Json* a, const Json* b) { return (!a && !b) || (a && b && exact(*a, *b)); }
bool identity(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    });
}
Ids changed_keys(const Json& before, const Json& after) {
    if (!before.is_object() || !after.is_object()) invalid("Roof component data must be objects");
    Ids result;
    for (const auto& [key, value] : before.items()) {
        (void)value;
        if (!same(field(before, key), field(after, key))) result.insert(key);
    }
    for (const auto& [key, value] : after.items()) {
        (void)value;
        if (!same(field(before, key), field(after, key))) result.insert(key);
    }
    return result;
}
void transfer(Json& target, const Json& staged, const std::string& key) {
    if (const auto value = field(staged, key)) target[key] = *value;
    else target.erase(key);
}
enum class Component { profile, openings, pose, form };
bool owns_property(Component component, const std::string& key) {
    switch (component) {
    case Component::form:
        if (key == "form") return true;
        [[fallthrough]];
    case Component::profile:
        return key == "run_m" || key == "length_m" || key == "span_m" || key == "rise_m" ||
            key == "overhang_m" || key == "thickness_m" || key == "pitch_rad";
    case Component::openings: return key == "version" || key == "roof_openings";
    case Component::pose: return key == "base_position_m" || key == "orientation_rad";
    }
    return false;
}
bool owns_receipt(Component component, const std::string& pointer) {
    if (component == Component::profile || component == Component::form)
        return pointer == "/run_m" || pointer == "/length_m" || pointer == "/span_m" ||
            pointer == "/rise_m" || pointer == "/overhang_m" || pointer == "/thickness_m";
    if (component == Component::pose)
        return pointer == "/base_position_m/0" || pointer == "/base_position_m/1" || pointer == "/base_position_m/2";
    return std::string_view(pointer).starts_with("/roof_openings/");
}
void merge_receipts(Entity& result, const Entity& source, const Entity& staged, Component component) {
    const Json empty = Json::object();
    const auto before = field(source.properties, "quantity_entries"), after = field(staged.properties, "quantity_entries");
    const auto& before_values = before ? *before : empty;
    const auto& after_values = after ? *after : empty;
    const auto differences = changed_keys(before_values, after_values);
    for (const auto& pointer : differences) {
        if (!owns_receipt(component, pointer)) invalid("Roof component escaped its quantity receipt ownership");
        if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
        transfer(result.properties.at("quantity_entries"), after_values, pointer);
    }
    if (result.properties.contains("quantity_entries") && result.properties.at("quantity_entries").empty() &&
        !source.properties.contains("quantity_entries")) result.properties.erase("quantity_entries");
}
void merge_component(Entity& result, const Entity& source, const Entity& staged, Component component) {
    if (staged.id != source.id || staged.type != source.type || staged.required != source.required)
        invalid("Roof component cannot change its owner envelope");
    for (const auto& key : changed_keys(source.properties, staged.properties)) {
        if (key == "quantity_entries") { merge_receipts(result, source, staged, component); continue; }
        if (!owns_property(component, key)) invalid("Roof component escaped its property ownership");
        transfer(result.properties, staged.properties, key);
    }
    for (const auto& key : changed_keys(source.extensions, staged.extensions)) {
        if (component != Component::openings || key != "roof_opening_input")
            invalid("Roof component escaped its extension ownership");
        transfer(result.extensions, staged.extensions, key);
    }
}
void admit_cohorts(const Entities& entities, const Ids& targets, const Ids& resize_targets, const Ids& uniform_targets) {
    const auto qualified_cohorts = phase_qualified_roof_join_cohort_ids(entities);
    const auto scope = constraint_phase_scope(entities);
    for (const auto& id : targets) {
        if (uniform_targets.contains(id)) validate_roof_uniform_transform_source_entity(entities.at(id));
        (void)make_roof_shape(decode_roof_entity(resolve_vertical_placement(entities, entities.at(id))));
    }
    Ids joined;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        // Actual ownership was admitted above. A preserved alternative's join
        // cannot constrain the active geometry of its shared ordinary member.
        if (scope.inactive_owner_ids.contains(id) && qualified_cohorts.contains(id)) continue;
        for (const auto& member : join.roof_ids) {
            const auto found = entities.find(member);
            if (found == entities.end() || found->second.type != "roof" || found->second.id != member)
                invalid("Roof join references a missing or inconsistent roof owner");
            if (!joined.insert(member).second) invalid("A retained roof belongs to more than one roof join");
        }
        if (std::none_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& member) { return targets.contains(member); })) continue;
        const bool resize = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return resize_targets.contains(member); });
        const bool uniform = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return uniform_targets.contains(member); });
        if (scope.inactive_owner_ids.contains(id)) {
            if (resize) invalid("An affected roof resize belongs to an inactive join");
            continue;
        }
        std::vector<TopoDS_Shape> members;
        for (const auto& member : join.roof_ids) {
            if (scope.inactive_owner_ids.contains(member)) invalid("An active roof join contains an inactive roof");
            if (uniform) validate_roof_uniform_transform_source_entity(entities.at(member));
            else if (resize) validate_roof_plan_resize_source_entity(entities.at(member));
            else validate_roof_profile_source_entity(entities.at(member));
            members.push_back(make_roof_shape(decode_roof_entity(resolve_vertical_placement(entities, entities.at(member)))));
        }
        (void)make_roof_join(join, members);
    }
}
// Bound discovery before recursive inspection or catalog expansion. This new
// boundary applies only to opted-in motion; historical proofs keep their exact
// source admission and replay semantics.
struct HostedSourceBudget {
    std::size_t nodes{}, bytes{};
    static constexpr std::size_t inventory_limit = 65536;
    void text(const std::string& value) {
        if (value.size() > 64 * 1024 * 1024 - bytes) invalid("Roof hosted source byte budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) invalid("Roof hosted source structural budget exceeded");
        if ((value.is_object() || value.is_array()) && value.size() > inventory_limit)
            invalid("Roof hosted source collection budget exceeded");
        if (value.is_binary()) {
            if (value.get_binary().size() > 64 * 1024 * 1024 - bytes)
                invalid("Roof hosted source binary budget exceeded");
            bytes += value.get_binary().size();
        }
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
};
void hosted_source_bounds(const Entities& source) {
    if (source.size() > HostedSourceBudget::inventory_limit) invalid("Roof hosted source entity budget exceeded");
    HostedSourceBudget budget;
    for (const auto& [id, entity] : source) if (entity.type == "assembly_model") {
        if (id != entity.id || !identity(id) || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("Roof hosted catalog requires an actual identified envelope");
        budget.text(id); budget.read(entity.properties); budget.read(entity.extensions);
    }
}
bool touches_host(const Json& value, const Ids& hosts) {
    if (value.is_string()) return hosts.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key, child] : value.items())
            if (hosts.contains(key) || touches_host(child, hosts)) return true;
    } else if (value.is_array()) for (const auto& child : value) if (touches_host(child, hosts)) return true;
    return false;
}
bool host_binding(const Json& value, const Ids& hosts) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key == "host_entity_id" && touches_host(child, hosts)) return true;
            if (host_binding(child, hosts)) return true;
        }
    } else if (value.is_array()) for (const auto& child : value) if (host_binding(child, hosts)) return true;
    return false;
}
bool supported_catalog(const Json& raw) {
    const auto schema = field(raw, "schema");
    if (!schema || !schema->is_string()) return false;
    for (int version = 1; version <= 7; ++version)
        if (*schema == "sketch.assemblies.v" + std::to_string(version)) return true;
    return false;
}
bool affected_catalog(const Entity& catalog, const Ids& hosts) {
    if (host_binding(catalog.properties, hosts) || host_binding(catalog.extensions, hosts)) return true;
    const auto raw = field(catalog.properties, "model");
    if (!raw || !supported_catalog(*raw)) return touches_host(catalog.properties, hosts);
    const auto rows = field(*raw, "instances");
    if (!rows || !rows->is_array()) return rows && touches_host(*rows, hosts);
    for (const auto& row : *rows) {
        if (!row.is_object() && touches_host(row, hosts)) return true;
        const auto placement = field(row, "placement");
        if (placement && !placement->is_null() && touches_host(*placement, hosts)) return true;
    }
    return false;
}
void admit_catalog_context(const Entity& catalog, const Entities& source, const ConstraintPhaseScope& scope) {
    if (scope.inactive_owner_ids.contains(catalog.id)) invalid("Roof hosted catalog is inactive in the saved design");
    struct Binding { const char* key; const char* type; };
    constexpr Binding bindings[]{{"property_id", "property"}, {"building_id", "building"},
        {"floor_id", "floor"}, {"layer_id", "layer"}, {"wall_id", "wall"}};
    for (const auto& binding : bindings) {
        const auto value = field(catalog.properties, binding.key);
        if (!value) continue;
        if (!value->is_string()) invalid("Roof hosted catalog context is malformed");
        const auto owner = source.find(value->get_ref<const std::string&>());
        if (owner == source.end() || owner->second.id != owner->first || owner->second.type != binding.type)
            invalid("Roof hosted catalog context is unresolved");
    }
}
bool admit_hosted_instance(const AssemblyModel& model, const AssemblyInstance& instance,
    const Entities& entities, AssemblyExpansionBudget& budget) try {
    if (!instance.placement) invalid("Roof hosted instance requires its actual placement");
    const auto& placement = *instance.placement;
    const auto host = entities.find(placement.host_entity_id);
    if (host == entities.end() || host->second.type != "roof" || host->first != host->second.id)
        invalid("Roof hosted instance requires its actual roof owner");
    const auto expansion = model.expand(instance, budget);
    if (!expansion.profiles.empty()) {
        (void)make_assembly_geometry(expansion);
        return true;
    }
    // Legacy catalogs derive the body from the host. Their saved placement is
    // an additional host-space operator, so the shared helper conjugates it.
    (void)transform_assembly_shape(make_roof_shape(decode_roof_entity(resolve_vertical_placement(entities, host->second))),
        {{placement.translation_m.x, placement.translation_m.y, placement.translation_z_m},
            placement.rotation_radians, placement.scale, placement.mirrored_y, placement.vertical_scale});
    return false;
} catch (const Standard_Failure&) {
    invalid("Roof hosted instance failed native geometry admission");
}
ArchitecturalGroupTransform admitted_roof_pose_operation(const Entity& original, const Entity& final) {
    const auto pose = [](const Entity& entity) {
        return std::visit([](const auto& roof) {
            return std::pair{roof.base_position, roof.orientation_radians};
        }, decode_roof_entity(entity));
    };
    const auto [before, before_yaw] = pose(original);
    const auto [after, after_yaw] = pose(final);
    ArchitecturalGroupTransform operation;
    operation.pivot = before;
    operation.offset = {after.x - before.x, after.y - before.y, after.z - before.z};
    operation.rotation_z_radians = before_yaw == after_yaw ? 0.0 :
        std::remainder(after_yaw - before_yaw, 2.0 * std::numbers::pi);
    // A profile/plan resize has no hosted stretch authority. Only the final
    // independently admitted base and yaw determine this rigid pose delta.
    (void)architectural_group_assembly_transform(operation);
    return operation;
}
void coordinate_hosted_catalogs(const Entities& source, Entities& result,
    const std::map<std::string, ArchitecturalGroupTransform, std::less<>>& operations,
    const ConstraintPhaseScope& scope) {
    if (operations.empty()) return;
    Ids hosts;
    for (const auto& [id, operation] : operations) { (void)operation; hosts.insert(id); }
    std::size_t inventory = 0;
    AssemblyExpansionBudget source_budget, candidate_budget;
    for (const auto& [id, catalog] : source) {
        if (catalog.type != "assembly_model" || !affected_catalog(catalog, hosts)) continue;
        admit_catalog_context(catalog, source, scope);
        const auto raw = field(catalog.properties, "model");
        if (!raw || !supported_catalog(*raw)) invalid("Roof hosted catalog schema is unsupported");
        for (const auto* key : {"materials", "types", "instances"}) {
            const auto rows = field(*raw, key);
            if (!rows || !rows->is_array() || rows->size() > HostedSourceBudget::inventory_limit - inventory)
                invalid("Roof hosted catalog inventory budget exceeded");
            inventory += rows->size();
        }
        const auto model = AssemblyModel::from_json(*raw);
        // Only actual placement rows establish ownership. Explicit aliases
        // elsewhere cannot borrow this authority; opaque unrelated data stays.
        auto remainder = catalog;
        for (auto& row : remainder.properties.at("model").at("instances"))
            if (row.contains("placement") && row.at("placement").is_object())
                row.at("placement").erase("host_entity_id");
        if (host_binding(remainder.properties, hosts) || host_binding(remainder.extensions, hosts))
            invalid("Roof hosted catalog contains an unsupported host binding");
        Ids affected;
        std::map<std::string, AssemblyTransform, std::less<>> transforms;
        for (const auto& instance : model.instances()) {
            if (!instance.placement || !hosts.contains(instance.placement->host_entity_id)) continue;
            const auto& host = instance.placement->host_entity_id;
            const bool type_owned = admit_hosted_instance(model, instance, source, source_budget);
            auto transform = architectural_group_assembly_transform(operations.at(host));
            if (type_owned) {
                // Physical roof edits use the authored frame, while expanded
                // profiles publish in Site world. Datum compensation stays
                // within roof staging and never enters this world operator.
                const auto frame = resolve_site_presentation(source, host).forward;
                transform = conjugate_assembly_transform_through_rigid_frame(transform,
                    {{frame.translation_m.x, frame.translation_m.y, frame.translation_m.z},
                        frame.rotation_radians, 1.0, false});
            }
            if (!transforms.emplace(instance.id, transform).second)
                invalid("Roof hosted instance has incoherent transform ownership");
            affected.insert(instance.id);
        }
        if (affected.empty()) invalid("Roof hosted binding is outside an actual supported instance");
        auto& after = result.at(id);
        after.properties["model"] = transform_hosted_assembly_model(*raw, transforms, true);
        const auto candidate = AssemblyModel::from_json(after.properties.at("model"));
        for (const auto& instance : candidate.instances()) if (affected.contains(instance.id))
            (void)admit_hosted_instance(candidate, instance, result, candidate_budget);
    }
}
} // namespace

nlohmann::json encode_roof_edit_intent(const RoofEditIntent& intent) {
    if (!identity(intent.roof_id) || (!intent.profile && !intent.openings && !intent.pose && !intent.form && !intent.transform && !intent.resize && !intent.uniform_transform))
        invalid("Roof edit requires a valid owner and a typed component");
    if (intent.form && intent.profile) invalid("Roof conversion cannot also author a profile component");
    if (intent.transform && (intent.profile || intent.openings || intent.pose || intent.form || intent.resize || intent.uniform_transform))
        invalid("Roof rigid transform cannot borrow another component's edit authority");
    if (intent.resize && (intent.profile || intent.openings || intent.pose || intent.form || intent.transform || intent.uniform_transform))
        invalid("Roof plan resize cannot borrow another component's edit authority");
    if (intent.uniform_transform && (intent.profile || intent.openings || intent.pose || intent.form || intent.transform || intent.resize))
        invalid("Roof uniform transform cannot borrow another component's edit authority");
    Json result{{"version", 1}, {"roof_id", intent.roof_id}, {"profile", nullptr}, {"openings", nullptr}, {"pose", nullptr}};
    if (intent.uniform_transform) {
        if (intent.uniform_transform->roof_id != intent.roof_id) invalid("Roof uniform transform component owner differs");
        result["version"] = 5;
        result["form"] = nullptr;
        result["transform"] = nullptr;
        result["resize"] = nullptr;
        result["uniform_transform"] = encode_roof_uniform_transform_intent(*intent.uniform_transform);
    }
    if (intent.resize) {
        if (intent.resize->roof_id != intent.roof_id) invalid("Roof plan resize component owner differs");
        result["version"] = 4;
        result["form"] = nullptr;
        result["transform"] = nullptr;
        result["resize"] = encode_roof_plan_resize_intent(*intent.resize);
    }
    if (intent.transform) {
        if (intent.transform->roof_id != intent.roof_id) invalid("Roof rigid transform component owner differs");
        result["version"] = 3;
        result["form"] = nullptr;
        result["transform"] = encode_roof_rigid_transform_intent(*intent.transform);
    }
    if (intent.form) {
        if (intent.form->roof_id != intent.roof_id) invalid("Roof form component owner differs");
        result["version"] = 2;
        result["form"] = encode_roof_form_edit_intent(*intent.form);
    }
    if (intent.profile) {
        if (intent.profile->roof_id != intent.roof_id) invalid("Roof profile component owner differs");
        result["profile"] = encode_roof_profile_edit_intent(*intent.profile);
    }
    if (intent.openings) {
        if (intent.openings->roof_id != intent.roof_id) invalid("Roof opening component owner differs");
        result["openings"] = encode_roof_opening_edit_intent(*intent.openings);
    }
    if (intent.pose) {
        if (intent.pose->roof_id != intent.roof_id) invalid("Roof pose component owner differs");
        result["pose"] = encode_roof_pose_edit_intent(*intent.pose);
    }
    if (intent.coordinate_world_hosted_geometry) {
        result["version"] = 6;
        for (const auto* key : {"form", "transform", "resize"})
            if (!result.contains(key)) result[key] = nullptr;
        if (!result.contains("uniform_transform")) result["uniform_transform"] = nullptr;
        result["coordinate_world_hosted_geometry"] = true;
    }
    if (result.dump().size() > proof_limit) invalid("Roof edit proof byte budget exceeded");
    return result;
}
RoofEditIntent decode_roof_edit_intent(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("version") ||
        !value.at("version").is_number_integer() ||
        !value.contains("roof_id") || !value.contains("profile") || !value.contains("openings") || !value.contains("pose") ||
        value.dump().size() > proof_limit) invalid("Roof edit fields or proof budget are invalid");
    const bool coordinated = value.at("version") == 6;
    const bool conversion = value.at("version") == 2 ||
        (coordinated && value.contains("form") && !value.at("form").is_null());
    const bool rigid_transform = value.at("version") == 3 ||
        (coordinated && value.contains("transform") && !value.at("transform").is_null());
    const bool plan_resize = value.at("version") == 4 ||
        (coordinated && value.contains("resize") && !value.at("resize").is_null());
    const bool uniform_transform = value.at("version") == 5 ||
        (coordinated && value.contains("uniform_transform") && !value.at("uniform_transform").is_null());
    if (coordinated) {
        if (value.size() != 10 || !value.contains("form") ||
            !value.contains("transform") || !value.contains("uniform_transform") ||
            !value.contains("resize") ||
            !value.contains("coordinate_world_hosted_geometry") ||
            !value.at("coordinate_world_hosted_geometry").is_boolean() ||
            value.at("coordinate_world_hosted_geometry") != true)
            invalid("Roof hosted coordination requires exactly ten version-six fields and a true coordination flag");
    } else if (uniform_transform) {
        if (value.size() != 9 || !value.contains("form") || !value.at("form").is_null() ||
            !value.contains("transform") || !value.at("transform").is_null() ||
            !value.contains("resize") || !value.at("resize").is_null() ||
            !value.contains("uniform_transform") || value.at("uniform_transform").is_null() ||
            !value.at("profile").is_null() || !value.at("openings").is_null() || !value.at("pose").is_null())
            invalid("Roof uniform transform requires exactly nine version-five fields and no other component");
    } else if (plan_resize) {
        if (value.size() != 8 || !value.contains("form") || !value.at("form").is_null() ||
            !value.contains("transform") || !value.at("transform").is_null() ||
            !value.contains("resize") || value.at("resize").is_null() ||
            !value.at("profile").is_null() || !value.at("openings").is_null() || !value.at("pose").is_null())
            invalid("Roof plan resize requires exactly eight version-four fields and no other component");
    } else if (rigid_transform) {
        if (value.size() != 7 || !value.contains("form") || !value.at("form").is_null() ||
            !value.contains("transform") || value.at("transform").is_null() ||
            !value.at("profile").is_null() || !value.at("openings").is_null() || !value.at("pose").is_null())
            invalid("Roof rigid transform requires exactly seven version-three fields and no other component");
    } else if (conversion) {
        if (value.size() != 6 || !value.contains("form") || value.at("form").is_null() || !value.at("profile").is_null())
            invalid("Roof conversion requires exactly six version-two fields with form and no profile");
    } else if (value.at("version") != 1 || value.size() != 5 || value.contains("form")) {
        invalid("Roof edit must contain exactly the five version-one fields");
    }
    RoofEditIntent result;
    result.roof_id = value.at("roof_id").get<std::string>();
    result.coordinate_world_hosted_geometry = coordinated;
    if (uniform_transform) result.uniform_transform = decode_roof_uniform_transform_intent(value.at("uniform_transform"));
    if (plan_resize) result.resize = decode_roof_plan_resize_intent(value.at("resize"));
    if (rigid_transform) result.transform = decode_roof_rigid_transform_intent(value.at("transform"));
    if (conversion) result.form = decode_roof_form_edit_intent(value.at("form"));
    if (!value.at("profile").is_null()) result.profile = decode_roof_profile_edit_intent(value.at("profile"));
    if (!value.at("openings").is_null()) result.openings = decode_roof_opening_edit_intent(value.at("openings"));
    if (!value.at("pose").is_null()) result.pose = decode_roof_pose_edit_intent(value.at("pose"));
    (void)encode_roof_edit_intent(result);
    return result;
}
Entity replay_roof_edit_entity(const Entity& source, const RoofEditIntent& intent) {
    (void)encode_roof_edit_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof edit target differs from its actual source identity");
    if (intent.coordinate_world_hosted_geometry) invalid("Roof hosted transform requires the actual source map");
    if (intent.uniform_transform) invalid("Roof uniform transform requires the actual source map and vertical datum");
    if (intent.resize) return replay_roof_plan_resize_entity(source, *intent.resize);
    if (intent.transform) return replay_roof_rigid_transform_entity(source, *intent.transform);
    validate_roof_profile_source_entity(source);
    auto result = source;
    if (intent.form) merge_component(result, source, stage_roof_form_entity(source, *intent.form), Component::form);
    if (intent.profile) merge_component(result, source, stage_roof_profile_entity(source, *intent.profile), Component::profile);
    if (intent.openings) merge_component(result, source, stage_roof_opening_entity(source, *intent.openings), Component::openings);
    if (intent.pose) merge_component(result, source, stage_roof_pose_entity(source, *intent.pose), Component::pose);
    // All stages read the same admitted source. Only the complete composed roof
    // is required to fit its final envelope; invalid intermediate cuts have no
    // standalone authority and never reach the document.
    validate_roof_profile_source_entity(result);
    return result;
}
std::vector<RoofOpeningEditIntent> roof_edit_opening_intents(const std::vector<RoofEditIntent>& intents) {
    std::vector<RoofOpeningEditIntent> result;
    for (const auto& intent : intents) if (intent.openings) result.push_back(*intent.openings);
    return result;
}
std::map<std::string, Entity, std::less<>> replay_roof_edit_entities(
    const std::map<std::string, Entity, std::less<>>& source, const std::vector<RoofEditIntent>& intents) {
    if (intents.empty()) return source;
    if (intents.size() > collection_limit) invalid("Roof edit target budget exceeded");
    if (std::any_of(intents.begin(), intents.end(), [](const auto& intent) { return intent.coordinate_world_hosted_geometry; }))
        hosted_source_bounds(source);
    (void)new_roof_opening_identity_ids(source, roof_edit_opening_intents(intents));
    const auto scope = constraint_phase_scope(source);
    Ids targets, resize_targets, uniform_targets;
    std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto count = encode_roof_edit_intent(intent).dump().size();
        if (count > proof_limit - bytes) invalid("Roof edit batch proof byte budget exceeded");
        bytes += count;
        if (!targets.insert(intent.roof_id).second) invalid("Roof edit contains duplicate targets");
        if (intent.resize || intent.uniform_transform) resize_targets.insert(intent.roof_id);
        if (intent.uniform_transform) uniform_targets.insert(intent.roof_id);
        const auto found = source.find(intent.roof_id);
        if (found == source.end() || found->second.id != found->first || found->second.type != "roof")
            invalid("Roof edit target is missing or inconsistent");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof edit target is inactive in the saved design");
    }
    if (resize_targets.size() > maximum_architectural_group_targets)
        invalid("Roof resize target budget exceeded");
    admit_cohorts(source, targets, resize_targets, uniform_targets);
    auto result = source;
    std::map<std::string, ArchitecturalGroupTransform, std::less<>> hosted_operations;
    std::size_t resize_archive_bytes = 0, uniform_archive_bytes = 0;
    for (const auto& intent : intents) {
        // Every member reads the complete original map. A transient partially
        // scaled join is never admitted; source/final cohorts bound the batch.
        auto physical_intent = intent;
        physical_intent.coordinate_world_hosted_geometry = false;
        auto roof = intent.uniform_transform
            ? stage_roof_uniform_transform_entity(source, *intent.uniform_transform)
            : replay_roof_edit_entity(source.at(intent.roof_id), physical_intent);
        if (intent.coordinate_world_hosted_geometry && !exact(roof, source.at(intent.roof_id)))
            hosted_operations.emplace(intent.roof_id,
                intent.transform ? intent.transform->transform : intent.uniform_transform
                    ? intent.uniform_transform->transform : admitted_roof_pose_operation(source.at(intent.roof_id), roof));
        if (intent.resize && roof.extensions.contains(std::string(roof_plan_resize_derivations_key))) {
            const auto bytes = roof.extensions.at(std::string(roof_plan_resize_derivations_key)).dump().size();
            if (bytes > proof_limit - resize_archive_bytes) invalid("Roof resize batch archive budget exceeded");
            resize_archive_bytes += bytes;
        }
        if (intent.uniform_transform && roof.extensions.contains(std::string(roof_uniform_transform_derivations_key))) {
            const auto bytes = roof.extensions.at(std::string(roof_uniform_transform_derivations_key)).dump().size();
            if (bytes > proof_limit - uniform_archive_bytes) invalid("Roof uniform transform batch archive budget exceeded");
            uniform_archive_bytes += bytes;
        }
        result.at(intent.roof_id) = std::move(roof);
    }
    admit_cohorts(result, targets, resize_targets, uniform_targets);
    coordinate_hosted_catalogs(source, result, hosted_operations, scope);
    return result;
}
std::optional<RoofEditIntent> capture_roof_edit(const Entity& original, const Entity& candidate) {
    validate_roof_profile_source_entity(original);
    if (exact(original, candidate)) return std::nullopt;
    validate_roof_profile_source_entity(candidate);
    RoofEditIntent intent;
    intent.roof_id = original.id;
    intent.form = infer_roof_form_edit(original, candidate);
    if (!intent.form) intent.profile = infer_roof_profile_edit(original, candidate);
    intent.openings = infer_roof_opening_edit(original, candidate, intent.form.has_value());
    intent.pose = infer_roof_pose_edit(original, candidate, intent.form.has_value());
    auto normalized = normalize_equivalent_roof_opening_inputs(original, candidate, intent.form.has_value());
    normalized = normalize_equivalent_roof_pose_inputs(original, normalized, intent.form.has_value());
    const auto expected = intent.form || intent.profile || intent.openings || intent.pose ? replay_roof_edit_entity(original, intent) : original;
    if (!exact(normalized, expected)) invalid("Roof candidate differs from independent combined typed replay");
    if (exact(expected, original)) return std::nullopt;
    return intent;
}
} // namespace sketch
