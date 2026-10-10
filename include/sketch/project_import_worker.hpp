#pragma once

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/dxf_project_exchange.hpp"
#include "sketch/geometry.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/stair_semantics.hpp"
#include "sketch/windows_import_worker.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

enum class ProjectImportKind { dxf, ifc };
inline constexpr std::size_t project_import_input_limit = 64 * 1024 * 1024;
inline constexpr std::size_t project_import_output_limit = 64 * 1024 * 1024;
inline constexpr std::size_t project_import_entity_limit = 100'000;
inline constexpr std::size_t project_import_diagnostic_limit = 250'000;
inline constexpr std::size_t project_import_boundary_segment_limit = 512;
inline constexpr std::size_t project_import_geometry_segment_limit = 50'000;
inline constexpr std::uint64_t project_import_geometry_pair_limit = 250'000;

struct ProjectImportDiagnostic {
    std::string source_id;
    std::string source_kind;
    std::string code;
};
struct ProjectImportCandidate {
    ProjectImportKind kind{ProjectImportKind::dxf};
    std::vector<Entity> entities;
    std::vector<ProjectImportDiagnostic> diagnostics;
    bool source_retention_required{};
    // Broker-derived receipt only; never trusted from the serialized worker.
    bool isolation_controls_attested{};
    // Transfer-only source proofs are shared by V7 members. They are never
    // embedded in live entities or treated as destination hierarchy authority.
    NativeDxfPhysicalSourceGraphs physical_source_graphs;
    // Original complete catalog snapshots are transfer evidence. The separate
    // live closure declares which owners reviewed destination binding may copy.
    NativeDxfCatalogSources catalog_sources;
    std::vector<std::string> authoring_catalog_ids;
};

inline const char* project_import_kind_name(ProjectImportKind kind) {
    switch (kind) {
    case ProjectImportKind::dxf: return "dxf";
    case ProjectImportKind::ifc: return "ifc";
    }
    throw std::invalid_argument("Invalid project import kind.");
}

namespace project_import_detail {
inline void reject() { throw std::invalid_argument("Invalid isolated project import response."); }

inline bool observed_worker_project_failure(const WindowsImportWorkerReport& report) {
    // controls_attested() deliberately requires successful completion. A
    // negative result uses the same observed controls, but can never publish a
    // candidate or establish why the operation failed. Any unrecognized broker
    // diagnostic preserves unavailable guidance.
    if (!report.launched || report.status != WindowsImportWorkerStatus::failed ||
        report.completed || report.timed_out || report.exit_code != 4 ||
        report.launch_error != 0 || report.job_assignment_error != 0 || !report.output.empty() ||
        !report.app_container_verified || !report.restricted_token_verified || !report.network_denial_verified ||
        !report.job_limits_verified || !report.job_membership_verified || !report.parent_exit_kill_verified ||
        !report.brokered_handles_verified || !report.private_temporary_root_verified ||
        !report.immutable_module_roots_verified || !report.fixed_search_applied || !report.proj_offline_applied)
        return false;
    if (report.diagnostics.empty()) return true;
    if (report.diagnostics.size() != 1) return false;
    const auto& code = report.diagnostics.front();
    return code == "worker_project_failed_core" || code == "worker_project_failed_library" ||
        code == "worker_project_failed_merge" || code == "worker_project_failed_candidate";
}

inline std::string observed_project_failure_message(const WindowsImportWorkerReport& report) {
    std::string message = "The isolated importer could not complete this project import";
    if (report.diagnostics.size() == 1) {
        const auto& stage = report.diagnostics.front();
        if (stage == "worker_project_failed_core") message += " during project parsing";
        else if (stage == "worker_project_failed_library") message += " during CAD library processing";
        else if (stage == "worker_project_failed_merge") message += " during import assembly";
        else if (stage == "worker_project_failed_candidate") message += " during import validation";
    }
    return message + "; the document is unchanged.";
}

struct GeometryBudget {
    std::size_t segments{};
    std::uint64_t pairs{};
    // Internal callers may identify their construction ledger exhaustion;
    // isolated protocol validation retains its canonical rejection message.
    const char* limit_error{};

    [[noreturn]] void reject_limit() const {
        if (limit_error) throw std::invalid_argument(limit_error);
        reject();
    }

