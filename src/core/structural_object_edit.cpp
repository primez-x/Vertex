#include "sketch/structural_object_edit.hpp"

#include "sketch/building_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
const Json* field(const Json& value, const std::string& name) {
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
}
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size())
        invalid("Structural edit fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Structural edit field is missing");
}
bool version_one(const Json& value) { return value.is_number_integer() && value == 1; }
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Structural edit identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Structural edit identity is invalid");
    return result;
}
double scalar(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        invalid("Structural edit scalar must be finite");
    return value.get<double>();
}
Vec3 point(const Json& value) {
    if (!value.is_array() || value.size() != 3) invalid("Structural edit coordinate requires three scalars");
    return {scalar(value.at(0)), scalar(value.at(1)), scalar(value.at(2))};
}
void budget_nodes(const Json& value, std::size_t& nodes, std::size_t depth = 0) {
    if (depth > 32 || ++nodes > 65536) invalid("Structural edit proof structural budget exceeded");
    if ((value.is_array() || value.is_object()) && value.size() > collection_limit)
        invalid("Structural edit collection budget exceeded");
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        invalid("Structural edit proof contains a nonfinite scalar");
    if (value.is_string() && value.get_ref<const std::string&>().size() > 16384)
        invalid("Structural edit string budget exceeded");
    if (value.is_object())
        for (const auto& [key, child] : value.items()) {
            if (key.size() > 16384) invalid("Structural edit key budget exceeded");
            budget_nodes(child, nodes, depth + 1);
        }
    else if (value.is_array())
        for (const auto& child : value) budget_nodes(child, nodes, depth + 1);
}
std::size_t budget(const Json& value) {
    std::size_t nodes = 0;
    budget_nodes(value, nodes);
    const auto bytes = value.dump().size();
    if (bytes > proof_limit) invalid("Structural edit proof byte budget exceeded");
    return bytes;
}
std::set<std::string, std::less<>> owned_fields(std::string_view type) {
    if (type == "column") return {"form", "base_center_m", "height_m", "rotation_rad",
        "width_m", "depth_m", "radius_m"};
    if (type == "beam") return {"form", "start_m", "end_m", "up_dir", "width_m", "depth_m"};
    invalid("Structural edit requires a column or straight beam");
}
void placement(const Json& value) {
    keys(value, {"version", "mode", "offset_m"});
    if (!version_one(value.at("version")) || !value.at("mode").is_string() ||
        (value.at("mode") != "absolute" && value.at("mode") != "level") ||
        std::abs(scalar(value.at("offset_m"))) > 1e9)
        invalid("Structural edit vertical placement is invalid");
}
std::string profile_type(const Json& fields, const std::string& id) {
    const auto form = field(fields, "form");
    if (!fields.is_object() || !form || !form->is_string()) invalid("Structural profile requires its form");
    auto physical = fields;
    if (physical.contains("vertical_placement")) {
        placement(physical.at("vertical_placement"));
        physical.erase("vertical_placement");
    }
    std::string type;
    if (*form == "rectangular_column") {
        keys(physical, {"form", "base_center_m", "width_m", "depth_m", "height_m", "rotation_rad"});
        type = "column";
    } else if (*form == "circular_column") {
        if (physical.contains("rotation_rad"))
            keys(physical, {"form", "base_center_m", "radius_m", "height_m", "rotation_rad"});
        else keys(physical, {"form", "base_center_m", "radius_m", "height_m"});
        type = "column";
    } else if (*form == "straight_beam") {
        keys(physical, {"form", "start_m", "end_m", "up_dir", "width_m", "depth_m"});
        type = "beam";
    } else invalid("Structural profile form is unsupported");
    physical["version"] = 1;
    Entity typed;
    typed.id = id;
    typed.type = type;
    typed.properties = std::move(physical);
    (void)decode_building_entity(typed);
    return type;
}
Json transform_json(const ArchitecturalGroupTransform& value) {
    Json result{{"pivot_m", {value.pivot.x, value.pivot.y, value.pivot.z}},
        {"offset_m", {value.offset.x, value.offset.y, value.offset.z}},
        {"rotation_z_radians", value.rotation_z_radians}, {"scale", value.scale},
        {"flip_horizontal", value.flip_horizontal}, {"flip_vertical", value.flip_vertical}};
    (void)budget(result);
    if (!(value.scale > 0.0)) invalid("Structural transform scale must be positive");
    return result;
}
ArchitecturalGroupTransform transform(const Json& value) {
    keys(value, {"pivot_m", "offset_m", "rotation_z_radians", "scale", "flip_horizontal", "flip_vertical"});
    if (!value.at("flip_horizontal").is_boolean() || !value.at("flip_vertical").is_boolean())
        invalid("Structural transform reflections must be boolean");
    ArchitecturalGroupTransform result{point(value.at("pivot_m")), point(value.at("offset_m")),
        scalar(value.at("rotation_z_radians")), scalar(value.at("scale")),
        value.at("flip_horizontal").get<bool>(), value.at("flip_vertical").get<bool>()};
    (void)transform_json(result);
    return result;
}
const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && (!result->is_object() || result->size() > collection_limit))
        invalid("Structural quantity entries must be a bounded object");
    return result;
}
bool known_pointer(std::string_view pointer, std::string_view type) {
    if (pointer == "/width_m" || pointer == "/depth_m" || pointer == "/vertical_placement/offset_m") return true;
    if (type == "column" && (pointer == "/height_m" || pointer == "/radius_m")) return true;
    for (auto prefix : {std::string_view("/base_center_m/"), std::string_view("/start_m/"),
            std::string_view("/end_m/")}) {
        if ((type == "column") != (prefix == "/base_center_m/")) continue;
        if (pointer.starts_with(prefix) && pointer.substr(prefix.size()).find('/') == std::string_view::npos) return true;
    }
    return false;
}
const Json* actual_scalar(const Entity& entity, const std::string& pointer) {
    if (!known_pointer(pointer, entity.type)) return nullptr;
    if (pointer.starts_with("/base_center_m/") || pointer.starts_with("/start_m/") || pointer.starts_with("/end_m/")) {
        const auto index = pointer.substr(pointer.rfind('/') + 1);
        if (index != "0" && index != "1" && index != "2")
            invalid("Structural quantity coordinate has no canonical index");
    }
    const Json::json_pointer path(pointer);
    return entity.properties.contains(path) ? &entity.properties.at(path) : nullptr;
}
bool known_receipt(const Json& value) {
    if (!value.is_object()) return false;
    const auto version = field(value, "version");
    // Retained legacy/future records do not claim understood measurement
    // authority. They survive unchanged; new input still requires version one.
    return version && version_one(*version);
}
void receipt(const Json& value, const Json* actual) {
    if (!actual) invalid("Structural quantity receipt has no canonical scalar");
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() || expression->get_ref<const std::string&>().size() > 4096)
        invalid("Structural quantity expression budget exceeded");
    const auto entered = decode_constraint_quantity_receipt(value);
    if (entered.metres != scalar(*actual)) invalid("Structural quantity receipt is stale");
}
bool scalar_changed(const Entity& before, const Entity& after, const std::string& pointer) {
    const auto old = actual_scalar(before, pointer), current = actual_scalar(after, pointer);
    return (old == nullptr) != (current == nullptr) || (old && current && scalar(*old) != scalar(*current));
}
void validate_receipt_delta(const Json* old, const Json& current) {
    if (!known_receipt(current)) invalid("Structural input cannot author an opaque future receipt");
    if (!old) {
        keys(current, {"version", "original_expression", "entered_unit", "exact_metres"});
        keys(current.at("exact_metres"), {"numerator", "denominator"});
        return;
    }
    if (!known_receipt(*old)) invalid("Structural input cannot overwrite an opaque future receipt");
    auto permitted = *old;
    for (const auto* name : {"version", "original_expression", "entered_unit"}) permitted[name] = current.at(name);
    for (const auto* name : {"numerator", "denominator"}) permitted["exact_metres"][name] = current.at("exact_metres").at(name);
    if (!exact(permitted, current)) invalid("Structural input changed opaque quantity metadata");
}
void apply_receipts(const Entity& source, Entity& result, const Json& requested) {
    const auto old = entries(source);
    if (requested.is_null()) {
        if (!old) return;
        auto retained = *old;
        for (const auto& [pointer, raw] : old->items()) {
            (void)raw;
            if (known_pointer(pointer, source.type) && scalar_changed(source, result, pointer)) retained.erase(pointer);
        }
        // Preserve an already empty map. Once the final affected receipt is
        // removed, the editor's absent-map representation is also retained.
        if (!old->empty() && retained.empty()) result.properties.erase("quantity_entries");
        else result.properties["quantity_entries"] = std::move(retained);
        return;
    }
    if (!requested.is_object()) invalid("Structural input quantities must be a complete map");
    if (old)
        for (const auto& [pointer, raw] : old->items()) {
            const auto current = field(requested, pointer);
            if (current && exact(raw, *current)) continue;
            if (!known_pointer(pointer, source.type)) invalid("Structural input changed an opaque quantity pointer");
            if (!current && !scalar_changed(source, result, pointer))
                invalid("Structural input removed an unchanged receipt");
            if (current) validate_receipt_delta(&raw, *current);
        }
    for (const auto& [pointer, raw] : requested.items()) {
        const auto previous = old ? field(*old, pointer) : nullptr;
        if (previous && exact(*previous, raw)) continue;
        if (!known_pointer(pointer, source.type)) invalid("Structural input added an opaque quantity pointer");
        validate_receipt_delta(previous, raw);
        receipt(raw, actual_scalar(result, pointer));
    }
    result.properties["quantity_entries"] = requested;
}
Entity stage_profile(const Entity& source, const StructuralObjectEditIntent& intent) {
    if (source.id != intent.object_id || profile_type(intent.profile_fields, intent.object_id) != source.type)
        invalid("Structural profile target differs from its source owner or family");
    validate_structural_object_source_entity(source);
    auto result = source;
    for (const auto& name : owned_fields(source.type)) result.properties.erase(name);
    for (const auto& [name, raw] : intent.profile_fields.items()) result.properties[name] = raw;
    apply_receipts(source, result, intent.quantity_entries);
    validate_structural_object_source_entity(result);
    return exact(source, result) ? source : result;
}
} // namespace

