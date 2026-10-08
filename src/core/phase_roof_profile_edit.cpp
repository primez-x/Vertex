#include "sketch/phase_roof_profile_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"

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
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof profile edit fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof profile edit field is missing");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Roof profile identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof profile identity is invalid");
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
const char* unit_name(Unit unit) {
    switch (unit) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof profile unit is unsupported");
}
Unit unit(const Json& value) {
    if (!value.is_string()) invalid("Roof opening receipt default unit must be a string");
    const auto& name = value.get_ref<const std::string&>();
    if (name == "m") return Unit::metre;
    if (name == "mm") return Unit::millimetre;
    if (name == "cm") return Unit::centimetre;
    if (name == "ft") return Unit::foot;
    if (name == "in") return Unit::inch;
    invalid("Roof opening receipt default unit is unsupported");
}
void receipt_budget(const Json& value) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit ||
        value.dump().size() > proof_limit)
        invalid("Roof profile quantity receipt budget exceeded");
}
Quantity admitted_receipt(const Json& value, double metres) {
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (result.metres != metres) invalid("Roof profile quantity receipt is stale");
    return result;
}
// Unknown receipt versions retain opaque meaning. A malformed envelope at a
// known pointer is not an unsupported version and cannot bypass admission.
bool known_receipt(const Json& value) {
    if (!value.is_object()) invalid("Roof profile quantity receipt must be an object");
    const auto version = field(value, "version");
    if (!version || (!version->is_number_integer() && !version->is_number_unsigned()) ||
        (version->is_number_integer() && !version->is_number_unsigned() && version->get<std::int64_t>() < 0))
        invalid("Roof profile quantity receipt version is invalid");
    return version_one(*version);
}
Quantity quantity(const Json& value) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (result.metres < 0.0) invalid("Roof profile dimensions must be nonnegative");
    return result;
}
Json quantity(const Quantity& value) {
    // Roof overhang and a flat sloped-panel rise permit zero. The common
    // fixed-length encoder enforces positivity, so compose its exact schema
    // and use the shared signed/zero decoder to prove internal consistency.
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)}, {"exact_metres", {
            {"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("Roof profile quantity is internally inconsistent");
    return result;
}
struct Dimension {
    const char* wire;
    const char* canonical;
    bool zero_allowed;
    std::optional<Quantity> RoofProfileEditIntent::* intent;
};
constexpr std::array dimensions{
    Dimension{"length", "length_m", false, &RoofProfileEditIntent::length},
    Dimension{"span", "span_m", false, &RoofProfileEditIntent::span},
    Dimension{"rise", "rise_m", true, &RoofProfileEditIntent::rise},
    Dimension{"overhang", "overhang_m", true, &RoofProfileEditIntent::overhang},
    Dimension{"thickness", "thickness_m", false, &RoofProfileEditIntent::thickness}};
const char* canonical(const Entity& entity, const Dimension& dimension) {
    if (dimension.intent == &RoofProfileEditIntent::length &&
        entity.properties.at("form") == "sloped_roof_panel") return "run_m";
    return dimension.canonical;
}
bool any(const RoofProfileEditIntent& intent) {
    return std::any_of(dimensions.begin(), dimensions.end(), [&](const auto& dimension) {
        return (intent.*(dimension.intent)).has_value();
    });
}
const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && (!result->is_object() || result->size() > collection_limit))
        invalid("Roof profile quantity_entries must be a bounded object");
    return result;
}
double number(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        invalid("Roof profile receipt requires its actual finite scalar");
    return value.get<double>();
}
const Json* actual_scalar(const Entity& source, const std::string& pointer) {
    if (pointer == "/run_m" || pointer == "/length_m" || pointer == "/span_m" ||
        pointer == "/rise_m" || pointer == "/overhang_m" || pointer == "/thickness_m")
        return field(source.properties, pointer.substr(1));
    constexpr std::string_view position_prefix = "/base_position_m/";
    const std::string_view path(pointer);
    if (path.starts_with(position_prefix)) {
        const auto index = path.substr(position_prefix.size());
        if (index != "0" && index != "1" && index != "2")
            invalid("Roof profile position receipt has no existing coordinate index");
        return &source.properties.at("base_position_m").at(static_cast<std::size_t>(index.front() - '0'));
    }
    constexpr std::string_view opening_prefix = "/roof_openings/";
    if (path.starts_with(opening_prefix)) {
        const auto remaining = path.substr(opening_prefix.size());
        const auto slash = remaining.find('/');
        if (slash == std::string_view::npos) return nullptr;
        const auto key = remaining.substr(slash + 1);
        if (key != "x_m" && key != "y_m" && key != "width_m" && key != "depth_m") return nullptr;
        const auto index = remaining.substr(0, slash);
        std::size_t position = 0;
        const auto parsed = std::from_chars(index.data(), index.data() + index.size(), position);
        const auto roster = field(source.properties, "roof_openings");
        if (index.empty() || (index.size() > 1 && index.front() == '0') ||
            parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size() ||
            !roster || position >= roster->size())
            invalid("Roof profile opening receipt has no existing opening index");
        return field(roster->at(position), std::string(key));
    }
    return nullptr;
}
bool known_pointer(const std::string& pointer) {
    if (pointer == "/run_m" || pointer == "/length_m" || pointer == "/span_m" ||
        pointer == "/rise_m" || pointer == "/overhang_m" || pointer == "/thickness_m") return true;
    const std::string_view path(pointer);
    if (path.starts_with("/base_position_m/")) {
        // Other descendants are opaque; only a direct coordinate is known.
        return path.substr(std::string_view("/base_position_m/").size()).find('/') == std::string_view::npos;
    }
    if (!path.starts_with("/roof_openings/")) return false;
    const auto remaining = path.substr(std::string_view("/roof_openings/").size());
    const auto slash = remaining.find('/');
    if (slash == std::string_view::npos) return false;
    const auto key = remaining.substr(slash + 1);
    return key == "x_m" || key == "y_m" || key == "width_m" || key == "depth_m";
}
void opening_receipts(const Entity& source) {
    const auto extension = field(source.extensions, "roof_opening_input");
    if (!extension) return;
    // Future extensions and unknown payloads remain wholly opaque. An
    // envelope declaring known schema 1 must pass the full current decoder.
    const auto version = field(*extension, "version");
    if (!version || *version != 1) return;
    if (!version_one(*version)) invalid("Roof opening input version must be an integer");
    const auto receipts = field(*extension, "entries");
    if (!receipts || !receipts->is_object() || receipts->size() > collection_limit)
        invalid("Roof opening input entries must be a bounded object");
    std::map<std::string, const Json*, std::less<>> children;
    if (const auto roster = field(source.properties, "roof_openings"))
        for (const auto& child : *roster) children.emplace(child.at("id").get<std::string>(), &child);
    for (const auto& [id, child_receipts] : receipts->items()) {
        if (!child_receipts.is_object()) {
            if (children.contains(id)) invalid("Roof opening input child entries must be objects");
            continue; // Unknown non-child extension content remains opaque.
        }
        if (const auto declared_version=field(child_receipts,"version")) {
            if (!declared_version->is_number_integer() || *declared_version<0)
                invalid("Roof opening child receipt version must be a nonnegative integer");
            if (*declared_version!=1) continue;
        }
        for (const auto* key : {"x_m", "y_m", "width_m", "depth_m"}) {
            const auto raw = field(child_receipts, key);
            if (!raw) continue;
            if (const auto declared_version=field(*raw,"version")) {
                if (!declared_version->is_number_integer() || *declared_version<0)
                    invalid("Roof opening receipt version must be a nonnegative integer");
                if (*declared_version!=1) continue;
            }
            const auto child = children.find(id);
            if (child == children.end()) invalid("Roof opening input receipt has no existing child identity");
            receipt_budget(*raw);
            const auto default_unit = field(*raw, "default_unit"), rational = field(*raw, "exact_metres");
            if (!default_unit || !rational) invalid("Roof opening input receipt fields are missing");
            const auto parsed = parse_quantity(raw->at("original_expression").get_ref<const std::string&>(), unit(*default_unit));
            const Json receipt{{"version", 1}, {"original_expression", parsed.original_expression},
                {"entered_unit", unit_name(parsed.entered_unit)}, {"exact_metres", *rational}};
            (void)admitted_receipt(receipt, number(child->second->at(key)));
        }
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
    if (!known_receipt(*found)) invalid("Roof profile edit cannot replace an opaque future receipt");
    // Source admission already bound this receipt to the old actual scalar.
    // Replace only the understood core, preserving every opaque sibling.
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
        if (!known_receipt(*new_receipt)) invalid("Roof capture cannot author a future quantity receipt");
        return admitted_receipt(*new_receipt, metres);
    }
    invalid("A changed roof dimension requires its exact entered quantity receipt");
}
void normalize_unchanged_receipt(Entity& candidate,const Entity& original,
    const std::string& pointer,double metres) {
    const auto before=entries(original),after=entries(candidate);
    const auto old_receipt=before ? field(*before,pointer) : nullptr;
    const auto new_receipt=after ? field(*after,pointer) : nullptr;
    if (!new_receipt || (old_receipt && exact(*old_receipt,*new_receipt))) return;
    if (!known_receipt(*new_receipt)) invalid("Equivalent roof input cannot rewrite a future quantity receipt");
    const auto entered=admitted_receipt(*new_receipt,metres);
    auto permitted=original;
    replace_receipt(permitted,pointer,entered);
    if (!exact(*new_receipt,permitted.properties.at("quantity_entries").at(pointer)))
        invalid("Equivalent roof input cannot change opaque quantity metadata");
    auto& values=candidate.properties.at("quantity_entries");
    if (old_receipt) values[pointer]=*old_receipt;
    else values.erase(pointer);
    if (values.empty() && !original.properties.contains("quantity_entries")) candidate.properties.erase("quantity_entries");
}
} // namespace

