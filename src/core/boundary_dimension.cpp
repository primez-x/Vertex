#include "sketch/boundary_dimension.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumIdentifierBytes = 128;
constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::size_t kMaximumJsonValues = 100'000;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

bool valid_identifier(std::string_view value) noexcept {
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

void validate_json_tree(const Json& value, std::size_t depth, std::size_t& count) {
    if (++count > kMaximumJsonValues || depth > kMaximumJsonDepth) {
        invalid("dimension JSON exceeds complexity limits");
    }
    if (value.is_discarded() || value.is_binary()) {
        invalid("dimension JSON contains a non-portable value");
    }
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        invalid("dimension JSON contains a non-finite number");
    }
    if (value.is_array()) {
        for (const auto& child : value) {
            validate_json_tree(child, depth + 1, count);
        }
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key.size() > kMaximumIdentifierBytes) {
                invalid("dimension JSON contains an oversized key");
            }
            validate_json_tree(child, depth + 1, count);
        }
    }
}

void validate_entity_container(const Entity& entity) {
    if (!can_recognize_boundary_dimension_entity_type(entity.type)) {
        invalid("dimension entity must have type dimension");
    }
    if (!valid_identifier(entity.id)) {
        invalid("dimension entity id is empty or invalid");
    }
    if (!entity.properties.is_object()) {
        invalid("dimension entity properties must be a JSON object");
    }
    if (!entity.extensions.is_object()) {
        invalid("dimension entity extensions must be a JSON object");
    }
    std::size_t count = 0;
    validate_json_tree(entity.properties, 0, count);
    validate_json_tree(entity.extensions, 0, count);
}

const Json& required_property(const Json& properties, std::string_view key) {
    const auto found = properties.find(std::string(key));
    if (found == properties.end()) {
        invalid("dimension is missing " + std::string(key));
    }
    return *found;
}

std::uint64_t json_uint64(const Json& value, std::string_view context) {
    if (!value.is_number_integer()) {
        invalid(std::string(context));
    }
    try {
        if (value.is_number_unsigned()) {
            return value.get<std::uint64_t>();
        }
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            invalid(std::string(context));
        }
        return static_cast<std::uint64_t>(signed_value);
    } catch (const std::exception&) {
        invalid(std::string(context));
    }
}

std::uint32_t json_uint32(const Json& value, std::string_view context) {
    const auto number = json_uint64(value, context);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        invalid(std::string(context));
    }
    return static_cast<std::uint32_t>(number);
}

double json_finite_double(const Json& value, std::string_view context) {
    if (!value.is_number()) {
        invalid(std::string(context));
    }
    try {
        const auto result = value.get<double>();
        if (!std::isfinite(result)) {
            invalid(std::string(context));
        }
        return result;
    } catch (const std::exception&) {
        invalid(std::string(context));
    }
}

std::string json_string(const Json& value, std::string_view context) {
    if (!value.is_string()) {
        invalid(std::string(context));
    }
    try {
        return value.get<std::string>();
    } catch (const std::exception&) {
        invalid(std::string(context));
    }
}

std::string json_identifier(const Json& value, std::string_view context) {
    auto result = json_string(value, context);
    if (!valid_identifier(result)) {
        invalid(std::string(context));
    }
    return result;
}

Vec2 json_point(const Json& value, std::string_view context) {
    if (!value.is_array() || value.size() != 2) {
        invalid(std::string(context) + " must contain exactly two coordinates");
    }
    return {json_finite_double(value[0], std::string(context) + " x coordinate"),
            json_finite_double(value[1], std::string(context) + " y coordinate")};
}

std::optional<BoundaryDimensionPlacement> placement_from_name(std::string_view name) {
    if (name == "manual") {
        return BoundaryDimensionPlacement::manual;
    }
    if (name == "automatic") {
        return BoundaryDimensionPlacement::automatic;
    }
    return std::nullopt;
}