    void charge(std::size_t count) {
        if (count == 0 || count > project_import_boundary_segment_limit ||
            segments > project_import_geometry_segment_limit ||
            count > project_import_geometry_segment_limit - segments) reject_limit();
        const auto pair_count = static_cast<std::uint64_t>(count) * (count - 1) / 2;
        if (pairs > project_import_geometry_pair_limit ||
            pair_count > project_import_geometry_pair_limit - pairs) reject_limit();
        segments += count;
        pairs += pair_count;
    }
    void charge_cross(std::size_t count) {
        const auto pair_count = static_cast<std::uint64_t>(count) * (count - 1) / 2;
        if (count > project_import_geometry_segment_limit ||
            pairs > project_import_geometry_pair_limit ||
            pair_count > project_import_geometry_pair_limit - pairs) reject_limit();
        pairs += pair_count;
    }
};
inline void fields(const nlohmann::json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) reject();
    for (const auto* key : keys) if (!value.contains(key)) reject();
}
inline std::string text(const nlohmann::json& value, bool empty = true,
                        std::size_t limit = 4096) {
    if (!value.is_string()) reject();
    const auto& result = value.get_ref<const std::string&>();
    if ((!empty && result.empty()) || result.size() > limit ||
        result.find('\0') != std::string::npos) reject();
    return result;
}
inline double number(const nlohmann::json& object, const char* key, bool positive = false) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number()) reject();
    const auto value = object.at(key).get<double>();
    if (!std::isfinite(value) || (positive && value <= default_geometry_tolerance_metres)) reject();
    return value;
}
inline Vec2 point(const nlohmann::json& value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) reject();
    const Vec2 result{value[0].get<double>(), value[1].get<double>()};
    if (!std::isfinite(result.x) || !std::isfinite(result.y)) reject();
    return result;
}
inline Segment segment(const nlohmann::json& value, bool exact_fields = true) {
    if (exact_fields) fields(value, {"start", "end", "sweep_radians"});
    else if (!value.is_object() || !value.contains("start") || !value.contains("end") ||
        !value.contains("sweep_radians")) reject();
    Segment result{point(value.at("start")), point(value.at("end")),
                   number(value, "sweep_radians")};
    try {
        if (segment_length(result) <= default_geometry_tolerance_metres) reject();
    } catch (...) { reject(); }
    return result;
}
inline Boundary boundary(const nlohmann::json& value, bool must_close, GeometryBudget& budget,
                         bool exact_fields = true) {
    if (!value.is_array() || value.empty()) reject();
    budget.charge(value.size());
    Boundary result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(segment(item, exact_fields));
    const auto diagnostics = validate_boundary(result);
    for (const auto& diagnostic : diagnostics) {
        if (!must_close && diagnostic.issue == BoundaryIssue::open_boundary) continue;
        reject();
    }
    return result;
}
inline Wall wall(const Entity& entity, bool native_physical_source = false) {
    Wall result;
    if (native_physical_source) {
        // V7 preserves valid native aliases, layer stacks and top profiles.
        // Decode an observation through the shared codec; leave raw candidate
        // properties and retained provenance unchanged.
        std::string error;
        if (!read_document_wall(entity, {}, result, error)) reject();
        return result;
    }
    result.id = entity.id;
    if (!entity.properties.contains("baseline")) reject();
    result.baseline = segment(entity.properties.at("baseline"));
    result.thickness = number(entity.properties, "thickness_m", true);
    result.height = number(entity.properties, "height_m", true);
    result.elevation = number(entity.properties, "elevation_m");
    if (entity.properties.contains("slope_rise_m"))
        result.slope_rise = number(entity.properties, "slope_rise_m");
    return result;
}
inline HostedOpening opening(const Entity& entity, std::string& wall_id, bool native_physical_source = false) {
    if (!entity.properties.contains("wall_id")) reject();
    wall_id = text(entity.properties.at("wall_id"), false);
    const auto quantity = [&](const char* canonical, const char* legacy, bool positive = false) {
        const auto* key = native_physical_source && !entity.properties.contains(canonical) ? legacy : canonical;
        return number(entity.properties, key, positive);
    };
    return HostedOpening{entity.id,
        quantity("offset_m", "offset"),
        quantity("width_m", "width", true),
        quantity("sill_m", "sill"),
        quantity("height_m", "height", true)};
}
inline void validate_slab(const Entity& entity, GeometryBudget& budget) {
    if (!entity.properties.contains("boundary") || !entity.properties.contains("holes") ||
        !entity.properties.at("holes").is_array()) reject();
    const auto outer = boundary(entity.properties.at("boundary"), true, budget);
    std::vector<Boundary> holes;
    std::size_t topology_segments = outer.size();
    holes.reserve(entity.properties.at("holes").size());
    for (const auto& value : entity.properties.at("holes")) {
        holes.push_back(boundary(value, true, budget));
        topology_segments += holes.back().size();
    }
    (void)number(entity.properties, "thickness_m", true);
    (void)number(entity.properties, "elevation_m");
    if (entity.properties.contains("element_kind")) {
        const auto kind = text(entity.properties.at("element_kind"), false);
        if (kind != "slab" && kind != "floor" && kind != "ceiling" && kind != "foundation") reject();
    }
    budget.charge_cross(topology_segments);
    if (validate_boundary_holes(outer, holes).has_value()) reject();
}

