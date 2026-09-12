#include "sketch/building_entity.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t schema_version = 1;
constexpr std::size_t maximum_id_bytes = 128;
constexpr std::size_t maximum_riser_count = 10'000;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

void validate_id(std::string_view id, std::string_view context) {
    if (id.empty() || id.size() > maximum_id_bytes || id.find('\0') != std::string_view::npos ||
        !std::all_of(id.begin(), id.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(context) + " is empty or invalid");
    }
}

void require_object(const Json& value, std::string_view context) {
    if (!value.is_object()) {
        invalid(std::string(context) + " must be a JSON object");
    }
}

const Json& required_field(const Json& properties, std::string_view key) {
    const auto found = properties.find(key);
    if (found == properties.end()) {
        invalid("Building entity is missing required field: " + std::string(key));
    }
    return *found;
}

double finite_number(const Json& value, std::string_view key) {
    if (!value.is_number()) {
        invalid("Building entity field " + std::string(key) + " must be a finite number");
    }
    try {
        const double result = value.get<double>();
        if (!std::isfinite(result)) {
            invalid("Building entity field " + std::string(key) + " must be a finite number");
        }
        return result;
    } catch (const Json::exception&) {
        invalid("Building entity field " + std::string(key) + " must be a finite number");
    }
}

std::string string_field(const Json& value, std::string_view key) {
    if (!value.is_string()) {
        invalid("Building entity field " + std::string(key) + " must be a string");
    }
    return value.get<std::string>();
}

std::string form_field(const Json& properties) {
    const auto form = string_field(required_field(properties, "form"), "form");
    if (form.empty()) {
        invalid("Building entity form must not be empty");
    }
    return form;
}

Vec3 vec3_field(const Json& value, std::string_view key) {
    if (!value.is_array() || value.size() != 3) {
        invalid("Building entity field " + std::string(key) +
                " must be an array of exactly three finite numbers");
    }
    return {finite_number(value[0], key), finite_number(value[1], key),
            finite_number(value[2], key)};
}

Vec3 required_vec3(const Json& properties, std::string_view key) {
    return vec3_field(required_field(properties, key), key);
}

double required_number(const Json& properties, std::string_view key) {
    return finite_number(required_field(properties, key), key);
}

std::size_t required_riser_count(const Json& properties) {
    const auto& value = required_field(properties, "riser_count");
    std::uint64_t unsigned_value = 0;
    if (value.is_number_unsigned()) {
        unsigned_value = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            invalid("Building entity field riser_count must be a positive bounded integer");
        }
        unsigned_value = static_cast<std::uint64_t>(signed_value);
    } else {
        invalid("Building entity field riser_count must be a positive bounded integer");
    }
    if (unsigned_value == 0 || unsigned_value > maximum_riser_count) {
        invalid("Building entity field riser_count is outside the supported range");
    }
    return static_cast<std::size_t>(unsigned_value);
}

void require_schema_version(const Json& properties) {
    const auto& value = required_field(properties, "version");
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        invalid("Building entity version must be an integer");
    }
    try {
        const bool supported = value.is_number_unsigned()
            ? value.get<std::uint64_t>() == static_cast<std::uint64_t>(schema_version)
            : value.get<std::int64_t>() == schema_version;
        if (!supported) {
            invalid("Unsupported building entity schema version");
        }
    } catch (const Json::exception&) {
        invalid("Building entity version must be an integer");
    }
}

std::optional<StairLanding> landing_field(const Json& properties) {
    const auto& value = required_field(properties, "top_landing");
    if (value.is_null()) {
        return std::nullopt;
    }
    require_object(value, "Building entity top_landing");
    return StairLanding{
        .depth = required_number(value, "depth_m"),
        .thickness = required_number(value, "thickness_m"),
    };
}

Json vec3_json(const Vec3& value) {
    Json result = Json::array();
    result.push_back(value.x);
    result.push_back(value.y);
    result.push_back(value.z);
    return result;
}

Json base_properties(std::string_view form) {
    Json result = Json::object();
    result["version"] = schema_version;
    result["form"] = form;
    return result;
}

template <typename Object>
Entity create_entity(std::string type, const Object& object, Json properties,
                     const Json& metadata) {
    require_object(metadata, "Building entity metadata");
    Entity result = Entity::create(std::move(type), std::move(properties), false, metadata);
    if (!object.id.empty()) {
        validate_id(object.id, "Building entity id");
        result.id = object.id;
    }
    return result;
}