Json encode_structural_object_edit_intent(const StructuralObjectEditIntent& intent) {
    (void)identity(intent.object_id);
    if ((!intent.profile_fields.is_null()) == intent.transform.has_value())
        invalid("Structural edit requires exactly one profile or transform");
    if (!intent.quantity_entries.is_null() && (intent.transform || !intent.quantity_entries.is_object()))
        invalid("Structural quantity input accompanies only a profile edit");
    Json result{{"version", 1}, {"object_id", intent.object_id}, {"profile_fields", intent.profile_fields},
        {"quantity_entries", intent.quantity_entries},
        {"transform", intent.transform ? transform_json(*intent.transform) : Json(nullptr)}};
    (void)budget(result);
    if (!intent.profile_fields.is_null()) (void)profile_type(intent.profile_fields, intent.object_id);
    return result;
}

StructuralObjectEditIntent decode_structural_object_edit_intent(const Json& value) {
    (void)budget(value);
    keys(value, {"version", "object_id", "profile_fields", "quantity_entries", "transform"});
    if (!version_one(value.at("version"))) invalid("Structural edit version is unsupported");
    StructuralObjectEditIntent result;
    result.object_id = identity(value.at("object_id"));
    result.profile_fields = value.at("profile_fields");
    result.quantity_entries = value.at("quantity_entries");
    if (!value.at("transform").is_null()) result.transform = transform(value.at("transform"));
    (void)encode_structural_object_edit_intent(result);
    return result;
}

