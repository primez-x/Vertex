#include "sketch/boundary_dimension.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/physical_wall_room_data.hpp"
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
#include "sketch/physical_room_dimension_source.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
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

std::optional<BoundaryDimensionKind> kind_from_name(std::string_view name) {
    if (name == "segment_length") return BoundaryDimensionKind::segment_length;
    if (name == "angle") return BoundaryDimensionKind::angle;
    if (name == "area") return BoundaryDimensionKind::area;
    if (name == "wall_axis_length") return BoundaryDimensionKind::wall_axis_length;
    return std::nullopt;
}

void validate_presentation(const BoundaryDimensionPresentation& value) {
    if (!std::isfinite(value.text_height_mm) || value.text_height_mm < 0.5 || value.text_height_mm > 20.0)
        invalid("dimension text height must be between 0.5 and 20 millimetres");
    if (value.color.size() != 7 || value.color.front() != '#' ||
        !std::all_of(value.color.begin() + 1, value.color.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))
        invalid("dimension color must be a #RRGGBB hexadecimal color");
    if (!std::isfinite(value.rotation_radians)) invalid("dimension text rotation must be finite");
}

BoundaryDimensionPresentation decode_presentation(const Json& value) {
    if (!value.is_object() || value.size() != 6)
        invalid("dimension presentation must contain exactly six fields");
    const auto boolean = [&](std::string_view key) {
        const auto& field = required_property(value, key);
        if (!field.is_boolean()) invalid("dimension presentation " + std::string(key) + " must be boolean");
        return field.get<bool>();
    };
    BoundaryDimensionPresentation result{
        json_finite_double(required_property(value, "text_height_mm"), "dimension text height must be finite"),
        json_string(required_property(value, "color"), "dimension color must be a string"),
        boolean("bold"), boolean("italic"), boolean("visible"),
        json_finite_double(required_property(value, "rotation_radians"), "dimension text rotation must be finite")};
    validate_presentation(result);
    return result;
}

void validate_model(const BoundaryDimension& dimension) {
    if (dimension.presentation) validate_presentation(*dimension.presentation);
    if (!valid_identifier(dimension.id)) {
        invalid("dimension id is empty or invalid");
    }
    if (!valid_identifier(dimension.boundary_id)) {
        invalid("dimension target entity id is empty or invalid");
    }
    if (!dimension.segment_chain_ids.empty()) {
        if (dimension.kind != BoundaryDimensionKind::segment_length ||
            dimension.segment_chain_ids.size() < 2 || dimension.segment_chain_ids.size() > 128 ||
            dimension.segment_id != dimension.segment_chain_ids.front())
            invalid("dimension segment chain must contain 2..128 edges and match its first segment id");
        std::set<std::string> unique;
        for (const auto& id : dimension.segment_chain_ids)
            if (!valid_identifier(id) || !unique.insert(id).second)
                invalid("dimension segment chain IDs must be valid and unique");
    }
    switch (dimension.kind) {
        case BoundaryDimensionKind::segment_length:
            if (!valid_identifier(dimension.segment_id)) {
                invalid("dimension target segment id is empty or invalid");
            }
            if (!dimension.vertex_id.empty() || !dimension.secondary_segment_id.empty()) {
                invalid("segment-length dimension contains angle target fields");
            }
            break;
        case BoundaryDimensionKind::angle:
            if (!valid_identifier(dimension.segment_id) ||
                !valid_identifier(dimension.secondary_segment_id) ||
                !valid_identifier(dimension.vertex_id)) {
                invalid("angle dimension target IDs are empty or invalid");
            }
            if (dimension.segment_id == dimension.secondary_segment_id) {
                invalid("angle dimension must reference two different segments");
            }
            break;
        case BoundaryDimensionKind::area:
        case BoundaryDimensionKind::wall_axis_length:
            if (!dimension.segment_id.empty() || !dimension.vertex_id.empty() ||
                !dimension.secondary_segment_id.empty()) {
                invalid("owner-only dimension cannot contain segment or vertex target fields");
            }
            break;
        default:
            invalid("dimension kind is unsupported");
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
                (*dimension.automatic_placement_version != 1 &&
                 *dimension.automatic_placement_version != 2)) {
                invalid("automatic dimension placement version is unsupported");
            }
            return;
    }
    invalid("dimension placement origin is unsupported");
}

