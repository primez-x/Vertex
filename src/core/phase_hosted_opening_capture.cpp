#include "sketch/phase_hosted_opening_capture.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/document_wall.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

bool exact(const Json& left, const Json& right) {
    return left == right && left.dump() == right.dump();
}

bool exact(const Entity& left, const Entity& right) {
    return left == right && exact(left.properties, right.properties) &&
        exact(left.extensions, right.extensions);
}

const Json* field(const Json& object, const std::string& key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

bool exact(const Json* left, const Json* right) {
    return left && right ? exact(*left, *right) : left == right;
}

struct Dimension {
    const char* canonical;
    const char* alias;
    bool zero_allowed;
    std::optional<Quantity> HostedOpeningProfileEditIntent::* intent;
};
constexpr std::array dimensions{
    Dimension{"offset_m", "offset", true, &HostedOpeningProfileEditIntent::offset},
    Dimension{"width_m", "width", false, &HostedOpeningProfileEditIntent::width},
    Dimension{"sill_m", "sill", true, &HostedOpeningProfileEditIntent::sill},
    Dimension{"height_m", "height", false, &HostedOpeningProfileEditIntent::height}};

double scalar(const Entity& entity, const Dimension& dimension) {
    const auto canonical = field(entity.properties, dimension.canonical);
    const auto alias = field(entity.properties, dimension.alias);
    const auto selected = canonical ? canonical : alias;
    if (!selected || !selected->is_number()) invalid("Opening capture dimension is missing or invalid");
    const auto value = selected->get<double>();
    if (!std::isfinite(value) || (dimension.zero_allowed ? value < 0 : value <= 0))
        invalid("Opening capture dimension is outside its admitted range");
    if (alias && (!alias->is_number() || alias->get<double>() != value))
        invalid("Opening capture scalar aliases disagree");
    return value;
}

const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && !result->is_object()) invalid("Opening capture quantity_entries must be an object");
    return result;
}

const Json* receipt(const Json* values, const std::string& pointer) {
    return values ? field(*values, pointer) : nullptr;
}

Quantity admitted_quantity(const Json& value, double metres) {
    const auto expression = field(value, "original_expression");
    if (!expression || !expression->is_string() ||
        expression->get_ref<const std::string&>().size() > expression_limit ||
        value.dump().size() > proof_limit)
        invalid("Opening capture quantity receipt budget exceeded");
    const auto result = decode_constraint_quantity_receipt(value);
    if (result.metres != metres) invalid("Opening capture candidate quantity receipt is stale");
    return result;
}

std::optional<Quantity> changed_quantity(const Entity& original, const Entity& candidate,
                                       const Dimension& dimension, double metres) {
    const auto before = entries(original);
    const auto after = entries(candidate);
    for (const auto* name : {dimension.canonical, dimension.alias}) {
        const auto pointer = "/" + std::string(name);
        const auto changed = receipt(after, pointer);
        if (changed && !exact(changed, receipt(before, pointer)))
            return admitted_quantity(*changed, metres);
    }
    return std::nullopt;
}

Quantity numeric_quantity(double metres) {
    // Decimal parsing uses signed int64 rationals: 10^18 is the largest plain
    // decimal denominator. Fixed conversion needs at most 309 whole digits
    // for a finite double. Both precision and buffer are finite bounds; failed
    // rational admission simply advances the search, never changes the value.
    std::array<char, 384> buffer{};
    for (int precision = 0; precision <= 18; ++precision) {
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
            metres, std::chars_format::fixed, precision);
        if (converted.ec != std::errc{}) continue;
        std::string text(buffer.data(), converted.ptr);
        if (text.find('.') != std::string::npos) {
            while (text.back() == '0') text.pop_back();
            if (text.back() == '.') text.pop_back();
        }
        text += " m";
        try {
            auto result = parse_quantity(text, Unit::metre);
            if (result.metres == metres) return result;
        } catch (const std::invalid_argument&) { /* Try the next bounded precision. */ }
          catch (const std::overflow_error&) { /* Try a representation that reduces exactly. */ }
    }
    invalid("Opening capture number has no exact bounded plain decimal quantity");
}

