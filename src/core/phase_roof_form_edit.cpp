#include "sketch/phase_roof_form_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/phase_roof_opening_edit.hpp"
#include "sketch/phase_roof_pose_edit.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof form edit fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof form edit field is missing");
}
const Json* field(const Json& object, const std::string& key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
bool version_one(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 1;
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Roof form identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof form identity is invalid");
    return result;
}
const char* form_name(RoofForm form) {
    switch (form) {
    case RoofForm::sloped_roof_panel: return "sloped_roof_panel";
    case RoofForm::gable_roof: return "gable_roof";
    case RoofForm::hip_roof: return "hip_roof";
    }
    invalid("Roof target form is unsupported");
}
RoofForm form(const Json& value) {
    if (!value.is_string()) invalid("Roof target form must be a string");
    const auto& name = value.get_ref<const std::string&>();
    if (name == "sloped_roof_panel") return RoofForm::sloped_roof_panel;
    if (name == "gable_roof") return RoofForm::gable_roof;
    if (name == "hip_roof") return RoofForm::hip_roof;
    invalid("Roof target form is unsupported");
}
const char* unit_name(Unit unit) {
    switch (unit) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof form quantity unit is unsupported");
}
double number(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) invalid("Roof form scalar must be finite");
    return value.get<double>();
}
void receipt_budget(const Json& value) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit || value.dump().size() > proof_limit)
        invalid("Roof form quantity receipt budget exceeded");
}
bool known_receipt(const Json& value) {
    if (!value.is_object()) invalid("Roof form receipt must be an object");
    const auto version = field(value, "version");
    if (!version || (!version->is_number_integer() && !version->is_number_unsigned()) ||
        (version->is_number_integer() && !version->is_number_unsigned() && version->get<std::int64_t>() < 0))
        invalid("Roof form quantity receipt version is invalid");
    return version_one(*version);
}
Quantity quantity(const Json& value, bool zero_allowed, bool strict = true) {
    if (strict) {
        keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
        keys(value.at("exact_metres"), {"numerator", "denominator"});
    }
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (zero_allowed ? result.metres < 0 : result.metres <= default_geometry_tolerance_metres)
        invalid("Roof form dimension is outside its admitted range");
    return result;
}
Json quantity(const Quantity& value, bool zero_allowed) {
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)}, {"exact_metres", {
            {"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result, zero_allowed);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("Roof form quantity is internally inconsistent");
    return result;
}
struct Dimension {
    const char* wire;
    const char* canonical;
    Quantity RoofFormEditIntent::* member;
};
constexpr std::array dimensions{
    Dimension{"length", "length_m", &RoofFormEditIntent::length},
    Dimension{"span", "span_m", &RoofFormEditIntent::span},
    Dimension{"rise", "rise_m", &RoofFormEditIntent::rise},
    Dimension{"overhang", "overhang_m", &RoofFormEditIntent::overhang},
    Dimension{"thickness", "thickness_m", &RoofFormEditIntent::thickness}};
const char* canonical(RoofForm target, const Dimension& dimension) {
    return dimension.member == &RoofFormEditIntent::length && target == RoofForm::sloped_roof_panel
        ? "run_m" : dimension.canonical;
}
bool zero_allowed(RoofForm target, const Dimension& dimension) {
    return dimension.member == &RoofFormEditIntent::overhang ||
        (dimension.member == &RoofFormEditIntent::rise && target == RoofForm::sloped_roof_panel);
}
const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && (!result->is_object() || result->size() > collection_limit))
        invalid("Roof form quantity_entries must be a bounded object");
    return result;
}
bool descendant(std::string_view path, std::string_view ancestor) {
    return path.size() > ancestor.size() && path.starts_with(ancestor) && path[ancestor.size()] == '/';
}
void affected_receipts(const Entity& source, const std::string& pointer, bool dimension) {
    const auto values = entries(source);
    if (!values) return;
    for (const auto& [path, raw] : values->items()) {
        if (path != pointer && !descendant(path, pointer) && !descendant(pointer, path)) continue;
        if (!dimension || path != pointer || !known_receipt(raw))
            invalid("Roof form conversion affects an opaque quantity receipt");
    }
}
void replace_receipt(Entity& result, const std::string& pointer, const Json& encoded) {
    if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
    auto& values = result.properties.at("quantity_entries");
    const auto found = values.find(pointer);
    if (found == values.end()) { values[pointer] = encoded; return; }
    if (!known_receipt(*found)) invalid("Roof form conversion cannot rewrite a future receipt");
    // Only the understood core changes; source-derived opaque siblings remain.
    for (const auto* key : {"version", "original_expression", "entered_unit"}) (*found)[key] = encoded.at(key);
    for (const auto* key : {"numerator", "denominator"})
        (*found)["exact_metres"][key] = encoded.at("exact_metres").at(key);
}
template<class Target>
RoofObject target_object(const RoofObject& actual, const RoofFormEditIntent& intent) {
    return std::visit([&](const auto& source) -> RoofObject {
        Target result;
        result.id = source.id;
        result.base_position = source.base_position;
        result.orientation_radians = source.orientation_radians;
        result.openings = source.openings;
        if constexpr (std::is_same_v<Target, SlopedRoofPanel>) result.run = intent.length.metres;
        else result.length = intent.length.metres;
        result.span = intent.span.metres;
        result.rise = intent.rise.metres;
        result.overhang = intent.overhang.metres;
        result.thickness = intent.thickness.metres;
        const double run = intent.target_form == RoofForm::sloped_roof_panel ? intent.length.metres : intent.span.metres * 0.5;
        result.pitch_radians = std::atan2(intent.rise.metres, run);
        return result;
    }, actual);
}
RoofObject target_object(const RoofObject& actual, const RoofFormEditIntent& intent) {
    switch (intent.target_form) {
    case RoofForm::sloped_roof_panel: return target_object<SlopedRoofPanel>(actual, intent);
    case RoofForm::gable_roof: return target_object<GableRoof>(actual, intent);
    case RoofForm::hip_roof: return target_object<HipRoof>(actual, intent);
    }
    invalid("Roof target form is unsupported");
}
void retain_equal_number(Json& value, const Json& before) {
    if (value.is_number() && before.is_number() && number(value) == number(before)) value = before;
}
} // namespace