Entity encode_one(const RectangularColumn& object, const Json& metadata) {
    auto properties = base_properties("rectangular_column");
    properties["base_center_m"] = vec3_json(object.base_center);
    properties["width_m"] = object.width;
    properties["depth_m"] = object.depth;
    properties["height_m"] = object.height;
    properties["rotation_rad"] = object.rotation_radians;
    return create_entity("column", object, std::move(properties), metadata);
}

Entity encode_one(const CircularColumn& object, const Json& metadata) {
    auto properties = base_properties("circular_column");
    properties["base_center_m"] = vec3_json(object.base_center);
    properties["radius_m"] = object.radius;
    properties["height_m"] = object.height;
    return create_entity("column", object, std::move(properties), metadata);
}

Entity encode_one(const Beam& object, const Json& metadata) {
    auto properties = base_properties("straight_beam");
    properties["start_m"] = vec3_json(object.start);
    properties["end_m"] = vec3_json(object.end);
    properties["up_dir"] = vec3_json(object.up);
    properties["width_m"] = object.width;
    properties["depth_m"] = object.depth;
    return create_entity("beam", object, std::move(properties), metadata);
}

Entity encode_one(const StairFlight& object, const Json& metadata) {
    auto properties = base_properties("straight_stair_flight");
    properties["base_position_m"] = vec3_json(object.base_position);
    properties["orientation_rad"] = object.orientation_radians;
    properties["riser_count"] = object.riser_count;
    properties["total_rise_m"] = object.total_rise;
    properties["going_m"] = object.going;
    properties["width_m"] = object.width;
    if (object.top_landing.has_value()) {
        properties["top_landing"] = {
            {"depth_m", object.top_landing->depth},
            {"thickness_m", object.top_landing->thickness},
        };
    } else {
        properties["top_landing"] = nullptr;
    }
    return create_entity("stair", object, std::move(properties), metadata);
}

Entity encode_one(const SlopedRoofPanel& object, const Json& metadata) {
    auto properties = base_properties("sloped_roof_panel");
    properties["base_position_m"] = vec3_json(object.base_position);
    properties["orientation_rad"] = object.orientation_radians;
    properties["run_m"] = object.run;
    properties["span_m"] = object.span;
    properties["rise_m"] = object.rise;
    properties["pitch_rad"] = object.pitch_radians;
    properties["overhang_m"] = object.overhang;
    properties["thickness_m"] = object.thickness;
    return create_entity("roof", object, std::move(properties), metadata);
}

Entity encode_one(const GableRoof& object, const Json& metadata) {
    auto properties = base_properties("gable_roof");
    properties["base_position_m"] = vec3_json(object.base_position);
    properties["orientation_rad"] = object.orientation_radians;
    properties["length_m"] = object.length;
    properties["span_m"] = object.span;
    properties["rise_m"] = object.rise;
    properties["pitch_rad"] = object.pitch_radians;
    properties["overhang_m"] = object.overhang;
    properties["thickness_m"] = object.thickness;
    return create_entity("roof", object, std::move(properties), metadata);
}

Entity encode_one(const HipRoof& object, const Json& metadata) {
    auto properties = base_properties("hip_roof");
    properties["base_position_m"] = vec3_json(object.base_position);
    properties["orientation_rad"] = object.orientation_radians;
    properties["length_m"] = object.length;
    properties["span_m"] = object.span;
    properties["rise_m"] = object.rise;
    properties["pitch_rad"] = object.pitch_radians;
    properties["overhang_m"] = object.overhang;
    properties["thickness_m"] = object.thickness;
    return create_entity("roof", object, std::move(properties), metadata);
}

BuildingObject decode_column(const Entity& entity, const Json& properties,
                             std::string_view form) {
    if (form == "rectangular_column") {
        return RectangularColumn{
            .id = entity.id,
            .base_center = required_vec3(properties, "base_center_m"),
            .width = required_number(properties, "width_m"),
            .depth = required_number(properties, "depth_m"),
            .height = required_number(properties, "height_m"),
            .rotation_radians = required_number(properties, "rotation_rad"),
        };
    }
    if (form == "circular_column") {
        return CircularColumn{
            .id = entity.id,
            .base_center = required_vec3(properties, "base_center_m"),
            .radius = required_number(properties, "radius_m"),
            .height = required_number(properties, "height_m"),
        };
    }
    invalid("Unsupported column building form: " + std::string(form));
}

BuildingObject decode_beam(const Entity& entity, const Json& properties,
                           std::string_view form) {
    if (form != "straight_beam") {
        invalid("Unsupported beam building form: " + std::string(form));
    }
    return Beam{
        .id = entity.id,
        .start = required_vec3(properties, "start_m"),
        .end = required_vec3(properties, "end_m"),
        .up = required_vec3(properties, "up_dir"),
        .width = required_number(properties, "width_m"),
        .depth = required_number(properties, "depth_m"),
    };
}