void validate_structural_object_source_entity(const Entity& source) {
    if (source.type != "column" && source.type != "beam") invalid("Structural source family is unsupported");
    if (!source.extensions.is_object()) invalid("Structural source extensions must be an object");
    (void)decode_building_entity(source);
    if (const auto value = field(source.properties, "vertical_placement")) placement(*value);
    if (const auto values = entries(source)) {
        (void)budget(*values);
        for (const auto& [pointer, raw] : values->items())
            if (known_pointer(pointer, source.type) && known_receipt(raw))
                receipt(raw, actual_scalar(source, pointer));
    }
}

Entities replay_structural_object_edit_entities(const Entities& source,
    const std::vector<StructuralObjectEditIntent>& intents) {
    if (intents.size() > collection_limit) invalid("Structural edit target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    auto result = source;
    std::set<std::string, std::less<>> targets;
    std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto size = encode_structural_object_edit_intent(intent).dump().size();
        if (size > proof_limit - bytes) invalid("Structural edit batch proof byte budget exceeded");
        bytes += size;
        if (!targets.insert(intent.object_id).second) invalid("Structural edit contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.object_id))
            invalid("Structural edit target is inactive in the saved design");
        const auto found = source.find(intent.object_id);
        if (found == source.end() || found->first != found->second.id)
            invalid("Structural edit target is missing or has inconsistent identity");
        validate_structural_object_source_entity(found->second);
        (void)decode_building_entity(resolve_vertical_placement(source, found->second));
        result.at(intent.object_id) = intent.transform
            ? stage_structural_group_transform_entity(source, intent.object_id, *intent.transform)
            : stage_profile(found->second, intent);
    }
    for (const auto& id : targets) {
        validate_structural_object_source_entity(result.at(id));
        (void)decode_building_entity(resolve_vertical_placement(result, result.at(id)));
    }
    return result;
}

std::optional<StructuralObjectEditIntent> capture_structural_object_edit(
    const Entity& source, const Entity& candidate) {
    if (source.type != "column" && source.type != "beam") return std::nullopt;
    validate_structural_object_source_entity(source);
    validate_structural_object_source_entity(candidate);
    if (exact(source, candidate)) return std::nullopt;
    if (source.id != candidate.id || source.type != candidate.type)
        invalid("Structural capture changed source identity or family");
    StructuralObjectEditIntent result;
    result.object_id = source.id;
    result.profile_fields = Json::object();
    for (const auto& name : owned_fields(source.type))
        if (const auto raw = field(candidate.properties, name)) result.profile_fields[name] = *raw;
    if (const auto raw = field(candidate.properties, "vertical_placement")) result.profile_fields["vertical_placement"] = *raw;
    if (const auto values = entries(candidate)) result.quantity_entries = *values;
    (void)encode_structural_object_edit_intent(result);
    if (!exact(stage_profile(source, result), candidate))
        invalid("Structural capture changed fields outside typed replay authority");
    return result;
}

} // namespace sketch