const IdentifiedSegment* find_segment(const IdentifiedBoundary& boundary,
                                      std::string_view id) {
    const auto found = std::find_if(boundary.segments.begin(), boundary.segments.end(),
                                    [&](const IdentifiedSegment& segment) {
                                        return segment.segment_id == id;
                                    });
    return found == boundary.segments.end() ? nullptr : &*found;
}

Vec2 tangent_from_vertex(const IdentifiedSegment& identified, std::string_view vertex_id) {
    const auto& segment = identified.segment;
    const bool at_start = identified.start_vertex_id == vertex_id;
    const bool at_end = identified.end_vertex_id == vertex_id;
    if (at_start == at_end) {
        invalid("angle dimension target segment does not contain the requested vertex");
    }
    Vec2 tangent{segment.end.x - segment.start.x, segment.end.y - segment.start.y};
    if (segment.sweep_radians != 0.0) {
        const auto chord_x = tangent.x;
        const auto chord_y = tangent.y;
        const auto chord = std::hypot(chord_x, chord_y);
        const auto half_sweep = segment.sweep_radians * 0.5;
        const auto arc_tangent = std::tan(half_sweep);
        if (!(chord > default_geometry_tolerance_metres) || !std::isfinite(arc_tangent) ||
            std::abs(arc_tangent) <= 1e-12) {
            invalid("angle dimension cannot resolve an invalid arc tangent");
        }
        const Vec2 midpoint{(segment.start.x + segment.end.x) * 0.5,
                            (segment.start.y + segment.end.y) * 0.5};
        const auto center_offset = chord / (2.0 * arc_tangent);
        const Vec2 center{midpoint.x - chord_y / chord * center_offset,
                          midpoint.y + chord_x / chord * center_offset};
        const Vec2 point = at_start ? segment.start : segment.end;
        const Vec2 radial{point.x - center.x, point.y - center.y};
        tangent = segment.sweep_radians > 0.0
                      ? Vec2{-radial.y, radial.x}
                      : Vec2{radial.y, -radial.x};
        if (at_end) tangent = {-tangent.x, -tangent.y};
    } else if (at_end) {
        tangent = {-tangent.x, -tangent.y};
    }
    const auto magnitude = std::hypot(tangent.x, tangent.y);
    if (!(magnitude > default_geometry_tolerance_metres) || !std::isfinite(magnitude)) {
        invalid("angle dimension target segment has a degenerate tangent");
    }
    return {tangent.x / magnitude, tangent.y / magnitude};
}

