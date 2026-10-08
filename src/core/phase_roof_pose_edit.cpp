#include "sketch/phase_roof_pose_edit.hpp"

#include "sketch/architecture.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_roof_form_edit.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
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
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof pose edit fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof pose edit field is missing");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Roof pose identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof pose identity is invalid");
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
double number(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        invalid("Roof pose requires its actual finite scalar");
    return value.get<double>();
}
const char* unit_name(Unit unit) {
    switch (unit) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof pose unit is unsupported");
}
void receipt_budget(const Json& value) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit ||
        value.dump().size() > proof_limit)
        invalid("Roof pose quantity receipt budget exceeded");
}
bool known_receipt(const Json& value) {
    if (!value.is_object()) invalid("Roof pose quantity receipt must be an object");
    const auto version = field(value, "version");
    if (!version || (!version->is_number_integer() && !version->is_number_unsigned()) ||
        (version->is_number_integer() && !version->is_number_unsigned() && version->get<std::int64_t>() < 0))
        invalid("Roof pose quantity receipt version is invalid");
    return version_one(*version);
}
Quantity admitted_receipt(const Json& value, double metres) {
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (result.metres != metres) invalid("Roof pose quantity receipt is stale");
    return result;
}
Quantity quantity(const Json& value) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    receipt_budget(value);
    // The shared receipt decoder admits exact signed/zero lengths too.
    return decode_constraint_quantity_receipt(value);
}
Json quantity(const Quantity& value) {
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)}, {"exact_metres", {
            {"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("Roof pose quantity is internally inconsistent");
    return result;
}
struct Coordinate {
    const char* wire;
    const char* pointer;
    std::size_t index;
    std::optional<Quantity> RoofPoseEditIntent::* member;
};
constexpr std::array coordinates{
    Coordinate{"x", "/base_position_m/0", 0, &RoofPoseEditIntent::x},
    Coordinate{"y", "/base_position_m/1", 1, &RoofPoseEditIntent::y},
    Coordinate{"z", "/base_position_m/2", 2, &RoofPoseEditIntent::z}};
bool any(const RoofPoseEditIntent& intent) {
    return intent.orientation_radians.has_value() ||
        std::any_of(coordinates.begin(), coordinates.end(), [&](const auto& coordinate) {
            return (intent.*(coordinate.member)).has_value();
        });
}
const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && (!result->is_object() || result->size() > collection_limit))
        invalid("Roof pose quantity_entries must be a bounded object");
    return result;
}
bool descendant(std::string_view path, std::string_view ancestor) {
    return path.size() > ancestor.size() && path.starts_with(ancestor) && path[ancestor.size()] == '/';
}
void affected_receipts(const Entity& source, const std::string& pointer, bool coordinate) {
    const auto values = entries(source);
    if (!values) return;
    for (const auto& [path, raw] : values->items()) {
        if (path != pointer && !descendant(path, pointer) && !descendant(pointer, path)) continue;
        // Only direct coordinate receipts have understood pose meaning. A
        // parent, descendant, future version, or angle-length receipt cannot be
        // retained stale or overwritten by authoring a different scalar.
        if (!coordinate || path != pointer || !known_receipt(raw))
            invalid("Roof pose edit affects an opaque quantity receipt");
    }
}
void replace_receipt(Entity& result, const std::string& pointer, const Quantity& value) {
    const auto encoded = quantity(value);
    if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
    auto& values = result.properties.at("quantity_entries");
    const auto found = values.find(pointer);
    if (found == values.end()) {
        values[pointer] = encoded;
        return;
    }
    if (!known_receipt(*found)) invalid("Roof pose edit cannot replace an opaque future receipt");
    // Source admission binds the old understood core to its actual coordinate.
    // Retain opaque fields at both the receipt and rational levels.
    for (const auto* key : {"version", "original_expression", "entered_unit"}) (*found)[key] = encoded.at(key);
    for (const auto* key : {"numerator", "denominator"})
        (*found)["exact_metres"][key] = encoded.at("exact_metres").at(key);
}
Quantity captured_quantity(const Entity& original, const Entity& candidate,
    const std::string& pointer, double metres) {
    const auto before = entries(original), after = entries(candidate);
    const auto old_receipt = before ? field(*before, pointer) : nullptr;
    const auto new_receipt = after ? field(*after, pointer) : nullptr;
    if (new_receipt && (!old_receipt || !exact(*new_receipt, *old_receipt))) {
        if (!known_receipt(*new_receipt)) invalid("Roof pose capture cannot author a future quantity receipt");
        return admitted_receipt(*new_receipt, metres);
    }
    invalid("A changed roof coordinate requires its exact entered quantity receipt");
}
void normalize_unchanged_receipt(Entity& candidate, const Entity& original,
    const std::string& pointer, double metres) {
    const auto before = entries(original), after = entries(candidate);
    const auto old_receipt = before ? field(*before, pointer) : nullptr;
    const auto new_receipt = after ? field(*after, pointer) : nullptr;
    if (!new_receipt || (old_receipt && exact(*old_receipt, *new_receipt))) return;
    if (!known_receipt(*new_receipt)) invalid("Equivalent roof pose input cannot rewrite a future receipt");
    const auto entered = admitted_receipt(*new_receipt, metres);
    auto permitted = original;
    replace_receipt(permitted, pointer, entered);
    if (!exact(*new_receipt, permitted.properties.at("quantity_entries").at(pointer)))
        invalid("Equivalent roof pose input cannot change opaque quantity metadata");
    auto& values = candidate.properties.at("quantity_entries");
    if (old_receipt) values[pointer] = *old_receipt;
    else values.erase(pointer);
    if (values.empty() && !original.properties.contains("quantity_entries")) candidate.properties.erase("quantity_entries");
}
TopoDS_Shape resolved_shape(const Entities& source, const std::string& id) {
    const auto found = source.find(id);
    if (found == source.end() || found->first != found->second.id || found->second.type != "roof")
        invalid("Roof pose source join member is missing or inconsistent");
    validate_roof_pose_source_entity(found->second);
    return make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second)));
}
std::vector<RoofJoin> affected_joins(const Entities& source, const Ids& targets,
    const ConstraintPhaseScope& scope) {
    std::vector<RoofJoin> result;
    std::map<std::string, std::string, std::less<>> owners;
    for (const auto& [id, entity] : source) {
        if (entity.id != id) invalid("Roof pose actual source contains inconsistent entity identities");
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        const bool affected = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return targets.contains(member); });
        for (const auto& member : join.roof_ids) {
            const auto roof = source.find(member);
            if (roof == source.end() || roof->second.type != "roof" || roof->second.id != member)
                invalid("Roof pose actual source has a dangling roof join member");
            if (!owners.emplace(member, id).second)
                invalid("Roof pose actual source has overlapping roof join membership");
            if (affected && scope.inactive_owner_ids.contains(member))
                invalid("Roof pose affected join member is inactive in the saved design");
        }
        if (!affected) continue;
        if (scope.inactive_owner_ids.contains(id)) invalid("Roof pose affected join is inactive in the saved design");
        if (result.size() == collection_limit) invalid("Roof pose affected join budget exceeded");
        result.push_back(join);
    }
    return result;
}
void admit_joins(const Entities& source, const std::vector<RoofJoin>& joins) {
    for (const auto& join : joins) {
        std::vector<TopoDS_Shape> members;
        members.reserve(join.roof_ids.size());
        for (const auto& id : join.roof_ids) members.push_back(resolved_shape(source, id));
        (void)make_roof_join(join, members);
    }
}
} // namespace

