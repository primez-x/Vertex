#include "sketch/wall_measurement.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/geometry_operations.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

constexpr double geometry_envelope_metres = 1'000'000.0;
constexpr std::size_t maximum_source_walls = 2'048;
constexpr double parallel_direction_tolerance = 1e-12;

constexpr std::array<std::string_view, 5> context_fields{
    "property_id", "building_id", "floor_id", "layer_id", "phase_id"};

[[noreturn]] void reject(std::string message) {
    throw std::invalid_argument(std::move(message));
}

bool bounded(double value) {
    return std::isfinite(value) && std::abs(value) <= geometry_envelope_metres;
}

double number(const Json& value, std::string_view label) {
    if (!value.is_number()) reject(std::string(label) + " must be a finite number");
    double result{};
    try {
        result = value.get<double>();
    } catch (const Json::exception&) {
        reject(std::string(label) + " must be a finite number");
    }
    if (!std::isfinite(result)) reject(std::string(label) + " must be a finite number");
    return result;
}

Vec2 point(const Json& value, std::string_view label) {
    if (!value.is_array() || value.size() != 2)
        reject(std::string(label) + " must contain two coordinates");
    const Vec2 result{number(value[0], label), number(value[1], label)};
    if (!bounded(result.x) || !bounded(result.y))
        reject("Wall coordinates exceed the supported +/-1e6 metre envelope");
    return result;
}

Segment baseline(const Json& properties) {
    const auto found = properties.find("baseline");
    if (found == properties.end() || !found->is_object() ||
        !found->contains("start") || !found->contains("end"))
        reject("Source wall baseline is missing or malformed");
    double sweep = 0.0;
    if (const auto sweep_value = found->find("sweep_radians"); sweep_value != found->end())
        sweep = number(*sweep_value, "Wall baseline sweep");
    if (sweep != 0.0)
        reject("Curved source walls are not supported for exterior measurements");
    const Segment result{point(found->at("start"), "Wall baseline point"),
                         point(found->at("end"), "Wall baseline point"), sweep};
    double length{};
    try {
        length = segment_length(result);
    } catch (const std::invalid_argument&) {
        reject("Source wall baseline is invalid");
    }
    if (!(length > default_geometry_tolerance_metres) || !std::isfinite(length))
        reject("Source wall baseline is degenerate");
    return result;
}

double thickness(const Json& properties) {
    auto found = properties.find("thickness_m");
    if (found == properties.end()) found = properties.find("thickness");
    if (found == properties.end()) reject("Source wall thickness is missing");
    const auto result = number(*found, "Wall thickness");
    if (!(result > default_geometry_tolerance_metres) || !bounded(result))
        reject("Wall thickness is outside the supported positive geometry envelope");
    return result;
}

Json wall_context(const Json& properties) {
    Json context = Json::object();
    for (const auto field : context_fields) {
        const auto found = properties.find(std::string(field));
        if (found == properties.end()) continue;
        if (!found->is_string() || found->get_ref<const std::string&>().empty())
            reject(std::string("Source wall ") + std::string(field) + " must be a non-empty string");
        context[std::string(field)] = *found;
    }
    return context;
}

struct SourceWall {
    std::string id;
    Segment baseline;
    double thickness{};
    Json context;
};

std::pair<double, double> point_key(Vec2 point) {
    return {point.x, point.y};
}

std::vector<SourceWall> read_source_walls(const DocumentSnapshot& document,
                                          const std::vector<std::string>& wall_ids) {
    if (wall_ids.size() < 3)
        reject("At least three source walls are required for an exterior loop");
    if (wall_ids.size() > maximum_source_walls)
        reject("Exterior measurement exceeds the supported source wall count");

    std::set<std::string, std::less<>> unique_ids;
    std::vector<SourceWall> result;
    result.reserve(wall_ids.size());
    std::optional<Json> shared_floor;
    std::optional<Json> shared_layer;
    bool first_wall = true;
    const auto validate_shared_context = [&](const Json& context, std::string_view field,
                                             std::optional<Json>& shared) {
        const auto value = context.find(std::string(field));
        const std::optional<Json> current = value == context.end()
            ? std::nullopt : std::optional<Json>(*value);
        if (first_wall) shared = current;
        else if (shared != current)
            reject("All source walls must belong to the same floor and layer");
    };
    for (const auto& id : wall_ids) {
        if (id.empty() || !unique_ids.insert(id).second)
            reject("Source wall IDs must be unique and non-empty");
        const auto found = document.entities().find(id);
        if (found == document.entities().end() || found->second.type != "wall")
            reject("Exterior measurement source wall is missing or is not a wall: " + id);
        const auto& properties = found->second.properties;
        if (!properties.is_object()) reject("Source wall properties must be an object");
        SourceWall wall{id, baseline(properties), thickness(properties), wall_context(properties)};
        validate_shared_context(wall.context, "floor_id", shared_floor);
        validate_shared_context(wall.context, "layer_id", shared_layer);
        first_wall = false;
        result.push_back(std::move(wall));
    }
    return result;
}

