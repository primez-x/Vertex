#include "sketch/phase_slab_profile_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"

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
} // namespace

nlohmann::json encode_slab_profile_edit_intent(const SlabProfileEditIntent& intent) {
    (void)identity(intent.slab_id);
    if (intent.layer_thicknesses &&
        (intent.layer_thicknesses->empty() || intent.layer_thicknesses->size() > layer_limit))
        invalid("Slab profile layer inventory budget is invalid");
    if (!any(intent)) invalid("Slab profile edit requires an authored dimension");
    Json result{{"version", retains_layer_thickness(intent) ? 2 : 1}, {"slab_id", intent.slab_id},
        {"thickness", intent.thickness ? quantity(*intent.thickness, true) : Json(nullptr)},
        {"elevation", intent.elevation ? quantity(*intent.elevation, false) : Json(nullptr)},
        {"layer_thicknesses", nullptr}};
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
    keys(value, {"version", "slab_id", "thickness", "elevation", "layer_thicknesses"});
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || (version != 1 && version != 2))
        invalid("Slab profile edit version is unsupported");
    const bool retained_layers = version == 2;
    SlabProfileEditIntent result;
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
            const bool retain = retained_layers && row.at("thickness").is_null();
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
    const auto scope = constraint_phase_scope(source);
    std::set<std::string, std::less<>> targets;
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
    }
    for (const auto& target : targets)
        (void)make_slab(native_slab(resolve_vertical_placement(result, result.at(target))));
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
