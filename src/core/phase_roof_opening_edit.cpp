#include "sketch/phase_roof_opening_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_roof_form_edit.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"

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
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t opening_limit = 256;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;
struct SkylightDimension { const char* scalar; bool positive; };
constexpr std::array skylight_dimensions{
    SkylightDimension{"frame_width_m", true}, SkylightDimension{"curb_height_m", false},
    SkylightDimension{"glazing_thickness_m", true}};

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof opening edit fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof opening edit field is missing");
}
const Json* field(const Json& value, const std::string& key) {
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Roof opening identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof opening identity is invalid");
    return result;
}
bool version_one(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 1;
}
bool exact(const Json& left, const Json& right) { return left == right && left.dump() == right.dump(); }
bool exact(const Entity& left, const Entity& right) {
    return left == right && exact(left.properties, right.properties) && exact(left.extensions, right.extensions);
}
double number(const Json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) invalid("Roof opening scalar must be finite");
    return value.get<double>();
}
void skylight_profile(const Json& value) {
    keys(value, {"version", "frame_width_m", "curb_height_m", "glazing_thickness_m"});
    if (!version_one(value.at("version"))) invalid("Roof skylight profile version is unsupported");
    if (number(value.at("frame_width_m")) <= default_geometry_tolerance_metres ||
        number(value.at("curb_height_m")) < 0.0 ||
        number(value.at("glazing_thickness_m")) <= default_geometry_tolerance_metres)
        invalid("Roof skylight profile dimensions are outside their admitted range");
}
bool same_skylight(const Json* before, const Json* after) {
    if (!before || !after) return !before && !after;
    skylight_profile(*before);
    skylight_profile(*after);
    for (const auto* key : {"frame_width_m", "curb_height_m", "glazing_thickness_m"})
        if (number(before->at(key)) != number(after->at(key))) return false;
    return true;
}
bool skylight_schema(const RoofOpeningEditIntent& intent) {
    return intent.uses_skylight_schema || intent.uses_clone_schema || std::any_of(intent.upserts.begin(), intent.upserts.end(),
        [](const auto& upsert) { return upsert.skylight.has_value() || upsert.clone_source.has_value(); });
}
bool clone_schema(const RoofOpeningEditIntent& intent) {
    return intent.uses_clone_schema || std::any_of(intent.upserts.begin(), intent.upserts.end(),
        [](const auto& upsert) { return upsert.clone_source.has_value(); });
}
const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof opening quantity unit is unsupported");
}
Unit unit(const Json& value) {
    if (!value.is_string()) invalid("Roof opening quantity unit must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text == "m") return Unit::metre;
    if (text == "mm") return Unit::millimetre;
    if (text == "cm") return Unit::centimetre;
    if (text == "ft") return Unit::foot;
    if (text == "in") return Unit::inch;
    invalid("Roof opening quantity unit is unsupported");
}
void receipt_budget(const Json& value) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit || value.dump().size() > proof_limit)
        invalid("Roof opening quantity receipt budget exceeded");
}
Quantity quantity(const Json& value, bool positive, bool strict = true) {
    if (strict) {
        keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
        keys(value.at("exact_metres"), {"numerator", "denominator"});
    }
    receipt_budget(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || (positive && result.metres <= 0))
        invalid("Roof opening dimension is outside its admitted range");
    return result;
}
Json quantity(const Quantity& value, bool positive) {
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)}, {"exact_metres", {
            {"numerator", value.exact_metres.numerator}, {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result, positive);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.original_expression != value.original_expression || parsed.entered_unit != value.entered_unit)
        invalid("Roof opening quantity is internally inconsistent");
    return result;
}
RoofOpeningQuantityInput input(const Json& value, bool positive) {
    keys(value, {"quantity", "default_unit"});
    RoofOpeningQuantityInput result{quantity(value.at("quantity"), positive), unit(value.at("default_unit"))};
    const auto parsed = parse_quantity(result.quantity.original_expression, result.default_unit);
    if (parsed.metres != result.quantity.metres || parsed.exact_metres != result.quantity.exact_metres ||
        parsed.entered_unit != result.quantity.entered_unit)
        invalid("Roof opening default unit does not reproduce its exact quantity");
    return result;
}
Json input(const RoofOpeningQuantityInput& value, bool positive) {
    Json result{{"quantity", quantity(value.quantity, positive)}, {"default_unit", unit_name(value.default_unit)}};
    (void)input(result, positive);
    return result;
}
struct Dimension {
    const char* wire;
    const char* scalar;
    bool positive;
    std::optional<RoofOpeningQuantityInput> RoofOpeningUpsertIntent::* member;
};
constexpr std::array dimensions{
    Dimension{"x", "x_m", false, &RoofOpeningUpsertIntent::x},
    Dimension{"y", "y_m", false, &RoofOpeningUpsertIntent::y},
    Dimension{"width", "width_m", true, &RoofOpeningUpsertIntent::width},
    Dimension{"depth", "depth_m", true, &RoofOpeningUpsertIntent::depth}};
bool any(const RoofOpeningUpsertIntent& value) {
    return value.skylight || value.clone_source || std::any_of(dimensions.begin(), dimensions.end(), [&](const auto& d) { return (value.*(d.member)).has_value(); });
}
bool all(const RoofOpeningUpsertIntent& value) {
    return std::all_of(dimensions.begin(), dimensions.end(), [&](const auto& d) { return (value.*(d.member)).has_value(); });
}
Json roster(const Entity& entity) {
    const auto rows = field(entity.properties, "roof_openings");
    return rows ? *rows : Json::array();
}
using RowPositions = std::map<std::string, std::size_t, std::less<>>;
RowPositions positions(const Json& rows) {
    if (!rows.is_array() || rows.size() > opening_limit) invalid("Roof opening roster budget exceeded");
    RowPositions result;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (!result.emplace(identity(rows.at(i).at("id")), i).second)
            invalid("Roof opening source has duplicate child identities");
    return result;
}