void validate_model(const BoundaryDimension& dimension) {
    if (!valid_identifier(dimension.id)) {
        invalid("dimension id is empty or invalid");
    }
    if (!valid_identifier(dimension.boundary_id)) {
        invalid("dimension target entity id is empty or invalid");
    }
    if (!valid_identifier(dimension.segment_id)) {
        invalid("dimension target segment id is empty or invalid");
    }
    if (!std::isfinite(dimension.text_position.x) ||
        !std::isfinite(dimension.text_position.y)) {
        invalid("dimension text position must be finite");
    }
    switch (dimension.placement) {
        case BoundaryDimensionPlacement::manual:
            if (dimension.automatic_placement_version.has_value()) {
                invalid("manual dimension cannot contain automatic placement version");
            }
            return;
        case BoundaryDimensionPlacement::automatic:
            if (!dimension.automatic_placement_version.has_value() ||
                *dimension.automatic_placement_version != 1) {
                invalid("automatic dimension requires placement version one");
            }
            return;
    }
    invalid("dimension placement origin is unsupported");
}

BoundaryDimensionDecodeResult unsupported_result(const Entity& entity, std::uint64_t version,
                                                  std::string kind, std::string reason) {
    return BoundaryDimensionDecodeResult{
        .dimension = std::nullopt,
        .unsupported_reason = std::move(reason),
        .original_entity = entity,
        .version = version,
        .kind = std::move(kind),
    };
}

}  // namespace

bool can_recognize_boundary_dimension_entity_type(std::string_view type) noexcept {
    return type == "dimension";
}

std::string_view boundary_dimension_placement_name(BoundaryDimensionPlacement placement) {
    switch (placement) {
        case BoundaryDimensionPlacement::manual:
            return "manual";
        case BoundaryDimensionPlacement::automatic:
            return "automatic";
    }
    invalid("unknown dimension placement origin");
}

BoundaryDimensionVersion inspect_boundary_dimension_version(const Entity& entity) {
    validate_entity_container(entity);
    const auto& value = required_property(entity.properties, "dimension_version");
    const auto version = json_uint64(value, "dimension version must be a positive integer");
    if (version == 0) {
        invalid("dimension version must be a positive integer");
    }
    if (version == 1) {
        return {BoundaryDimensionFormat::supported_v1, version, {}};
    }
    return {BoundaryDimensionFormat::unsupported_version, version,
            "unsupported boundary dimension version " + std::to_string(version)};
}

BoundaryDimensionDecodeResult decode_boundary_dimension_entity(const Entity& entity) {
    const auto version_info = inspect_boundary_dimension_version(entity);
    const auto version = *version_info.version;
    if (version_info.format == BoundaryDimensionFormat::unsupported_version) {
        std::string kind;
        const auto found = entity.properties.find("dimension_kind");
        if (found != entity.properties.end() && found->is_string()) {
            kind = found->get<std::string>();
        }
        return unsupported_result(entity, version, std::move(kind), version_info.diagnostic);
    }

    const auto kind = json_string(required_property(entity.properties, "dimension_kind"),
                                  "dimension kind must be a string");
    if (kind != "segment_length") {
        return unsupported_result(entity, version, kind,
                                  "unsupported boundary dimension kind " + kind);
    }

    const auto& target = required_property(entity.properties, "target");
    if (!target.is_object()) {
        invalid("dimension target must be a JSON object");
    }
    const auto boundary_id = json_identifier(
        required_property(target, "entity_id"), "dimension target entity_id must be a valid id");
    const auto segment_id = json_identifier(
        required_property(target, "segment_id"), "dimension target segment_id must be a valid id");
    const auto text_position = json_point(
        required_property(entity.properties, "text_position"), "dimension text_position");
    const auto placement_name = json_string(
        required_property(entity.properties, "placement_origin"),
        "dimension placement_origin must be a string");
    const auto placement = placement_from_name(placement_name);
    if (!placement.has_value()) {
        invalid("dimension placement_origin is unsupported");
    }

    std::optional<std::uint32_t> automatic_version;
    const auto automatic_field = entity.properties.find("automatic_placement_version");
    if (*placement == BoundaryDimensionPlacement::manual) {
        if (automatic_field != entity.properties.end()) {
            invalid("manual dimension cannot contain automatic placement version");
        }
    } else {
        if (automatic_field == entity.properties.end()) {
            invalid("automatic dimension is missing placement version");
        }
        const auto parsed = json_uint32(
            *automatic_field, "automatic placement version must be a positive integer");
        if (parsed != 1) {
            invalid("automatic placement version is unsupported");
        }
        automatic_version = parsed;
    }

    BoundaryDimension result{
        .id = entity.id,
        .boundary_id = boundary_id,
        .segment_id = segment_id,
        .text_position = text_position,
        .placement = *placement,
        .automatic_placement_version = automatic_version,
    };
    validate_model(result);
    return BoundaryDimensionDecodeResult{
        .dimension = std::move(result),
        .unsupported_reason = {},
        .original_entity = std::nullopt,
        .version = version,
        .kind = kind,
    };
}

