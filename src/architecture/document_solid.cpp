#include "sketch/document_solid.hpp"

#include <algorithm>
#include <array>
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
    if (!required_number(entity.properties, {"thickness_m", "thickness"}, output.thickness,
                         "thickness_m", error) ||
        !required_number(entity.properties, {"elevation_m", "elevation"}, output.elevation,
                         "elevation_m", error)) {
        return false;
    }
    if (const auto* layers = property(entity.properties, {"layers"})) {
        try {
            output.layers = parse_slab_layers(*layers, output.thickness);
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }
    return true;
}

bool has_document_room_volume_fields(const Entity& entity) {
    return property(entity.properties, {"height_m", "height"}) != nullptr &&
        property(entity.properties, {"elevation_m", "elevation"}) != nullptr;
}

bool read_document_room_footprint(const Entity& entity, DocumentRoomFootprint& output,
                                  std::string& error) {
    if (!entity.properties.is_object()) {
        error = "properties must be an object";
        return false;
    }

    DocumentRoomFootprint candidate;
    const auto* boundary = property(entity.properties, {"boundary", "segments"});
    if (boundary == nullptr ||
        !required_boundary(*boundary, candidate.boundary, "boundary", error)) {
        return false;
    }
    if (const auto* holes = property(entity.properties, {"holes"})) {
        if (!holes->is_array()) {
            error = "holes must be an array of segment arrays";
            return false;
        }
        candidate.holes.reserve(holes->size());
        for (std::size_t index = 0; index < holes->size(); ++index) {
            Boundary hole;
            if (!required_boundary((*holes)[index], hole,
                                   "holes[" + std::to_string(index) + "]", error)) {
                return false;
            }
            candidate.holes.push_back(std::move(hole));
        }
    }
    try {
        if (const auto invalid = validate_boundary_holes(candidate.boundary, candidate.holes)) {
            error = *invalid;
            return false;
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    output = std::move(candidate);
    return true;
}

std::optional<Vec2> room_footprint_rectangle_dimensions(const DocumentRoomFootprint& footprint) {
    const auto& boundary = footprint.boundary;
    if (!footprint.holes.empty() || boundary.size() != 4 ||
        std::any_of(boundary.begin(), boundary.end(),
            [](const Segment& edge) { return edge.sweep_radians != 0; })) return std::nullopt;
    const auto origin = boundary[0].start;
    const auto& first = boundary[0];
    const auto& second = boundary[1];
    const double width = std::hypot(first.end.x-first.start.x, first.end.y-first.start.y);
    const double depth = std::hypot(second.end.x-second.start.x, second.end.y-second.start.y);
    if (!std::isfinite(width) || !std::isfinite(depth) || width <= 0 || depth <= 0)
        return std::nullopt;
    const Vec2 u{(first.end.x-first.start.x)/width, (first.end.y-first.start.y)/width};
    const Vec2 v{(second.end.x-second.start.x)/depth, (second.end.y-second.start.y)/depth};
    if (std::abs(u.x*v.x + u.y*v.y) > 1e-10) return std::nullopt;
    const auto corner = [&](double x, double y) {
        return Vec2{origin.x + u.x*x + v.x*y, origin.y + u.y*x + v.y*y};
    };
    const std::array<Vec2, 4> expected{origin, corner(width, 0),
        corner(width, depth), corner(0, depth)};
    const auto close = [](Vec2 a, Vec2 b) {
        return std::hypot(a.x-b.x, a.y-b.y) <= default_geometry_tolerance_metres;
    };
    for (std::size_t i = 0; i < 4; ++i)
        if (!close(boundary[i].start, expected[i]) ||
            !close(boundary[i].end, expected[(i+1)%4])) return std::nullopt;
    return Vec2{width,depth};
}

bool read_document_room(const Entity& entity, RoomVolume& output, std::string& error) {
    DocumentRoomFootprint footprint;
    if (!read_document_room_footprint(entity, footprint, error)) return false;
    RoomVolume candidate;
    candidate.id = entity.id;
    candidate.boundary = std::move(footprint.boundary);
    candidate.holes = std::move(footprint.holes);
    if (!required_number(entity.properties, {"height_m", "height"}, candidate.height,
                         "height_m", error) ||
        !required_number(entity.properties, {"elevation_m", "elevation"}, candidate.elevation,
                         "elevation_m", error)) {
        return false;
    }
    try {
        (void)make_room_volume(candidate);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    output = std::move(candidate);
    return true;
}

} // namespace sketch