inline void validate_pending_dxf_physical_graph(const Entity& entity, bool physical_source_group) {
    const auto graph = entity.extensions.find("vertex_dxf_physical_source_graph");
    if (!physical_source_group) {
        if (graph != entity.extensions.end()) reject();
        return;
    }
    if (graph == entity.extensions.end()) reject();
    fields(*graph, {"version", "source_graph_id", "source_owner_id"});
    if (!graph->at("version").is_number_integer() || graph->at("version") != 1 ||
        text(graph->at("source_graph_id"), false, 128).empty() ||
        text(graph->at("source_owner_id"), false, 128).empty()) reject();
    // Only pending state is legal across the isolated-worker boundary. The
    // shared graph admission performs deep proof after all raw shape charges.
}
inline void validate_native_dxf_boundary(const Entity& entity, GeometryBudget& budget) {
    const auto& marker = entity.extensions.at("vertex_dxf_boundary");
    if (!marker.is_object() || !marker.contains("version") ||
        !marker.at("version").is_number_integer()) reject();
    const bool physical_source_group = marker.at("version") == 7 || marker.at("version") == 8;
    const bool measured_source_group = marker.at("version") == 6 || physical_source_group;
    const bool wall_source_group = marker.at("version") == 5 || measured_source_group;
    const bool floor_group = marker.at("version") == 4 || wall_source_group;
    const bool dependency_group = marker.at("version") == 3 || floor_group;
    if (dependency_group) {
        fields(marker, {"version", "depiction", "member_ids"});
        if (!marker.at("member_ids").is_array() || marker.at("member_ids").empty() ||
            marker.at("member_ids").size() > project_import_entity_limit) reject();
        for (const auto& id : marker.at("member_ids")) (void)text(id, false);
    } else {
        fields(marker, {"version", "depiction"});
        if (marker.at("version") != 2) reject();
    }
    if (entity.extensions.contains("vertex_dxf_stair_floor_binding") && !floor_group) reject();
    if (entity.extensions.contains("vertex_dxf_wall_source_context_binding") && !wall_source_group) reject();
    if (entity.extensions.contains("vertex_dxf_measured_graph") && !measured_source_group) reject();
    if (entity.extensions.contains("vertex_dxf_wall_source_hosted_openings")) reject();
    validate_pending_dxf_physical_graph(entity, physical_source_group);
    if (floor_group) {
        // The isolated worker supplies a detached graph. Only the desktop's
        // reviewed destination may establish an active floor relationship.
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
            if (entity.properties.contains(key)) reject();
        if (const auto binding = entity.extensions.find("vertex_dxf_stair_floor_binding");
            binding != entity.extensions.end()) {
            if (!binding->is_object() || !binding->contains("destination_floor_id") ||
                !binding->at("destination_floor_id").is_null()) reject();
        }
    }
    if (wall_source_group) {
        if (entity.properties.contains("phase_id")) reject();
        const auto binding = entity.extensions.find("vertex_dxf_wall_source_context_binding");
        if (binding == entity.extensions.end() || !binding->is_object() ||
            !binding->contains("destination_context") || !binding->at("destination_context").is_null()) reject();
        if (measured_source_group && (!binding->contains("destination_resolved_context") ||
            !binding->at("destination_resolved_context").is_null())) reject();
        // Full member/source admission follows the response-wide raw geometry
        // charges below. Dependency inspection must not trigger early replay.
    }
    if (marker.at("depiction") != "BOUNDARY_PLAN_V1" ||
        (entity.extensions.contains("physical_wall_room") && !physical_source_group) ||
        (!wall_source_group && (dependency_group ? native_dxf_boundary_has_untransported_source_links(entity)
                                                : native_dxf_boundary_has_untransported_links(entity)))) reject();
    const auto& properties = entity.properties;
    const char* key = properties.contains("boundary_model_version") ? "segments" :
        properties.contains("boundary") ? "boundary" : "segments";
    if (!properties.contains(key)) reject();
    // Bound pair/segment work before the full Document topology/receipt checks.
    // Native edges may include local identities and opaque source fields.
    const auto outer = boundary(properties.at(key), true, budget, false);
    std::vector<Boundary> holes;
    std::size_t topology_segments = outer.size();
    if (const auto values = properties.find("holes"); values != properties.end()) {
        if (!values->is_array()) reject();
        for (const auto& value : *values) {
            holes.push_back(boundary(value, true, budget, false));
            topology_segments += holes.back().size();
        }
    }
    if (const auto room = entity.extensions.find("physical_wall_room"); room != entity.extensions.end()) {
        if (entity.type != "room_boundary" || !room->is_object() || !room->contains("holes") ||
            !room->at("holes").is_array()) reject();
        for (const auto& value : room->at("holes")) {
            // Physical-room voids belong to the retained descriptor, rather
            // than the ordinary boundary properties. Charge them before any
            // shared descriptor decoder or source geometry proof can run.
            holes.push_back(boundary(value, true, budget));
            topology_segments += holes.back().size();
        }
    }
    budget.charge_cross(topology_segments);
    if (validate_boundary_holes(outer, holes)) reject();
}
// These checks deliberately use only the core protocol/analytical geometry.
// A broker must bound authored parameters before any native solid construction.
inline void validate_native_context(const Entity& entity) {
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "parent_id", "phase_id",
        "property_ids", "building_ids", "floor_ids", "layer_ids", "parent_ids", "phase_ids",
        "vertical_level_binding", "level_connection"})
        if (entity.properties.contains(key)) reject();
    const auto placement = entity.properties.find("vertical_placement");
    if (placement != entity.properties.end()) {
        fields(*placement, {"version", "mode", "offset_m"});
        if (!placement->at("version").is_number_integer() || placement->at("version") != 1 ||
            placement->at("mode") != "absolute" || std::abs(number(*placement, "offset_m")) > 1e9) reject();
    }
}
inline void validate_room(const Entity& entity, GeometryBudget& budget) {
    validate_native_context(entity);
    const auto& p = entity.properties;
    const auto* outer_key = p.contains("boundary") ? "boundary" : "segments";
    if (!p.contains(outer_key)) reject();
    const auto outer = boundary(p.at(outer_key), true, budget, false);
    std::vector<Boundary> holes;
    std::size_t topology_segments = outer.size();
    if (p.contains("holes")) {
        if (!p.at("holes").is_array() || p.at("holes").size() > 256) reject();
        for (const auto& value : p.at("holes")) {
            holes.push_back(boundary(value, true, budget, false));
            topology_segments += holes.back().size();
        }
    }
    (void)number(p, p.contains("height_m") ? "height_m" : "height", true);
    (void)number(p, p.contains("elevation_m") ? "elevation_m" : "elevation");
    budget.charge_cross(topology_segments);
    if (validate_boundary_holes(outer, holes).has_value()) reject();
}
inline void validate_roof(const Entity& entity, GeometryBudget& budget) {
    validate_native_context(entity);
    const auto& p = entity.properties;
    if (!p.contains("version") || !p.at("version").is_number_integer() ||
        (p.at("version") != 1 && p.at("version") != 2) || !p.contains("form")) reject();
    const auto form = text(p.at("form"), false);
    const bool panel = form == "sloped_roof_panel";
    if (!panel && form != "gable_roof" && form != "hip_roof") reject();
    if (!p.contains("base_position_m") || !p.at("base_position_m").is_array() ||
        p.at("base_position_m").size() != 3) reject();
    for (const auto& coordinate : p.at("base_position_m")) {
        if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>()) ||
            std::abs(coordinate.get<double>()) > 1e9) reject();
    }
    if (std::abs(number(p, "orientation_rad")) > 1e6) reject();
    const auto dimension = [&](const char* key, bool positive = true) {
        const auto value = number(p, key, positive);
        if (value < 0 || value > 1e6) reject();
        return value;
    };
    const auto length = dimension(panel ? "run_m" : "length_m");
    const auto span = dimension("span_m");
    const auto rise = dimension("rise_m", false);
    const auto pitch = number(p, "pitch_rad");
    const auto overhang = dimension("overhang_m", false);
    const auto thickness = dimension("thickness_m");
    const auto run = panel ? length : span * .5;
    const auto tolerance = default_geometry_tolerance_metres;
    if (run <= tolerance) reject();
    if (!(panel && rise == 0 && pitch == 0)) {
        if (rise <= tolerance || pitch <= tolerance || pitch >= std::acos(-1.0) * .5 - tolerance) reject();
        const auto expected_rise = run * std::tan(pitch);
        if (!std::isfinite(expected_rise) || std::abs(expected_rise) > 1e9 ||
            std::abs(expected_rise - rise) > std::max(1e-9, std::abs(rise) * 1e-9)) reject();
    }
    const auto slope = rise / run;
    const auto normal_scale = std::sqrt(1 + slope * slope);
    if (!std::isfinite(normal_scale) || normal_scale > 1e9) reject();
    if (form == "gable_roof") {
        const auto inward = slope * thickness / normal_scale;
        if (length + 2 * overhang > 1e6 || inward <= tolerance || inward >= span * .5 + overhang - tolerance ||
            std::abs(rise - thickness * normal_scale) > 1e9 || overhang * slope > 1e9) reject();
    } else if (form == "hip_roof") {
        const auto ridge_half = (length - span) * .5;
        if (length < span || (ridge_half != 0 && ridge_half <= tolerance) ||
            length + 2 * overhang > 1e6 || span + 2 * overhang > 1e6 ||
            thickness * normal_scale > 1e6 || overhang * slope > 1e9) reject();
    } else if (std::abs(slope * (length + overhang)) > 1e9 || overhang * slope > 1e9) reject();
    if (p.at("version") == 1) {
        if (p.contains("roof_openings")) reject();
        return;
    }
    if (!p.contains("roof_openings") || !p.at("roof_openings").is_array() || p.at("roof_openings").size() > 256) reject();
    const auto& cuts = p.at("roof_openings");
    budget.charge_cross(cuts.size());
    std::set<std::string> ids;
    struct Cut { double x, y, right, top; };
    std::vector<Cut> previous;
    for (const auto& cut : cuts) {
        if (!cut.is_object() || !cut.contains("id")) reject();
        const auto id = text(cut.at("id"), false, 128);
        if (!ids.insert(id).second || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) reject();
        const auto x = number(cut, "x_m"), y = number(cut, "y_m");
        const auto width = number(cut, "width_m", true), depth = number(cut, "depth_m", true);
        if (width > 1e6 || depth > 1e6) reject();
        const auto right = x + width, top = y + depth;
        const auto xmin = panel ? 0 : -length * .5, ymin = panel ? 0 : -span * .5;
        const auto xmax = panel ? length : length * .5, ymax = panel ? span : span * .5;
        const auto clearance = thickness + tolerance;
        if (!std::isfinite(right) || !std::isfinite(top) || x < xmin + clearance || y < ymin + clearance ||
            right > xmax - clearance || top > ymax - clearance) reject();
        for (const auto& prior : previous)
            if (x <= prior.right + tolerance && right >= prior.x - tolerance &&
                y <= prior.top + tolerance && top >= prior.y - tolerance) reject();
        previous.push_back({x, y, right, top});
    }
}
inline void validate_ifc_reference(const Entity& entity) {
    fields(entity.properties, {"ifc_name", "ifc_type"});
    (void)text(entity.properties.at("ifc_name"));
    (void)text(entity.properties.at("ifc_type"), false);
    fields(entity.extensions, {"ifc_source", "ifc_vertex_properties"});
    const auto& source = entity.extensions.at("ifc_source");
    fields(source, {"record_id", "record_type", "arguments"});
    if (!source.at("record_id").is_number_integer() ||
        source.at("record_id").get<std::int64_t>() <= 0) reject();
    (void)text(source.at("record_type"), false);
    (void)text(source.at("arguments"), true, 1024 * 1024);
    if (!entity.extensions.at("ifc_vertex_properties").is_object()) reject();
}

