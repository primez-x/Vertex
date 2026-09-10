#include "sketch/constraint_entity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sketch {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumIdentifierBytes = 128;
constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::size_t kMaximumJsonValues = 100'000;

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

bool valid_identifier(std::string_view value) {
    if (value.empty() || value.size() > kMaximumIdentifierBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.' || character == ':';
    });
}

void validate_json_tree(const json& value, std::size_t depth, std::size_t& count) {
    if (++count > kMaximumJsonValues || depth > kMaximumJsonDepth) {
        invalid("constraint JSON exceeds complexity limits");
    }
    if (value.is_discarded() || value.is_binary()) {
        invalid("constraint JSON contains a non-portable value");
    }
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        invalid("constraint JSON contains a non-finite number");
    }
    if (value.is_array()) {
        for (const auto& child : value) {
            validate_json_tree(child, depth + 1, count);
        }
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key.size() > kMaximumIdentifierBytes) {
                invalid("constraint JSON contains an oversized key");
            }
            validate_json_tree(child, depth + 1, count);
        }
    }
}

void validate_entity_container(const Entity& entity) {
    if (entity.type != "constraint") {
        invalid("constraint entity must have type constraint");
    }
    if (!valid_identifier(entity.id)) {
        invalid("constraint entity id is empty or invalid");
    }
    if (!entity.properties.is_object()) {
        invalid("constraint entity properties must be a JSON object");
    }
    if (!entity.extensions.is_object()) {
        invalid("constraint entity extensions must be a JSON object");
    }
    std::size_t count = 0;
    validate_json_tree(entity.properties, 0, count);
    validate_json_tree(entity.extensions, 0, count);
}

std::uint64_t json_uint64(const json& value, std::string_view context) {
    if (!value.is_number_integer()) {
        invalid(context);
    }
    try {
        if (value.is_number_unsigned()) {
            return value.get<std::uint64_t>();
        }
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            invalid(context);
        }
        return static_cast<std::uint64_t>(signed_value);
    } catch (const std::exception&) {
        invalid(context);
    }
}

std::int64_t json_int64(const json& value, std::string_view context) {
    if (!value.is_number_integer()) {
        invalid(context);
    }
    try {
        if (value.is_number_unsigned()) {
            const auto unsigned_value = value.get<std::uint64_t>();
            if (unsigned_value >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                invalid(context);
            }
            return static_cast<std::int64_t>(unsigned_value);
        }
        return value.get<std::int64_t>();
    } catch (const std::exception&) {
        invalid(context);
    }
}

double json_finite_double(const json& value, std::string_view context) {
    if (!value.is_number()) {
        invalid(context);
    }
    try {
        const auto result = value.get<double>();
        if (!std::isfinite(result)) {
            invalid(context);
        }
        return result;
    } catch (const std::exception&) {
        invalid(context);
    }
}

std::string json_string(const json& value, std::string_view context) {
    if (!value.is_string()) {
        invalid(context);
    }
    try {
        return value.get<std::string>();
    } catch (const std::exception&) {
        invalid(context);
    }
}

std::optional<ConstraintRelationKind> relation_from_name(std::string_view name) {
    if (name == "horizontal") {
        return ConstraintRelationKind::horizontal;
    }
    if (name == "vertical") {
        return ConstraintRelationKind::vertical;
    }
    if (name == "coincident") {
        return ConstraintRelationKind::coincident;
    }
    if (name == "fixed_length") {
        return ConstraintRelationKind::fixed_length;
    }
    if (name == "parallel") {
        return ConstraintRelationKind::parallel;
    }
    if (name == "perpendicular") {
        return ConstraintRelationKind::perpendicular;
    }
    if (name == "fixed_anchor") {
        return ConstraintRelationKind::fixed_anchor;
    }
    return std::nullopt;
}

std::optional<WallEndpointRole> role_from_name(std::string_view name) {
    if (name == "start") {
        return WallEndpointRole::start;
    }
    if (name == "end") {
        return WallEndpointRole::end;
    }
    return std::nullopt;
}