Json source_for_walls(const std::vector<SourceWall>& walls) {
    std::vector<const SourceWall*> ordered;
    ordered.reserve(walls.size());
    for (const auto& wall : walls) ordered.push_back(&wall);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    Json records = Json::array();
    for (const auto* wall : ordered)
        records.push_back({{"id", wall->id}, {"context", wall->context}});
    return {{"version", 1}, {"basis", "exterior"}, {"walls", std::move(records)}};
}

double cross(Vec2 left, Vec2 right) {
    return left.x * right.y - left.y * right.x;
}

double dot(Vec2 left, Vec2 right) {
    return left.x * right.x + left.y * right.y;
}

Vec2 add(Vec2 left, Vec2 right) {
    return {left.x + right.x, left.y + right.y};
}

Vec2 subtract(Vec2 left, Vec2 right) {
    return {left.x - right.x, left.y - right.y};
}

Vec2 multiply(Vec2 point, double scale) {
    return {point.x * scale, point.y * scale};
}

double distance(Vec2 left, Vec2 right) {
    return std::hypot(left.x - right.x, left.y - right.y);
}

struct UnitEdge {
    Vec2 direction;
    Vec2 outward;
    double half_thickness{};
};

Boundary offset_loop(const Boundary& loop, const std::vector<SourceWall>& walls) {
    if (loop.size() != walls.size()) reject("Source wall loop assembly was incomplete");
    const auto area = signed_area(loop);
    if (!std::isfinite(area) || std::abs(area) <= default_geometry_tolerance_metres)
        reject("Source wall loop has no stable orientation");
    const double outward_multiplier = area > 0.0 ? -1.0 : 1.0;

    std::vector<UnitEdge> edges;
    edges.reserve(loop.size());
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const auto& segment = loop[index];
        const auto dx = segment.end.x - segment.start.x;
        const auto dy = segment.end.y - segment.start.y;
        const auto length = std::hypot(dx, dy);
        if (!(length > default_geometry_tolerance_metres) || !std::isfinite(length))
            reject("Offset source wall segment is degenerate");
        const Vec2 direction{dx / length, dy / length};
        const Vec2 left_normal{-direction.y, direction.x};
        edges.push_back({direction, multiply(left_normal, outward_multiplier),
                         walls[index].thickness * 0.5});
    }

    std::vector<Vec2> corners(loop.size());
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const auto previous = (index + loop.size() - 1) % loop.size();
        const auto vertex = loop[index].start;
        const auto& incoming = edges[previous];
        const auto& outgoing = edges[index];
        const auto incoming_point = add(vertex, multiply(incoming.outward,
                                                          incoming.half_thickness));
        const auto outgoing_point = add(vertex, multiply(outgoing.outward,
                                                          outgoing.half_thickness));
        const auto denominator = cross(incoming.direction, outgoing.direction);
        const auto incoming_raw = subtract(loop[previous].end, loop[previous].start);
        const auto outgoing_raw = subtract(loop[index].end, loop[index].start);
        const bool collinear_continuation = cross(incoming_raw, outgoing_raw) == 0.0 &&
                                             dot(incoming_raw, outgoing_raw) > 0.0;
        if (collinear_continuation) {
            if (incoming.half_thickness != outgoing.half_thickness ||
                distance(incoming_point, outgoing_point) > default_geometry_tolerance_metres)
                reject("Parallel wall offsets do not have one bounded common corner");
            corners[index] = multiply(add(incoming_point, outgoing_point), 0.5);
        } else {
            if (denominator == 0.0 || std::abs(denominator) < parallel_direction_tolerance)
                reject("A near-parallel wall corner has an ambiguous unbounded miter");
            const auto parameter = cross(subtract(outgoing_point, incoming_point),
                                         outgoing.direction) / denominator;
            if (!std::isfinite(parameter)) reject("Wall miter exceeds the supported numeric range");
            const auto corner = add(incoming_point, multiply(incoming.direction, parameter));
            if (!bounded(corner.x) || !bounded(corner.y))
                reject("Wall miter exceeds the supported +/-1e6 metre envelope");
            corners[index] = corner;
        }
        if (!bounded(corners[index].x) || !bounded(corners[index].y))
            reject("Wall miter exceeds the supported +/-1e6 metre envelope");
    }

    Boundary result;
    result.reserve(loop.size());
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const auto next = (index + 1) % loop.size();
        Segment segment{corners[index], corners[next], 0.0};
        if (!(segment_length(segment) > default_geometry_tolerance_metres))
            reject("Exterior wall offset produced a degenerate segment");
        result.push_back(segment);
    }
    if (const auto diagnostics = validate_boundary(result); !diagnostics.empty())
        reject("Exterior wall offset is not a simple closed boundary: " + diagnostics.front().message);
    return result;
}