// Bound canonical stair/rail layout work before a decoder can allocate treads,
// posts or perform landing-pair checks. This is analytical, not solid creation.
inline std::size_t native_stair_railing_work(const Entity& entity, const Entity* host = nullptr) {
    const auto stair_work = [](const nlohmann::json& p) {
        if (!p.is_object() || !p.contains("riser_count") || !p.at("riser_count").is_number_integer() ||
            p.at("riser_count") < 1 || p.at("riser_count") > 10000) reject();
        std::size_t landings = 0;
        if (p.contains("landings")) {
            if (!p.at("landings").is_array() || p.at("landings").size() > 256) reject();
            landings = p.at("landings").size();
        }
        if (p.contains("flights")) {
            const auto& flights = p.at("flights");
            if (!flights.is_array() || flights.empty() || flights.size() > 256) reject();
            std::size_t risers = 0;
            for (const auto& flight : flights) {
                if (!flight.is_object() || !flight.contains("riser_count") ||
                    !flight.at("riser_count").is_number_integer() || flight.at("riser_count") < 1 ||
                    flight.at("riser_count") > 10000 - risers) reject();
                risers += flight.at("riser_count").get<std::size_t>();
            }
            if (risers != p.at("riser_count").get<std::size_t>() || landings + 1 != flights.size()) reject();
        }
        return p.at("riser_count").get<std::size_t>() + landings + 2;
    };
    if (entity.type == "stair") return stair_work(entity.properties);
    if (entity.type != "railing") reject();
    const auto positive = [](const nlohmann::json& p, const char* key) {
        const auto value = number(p, key, true);
        if (value > 1e6) reject();
        return value;
    };
    const auto& p = entity.properties;
    const auto spacing = positive(p, "post_spacing_m");
    double length = 0;
    std::size_t work = 0;
    if (p.contains("host")) {
        if (!host || host->type != "stair") reject();
        work = stair_work(host->properties);
        const auto& s = host->properties;
        const auto going = positive(s, "going_m"), width = positive(s, "width_m");
        double maximum_width = 0;
        length = positive(s, "total_rise_m");
        if (s.contains("flights")) {
            for (const auto& flight : s.at("flights")) {
                const auto flight_going = flight.contains("going_m") ? positive(flight, "going_m") : going;
                const auto flight_width = flight.contains("width_m") ? positive(flight, "width_m") : width;
                length += flight.at("riser_count").get<double>() * flight_going;
                maximum_width = std::max(maximum_width, flight_width);
            }
        } else {
            length += s.at("riser_count").get<double>() * going;
            maximum_width = width;
        }
        // Every landing edge is bounded by its depth or the sum of its actual
        // adjacent flight widths plus the return gap. Maximum resolved width
        // also covers straight/quarter-turn and final landing extents.
        if (s.contains("landings")) for (const auto& landing : s.at("landings")) {
            const auto gap = landing.contains("return_gap_m") ? number(landing, "return_gap_m") : 0.0;
            if (gap < 0 || gap > 1e6) reject();
            length += positive(landing, "depth_m") + 2 * maximum_width + gap;
        }
        if (s.contains("top_landing") && !s.at("top_landing").is_null())
            length += positive(s.at("top_landing"), "depth_m") + maximum_width;
    } else length = positive(p, "length_m");
    const auto posts = std::ceil(length / spacing) + 3;
    if (!std::isfinite(posts) || posts <= 0 || posts > 10003) reject();
    return work + static_cast<std::size_t>(posts);
}

