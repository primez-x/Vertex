#include "sketch/stair_compound_edit.hpp"

#include "sketch/project_organization.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Names = std::set<std::string, std::less<>>;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Stair compound edit: " + reason);
}
const Json* field(const Json& value, const std::string& key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
double scalar(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) invalid("pose must be finite");
    return value.get<double>();
}
Vec3 point(const Json& value) {
    if (!value.is_array() || value.size() != 3) invalid("base must contain three coordinates");
    return {scalar(value.at(0)), scalar(value.at(1)), scalar(value.at(2))};
}
// Bounds precede JSON serialization and all nested profile/transform codecs.
void nodes(const Json& value, std::size_t& count, std::size_t& bytes, std::size_t depth = 0) {
    if (++count > 65536 || depth > 32) invalid("proof node/nesting budget exceeded");
    if (value.is_binary() || value.is_discarded()) invalid("proof contains non-JSON data");
    if (value.is_number_float()) (void)scalar(value);
    const auto text = [&](const std::string& raw) {
        if (raw.size() > 16384 || raw.size() > proof_limit - bytes) invalid("proof string budget exceeded");
        bytes += raw.size();
    };
    if (value.is_string()) text(value.get_ref<const std::string&>());
    if ((value.is_array() || value.is_object()) && value.size() > 4096) invalid("proof collection budget exceeded");
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        text(key); nodes(child, count, bytes, depth + 1);
    }
    else if (value.is_array()) for (const auto& child : value) nodes(child, count, bytes, depth + 1);
}
std::size_t budget(const Json& value) {
    std::size_t count = 0, bytes = 0;
    nodes(value, count, bytes);
    const auto size = value.dump().size();
    if (size > proof_limit) invalid("proof byte budget exceeded");
    return size;
}
void rigid(const StairCompoundEditIntent& intent) {
    if (intent.profile_edit.object_id != intent.placement_edit.object_id) invalid("typed lanes have different owners");
    const auto& movement = intent.placement_edit.transform;
    if (movement.scale != 1.0 || movement.flip_horizontal || movement.flip_vertical)
        invalid("object placement requires rigid yaw and translation");
    (void)architectural_group_assembly_transform(movement);
}
Json wire(const StairCompoundEditIntent& intent) {
    const auto& p = intent.profile_edit;
    const auto& t = intent.placement_edit.transform;
    Json placement{{"version", intent.placement_edit.quantity_entries.is_null() ? 1 : 2},
        {"object_id", intent.placement_edit.object_id}, {"transform", {
            {"pivot_m", {t.pivot.x, t.pivot.y, t.pivot.z}}, {"offset_m", {t.offset.x, t.offset.y, t.offset.z}},
            {"rotation_z_radians", t.rotation_z_radians}, {"uniform_scale", t.scale},
            {"flip_horizontal", t.flip_horizontal}, {"flip_vertical", t.flip_vertical}}}};
    if (!intent.placement_edit.quantity_entries.is_null()) placement["quantity_entries"] = intent.placement_edit.quantity_entries;
    Json result{{"version", intent.coordinate_profile_hosted_geometry ? 2 : 1}, {"profile_edit", {{"version", 1}, {"object_id", p.object_id},
        {"profile_fields", p.profile_fields}, {"quantity_entries", p.quantity_entries}}},
        {"placement_edit", std::move(placement)}};
    if (intent.coordinate_profile_hosted_geometry) result["coordinate_profile_hosted_geometry"] = true;
    return result;
}
bool coordinate_pointer(std::string_view key) {
    return key == "/base_position_m/0" || key == "/base_position_m/1" || key == "/base_position_m/2";
}
Json coordinates(const Json* entries) {
    auto result = Json::object();
    if (entries) for (const auto& [key, raw] : entries->items())
        if (coordinate_pointer(key)) result[key] = raw;
    return result;
}
Json noncoordinates(const Json* entries) {
    auto result = entries ? *entries : Json::object();
    for (const auto* key : {"/base_position_m/0", "/base_position_m/1", "/base_position_m/2"}) result.erase(key);
    return result;
}
// Coordinate receipts belong to the final pose lane. Restore only those keys;
// preserve every entered nonplacement value and opaque pointer verbatim.
Entity neutralized(const Entity& original, const Entity& edited) {
    auto result = edited;
    for (const auto* key : {"base_position_m", "orientation_rad"})
        if (const auto raw=field(original.properties,key)) result.properties[key]=*raw;
        else result.properties.erase(key);
    const auto old = field(original.properties, "quantity_entries");
    const auto current = field(edited.properties, "quantity_entries");
    if (!old && !current) return result;
    auto restored = current ? *current : Json::object();
    for (const auto* key : {"/base_position_m/0", "/base_position_m/1", "/base_position_m/2"}) {
        restored.erase(key);
        if (const auto raw = old ? field(*old, key) : nullptr) restored[key] = *raw;
    }
    // Do not manufacture an empty profile receipt envelope when its only new
    // inputs were coordinates. Explicit empty input maps remain meaningful.
    if (!old && restored.empty() && current && !current->empty()) result.properties.erase("quantity_entries");
    else if (!current && restored.empty()) result.properties.erase("quantity_entries");
    else result.properties["quantity_entries"] = std::move(restored);
    return result;
}
void neutral_pose(const Entity& actual, const StairObjectEditIntent& profile) {
    for (const auto* key : {"base_position_m", "orientation_rad"}) {
        const auto old = field(actual.properties, key), requested = field(profile.profile_fields, key);
        if (!requested || (old ? *old != *requested : !requested->is_null())) invalid("profile lane changed actual source pose");
    }
    if (!profile.quantity_entries.is_null() && !exact(coordinates(field(actual.properties, "quantity_entries")),
        coordinates(&profile.quantity_entries))) invalid("profile lane changed coordinate input authority");
}
bool placement_required(const Entity& profile, const StairTransformIntent& placement) {
    if (!placement.quantity_entries.is_null() &&
        !exact(noncoordinates(field(profile.properties, "quantity_entries")), noncoordinates(&placement.quantity_entries)))
        invalid("placement lane changed nonplacement entered-input authority");
    if (architectural_group_assembly_transform(placement.transform) != AssemblyTransform{}) return true;
    return !placement.quantity_entries.is_null() &&
        !exact(coordinates(field(profile.properties, "quantity_entries")), coordinates(&placement.quantity_entries));
}
Entity resolved_anchor(const Entities& profiles, const Entity& entity) {
    const auto host=entity.type=="railing"?field(entity.properties,"host"):nullptr;
    const auto stair=host?field(*host,"stair_id"):nullptr;
    return resolve_vertical_placement(profiles,stair && stair->is_string() ?
        profiles.at(stair->get_ref<const std::string&>()) : entity);
}
void anchored(const Entities& profiles, const StairCompoundEditIntent& intent) {
    const auto resolved = resolved_anchor(profiles,profiles.at(intent.profile_edit.object_id));
    const auto base = point(resolved.properties.at("base_position_m"));
    const auto& pivot = intent.placement_edit.transform.pivot;
    if (pivot.x != base.x || pivot.y != base.y || pivot.z != base.z)
        invalid("placement pivot differs from resolved actual profile base");
}
// Compare against the already admitted complete edit. Never repair a replay
// using candidate bytes. Only physical transform roundoff/whole-turn yaw differs.
void agrees(const Entity& wanted, const Entity& replayed) {
    if (!wanted.properties.contains("base_position_m")) {
        if (!exact(wanted,replayed)) invalid("hosted compound replay differs from its admitted profile");
        return;
    }
    const auto desired_base = point(wanted.properties.at("base_position_m"));
    const auto actual_base = point(replayed.properties.at("base_position_m"));
    const auto close = [](double a, double b) {
        return std::abs(a - b) <= 1e-12 * std::max({1.0, std::abs(a), std::abs(b)});
    };
    if (!close(desired_base.x, actual_base.x) || !close(desired_base.y, actual_base.y) ||
        !close(desired_base.z, actual_base.z)) invalid("captured placement cannot reproduce the edited base");
    const auto desired_angle = scalar(wanted.properties.at("orientation_rad"));
    const auto actual_angle = scalar(replayed.properties.at("orientation_rad"));
    if (!close(std::remainder(desired_angle - actual_angle, 2 * std::numbers::pi), 0.0))
        invalid("captured placement cannot reproduce the edited orientation");
    auto left = wanted, right = replayed;
    for (auto* entity : {&left, &right}) {
        entity->properties.erase("base_position_m");
        entity->properties.erase("orientation_rad");
    }
    if (!exact(left, right)) invalid("compound replay differs from complete admitted edit authority");
}
} // namespace