bool known_receipt_only(const Json& value) {
    if (!value.is_object() || value.size() != 4 || !value.contains("version") ||
        !value.contains("original_expression") || !value.contains("entered_unit") ||
        !value.contains("exact_metres")) return false;
    const auto& rational = value.at("exact_metres");
    return rational.is_object() && rational.size() == 2 &&
        rational.contains("numerator") && rational.contains("denominator");
}

void compare_candidate(const Entity& original, const Entity& candidate, const Entity& expected,
                       const std::set<std::string, std::less<>>& changed_pointers) {
    if (candidate.id != expected.id || candidate.type != expected.type ||
        candidate.required != expected.required || !exact(candidate.extensions, expected.extensions))
        invalid("Opening capture changed identity or opaque owner data");
    for (const auto& [key, value] : expected.properties.items()) {
        if (key == "quantity_entries") continue;
        if (!exact(&value, field(candidate.properties, key)))
            invalid("Opening capture candidate differs from independent typed replay");
    }
    for (const auto& [key, value] : candidate.properties.items()) {
        (void)value;
        if (key != "quantity_entries" && !expected.properties.contains(key))
            invalid("Opening capture contains an unsupported property change");
    }
    const auto before = entries(original);
    const auto after = entries(candidate);
    const auto replayed = entries(expected);
    // A legacy setter retains an empty object after receipt invalidation. Its
    // distinction from absence is authoritative whenever the source had one.
    if (before && !after) invalid("Opening capture removed the quantity_entries object");
    if (!before && after && !replayed)
        invalid("Opening capture added an unsupported quantity_entries object");
    if (!before && after && after->empty())
        invalid("Opening capture added an empty quantity_entries object");
    std::set<std::string, std::less<>> pointers;
    for (const auto* collection : {before, after, replayed})
        if (collection)
            for (const auto& [pointer, value] : collection->items()) {
                (void)value;
                pointers.insert(pointer);
            }
    for (const auto& pointer : pointers) {
        const auto old_value = receipt(before, pointer);
        const auto raw_value = receipt(after, pointer);
        const auto typed_value = receipt(replayed, pointer);
        if (exact(raw_value, typed_value)) continue;
        if (!changed_pointers.contains(pointer))
            invalid("Opening capture changed an unknown or unchanged quantity receipt");
        if (old_value && exact(raw_value, old_value)) continue;
        if (!raw_value && (!old_value || known_receipt_only(*old_value))) continue;
        invalid("Opening capture changed opaque quantity receipt fields or unsupported authority");
    }
}

std::set<std::string, std::less<>> changed_pointers(const Entity& original, const Entity& candidate) {
    std::set<std::string, std::less<>> result;
    for (const auto& dimension : dimensions) {
        if (scalar(original, dimension) == scalar(candidate, dimension)) continue;
        result.insert("/" + std::string(dimension.canonical));
        if (original.properties.contains(dimension.alias))
            result.insert("/" + std::string(dimension.alias));
    }
    return result;
}

std::size_t authored_fields(const HostedOpeningProfileEditIntent& intent) {
    return static_cast<std::size_t>(intent.offset.has_value()) + intent.width.has_value() +
        intent.sill.has_value() + intent.height.has_value() + intent.assembly.has_value() +
        intent.door_operation.has_value() + static_cast<std::size_t>(intent.clear_door_operation);
}

std::optional<OpeningAssembly> source_assembly(const Entity& original) {
    if (const auto value = field(original.properties, "opening_assembly"))
        return parse_opening_assembly(*value);
    const auto kind = field(original.properties, "opening_kind");
    if (kind && kind->is_string())
        if (const auto family = parse_opening_assembly_kind(kind->get_ref<const std::string&>()))
            return default_opening_assembly(*family);
    return std::nullopt;
}