// Hosted native rail admission requires explicit organization. An IFC candidate
// has deliberately detached that foreign authority. Validate a private copy in
// one collision-free organization; these entities/refs never enter the wire or
// live project. The desktop later assigns the actual complete drawing context.
inline std::vector<Entity> detached_ifc_validation_entities(const std::vector<Entity>& entities) {
    std::vector<Entity> copy = entities;
    std::set<std::string, std::less<>> used;
    bool needs_context = false;
    for (const auto& entity : entities) {
        used.insert(entity.id);
        if (entity.type != "stair" && entity.type != "railing") continue;
        needs_context = true;
        validate_native_context(entity);
        if (entity.properties.contains("vertical_placement")) reject();
        for (const auto* key : {"flights", "landings"}) {
            const auto records = entity.properties.find(key);
            if (records == entity.properties.end()) continue;
            if (!records->is_array() || records->size() > 256) reject();
            for (const auto& record : *records)
                if (record.is_object() && record.contains("id")) used.insert(text(record.at("id"), false, 128));
        }
    }
    if (!needs_context) return copy;
    const auto unique = [&](const char* role) {
        const std::string base = std::string{"ifc-validation-"} + role;
        auto id = base;
        for (std::size_t suffix = 1; used.contains(id); ++suffix) id = base + '-' + std::to_string(suffix);
        used.insert(id);
        return id;
    };
    const auto property = unique("property"), building = unique("building"),
        floor = unique("floor"), layer = unique("layer");
    for (auto& entity : copy) if (entity.type == "stair" || entity.type == "railing") {
        entity.properties["property_id"] = property;
        entity.properties["building_id"] = building;
        entity.properties["floor_id"] = floor;
        entity.properties["layer_id"] = layer;
    }
    copy.push_back({property, "property"});
    copy.push_back({building, "building", {{"property_id", property}}});
    copy.push_back({floor, "floor", {{"building_id", building}}});
    copy.push_back({layer, "layer", {{"floor_id", floor}}});
    return copy;
}
inline void validate(const ProjectImportCandidate& result) {
    (void)project_import_kind_name(result.kind);
    if (result.entities.size() > project_import_entity_limit ||
        result.diagnostics.size() > project_import_diagnostic_limit ||
        (!result.diagnostics.empty() && !result.source_retention_required)) reject();
    if (result.kind != ProjectImportKind::dxf && (!result.physical_source_graphs.empty() ||
        !result.catalog_sources.empty() || !result.authoring_catalog_ids.empty())) reject();
    if (result.catalog_sources.size() > project_import_entity_limit ||
        result.authoring_catalog_ids.size() > result.catalog_sources.size() ||
        !std::is_sorted(result.authoring_catalog_ids.begin(), result.authoring_catalog_ids.end()) ||
        std::adjacent_find(result.authoring_catalog_ids.begin(), result.authoring_catalog_ids.end()) !=
            result.authoring_catalog_ids.end()) reject();
    for (const auto& id : result.authoring_catalog_ids) {
        (void)text(nlohmann::json(id), false, 128);
        if (!result.catalog_sources.contains(id)) reject();
    }
    for (const auto& [id, snapshot] : result.catalog_sources) {
        (void)text(nlohmann::json(id), false, 128);
        fields(snapshot, {"id", "type", "properties", "required", "extensions"});
        if (text(snapshot.at("id"), false, 128) != id || snapshot.at("type") != "assembly_model" ||
            !snapshot.at("required").is_boolean() || !snapshot.at("properties").is_object() ||
            !snapshot.at("extensions").is_object()) reject();
        // The core's shared catalog ledger admits raw model size/work before
        // decoding. Do not normalize or prune any row at the wire boundary.
    }
    if (result.physical_source_graphs.size() > project_import_entity_limit) reject();
    std::size_t physical_proof_bytes = 0;
    std::set<std::string, std::less<>> referenced_physical_graphs;
    for (const auto& [id, proof] : result.physical_source_graphs) {
        (void)text(nlohmann::json(id), false, 128);
        const bool catalog_proof = proof.is_object() && proof.contains("version") && proof.at("version") == 2;
        if (catalog_proof) fields(proof, {"version", "entities", "catalog_ids"});
        else fields(proof, {"version", "entities"});
        if (!proof.at("version").is_number_integer() || (!catalog_proof && proof.at("version") != 1) ||
            !proof.at("entities").is_array() || proof.at("entities").empty() ||
            proof.at("entities").size() > 4096) reject();
        if (catalog_proof) {
            if (result.catalog_sources.empty() || !proof.at("catalog_ids").is_array() ||
                proof.at("catalog_ids").empty() || proof.at("catalog_ids").size() > result.catalog_sources.size()) reject();
            std::string previous;
            for (const auto& value : proof.at("catalog_ids")) {
                const auto catalog_id = text(value, false, 128);
                if ((!previous.empty() && previous >= catalog_id) || !result.catalog_sources.contains(catalog_id)) reject();
                previous = catalog_id;
            }
            for (const auto& snapshot : proof.at("entities")) {
                fields(snapshot, {"id", "type", "properties", "required", "extensions"});
                if (!snapshot.at("required").is_boolean() ||
                    snapshot.at("type") == "assembly_model" || !snapshot.at("properties").is_object() ||
                    !snapshot.at("extensions").is_object()) reject();
                (void)text(snapshot.at("id"), false, 128);
                (void)text(snapshot.at("type"), false, 128);
            }
        }
        const auto bytes = proof.dump().size();
        constexpr std::size_t physical_proof_limit = 16 * 1024 * 1024;
        if (bytes > physical_proof_limit - physical_proof_bytes) reject();
        physical_proof_bytes += bytes;
    }
    for (const auto& diagnostic : result.diagnostics) {
        (void)text(diagnostic.source_id); (void)text(diagnostic.source_kind); (void)text(diagnostic.code, false);
    }
    std::set<std::string, std::less<>> entity_ids;
    std::map<std::string, Wall, std::less<>> walls;
    std::vector<std::pair<std::string, HostedOpening>> openings;
    std::map<std::string, const Entity*, std::less<>> native_entities;
    for (const auto& entity : result.entities)
        if (entity.type == "stair" || entity.type == "railing") {
            if (!native_entities.emplace(entity.id, &entity).second) reject();
        }
    GeometryBudget geometry_budget;
    for (const auto& entity : result.entities) {
        (void)text(entity.id, false);
        const auto transfer_marker = entity.extensions.find("vertex_dxf_boundary");
        const bool complete_catalog_body = result.kind == ProjectImportKind::dxf &&
            transfer_marker != entity.extensions.end() && transfer_marker->is_object() &&
            transfer_marker->value("version", 0) == 8;
        if (!entity_ids.insert(entity.id).second || (entity.required && !complete_catalog_body) ||
            !entity.properties.is_object() || !entity.extensions.is_object()) reject();
        const bool native_dxf_boundary = result.kind == ProjectImportKind::dxf &&
            can_recognize_boundary_entity_type(entity.type) && entity.extensions.contains("vertex_dxf_boundary");
        const bool native_dxf_wall_source = result.kind == ProjectImportKind::dxf &&
            entity.extensions.contains("vertex_dxf_boundary") && entity.extensions.at("vertex_dxf_boundary").is_object() &&
            (entity.extensions.at("vertex_dxf_boundary").value("version", 0) == 5 ||
             entity.extensions.at("vertex_dxf_boundary").value("version", 0) == 6 ||
             entity.extensions.at("vertex_dxf_boundary").value("version", 0) == 7 ||
             entity.extensions.at("vertex_dxf_boundary").value("version", 0) == 8);
        const bool native_dxf_catalog_source = native_dxf_wall_source &&
            entity.extensions.at("vertex_dxf_boundary").at("version") == 8;
        const bool native_dxf_physical_source = native_dxf_wall_source &&
            (entity.extensions.at("vertex_dxf_boundary").at("version") == 7 || native_dxf_catalog_source);
        const bool native_dxf_measured_source = native_dxf_wall_source &&
            (entity.extensions.at("vertex_dxf_boundary").at("version") == 6 || native_dxf_physical_source);
        validate_pending_dxf_physical_graph(entity, native_dxf_physical_source);
        if (native_dxf_catalog_source && result.catalog_sources.empty()) reject();
        if (native_dxf_physical_source) {
            const auto graph_id = text(entity.extensions.at("vertex_dxf_physical_source_graph").at("source_graph_id"), false, 128);
            if (!result.physical_source_graphs.contains(graph_id)) reject();
            if (result.physical_source_graphs.at(graph_id).at("version") != (native_dxf_catalog_source ? 2 : 1)) reject();
            referenced_physical_graphs.insert(graph_id);
        }
        const bool shared = entity.type == "boundary" || entity.type == "wall" || entity.type == "opening";
        const bool dxf = entity.type == "annotation_state" || native_dxf_boundary ||
            (native_dxf_measured_source && entity.type == "measurement_linework");
        const bool ifc = entity.type == "wall" || entity.type == "slab" || entity.type == "roof" || entity.type == "room" ||
            entity.type == "opening" || entity.type == "ifc_reference" ||
            entity.type == "stair" || entity.type == "railing";
        if (!shared && !(result.kind == ProjectImportKind::dxf ? dxf : ifc)) reject();
        if (entity.properties.contains("parent_id") || entity.properties.contains("layer_id")) reject();
        if (native_dxf_wall_source && !native_dxf_boundary) {
            if (entity.type != "wall" && entity.type != "opening" &&
                !(native_dxf_measured_source && entity.type == "measurement_linework")) reject();
            for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "phase_id"})
                if (entity.properties.contains(key)) reject();
            const auto binding = entity.extensions.find("vertex_dxf_wall_source_context_binding");
            if (binding == entity.extensions.end() || !binding->is_object() ||
                !binding->contains("destination_context") || !binding->at("destination_context").is_null()) reject();
            if (native_dxf_measured_source && (!binding->contains("destination_resolved_context") ||
                !binding->at("destination_resolved_context").is_null())) reject();
            // The shared whole-group pass validates this member after all raw
            // shape charges, including standalone measured strokes.
        } else if (!native_dxf_boundary &&
            (entity.extensions.contains("vertex_dxf_wall_source_context_binding") ||
             entity.extensions.contains("vertex_dxf_wall_source_hosted_openings") ||
             entity.extensions.contains("vertex_dxf_measured_graph"))) reject();
        if (entity.extensions.contains("vertex_dxf_measured_graph") && !native_dxf_measured_source) reject();
        if (native_dxf_boundary) {
            validate_native_dxf_boundary(entity, geometry_budget);
        } else if (entity.type == "boundary") {
            if (entity.properties.contains("boundary_model_version") ||
                !entity.properties.contains("boundary") ||
                !entity.properties.contains("classification") ||
                !entity.properties.at("classification").is_string()) reject();
            (void)text(entity.properties.at("classification"), false);
            (void)boundary(entity.properties.at("boundary"), false, geometry_budget);
        } else if (entity.type == "annotation_state") {
            try { validate_annotation_entity(entity); } catch (...) { reject(); }
        } else if (entity.type == "wall") {
            if (native_dxf_wall_source) geometry_budget.charge(1);
            if (!walls.emplace(entity.id, wall(entity, native_dxf_physical_source)).second) reject();
        } else if (entity.type == "measurement_linework") {
            if (!native_dxf_measured_source || !entity.properties.contains("model")) reject();
            const auto& model = entity.properties.at("model");
            if (!model.is_object() || !model.contains("segments") || !model.at("segments").is_array()) reject();
            // Charge shape work before the shared V6 preflight and replay.
            // Open, retraced and crossing strokes are valid source inputs;
            // they must not receive closed-boundary topology validation here.
            geometry_budget.charge(model.at("segments").size());
        } else if (entity.type == "slab") {
            validate_slab(entity, geometry_budget);
        } else if (entity.type == "roof") {
            validate_roof(entity, geometry_budget);
        } else if (entity.type == "room") {
            validate_room(entity, geometry_budget);
        } else if (entity.type == "opening") {
            std::string wall_id;
            auto hosted = opening(entity, wall_id, native_dxf_physical_source);
            if (entity.properties.contains("opening_kind")) {
                const auto kind = text(entity.properties.at("opening_kind"), false);
                if (kind != "opening" && kind != "door" && kind != "window") reject();
            }
            try {
                if (entity.properties.contains("opening_assembly")) {
                    const auto assembly = parse_opening_assembly(entity.properties.at("opening_assembly"));
                    const auto kind = entity.properties.value("opening_kind", std::string{"opening"});
                    if (kind != opening_assembly_kind_name(assembly.kind)) reject();
                }
                if (entity.properties.contains("door_operation"))
                    (void)decode_door_operation(entity.properties.at("door_operation"));
            } catch (...) { reject(); }
            openings.emplace_back(std::move(wall_id), std::move(hosted));
        } else if (entity.type == "ifc_reference") {
            validate_ifc_reference(entity);
        } else if (entity.type == "stair" || entity.type == "railing") {
            validate_native_context(entity);
            const auto& p = entity.properties;
            if (p.contains("vertical_placement") || !p.contains("version") ||
                !p.at("version").is_number_integer() || !p.contains("form") || !p.at("form").is_string()) reject();
            const bool supported = entity.type == "stair"
                ? (p.at("version") == 1 && p.at("form") == "straight_stair_flight") ||
                    ((p.at("version") == 2 || p.at("version") == 3 || p.at("version") == 4) && p.at("form") == "multi_flight_stair")
                : (p.at("version") == 1 && p.at("form") == "straight_railing") ||
                    (p.at("version") == 2 && p.at("form") == "stair_flight_railing") ||
                    (p.at("version") == 3 && p.at("form") == "stair_landing_railing");
            if (!supported) reject();
            const Entity* host = nullptr;
            if (p.contains("host")) {
                if (!p.at("host").is_object() || !p.at("host").contains("stair_id")) reject();
                const auto id = text(p.at("host").at("stair_id"), false, 128);
                const auto found = native_entities.find(id);
                if (found == native_entities.end() || found->second->type != "stair") reject();
                host = found->second;
            }
            geometry_budget.charge(native_stair_railing_work(entity, host));
        }
    }
    if (referenced_physical_graphs.size() != result.physical_source_graphs.size()) reject();
    for (auto& [wall_id, hosted] : openings) {
        const auto host = walls.find(wall_id);
        if (host == walls.end()) reject();
        host->second.openings.push_back(std::move(hosted));
    }
    for (const auto& [id, value] : walls) {
        (void)id;
        try { validate_wall_semantics(value); } catch (...) { reject(); }
    }
    // Validate the detached graph using the same native entity, reference and
    // geometry checks as an ordinary command. No live document is mutated.
    if (result.kind == ProjectImportKind::dxf && !result.catalog_sources.empty()) {
        NativeDxfWallSourceWorkBudget source_budget;
        try {
            // All candidate shape charges precede catalog/model decoding. The
            // same ledger includes preflight, authentication and private-copy
            // admission; downstream repeats do not receive a fresh budget.
            validate_native_dxf_catalog_sources(result.entities, result.physical_source_graphs,
                result.catalog_sources, result.authoring_catalog_ids, &source_budget, true);
            auto document = Document::create(native_dxf_catalog_pending_admission_entities(result.entities,
                result.physical_source_graphs, result.catalog_sources, result.authoring_catalog_ids, &source_budget));
            if (!document.snapshot().is_editable()) reject();
        } catch (...) { reject(); }
        return;
    }
    if (result.kind == ProjectImportKind::dxf) {
        try { validate_native_dxf_boundary_groups(result.entities, &result.physical_source_graphs); } catch (...) { reject(); }
    }
    auto document = Document::create(result.kind == ProjectImportKind::ifc
        ? detached_ifc_validation_entities(result.entities) : result.entities);
    if (!document.snapshot().is_editable()) reject();
}
} // namespace project_import_detail