std::size_t expected_binding_count(ConstraintRelationKind relation) {
    switch (relation) {
        case ConstraintRelationKind::horizontal:
        case ConstraintRelationKind::vertical:
        case ConstraintRelationKind::coincident:
        case ConstraintRelationKind::fixed_length:
            return 2;
        case ConstraintRelationKind::parallel:
        case ConstraintRelationKind::perpendicular:
            return 4;
        case ConstraintRelationKind::fixed_anchor:
            return 1;
    }
    invalid("unknown constraint relation kind");
}

std::string role_name(WallEndpointRole role) {
    switch (role) {
        case WallEndpointRole::start:
            return "start";
        case WallEndpointRole::end:
            return "end";
    }
    invalid("unknown wall endpoint role");
}

const json& required_property(const json& properties, std::string_view key) {
    const auto found = properties.find(key);
    if (found == properties.end()) {
        invalid(std::string("constraint is missing ") + std::string(key));
    }
    return *found;
}

struct RawBinding {
    std::string owner_id;
    std::string feature;
    std::string role;
};

std::vector<RawBinding> decode_binding_envelope(const json& properties) {
    const auto& value = required_property(properties, "bindings");
    if (!value.is_array() || value.empty()) {
        invalid("constraint bindings must be a non-empty JSON array");
    }

    std::vector<RawBinding> bindings;
    bindings.reserve(value.size());
    std::set<std::tuple<std::string, std::string, std::string>> seen;
    for (const auto& item : value) {
        if (!item.is_object()) {
            invalid("constraint binding must be a JSON object");
        }
        const auto owner = item.find("owner_id");
        const auto feature = item.find("feature");
        const auto role = item.find("role");
        if (owner == item.end() || feature == item.end() || role == item.end()) {
            invalid("constraint binding is missing owner_id, feature, or role");
        }
        const auto owner_id = json_string(*owner, "constraint binding owner_id must be a string");
        if (!valid_identifier(owner_id)) {
            invalid("constraint binding owner_id is empty or invalid");
        }
        const auto feature_name = json_string(*feature, "constraint binding feature must be a string");
        const auto role_name_text = json_string(*role, "constraint binding role must be a string");
        if (feature_name.empty() || role_name_text.empty()) {
            invalid("constraint binding feature and role must be non-empty");
        }
        if (!seen.emplace(owner_id, feature_name, role_name_text).second) {
            invalid("constraint bindings contain a duplicate endpoint");
        }
        bindings.push_back(RawBinding{owner_id, feature_name, role_name_text});
    }
    return bindings;
}

std::vector<WallEndpointBinding> decode_v1_bindings(const std::vector<RawBinding>& raw_bindings) {
    std::vector<WallEndpointBinding> bindings;
    bindings.reserve(raw_bindings.size());
    for (const auto& raw : raw_bindings) {
        if (raw.feature != "baseline") {
            invalid("constraint binding feature must be baseline");
        }
        const auto endpoint_role = role_from_name(raw.role);
        if (!endpoint_role.has_value()) {
            invalid("constraint binding role must be start or end");
        }
        // Raw envelope validation already checked unique owner/feature/role;
        // the known v1 role mapping is one-to-one.
        bindings.push_back({raw.owner_id, *endpoint_role});
    }
    return bindings;
}

std::vector<std::string> decode_wall_ids(const json& properties,
                                         const std::vector<std::string>& binding_owners) {
    const auto& value = required_property(properties, "wall_ids");
    if (!value.is_array() || value.empty()) {
        invalid("constraint wall_ids must be a non-empty JSON array");
    }

    std::vector<std::string> wall_ids;
    wall_ids.reserve(value.size());
    for (const auto& item : value) {
        const auto id = json_string(item, "constraint wall_ids must contain strings");
        if (!valid_identifier(id)) {
            invalid("constraint wall_ids contains an empty or invalid id");
        }
        wall_ids.push_back(id);
    }
    if (!std::is_sorted(wall_ids.begin(), wall_ids.end()) ||
        std::adjacent_find(wall_ids.begin(), wall_ids.end()) != wall_ids.end()) {
        invalid("constraint wall_ids must be sorted and unique");
    }

    std::vector<std::string> expected;
    expected = binding_owners;
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    if (wall_ids != expected) {
        invalid("constraint wall_ids must exactly match binding owners");
    }
    return wall_ids;
}