BuildingObject decode_stair(const Entity& entity, const Json& properties,
                            std::string_view form) {
    if (form != "straight_stair_flight") {
        invalid("Unsupported stair building form: " + std::string(form));
    }
    return StairFlight{
        .id = entity.id,
        .base_position = required_vec3(properties, "base_position_m"),
        .orientation_radians = required_number(properties, "orientation_rad"),
        .riser_count = required_riser_count(properties),
        .total_rise = required_number(properties, "total_rise_m"),
        .going = required_number(properties, "going_m"),
        .width = required_number(properties, "width_m"),
        .top_landing = landing_field(properties),
    };
}

BuildingObject decode_roof(const Entity& entity, const Json& properties,
                           std::string_view form) {
    if (form == "sloped_roof_panel") {
        return SlopedRoofPanel{
            .id = entity.id,
            .base_position = required_vec3(properties, "base_position_m"),
            .orientation_radians = required_number(properties, "orientation_rad"),
            .run = required_number(properties, "run_m"),
            .span = required_number(properties, "span_m"),
            .rise = required_number(properties, "rise_m"),
            .pitch_radians = required_number(properties, "pitch_rad"),
            .overhang = required_number(properties, "overhang_m"),
            .thickness = required_number(properties, "thickness_m"),
        };
    }
    if (form == "gable_roof") {
        return GableRoof{
            .id = entity.id,
            .base_position = required_vec3(properties, "base_position_m"),
            .orientation_radians = required_number(properties, "orientation_rad"),
            .length = required_number(properties, "length_m"),
            .span = required_number(properties, "span_m"),
            .rise = required_number(properties, "rise_m"),
            .pitch_radians = required_number(properties, "pitch_rad"),
            .overhang = required_number(properties, "overhang_m"),
            .thickness = required_number(properties, "thickness_m"),
        };
    }
    if (form == "hip_roof") {
        return HipRoof{
            .id = entity.id,
            .base_position = required_vec3(properties, "base_position_m"),
            .orientation_radians = required_number(properties, "orientation_rad"),
            .length = required_number(properties, "length_m"),
            .span = required_number(properties, "span_m"),
            .rise = required_number(properties, "rise_m"),
            .pitch_radians = required_number(properties, "pitch_rad"),
            .overhang = required_number(properties, "overhang_m"),
            .thickness = required_number(properties, "thickness_m"),
        };
    }
    invalid("Unsupported roof building form: " + std::string(form));
}

void validate_geometry(const BuildingObject& object) {
    try {
        (void)make_building_shape(object);
    } catch (const std::exception& error) {
        invalid(std::string("Invalid building object geometry: ") + error.what());
    }
}

}  // namespace

bool can_recognize_building_entity_type(std::string_view type) noexcept {
    return type == "column" || type == "beam" || type == "stair" || type == "roof";
}

Entity encode_building_entity(const BuildingObject& object, Json metadata) {
    validate_geometry(object);
    return std::visit([&metadata](const auto& value) { return encode_one(value, metadata); },
                      object);
}

BuildingObject decode_building_entity(const Entity& entity) {
    validate_id(entity.id, "Building entity id");
    if (!can_recognize_building_entity_type(entity.type)) {
        invalid("Unsupported building entity type: " + entity.type);
    }
    require_object(entity.properties, "Building entity properties");
    require_schema_version(entity.properties);
    const auto form = form_field(entity.properties);

    BuildingObject object;
    if (entity.type == "column") {
        object = decode_column(entity, entity.properties, form);
    } else if (entity.type == "beam") {
        object = decode_beam(entity, entity.properties, form);
    } else if (entity.type == "stair") {
        object = decode_stair(entity, entity.properties, form);
    } else {
        object = decode_roof(entity, entity.properties, form);
    }
    validate_geometry(object);
    return object;
}

TopoDS_Shape make_building_shape(const BuildingObject& object) {
    return std::visit(
        [](const auto& value) -> TopoDS_Shape {
            using Object = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Object, RectangularColumn>) {
                return make_rectangular_column(value);
            } else if constexpr (std::is_same_v<Object, CircularColumn>) {
                return make_circular_column(value);
            } else if constexpr (std::is_same_v<Object, Beam>) {
                return make_beam(value);
            } else if constexpr (std::is_same_v<Object, StairFlight>) {
                return make_stair_flight(value);
            } else if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
                return make_sloped_roof_panel(value);
            } else if constexpr (std::is_same_v<Object, GableRoof>) {
                return make_gable_roof(value);
            } else {
                return make_hip_roof(value);
            }
        },
        object);
}

}  // namespace sketch