nlohmann::json encode_roof_form_edit_intent(const RoofFormEditIntent& intent) {
    (void)identity(intent.roof_id);
    Json result{{"version", 1}, {"roof_id", intent.roof_id}, {"target_form", form_name(intent.target_form)}};
    for (const auto& dimension : dimensions)
        result[dimension.wire] = quantity(intent.*(dimension.member), zero_allowed(intent.target_form, dimension));
    if (result.dump().size() > proof_limit) invalid("Roof form edit proof byte budget exceeded");
    return result;
}
RoofFormEditIntent decode_roof_form_edit_intent(const nlohmann::json& value) {
    if (value.dump().size() > proof_limit) invalid("Roof form edit proof byte budget exceeded");
    keys(value, {"version", "roof_id", "target_form", "length", "span", "rise", "overhang", "thickness"});
    if (!version_one(value.at("version"))) invalid("Roof form edit version is unsupported");
    RoofFormEditIntent result;
    result.roof_id = identity(value.at("roof_id"));
    result.target_form = form(value.at("target_form"));
    for (const auto& dimension : dimensions)
        result.*(dimension.member) = quantity(value.at(dimension.wire), zero_allowed(result.target_form, dimension));
    return result;
}
Entity stage_roof_form_entity(const Entity& source, const RoofFormEditIntent& intent) {
    (void)encode_roof_form_edit_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof form target differs from its actual source identity");
    validate_roof_profile_source_entity(source);
    const auto source_form = form(source.properties.at("form"));
    if (source_form == intent.target_form) invalid("Same-form roof authoring requires the existing profile contract");
    affected_receipts(source, "/form", false);
    affected_receipts(source, "/pitch_rad", false);
    const auto encoded = encode_roof_properties(target_object(decode_roof_entity(source), intent));
    auto result = source;
    result.properties["form"] = encoded.at("form");
    result.properties["pitch_rad"] = encoded.at("pitch_rad");
    retain_equal_number(result.properties.at("pitch_rad"), source.properties.at("pitch_rad"));
    for (const auto& dimension : dimensions) {
        const std::string old_name = canonical(source_form, dimension), new_name = canonical(intent.target_form, dimension);
        const std::string old_pointer = "/" + old_name, new_pointer = "/" + new_name;
        affected_receipts(source, old_pointer, true);
        if (old_pointer != new_pointer) {
            affected_receipts(source, new_pointer, true);
            const auto values = entries(source);
            if (source.properties.contains(new_name) || (values && values->contains(new_pointer)))
                invalid("Roof form conversion would overwrite an opaque inactive form field");
            // Retirement is derived from the actual source discriminant. Move
            // its known receipt with all opaque siblings before replacing core.
            if (values && values->contains(old_pointer)) {
                result.properties["quantity_entries"][new_pointer] = values->at(old_pointer);
                result.properties["quantity_entries"].erase(old_pointer);
            }
            result.properties.erase(old_name);
        }
        result.properties[new_name] = encoded.at(new_name);
        retain_equal_number(result.properties.at(new_name), source.properties.at(old_name));
        replace_receipt(result, new_pointer, quantity(intent.*(dimension.member), zero_allowed(intent.target_form, dimension)));
    }
    (void)entries(result);
    // Codec admission proves the retained version and actual roster bindings;
    // native geometry is deliberately deferred until the composite is whole.
    (void)decode_roof_entity(result);
    return result;
}
Entity replay_roof_form_entity(const Entity& source, const RoofFormEditIntent& intent) {
    auto result = stage_roof_form_entity(source, intent);
    validate_roof_profile_source_entity(result);
    return result;
}
std::optional<RoofFormEditIntent> infer_roof_form_edit(const Entity& original, const Entity& candidate) {
    validate_roof_profile_source_entity(original);
    validate_roof_profile_source_entity(candidate);
    if (original.id != candidate.id || original.type != candidate.type)
        invalid("Roof form input inference cannot change its owner identity or type");
    const auto target = form(candidate.properties.at("form"));
    if (form(original.properties.at("form")) == target) return std::nullopt;
    RoofFormEditIntent result;
    result.roof_id = original.id;
    result.target_form = target;
    const auto values = entries(candidate);
    for (const auto& dimension : dimensions) {
        const std::string name = canonical(target, dimension), pointer = "/" + name;
        const auto raw = values ? field(*values, pointer) : nullptr;
        if (!raw || !known_receipt(*raw)) invalid("Every target roof dimension requires its exact entered receipt");
        auto parsed = quantity(*raw, zero_allowed(target, dimension), false);
        if (parsed.metres != number(candidate.properties.at(name))) invalid("Roof form target quantity receipt is stale");
        result.*(dimension.member) = std::move(parsed);
    }
    (void)encode_roof_form_edit_intent(result);
    return result;
}
Entity normalize_equivalent_roof_form_inputs(const Entity& original, const Entity& candidate) {
    validate_roof_profile_source_entity(original);
    if (exact(original, candidate)) return original;
    validate_roof_profile_source_entity(candidate);
    if (original.id != candidate.id || original.type != candidate.type)
        invalid("Roof form normalization cannot change its owner identity or type");
    const auto before_form = form(original.properties.at("form")), after_form = form(candidate.properties.at("form"));
    if (before_form == after_form) {
        auto normalized = normalize_equivalent_roof_opening_inputs(original, candidate);
        return normalize_equivalent_roof_pose_inputs(original, normalized);
    }
    auto normalized = candidate;
    for (const auto* key : {"orientation_rad", "pitch_rad"})
        retain_equal_number(normalized.properties.at(key), original.properties.at(key));
    for (std::size_t index = 0; index < 3; ++index)
        retain_equal_number(normalized.properties.at("base_position_m").at(index), original.properties.at("base_position_m").at(index));
    for (const auto& dimension : dimensions)
        retain_equal_number(normalized.properties.at(canonical(after_form, dimension)), original.properties.at(canonical(before_form, dimension)));
    const auto before_rows = field(original.properties, "roof_openings");
    if (before_rows && normalized.properties.contains("roof_openings")) {
        auto& after_rows = normalized.properties.at("roof_openings");
        for (std::size_t index = 0; index < std::min(before_rows->size(), after_rows.size()); ++index) {
            if (before_rows->at(index).at("id") != after_rows.at(index).at("id")) continue;
            for (const auto* key : {"x_m", "y_m", "width_m", "depth_m"})
                retain_equal_number(after_rows.at(index).at(key), before_rows->at(index).at(key));
        }
    }
    return normalized;
}
std::optional<RoofFormEditIntent> capture_roof_form_edit(const Entity& original, const Entity& candidate) {
    const auto normalized = normalize_equivalent_roof_form_inputs(original, candidate);
    const auto intent = infer_roof_form_edit(original, candidate);
    const auto expected = intent ? replay_roof_form_entity(original, *intent) : original;
    if (!exact(normalized, expected)) invalid("Roof form candidate differs from independent typed replay");
    return intent;
}
} // namespace sketch