Entity encode_boundary_dimension_entity(const BoundaryDimension& dimension,
                                         const Entity* original) {
    validate_model(dimension);
    Entity result{dimension.id, "dimension", Json::object(), false, Json::object()};
    if (original != nullptr) {
        validate_entity_container(*original);
        if (original->id != dimension.id) {
            invalid("original dimension entity id does not match the model");
        }
        const auto previous = decode_boundary_dimension_entity(*original);
        if (!previous.supported()) {
            invalid("cannot encode over unsupported dimension semantics");
        }
        result = *original;
    }

    auto& properties = result.properties;
    properties["dimension_version"] = 1;
    properties["dimension_kind"] = "segment_length";
    Json target = Json::object();
    const auto previous_target = properties.find("target");
    if (previous_target != properties.end()) {
        if (!previous_target->is_object()) {
            invalid("dimension target must be a JSON object");
        }
        target = *previous_target;
    }
    target["entity_id"] = dimension.boundary_id;
    target["segment_id"] = dimension.segment_id;
    properties["target"] = std::move(target);
    properties["text_position"] = Json::array({dimension.text_position.x,
                                                  dimension.text_position.y});
    properties["placement_origin"] =
        std::string(boundary_dimension_placement_name(dimension.placement));
    if (dimension.placement == BoundaryDimensionPlacement::automatic) {
        properties["automatic_placement_version"] = 1;
    } else {
        properties.erase("automatic_placement_version");
    }
    return result;
}

BoundaryDimensionResolution resolve_boundary_dimension(const BoundaryDimension& dimension,
                                                        const Entity& boundary_entity) {
    validate_model(dimension);
    if (boundary_entity.id != dimension.boundary_id) {
        invalid("dimension source boundary id does not match target entity id");
    }
    if (!can_recognize_boundary_entity_type(boundary_entity.type)) {
        invalid("dimension source entity type is not a supported boundary");
    }
    const auto source_version = inspect_boundary_entity_version(boundary_entity);
    if (source_version.format != BoundaryEntityFormat::identified_v1) {
        invalid("dimension source boundary must use identified model version one");
    }
    const auto source = decode_identified_boundary_entity(boundary_entity);
    const auto found = std::find_if(
        source.segments.begin(), source.segments.end(), [&](const IdentifiedSegment& segment) {
            return segment.segment_id == dimension.segment_id;
        });
    if (found == source.segments.end()) {
        invalid("dimension source boundary is missing the target segment id");
    }
    return BoundaryDimensionResolution{found->segment, segment_length(found->segment)};
}

BoundaryDimensionResolution BoundaryDimension::resolve(const Entity& boundary_entity) const {
    return resolve_boundary_dimension(*this, boundary_entity);
}

}  // namespace sketch