bool closed_boundary(const Boundary& boundary) {
    if (boundary.empty()) return false;
    for (std::size_t index = 1; index < boundary.size(); ++index) {
        const auto& previous = boundary[index - 1];
        const auto& current = boundary[index];
        if (std::hypot(previous.end.x - current.start.x,
                       previous.end.y - current.start.y) > default_geometry_tolerance_metres) {
            return false;
        }
    }
    const auto& first = boundary.front();
    const auto& last = boundary.back();
    return std::hypot(last.end.x - first.start.x, last.end.y - first.start.y) <=
           default_geometry_tolerance_metres;
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

std::string_view boundary_dimension_kind_name(BoundaryDimensionKind kind) {
    switch (kind) {
        case BoundaryDimensionKind::segment_length: return "segment_length";
        case BoundaryDimensionKind::angle: return "angle";
        case BoundaryDimensionKind::area: return "area";
        case BoundaryDimensionKind::wall_axis_length: return "wall_axis_length";
    }
    invalid("unknown dimension kind");
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
    if (version == 2) {
        return {BoundaryDimensionFormat::supported_v2, version, {}};
    }
    if (version == 3) {
        return {BoundaryDimensionFormat::supported_v3, version, {}};
    }
    if (version == 4) {
        const auto kind = entity.properties.find("dimension_kind");
        if (kind != entity.properties.end() && kind->is_string() && *kind == "wall_axis_length")
            return {BoundaryDimensionFormat::supported_v4, version, {}};
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
    const auto parsed_kind = kind_from_name(kind);
    if (!parsed_kind.has_value() ||
        (*parsed_kind == BoundaryDimensionKind::wall_axis_length && version != 4)) {
        return unsupported_result(entity, version, kind,
                                  "unsupported boundary dimension kind " + kind);
    }
    if (version == 3 && *parsed_kind != BoundaryDimensionKind::segment_length)
        invalid("dimension version three requires segment_length chain semantics");

    const auto& target = required_property(entity.properties, "target");
    if (!target.is_object()) {
        invalid("dimension target must be a JSON object");
    }
    const auto boundary_id = json_identifier(
        required_property(target, "entity_id"), "dimension target entity_id must be a valid id");
    std::string segment_id;
    std::string vertex_id;
    std::string secondary_segment_id;
    std::vector<std::string> segment_chain_ids;
    if (version != 3 && target.contains("segment_ids"))
        invalid("dimension segment chain requires version three");
    if (*parsed_kind == BoundaryDimensionKind::segment_length) {
        if (version == 3) {
            if (target.contains("segment_id") || target.contains("second_segment_id") || target.contains("vertex_id"))
                invalid("chain dimension target cannot contain single-edge or angle target fields");
            const auto& ids = required_property(target, "segment_ids");
            if (!ids.is_array() || ids.size() < 2 || ids.size() > 128)
                invalid("dimension segment chain must contain 2..128 edges");
            for (const auto& id : ids)
                segment_chain_ids.push_back(json_identifier(id, "dimension segment chain ID must be valid"));
            segment_id = segment_chain_ids.front();
        } else {
            segment_id = json_identifier(required_property(target, "segment_id"),
                                         "dimension target segment_id must be a valid id");
        }
    } else if (*parsed_kind == BoundaryDimensionKind::angle) {
        segment_id = json_identifier(required_property(target, "segment_id"),
                                     "angle dimension target segment_id must be a valid id");
        secondary_segment_id = json_identifier(
            required_property(target, "second_segment_id"),
            "angle dimension target second_segment_id must be a valid id");
        vertex_id = json_identifier(required_property(target, "vertex_id"),
                                    "angle dimension target vertex_id must be a valid id");
    } else {
        if (target.contains("segment_id") || target.contains("second_segment_id") ||
            target.contains("vertex_id")) {
            invalid("owner-only dimension target cannot contain segment or vertex fields");
        }
    }
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
        if (parsed != 1 && parsed != 2) {
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
        .kind = *parsed_kind,
        .vertex_id = vertex_id,
        .secondary_segment_id = secondary_segment_id,
        .segment_chain_ids = std::move(segment_chain_ids),
    };
    if (version == 2 || ((version == 3 || version == 4) && entity.properties.contains("presentation")))
        result.presentation = decode_presentation(required_property(entity.properties, "presentation"));
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
    bool preserve_presentation = false;
    bool preserve_text_position = false;
    if (original != nullptr) {
        validate_entity_container(*original);
        if (original->id != dimension.id) {
            invalid("original dimension entity id does not match the model");
        }
        const auto previous = decode_boundary_dimension_entity(*original);
        if (!previous.supported()) {
            invalid("cannot encode over unsupported dimension semantics");
        }
        if ((previous.version == 2 || ((previous.version == 3 || previous.version == 4) && previous.dimension->presentation)) && !dimension.presentation)
            invalid("cannot remove version two dimension presentation");
        if (previous.version == 1 && (dimension.presentation || !dimension.segment_chain_ids.empty() ||
            dimension.kind == BoundaryDimensionKind::wall_axis_length) &&
            original->properties.contains("presentation"))
            invalid("dimension presentation upgrade would overwrite opaque version one metadata");
        preserve_presentation = dimension.presentation == previous.dimension->presentation;
        preserve_text_position = dimension.text_position.x == previous.dimension->text_position.x &&
            dimension.text_position.y == previous.dimension->text_position.y;
        result = *original;
    }

    auto& properties = result.properties;
    properties["dimension_version"] = dimension.kind == BoundaryDimensionKind::wall_axis_length
        ? 4 : (!dimension.segment_chain_ids.empty() ? 3 : (dimension.presentation ? 2 : 1));
    if (dimension.presentation && !preserve_presentation) {
        const auto& value = *dimension.presentation;
        properties["presentation"] = {{"text_height_mm", value.text_height_mm}, {"color", value.color},
            {"bold", value.bold}, {"italic", value.italic}, {"visible", value.visible},
            {"rotation_radians", value.rotation_radians}};
    }
    properties["dimension_kind"] = std::string(boundary_dimension_kind_name(dimension.kind));
    Json target = Json::object();
    const auto previous_target = properties.find("target");
    if (previous_target != properties.end()) {
        if (!previous_target->is_object()) {
            invalid("dimension target must be a JSON object");
        }
        target = *previous_target;
    }
    target["entity_id"] = dimension.boundary_id;
    switch (dimension.kind) {
        case BoundaryDimensionKind::segment_length:
            if (dimension.segment_chain_ids.empty()) {
                target["segment_id"] = dimension.segment_id;
                target.erase("segment_ids");
            } else {
                target["segment_ids"] = dimension.segment_chain_ids;
                target.erase("segment_id");
            }
            target.erase("second_segment_id");
            target.erase("vertex_id");
            break;
        case BoundaryDimensionKind::angle:
            target.erase("segment_ids");
            target["segment_id"] = dimension.segment_id;
            target["second_segment_id"] = dimension.secondary_segment_id;
            target["vertex_id"] = dimension.vertex_id;
            break;
        case BoundaryDimensionKind::area:
        case BoundaryDimensionKind::wall_axis_length:
            target.erase("segment_ids");
            target.erase("segment_id");
            target.erase("second_segment_id");
            target.erase("vertex_id");
            break;
    }
    properties["target"] = std::move(target);
    if (!preserve_text_position)
        properties["text_position"] = Json::array({dimension.text_position.x,
                                                 dimension.text_position.y});
    properties["placement_origin"] =
        std::string(boundary_dimension_placement_name(dimension.placement));
    if (dimension.placement == BoundaryDimensionPlacement::automatic) {
        properties["automatic_placement_version"] =
            *dimension.automatic_placement_version;
    } else {
        properties.erase("automatic_placement_version");
    }
    return result;
}

IdentifiedBoundary resolve_dimension_geometry_owner(const Entity& entity) {
    if (entity.type == "measurement_linework") {
        if (!entity.properties.contains("model")) invalid("dimension measured source is missing its model");
        const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
        if (!decoded.supported()) invalid("dimension measured source is unsupported: " + decoded.diagnostic);
        const auto replay = replay_measurement_linework(*decoded.model);
        if (replay.stroke_id != entity.id) invalid("dimension measured source identity differs from its model");
        IdentifiedBoundary result{entity.id, entity.type, {}};
        result.segments.reserve(replay.edges.size());
        for (const auto& edge : replay.edges)
            result.segments.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id, edge.segment});
        return result;
    }
    if (!can_recognize_boundary_entity_type(entity.type)) {
        invalid("dimension source entity type is not a supported boundary");
    }
    const auto source_version = inspect_boundary_entity_version(entity);
    if (source_version.format != BoundaryEntityFormat::identified_v1) {
        invalid("dimension source boundary must use identified model version one");
    }
    return decode_identified_boundary_entity(entity);
}