nlohmann::json encode_roof_pose_edit_intent(const RoofPoseEditIntent& intent) {
    (void)identity(intent.roof_id);
    if (!any(intent)) invalid("Roof pose edit requires an authored coordinate or orientation");
    Json result{{"version", 1}, {"roof_id", intent.roof_id}};
    for (const auto& coordinate : coordinates) {
        const auto& value = intent.*(coordinate.member);
        result[coordinate.wire] = value ? quantity(*value) : Json(nullptr);
    }
    if (intent.orientation_radians && !std::isfinite(*intent.orientation_radians))
        invalid("Roof pose orientation must be finite");
    result["orientation_radians"] = intent.orientation_radians ? Json(*intent.orientation_radians) : Json(nullptr);
    if (result.dump().size() > proof_limit) invalid("Roof pose edit proof byte budget exceeded");
    return result;
}
RoofPoseEditIntent decode_roof_pose_edit_intent(const nlohmann::json& value) {
    if (value.dump().size() > proof_limit) invalid("Roof pose edit proof byte budget exceeded");
    keys(value, {"version", "roof_id", "x", "y", "z", "orientation_radians"});
    if (!version_one(value.at("version"))) invalid("Roof pose edit version is unsupported");
    RoofPoseEditIntent result;
    result.roof_id = identity(value.at("roof_id"));
    for (const auto& coordinate : coordinates)
        if (!value.at(coordinate.wire).is_null()) result.*(coordinate.member) = quantity(value.at(coordinate.wire));
    if (!value.at("orientation_radians").is_null()) result.orientation_radians = number(value.at("orientation_radians"));
    if (!any(result)) invalid("Roof pose edit requires an authored coordinate or orientation");
    return result;
}
void validate_roof_pose_source_entity(const Entity& source) {
    validate_roof_profile_source_entity(source);
}
Entity stage_roof_pose_entity(const Entity& source, const RoofPoseEditIntent& intent) {
    (void)encode_roof_pose_edit_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof pose target differs from its actual source identity");
    validate_roof_pose_source_entity(source);
    auto result = source;
    bool changed = false;
    for (const auto& coordinate : coordinates) {
        const auto& value = intent.*(coordinate.member);
        if (!value || number(source.properties.at("base_position_m").at(coordinate.index)) == value->metres) continue;
        affected_receipts(source, coordinate.pointer, true);
        result.properties["base_position_m"][coordinate.index] = value->metres;
        replace_receipt(result, coordinate.pointer, *value);
        changed = true;
    }
    if (intent.orientation_radians && number(source.properties.at("orientation_rad")) != *intent.orientation_radians) {
        affected_receipts(source, "/orientation_rad", false);
        result.properties["orientation_rad"] = *intent.orientation_radians;
        changed = true;
    }
    if (!changed) return source;
    return result;
}
Entity replay_roof_pose_entity(const Entity& source, const RoofPoseEditIntent& intent) {
    auto result = stage_roof_pose_entity(source, intent);
    validate_roof_pose_source_entity(result);
    return result;
}
std::map<std::string, Entity, std::less<>> replay_roof_pose_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofPoseEditIntent>& intents) {
    if (intents.size() > collection_limit) invalid("Roof pose target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    Ids targets;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_roof_pose_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Roof pose batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.roof_id).second) invalid("Roof pose batch contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof pose target is inactive in the saved design");
        (void)resolved_shape(source, intent.roof_id);
    }
    const auto joins = affected_joins(source, targets, scope);
    admit_joins(source, joins);
    auto result = source;
    for (const auto& intent : intents)
        result.at(intent.roof_id) = replay_roof_pose_entity(source.at(intent.roof_id), intent);
    for (const auto& target : targets) (void)resolved_shape(result, target);
    admit_joins(result, joins);
    return result;
}
Entity normalize_equivalent_roof_pose_inputs(const Entity& original, const Entity& candidate, bool allow_form_change) {
    auto normalized = allow_form_change && original.properties.at("form") != candidate.properties.at("form")
        ? normalize_equivalent_roof_form_inputs(original, candidate)
        : normalize_equivalent_roof_inputs(original, candidate);
    for (const auto& coordinate : coordinates) {
        const double before = number(original.properties.at("base_position_m").at(coordinate.index));
        const double after = number(candidate.properties.at("base_position_m").at(coordinate.index));
        if (before == after) normalize_unchanged_receipt(normalized, original, coordinate.pointer, after);
    }
    return normalized;
}
std::optional<RoofPoseEditIntent> infer_roof_pose_edit(const Entity& original, const Entity& candidate, bool allow_form_change) {
    validate_roof_pose_source_entity(original);
    validate_roof_pose_source_entity(candidate);
    if (original.id != candidate.id || original.type != candidate.type ||
        (!allow_form_change && original.properties.at("form") != candidate.properties.at("form")))
        invalid("Roof pose input inference cannot change owner identity or type");
    RoofPoseEditIntent intent;
    intent.roof_id = original.id;
    for (const auto& coordinate : coordinates) {
        const double before = number(original.properties.at("base_position_m").at(coordinate.index));
        const double after = number(candidate.properties.at("base_position_m").at(coordinate.index));
        if (before != after) intent.*(coordinate.member) = captured_quantity(original, candidate, coordinate.pointer, after);
    }
    const double before = number(original.properties.at("orientation_rad"));
    const double after = number(candidate.properties.at("orientation_rad"));
    if (before != after) intent.orientation_radians = after;
    if (!any(intent)) return std::nullopt;
    (void)encode_roof_pose_edit_intent(intent);
    return intent;
}
std::optional<RoofPoseEditIntent> capture_roof_pose_edit(const Entity& original, const Entity& candidate) {
    const auto normalized = normalize_equivalent_roof_pose_inputs(original, candidate);
    const auto intent = infer_roof_pose_edit(original, candidate);
    const auto expected = intent ? replay_roof_pose_entity(original, *intent) : original;
    if (!exact(normalized, expected)) invalid("Roof pose candidate differs from independent typed replay");
    if (exact(expected, original)) return std::nullopt;
    return intent;
}
} // namespace sketch