void validate_binding_count(const std::vector<WallEndpointBinding>& bindings,
                            ConstraintRelationKind relation) {
    if (bindings.size() != expected_binding_count(relation)) {
        invalid("constraint binding count does not match its relation");
    }
}

std::optional<Unit> unit_from_name(std::string_view name) {
    if (name == "m") {
        return Unit::metre;
    }
    if (name == "mm") {
        return Unit::millimetre;
    }
    if (name == "cm") {
        return Unit::centimetre;
    }
    if (name == "ft") {
        return Unit::foot;
    }
    if (name == "in") {
        return Unit::inch;
    }
    return std::nullopt;
}

std::string unit_name(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "m";
        case Unit::millimetre:
            return "mm";
        case Unit::centimetre:
            return "cm";
        case Unit::foot:
            return "ft";
        case Unit::inch:
            return "in";
    }
    invalid("unknown quantity unit");
}

Quantity decode_quantity_receipt(const json& value) {
    if (!value.is_object()) {
        invalid("constraint quantity receipt must be a JSON object");
    }
    const auto& version = required_property(value, "version");
    if (json_uint64(version, "constraint quantity receipt version must be a non-negative integer") !=
        1) {
        invalid("constraint quantity receipt version is unsupported");
    }
    const auto expression = json_string(required_property(value, "original_expression"),
                                        "constraint quantity receipt expression must be a string");
    const auto entered_unit_name =
        json_string(required_property(value, "entered_unit"),
                    "constraint quantity receipt unit must be a string");
    const auto entered_unit = unit_from_name(entered_unit_name);
    if (!entered_unit.has_value()) {
        invalid("constraint quantity receipt unit is unsupported");
    }
    const auto& exact = required_property(value, "exact_metres");
    if (!exact.is_object()) {
        invalid("constraint quantity receipt exact_metres must be a JSON object");
    }
    const auto numerator = json_int64(required_property(exact, "numerator"),
                                      "constraint quantity numerator must be an int64");
    const auto denominator = json_int64(required_property(exact, "denominator"),
                                        "constraint quantity denominator must be an int64");
    if (denominator <= 0) {
        invalid("constraint quantity denominator must be positive");
    }

    Quantity parsed;
    try {
        parsed = parse_quantity(expression, *entered_unit);
    } catch (const std::exception&) {
        invalid("constraint quantity receipt expression is not exactly parseable");
    }
    if (parsed.entered_unit != *entered_unit ||
        parsed.exact_metres != ExactRational{numerator, denominator}) {
        invalid("constraint quantity receipt does not match its exact expression");
    }
    if (!std::isfinite(parsed.metres)) {
        invalid("constraint quantity receipt is not finite");
    }
    return parsed;
}

json encode_quantity_receipt(const Quantity& quantity, const json* original = nullptr) {
    // Reparse the source expression so a caller cannot introduce a double-only
    // lock or an inconsistent rational pair through the public struct.
    const auto receipt = json{{"version", 1},
                              {"original_expression", quantity.original_expression},
                              {"entered_unit", unit_name(quantity.entered_unit)},
                              {"exact_metres",
                               {{"numerator", quantity.exact_metres.numerator},
                                {"denominator", quantity.exact_metres.denominator}}}};
    const auto parsed = decode_quantity_receipt(receipt);
    if (parsed.metres != quantity.metres || parsed.exact_metres != quantity.exact_metres ||
        parsed.entered_unit != quantity.entered_unit ||
        parsed.original_expression != quantity.original_expression) {
        invalid("constraint quantity is internally inconsistent");
    }
    if (!(quantity.metres > 0.0) || !std::isfinite(quantity.metres)) {
        invalid("fixed length quantity must be finite and positive");
    }

    if (original != nullptr) {
        try {
            const auto previous = decode_quantity_receipt(*original);
            if (previous.exact_metres == quantity.exact_metres &&
                previous.metres == quantity.metres) {
                auto merged = *original;
                merged["version"] = receipt.at("version");
                merged["original_expression"] = receipt.at("original_expression");
                merged["entered_unit"] = receipt.at("entered_unit");
                auto exact = merged.find("exact_metres");
                if (exact == merged.end() || !exact->is_object()) {
                    merged["exact_metres"] = receipt.at("exact_metres");
                } else {
                    (*exact)["numerator"] = quantity.exact_metres.numerator;
                    (*exact)["denominator"] = quantity.exact_metres.denominator;
                }
                return merged;
            }
        } catch (const std::exception&) {
            // An invalid or changed old receipt cannot supply opaque metadata
            // for a new lock. The canonical receipt below remains authoritative.
        }
    }
    return receipt;
}

