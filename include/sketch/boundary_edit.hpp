#pragma once

#include "sketch/geometry.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>
#include <set>
#include <string>

namespace sketch {

enum class BoundaryGeometryEditKind { move_vertex, resize_segment };
enum class BoundaryFixedEndpoint { start, end };

// Replayable semantic intent for a coordinate edit that retains boundary,
// segment and vertex identities. A receipt-backed boundary archives its exact
// construction receipt and appends these intents as deterministic derivation
// evidence; historical input is never rewritten to impersonate the new shape.
struct BoundaryGeometryEdit {
    std::string boundary_id;
    BoundaryGeometryEditKind kind{BoundaryGeometryEditKind::move_vertex};
    std::string target_id;
    Vec2 target_position{};
    double target_length_metres{};
    BoundaryFixedEndpoint fixed_endpoint{BoundaryFixedEndpoint::start};
    bool move_connected{};

    bool operator==(const BoundaryGeometryEdit& other) const {
        return boundary_id == other.boundary_id && kind == other.kind &&
            target_id == other.target_id &&
            target_position.x == other.target_position.x &&
            target_position.y == other.target_position.y &&
            target_length_metres == other.target_length_metres &&
            fixed_endpoint == other.fixed_endpoint && move_connected == other.move_connected;
    }
};

inline void validate_boundary_geometry_edit(const BoundaryGeometryEdit& edit) {
    const auto valid_id = [](const std::string& value) {
        if (value.empty() || value.size() > 128) return false;
        for (const auto c : value) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':'))
                return false;
        }
        return true;
    };
    if (!valid_id(edit.boundary_id) || !valid_id(edit.target_id))
        throw std::invalid_argument("Boundary geometry edit identifiers are invalid");
    if (edit.kind == BoundaryGeometryEditKind::move_vertex) {
        if (!std::isfinite(edit.target_position.x) || !std::isfinite(edit.target_position.y) ||
            edit.target_length_metres != 0.0 || edit.move_connected ||
            edit.fixed_endpoint != BoundaryFixedEndpoint::start) {
            throw std::invalid_argument("Boundary vertex edit contains incompatible fields");
        }
    } else if (edit.kind == BoundaryGeometryEditKind::resize_segment) {
        if (!(edit.target_length_metres > 0.0) ||
            !std::isfinite(edit.target_length_metres) || edit.target_position.x != 0.0 ||
            edit.target_position.y != 0.0) {
            throw std::invalid_argument("Boundary segment edit contains incompatible fields");
        }
    } else {
        throw std::invalid_argument("Boundary geometry edit kind is unsupported");
    }
}

inline nlohmann::json encode_boundary_geometry_edit(const BoundaryGeometryEdit& edit) {
    validate_boundary_geometry_edit(edit);
    if (edit.kind == BoundaryGeometryEditKind::move_vertex) {
        return {{"version", 1}, {"kind", "move_vertex"},
                {"boundary_id", edit.boundary_id}, {"vertex_id", edit.target_id},
                {"position", {edit.target_position.x, edit.target_position.y}}};
    }
    return {{"version", 1}, {"kind", "resize_segment"},
            {"boundary_id", edit.boundary_id}, {"segment_id", edit.target_id},
            {"target_length_metres", edit.target_length_metres},
            {"fixed_endpoint", edit.fixed_endpoint == BoundaryFixedEndpoint::start
                                   ? "start" : "end"},
            {"move_connected", edit.move_connected}};
}

inline BoundaryGeometryEdit decode_boundary_geometry_edit(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("kind") || !value.at("kind").is_string() ||
        !value.contains("boundary_id") || !value.at("boundary_id").is_string()) {
        throw std::invalid_argument("Boundary geometry edit envelope is invalid");
    }
    const auto kind = value.at("kind").get<std::string>();
    BoundaryGeometryEdit result;
    result.boundary_id = value.at("boundary_id").get<std::string>();
    if (kind == "move_vertex") {
        const std::set<std::string> expected{
            "version", "kind", "boundary_id", "vertex_id", "position"};
        std::set<std::string> actual;
        for (const auto& [key, ignored] : value.items()) { (void)ignored; actual.insert(key); }
        if (actual != expected || !value.at("vertex_id").is_string() ||
            !value.at("position").is_array() || value.at("position").size() != 2 ||
            !value.at("position")[0].is_number() || !value.at("position")[1].is_number())
            throw std::invalid_argument("Boundary vertex edit fields are invalid");
        result.kind = BoundaryGeometryEditKind::move_vertex;
        result.target_id = value.at("vertex_id").get<std::string>();
        result.target_position = {value.at("position")[0].get<double>(),
                                  value.at("position")[1].get<double>()};
    } else if (kind == "resize_segment") {
        const std::set<std::string> expected{"version", "kind", "boundary_id", "segment_id",
            "target_length_metres", "fixed_endpoint", "move_connected"};
        std::set<std::string> actual;
        for (const auto& [key, ignored] : value.items()) { (void)ignored; actual.insert(key); }
        if (actual != expected || !value.at("segment_id").is_string() ||
            !value.at("target_length_metres").is_number() ||
            !value.at("fixed_endpoint").is_string() ||
            !value.at("move_connected").is_boolean())
            throw std::invalid_argument("Boundary segment edit fields are invalid");
        result.kind = BoundaryGeometryEditKind::resize_segment;
        result.target_id = value.at("segment_id").get<std::string>();
        result.target_length_metres = value.at("target_length_metres").get<double>();
        const auto fixed = value.at("fixed_endpoint").get<std::string>();
        if (fixed == "start") result.fixed_endpoint = BoundaryFixedEndpoint::start;
        else if (fixed == "end") result.fixed_endpoint = BoundaryFixedEndpoint::end;
        else throw std::invalid_argument("Boundary segment fixed endpoint is invalid");
        result.move_connected = value.at("move_connected").get<bool>();
    } else {
        throw std::invalid_argument("Boundary geometry edit kind is unsupported");
    }
    validate_boundary_geometry_edit(result);
    return result;
}

} // namespace sketch