nlohmann::json encode_roof_profile_edit_intent(const RoofProfileEditIntent& intent) {
    (void)identity(intent.roof_id);
    if (!any(intent)) invalid("Roof profile edit requires an authored dimension");
    Json result{{"version", 1}, {"roof_id", intent.roof_id}};
    for (const auto& dimension : dimensions) {
        const auto& value = intent.*(dimension.intent);
        if (value && !dimension.zero_allowed && value->metres <= default_geometry_tolerance_metres)
            invalid("Roof profile dimension must be positive");
        result[dimension.wire] = value ? quantity(*value) : Json(nullptr);
    }
    if (result.dump().size() > proof_limit) invalid("Roof profile edit proof byte budget exceeded");
    return result;
}

RoofProfileEditIntent decode_roof_profile_edit_intent(const nlohmann::json& value) {
    if (value.dump().size() > proof_limit) invalid("Roof profile edit proof byte budget exceeded");
    keys(value, {"version", "roof_id", "length", "span", "rise", "overhang", "thickness"});
    if (!version_one(value.at("version"))) invalid("Roof profile edit version is unsupported");
    RoofProfileEditIntent result;
    result.roof_id = identity(value.at("roof_id"));
    for (const auto& dimension : dimensions)
        if (!value.at(dimension.wire).is_null()) {
            auto parsed = quantity(value.at(dimension.wire));
            if (!dimension.zero_allowed && parsed.metres <= default_geometry_tolerance_metres)
                invalid("Roof profile dimension must be positive");
            result.*(dimension.intent) = std::move(parsed);
        }
    if (!any(result)) invalid("Roof profile edit requires an authored dimension");
    return result;
}

