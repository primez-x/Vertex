#include "sketch/phase_wall_profile_capture.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/document_wall.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t layer_limit = 4096;

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
    std::optional<Quantity> WallProfileEditIntent::* intent;
};
constexpr std::array dimensions{
    Dimension{"thickness_m", "thickness", &WallProfileEditIntent::thickness},
    Dimension{"height_m", "height", &WallProfileEditIntent::height}};

double scalar(const Entity& entity, const Dimension& dimension) {
    const auto canonical = field(entity.properties, dimension.canonical);
    const auto alias = field(entity.properties, dimension.alias);
    const auto selected = canonical ? canonical : alias;
    if (!selected || !selected->is_number()) invalid("Wall capture dimension is missing or invalid");
    const auto value = selected->get<double>();
    if (!std::isfinite(value) || value <= default_geometry_tolerance_metres)
        invalid("Wall capture dimension is outside its admitted range");
    if (alias && (!alias->is_number() || alias->get<double>() != value))
        invalid("Wall capture scalar aliases disagree");
    return value;
}

Wall wall(const Entity& entity) {
    Wall result;
    std::string diagnostic;
    if (!read_document_wall(entity, {}, result, diagnostic))
        throw std::invalid_argument("Wall capture source: " + diagnostic);
    validate_wall_semantics(result);
    if (result.layers.size() > layer_limit) invalid("Wall capture layer inventory budget exceeded");
    const double rise = result.slope_rise.value_or(0.0);
    for (const auto* name : {"slope_rise_m", "slope_rise"})
        if (const auto value = field(entity.properties, name))
            if (!value->is_number() || !std::isfinite(value->get<double>()) || value->get<double>() != rise)
                invalid("Wall capture top rise aliases disagree");
    return result;
}

const Json* entries(const Entity& entity) {
    const auto result = field(entity.properties, "quantity_entries");
    if (result && !result->is_object()) invalid("Wall capture quantity_entries must be an object");
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
        invalid("Wall capture quantity receipt budget exceeded");
    const auto result = decode_constraint_quantity_receipt(value);
    if (result.metres != metres) invalid("Wall capture quantity receipt is stale");
    return result;
}
Quantity numeric_quantity(double metres) {
    // Exact rational admission has a finite int64 decimal denominator. Try
    // bounded plain decimals, including signed/zero rise, without rounding.
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
          catch (const std::overflow_error&) { /* Try an exactly reduced representation. */ }
    }
    invalid("Wall capture number has no exact bounded plain decimal quantity");
}

Quantity captured_quantity(const Entity& original, const Entity& candidate,
    const std::vector<std::string>& pointers, double old_metres, double metres) {
    const auto before = entries(original), after = entries(candidate);
    std::optional<Quantity> changed, retained;
    for (const auto& pointer : pointers) {
        const auto old_value = receipt(before, pointer), raw_value = receipt(after, pointer);
        // Known retained authority is independently decoded even when an
        // authored update or another alias supplies the resulting quantity.
        if (old_value) {
            auto quantity = admitted_quantity(*old_value, old_metres);
            if (old_metres == metres && !retained) retained = std::move(quantity);
        }
        if (raw_value && !exact(raw_value, old_value)) {
            auto quantity = admitted_quantity(*raw_value, metres);
            if (!changed) changed = std::move(quantity);
        }
    }
    if (changed) return *changed;
    if (retained) return *retained;
    return numeric_quantity(metres);
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
        invalid("Wall capture changed identity or opaque owner data");
    for (const auto& [key, value] : expected.properties.items())
        if (key != "quantity_entries" && !exact(&value, field(candidate.properties, key)))
            invalid("Wall capture candidate differs from independent typed replay");
    for (const auto& [key, value] : candidate.properties.items()) {
        (void)value;
        if (key != "quantity_entries" && !expected.properties.contains(key))
            invalid("Wall capture contains an unsupported property change");
    }
    const auto before = entries(original), after = entries(candidate), replayed = entries(expected);
    if (before && !after) invalid("Wall capture removed the quantity_entries object");
    if (!before && after && (!replayed || after->empty()))
        invalid("Wall capture added an unsupported quantity_entries object");
    std::set<std::string, std::less<>> pointers;
    for (const auto* collection : {before, after, replayed})
        if (collection)
            for (const auto& [pointer, value] : collection->items()) {
                (void)value;
                pointers.insert(pointer);
            }
    for (const auto& pointer : pointers) {
        const auto old_value = receipt(before, pointer), raw_value = receipt(after, pointer);
        if (exact(raw_value, receipt(replayed, pointer))) continue;
        if (!changed_pointers.contains(pointer))
            invalid("Wall capture changed an unknown or unchanged quantity receipt");
        if (old_value && exact(raw_value, old_value)) continue;
        if (!raw_value && (!old_value || known_receipt_only(*old_value))) continue;
        invalid("Wall capture changed opaque quantity receipt fields or unsupported authority");
    }
}

