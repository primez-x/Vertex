#include "sketch/structural_profile_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/structural_hosted_components.hpp"

#include <algorithm>
#include <array>
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
constexpr std::size_t target_limit = 1000;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) {
    throw std::invalid_argument(std::string("Structural profile edit: ") + reason);
}
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("unsupported closed fields");
    for (const auto* key : expected) if (!value.contains(key)) invalid("missing required field");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("invalid actual source identity");
    return result;
}
void receipt_fields(const Json& value) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    const auto& expression = value.at("original_expression");
    if (!expression.is_string() || expression.get_ref<const std::string&>().size() > 4096)
        invalid("quantity expression budget exceeded");
    const auto& unit = value.at("entered_unit");
    if (!unit.is_string() || unit.get_ref<const std::string&>().size() > 2)
        invalid("quantity unit is unsupported");
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("quantity version is unsupported");
}
Json quantity(const Quantity& value) {
    if (value.original_expression.size() > 4096 || !std::isfinite(value.metres) || !(value.metres > 0.0))
        invalid("quantity must be finite, positive and bounded");
    auto encoded = encode_constraint_quantity_receipt(value);
    receipt_fields(encoded);
    return encoded;
}
Quantity quantity(const Json& value) {
    receipt_fields(value);
    const auto decoded = decode_constraint_quantity_receipt(value);
    if (quantity(decoded).dump() != value.dump()) invalid("quantity must use its exact canonical codec");
    return decoded;
}
struct Dimension {
    const char* name;
    const char* field;
    std::optional<Quantity> StructuralProfileEditIntent::* intent;
};
constexpr std::array dimensions{
    Dimension{"width", "width_m", &StructuralProfileEditIntent::width},
    Dimension{"depth", "depth_m", &StructuralProfileEditIntent::depth},
    Dimension{"height", "height_m", &StructuralProfileEditIntent::height},
    Dimension{"radius", "radius_m", &StructuralProfileEditIntent::radius}};

std::set<std::string_view> applicable_dimensions(const Entity& source) {
    const auto& form = source.properties.at("form");
    if (source.type == "column" && form == "rectangular_column") return {"width_m", "depth_m", "height_m"};
    if (source.type == "column" && form == "circular_column") return {"radius_m", "height_m"};
    if (source.type == "beam" && form == "straight_beam") return {"width_m", "depth_m"};
    invalid("unsupported actual column or beam form");
}
void replace_receipt(Json& entries, const std::string& pointer, const Quantity& value) {
    const auto encoded = quantity(value);
    const auto old = entries.find(pointer);
    if (old == entries.end()) {
        if (entries.size() >= 4096) invalid("quantity entry budget exceeded");
        entries[pointer] = encoded;
        return;
    }
    if (!old->is_object() || !old->contains("version") ||
        !old->at("version").is_number_integer() || old->at("version") != 1)
        invalid("cannot overwrite an opaque future receipt");
    // The existing source validator has authenticated this receipt against its
    // actual scalar. Retain every unknown top-level and rational sibling.
    for (const auto* key : {"version", "original_expression", "entered_unit"}) (*old)[key] = encoded.at(key);
    for (const auto* key : {"numerator", "denominator"})
        (*old)["exact_metres"][key] = encoded.at("exact_metres").at(key);
}
Entity replay_entity(const Entity& source, const StructuralProfileEditIntent& intent) {
    if (source.id != intent.object_id) invalid("target differs from its actual source owner");
    validate_structural_profile_source_entity(source);
    const auto applicable = applicable_dimensions(source);
    auto result = source;
    for (const auto& dimension : dimensions) {
        const auto& entered = intent.*(dimension.intent);
        if (!entered) continue;
        if (!applicable.contains(dimension.field)) invalid("quantity is inapplicable to the actual source form");
        if (entered->metres == source.properties.at(dimension.field).get<double>()) continue;
        result.properties[dimension.field] = entered->metres;
        if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
        replace_receipt(result.properties.at("quantity_entries"), "/" + std::string(dimension.field), *entered);
    }
    validate_structural_profile_source_entity(result);
    return result;
}
} // namespace

Json encode_structural_profile_edit_intent(const StructuralProfileEditIntent& intent) {
    (void)identity(intent.object_id);
    Json result{{"version", 1}, {"object_id", intent.object_id}};
    for (const auto& dimension : dimensions) {
        const auto& entered = intent.*(dimension.intent);
        result[dimension.name] = entered ? quantity(*entered) : Json(nullptr);
    }
    return result;
}
StructuralProfileEditIntent decode_structural_profile_edit_intent(const Json& value) {
    keys(value, {"version", "object_id", "width", "depth", "height", "radius"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("unsupported version");
    StructuralProfileEditIntent result;
    result.object_id = identity(value.at("object_id"));
    for (const auto& dimension : dimensions)
        if (!value.at(dimension.name).is_null()) result.*(dimension.intent) = quantity(value.at(dimension.name));
    if (encode_structural_profile_edit_intent(result).dump() != value.dump())
        invalid("intent must use its exact canonical codec");
    return result;
}
void validate_structural_profile_source_entity(const Entity& source) {
    (void)identity(source.id);
    validate_structural_object_source_entity(source);
    (void)applicable_dimensions(source);
}
Entity prepare_structural_profile_edit_entity(const Entity& source, const StructuralProfileEditIntent& intent) {
    (void)encode_structural_profile_edit_intent(intent);
    return replay_entity(source, intent);
}
Entities replay_structural_profile_entities(const Entities& source,
    const std::vector<StructuralProfileEditIntent>& intents) {
    if (intents.size() > target_limit) invalid("target budget exceeded");
    std::set<std::string, std::less<>> targets;
    std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto count = encode_structural_profile_edit_intent(intent).dump().size();
        if (count > proof_limit - bytes) invalid("batch proof byte budget exceeded");
        bytes += count;
        if (!targets.insert(intent.object_id).second) invalid("duplicate targets");
        const auto found = source.find(intent.object_id);
        if (found == source.end() || found->first != found->second.id)
            invalid("target is missing or has inconsistent identity");
    }
    if (intents.empty()) return source;
    const std::vector<std::string> hosts(targets.begin(), targets.end());
    const auto admit = [&](const Entities& actual) {
        const auto plan = inspect_structural_hosted_components(actual, hosts);
        if (!plan.ready())
            throw std::invalid_argument("Structural profile edit: " + plan.diagnostics.front().entity_id +
                ": " + plan.diagnostics.front().reason);
        return plan;
    };
    // Scalar authority starts from the exact raw owner, never the full-profile
    // descriptor whose historical meaning includes replacing dormant slots.
    // No transform is authored, so all actual catalog rows remain untouched.
    // Inspect retained members too: source context, active phase, resolved Z,
    // physical hosts and every qualified hosted geometry must remain admitted.
    const auto original_plan = admit(source);
    auto result = source;
    for (const auto& intent : intents)
        result.at(intent.object_id) = prepare_structural_profile_edit_entity(source.at(intent.object_id), intent);
    const auto final_plan = admit(result);
    if (final_plan.catalog_ids != original_plan.catalog_ids || final_plan.instance_ids != original_plan.instance_ids ||
        final_plan.original_presentation_ids != original_plan.original_presentation_ids)
        invalid("scalar replay changed the qualified hosted roster or original aliases");
    return result;
}

} // namespace sketch