// Conservative read-only reservation includes opaque keys and values. Nothing
// outside schema-owned receipts is rewritten by this inspection.
struct Strings {
    Ids values;
    std::size_t nodes{}, bytes{};
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 4 * 1024 * 1024) invalid("Roof opening source JSON budget exceeded");
        const auto add = [&](const std::string& text) {
            if (text.size() > 64 * 1024 * 1024 - bytes) invalid("Roof opening source string budget exceeded");
            bytes += text.size(); values.insert(text);
        };
        if (value.is_string()) add(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) { add(key); read(child, depth + 1); }
    }
    void read(const Entity& entity) {
        values.insert(entity.id); values.insert(entity.type);
        for (const auto* key : {"id", "type", "properties", "required", "extensions"}) values.insert(key);
        read(entity.properties); read(entity.extensions);
    }
};
bool known_receipt(const Json& value) {
    if (!value.is_object()) invalid("Roof opening indexed receipt must be an object");
    const auto version = field(value, "version");
    if (!version || (!version->is_number_integer() && !version->is_number_unsigned()) ||
        (version->is_number_integer() && !version->is_number_unsigned() && version->get<std::int64_t>() < 0))
        invalid("Roof opening indexed receipt version is invalid");
    return version_one(*version);
}
bool receipt_core(const Json& value, bool raw = false) {
    if (!value.is_object()) return true;
    for (const auto* key : {"version", "original_expression", raw ? "default_unit" : "entered_unit"})
        if (value.contains(key)) return true;
    const auto rational = field(value, "exact_metres");
    return rational && (!rational->is_object() || rational->contains("numerator") || rational->contains("denominator"));
}
void erase_receipt_core(Json& value, bool raw) {
    for (const auto* key : {"version", "original_expression", raw ? "default_unit" : "entered_unit"}) value.erase(key);
    if (auto rational = value.find("exact_metres"); rational != value.end()) {
        rational->erase("numerator"); rational->erase("denominator");
        if (rational->empty()) value.erase(rational);
    }
}
bool known_raw(const Json& value) {
    if (!value.is_object()) return false;
    const auto version = field(value, "version");
    return !version || version_one(*version);
}
bool known_extension(const Json& value) {
    const auto version = field(value, "version");
    return value.is_object() && version && version_one(*version);
}
RoofOpeningQuantityInput raw_input(const Json& value, double metres, bool positive) {
    receipt_budget(value);
    if (!known_raw(value)) invalid("Roof opening cannot author a future child receipt");
    const auto declared_unit = field(value, "default_unit"), rational = field(value, "exact_metres");
    if (!declared_unit || !rational) invalid("Roof opening child receipt fields are missing");
    const auto default_unit = unit(*declared_unit);
    const auto parsed = parse_quantity(value.at("original_expression").get_ref<const std::string&>(), default_unit);
    const Json encoded{{"version", 1}, {"original_expression", parsed.original_expression},
        {"entered_unit", unit_name(parsed.entered_unit)}, {"exact_metres", *rational}};
    const auto admitted = quantity(encoded, positive, false);
    if (admitted.metres != metres) invalid("Roof opening child receipt is stale");
    return {admitted, default_unit};
}
RoofOpeningQuantityInput numeric_input(double metres, bool positive) {
    std::array<char, 64> buffer{};
    // Start with the full double precision. Some redundant decimal tails need
    // a denominator beyond int64; admit a shorter spelling only if it returns
    // exactly the same actual scalar, never a rounded replacement.
    for (int precision = 17; precision > 0; --precision) {
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
            metres, std::chars_format::general, precision);
        if (converted.ec != std::errc{}) continue;
        try {
            RoofOpeningQuantityInput result{parse_quantity(std::string(buffer.data(), converted.ptr) + " m", Unit::metre), Unit::metre};
            if (result.quantity.metres != metres) continue;
            (void)input(result, positive);
            return result;
        } catch (const std::invalid_argument&) { /* Try the next exact bounded spelling. */ }
          catch (const std::overflow_error&) { /* The rational may reduce at shorter precision. */ }
    }
    invalid("Roof skylight scalar has no exact bounded metre input");
}
void write_raw_core(Json& receipt, const RoofOpeningQuantityInput& value) {
    receipt["original_expression"] = value.quantity.original_expression;
    receipt["default_unit"] = unit_name(value.default_unit);
    if (!receipt.contains("exact_metres")) receipt["exact_metres"] = Json::object();
    if (!receipt.at("exact_metres").is_object()) invalid("Roof opening receipt rational must be an object");
    receipt["exact_metres"]["numerator"] = value.quantity.exact_metres.numerator;
    receipt["exact_metres"]["denominator"] = value.quantity.exact_metres.denominator;
}
void admit(const Entity& entity) {
    validate_roof_profile_source_entity(entity);
    const auto rows = roster(entity);
    const auto children = positions(rows);
    if (const auto values = field(entity.properties, "quantity_entries")) {
        if (!values->is_object() || values->size() > collection_limit) invalid("Roof opening quantity_entries budget exceeded");
        constexpr std::string_view prefix = "/roof_openings/";
        for (const auto& [pointer, receipt] : values->items()) {
            const std::string_view path(pointer);
            if (!path.starts_with(prefix)) continue;
            const auto tail = path.substr(prefix.size());
            const auto slash = tail.find('/');
            if (slash == std::string_view::npos) continue;
            const auto suffix = tail.substr(slash + 1);
            const auto dimension = std::find_if(skylight_dimensions.begin(), skylight_dimensions.end(),
                [&](const auto& d) { return suffix == std::string("skylight/") + d.scalar; });
            if (dimension == skylight_dimensions.end() || !receipt_core(receipt) || !known_receipt(receipt)) continue;
            const auto token = tail.substr(0, slash);
            std::size_t index = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
            if (token.empty() || (token.size() > 1 && token.front() == '0') || parsed.ec != std::errc{} ||
                parsed.ptr != token.data() + token.size() || index >= rows.size())
                invalid("Roof skylight indexed receipt has no existing child index");
            const auto profile = field(rows.at(index), "skylight");
            if (!profile) invalid("Roof skylight indexed receipt is dangling");
            if (quantity(receipt, dimension->positive, false).metres != number(profile->at(dimension->scalar)))
                invalid("Roof skylight indexed receipt is stale");
        }
    }
    const auto extension = field(entity.extensions, "roof_opening_input");
    if (!extension || !known_extension(*extension)) return;
    const auto entries = field(*extension, "entries");
    if (!entries || !entries->is_object() || entries->size() > collection_limit)
        invalid("Roof opening input entries must be a bounded object");
    for (const auto& [id, receipts] : entries->items()) {
        if (!receipts.is_object()) {
            if (children.contains(id)) invalid("Roof opening child receipts must be an object");
            continue;
        }
        if (!known_raw(receipts)) continue; // A future child envelope is wholly opaque.
        for (const auto& d : dimensions) {
            const auto raw = field(receipts, d.scalar);
            if (!raw) continue;
            if (!known_raw(*raw) || !receipt_core(*raw, true)) continue;
            const auto child = children.find(id);
            if (child == children.end()) invalid("Roof opening child receipt is dangling");
            (void)raw_input(*raw, number(rows.at(child->second).at(d.scalar)), d.positive);
        }
        const auto profile_receipts = field(receipts, "skylight");
        if (!profile_receipts || !known_raw(*profile_receipts)) continue;
        for (const auto& d : skylight_dimensions) {
            const auto raw = field(*profile_receipts, d.scalar);
            if (!raw || !known_raw(*raw) || !receipt_core(*raw, true)) continue;
            const auto child = children.find(id);
            if (child == children.end()) invalid("Roof skylight child receipt is dangling");
            const auto profile = field(rows.at(child->second), "skylight");
            if (!profile) invalid("Roof skylight child receipt has no existing profile");
            (void)raw_input(*raw, number(profile->at(d.scalar)), d.positive);
        }
    }
}
const Json* child_receipts(const Entity& entity, const std::string& id) {
    const auto extension = field(entity.extensions, "roof_opening_input");
    if (!extension || !known_extension(*extension)) return nullptr;
    const auto entries = field(*extension, "entries");
    const auto result = entries ? field(*entries, id) : nullptr;
    return result && known_raw(*result) ? result : nullptr;
}
void write_child_receipt(Entity& entity, const std::string& id, const Dimension& dimension,
    const RoofOpeningQuantityInput& value) {
    auto extension = entity.extensions.find("roof_opening_input");
    if (extension == entity.extensions.end()) {
        entity.extensions["roof_opening_input"] = {{"version", 1}, {"entries", Json::object()}};
        extension = entity.extensions.find("roof_opening_input");
    }
    if (!known_extension(*extension)) invalid("Roof opening edit cannot affect an opaque input envelope");
    auto& entries = extension->at("entries");
    auto child = entries.find(id);
    if (child == entries.end()) { entries[id] = Json::object(); child = entries.find(id); }
    if (entries.size() > collection_limit) invalid("Roof opening input entry budget exceeded");
    if (!known_raw(*child)) invalid("Roof opening edit cannot replace opaque child receipt data");
    auto receipt = child->find(dimension.scalar);
    if (receipt == child->end()) { (*child)[dimension.scalar] = Json::object(); receipt = child->find(dimension.scalar); }
    else if (!known_raw(*receipt)) invalid("Roof opening edit cannot replace an opaque future child receipt");
    write_raw_core(*receipt, value);
}
void edit_skylight_receipts(Entity& entity, const std::string& id, const Json* before, const Json* after) {
    const auto extension = entity.extensions.find("roof_opening_input");
    if (extension == entity.extensions.end()) return;
    if (!known_extension(*extension)) invalid("Roof skylight edit cannot affect an opaque input envelope");
    auto& entries = extension->at("entries");
    const auto child = entries.find(id);
    if (child == entries.end()) return;
    if (!known_raw(*child)) invalid("Roof skylight edit cannot affect an opaque child input envelope");
    const auto receipts = child->find("skylight");
    if (receipts == child->end()) return;
    if (!known_raw(*receipts)) invalid("Roof skylight edit cannot affect an opaque profile input envelope");
    for (const auto& d : skylight_dimensions) {
        if (before && after && number(before->at(d.scalar)) == number(after->at(d.scalar))) continue;
        const auto raw = receipts->find(d.scalar);
        if (raw == receipts->end()) continue;
        if (!known_raw(*raw)) invalid("Roof skylight edit cannot affect a future child receipt");
        if (after && receipt_core(*raw, true)) write_raw_core(*raw, numeric_input(number(after->at(d.scalar)), d.positive));
        else if (!after) {
            erase_receipt_core(*raw, true);
            if (raw->empty()) receipts->erase(raw);
        }
    }
    if (receipts->empty()) child->erase(receipts);
}
void remove_child_receipt(Entity& entity, const std::string& id) {
    const auto extension = entity.extensions.find("roof_opening_input");
    if (extension == entity.extensions.end()) return;
    if (!known_extension(*extension)) invalid("Roof opening removal cannot affect an opaque input envelope");
    auto& entries = extension->at("entries");
    if (const auto child = entries.find(id); child != entries.end()) {
        if (!known_raw(*child)) invalid("Roof opening removal cannot erase opaque child receipt data");
        for (const auto& d : dimensions)
            if (const auto receipt = field(*child, d.scalar); receipt && !known_raw(*receipt))
                invalid("Roof opening removal cannot erase a future child receipt");
        edit_skylight_receipts(entity, id, nullptr, nullptr);
        for (const auto& d : dimensions) {
            const auto receipt = child->find(d.scalar);
            if (receipt == child->end()) continue;
            auto residue = *receipt;
            erase_receipt_core(residue, true);
            if (!residue.empty()) invalid("Roof opening receipt annotation metadata requires resolution before deleting its child");
            child->erase(receipt);
        }
        child->erase("version"); // The admitted version-one child envelope is owned.
        if (!child->empty())
            invalid("Roof opening child annotation metadata requires resolution before deleting its child");
        entries.erase(id);
    }
}
void normalize_unchanged_child_receipt(Entity& candidate, const Entity& source,
    const std::string& id, const Dimension& dimension, double metres) {
    const auto before = child_receipts(source, id), after = child_receipts(candidate, id);
    const auto old_receipt = before ? field(*before, dimension.scalar) : nullptr;
    const auto new_receipt = after ? field(*after, dimension.scalar) : nullptr;
    if (!new_receipt || (old_receipt && exact(*old_receipt, *new_receipt))) return;
    const auto entered = raw_input(*new_receipt, metres, dimension.positive);
    auto permitted = source;
    write_child_receipt(permitted, id, dimension, entered);
    if (!exact(*new_receipt, *field(*child_receipts(permitted, id), dimension.scalar)))
        invalid("Equivalent roof opening input cannot change opaque quantity metadata");
    auto& extension = candidate.extensions.at("roof_opening_input");
    auto& entries = extension.at("entries");
    auto& child = entries.at(id);
    if (old_receipt) child[dimension.scalar] = *old_receipt;
    else child.erase(dimension.scalar);
    if (child.empty() && !before) entries.erase(id);
    if (entries.empty() && !source.extensions.contains("roof_opening_input"))
        candidate.extensions.erase("roof_opening_input");
}
const RoofOpeningQuantityInput* authored_input(const RoofOpeningEditIntent& intent,
    const std::string& id, const Dimension& d) {
    for (const auto& upsert : intent.upserts)
        if (upsert.opening_id == id) {
            const auto& value = upsert.*(d.member);
            return value ? &*value : nullptr;
        }
    return nullptr;
}
void merge_quantity_core(Json& raw, const RoofOpeningQuantityInput& value, bool positive) {
    const auto encoded = quantity(value.quantity, positive);
    for (const auto* key : {"version", "original_expression", "entered_unit"}) raw[key] = encoded.at(key);
    for (const auto* key : {"numerator", "denominator"}) raw["exact_metres"][key] = encoded.at("exact_metres").at(key);
}
void normalize_unchanged_skylight_receipts(Entity& candidate, const Entity& source,
    const std::string& id, std::size_t old_index, std::size_t new_index, const SkylightDimension& d, double metres) {
    const auto before_child = child_receipts(source, id), after_child = child_receipts(candidate, id);
    const auto before = before_child ? field(*before_child, "skylight") : nullptr;
    const auto after = after_child ? field(*after_child, "skylight") : nullptr;
    const auto old_raw = before && known_raw(*before) ? field(*before, d.scalar) : nullptr;
    const auto new_raw = after && known_raw(*after) ? field(*after, d.scalar) : nullptr;
    if (new_raw && (!old_raw || !exact(*old_raw, *new_raw))) {
        const auto entered = raw_input(*new_raw, metres, d.positive);
        auto permitted = old_raw ? *old_raw : Json::object();
        if (!known_raw(permitted)) invalid("Equivalent roof skylight input cannot rewrite a future child receipt");
        write_raw_core(permitted, entered);
        if (!exact(*new_raw, permitted)) invalid("Equivalent roof skylight input cannot change opaque quantity metadata");
        auto& entries = candidate.extensions.at("roof_opening_input").at("entries");
        auto& child = entries.at(id);
        auto& profile = child.at("skylight");
        if (old_raw) profile[d.scalar] = *old_raw;
        else profile.erase(d.scalar);
        if (profile.empty() && !before) child.erase("skylight");
        if (child.empty() && !before_child) entries.erase(id);
        if (entries.empty() && !source.extensions.contains("roof_opening_input")) candidate.extensions.erase("roof_opening_input");
    }
    const auto old_values = field(source.properties, "quantity_entries"), new_values = field(candidate.properties, "quantity_entries");
    const auto suffix = "/skylight/" + std::string(d.scalar);
    const auto old_pointer = "/roof_openings/" + std::to_string(old_index) + suffix;
    const auto new_pointer = "/roof_openings/" + std::to_string(new_index) + suffix;
    const auto old_receipt = old_values ? field(*old_values, old_pointer) : nullptr;
    const auto new_receipt = new_values ? field(*new_values, new_pointer) : nullptr;
    if (!new_receipt || (old_receipt && exact(*old_receipt, *new_receipt))) return;
    if (!known_receipt(*new_receipt)) invalid("Equivalent roof skylight input cannot rewrite a future indexed receipt");
    const auto entered_quantity = quantity(*new_receipt, d.positive, false);
    if (entered_quantity.metres != metres) invalid("Equivalent roof skylight indexed receipt is stale");
    const RoofOpeningQuantityInput entered{entered_quantity, entered_quantity.entered_unit};
    auto permitted = old_receipt ? *old_receipt : Json::object();
    if (old_receipt && receipt_core(*old_receipt) && !known_receipt(*old_receipt))
        invalid("Equivalent roof skylight input cannot rewrite a future indexed receipt");
    merge_quantity_core(permitted, entered, d.positive);
    if (!exact(*new_receipt, permitted)) invalid("Equivalent roof skylight input cannot change opaque indexed metadata");
    auto& values = candidate.properties.at("quantity_entries");
    if (old_receipt) values[new_pointer] = *old_receipt;
    else values.erase(new_pointer);
    if (values.empty() && !source.properties.contains("quantity_entries")) candidate.properties.erase("quantity_entries");
}
// Indexed authority follows a source child identity, never its incidental new
// index. Opaque row pointers cannot move or silently acquire a new binding.
void indexed_receipts(const Entity& source, Entity& result, const RoofOpeningEditIntent& intent,
    const Json& before, const Json& after) {
    const auto values = field(source.properties, "quantity_entries");
    if (!values) return;
    if (!values->is_object() || values->size() > collection_limit) invalid("Roof opening quantity_entries budget exceeded");
    const auto remaining = positions(after);
    auto rebuilt = Json::object();
    constexpr std::string_view prefix = "/roof_openings/";
    for (const auto& [pointer, receipt] : values->items()) {
        const std::string_view path(pointer);
        if (!path.starts_with(prefix)) { rebuilt[pointer] = receipt; continue; }
        const auto tail = path.substr(prefix.size());
        const auto slash = tail.find('/');
        const auto token = tail.substr(0, slash);
        std::size_t index = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
        if (token.empty() || (token.size() > 1 && token.front() == '0') ||
            parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || index >= before.size()) {
            if (!exact(before, after)) invalid("Roof opening edit cannot rebind an opaque indexed pointer");
            rebuilt[pointer] = receipt; continue;
        }
        const auto id = before.at(index).at("id").get<std::string>();
        const auto retained = remaining.find(id);
        const auto suffix = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash + 1);
        const auto dimension = std::find_if(dimensions.begin(), dimensions.end(), [&](const auto& d) { return suffix == d.scalar; });
        const auto profile_dimension = std::find_if(skylight_dimensions.begin(), skylight_dimensions.end(),
            [&](const auto& d) { return suffix == std::string("skylight/") + d.scalar; });
        if (dimension == dimensions.end() && profile_dimension == skylight_dimensions.end()) {
            if (retained == remaining.end() || retained->second != index || !exact(before.at(index), after.at(retained->second)))
                invalid("Roof opening edit cannot affect an opaque indexed row pointer");
            rebuilt[pointer] = receipt; continue;
        }
        const bool profile_receipt = profile_dimension != skylight_dimensions.end();
        const auto old_profile = profile_receipt ? field(before.at(index), "skylight") : nullptr;
        const auto new_profile = profile_receipt && retained != remaining.end()
            ? field(after.at(retained->second), "skylight") : nullptr;
        const bool scalar_removed = retained == remaining.end() || (profile_receipt && old_profile && !new_profile);
        const bool scalar_changed = retained != remaining.end() && (profile_receipt
            ? (((old_profile == nullptr) != (new_profile == nullptr)) || (old_profile && new_profile &&
                number(old_profile->at(profile_dimension->scalar)) != number(new_profile->at(profile_dimension->scalar))))
            : number(before.at(index).at(dimension->scalar)) != number(after.at(retained->second).at(dimension->scalar)));
        // Core-free remnants retain annotations under the same child identity.
        const bool has_core = receipt_core(receipt);
        const bool understood = !has_core || known_receipt(receipt);
        if (!understood) {
            if (scalar_removed || retained->second != index || scalar_changed)
                invalid("Roof opening edit cannot affect a future indexed receipt");
            rebuilt[pointer] = receipt; continue;
        }
        auto raw = receipt;
        if (scalar_removed) {
            if (has_core) erase_receipt_core(raw, false);
            if (retained == remaining.end()) {
                if (!raw.empty()) invalid("Roof opening indexed annotation metadata requires resolution before deleting its child");
                continue;
            }
            if (raw.empty()) continue;
        } else if (scalar_changed && has_core) {
            if (profile_receipt) {
                const auto value = numeric_input(number(new_profile->at(profile_dimension->scalar)), profile_dimension->positive);
                merge_quantity_core(raw, value, profile_dimension->positive);
            } else {
                const auto value = authored_input(intent, id, *dimension);
                if (!value) invalid("Roof opening changed scalar lacks exact authored input");
                merge_quantity_core(raw, *value, dimension->positive);
            }
        }
        const auto destination = std::string(prefix) + std::to_string(retained->second) + "/" + std::string(suffix);
        if (rebuilt.contains(destination)) invalid("Roof opening indexed receipt remap collision");
        rebuilt[destination] = std::move(raw);
    }
    result.properties["quantity_entries"] = std::move(rebuilt);
}