Boundary actual_boundary_geometry(const Entity& boundary) {
    if (!boundary.properties.is_object()) reject("Boundary properties must be an object");
    const auto format = inspect_boundary_entity_version(boundary);
    if (format.format == BoundaryEntityFormat::identified_v1)
        return boundary_geometry(decode_identified_boundary_entity(boundary));
    if (format.format == BoundaryEntityFormat::unsupported_version)
        reject(format.diagnostic);
    const Json* values = nullptr;
    if (const auto boundary_value = boundary.properties.find("boundary");
        boundary_value != boundary.properties.end())
        values = &*boundary_value;
    else if (const auto segments_value = boundary.properties.find("segments");
             segments_value != boundary.properties.end())
        values = &*segments_value;
    if (values == nullptr || !values->is_array() || values->empty())
        reject("Boundary geometry is missing");
    Boundary result;
    result.reserve(values->size());
    for (const auto& value : *values) {
        if (!value.is_object() || !value.contains("start") || !value.contains("end"))
            reject("Boundary segment is incomplete");
        double sweep = 0.0;
        if (const auto field = value.find("sweep_radians"); field != value.end())
            sweep = number(*field, "Boundary sweep");
        result.push_back({point(value.at("start"), "Boundary point"),
                          point(value.at("end"), "Boundary point"), sweep});
    }
    return result;
}

using EdgeKey = std::tuple<double, double, double, double>;

std::vector<EdgeKey> edge_keys(const Boundary& boundary) {
    std::vector<EdgeKey> result;
    result.reserve(boundary.size());
    for (const auto& segment : boundary) {
        if (segment.sweep_radians != 0.0) reject("Wall measurement outlines must be straight");
        auto start = point_key(segment.start);
        auto end = point_key(segment.end);
        if (end < start) std::swap(start, end);
        result.emplace_back(start.first, start.second, end.first, end.second);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void validate_source_schema(const Json& source, std::vector<std::string>& ids) {
    if (!source.is_object() || source.size() != 3 || !source.contains("version") ||
        !source.contains("basis") || !source.contains("walls"))
        reject("Wall measurement source is incomplete");
    const auto& version = source.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || version != 1 ||
        !source.at("basis").is_string() || source.at("basis") != "exterior" ||
        !source.at("walls").is_array() || source.at("walls").empty() ||
        source.at("walls").size() > maximum_source_walls)
        reject("Wall measurement source has an unknown or invalid schema");
    std::set<std::string, std::less<>> seen;
    for (const auto& record : source.at("walls")) {
        if (!record.is_object() || record.size() != 2 || !record.contains("id") ||
            !record.contains("context") || !record.at("id").is_string() ||
            !record.at("context").is_object())
            reject("Wall measurement source record is malformed");
        const auto id = record.at("id").get<std::string>();
        if (id.empty() || !seen.insert(id).second)
            reject("Wall measurement source IDs must be unique and non-empty");
        for (const auto& [key, value] : record.at("context").items()) {
            if (std::find(context_fields.begin(), context_fields.end(), key) == context_fields.end() ||
                !value.is_string() || value.get_ref<const std::string&>().empty())
                reject("Wall measurement source context is malformed");
        }
        ids.push_back(id);
    }
}

Json normalize_source_order(const Json& source) {
    std::vector<Json> records;
    records.reserve(source.at("walls").size());
    for (const auto& record : source.at("walls")) records.push_back(record);
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.at("id").get_ref<const std::string&>() <
               right.at("id").get_ref<const std::string&>();
    });
    Json walls = Json::array();
    for (auto& record : records) walls.push_back(std::move(record));
    return {{"version", 1}, {"basis", "exterior"}, {"walls", std::move(walls)}};
}