void validate_semantics(const json& properties, ConstraintRelationKind relation,
                        PersistentConstraint& result) {
    const bool has_length = properties.contains("length_m");
    const bool has_quantity_entries = properties.contains("quantity_entries");
    const bool has_anchor = properties.contains("anchor_m");

    if (relation == ConstraintRelationKind::fixed_length) {
        if (!has_length || !has_quantity_entries) {
            invalid("fixed length constraint requires length_m and quantity_entries");
        }
        const auto length = json_finite_double(properties.at("length_m"),
                                               "fixed length length_m must be finite");
        if (!(length > 0.0)) {
            invalid("fixed length length_m must be positive");
        }
        const auto& entries = properties.at("quantity_entries");
        if (!entries.is_object()) {
            invalid("fixed length quantity_entries must be a JSON object");
        }
        const auto receipt = entries.find("/length_m");
        if (receipt == entries.end()) {
            invalid("fixed length quantity_entries must contain /length_m");
        }
        auto quantity = decode_quantity_receipt(*receipt);
        if (quantity.metres != length) {
            invalid("fixed length metre value does not match its exact receipt");
        }
        result.length = std::move(quantity);
        if (has_anchor) {
            invalid("fixed length constraint cannot contain anchor_m");
        }
        return;
    }

    if (relation == ConstraintRelationKind::fixed_anchor) {
        if (!has_anchor) {
            invalid("fixed anchor constraint requires anchor_m");
        }
        const auto& value = properties.at("anchor_m");
        if (!value.is_array() || value.size() != 2) {
            invalid("fixed anchor anchor_m must contain exactly two coordinates");
        }
        result.anchor = Vec2{json_finite_double(value.at(0), "fixed anchor x must be finite"),
                             json_finite_double(value.at(1), "fixed anchor y must be finite")};
        if (has_length) {
            invalid("fixed anchor constraint cannot contain length_m");
        }
        if (has_quantity_entries) {
            const auto& entries = properties.at("quantity_entries");
            if (!entries.is_object()) {
                invalid("constraint quantity_entries must be a JSON object");
            }
            if (entries.contains("/length_m")) {
                invalid("fixed anchor constraint cannot contain /length_m receipt");
            }
        }
        return;
    }

    if (has_length || has_anchor) {
        invalid("constraint contains semantic fields for the wrong relation");
    }
    if (has_quantity_entries) {
        const auto& entries = properties.at("quantity_entries");
        if (!entries.is_object()) {
            invalid("constraint quantity_entries must be a JSON object");
        }
        if (entries.contains("/length_m")) {
            invalid("constraint contains a /length_m receipt for the wrong relation");
        }
    }
}