void validate_roof_profile_source_entity(const Entity& source) {
    const auto object = decode_roof_entity(source);
    if (!source.extensions.is_object()) invalid("Roof profile extensions must be an object");
    if (const auto values = entries(source))
        for (const auto& [pointer, raw] : values->items()) {
            if (!known_pointer(pointer) || !known_receipt(raw)) continue;
            const auto actual = actual_scalar(source, pointer);
            if (!actual) invalid("Roof profile quantity receipt is dangling");
            (void)admitted_receipt(raw, number(*actual));
        }
    opening_receipts(source);
    (void)make_roof_shape(object);
}

Entity replay_roof_profile_entity(const Entity& source, const RoofProfileEditIntent& intent) {
    (void)encode_roof_profile_edit_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof profile target differs from its actual source identity");
    validate_roof_profile_source_entity(source);
    auto result = source;
    bool changed = false, pitch_changed = false;
    const bool panel = source.properties.at("form") == "sloped_roof_panel";
    for (const auto& dimension : dimensions) {
        const auto& value = intent.*(dimension.intent);
        if (!value) continue;
        const auto name = canonical(source, dimension);
        if (number(source.properties.at(name)) == value->metres) continue;
        result.properties[name] = value->metres;
        replace_receipt(result, "/" + std::string(name), *value);
        changed = true;
        pitch_changed = pitch_changed || dimension.intent == &RoofProfileEditIntent::rise ||
            (panel ? dimension.intent == &RoofProfileEditIntent::length : dimension.intent == &RoofProfileEditIntent::span);
    }
    if (!changed) return source;
    if (pitch_changed) {
        const double run = panel ? number(result.properties.at("run_m")) : number(result.properties.at("span_m")) * 0.5;
        result.properties["pitch_rad"] = std::atan2(number(result.properties.at("rise_m")), run);
    }
    validate_roof_profile_source_entity(result);
    return result;
}