void normalize_scalar(Entity& candidate, const Entity& original, const Entity& expected,
    const char* canonical, const char* alias) {
    for (const auto* name : {canonical, alias}) {
        const auto source = field(original.properties, name), raw = field(candidate.properties, name);
        if (source && raw && exact(source, field(expected.properties, name)) &&
            source->is_number() && raw->is_number() && source->get<double>() == raw->get<double>())
            candidate.properties[name] = *source;
    }
    const auto raw = field(candidate.properties, canonical), source_alias = field(original.properties, alias);
    if (!original.properties.contains(canonical) && !expected.properties.contains(canonical) &&
        raw && source_alias && raw->is_number() && source_alias->is_number() &&
        raw->get<double>() == source_alias->get<double>())
        candidate.properties.erase(canonical);
}

Entity normalize_equivalent_source(const Entity& original, const Entity& candidate,
    const Entity& expected, const Wall& before, const Wall& after) {
    auto result = candidate;
    for (const auto& dimension : dimensions)
        normalize_scalar(result, original, expected, dimension.canonical, dimension.alias);
    for (std::size_t i = 0; i < before.layers.size(); ++i) {
        const auto& old_value = original.properties.at("layers").at(i).at("thickness_m");
        auto& raw_value = result.properties.at("layers").at(i).at("thickness_m");
        const auto& replayed = expected.properties.at("layers").at(i).at("thickness_m");
        if (exact(old_value, replayed) && old_value.get<double>() == raw_value.get<double>()) raw_value = old_value;
    }
    const auto old_gradient = wall_top_gradient(before), new_gradient = wall_top_gradient(after);
    if (old_gradient.x == new_gradient.x && old_gradient.y == new_gradient.y) {
        // Only independently admitted, equal full gradients may normalize a
        // supported top encoding. Equal projected rise alone is insufficient.
        for (const auto* name : {"top_plane", "slope_rise_m", "slope_rise"}) {
            const auto source = field(original.properties, name);
            if (!exact(source, field(expected.properties, name))) continue;
            if (source) result.properties[name] = *source;
            else result.properties.erase(name);
        }
    }
    return result;
}
} // namespace

void validate_wall_profile_source_entity(const Entity& source) {
    if (source.type != "wall" || !source.properties.is_object())
        invalid("Wall profile source admission requires an existing wall");
    const auto admitted = wall(source);
    for (const auto& dimension : dimensions) (void)scalar(source, dimension);
    const auto values = entries(source);
    if (!values) return;
    const auto bound_receipt = [&](const std::string& pointer, const Json& value) {
        if (const auto entry = receipt(values, pointer)) {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                invalid("Wall profile source receipt requires its actual finite scalar");
            (void)admitted_quantity(*entry, value.get<double>());
        }
    };
    for (const auto* name : {"height_m", "height", "thickness_m", "thickness",
                            "slope_rise_m", "slope_rise"}) {
        const auto pointer = "/" + std::string(name);
        if (!receipt(values, pointer)) continue;
        const auto value = field(source.properties, name);
        if (!value) invalid("Wall profile source quantity receipt is dangling");
        bound_receipt(pointer, *value);
    }
    constexpr std::string_view prefix = "/layers/", suffix = "/thickness_m";
    for (const auto& [pointer, value] : values->items()) {
        (void)value;
        const std::string_view path(pointer);
        if (!path.starts_with(prefix) || !path.ends_with(suffix) ||
            path.size() < prefix.size() + suffix.size()) continue;
        const auto index = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        // Only the direct layer-thickness path is known. Opaque descendants
        // and other fields retain their own authority without interpretation.
        if (index.find('/') != std::string_view::npos) continue;
        std::size_t position = 0;
        const auto parsed = std::from_chars(index.data(), index.data() + index.size(), position);
        if (index.empty() || (index.size() > 1 && index.front() == '0') ||
            parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size() ||
            position >= admitted.layers.size())
            invalid("Wall profile source layer quantity receipt has no existing layer index");
        bound_receipt(pointer, source.properties.at("layers").at(position).at("thickness_m"));
    }
}

