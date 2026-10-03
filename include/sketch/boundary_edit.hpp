#pragma once

#include "sketch/geometry.hpp"
#include "sketch/boundary_receipt.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

namespace sketch {

enum class BoundaryGeometryEditKind { move_vertex, resize_segment, insert_vertex, redefine_boundary, reconstruct_arc };
enum class BoundaryFixedEndpoint { start, end };

// Replayable semantic intent that retains the boundary identity. Coordinate
// edits retain existing children; changed-count redraws explicitly retire them.
// A receipt-backed boundary archives its exact
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
    double fraction{};
    std::string new_vertex_id;
    std::string new_segment_id;
    std::string new_dimension_id;
    nlohmann::json replacement_segments = nullptr;
    nlohmann::json replacement_authoring = nullptr;
    nlohmann::json replacement_properties = nlohmann::json::object();
    std::vector<std::string> replacement_dimension_ids;
    // Explicit changed-topology reference decisions. A nonempty mapping has
    // exactly segments/vertices objects of old -> new child IDs. Equal IDs
    // in different namespaces remain independent; omitted decisions do not
    // authorize silently dropping or reinterpreting references.
    nlohmann::json replacement_child_mapping = nlohmann::json::object();
    std::vector<std::string> replacement_removed_reference_ids;
    // Exact construction inputs for reconstruct_arc; the selected edge's
    // endpoints and segment identity remain fixed.
    std::optional<ConstructionReceipt> arc_construction;
    // Explicitly reviewed exterior-wall source replacement; never generic
    // caller-supplied source JSON. Only valid on a boundary redefinition.
    std::vector<std::string> replacement_wall_source_ids;
    // Explicit reviewed redraw intent. All child identities are fresh even
    // when the replacement has the same number of edges as the old outline.
    bool fresh_topology{};
    // Version-five redraw policy. Only explicit removal of affected automatic
    // angle dimensions is authorized; automatic edge lengths still regenerate.
    bool allow_automatic_angle_removal{};