Entity normalize_equivalent_source_profile(const Entity& original, const Entity& candidate,
                                           const Entity& replayed) {
    auto result = candidate;
    for (const auto& dimension : dimensions) {
        for (const auto* name : {dimension.canonical, dimension.alias}) {
            const auto source = field(original.properties, name);
            const auto raw = field(candidate.properties, name);
            // Existing scalar representation is preserved across an equal
            // value; an actual dimension edit cannot use this normalization.
            if (source && raw && exact(source, field(replayed.properties, name)) &&
                source->is_number() && raw->is_number() &&
                std::isfinite(source->get<double>()) && source->get<double>() == raw->get<double>())
                result.properties[name] = *source;
        }
        const auto canonical = field(candidate.properties, dimension.canonical);
        const auto source_alias = field(original.properties, dimension.alias);
        if (!original.properties.contains(dimension.canonical) && source_alias && canonical &&
            !replayed.properties.contains(dimension.canonical) && canonical->is_number() &&
            source_alias->is_number() && std::isfinite(canonical->get<double>()) &&
            canonical->get<double>() == source_alias->get<double>())
            result.properties.erase(dimension.canonical);
    }
    const auto source_profile = field(original.properties, "opening_assembly");
    const auto raw_profile = field(candidate.properties, "opening_assembly");
    if (raw_profile && exact(source_profile, field(replayed.properties, "opening_assembly"))) {
        // Both explicit schemas decode strictly. An implicit default comes only
        // from the retained source family already admitted by typed replay.
        const auto raw = parse_opening_assembly(*raw_profile);
        const auto source = source_assembly(original);
        if (source && *source == raw) {
            if (source_profile) result.properties["opening_assembly"] = *source_profile;
            else result.properties.erase("opening_assembly");
        }
    }
    const auto source_operation = field(original.properties, "door_operation");
    const auto raw_operation = field(candidate.properties, "door_operation");
    if (source_operation && raw_operation &&
        exact(source_operation, field(replayed.properties, "door_operation")) &&
        decode_door_operation(*source_operation) == decode_door_operation(*raw_operation))
        result.properties["door_operation"] = *source_operation;
    // Absence of an operation remains distinct from any explicit operation.
    // Receipts and all other fields are deliberately untouched.
    return result;
}

void seed_source_admission(const Entity& original, HostedOpeningProfileEditIntent& intent) {
    if (const auto assembly = source_assembly(original)) {
        intent.assembly = *assembly;
        return;
    }
    // A representation-only bare cut still needs independent replay admission.
    // Prefer retained exact authority, then a bounded unchanged dimension.
    const auto source_entries = entries(original);
    for (const auto& dimension : dimensions)
        for (const auto* name : {dimension.canonical, dimension.alias})
            if (const auto value = receipt(source_entries, "/" + std::string(name))) {
                intent.*(dimension.intent) = admitted_quantity(*value, scalar(original, dimension));
                return;
            }
    for (const auto& dimension : dimensions) {
        try {
            intent.*(dimension.intent) = numeric_quantity(scalar(original, dimension));
            return;
        } catch (const std::invalid_argument&) { /* Try another retained dimension. */ }
    }
    invalid("Opening capture source has no exact unchanged field for replay admission");
}
} // namespace

