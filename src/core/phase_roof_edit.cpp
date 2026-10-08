#include "sketch/phase_roof_edit.hpp"

#include "sketch/architecture.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>

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
void admit_cohorts(const Entities& entities, const Ids& targets, const Ids& resize_targets) {
    const auto scope = constraint_phase_scope(entities);
    for (const auto& id : targets)
        (void)make_roof_shape(decode_roof_entity(resolve_vertical_placement(entities, entities.at(id))));
    Ids joined;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        for (const auto& member : join.roof_ids) {
            const auto found = entities.find(member);
            if (found == entities.end() || found->second.type != "roof" || found->second.id != member)
                invalid("Roof join references a missing or inconsistent roof owner");
            if (!joined.insert(member).second) invalid("A retained roof belongs to more than one roof join");
        }
        if (std::none_of(join.roof_ids.begin(), join.roof_ids.end(), [&](const auto& member) { return targets.contains(member); })) continue;
        const bool resize = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return resize_targets.contains(member); });
        if (scope.inactive_owner_ids.contains(id)) {
            if (resize) invalid("An affected roof resize belongs to an inactive join");
            continue;
        }
        std::vector<TopoDS_Shape> members;
        for (const auto& member : join.roof_ids) {
            if (scope.inactive_owner_ids.contains(member)) invalid("An active roof join contains an inactive roof");
            if (resize) validate_roof_plan_resize_source_entity(entities.at(member));
            else validate_roof_profile_source_entity(entities.at(member));
            members.push_back(make_roof_shape(decode_roof_entity(resolve_vertical_placement(entities, entities.at(member)))));
        }
        (void)make_roof_join(join, members);
    }
}
} // namespace

nlohmann::json encode_roof_edit_intent(const RoofEditIntent& intent) {
    if (!identity(intent.roof_id) || (!intent.profile && !intent.openings && !intent.pose && !intent.form && !intent.transform && !intent.resize))
        invalid("Roof edit requires a valid owner and a typed component");
    if (intent.form && intent.profile) invalid("Roof conversion cannot also author a profile component");
    if (intent.transform && (intent.profile || intent.openings || intent.pose || intent.form || intent.resize))
        invalid("Roof rigid transform cannot borrow another component's edit authority");
    if (intent.resize && (intent.profile || intent.openings || intent.pose || intent.form || intent.transform))
        invalid("Roof plan resize cannot borrow another component's edit authority");
    Json result{{"version", 1}, {"roof_id", intent.roof_id}, {"profile", nullptr}, {"openings", nullptr}, {"pose", nullptr}};
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
    if (result.dump().size() > proof_limit) invalid("Roof edit proof byte budget exceeded");
    return result;
}
RoofEditIntent decode_roof_edit_intent(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("version") ||
        !value.at("version").is_number_integer() ||
        !value.contains("roof_id") || !value.contains("profile") || !value.contains("openings") || !value.contains("pose") ||
        value.dump().size() > proof_limit) invalid("Roof edit fields or proof budget are invalid");
    const bool conversion = value.at("version") == 2;
    const bool rigid_transform = value.at("version") == 3;
    const bool plan_resize = value.at("version") == 4;
    if (plan_resize) {
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
    (void)new_roof_opening_identity_ids(source, roof_edit_opening_intents(intents));
    const auto scope = constraint_phase_scope(source);
    Ids targets, resize_targets;
    std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto count = encode_roof_edit_intent(intent).dump().size();
        if (count > proof_limit - bytes) invalid("Roof edit batch proof byte budget exceeded");
        bytes += count;
        if (!targets.insert(intent.roof_id).second) invalid("Roof edit contains duplicate targets");
        if (intent.resize) resize_targets.insert(intent.roof_id);
        const auto found = source.find(intent.roof_id);
        if (found == source.end() || found->second.id != found->first || found->second.type != "roof")
            invalid("Roof edit target is missing or inconsistent");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof edit target is inactive in the saved design");
    }
    if (resize_targets.size() > maximum_architectural_group_targets)
        invalid("Roof resize target budget exceeded");
    admit_cohorts(source, targets, resize_targets);
    auto result = source;
    std::size_t resize_archive_bytes = 0;
    for (const auto& intent : intents) {
        auto roof = replay_roof_edit_entity(source.at(intent.roof_id), intent);
        if (intent.resize && roof.extensions.contains(std::string(roof_plan_resize_derivations_key))) {
            const auto bytes = roof.extensions.at(std::string(roof_plan_resize_derivations_key)).dump().size();
            if (bytes > proof_limit - resize_archive_bytes) invalid("Roof resize batch archive budget exceeded");
            resize_archive_bytes += bytes;
        }
        result.at(intent.roof_id) = std::move(roof);
    }
    admit_cohorts(result, targets, resize_targets);
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