Json clone_envelope(const Entity& roof) {
    (void)identity(roof.id);
    if (roof.type != "roof" || !roof.properties.is_object() || !roof.extensions.is_object())
        invalid("Roof opening clone requires an actual roof envelope");
    Strings budget; budget.read(roof);
    Json result{{"id", roof.id}, {"type", roof.type}, {"properties", roof.properties},
        {"required", roof.required}, {"extensions", roof.extensions}};
    if (result.dump().size() > proof_limit) invalid("Roof opening clone envelope byte budget exceeded");
    return result;
}
Entity clone_envelope(const Json& value) {
    keys(value, {"id", "type", "properties", "required", "extensions"});
    if (!value.at("type").is_string() || !value.at("required").is_boolean())
        invalid("Roof opening clone envelope fields are invalid");
    Entity result{identity(value.at("id")), value.at("type").get<std::string>(),
        value.at("properties"), value.at("required").get<bool>(), value.at("extensions")};
    (void)clone_envelope(result);
    return result;
}
struct CloneContent {
    Json row;
    std::optional<Json> child;
    // Keys are the understood scalar suffix, independent of either row index.
    Json indexed = Json::object();
};
CloneContent clone_content(const RoofOpeningUpsertIntent& upsert) {
    if (!upsert.clone_source) invalid("Roof opening clone source is missing");
    if (!all(upsert) || !upsert.skylight || upsert.skylight->is_null())
        invalid("Roof opening clone requires all four inputs and an actual skylight profile");
    const auto& source = *upsert.clone_source;
    (void)clone_envelope(source.roof);
    (void)identity(source.opening_id);
    admit(source.roof);
    const auto rows = roster(source.roof);
    const auto children = positions(rows);
    const auto found = children.find(source.opening_id);
    if (found == children.end()) invalid("Roof opening clone requires an actual source child");
    CloneContent result{rows.at(found->second), std::nullopt, Json::object()};
    const auto profile = field(result.row, "skylight");
    if (!profile || !exact(*profile, *upsert.skylight))
        invalid("Roof opening clone must retain its actual source skylight profile");
    skylight_profile(*profile);
    if (const auto extension = field(source.roof.extensions, "roof_opening_input")) {
        if (!known_extension(*extension)) invalid("Roof opening clone cannot bind a future input envelope");
        const auto& entries = extension->at("entries");
        if (const auto child = field(entries, source.opening_id)) {
            if (!known_raw(*child)) invalid("Roof opening clone cannot bind future child receipts");
            for (const auto& d : dimensions)
                if (const auto raw = field(*child, d.scalar)) {
                    if (!known_raw(*raw)) invalid("Roof opening clone cannot bind a future child receipt");
                    if (receipt_core(*raw, true)) (void)raw_input(*raw, number(result.row.at(d.scalar)), d.positive);
                }
            if (const auto receipts = field(*child, "skylight")) {
                if (!known_raw(*receipts)) invalid("Roof opening clone cannot bind future profile receipts");
                for (const auto& d : skylight_dimensions)
                    if (const auto raw = field(*receipts, d.scalar)) {
                        if (!known_raw(*raw)) invalid("Roof opening clone cannot bind a future profile receipt");
                        if (receipt_core(*raw, true)) (void)raw_input(*raw, number(profile->at(d.scalar)), d.positive);
                    }
            }
            result.child = *child;
        }
    }
    if (const auto values = field(source.roof.properties, "quantity_entries")) {
        constexpr std::string_view prefix = "/roof_openings/";
        for (const auto& [pointer, raw] : values->items()) {
            const std::string_view path(pointer);
            if (!path.starts_with(prefix)) continue;
            const auto tail = path.substr(prefix.size());
            const auto slash = tail.find('/');
            const auto token = tail.substr(0, slash);
            std::size_t index = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
            if (token.empty() || (token.size() > 1 && token.front() == '0') || parsed.ec != std::errc{} ||
                parsed.ptr != token.data() + token.size() || index >= rows.size())
                invalid("Roof opening clone cannot resolve an opaque indexed binding");
            if (index != found->second) continue;
            const auto suffix = slash == std::string_view::npos ? std::string_view{} : tail.substr(slash + 1);
            const auto dimension = std::find_if(dimensions.begin(), dimensions.end(), [&](const auto& d) { return suffix == d.scalar; });
            const auto profile_dimension = std::find_if(skylight_dimensions.begin(), skylight_dimensions.end(),
                [&](const auto& d) { return suffix == std::string("skylight/") + d.scalar; });
            if (dimension == dimensions.end() && profile_dimension == skylight_dimensions.end())
                invalid("Roof opening clone cannot retarget an opaque indexed row binding");
            if (!raw.is_object() || (receipt_core(raw) && !known_receipt(raw)))
                invalid("Roof opening clone cannot retarget a future indexed receipt");
            auto receipt = raw;
            if (receipt_core(receipt)) {
                const bool positive = dimension != dimensions.end() ? dimension->positive : profile_dimension->positive;
                const double metres = dimension != dimensions.end() ? number(result.row.at(dimension->scalar))
                    : number(profile->at(profile_dimension->scalar));
                if (quantity(receipt, positive, false).metres != metres)
                    invalid("Roof opening clone indexed receipt is stale");
                if (dimension != dimensions.end() &&
                    metres != (upsert.*(dimension->member))->quantity.metres)
                    merge_quantity_core(receipt, *(upsert.*(dimension->member)), positive);
            }
            result.indexed[std::string(suffix)] = std::move(receipt);
        }
    }
    return result;
}
void clone_child_receipts(Entity& result, const std::string& id, const CloneContent& content) {
    if (!content.child) return;
    auto extension = result.extensions.find("roof_opening_input");
    if (extension == result.extensions.end()) {
        result.extensions["roof_opening_input"] = {{"version", 1}, {"entries", Json::object()}};
        extension = result.extensions.find("roof_opening_input");
    }
    if (!known_extension(*extension)) invalid("Roof opening clone cannot affect an opaque destination input envelope");
    auto& entries = extension->at("entries");
    if (entries.contains(id)) invalid("Roof opening clone destination receipt identity is occupied");
    entries[id] = *content.child;
    if (entries.size() > collection_limit) invalid("Roof opening clone child receipt budget exceeded");
}
void clone_indexed_receipts(Entity& result, const std::vector<std::pair<std::string, CloneContent>>& clones, const Json& after) {
    const auto children = positions(after);
    for (const auto& [id, content] : clones) {
        if (content.indexed.empty()) continue;
        if (!result.properties.contains("quantity_entries")) result.properties["quantity_entries"] = Json::object();
        auto& values = result.properties.at("quantity_entries");
        for (const auto& [suffix, receipt] : content.indexed.items()) {
            const auto pointer = "/roof_openings/" + std::to_string(children.at(id)) + "/" + suffix;
            if (values.contains(pointer)) invalid("Roof opening clone indexed receipt binding is occupied");
            values[pointer] = receipt;
        }
        if (values.size() > collection_limit) invalid("Roof opening clone indexed receipt budget exceeded");
    }
}
RoofOpeningQuantityInput captured_input(const Entity& candidate, const std::string& id,
    const Dimension& d, double metres) {
    const auto receipts = child_receipts(candidate, id);
    if (const auto raw = receipts ? field(*receipts, d.scalar) : nullptr)
        return raw_input(*raw, metres, d.positive);
    invalid("A changed or new roof opening dimension requires its exact entered input receipt");
}
} // namespace