void validate_model(const PersistentConstraint& constraint) {
    if (!valid_identifier(constraint.id)) {
        invalid("constraint id is empty or invalid");
    }
    if (constraint.bindings.size() != expected_binding_count(constraint.relation)) {
        invalid("constraint binding count does not match its relation");
    }
    std::vector<WallEndpointBinding> seen;
    seen.reserve(constraint.bindings.size());
    for (const auto& binding : constraint.bindings) {
        if (!valid_identifier(binding.owner_id)) {
            invalid("constraint binding owner_id is empty or invalid");
        }
        if (binding.role != WallEndpointRole::start && binding.role != WallEndpointRole::end) {
            invalid("constraint binding role is invalid");
        }
        if (std::find(seen.begin(), seen.end(), binding) != seen.end()) {
            invalid("constraint bindings contain a duplicate endpoint");
        }
        seen.push_back(binding);
    }

    switch (constraint.relation) {
        case ConstraintRelationKind::fixed_length:
            if (!constraint.length.has_value()) {
                invalid("fixed length constraint requires an exact quantity");
            }
            (void)encode_quantity_receipt(*constraint.length);
            if (constraint.anchor.has_value()) {
                invalid("fixed length constraint cannot contain an anchor");
            }
            break;
        case ConstraintRelationKind::fixed_anchor:
            if (!constraint.anchor.has_value()) {
                invalid("fixed anchor constraint requires an anchor");
            }
            if (!std::isfinite(constraint.anchor->x) || !std::isfinite(constraint.anchor->y)) {
                invalid("fixed anchor coordinates must be finite");
            }
            if (constraint.length.has_value()) {
                invalid("fixed anchor constraint cannot contain a length");
            }
            break;
        case ConstraintRelationKind::horizontal:
        case ConstraintRelationKind::vertical:
        case ConstraintRelationKind::coincident:
        case ConstraintRelationKind::parallel:
        case ConstraintRelationKind::perpendicular:
            if (constraint.length.has_value() || constraint.anchor.has_value()) {
                invalid("constraint contains semantic fields for the wrong relation");
            }
            break;
    }
}

json encode_bindings(const std::vector<WallEndpointBinding>& bindings,
                     const json* original = nullptr) {
    auto result = json::array();
    for (const auto& binding : bindings) {
        auto item = json::object();
        if (original != nullptr && original->is_array()) {
            for (const auto& candidate : *original) {
                if (!candidate.is_object()) {
                    continue;
                }
                const auto owner = candidate.find("owner_id");
                const auto role = candidate.find("role");
                if (owner != candidate.end() && role != candidate.end() && owner->is_string() &&
                    role->is_string() && owner->get<std::string>() == binding.owner_id &&
                    role->get<std::string>() == role_name(binding.role)) {
                    item = candidate;
                    break;
                }
            }
        }
        item["owner_id"] = binding.owner_id;
        item["feature"] = "baseline";
        item["role"] = role_name(binding.role);
        result.push_back(std::move(item));
    }
    return result;
}