std::map<std::string, Entity, std::less<>> replay_roof_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofProfileEditIntent>& intents) {
    if (intents.size() > collection_limit) invalid("Roof profile target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    std::set<std::string, std::less<>> targets;
    auto result = source;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_roof_profile_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Roof profile batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.roof_id).second) invalid("Roof profile edit contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof profile target is inactive in the saved design");
        const auto found = source.find(intent.roof_id);
        if (found == source.end() || found->second.id != found->first)
            invalid("Roof profile target is missing or has inconsistent identity");
        (void)make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second)));
        result.at(intent.roof_id) = replay_roof_profile_entity(found->second, intent);
    }
    for (const auto& target : targets)
        (void)make_roof_shape(decode_roof_entity(resolve_vertical_placement(result, result.at(target))));
    return result;
}

std::optional<RoofProfileEditIntent> capture_roof_profile_edit(const Entity& original, const Entity& candidate) {
    validate_roof_profile_source_entity(original);
    if (exact(original, candidate)) return std::nullopt;
    validate_roof_profile_source_entity(candidate);
    if (original.properties.at("form") != candidate.properties.at("form"))
        invalid("Roof profile capture cannot change roof form");
    RoofProfileEditIntent intent;
    intent.roof_id = original.id;
    auto normalized = candidate;
    const auto retain_equal_number=[](Json& value,const Json& before) {
        if (value.is_number() && before.is_number() && number(value)==number(before)) value=before;
    };
    // Historical roof wires admit integer as well as floating numbers. A
    // canonical editor may re-encode an untouched zero as 0.0; that cannot
    // grant authority for a pose, pitch or opening change.
    for (const auto* key:{"orientation_rad","pitch_rad"})
        retain_equal_number(normalized.properties.at(key),original.properties.at(key));
    for (std::size_t coordinate=0;coordinate<3;++coordinate)
        retain_equal_number(normalized.properties.at("base_position_m").at(coordinate),
            original.properties.at("base_position_m").at(coordinate));
    if (original.properties.contains("roof_openings") && normalized.properties.contains("roof_openings")) {
        auto& rows=normalized.properties.at("roof_openings");
        const auto& before=original.properties.at("roof_openings");
        for (std::size_t index=0;index<std::min(rows.size(),before.size());++index) {
            if (rows.at(index).at("id")!=before.at(index).at("id")) continue;
            for (const auto* key:{"x_m","y_m","width_m","depth_m"})
                retain_equal_number(rows.at(index).at(key),before.at(index).at(key));
        }
    }
    for (const auto& dimension : dimensions) {
        const auto name = canonical(original, dimension);
        const double before = number(original.properties.at(name)), after = number(candidate.properties.at(name));
        if (before == after) {
            // Equal actual values retain the source's exact JSON number type.
            // No receipt, pitch, roster, pose or extension is normalized away.
            normalized.properties[name] = original.properties.at(name);
            normalize_unchanged_receipt(normalized,original,"/"+std::string(name),after);
            continue;
        }
        intent.*(dimension.intent) = captured_quantity(original, candidate, "/" + std::string(name), after);
    }
    const auto expected = any(intent) ? replay_roof_profile_entity(original, intent) : original;
    if (!exact(normalized, expected)) invalid("Roof profile candidate differs from independent typed replay");
    if (exact(expected, original)) return std::nullopt;
    return intent;
}
} // namespace sketch