// Versioned bounded JSON is a native candidate protocol, never DXF/STEP text.
// Decoding rejects untrusted depth/counts before DOM allocation, then validates
// every candidate through Document. Callers still commit an ordinary command.
inline std::vector<std::byte> encode_project_import_candidate(const ProjectImportCandidate& result) {
    project_import_detail::validate(result);
    const bool catalog_protocol = !result.catalog_sources.empty();
    nlohmann::json entities = nlohmann::json::array(), diagnostics = nlohmann::json::array();
    for (const auto& e : result.entities) {
        auto record = nlohmann::json{{"id", e.id}, {"type", e.type}, {"properties", e.properties}, {"extensions", e.extensions}};
        if (catalog_protocol) record["required"] = e.required;
        entities.push_back(std::move(record));
    }
    for (const auto& d : result.diagnostics)
        diagnostics.push_back({{"source_id", d.source_id}, {"source_kind", d.source_kind}, {"code", d.code}});
    auto value = nlohmann::json{{"protocol", catalog_protocol ? "PSIP0003" :
            result.physical_source_graphs.empty() ? "PSIP0001" : "PSIP0002"},
        {"kind", project_import_kind_name(result.kind)},
        {"entities", std::move(entities)}, {"diagnostics", std::move(diagnostics)},
        {"source_retention_required", result.source_retention_required}};
    if (catalog_protocol || !result.physical_source_graphs.empty())
        value["physical_source_graphs"] = result.physical_source_graphs;
    if (catalog_protocol) {
        value["catalog_sources"] = result.catalog_sources;
        value["authoring_catalog_ids"] = result.authoring_catalog_ids;
    }
    const auto wire = value.dump();
    if (wire.size() > project_import_output_limit) project_import_detail::reject();
    std::vector<std::byte> output(wire.size());
    std::memcpy(output.data(), wire.data(), wire.size());
    return output;
}