json encode_wall_ids(const std::vector<WallEndpointBinding>& bindings) {
    std::vector<std::string> ids;
    ids.reserve(bindings.size());
    for (const auto& binding : bindings) {
        ids.push_back(binding.owner_id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void remove_length_semantics(json& properties) {
    properties.erase("length_m");
    const auto found = properties.find("quantity_entries");
    if (found == properties.end()) {
        return;
    }
    if (!found->is_object()) {
        invalid("constraint quantity_entries must be a JSON object");
    }
    found->erase("/length_m");
    if (found->empty()) {
        properties.erase(found);
    }
}

}  // namespace

std::string_view constraint_relation_name(ConstraintRelationKind relation) {
    switch (relation) {
        case ConstraintRelationKind::horizontal:
            return "horizontal";
        case ConstraintRelationKind::vertical:
            return "vertical";
        case ConstraintRelationKind::coincident:
            return "coincident";
        case ConstraintRelationKind::fixed_length:
            return "fixed_length";
        case ConstraintRelationKind::parallel:
            return "parallel";
        case ConstraintRelationKind::perpendicular:
            return "perpendicular";
        case ConstraintRelationKind::fixed_anchor:
            return "fixed_anchor";
    }
    invalid("unknown constraint relation kind");
}

std::string_view wall_endpoint_role_name(WallEndpointRole role) {
    switch (role) {
        case WallEndpointRole::start:
            return "start";
        case WallEndpointRole::end:
            return "end";
    }
    invalid("unknown wall endpoint role");
}

ConstraintEntityDecodeResult decode_constraint_entity(const Entity& entity) {
    validate_entity_container(entity);
    const auto& properties = entity.properties;
    const auto version = json_uint64(required_property(properties, "version"),
                                     "constraint version must be a non-negative integer");
    const auto relation_text =
        json_string(required_property(properties, "relation"),
                    "constraint relation must be a non-empty string");
    if (relation_text.empty()) {
        invalid("constraint relation must be a non-empty string");
    }

    const auto relation = relation_from_name(relation_text);
    if (version == 1 && relation) {
        const auto& bindings = required_property(properties, "bindings");
        if (!bindings.is_array() || bindings.size() != expected_binding_count(*relation))
            invalid("constraint has the wrong number of endpoint bindings");
    }
    const auto raw_bindings = decode_binding_envelope(properties);
    std::vector<std::string> binding_owners;
    binding_owners.reserve(raw_bindings.size());
    for (const auto& binding : raw_bindings) {
        binding_owners.push_back(binding.owner_id);
    }
    (void)decode_wall_ids(properties, binding_owners);

    if (version != 1 || !relation.has_value()) {
        std::string reason;
        if (version != 1) {
            reason = "unsupported constraint entity version";
        }
        if (!relation.has_value()) {
            if (!reason.empty()) {
                reason += "; ";
            }
            reason += "unsupported constraint relation";
        }
        return ConstraintEntityDecodeResult{
            .constraint = std::nullopt,
            .unsupported_reason = std::move(reason),
            .original_entity = entity,
            .version = version,
            .relation = relation_text,
        };
    }

    const auto bindings = decode_v1_bindings(raw_bindings);
    validate_binding_count(bindings, *relation);
    PersistentConstraint result;
    result.id = entity.id;
    result.relation = *relation;
    result.bindings = bindings;
    validate_semantics(properties, *relation, result);
    return ConstraintEntityDecodeResult{
        .constraint = std::move(result),
        .unsupported_reason = {},
        .original_entity = std::nullopt,
        .version = version,
        .relation = relation_text,
    };
}

Entity encode_constraint_entity(const PersistentConstraint& constraint, const Entity* original) {
    validate_model(constraint);
    Entity result{constraint.id, "constraint", json::object(), false, json::object()};
    const json* original_bindings = nullptr;
    const json* original_length_receipt = nullptr;
    if (original != nullptr) {
        validate_entity_container(*original);
        if (original->id != constraint.id) {
            invalid("original constraint entity id does not match the model");
        }
        const auto original_decoded = decode_constraint_entity(*original);
        if (!original_decoded.constraint.has_value()) {
            invalid("cannot encode over unsupported constraint semantics");
        }
        result = *original;
        const auto bindings = result.properties.find("bindings");
        if (bindings != result.properties.end() && bindings->is_array()) {
            original_bindings = &*bindings;
        }
        const auto entries = result.properties.find("quantity_entries");
        if (entries != result.properties.end() && entries->is_object()) {
            const auto receipt = entries->find("/length_m");
            if (receipt != entries->end() && receipt->is_object()) {
                original_length_receipt = &*receipt;
            }
        }
    }

    auto& properties = result.properties;
    properties["version"] = 1;
    properties["relation"] = std::string(constraint_relation_name(constraint.relation));
    properties["bindings"] = encode_bindings(constraint.bindings, original_bindings);
    properties["wall_ids"] = encode_wall_ids(constraint.bindings);

    if (constraint.relation == ConstraintRelationKind::fixed_length) {
        properties["length_m"] = constraint.length->metres;
        auto entries = json::object();
        const auto found = properties.find("quantity_entries");
        if (found != properties.end()) {
            if (!found->is_object()) {
                invalid("constraint quantity_entries must be a JSON object");
            }
            entries = *found;
        }
        entries["/length_m"] = encode_quantity_receipt(*constraint.length, original_length_receipt);
        properties["quantity_entries"] = std::move(entries);
        properties.erase("anchor_m");
    } else if (constraint.relation == ConstraintRelationKind::fixed_anchor) {
        properties["anchor_m"] = json::array({constraint.anchor->x, constraint.anchor->y});
        remove_length_semantics(properties);
    } else {
        remove_length_semantics(properties);
        properties.erase("anchor_m");
    }
    return result;
}

}  // namespace sketch
