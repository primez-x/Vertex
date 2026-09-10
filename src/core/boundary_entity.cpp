#include "sketch/boundary_entity.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) { throw std::invalid_argument(std::move(message)); }

bool same_point(Vec2 a, Vec2 b) noexcept { return a.x == b.x && a.y == b.y; }
bool same_segment(const Segment& a, const Segment& b) noexcept {
    return same_point(a.start, b.start) && same_point(a.end, b.end) &&
           a.sweep_radians == b.sweep_radians;
}

void identifier(std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Boundary identifier must contain 1..128 supported ASCII characters");
}

void envelope(const Entity& entity) {
    identifier(entity.id);
    if (!can_recognize_boundary_entity_type(entity.type)) invalid("Not a boundary entity");
    if (!entity.properties.is_object() || !entity.extensions.is_object())
        invalid("Boundary properties and extensions must be objects");
}

const Json& field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) invalid(std::string("Missing boundary field: ") + key);
    return object.at(key);
}

double number(const Json& value) {
    if (!value.is_number()) invalid("Boundary coordinate or sweep must be numeric");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid("Boundary coordinate or sweep must be finite");
    return result;
}

Vec2 point(const Json& value) {
    if (!value.is_array() || value.size() != 2) invalid("Boundary point must have exactly two coordinates");
    return {number(value[0]), number(value[1])};
}

Segment segment(const Json& value) {
    return {point(field(value, "start")), point(field(value, "end")),
            number(field(value, "sweep_radians"))};
}

std::string id_field(const Json& value, const char* key) {
    const auto& text = field(value, key);
    if (!text.is_string()) invalid(std::string("Boundary identity must be a string: ") + key);
    auto result = text.get<std::string>();
    identifier(result);
    return result;
}

const Json& segment_array(const Json& value) {
    if (!value.is_array() || value.empty()) invalid("Boundary segments must be a nonempty array");
    return value;
}

void validate(const IdentifiedBoundary& boundary) {
    identifier(boundary.id);
    if (!can_recognize_boundary_entity_type(boundary.type)) invalid("Not a boundary entity");
    if (boundary.segments.empty()) invalid("Boundary has no segments");
    std::set<std::string, std::less<>> edges;
    std::set<std::string, std::less<>> starts;
    Boundary geometry;
    geometry.reserve(boundary.segments.size());
    for (std::size_t i = 0; i < boundary.segments.size(); ++i) {
        const auto& edge = boundary.segments[i];
        const auto& next = boundary.segments[(i + 1) % boundary.segments.size()];
        identifier(edge.segment_id);
        identifier(edge.start_vertex_id);
        identifier(edge.end_vertex_id);
        if (!edges.insert(edge.segment_id).second) invalid("Duplicate boundary segment ID");
        if (!starts.insert(edge.start_vertex_id).second) invalid("Boundary cycle revisits a vertex identity");
        if (edge.end_vertex_id != next.start_vertex_id ||
            !same_point(edge.segment.end, next.segment.start))
            invalid("Boundary endpoint identities and coordinates must join exactly");
        geometry.push_back(edge.segment);
    }
    const auto diagnostics = validate_boundary(geometry);
    if (!diagnostics.empty()) invalid("Boundary geometry: " + diagnostics.front().message);
}

bool has_owned_semantics(const Json& value) {
    if (!value.is_object()) return false;
    for (const auto* key : {"receipt", "receipts", "dimensions", "dimension_refs", "constraints",
                            "constraint_refs", "source_refs", "direction", "boundary_authoring"})
        if (value.contains(key)) return true;
    return false;
}

void write_geometry(Json& target, const IdentifiedSegment& edge) {
    const Json values{{"start", {edge.segment.start.x, edge.segment.start.y}},
                      {"end", {edge.segment.end.x, edge.segment.end.y}},
                      {"sweep_radians", edge.segment.sweep_radians},
                      {"segment_id", edge.segment_id},
                      {"start_vertex_id", edge.start_vertex_id},
                      {"end_vertex_id", edge.end_vertex_id}};
    for (const auto& [key, value] : values.items()) {
        // Retain the original numeric JSON representation when the semantic
        // value is unchanged, including integer coordinates in legacy files.
        if (!target.contains(key) || target.at(key) != value) target[key] = value;
    }
}
} // namespace

bool IdentifiedSegment::operator==(const IdentifiedSegment& other) const noexcept {
    return segment_id == other.segment_id && start_vertex_id == other.start_vertex_id &&
           end_vertex_id == other.end_vertex_id && same_segment(segment, other.segment);
}

bool can_recognize_boundary_entity_type(std::string_view type) noexcept {
    return type == "boundary" || type == "measurement_boundary" || type == "room_boundary";
}

BoundaryEntityVersion inspect_boundary_entity_version(const Entity& entity) {
    envelope(entity);
    if (!entity.properties.contains("boundary_model_version")) {
        // Child-field names alone do not reinterpret opaque legacy metadata.
        // Upgrade rejects collisions; transition validation rejects stripping
        // the version from an already identified entity.
        return {BoundaryEntityFormat::anonymous_legacy, std::nullopt, {}};
    }
    const auto& value = entity.properties.at("boundary_model_version");
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() <= 0))
        invalid("Boundary model version must be a positive integer");
    const auto version = value.get<std::uint64_t>();
    if (version == 0) invalid("Boundary model version must be a positive integer");
    if (version == 1) return {BoundaryEntityFormat::identified_v1, version, {}};
    return {BoundaryEntityFormat::unsupported_version, version,
            "unsupported boundary model version " + std::to_string(version)};
}

