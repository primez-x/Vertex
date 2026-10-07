#include "sketch/document_wall.hpp"

#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
const Json* property(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object()) {
        return nullptr;
    }
    for (const auto* name : names) {
        const auto found = object.find(name);
        if (found != object.end()) {
            return &*found;
        }
    }
    return nullptr;
}

bool finite_number(const Json& value, double& output, std::string_view label,
                   std::string& error) {
    if (!value.is_number()) {
        error = std::string(label) + " must be a finite number";
        return false;
    }
    try {
        output = value.get<double>();
    } catch (const Json::exception&) {
        error = std::string(label) + " must be a finite number";
        return false;
    }
    if (!std::isfinite(output)) {
        error = std::string(label) + " must be a finite number";
        return false;
    }
    return true;
}

bool required_number(const Json& object, std::initializer_list<const char*> names,
                     double& output, std::string_view label, std::string& error) {
    const auto* value = property(object, names);
    if (value == nullptr) {
        error = std::string(label) + " is required";
        return false;
    }
    return finite_number(*value, output, label, error);
}

bool required_string(const Json& object, std::initializer_list<const char*> names,
                     std::string& output, std::string_view label, std::string& error) {
    const auto* value = property(object, names);
    if (value == nullptr || !value->is_string()) {
        error = std::string(label) + " is required and must be a non-empty string";
        return false;
    }
    try {
        output = value->get<std::string>();
    } catch (const Json::exception&) {
        error = std::string(label) + " is required and must be a non-empty string";
        return false;
    }
    if (output.empty()) {
        error = std::string(label) + " is required and must be a non-empty string";
        return false;
    }
    return true;
}

bool required_point(const Json& value, Vec2& output, std::string_view label,
                    std::string& error) {
    if (!value.is_array() || value.size() != 2) {
        error = std::string(label) + " must be [x, y]";
        return false;
    }
    if (!finite_number(value[0], output.x, std::string(label) + "[0]", error) ||
        !finite_number(value[1], output.y, std::string(label) + "[1]", error)) {
        return false;
    }
    return true;
}

bool required_segment(const Json& value, Segment& output, std::string_view label,
                      std::string& error) {
    if (!value.is_object()) {
        error = std::string(label) + " must be an object";
        return false;
    }
    const auto* start = property(value, {"start"});
    const auto* end = property(value, {"end"});
    if (start == nullptr || end == nullptr) {
        error = std::string(label) + " requires start and end points";
        return false;
    }
    if (!required_point(*start, output.start, std::string(label) + ".start", error) ||
        !required_point(*end, output.end, std::string(label) + ".end", error)) {
        return false;
    }
    return required_number(value, {"sweep_radians"}, output.sweep_radians,
                           std::string(label) + ".sweep_radians", error);
}

} // namespace

bool read_document_wall_top_profile(const Entity& entity, Wall& output, std::string& error) {
    output.slope_rise.reset();
    output.top_gradient_m_per_m.reset();
    if (!entity.properties.is_object()) {
        error = "properties must be an object";
        return false;
    }
    if (const auto* slope = property(entity.properties, {"slope_rise_m", "slope_rise"})) {
        if (!finite_number(*slope, output.slope_rise.emplace(), "slope_rise_m", error)) return false;
    }
    if (const auto* plane = property(entity.properties, {"top_plane"})) {
        try {
            output.top_gradient_m_per_m = parse_wall_top_plane(*plane);
            if (!output.slope_rise) {
                const auto gradient = *output.top_gradient_m_per_m;
                const auto chord = output.baseline.end - output.baseline.start;
                const auto rise = gradient.x * chord.x + gradient.y * chord.y;
                if (!std::isfinite(rise)) throw std::invalid_argument("Wall top plane rise is not finite");
                output.slope_rise = rise;
            }
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }
    return true;
}

bool read_document_wall(const Entity& entity, const std::vector<const Entity*>& opening_entities,
               Wall& output, std::string& error) {
    if (!entity.properties.is_object()) {
        error = "properties must be an object";
        return false;
    }
    output = Wall{};
    output.id = entity.id;
    const auto* baseline = property(entity.properties, {"baseline"});
    if (baseline == nullptr ||
        !required_segment(*baseline, output.baseline, "baseline", error)) {
        return false;
    }
    if (!required_number(entity.properties, {"thickness_m", "thickness"}, output.thickness,
                         "thickness_m", error) ||
        !required_number(entity.properties, {"height_m", "height"}, output.height, "height_m",
                         error) ||
        !required_number(entity.properties, {"elevation_m", "elevation"}, output.elevation,
                         "elevation_m", error)) {
        return false;
    }

    if (const auto* layers = property(entity.properties, {"layers"})) {
        try {
            output.layers = parse_wall_layers(*layers, output.thickness);
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }
    if (!read_document_wall_top_profile(entity, output, error)) return false;

    output.openings.reserve(opening_entities.size());
    for (const auto* opening_entity : opening_entities) {
        if (opening_entity == nullptr || !opening_entity->properties.is_object()) {
            error = "hosted opening properties must be an object";
            return false;
        }
        HostedOpening opening;
        opening.id = opening_entity->id;
        if (!required_number(opening_entity->properties, {"offset_m", "offset"}, opening.offset,
                             "offset_m", error) ||
            !required_number(opening_entity->properties, {"width_m", "width"}, opening.width,
                             "width_m", error) ||
            !required_number(opening_entity->properties, {"sill_m", "sill"}, opening.sill,
                             "sill_m", error) ||
            !required_number(opening_entity->properties, {"height_m", "height"}, opening.height,
                             "height_m", error)) {
            return false;
        }
        output.openings.push_back(opening);
    }
    return true;
}

bool read_document_wall_id(const Entity& entity, std::string& wall_id, std::string& error) {
    if (!entity.properties.is_object()) {
        error = "properties must be an object";
        return false;
    }
    return required_string(entity.properties, {"wall_id"}, wall_id, "wall_id", error);
}

} // namespace sketch
