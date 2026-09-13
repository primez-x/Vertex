#include "sketch/document_solid.hpp"

#include <cmath>
#include <initializer_list>
#include <string_view>
#include <utility>

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

bool required_boundary(const Json& value, Boundary& output, std::string_view label,
                       std::string& error) {
    if (!value.is_array() || value.empty()) {
        error = std::string(label) + " must be a non-empty segment array";
        return false;
    }
    output.clear();
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        Segment segment;
        if (!required_segment(value[index], segment,
                              std::string(label) + "[" + std::to_string(index) + "]", error)) {
            return false;
        }
        output.push_back(segment);
    }
    return true;
}

} // namespace

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
    if (const auto* slope = property(entity.properties, {"slope_rise_m", "slope_rise"})) {
        if (!finite_number(*slope, output.slope_rise.emplace(), "slope_rise_m", error)) {
            return false;
        }
    }

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

bool read_document_slab(const Entity& entity, Slab& output, std::string& error) {
    if (!entity.properties.is_object()) {
        error = "properties must be an object";
        return false;
    }
    output = Slab{};
    output.id = entity.id;
    if (const auto* kind = property(entity.properties, {"element_kind"})) {
        if (!kind->is_string()) {
            error = "element_kind must be one of slab, floor, ceiling, or foundation";
            return false;
        }
        try {
            const auto parsed = parse_slab_element_kind(kind->get<std::string>());
            if (!parsed.has_value()) {
                error = "element_kind must be one of slab, floor, ceiling, or foundation";
                return false;
            }
            output.element_kind = *parsed;
        } catch (const Json::exception&) {
            error = "element_kind must be one of slab, floor, ceiling, or foundation";
            return false;
        }
    }
    const auto* boundary = property(entity.properties, {"boundary"});
    const auto* holes = property(entity.properties, {"holes"});
    if (boundary == nullptr ||
        !required_boundary(*boundary, output.boundary, "boundary", error)) {
        return false;
    }
    if (holes == nullptr || !holes->is_array()) {
        error = "holes is required and must be an array of segment arrays";
        return false;
    }
    output.holes.reserve(holes->size());
    for (std::size_t index = 0; index < holes->size(); ++index) {
        Boundary hole;
        if (!required_boundary((*holes)[index], hole,
                               "holes[" + std::to_string(index) + "]", error)) {
            return false;
        }
        output.holes.push_back(std::move(hole));
    }
    return required_number(entity.properties, {"thickness_m", "thickness"}, output.thickness,
                           "thickness_m", error) &&
           required_number(entity.properties, {"elevation_m", "elevation"}, output.elevation,
                           "elevation_m", error);
}

bool read_document_wall_id(const Entity& entity, std::string& wall_id, std::string& error) {
    if (!entity.properties.is_object()) {
        error = "properties must be an object";
        return false;
    }
    return required_string(entity.properties, {"wall_id"}, wall_id, "wall_id", error);
}

} // namespace sketch