inline ProjectImportCandidate decode_project_import_candidate(
    const WindowsImportWorkerReport& report, ProjectImportKind expected_kind) {
    using namespace project_import_detail;
    if (!report.controls_attested() || report.exit_code != 0 || report.timed_out ||
        !report.diagnostics.empty() || report.output.empty() || report.output.size() > project_import_output_limit) reject();
    std::size_t nodes = 0;
    std::vector<std::set<std::string>> object_keys;
    const auto callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
        if (depth > assembly_catalog_transport_depth_limit || ++nodes > assembly_catalog_transport_node_limit) reject();
        if (event == nlohmann::json::parse_event_t::object_start) object_keys.emplace_back();
        if (event == nlohmann::json::parse_event_t::key &&
            (object_keys.empty() || !object_keys.back().insert(value.get<std::string>()).second)) reject();
        if (event == nlohmann::json::parse_event_t::object_end) object_keys.pop_back();
        if (value.is_string() && value.get_ref<const std::string&>().size() > assembly_catalog_transport_byte_limit) reject();
        if (event == nlohmann::json::parse_event_t::value && value.is_number_float() &&
            !std::isfinite(value.get<double>())) reject();
        return true;
    };
    const auto* begin = reinterpret_cast<const char*>(report.output.data());
    const auto value = nlohmann::json::parse(begin, begin + report.output.size(), callback);
    const bool physical_protocol = value.is_object() && value.contains("protocol") && value.at("protocol") == "PSIP0002";
    const bool catalog_protocol = value.is_object() && value.contains("protocol") && value.at("protocol") == "PSIP0003";
    // The initial parser has one bounded ceiling. Only the exact PSIP0003
    // catalog field uses the complete-catalog framing allowance. Preserve the
    // original limits for every other field and for the entire PSIP0001/2
    // payload, regardless of incoming object-key order.
    const auto legacy_json_limits = [&](const auto& self, const nlohmann::json& item, int depth) -> void {
        if (depth > 32) reject();
        const auto check_string = [&](const std::string& text) {
            if (text.size() > 1024 * 1024 || text.find('\0') != std::string::npos) reject();
        };
        if (item.is_string()) check_string(item.get_ref<const std::string&>());
        else if (item.is_object()) for (auto field = item.begin(); field != item.end(); ++field) {
            if (depth + 1 > 32) reject();
            check_string(field.key());
            if (catalog_protocol && depth == 0 && field.key() == "catalog_sources") continue;
            self(self, field.value(), depth + 1);
        }
        else if (item.is_array()) for (const auto& child : item) self(self, child, depth + 1);
    };
    legacy_json_limits(legacy_json_limits, value, 0);
    if (catalog_protocol)
        fields(value, {"protocol", "kind", "entities", "diagnostics", "source_retention_required",
            "physical_source_graphs", "catalog_sources", "authoring_catalog_ids"});
    else if (physical_protocol)
        fields(value, {"protocol", "kind", "entities", "diagnostics", "source_retention_required", "physical_source_graphs"});
    else fields(value, {"protocol", "kind", "entities", "diagnostics", "source_retention_required"});
    if ((!physical_protocol && !catalog_protocol && value.at("protocol") != "PSIP0001") ||
        value.at("kind") != project_import_kind_name(expected_kind) ||
        !value.at("source_retention_required").is_boolean() || !value.at("entities").is_array() ||
        !value.at("diagnostics").is_array() || value.at("entities").size() > project_import_entity_limit ||
        value.at("diagnostics").size() > project_import_diagnostic_limit) reject();
    ProjectImportCandidate result;
    result.kind = expected_kind;
    result.source_retention_required = value.at("source_retention_required").get<bool>();
    if (physical_protocol || catalog_protocol) {
        const auto& proofs = value.at("physical_source_graphs");
        if (expected_kind != ProjectImportKind::dxf || !proofs.is_object() || (!catalog_protocol && proofs.empty()) ||
            proofs.size() > project_import_entity_limit) reject();
        for (const auto& [id, proof] : proofs.items())
            result.physical_source_graphs.emplace(text(nlohmann::json(id), false, 128), proof);
    }
    if (catalog_protocol) {
        const auto& catalogs = value.at("catalog_sources");
        const auto& authoring = value.at("authoring_catalog_ids");
        if (expected_kind != ProjectImportKind::dxf || !catalogs.is_object() || catalogs.empty() ||
            catalogs.size() > project_import_entity_limit || !authoring.is_array() ||
            authoring.size() > catalogs.size()) reject();
        for (const auto& [id, snapshot] : catalogs.items())
            result.catalog_sources.emplace(text(nlohmann::json(id), false, 128), snapshot);
        for (const auto& id : authoring) result.authoring_catalog_ids.push_back(text(id, false, 128));
    }
    for (const auto& e : value.at("entities")) {
        if (catalog_protocol) {
            fields(e, {"id", "type", "properties", "required", "extensions"});
            if (!e.at("required").is_boolean()) reject();
        } else fields(e, {"id", "type", "properties", "extensions"});
        result.entities.push_back({text(e.at("id"), false), text(e.at("type"), false),
            e.at("properties"), catalog_protocol && e.at("required").get<bool>(), e.at("extensions")});
    }
    for (const auto& d : value.at("diagnostics")) {
        fields(d, {"source_id", "source_kind", "code"});
        result.diagnostics.push_back({text(d.at("source_id")), text(d.at("source_kind")), text(d.at("code"), false)});
    }
    validate(result);
    result.isolation_controls_attested = true;
    return result;
}