Json encode_stair_compound_edit_intent(const StairCompoundEditIntent& intent) try {
    // Bound the whole wire before nested encoders invoke family codecs.
    (void)budget(wire(intent));
    rigid(intent);
    Json result{{"version", intent.coordinate_profile_hosted_geometry ? 2 : 1}, {"profile_edit", encode_stair_object_edit_intent(intent.profile_edit)},
        {"placement_edit", encode_stair_transform_intent(intent.placement_edit)}};
    if (intent.coordinate_profile_hosted_geometry) result["coordinate_profile_hosted_geometry"] = true;
    (void)budget(result);
    return result;
} catch (const Json::exception& error) {
    invalid(std::string("malformed intent: ") + error.what());
}
StairCompoundEditIntent decode_stair_compound_edit_intent(const Json& value) try {
    (void)budget(value);
    const auto version = field(value, "version");
    if (!version || !version->is_number_integer() || (*version != 1 && *version != 2))
        invalid("proof version is unsupported");
    const bool coordinated = *version == 2;
    if (!value.is_object() || value.size() != (coordinated ? 4 : 3) || !value.contains("version") ||
        !value.contains("profile_edit") || !value.contains("placement_edit") ||
        (coordinated && (!value.contains("coordinate_profile_hosted_geometry") ||
            !value.at("coordinate_profile_hosted_geometry").is_boolean() ||
            value.at("coordinate_profile_hosted_geometry") != true)))
        invalid("proof fields/version are not closed");
    StairCompoundEditIntent result{decode_stair_object_edit_intent(value.at("profile_edit")),
        decode_stair_transform_intent(value.at("placement_edit")), coordinated};
    (void)encode_stair_compound_edit_intent(result);
    return result;
} catch (const Json::exception& error) {
    invalid(std::string("malformed intent: ") + error.what());
}
Entities replay_stair_compound_profile_entities(const Entities& actual,
    const std::vector<StairCompoundEditIntent>& intents) try {
    if (intents.size() > maximum_architectural_group_targets) invalid("target budget exceeded");
    if (intents.empty()) return actual;
    stair_transform_detail::source_bounds(actual);
    Names targets;
    std::vector<StairObjectEditIntent> profiles;
    profiles.reserve(intents.size());
    std::size_t bytes = 0;
    // The complete aggregate is bounded before any intent's family codec runs.
    for (const auto& intent : intents) {
        const auto size = budget(wire(intent));
        if (size > proof_limit - bytes) invalid("batch proof byte budget exceeded");
        bytes += size;
    }
    for (const auto& intent : intents) {
        (void)encode_stair_compound_edit_intent(intent);
        const auto& id = intent.profile_edit.object_id;
        if (!targets.insert(id).second) invalid("duplicate actual owners");
        const auto found = actual.find(id);
        if (found == actual.end()) invalid("actual owner is missing");
        neutral_pose(found->second, intent.profile_edit);
        profiles.push_back(intent.profile_edit);
    }
    auto profiled = replay_stair_object_edit_entities(actual, profiles);
    std::vector<std::string> coordinated;
    for (const auto& intent : intents) if (intent.coordinate_profile_hosted_geometry)
        coordinated.push_back(intent.profile_edit.object_id);
    return stair_transform_detail::coordinate_profile_hosted_geometry(actual, std::move(profiled), coordinated);
} catch (const Standard_Failure& error) {
    const auto message = error.GetMessageString();
    invalid(std::string("native profile admission failed: ") + (message ? message : "Open CASCADE failure"));
} catch (const Json::exception& error) {
    invalid(std::string("malformed actual profile source: ") + error.what());
}
Entities replay_stair_compound_edit_entities(const Entities& actual,
    const std::vector<StairCompoundEditIntent>& intents) try {
    if (intents.empty()) return actual;
    auto profiled = replay_stair_compound_profile_entities(actual, intents);
    std::vector<StairTransformIntent> placements;
    placements.reserve(intents.size());
    Names moving, selected_hosts;
    for (const auto& intent : intents) {
        anchored(profiled, intent);
        const auto& entity = profiled.at(intent.profile_edit.object_id);
        if (!placement_required(entity, intent.placement_edit)) continue;
        moving.insert(entity.id);
        // A selected host with identity placement is still explicit selection
        // authority for an identity hosted-rail coordinate receipt edit.
        const auto host = entity.type=="railing" ? field(entity.properties, "host") : nullptr;
        const auto id = host ? field(*host, "stair_id") : nullptr;
        if (id && id->is_string()) selected_hosts.insert(id->get<std::string>());
    }
    for (const auto& intent : intents) if (moving.contains(intent.profile_edit.object_id) ||
        selected_hosts.contains(intent.profile_edit.object_id)) placements.push_back(intent.placement_edit);
    if (placements.empty()) return profiled;
    return replay_stair_transform_entities(profiled, placements);
} catch (const Standard_Failure& error) {
    const auto message = error.GetMessageString();
    invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
} catch (const Json::exception& error) {
    invalid(std::string("malformed actual source: ") + error.what());
}
std::vector<StairCompoundEditIntent> capture_stair_compound_edits(const Entities& actual,
    const std::vector<Entity>& edited) try {
    if (edited.size()>maximum_architectural_group_targets) invalid("capture target budget exceeded");
    stair_transform_detail::source_bounds(actual);
    Entities candidates;
    for (const auto& entity:edited)
        if (!candidates.emplace(entity.id,entity).second) invalid("duplicate capture owners");
    stair_transform_detail::source_bounds(candidates);
    std::vector<StairObjectEditIntent> full;
    for (const auto& entity:edited) {
        const auto found=actual.find(entity.id);
        if (found==actual.end()) invalid("capture actual owner is missing");
        if (found->second.type!="stair" && found->second.type!="railing")
            invalid("capture owner is not a stair or railing");
        if (const auto intent=capture_stair_object_edit(found->second,entity)) full.push_back(*intent);
    }
    if (full.empty()) return {};
    const auto admitted=replay_stair_object_edit_entities(actual,full);
    std::vector<StairObjectEditIntent> profiles;
    for (const auto& edit:full) {
        const auto& original=actual.at(edit.object_id);
        const auto neutral=neutralized(original,admitted.at(edit.object_id));
        auto profile=edit;
        for (const auto* key:{"base_position_m","orientation_rad"})
            profile.profile_fields[key]=original.properties.contains(key)?original.properties.at(key):Json(nullptr);
        profile.quantity_entries=field(neutral.properties,"quantity_entries")?neutral.properties.at("quantity_entries"):Json(nullptr);
        if (const auto captured=capture_stair_object_edit(original,neutral)) profile=*captured;
        profiles.push_back(std::move(profile));
    }
    const auto profiled=replay_stair_object_edit_entities(actual,profiles);
    std::vector<StairCompoundEditIntent> result;
    for (auto& profile:profiles) {
        const auto& intermediate=profiled.at(profile.object_id);
        const auto& wanted=admitted.at(profile.object_id);
        const auto effective=resolved_anchor(profiled,intermediate);
        StairTransformIntent placement{profile.object_id,{point(effective.properties.at("base_position_m"))}};
        if (const auto base=field(intermediate.properties,"base_position_m");base && base->is_array()) {
            const auto current=point(*base),desired=point(wanted.properties.at("base_position_m"));
            placement.transform.offset={desired.x-current.x,desired.y-current.y,desired.z-current.z};
            placement.transform.rotation_z_radians=scalar(wanted.properties.at("orientation_rad"))-
                scalar(intermediate.properties.at("orientation_rad"));
        }
        if (const auto entered=field(wanted.properties,"quantity_entries")) placement.quantity_entries=*entered;
        result.push_back({std::move(profile),std::move(placement),true});
    }
    const auto replayed=replay_stair_compound_edit_entities(actual,result);
    for (const auto& intent:result) agrees(admitted.at(intent.profile_edit.object_id),replayed.at(intent.profile_edit.object_id));
    std::erase_if(result,[&](const auto& intent) {
        const auto& id=intent.profile_edit.object_id;
        return exact(actual.at(id),replayed.at(id));
    });
    return result;
} catch (const Standard_Failure& error) {
    const auto message = error.GetMessageString();
    invalid(std::string("native capture admission failed: ") + (message ? message : "Open CASCADE failure"));
} catch (const Json::exception& error) {
    invalid(std::string("malformed capture: ") + error.what());
}
std::optional<StairCompoundEditIntent> capture_stair_compound_edit(const Entities& actual,
    const Entity& original, const Entity& edited) {
    const auto found=actual.find(original.id);
    if (found==actual.end() || !exact(found->second,original)) invalid("capture original is not the exact actual owner");
    if (edited.id!=original.id) invalid("capture edit names a different actual owner");
    auto intents=capture_stair_compound_edits(actual,{edited});
    if (intents.empty()) return std::nullopt;
    return std::move(intents.front());
}
std::optional<StairCompoundEditIntent> capture_stair_compound_edit(const Entities& actual,
    const std::string& object_id, const Entity& edited) {
    stair_transform_detail::source_bounds(actual);
    const auto found = actual.find(object_id);
    if (found == actual.end()) invalid("capture owner is missing");
    return capture_stair_compound_edit(actual, found->second, edited);
}
} // namespace sketch