Quantity capture_hosted_opening_dimension_quantity(const Entity& original, std::string_view scalar_field) {
    const Dimension* selected_dimension = nullptr;
    for (const auto& dimension : dimensions) {
        if (scalar_field == dimension.canonical || scalar_field == dimension.alias) {
            selected_dimension = &dimension;
            break;
        }
    }
    if (!selected_dimension) invalid("Opening capture dimension field is unsupported");

    const auto metres = scalar(original, *selected_dimension);
    const auto source_entries = entries(original);
    const auto canonical = receipt(source_entries, "/" + std::string(selected_dimension->canonical));
    const auto alias = receipt(source_entries, "/" + std::string(selected_dimension->alias));
    std::optional<Quantity> canonical_quantity;
    std::optional<Quantity> alias_quantity;
    if (canonical) canonical_quantity = admitted_quantity(*canonical, metres);
    if (alias) alias_quantity = admitted_quantity(*alias, metres);

    Quantity quantity = canonical_quantity ? *canonical_quantity :
        alias_quantity ? *alias_quantity : numeric_quantity(metres);
    HostedOpeningProfileEditIntent hint;
    hint.opening_id = original.id;
    std::string diagnostic;
    if (!read_document_wall_id(original, hint.wall_id, diagnostic))
        invalid("Opening capture original host is missing or invalid");
    hint.*(selected_dimension->intent) = quantity;
    (void)replay_hosted_opening_profile_entity(original, hint);
    return quantity;
}

std::optional<HostedOpeningProfileEditIntent> capture_hosted_opening_profile_edit(
    const Entity& original, const Entity& candidate,
    std::optional<HostedOpeningProfileEditIntent> authored) {
    if (exact(original, candidate)) return std::nullopt;
    if (original.type != "opening" || candidate.type != "opening" ||
        !original.properties.is_object() || !candidate.properties.is_object())
        invalid("Opening capture requires an existing opening profile");
    std::string host, diagnostic;
    if (!read_document_wall_id(original, host, diagnostic))
        invalid("Opening capture original host is missing or invalid");
    const auto pointers = changed_pointers(original, candidate);
    if (authored) {
        if (authored->opening_id != original.id || authored->wall_id != host || authored_fields(*authored) != 1)
            invalid("Opening capture authored hint must bind this original and one field");
        // Admit the exact entered quantity before any numeric fallback. Replay
        // independently checks source receipts, same family and retained host.
        const auto expected = replay_hosted_opening_profile_entity(original, *authored);
        const auto normalized = normalize_equivalent_source_profile(original, candidate, expected);
        compare_candidate(original, normalized, expected, pointers);
        if (exact(normalized, original) && exact(expected, original)) return std::nullopt;
        return authored;
    }
    HostedOpeningProfileEditIntent intent;
    intent.opening_id = original.id;
    intent.wall_id = host;
    for (const auto& dimension : dimensions) {
        const auto value = scalar(candidate, dimension);
        if (value == scalar(original, dimension)) continue;
        auto quantity = changed_quantity(original, candidate, dimension, value);
        intent.*(dimension.intent) = quantity ? std::move(*quantity) : numeric_quantity(value);
    }
    const auto original_assembly = field(original.properties, "opening_assembly");
    const auto candidate_assembly = field(candidate.properties, "opening_assembly");
    if (!exact(original_assembly, candidate_assembly)) {
        if (!candidate_assembly) invalid("Opening capture cannot clear an assembly");
        intent.assembly = parse_opening_assembly(*candidate_assembly);
    }
    const auto original_operation = field(original.properties, "door_operation");
    const auto candidate_operation = field(candidate.properties, "door_operation");
    if (!exact(original_operation, candidate_operation)) {
        if (candidate_operation) intent.door_operation = decode_door_operation(*candidate_operation);
        else {
            // Decode the original first: omission cannot delete unknown fields.
            (void)decode_door_operation(*original_operation);
            intent.clear_door_operation = true;
        }
    }
    if (authored_fields(intent) == 0) seed_source_admission(original, intent);
    const auto expected = replay_hosted_opening_profile_entity(original, intent);
    const auto normalized = normalize_equivalent_source_profile(original, candidate, expected);
    compare_candidate(original, normalized, expected, pointers);
    if (exact(normalized, original) && exact(expected, original)) return std::nullopt;
    return intent;
}

} // namespace sketch
