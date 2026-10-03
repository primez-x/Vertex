#include "sketch/measurement_linework.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument(std::move(message));
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return left.x == right.x && left.y == right.y;
}

void require_point(Vec2 point, std::string_view label) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        invalid(std::string(label) + " must be finite");
    }
}

void require_identifier(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '-' || character == '_' || character == '.' || character == ':';
        })) {
        invalid(std::string(label) + " must contain 1..128 supported ASCII characters");
    }
}

void require_object(const Json& value, std::string_view label) {
    if (!value.is_object()) invalid(std::string(label) + " must be an object");
}

void require_keys(const Json& value, std::initializer_list<std::string_view> keys,
                  std::string_view label) {
    require_object(value, label);
    for (const auto key : keys) {
        if (!value.contains(std::string(key))) {
            invalid(std::string(label) + " is missing " + std::string(key));
        }
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::find(keys.begin(), keys.end(), field.key()) == keys.end()) {
            invalid(std::string(label) + " has unknown field " + field.key());
        }
    }
}

std::uint64_t read_positive_version(const Json& value, std::string_view label) {
    if (value.is_number_unsigned()) {
        const auto version = value.get<std::uint64_t>();
        if (version > 0) return version;
    } else if (value.is_number_integer()) {
        const auto version = value.get<std::int64_t>();
        if (version > 0) return static_cast<std::uint64_t>(version);
    }
    invalid(std::string(label) + " must be a positive integer");
}

std::string read_identifier(const Json& value, std::string_view label) {
    if (!value.is_string()) invalid(std::string(label) + " must be a string");
    auto result = value.get<std::string>();
    require_identifier(result, label);
    return result;
}

Vec2 read_point(const Json& value, std::string_view label) {
    if (!value.is_array() || value.size() != 2 ||
        !value[0].is_number() || !value[1].is_number()) {
        invalid(std::string(label) + " must contain exactly two numeric coordinates");
    }
    const Vec2 result{value[0].get<double>(), value[1].get<double>()};
    require_point(result, label);
    return result;
}

void record_vertex(std::map<std::string, Vec2, std::less<>>& vertices,
                   const std::string& id, Vec2 point) {
    const auto [existing, inserted] = vertices.emplace(id, point);
    if (!inserted && !same_point(existing->second, point)) {
        invalid("measurement linework vertex identity has conflicting coordinates");
    }
}

}  // namespace

MeasurementLineworkReplay replay_measurement_linework(const MeasurementLinework& model,
                                                     double tolerance_metres) {
    if (!std::isfinite(tolerance_metres) || !(tolerance_metres > 0)) {
        invalid("measurement linework tolerance must be finite and positive");
    }
    if (model.schema_version != measurement_linework_schema_version) {
        invalid("unsupported measurement linework schema version");
    }
    if (model.replay_version != measurement_linework_replay_version) {
        invalid("unsupported measurement linework replay version");
    }
    require_identifier(model.stroke_id, "measurement linework stroke_id");
    require_point(model.anchor, "measurement linework anchor");
    require_object(model.extensions, "measurement linework extensions");
    if (model.edges.empty()) invalid("measurement linework has no segments");

    MeasurementLineworkReplay result;
    result.replay_version = model.replay_version;
    result.stroke_id = model.stroke_id;
    result.anchor = model.anchor;
    result.closed = model.closed;
    result.edges.reserve(model.edges.size());
    result.receipts.reserve(model.edges.size());
    std::set<std::string, std::less<>> segment_ids;
    std::map<std::string, Vec2, std::less<>> vertices;
    Vec2 expected_start = model.anchor;
    for (std::size_t index = 0; index < model.edges.size(); ++index) {
        const auto& edge = model.edges[index];
        require_identifier(edge.segment_id, "measurement linework segment_id");
        require_identifier(edge.start_vertex_id, "measurement linework start_vertex_id");
        require_identifier(edge.end_vertex_id, "measurement linework end_vertex_id");
        if (edge.segment_id == model.stroke_id || edge.start_vertex_id == model.stroke_id ||
            edge.end_vertex_id == model.stroke_id)
            invalid("measurement linework child identity collides with its stroke identity");
        if (!segment_ids.insert(edge.segment_id).second) {
            invalid("measurement linework contains duplicate segment identity");
        }
        if (edge.receipt.segment_id != edge.segment_id) {
            invalid("measurement linework receipt segment identity mismatch");
        }
        if (index > 0 && edge.start_vertex_id != model.edges[index - 1].end_vertex_id) {
            invalid("measurement linework vertex identities do not join");
        }
        const auto kind = edge.receipt.kind;
        if (kind == BoundaryConstructionKind::line_closure &&
            (!model.closed || index + 1 != model.edges.size())) {
            invalid("measurement linework closure receipt must be the final edge of a closed stroke");
        }
        const auto previous = kind == BoundaryConstructionKind::line_relative_turn && index > 0
            ? std::optional<Segment>{result.edges.back().segment} : std::nullopt;
        const auto closure_anchor = kind == BoundaryConstructionKind::line_closure
            ? std::optional<Vec2>{model.anchor} : std::nullopt;
        const auto rebuilt = replay_construction_receipt(
            edge.receipt, {expected_start, previous, closure_anchor, tolerance_metres});
        if (!(rebuilt.receipt == edge.receipt)) {
            invalid("measurement linework receipt is not canonically normalized");
        }
        record_vertex(vertices, edge.start_vertex_id, rebuilt.segment.start);
        record_vertex(vertices, edge.end_vertex_id, rebuilt.segment.end);
        result.edges.push_back({edge.segment_id, edge.start_vertex_id, edge.end_vertex_id,
                                rebuilt.segment});
        result.receipts.push_back(rebuilt.receipt);
        expected_start = rebuilt.segment.end;
    }
    for (const auto& segment_id : segment_ids) {
        if (vertices.contains(segment_id)) {
            invalid("measurement linework segment and vertex identities collide");
        }
    }
    const bool closes_identity = model.edges.back().end_vertex_id == model.edges.front().start_vertex_id;
    if (model.closed) {
        if (!closes_identity || !same_point(expected_start, model.anchor)) {
            invalid("measurement linework is not exactly closed at its anchor identity");
        }
    } else if (closes_identity) {
        invalid("open measurement linework cannot finish with its anchor identity");
    }
    return result;
}