Segment resolve_dimension_wall_axis_owner(const Entity& entity) {
    if (entity.type != "wall" || !valid_identifier(entity.id))
        invalid("dimension wall source must be a supported physical wall with a valid id");
    if (!entity.properties.is_object() || !entity.extensions.is_object())
        invalid("dimension wall source properties and extensions must be JSON objects");
    std::size_t count = 0;
    validate_json_tree(entity.properties, 0, count);
    validate_json_tree(entity.extensions, 0, count);
    Wall wall;
    std::string error;
    if (!read_document_wall(entity, {}, wall, error))
        invalid("dimension wall source is malformed: " + error);
    validate_wall_semantics(wall);
    try {
        validate_wall_curve_input(entity);
    } catch (const Json::exception& exception) {
        invalid("dimension wall source provenance is malformed: " + std::string(exception.what()));
    }
    return wall.baseline;
}

static BoundaryDimensionResolution resolve_identified_dimension(
    const BoundaryDimension& dimension, const IdentifiedBoundary& source,
    std::optional<double> net_clear_area = std::nullopt) {
    if (dimension.kind == BoundaryDimensionKind::segment_length) {
        if (!dimension.segment_chain_ids.empty()) {
            const IdentifiedSegment* first = nullptr;
            const IdentifiedSegment* previous = nullptr;
            double length = 0;
            for (const auto& id : dimension.segment_chain_ids) {
                const auto edge = find_segment(source, id);
                if (!edge) invalid("dimension source boundary is missing a chain segment id");
                if (previous && (previous->end_vertex_id != edge->start_vertex_id ||
                    std::hypot(previous->segment.end.x - edge->segment.start.x,
                               previous->segment.end.y - edge->segment.start.y) > default_geometry_tolerance_metres))
                    invalid("dimension segment chain must have contiguous geometry and stable vertex IDs");
                if (!first) first = edge;
                length += segment_length(edge->segment);
                previous = edge;
            }
            if (!std::isfinite(length)) invalid("dimension segment chain length must be finite");
            return BoundaryDimensionResolution{{first->segment.start, previous->segment.end, 0.0},
                                               length, BoundaryDimensionKind::segment_length, 0.0, 0.0};
        }
        const auto found = find_segment(source, dimension.segment_id);
        if (found == nullptr) {
            invalid("dimension source boundary is missing the target segment id");
        }
        return BoundaryDimensionResolution{found->segment, segment_length(found->segment),
                                           BoundaryDimensionKind::segment_length, 0.0, 0.0};
    }
    if (dimension.kind == BoundaryDimensionKind::angle) {
        const auto first = find_segment(source, dimension.segment_id);
        const auto second = find_segment(source, dimension.secondary_segment_id);
        if (first == nullptr || second == nullptr) {
            invalid("angle dimension source boundary is missing a target segment");
        }
        const auto first_tangent = tangent_from_vertex(*first, dimension.vertex_id);
        const auto second_tangent = tangent_from_vertex(*second, dimension.vertex_id);
        const auto dot_product = std::clamp(first_tangent.x * second_tangent.x +
                                                 first_tangent.y * second_tangent.y,
                                             -1.0, 1.0);
        const auto angle = std::acos(dot_product);
        if (!std::isfinite(angle) || angle <= default_geometry_tolerance_metres) {
            invalid("angle dimension target tangents do not form a measurable angle");
        }
        return BoundaryDimensionResolution{first->segment, segment_length(first->segment),
                                           BoundaryDimensionKind::angle, angle, 0.0};
    }

    Boundary boundary;
    boundary.reserve(source.segments.size());
    for (const auto& segment : source.segments) boundary.push_back(segment.segment);
    if (!closed_boundary(boundary)) {
        invalid("area dimension source boundary must be closed");
    }
    const auto area = net_clear_area.value_or(std::abs(signed_area(boundary)));
    if (!std::isfinite(area) || area <= default_geometry_tolerance_metres *
                                      default_geometry_tolerance_metres) {
        invalid("area dimension source boundary has no measurable area");
    }
    return BoundaryDimensionResolution{{}, 0.0, BoundaryDimensionKind::area, 0.0, area};
}