std::optional<WallProfileEditIntent> capture_wall_profile_edit(
    const Entity& original, const Entity& candidate,
    const std::optional<WallProfileEditIntent>& authored) {
    validate_wall_profile_source_entity(original);
    if (exact(original, candidate)) return std::nullopt;
    if (original.type != "wall" || candidate.type != "wall" ||
        !original.properties.is_object() || !candidate.properties.is_object())
        invalid("Wall capture requires an existing wall profile");
    const auto before = wall(original), after = wall(candidate);
    if (before.layers.size() != after.layers.size()) invalid("Wall capture cannot change layer inventory");
    for (std::size_t i = 0; i < before.layers.size(); ++i)
        if (before.layers[i].id != after.layers[i].id)
            invalid("Wall capture must retain layer identities and order");
    std::set<std::string, std::less<>> pointers;
    WallProfileEditIntent captured;
    captured.wall_id = original.id;
    for (const auto& dimension : dimensions) {
        const auto old_value = scalar(original, dimension), value = scalar(candidate, dimension);
        if (old_value == value) continue;
        const std::vector<std::string> names{"/" + std::string(dimension.canonical), "/" + std::string(dimension.alias)};
        pointers.insert(names.front());
        if (original.properties.contains(dimension.alias)) pointers.insert(names.back());
        if (!authored) captured.*(dimension.intent) = captured_quantity(original, candidate, names, old_value, value);
    }
    bool layers_changed = false;
    for (std::size_t i = 0; i < before.layers.size(); ++i) {
        if (before.layers[i].thickness == after.layers[i].thickness) continue;
        layers_changed = true;
        pointers.insert("/layers/" + std::to_string(i) + "/thickness_m");
    }
    if (layers_changed && !authored) {
        captured.layer_thicknesses.emplace();
        for (std::size_t i = 0; i < before.layers.size(); ++i) {
            const auto pointer = "/layers/" + std::to_string(i) + "/thickness_m";
            captured.layer_thicknesses->push_back({before.layers[i].id,
                captured_quantity(original, candidate, {pointer}, before.layers[i].thickness, after.layers[i].thickness)});
        }
    }
    const auto old_gradient = wall_top_gradient(before), new_gradient = wall_top_gradient(after);
    if (old_gradient.x != new_gradient.x || old_gradient.y != new_gradient.y ||
        before.slope_rise.value_or(0.0) != after.slope_rise.value_or(0.0)) {
        pointers.insert("/slope_rise_m");
        if (original.properties.contains("slope_rise")) pointers.insert("/slope_rise");
        if (!authored) captured.top_rise = captured_quantity(original, candidate,
            {"/slope_rise_m", "/slope_rise"}, before.slope_rise.value_or(0.0), after.slope_rise.value_or(0.0));
    }
    if (authored && authored->wall_id != original.id) invalid("Wall capture authored hint targets another wall");
    const bool captured_edit = captured.thickness || captured.height || captured.layer_thicknesses || captured.top_rise;
    const auto& intent = authored ? *authored : captured;
    // Pure admission already establishes an unchanged source's semantics and
    // receipt bindings. A representation-only candidate needs no invented
    // quantity for an arbitrary, valid unreceipted double.
    const auto expected = authored || captured_edit ? replay_wall_profile_entity(original, intent) : original;
    const auto old_receipts = entries(original), expected_receipts = entries(expected);
    for (auto pointer = pointers.begin(); pointer != pointers.end();) {
        // A supplied flat/equal top rise can replay as an exact no-op. It must
        // not authorize receipt removal while another dimension is changing.
        if (exact(receipt(old_receipts, *pointer), receipt(expected_receipts, *pointer)))
            pointer = pointers.erase(pointer);
        else ++pointer;
    }
    const auto normalized = normalize_equivalent_source(original, candidate, expected, before, after);
    compare_candidate(original, normalized, expected, pointers);
    if (exact(expected, original)) return std::nullopt;
    return intent;
}

} // namespace sketch