    bool operator==(const BoundaryGeometryEdit& other) const {
        return boundary_id == other.boundary_id && kind == other.kind &&
            target_id == other.target_id &&
            target_position.x == other.target_position.x &&
            target_position.y == other.target_position.y &&
            target_length_metres == other.target_length_metres &&
            fixed_endpoint == other.fixed_endpoint && move_connected == other.move_connected &&
            fraction == other.fraction && new_vertex_id == other.new_vertex_id &&
            new_segment_id == other.new_segment_id && new_dimension_id == other.new_dimension_id &&
            replacement_segments == other.replacement_segments && replacement_authoring == other.replacement_authoring &&
            replacement_properties == other.replacement_properties && replacement_dimension_ids == other.replacement_dimension_ids &&
            replacement_child_mapping == other.replacement_child_mapping &&
            replacement_removed_reference_ids == other.replacement_removed_reference_ids &&
            arc_construction == other.arc_construction &&
            replacement_wall_source_ids == other.replacement_wall_source_ids &&
            fresh_topology == other.fresh_topology &&
            allow_automatic_angle_removal == other.allow_automatic_angle_removal;
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
    if (!edit.replacement_child_mapping.is_object())
        throw std::invalid_argument("Boundary reference mapping must be an object");
    if (edit.kind != BoundaryGeometryEditKind::redefine_boundary &&
        (!edit.replacement_segments.is_null() || !edit.replacement_authoring.is_null() ||
         !edit.replacement_properties.empty() || !edit.replacement_dimension_ids.empty() ||
         !edit.replacement_child_mapping.empty() || !edit.replacement_removed_reference_ids.empty() ||
         !edit.replacement_wall_source_ids.empty() || edit.fresh_topology || edit.allow_automatic_angle_removal))
        throw std::invalid_argument("Boundary coordinate edit contains redefinition fields");
    if (edit.kind != BoundaryGeometryEditKind::reconstruct_arc && edit.arc_construction)
        throw std::invalid_argument("Boundary geometry edit contains arc reconstruction fields");
    if (edit.kind != BoundaryGeometryEditKind::insert_vertex &&
        (edit.fraction != 0.0 || !edit.new_vertex_id.empty() || !edit.new_segment_id.empty() ||
         !edit.new_dimension_id.empty()))
        throw std::invalid_argument("Boundary coordinate edit contains insertion fields");
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
    } else if (edit.kind == BoundaryGeometryEditKind::insert_vertex) {
        if (!std::isfinite(edit.fraction) || edit.fraction <= 0.0 || edit.fraction >= 1.0 ||
            !valid_id(edit.new_vertex_id) || !valid_id(edit.new_segment_id) ||
            (!edit.new_dimension_id.empty() && !valid_id(edit.new_dimension_id)) ||
            edit.new_vertex_id == edit.new_segment_id || edit.new_vertex_id == edit.target_id ||
            edit.new_segment_id == edit.target_id || edit.target_position.x != 0.0 ||
            edit.target_position.y != 0.0 || edit.target_length_metres != 0.0 ||
            edit.move_connected || edit.fixed_endpoint != BoundaryFixedEndpoint::start)
            throw std::invalid_argument("Boundary insertion edit contains incompatible fields");
    } else if (edit.kind == BoundaryGeometryEditKind::redefine_boundary) {
        if (edit.target_id != edit.boundary_id || !edit.replacement_segments.is_array() ||
            edit.replacement_segments.empty() ||
            (!edit.replacement_authoring.is_null() && !edit.replacement_authoring.is_object()) ||
            !edit.replacement_properties.is_object() || edit.target_position.x != 0 ||
            edit.target_position.y != 0 || edit.target_length_metres != 0 || edit.move_connected ||
            edit.fixed_endpoint != BoundaryFixedEndpoint::start)
            throw std::invalid_argument("Boundary redefinition fields are invalid");
        for (const auto& [key, value] : edit.replacement_properties.items()) {
            if ((key != "classification" && key != "measurement_classification" &&
                 key != "appraisal_category" && key != "name") || !value.is_string() ||
                value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > 4096)
                throw std::invalid_argument("Boundary redefinition metadata fields are invalid");
        }
        std::set<std::string> dimensions;
        for (const auto& id : edit.replacement_dimension_ids)
            if (!valid_id(id) || !dimensions.insert(id).second)
                throw std::invalid_argument("Boundary redefinition dimension IDs are invalid");
        if (!edit.replacement_child_mapping.empty()) {
            const auto& mapping = edit.replacement_child_mapping;
            if (mapping.size() != 2 || !mapping.contains("segments") || !mapping.contains("vertices") ||
                !mapping.at("segments").is_object() || !mapping.at("vertices").is_object() ||
                (mapping.at("segments").empty() && mapping.at("vertices").empty()))
                throw std::invalid_argument("Boundary child mapping requires nonempty segments/vertices decisions");
            for (const auto* group : {"segments", "vertices"})
                for (const auto& [old_id, new_id] : mapping.at(group).items())
                    if (!valid_id(old_id) || !new_id.is_string() || !valid_id(new_id.get_ref<const std::string&>()))
                        throw std::invalid_argument("Boundary redefinition child mapping identifiers are invalid");
        }
        std::set<std::string> removed;
        for (const auto& id : edit.replacement_removed_reference_ids)
            if (!valid_id(id) || !removed.insert(id).second)
                throw std::invalid_argument("Boundary redefinition removed reference IDs are invalid");
        if (edit.allow_automatic_angle_removal && removed.empty())
            throw std::invalid_argument("Automatic angle removal requires explicit removed reference IDs");
        if (!edit.replacement_wall_source_ids.empty()) {
            if (edit.replacement_wall_source_ids.size() < 3 || edit.replacement_wall_source_ids.size() > 2048)
                throw std::invalid_argument("Boundary wall source replacement requires 3 to 2048 walls");
            std::set<std::string> walls;
            for (const auto& id : edit.replacement_wall_source_ids)
                if (!valid_id(id) || !walls.insert(id).second)
                    throw std::invalid_argument("Boundary wall source replacement IDs are invalid");
        }
        const auto reference_plan_bytes = edit.replacement_child_mapping.empty() &&
                edit.replacement_removed_reference_ids.empty()
            ? std::size_t{0}
            : edit.replacement_child_mapping.dump().size() + nlohmann::json(edit.replacement_removed_reference_ids).dump().size();
        if (edit.replacement_segments.dump().size() + edit.replacement_authoring.dump().size() +
            edit.replacement_properties.dump().size() + nlohmann::json(edit.replacement_dimension_ids).dump().size() +
            reference_plan_bytes + (edit.replacement_wall_source_ids.empty() ? std::size_t{0} :
                nlohmann::json(edit.replacement_wall_source_ids).dump().size()) +
            (edit.fresh_topology ? std::size_t{40} : std::size_t{0}) +
            (edit.allow_automatic_angle_removal ? std::size_t{80} : std::size_t{0}) >
            1024 * 1024 - 4096)
            throw std::invalid_argument("Boundary redefinition exceeds the persisted proof budget");
    } else if (edit.kind == BoundaryGeometryEditKind::reconstruct_arc) {
        if (!edit.arc_construction || edit.target_id != edit.arc_construction->segment_id ||
            edit.target_position.x != 0.0 || edit.target_position.y != 0.0 ||
            edit.target_length_metres != 0.0 || edit.move_connected ||
            edit.fixed_endpoint != BoundaryFixedEndpoint::start)
            throw std::invalid_argument("Boundary arc reconstruction fields are invalid");
        const auto kind = edit.arc_construction->kind;
        if (kind != BoundaryConstructionKind::arc_chord_angle &&
            kind != BoundaryConstructionKind::arc_chord_height &&
            kind != BoundaryConstructionKind::arc_chord_length)
            throw std::invalid_argument("Boundary arc reconstruction receipt must describe a chord arc");
        (void)encode_construction_receipt(*edit.arc_construction);
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
    if (edit.kind == BoundaryGeometryEditKind::insert_vertex) {
        return {{"version", 1}, {"kind", "insert_vertex"}, {"boundary_id", edit.boundary_id},
                {"segment_id", edit.target_id}, {"fraction", edit.fraction},
                {"new_vertex_id", edit.new_vertex_id}, {"new_segment_id", edit.new_segment_id},
                {"new_dimension_id", edit.new_dimension_id}};
    }
    if (edit.kind == BoundaryGeometryEditKind::redefine_boundary) {
        nlohmann::json result{{"version", 1}, {"kind", "redefine_boundary"}, {"boundary_id", edit.boundary_id},
            {"replacement_segments", edit.replacement_segments}, {"replacement_authoring", edit.replacement_authoring},
            {"replacement_properties", edit.replacement_properties}, {"replacement_dimension_ids", edit.replacement_dimension_ids}};
        if (!edit.replacement_child_mapping.empty() || !edit.replacement_removed_reference_ids.empty()) {
            result["version"] = 2;
            result["replacement_child_mapping"] = edit.replacement_child_mapping;
            result["replacement_removed_reference_ids"] = edit.replacement_removed_reference_ids;
        }
        if (!edit.replacement_wall_source_ids.empty()) {
            result["version"] = 3;
            result["replacement_wall_source_ids"] = edit.replacement_wall_source_ids;
            result["replacement_child_mapping"] = edit.replacement_child_mapping;
            result["replacement_removed_reference_ids"] = edit.replacement_removed_reference_ids;
        }
        if (edit.fresh_topology) {
            result["version"] = 4;
            result["fresh_topology"] = true;
            result["replacement_wall_source_ids"] = edit.replacement_wall_source_ids;
            result["replacement_child_mapping"] = edit.replacement_child_mapping;
            result["replacement_removed_reference_ids"] = edit.replacement_removed_reference_ids;
        }
        if (edit.allow_automatic_angle_removal) {
            result["version"] = 5;
            result["allow_automatic_angle_removal"] = true;
            result["fresh_topology"] = edit.fresh_topology;
            result["replacement_wall_source_ids"] = edit.replacement_wall_source_ids;
            result["replacement_child_mapping"] = edit.replacement_child_mapping;
            result["replacement_removed_reference_ids"] = edit.replacement_removed_reference_ids;
        }
        return result;
    }
    if (edit.kind == BoundaryGeometryEditKind::reconstruct_arc) {
        return {{"version", 1}, {"kind", "reconstruct_arc"},
                {"boundary_id", edit.boundary_id}, {"segment_id", edit.target_id},
                {"construction", encode_construction_receipt(*edit.arc_construction)}};
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
        !value.at("version").is_number_integer() || (value.at("version") != 1 && value.at("version") != 2 && value.at("version") != 3 && value.at("version") != 4 && value.at("version") != 5) ||
        !value.contains("kind") || !value.at("kind").is_string() ||
        !value.contains("boundary_id") || !value.at("boundary_id").is_string()) {
        throw std::invalid_argument("Boundary geometry edit envelope is invalid");
    }
    const auto kind = value.at("kind").get<std::string>();
    const bool automatic_angle_removal = value.at("version") == 5;
    const bool fresh_topology = value.at("version") == 4 || automatic_angle_removal;
    const bool wall_source_replacement = value.at("version") == 3 || fresh_topology;
    const bool reference_plan = value.at("version") == 2 || wall_source_replacement;
    if (reference_plan && kind != "redefine_boundary")
        throw std::invalid_argument("Boundary redefinition versions two through five require redefinition intent");
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
    } else if (kind == "insert_vertex") {
        const std::set<std::string> expected{"version", "kind", "boundary_id", "segment_id",
            "fraction", "new_vertex_id", "new_segment_id", "new_dimension_id"};
        std::set<std::string> actual;
        for (const auto& [key, ignored] : value.items()) { (void)ignored; actual.insert(key); }
        if (actual != expected || !value.at("segment_id").is_string() ||
            !value.at("fraction").is_number() || !value.at("new_vertex_id").is_string() ||
            !value.at("new_segment_id").is_string() || !value.at("new_dimension_id").is_string())
            throw std::invalid_argument("Boundary insertion edit fields are invalid");
        result.kind = BoundaryGeometryEditKind::insert_vertex;
        result.target_id = value.at("segment_id").get<std::string>();
        result.fraction = value.at("fraction").get<double>();
        result.new_vertex_id = value.at("new_vertex_id").get<std::string>();
        result.new_segment_id = value.at("new_segment_id").get<std::string>();
        result.new_dimension_id = value.at("new_dimension_id").get<std::string>();
    } else if (kind == "redefine_boundary") {
        std::set<std::string> expected{"version", "kind", "boundary_id", "replacement_segments",
            "replacement_authoring", "replacement_properties", "replacement_dimension_ids"};
        if (reference_plan) {
            expected.insert("replacement_child_mapping");
            expected.insert("replacement_removed_reference_ids");
        }
        if (wall_source_replacement) expected.insert("replacement_wall_source_ids");
        if (fresh_topology) expected.insert("fresh_topology");
        if (automatic_angle_removal) expected.insert("allow_automatic_angle_removal");
        std::set<std::string> actual;
        for (const auto& [key, ignored] : value.items()) { (void)ignored; actual.insert(key); }
        if (actual != expected || !value.at("replacement_dimension_ids").is_array())
            throw std::invalid_argument("Boundary redefinition fields are invalid");
        result.kind = BoundaryGeometryEditKind::redefine_boundary;
        result.target_id = result.boundary_id;
        result.replacement_segments = value.at("replacement_segments");
        result.replacement_authoring = value.at("replacement_authoring");
        result.replacement_properties = value.at("replacement_properties");
        result.replacement_dimension_ids = value.at("replacement_dimension_ids").get<std::vector<std::string>>();
        if (fresh_topology) {
            if (!value.at("fresh_topology").is_boolean() || (!automatic_angle_removal && value.at("fresh_topology") != true))
                throw std::invalid_argument("Version four redefinition requires explicit fresh topology");
            result.fresh_topology = value.at("fresh_topology").get<bool>();
        }
        if (automatic_angle_removal) {
            if (!value.at("allow_automatic_angle_removal").is_boolean() || value.at("allow_automatic_angle_removal") != true)
                throw std::invalid_argument("Version five redefinition requires explicit automatic angle removal");
            result.allow_automatic_angle_removal = true;
        }
        if (reference_plan) {
            result.replacement_child_mapping = value.at("replacement_child_mapping");
            if (!value.at("replacement_removed_reference_ids").is_array())
                throw std::invalid_argument("Boundary redefinition removed references must be an array");
            result.replacement_removed_reference_ids = value.at("replacement_removed_reference_ids").get<std::vector<std::string>>();
            if (!wall_source_replacement && result.replacement_child_mapping.empty() && result.replacement_removed_reference_ids.empty())
                throw std::invalid_argument("Version two redefinition requires explicit reference decisions");
        }
        if (wall_source_replacement) {
            if (!value.at("replacement_wall_source_ids").is_array() || (!fresh_topology && value.at("replacement_wall_source_ids").empty()))
                throw std::invalid_argument("Version three redefinition requires explicit replacement wall sources");
            result.replacement_wall_source_ids = value.at("replacement_wall_source_ids").get<std::vector<std::string>>();
        }
    } else if (kind == "reconstruct_arc") {
        const std::set<std::string> expected{
            "version", "kind", "boundary_id", "segment_id", "construction"};
        std::set<std::string> actual;
        for (const auto& [key, ignored] : value.items()) { (void)ignored; actual.insert(key); }
        if (reference_plan || actual != expected || !value.at("segment_id").is_string())
            throw std::invalid_argument("Boundary arc reconstruction edit fields are invalid");
        result.kind = BoundaryGeometryEditKind::reconstruct_arc;
        result.target_id = value.at("segment_id").get<std::string>();
        result.arc_construction = decode_construction_receipt(value.at("construction"));
    } else {
        throw std::invalid_argument("Boundary geometry edit kind is unsupported");
    }
    validate_boundary_geometry_edit(result);
    return result;
}

} // namespace sketch