bool boundary_context_matches(const Entity& boundary, const Json& source) {
    if (!boundary.properties.is_object()) return false;
    for (const auto& record : source.at("walls")) {
        const auto& context = record.at("context");
        for (const auto field : {std::string_view{"floor_id"}, std::string_view{"layer_id"}}) {
            const auto source_value = context.find(std::string(field));
            if (source_value == context.end()) continue;
            const auto boundary_value = boundary.properties.find(std::string(field));
            if (boundary_value == boundary.properties.end() || !boundary_value->is_string() ||
                *boundary_value != *source_value)
                return false;
        }
    }
    return true;
}

} // namespace

WallMeasurementResult derive_exterior_wall_measurement(
    const DocumentSnapshot& document, const std::vector<std::string>& wall_ids) {
    auto walls = read_source_walls(document, wall_ids);
    auto geometry_walls = walls;
    for (auto& wall : geometry_walls) {
        if (point_key(wall.baseline.end) < point_key(wall.baseline.start))
            std::swap(wall.baseline.start, wall.baseline.end);
    }
    std::sort(geometry_walls.begin(), geometry_walls.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    std::vector<Segment> baselines;
    baselines.reserve(geometry_walls.size());
    std::map<EdgeKey, const SourceWall*> wall_by_baseline;
    for (const auto& wall : geometry_walls) {
        baselines.push_back(wall.baseline);
        const auto start = point_key(wall.baseline.start);
        const auto end = point_key(wall.baseline.end);
        wall_by_baseline.emplace(EdgeKey{start.first, start.second, end.first, end.second}, &wall);
    }
    const auto loop = assemble_boundary_from_segments(baselines);
    std::vector<SourceWall> loop_walls;
    loop_walls.reserve(loop.size());
    for (const auto& segment : loop) {
        auto start = point_key(segment.start);
        auto end = point_key(segment.end);
        if (end < start) std::swap(start, end);
        const auto found = wall_by_baseline.find(
            EdgeKey{start.first, start.second, end.first, end.second});
        if (found == wall_by_baseline.end()) reject("Assembled wall loop lost a source wall");
        loop_walls.push_back(*found->second);
    }
    auto boundary = offset_loop(loop, loop_walls);
    if (signed_area(boundary) < 0.0) {
        std::reverse(boundary.begin(), boundary.end());
        for (auto& segment : boundary) std::swap(segment.start, segment.end);
        std::reverse(loop_walls.begin(), loop_walls.end());
    }
    const auto seed = geometry_walls.front().id;
    const auto seed_edge = std::find_if(loop_walls.begin(), loop_walls.end(), [&](const auto& wall) {
        return wall.id == seed;
    });
    if (seed_edge == loop_walls.end()) reject("Exterior boundary lost its stable source wall seed");
    const auto rotation = static_cast<std::size_t>(seed_edge - loop_walls.begin());
    std::rotate(boundary.begin(), boundary.begin() + static_cast<std::ptrdiff_t>(rotation),
                boundary.end());
    std::rotate(loop_walls.begin(), loop_walls.begin() + static_cast<std::ptrdiff_t>(rotation),
                loop_walls.end());
    return {std::move(boundary), source_for_walls(walls)};
}

bool wall_measurement_source_current(const DocumentSnapshot& document, const Entity& boundary) {
    try {
        if (!boundary.properties.is_object()) return false;
        const auto source_property = boundary.properties.find("wall_measurement_source");
        if (source_property == boundary.properties.end()) return true;
        if (boundary.type != "boundary" && boundary.type != "measurement_boundary") return false;
        std::vector<std::string> wall_ids;
        validate_source_schema(*source_property, wall_ids);
        const auto expected = derive_exterior_wall_measurement(document, wall_ids);
        if (normalize_source_order(*source_property) != expected.source ||
            !boundary_context_matches(boundary, expected.source))
            return false;
        const auto actual = actual_boundary_geometry(boundary);
        if (!validate_boundary(actual).empty()) return false;
        return edge_keys(actual) == edge_keys(expected.boundary);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace sketch