nlohmann::json encode_roof_opening_edit_intent(const RoofOpeningEditIntent& intent) {
    (void)identity(intent.roof_id);
    if (intent.upserts.size() > opening_limit || intent.removed_opening_ids.size() > opening_limit ||
        (intent.upserts.empty() && intent.removed_opening_ids.empty())) invalid("Roof opening edit roster budget is invalid");
    const bool profiles = skylight_schema(intent);
    const bool clones = clone_schema(intent);
    Json result{{"version", clones ? 3 : profiles ? 2 : 1}, {"roof_id", intent.roof_id}, {"upserts", Json::array()}, {"removed_opening_ids", Json::array()}};
    Ids children;
    for (const auto& upsert : intent.upserts) {
        (void)identity(upsert.opening_id);
        if (!children.insert(upsert.opening_id).second || (!profiles && !any(upsert))) invalid("Roof opening edit has a duplicate or empty upsert");
        Json entry{{"opening_id", upsert.opening_id}};
        for (const auto& d : dimensions) {
            const auto& value = upsert.*(d.member);
            entry[d.wire] = value ? input(*value, d.positive) : Json(nullptr);
        }
        if (profiles) {
            entry["skylight_edit"] = nullptr;
            if (upsert.skylight) {
                if (!upsert.skylight->is_null()) skylight_profile(*upsert.skylight);
                entry["skylight_edit"] = Json{{"value", *upsert.skylight}};
            }
        }
        if (clones) {
            entry["clone_source"] = nullptr;
            if (upsert.clone_source) {
                (void)clone_content(upsert);
                entry["clone_source"] = Json{{"roof", clone_envelope(upsert.clone_source->roof)},
                    {"opening_id", upsert.clone_source->opening_id}};
            }
        }
        result["upserts"].push_back(std::move(entry));
    }
    for (const auto& id : intent.removed_opening_ids) {
        (void)identity(id);
        if (!children.insert(id).second) invalid("Roof opening edit has duplicate or conflicting child operations");
        result["removed_opening_ids"].push_back(id);
    }
    if (result.dump().size() > proof_limit) invalid("Roof opening edit proof byte budget exceeded");
    return result;
}
RoofOpeningEditIntent decode_roof_opening_edit_intent(const nlohmann::json& value) {
    if (value.dump().size() > proof_limit) invalid("Roof opening edit proof byte budget exceeded");
    keys(value, {"version", "roof_id", "upserts", "removed_opening_ids"});
    const bool clones = (value.at("version").is_number_integer() || value.at("version").is_number_unsigned()) &&
        value.at("version") == 3;
    const bool profiles = clones || ((value.at("version").is_number_integer() || value.at("version").is_number_unsigned()) &&
        value.at("version") == 2);
    if (!profiles && !version_one(value.at("version"))) invalid("Roof opening edit version is unsupported");
    const auto& upserts = value.at("upserts");
    const auto& removed = value.at("removed_opening_ids");
    if (!upserts.is_array() || !removed.is_array() || upserts.size() > opening_limit || removed.size() > opening_limit)
        invalid("Roof opening edit arrays exceed the roster budget");
    RoofOpeningEditIntent result;
    result.roof_id = identity(value.at("roof_id"));
    result.uses_skylight_schema = profiles;
    result.uses_clone_schema = clones;
    for (const auto& entry : upserts) {
        if (clones) keys(entry, {"opening_id", "x", "y", "width", "depth", "skylight_edit", "clone_source"});
        else if (profiles) keys(entry, {"opening_id", "x", "y", "width", "depth", "skylight_edit"});
        else keys(entry, {"opening_id", "x", "y", "width", "depth"});
        RoofOpeningUpsertIntent upsert;
        upsert.opening_id = identity(entry.at("opening_id"));
        for (const auto& d : dimensions)
            if (!entry.at(d.wire).is_null()) upsert.*(d.member) = input(entry.at(d.wire), d.positive);
        if (profiles && !entry.at("skylight_edit").is_null()) {
            const auto& edit = entry.at("skylight_edit");
            keys(edit, {"value"});
            if (!edit.at("value").is_null()) skylight_profile(edit.at("value"));
            upsert.skylight = edit.at("value");
        }
        if (clones && !entry.at("clone_source").is_null()) {
            const auto& clone = entry.at("clone_source");
            keys(clone, {"roof", "opening_id"});
            upsert.clone_source = RoofOpeningCloneSource{clone_envelope(clone.at("roof")), identity(clone.at("opening_id"))};
        }
        result.upserts.push_back(std::move(upsert));
    }
    for (const auto& id : removed) result.removed_opening_ids.push_back(identity(id));
    (void)encode_roof_opening_edit_intent(result);
    return result;
}
Entity stage_roof_opening_entity(const Entity& source, const RoofOpeningEditIntent& intent) {
    (void)encode_roof_opening_edit_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof opening edit target differs from actual source identity");
    admit(source);
    if (source.properties.at("version") == 3 && !skylight_schema(intent))
        invalid("Schema-three roof opening edits require version-two proof authority");
    const auto before = roster(source);
    const auto children = positions(before);
    Ids removed(intent.removed_opening_ids.begin(), intent.removed_opening_ids.end());
    for (const auto& id : removed)
        if (!children.contains(id)) invalid("Roof opening removal requires an actual existing child");
    Strings occupied; occupied.read(source);
    for (const auto& upsert : intent.upserts)
        if (upsert.clone_source) occupied.read(upsert.clone_source->roof);
    auto result = source;
    auto after = Json::array();
    for (const auto& row : before)
        if (!removed.contains(row.at("id").get<std::string>())) after.push_back(row);
    auto final_positions = positions(after);
    std::vector<std::pair<std::string, CloneContent>> clones;
    bool changed = !removed.empty();
    for (const auto& upsert : intent.upserts) {
        const auto existing = final_positions.find(upsert.opening_id);
        if (upsert.clone_source && children.contains(upsert.opening_id))
            invalid("Roof opening clone requires a new destination child identity");
        if (existing == final_positions.end()) {
            if (!all(upsert)) invalid("A new roof opening requires all four exact dimensions");
            if (occupied.values.contains(upsert.opening_id)) invalid("A new roof opening aliases retained source identity data");
            if (after.size() >= opening_limit) invalid("Roof opening result exceeds the roster budget");
            if (upsert.clone_source) {
                auto content = clone_content(upsert);
                auto row = content.row;
                row["id"] = upsert.opening_id;
                after.push_back(std::move(row));
                clone_child_receipts(result, upsert.opening_id, content);
                clones.emplace_back(upsert.opening_id, std::move(content));
            } else after.push_back({{"id", upsert.opening_id}});
            final_positions.emplace(upsert.opening_id, after.size() - 1);
            occupied.values.insert(upsert.opening_id);
            changed = true;
        }
        auto& row = after.at(final_positions.at(upsert.opening_id));
        const bool fresh = !children.contains(upsert.opening_id);
        for (const auto& d : dimensions) {
            const auto& value = upsert.*(d.member);
            if (!value || ((!fresh || upsert.clone_source) &&
                number(row.at(d.scalar)) == value->quantity.metres)) continue;
            row[d.scalar] = value->quantity.metres;
            write_child_receipt(result, upsert.opening_id, d, *value);
            changed = true;
        }
        if (upsert.skylight) {
            const auto before_profile = field(row, "skylight");
            const auto after_profile = upsert.skylight->is_null() ? nullptr : &*upsert.skylight;
            if (!same_skylight(before_profile, after_profile)) {
                edit_skylight_receipts(result, upsert.opening_id, before_profile, after_profile);
                if (after_profile && before_profile) {
                    for (const auto* key : {"frame_width_m", "curb_height_m", "glazing_thickness_m"})
                        if (number(before_profile->at(key)) != number(after_profile->at(key)))
                            row["skylight"][key] = after_profile->at(key);
                } else if (after_profile) row["skylight"] = *after_profile;
                else row.erase("skylight");
                changed = true;
            }
        }
    }
    if (!changed) return source;
    for (const auto& id : removed) remove_child_receipt(result, id);
    // Retain schema three even after the last profile/removal. Older schemas
    // promote only when an actual child/profile was authored, never for a no-op.
    const bool has_profile = std::any_of(after.begin(), after.end(), [](const auto& row) { return row.contains("skylight"); });
    result.properties["version"] = source.properties.at("version") == 3 || has_profile ? 3 : 2;
    result.properties["roof_openings"] = after;
    indexed_receipts(source, result, intent, before, after);
    clone_indexed_receipts(result, clones, after);
    return result;
}
Entity replay_roof_opening_entity(const Entity& source, const RoofOpeningEditIntent& intent) {
    auto result=stage_roof_opening_entity(source,intent);
    admit(result);
    return result;
}
std::vector<std::string> new_roof_opening_identity_ids(const Entities& source,
    const std::vector<RoofOpeningEditIntent>& intents) {
    if (intents.size() > collection_limit) invalid("Roof opening target budget exceeded");
    if (intents.empty()) return {};
    Strings occupied;
    for (const auto& [id, entity] : source) { occupied.values.insert(id); occupied.read(entity); }
    // Passive source payloads also occupy the namespace: a destination cannot
    // acquire a copied opaque reference or another transfer's source identity.
    for (const auto& intent : intents)
        for (const auto& upsert : intent.upserts)
            if (upsert.clone_source) occupied.read(upsert.clone_source->roof);
    Ids targets, fresh;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_roof_opening_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Roof opening batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.roof_id).second) invalid("Roof opening batch contains duplicate targets");
        const auto found = source.find(intent.roof_id);
        if (found == source.end() || found->first != found->second.id) invalid("Roof opening target is missing or inconsistent");
        admit(found->second);
        if (found->second.properties.at("version") == 3 && !skylight_schema(intent))
            invalid("Schema-three roof opening edits require version-two proof authority");
        const auto children = positions(roster(found->second));
        for (const auto& id : intent.removed_opening_ids)
            if (!children.contains(id)) invalid("Roof opening removal requires an actual existing child");
        for (const auto& upsert : intent.upserts) {
            if (children.contains(upsert.opening_id)) {
                if (upsert.clone_source) invalid("Roof opening clone requires a new destination child identity");
                continue;
            }
            if (!all(upsert)) invalid("A new roof opening requires all four exact dimensions");
            if (occupied.values.contains(upsert.opening_id) || !fresh.insert(upsert.opening_id).second)
                invalid("A new roof opening aliases current or authored identity data");
            if (fresh.size() > collection_limit) invalid("Roof opening fresh identity budget exceeded");
        }
    }
    return {fresh.begin(), fresh.end()};
}
Entities replay_roof_opening_entities(const Entities& source, const std::vector<RoofOpeningEditIntent>& intents) {
    if (intents.empty()) return source;
    (void)new_roof_opening_identity_ids(source, intents);
    const auto scope = constraint_phase_scope(source);
    auto result = source;
    for (const auto& intent : intents) {
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof opening target is inactive in the saved design");
        const auto& original = source.at(intent.roof_id);
        (void)make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, original)));
        result.at(intent.roof_id) = replay_roof_opening_entity(original, intent);
    }
    for (const auto& intent : intents)
        (void)make_roof_shape(decode_roof_entity(resolve_vertical_placement(result, result.at(intent.roof_id))));
    return result;
}
Entity normalize_equivalent_roof_opening_inputs(const Entity& original, const Entity& candidate, bool allow_form_change) {
    admit(original);
    admit(candidate);
    auto normalized = allow_form_change && original.properties.at("form") != candidate.properties.at("form")
        ? normalize_equivalent_roof_form_inputs(original, candidate)
        : normalize_equivalent_roof_inputs(original, candidate);
    const auto before=roster(original),after=roster(candidate);
    const auto old_positions=positions(before);
    for (std::size_t index=0;index<after.size();++index) {
        const auto id=after.at(index).at("id").get<std::string>();
        const auto old=old_positions.find(id);
        if (old==old_positions.end()) continue;
        for (const auto& d:dimensions) {
            const double metres=number(after.at(index).at(d.scalar));
            if (number(before.at(old->second).at(d.scalar))!=metres) continue;
            normalized.properties["roof_openings"][index][d.scalar]=before.at(old->second).at(d.scalar);
            normalize_unchanged_child_receipt(normalized,original,id,d,metres);
        }
        const auto old_profile = field(before.at(old->second), "skylight"), new_profile = field(after.at(index), "skylight");
        if (old_profile && new_profile)
            for (const auto& d : skylight_dimensions) {
                const double metres = number(new_profile->at(d.scalar));
                if (number(old_profile->at(d.scalar)) != metres) continue;
                normalized.properties["roof_openings"][index]["skylight"][d.scalar] = old_profile->at(d.scalar);
                normalize_unchanged_skylight_receipts(normalized, original, id, old->second, index, d, metres);
            }
    }
    return normalized;
}
std::optional<RoofOpeningEditIntent> infer_roof_opening_edit(const Entity& original, const Entity& candidate, bool allow_form_change) {
    admit(original);
    if (exact(original, candidate)) return std::nullopt;
    admit(candidate);
    if (original.id!=candidate.id || original.type!=candidate.type ||
        (!allow_form_change && original.properties.at("form")!=candidate.properties.at("form")))
        invalid("Roof opening input inference cannot change its owner or form");
    const auto before = roster(original), after = roster(candidate);
    const auto old_positions = positions(before), new_positions = positions(after);
    RoofOpeningEditIntent intent;
    intent.roof_id = original.id;
    intent.uses_skylight_schema = original.properties.at("version") == 3 || candidate.properties.at("version") == 3;
    for (const auto& row : before) {
        const auto id = row.at("id").get<std::string>();
        if (!new_positions.contains(id)) intent.removed_opening_ids.push_back(id);
    }
    for (std::size_t i = 0; i < after.size(); ++i) {
        const auto& row = after.at(i);
        const auto id = row.at("id").get<std::string>();
        const auto old = old_positions.find(id);
        RoofOpeningUpsertIntent upsert;
        upsert.opening_id = id;
        for (const auto& d : dimensions) {
            const double metres = number(row.at(d.scalar));
            if (old != old_positions.end() && number(before.at(old->second).at(d.scalar)) == metres) {
                continue;
            }
            upsert.*(d.member) = captured_input(candidate, id, d, metres);
        }
        const auto before_profile = old == old_positions.end() ? nullptr : field(before.at(old->second), "skylight");
        const auto after_profile = field(row, "skylight");
        if (!same_skylight(before_profile, after_profile))
            upsert.skylight = after_profile ? *after_profile : Json(nullptr);
        if (old == old_positions.end()) {
            // Ordinary inference has only dimension/profile authority. A new
            // row carrying passive source metadata must name that source in a
            // typed v3 intent rather than have its content silently omitted.
            Json ordinary_row{{"id", id}}, ordinary_receipts = Json::object();
            for (const auto& d : dimensions) {
                const auto& entered = *(upsert.*(d.member));
                ordinary_row[d.scalar] = entered.quantity.metres;
                ordinary_receipts[d.scalar] = Json::object();
                write_raw_core(ordinary_receipts[d.scalar], entered);
            }
            if (after_profile) ordinary_row["skylight"] = *after_profile;
            const auto receipts = child_receipts(candidate, id);
            if (!exact(row, ordinary_row) || !receipts || !exact(*receipts, ordinary_receipts))
                invalid("Roof opening transfer metadata requires an explicit version-three clone source intent");
            if (const auto values = field(candidate.properties, "quantity_entries")) {
                const auto prefix = "/roof_openings/" + std::to_string(i) + "/";
                for (const auto& [pointer, receipt] : values->items()) {
                    (void)receipt;
                    if (pointer.starts_with(prefix))
                        invalid("Roof opening transfer indexed metadata requires an explicit version-three clone source intent");
                }
            }
        }
        if (any(upsert)) intent.upserts.push_back(std::move(upsert));
    }
    if (intent.upserts.empty() && intent.removed_opening_ids.empty()) return std::nullopt;
    (void)encode_roof_opening_edit_intent(intent);
    return intent;
}
std::optional<RoofOpeningEditIntent> capture_roof_opening_edit(const Entity& original, const Entity& candidate) {
    const auto normalized=normalize_equivalent_roof_opening_inputs(original,candidate);
    if (exact(normalized,original)) return std::nullopt;
    const auto intent=infer_roof_opening_edit(original,candidate);
    const auto expected=intent ? replay_roof_opening_entity(original,*intent) : original;
    if (!exact(normalized, expected)) invalid("Roof opening candidate differs from independent typed replay");
    if (exact(expected, original)) return std::nullopt;
    return intent;
}
} // namespace sketch
