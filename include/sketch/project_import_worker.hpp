#pragma once

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/windows_import_worker.hpp"
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

struct GeometryBudget {
    std::size_t segments{};
    std::uint64_t pairs{};

    void charge(std::size_t count) {
        if (count == 0 || count > project_import_boundary_segment_limit ||
            segments > project_import_geometry_segment_limit ||
            count > project_import_geometry_segment_limit - segments) reject();
        const auto pair_count = static_cast<std::uint64_t>(count) * (count - 1) / 2;
        if (pairs > project_import_geometry_pair_limit ||
            pair_count > project_import_geometry_pair_limit - pairs) reject();
        segments += count;
        pairs += pair_count;
    }
    void charge_cross(std::size_t count) {
        const auto pair_count = static_cast<std::uint64_t>(count) * (count - 1) / 2;
        if (count > project_import_geometry_segment_limit ||
            pairs > project_import_geometry_pair_limit ||
            pair_count > project_import_geometry_pair_limit - pairs) reject();
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
inline Segment segment(const nlohmann::json& value) {
    fields(value, {"start", "end", "sweep_radians"});
    Segment result{point(value.at("start")), point(value.at("end")),
                   number(value, "sweep_radians")};
    try {
        if (segment_length(result) <= default_geometry_tolerance_metres) reject();
    } catch (...) { reject(); }
    return result;
}
inline Boundary boundary(const nlohmann::json& value, bool must_close, GeometryBudget& budget) {
    if (!value.is_array() || value.empty()) reject();
    budget.charge(value.size());
    Boundary result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(segment(item));
    const auto diagnostics = validate_boundary(result);
    for (const auto& diagnostic : diagnostics) {
        if (!must_close && diagnostic.issue == BoundaryIssue::open_boundary) continue;
        reject();
    }
    return result;
}
inline Wall wall(const Entity& entity) {
    Wall result;
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
inline HostedOpening opening(const Entity& entity, std::string& wall_id) {
    if (!entity.properties.contains("wall_id")) reject();
    wall_id = text(entity.properties.at("wall_id"), false);
    return HostedOpening{entity.id,
        number(entity.properties, "offset_m"),
        number(entity.properties, "width_m", true),
        number(entity.properties, "sill_m"),
        number(entity.properties, "height_m", true)};
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
inline void validate(const ProjectImportCandidate& result) {
    (void)project_import_kind_name(result.kind);
    if (result.entities.size() > project_import_entity_limit ||
        result.diagnostics.size() > project_import_diagnostic_limit ||
        (!result.diagnostics.empty() && !result.source_retention_required)) reject();
    for (const auto& diagnostic : result.diagnostics) {
        (void)text(diagnostic.source_id); (void)text(diagnostic.source_kind); (void)text(diagnostic.code, false);
    }
    std::set<std::string, std::less<>> entity_ids;
    std::map<std::string, Wall, std::less<>> walls;
    std::vector<std::pair<std::string, HostedOpening>> openings;
    GeometryBudget geometry_budget;
    for (const auto& entity : result.entities) {
        (void)text(entity.id, false);
        if (!entity_ids.insert(entity.id).second || entity.required ||
            !entity.properties.is_object() || !entity.extensions.is_object()) reject();
        const bool shared = entity.type == "boundary";
        const bool dxf = entity.type == "annotation_state";
        const bool ifc = entity.type == "wall" || entity.type == "slab" ||
            entity.type == "opening" || entity.type == "ifc_reference";
        if (!shared && !(result.kind == ProjectImportKind::dxf ? dxf : ifc)) reject();
        if (entity.properties.contains("parent_id") || entity.properties.contains("layer_id")) reject();
        if (entity.type == "boundary") {
            if (entity.properties.contains("boundary_model_version") ||
                !entity.properties.contains("boundary") ||
                !entity.properties.contains("classification") ||
                !entity.properties.at("classification").is_string()) reject();
            (void)text(entity.properties.at("classification"), false);
            (void)boundary(entity.properties.at("boundary"), false, geometry_budget);
        } else if (entity.type == "annotation_state") {
            try { validate_annotation_entity(entity); } catch (...) { reject(); }
        } else if (entity.type == "wall") {
            if (!walls.emplace(entity.id, wall(entity)).second) reject();
        } else if (entity.type == "slab") {
            validate_slab(entity, geometry_budget);
        } else if (entity.type == "opening") {
            std::string wall_id;
            auto hosted = opening(entity, wall_id);
            if (entity.properties.contains("opening_kind")) {
                const auto kind = text(entity.properties.at("opening_kind"), false);
                if (kind != "opening" && kind != "door" && kind != "window") reject();
            }
            openings.emplace_back(std::move(wall_id), std::move(hosted));
        } else if (entity.type == "ifc_reference") {
            validate_ifc_reference(entity);
        }
    }
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
    auto document = Document::create(result.entities);
    if (!document.snapshot().is_editable()) reject();
}
} // namespace project_import_detail

// Versioned bounded JSON is a native candidate protocol, never DXF/STEP text.
// Decoding rejects untrusted depth/counts before DOM allocation, then validates
// every candidate through Document. Callers still commit an ordinary command.
inline std::vector<std::byte> encode_project_import_candidate(const ProjectImportCandidate& result) {
    project_import_detail::validate(result);
    nlohmann::json entities = nlohmann::json::array(), diagnostics = nlohmann::json::array();
    for (const auto& e : result.entities)
        entities.push_back({{"id", e.id}, {"type", e.type}, {"properties", e.properties}, {"extensions", e.extensions}});
    for (const auto& d : result.diagnostics)
        diagnostics.push_back({{"source_id", d.source_id}, {"source_kind", d.source_kind}, {"code", d.code}});
    const auto wire = nlohmann::json{{"protocol", "PSIP0001"}, {"kind", project_import_kind_name(result.kind)},
        {"entities", std::move(entities)}, {"diagnostics", std::move(diagnostics)},
        {"source_retention_required", result.source_retention_required}}.dump();
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
        if (depth > 32 || ++nodes > 1'000'000) reject();
        if (event == nlohmann::json::parse_event_t::object_start) object_keys.emplace_back();
        if (event == nlohmann::json::parse_event_t::key &&
            (object_keys.empty() || !object_keys.back().insert(value.get<std::string>()).second)) reject();
        if (event == nlohmann::json::parse_event_t::object_end) object_keys.pop_back();
        if (value.is_string() && (value.get_ref<const std::string&>().size() > 1024 * 1024 ||
            value.get_ref<const std::string&>().find('\0') != std::string::npos)) reject();
        if (event == nlohmann::json::parse_event_t::value && value.is_number_float() &&
            !std::isfinite(value.get<double>())) reject();
        return true;
    };
    const auto* begin = reinterpret_cast<const char*>(report.output.data());
    const auto value = nlohmann::json::parse(begin, begin + report.output.size(), callback);
    fields(value, {"protocol", "kind", "entities", "diagnostics", "source_retention_required"});
    if (value.at("protocol") != "PSIP0001" || value.at("kind") != project_import_kind_name(expected_kind) ||
        !value.at("source_retention_required").is_boolean() || !value.at("entities").is_array() ||
        !value.at("diagnostics").is_array() || value.at("entities").size() > project_import_entity_limit ||
        value.at("diagnostics").size() > project_import_diagnostic_limit) reject();
    ProjectImportCandidate result;
    result.kind = expected_kind;
    result.source_retention_required = value.at("source_retention_required").get<bool>();
    for (const auto& e : value.at("entities")) {
        fields(e, {"id", "type", "properties", "extensions"});
        result.entities.push_back({text(e.at("id"), false), text(e.at("type"), false),
            e.at("properties"), false, e.at("extensions")});
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
    if (!report.controls_attested())
        throw std::runtime_error("Isolated project import is unavailable. Install or repair the bundled vertex-import-worker in a read-only application directory; the Windows sandbox must be available.");
    return decode_project_import_candidate(report, kind);
}
} // namespace sketch