static BoundaryDimensionResolution resolve_retained_dimension_target(
    const BoundaryDimension& dimension, const Entity& boundary_entity) {
    validate_model(dimension);
    if (boundary_entity.id != dimension.boundary_id)
        invalid("dimension source boundary id does not match target entity id");
    if (dimension.kind == BoundaryDimensionKind::wall_axis_length) {
        const auto axis = resolve_dimension_wall_axis_owner(boundary_entity);
        return {axis, segment_length(axis), BoundaryDimensionKind::wall_axis_length, 0.0, 0.0};
    }
    if (dimension.kind == BoundaryDimensionKind::area && boundary_entity.type == "measurement_linework")
        invalid("area dimensions cannot target measured strokes");
    return resolve_identified_dimension(dimension, resolve_dimension_geometry_owner(boundary_entity));
}

void validate_boundary_dimension_target(const BoundaryDimension& dimension,
                                       const Entity& boundary_entity) {
    // Any analytical work here is discarded. This admits stable retained
    // targets, including stale physical rooms, without publishing their values
    // or changing the source-bound marker to bypass currentness checks.
    (void)resolve_retained_dimension_target(dimension, boundary_entity);
}

BoundaryDimensionResolution resolve_boundary_dimension(const BoundaryDimension& dimension,
                                                        const Entity& boundary_entity) {
    validate_model(dimension);
    // This Entity-only resolver has no live wall snapshot. It cannot certify
    // clear-room geometry or its holes after a physical source edit.
    if (is_physical_wall_room(boundary_entity))
        invalid("Source-bound room dimensions require current physical-room geometry; use authoritative map or snapshot resolution");
    return resolve_retained_dimension_target(dimension, boundary_entity);
}