IdentifiedBoundary decode_identified_boundary_entity(const Entity& entity) {
    if (inspect_boundary_entity_version(entity).format != BoundaryEntityFormat::identified_v1)
        invalid("A supported identified boundary model is required");
    IdentifiedBoundary result{entity.id, entity.type, {}};
    for (const auto& value : segment_array(field(entity.properties, "segments")))
        result.segments.push_back({id_field(value, "segment_id"), id_field(value, "start_vertex_id"),
                                   id_field(value, "end_vertex_id"), segment(value)});
    validate(result);
    return result;
}

Boundary boundary_geometry(const IdentifiedBoundary& boundary) {
    validate(boundary);
    Boundary result;
    result.reserve(boundary.segments.size());
    for (const auto& edge : boundary.segments) result.push_back(edge.segment);
    return result;
}

Entity encode_identified_boundary_entity(const IdentifiedBoundary& boundary, const Entity* original) {
    validate(boundary);
    Entity result{boundary.id, boundary.type, Json::object(), false, Json::object()};
    std::map<std::string, Json, std::less<>> previous;
    std::map<std::string, IdentifiedSegment, std::less<>> previous_edges;
    if (original) {
        const auto decoded = decode_identified_boundary_entity(*original);
        if (decoded.id != boundary.id || decoded.type != boundary.type)
            invalid("Boundary update cannot change entity identity or type");
        result = *original;
        for (const auto& edge : decoded.segments) previous_edges.emplace(edge.segment_id, edge);
        for (const auto& value : original->properties.at("segments"))
            previous.emplace(value.at("segment_id").get<std::string>(), value);
        if (decoded != boundary &&
            (has_owned_semantics(original->extensions) || has_owned_semantics(original->properties)))
            invalid("Boundary edit requires handling existing receipt or dependent-reference semantics");
    }
    Json encoded = Json::array();
    for (const auto& edge : boundary.segments) {
        auto value = Json::object();
        if (const auto found = previous.find(edge.segment_id); found != previous.end()) {
            value = found->second;
            if (previous_edges.at(edge.segment_id) != edge && has_owned_semantics(value))
                invalid("Boundary segment edit requires handling directional receipt or reference semantics");
            previous.erase(found);
        }
        write_geometry(value, edge);
        encoded.push_back(std::move(value));
    }
    for (const auto& [id, removed] : previous) {
        (void)id;
        if (has_owned_semantics(removed)) invalid("Cannot retire a segment with unhandled receipt or reference semantics");
    }
    result.properties["boundary_model_version"] = 1;
    result.properties["segments"] = std::move(encoded);
    return result;
}

Entity upgrade_legacy_boundary_entity(const Entity& original, const LegacyBoundaryIdentityOptions& options) {
    if (inspect_boundary_entity_version(original).format != BoundaryEntityFormat::anonymous_legacy)
        invalid("Identity upgrade requires an anonymous legacy boundary");
    const bool has_segments = original.properties.contains("segments");
    const bool has_boundary = original.properties.contains("boundary");
    if (has_segments == has_boundary) invalid("Legacy boundary requires exactly one unambiguous geometry array");
    if (has_owned_semantics(original.properties) || has_owned_semantics(original.extensions))
        invalid("Legacy upgrade requires handling existing receipt or reference semantics");
    const auto& values = segment_array(original.properties.at(has_segments ? "segments" : "boundary"));
    const auto count = values.size();
    if ((!options.segment_ids.empty() && options.segment_ids.size() != count) ||
        (!options.vertex_ids.empty() && options.vertex_ids.size() != count))
        invalid("Supplied boundary identity counts must match topology");
    auto edge_ids = options.segment_ids;
    auto vertex_ids = options.vertex_ids;
    if (edge_ids.empty()) for (std::size_t i = 0; i < count; ++i) edge_ids.push_back(make_stable_id());
    if (vertex_ids.empty()) for (std::size_t i = 0; i < count; ++i) vertex_ids.push_back(make_stable_id());
    IdentifiedBoundary model{original.id, original.type, {}};
    auto identified_values = values;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& value = values[i];
        if (value.contains("segment_id") || value.contains("start_vertex_id") || value.contains("end_vertex_id") ||
            has_owned_semantics(value)) invalid("Legacy segment contains unqualified identity, receipt or reference data");
        model.segments.push_back({edge_ids[i], vertex_ids[i], vertex_ids[(i + 1) % count], segment(value)});
        // Add only identity fields. Original geometry and unknown JSON values
        // remain byte-equivalent at their value level.
        identified_values[i]["segment_id"] = edge_ids[i];
        identified_values[i]["start_vertex_id"] = vertex_ids[i];
        identified_values[i]["end_vertex_id"] = vertex_ids[(i + 1) % count];
    }
    validate(model);
    auto result = original;
    result.properties.erase("boundary");
    result.properties["segments"] = std::move(identified_values);
    result.properties["boundary_model_version"] = 1;
    return result;
}

IdentifiedBoundary reverse_identified_boundary(const IdentifiedBoundary& boundary) {
    validate(boundary);
    auto result = boundary;
    std::reverse(result.segments.begin(), result.segments.end());
    for (auto& edge : result.segments) {
        std::swap(edge.start_vertex_id, edge.end_vertex_id);
        std::swap(edge.segment.start, edge.segment.end);
        edge.segment.sweep_radians = -edge.segment.sweep_radians;
    }
    validate(result);
    return result;
}

Entity reverse_identified_boundary_entity(const Entity& original) {
    return encode_identified_boundary_entity(
        reverse_identified_boundary(decode_identified_boundary_entity(original)), &original);
}
} // namespace sketch