MeasurementLineworkVersion inspect_measurement_linework_model(const Json& model) {
    require_object(model, "measurement linework model");
    if (!model.contains("version")) invalid("measurement linework model is missing version");
    const auto version = read_positive_version(model.at("version"), "measurement linework version");
    if (version != measurement_linework_schema_version) {
        return {MeasurementLineworkFormat::unsupported_version, version, std::nullopt,
                "unsupported measurement linework schema version"};
    }
    if (!model.contains("replay_version")) {
        invalid("measurement linework model is missing replay_version");
    }
    const auto replay_version = read_positive_version(model.at("replay_version"),
                                                      "measurement linework replay_version");
    if (replay_version != measurement_linework_replay_version) {
        return {MeasurementLineworkFormat::unsupported_replay_version, version, replay_version,
                "unsupported measurement linework replay version"};
    }
    return {MeasurementLineworkFormat::supported_v1, version, replay_version, {}};
}

MeasurementLineworkDecodeResult decode_measurement_linework_model(const Json& encoded) {
    const auto inspected = inspect_measurement_linework_model(encoded);
    if (inspected.format != MeasurementLineworkFormat::supported_v1) {
        return {std::nullopt, encoded, inspected.version, inspected.replay_version,
                inspected.diagnostic};
    }
    require_keys(encoded, {"version", "replay_version", "stroke_id", "anchor", "closed",
                           "segments", "extensions"}, "measurement linework model");
    MeasurementLinework model;
    model.stroke_id = read_identifier(encoded.at("stroke_id"), "measurement linework stroke_id");
    model.anchor = read_point(encoded.at("anchor"), "measurement linework anchor");
    if (!encoded.at("closed").is_boolean()) invalid("measurement linework closed must be boolean");
    model.closed = encoded.at("closed").get<bool>();
    require_object(encoded.at("extensions"), "measurement linework extensions");
    model.extensions = encoded.at("extensions");
    const auto& segments = encoded.at("segments");
    if (!segments.is_array()) invalid("measurement linework segments must be an array");
    model.edges.reserve(segments.size());
    for (const auto& segment : segments) {
        require_keys(segment, {"segment_id", "start_vertex_id", "end_vertex_id", "receipt"},
                     "measurement linework segment");
        model.edges.push_back({
            read_identifier(segment.at("segment_id"), "measurement linework segment_id"),
            read_identifier(segment.at("start_vertex_id"), "measurement linework start_vertex_id"),
            read_identifier(segment.at("end_vertex_id"), "measurement linework end_vertex_id"),
            decode_construction_receipt(segment.at("receipt"))});
    }
    (void)replay_measurement_linework(model);
    return {std::move(model), std::nullopt, inspected.version, inspected.replay_version, {}};
}

Json encode_measurement_linework_model(const MeasurementLinework& model) {
    const auto replay = replay_measurement_linework(model);
    Json segments = Json::array();
    for (std::size_t index = 0; index < replay.edges.size(); ++index) {
        const auto& edge = replay.edges[index];
        segments.push_back(Json{{"segment_id", edge.segment_id},
                                {"start_vertex_id", edge.start_vertex_id},
                                {"end_vertex_id", edge.end_vertex_id},
                                {"receipt", encode_construction_receipt(replay.receipts[index])}});
    }
    return Json{{"version", model.schema_version}, {"replay_version", model.replay_version},
                {"stroke_id", model.stroke_id}, {"anchor", Json::array({model.anchor.x, model.anchor.y})},
                {"closed", model.closed}, {"segments", std::move(segments)},
                {"extensions", model.extensions}};
}

}  // namespace sketch