BoundaryDimensionResolution resolve_boundary_dimension(const BoundaryDimension& dimension,
    const std::map<std::string, Entity, std::less<>>& entities) {
    validate_model(dimension);
    const auto owner = entities.find(dimension.boundary_id);
    if (owner == entities.end()) invalid("dimension source owner is missing from the authoritative map");
    if (owner->second.id != dimension.boundary_id)
        invalid("dimension source owner identity differs from its authoritative map key");
    if (!is_physical_wall_room(owner->second)) return resolve_boundary_dimension(dimension, owner->second);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    const auto current = resolve_physical_room_dimension_source(owner->second, entities);
    return resolve_identified_dimension(dimension, current.boundary, current.area_square_metres);
#else
    invalid("Physical room dimension resolution is unavailable in this runtime");
#endif
}

BoundaryDimensionResolution resolve_boundary_dimension(const BoundaryDimension& dimension,
                                                        const DocumentSnapshot& snapshot) {
    return resolve_boundary_dimension(dimension, snapshot.entities());
}

BoundaryDimensionResolution resolve_current_boundary_dimension(const BoundaryDimension& dimension,
                                                                const DocumentSnapshot& snapshot) {
    validate_model(dimension);
    const auto& entities = snapshot.entities();
    const auto owner = entities.find(dimension.boundary_id);
    if (owner == entities.end()) invalid("dimension source owner is missing from the authoritative map");
    if (owner->second.id != dimension.boundary_id)
        invalid("dimension source owner identity differs from its authoritative map key");
    if (!is_physical_wall_room(owner->second)) return resolve_boundary_dimension(dimension, owner->second);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    const auto current = resolve_current_physical_room_dimension_source(owner->second, snapshot);
    return resolve_identified_dimension(dimension, current.boundary, current.area_square_metres);
#else
    invalid("Physical room dimension resolution is unavailable in this runtime");
#endif
}

BoundaryDimensionResolution BoundaryDimension::resolve(const Entity& boundary_entity) const {
    return resolve_boundary_dimension(*this, boundary_entity);
}

BoundaryDimensionResolution BoundaryDimension::resolve(
    const std::map<std::string, Entity, std::less<>>& entities) const {
    return resolve_boundary_dimension(*this, entities);
}

BoundaryDimensionResolution BoundaryDimension::resolve(const DocumentSnapshot& snapshot) const {
    return resolve_boundary_dimension(*this, snapshot);
}

}  // namespace sketch