using ProjectImportBroker = std::function<WindowsImportWorkerReport(const WindowsImportWorkerOptions&)>;
inline ProjectImportCandidate import_project_in_worker(std::span<const std::byte> source,
    ProjectImportKind kind, WindowsImportWorkerOptions options,
    const ProjectImportBroker& broker = run_windows_import_worker) {
    if (source.empty() || source.size() > project_import_input_limit)
        throw std::invalid_argument("Project imports must contain between 1 byte and 64 MiB.");
    const std::string name = project_import_kind_name(kind);
    options.arguments = {std::wstring(name.begin(), name.end()), L"0"};
    options.input.assign(source.begin(), source.end());
    options.max_output_bytes = project_import_output_limit;
    options.timeout_ms = 30'000;
    options.memory_bytes = 512ULL * 1024 * 1024;
    options.max_active_processes = 1;
    options.proj_offline_required = true;
    const auto report = broker(options);
    if (!report.controls_attested()) {
        if (project_import_detail::observed_worker_project_failure(report))
            throw std::runtime_error(project_import_detail::observed_project_failure_message(report));
        throw std::runtime_error("Isolated project import is unavailable. Install or repair the bundled vertex-import-worker in a read-only application directory; the Windows sandbox must be available.");
    }
    return decode_project_import_candidate(report, kind);
}
} // namespace sketch
