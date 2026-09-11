#include "sketch/document_schedule_adapter.hpp"

#include "sketch/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {

using Json = nlohmann::json;

void diagnostic(std::vector<std::string>& result, const Entity& entity,
                std::string message) {
    result.push_back(entity.type + " " + entity.id + ": " + std::move(message));
}

const Json* field(const Entity& entity, std::string_view name) {
    if (!entity.properties.is_object()) return nullptr;
    const auto found = entity.properties.find(std::string(name));
    return found == entity.properties.end() ? nullptr : &found.value();
}

std::optional<std::string> text_field(const Entity& entity, std::string_view name) {
    const auto* value = field(entity, name);
    if (value == nullptr || !value->is_string()) return std::nullopt;
    const auto result = value->get<std::string>();
    return result.empty() ? std::nullopt : std::optional<std::string>(result);
}

std::optional<double> finite_field(const Entity& entity, std::string_view name) {
    const auto* value = field(entity, name);
    if (value == nullptr || !value->is_number()) return std::nullopt;
    const double result = value->get<double>();
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

std::optional<Boundary> boundary_field(const Entity& entity, std::string_view name) {
    const auto* value = field(entity, name);
    if (value == nullptr || !value->is_array()) return std::nullopt;
    Boundary result;
    try {
        for (const auto& edge : *value) {
            if (!edge.is_object() || !edge.contains("start") || !edge.contains("end") ||
                !edge.contains("sweep_radians") || !edge.at("start").is_array() ||
                !edge.at("end").is_array() || edge.at("start").size() != 2 ||
                edge.at("end").size() != 2 || !edge.at("start")[0].is_number() ||
                !edge.at("start")[1].is_number() || !edge.at("end")[0].is_number() ||
                !edge.at("end")[1].is_number() || !edge.at("sweep_radians").is_number()) {
                return std::nullopt;
            }
            const Segment segment{
                {edge.at("start")[0].get<double>(), edge.at("start")[1].get<double>()},
                {edge.at("end")[0].get<double>(), edge.at("end")[1].get<double>()},
                edge.at("sweep_radians").get<double>()};
            if (!std::isfinite(segment.start.x) || !std::isfinite(segment.start.y) ||
                !std::isfinite(segment.end.x) || !std::isfinite(segment.end.y) ||
                !std::isfinite(segment.sweep_radians)) {
                return std::nullopt;
            }
            result.push_back(segment);
        }
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    return result;
}

std::string mark_for(const Entity& entity, std::string_view prefix,
                     std::vector<std::string>& diagnostics) {
    if (const auto mark = text_field(entity, "mark")) return *mark;
    diagnostic(diagnostics, entity,
               "mark is absent; the stable entity ID is used as the deterministic mark");
    return std::string(prefix) + entity.id;
}

bool positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

void add_opening(const Entity& entity, std::vector<ScheduleRecord>& records,
                 std::vector<std::string>& diagnostics) {
    const auto opening_kind = text_field(entity, "opening_kind");
    if (!opening_kind || (*opening_kind != "door" && *opening_kind != "window")) {
        diagnostic(diagnostics, entity, "opening_kind must be door or window");
        return;
    }
    const auto width = finite_field(entity, "width_m");
    const auto height = finite_field(entity, "height_m");
    if (!width || !height || !positive(*width) || !positive(*height)) {
        diagnostic(diagnostics, entity, "width_m and height_m must be finite and positive");
        return;
    }
    ScheduleRecord record;
    record.object_id = entity.id;
    record.kind = *opening_kind == "door" ? ScheduleRowKind::door : ScheduleRowKind::window;
    record.mark = mark_for(entity, record.kind == ScheduleRowKind::door ? "D-" : "W-",
                           diagnostics);
    record.properties.emplace("width", ScheduleQuantity{*width, ScheduleUnit::metre});
    record.properties.emplace("height", ScheduleQuantity{*height, ScheduleUnit::metre});
    for (const auto [name, unit] : {std::pair{"sill", ScheduleUnit::metre},
                                    std::pair{"offset", ScheduleUnit::metre}}) {
        if (const auto value = finite_field(entity, std::string(name) + "_m")) {
            if (*value < 0.0) {
                diagnostic(diagnostics, entity, std::string(name) + "_m must be nonnegative");
                return;
            }
            record.properties.emplace(name, ScheduleQuantity{*value, unit});
        }
    }
    if (const auto wall = text_field(entity, "wall_id")) record.properties.emplace("wall_id", *wall);
    if (const auto description = text_field(entity, "description"))
        record.properties.emplace("description", *description);
    if (const auto fire_rated = field(entity, "fire_rated"); fire_rated && fire_rated->is_boolean())
        record.properties.emplace("fire_rated", fire_rated->get<bool>());
    record.calculated.emplace("area", ScheduleCalculation{
        ScheduleQuantity{*width * *height, ScheduleUnit::square_metre},
        {{entity.id, "width"}, {entity.id, "height"}}, "Width multiplied by height"});
    records.push_back(std::move(record));
}

void add_room(const Entity& entity, std::vector<ScheduleRecord>& records,
              std::vector<std::string>& diagnostics) {
    const auto area = finite_field(entity, "area_m2");
    const auto boundary = area ? std::optional<Boundary>{} : boundary_field(entity, "boundary");
    double area_value = area.value_or(0.0);
    if (!area && boundary) {
        const auto issues = validate_boundary(*boundary);
        if (!issues.empty()) {
            diagnostic(diagnostics, entity, "boundary is invalid: " + issues.front().message);
            return;
        }
        area_value = std::abs(signed_area(*boundary));
    }
    if (!positive(area_value)) {
        diagnostic(diagnostics, entity, "area_m2 or a valid closed boundary is required");
        return;
    }
    ScheduleRecord record;
    record.object_id = entity.id;
    record.kind = ScheduleRowKind::room;
    record.mark = mark_for(entity, "R-", diagnostics);
    if (const auto name = text_field(entity, "name")) record.properties.emplace("name", *name);
    if (const auto boundary_id = text_field(entity, "boundary_id"))
        record.properties.emplace("boundary_id", *boundary_id);
    record.properties.emplace("gross_area", ScheduleQuantity{area_value, ScheduleUnit::square_metre});
    records.push_back(std::move(record));
}

void add_material(const Entity& entity, std::vector<ScheduleRecord>& records,
                  std::vector<std::string>& diagnostics) {
    const auto material = text_field(entity, "material_name");
    const auto volume = finite_field(entity, "volume_m3");
    if (!material && !volume) return;
    if (!material || !volume || !positive(*volume)) {
        diagnostic(diagnostics, entity, "material_name and positive volume_m3 are required");
        return;
    }
    ScheduleRecord record;
    record.object_id = entity.id + ":material";
    record.kind = ScheduleRowKind::material;
    record.mark = mark_for(entity, "M-", diagnostics);
    record.properties.emplace("name", *material);
    record.properties.emplace("volume", ScheduleQuantity{*volume, ScheduleUnit::cubic_metre});
    records.push_back(std::move(record));
}

}  // namespace

DocumentScheduleProjection build_document_schedules(const DocumentSnapshot& document) {
    DocumentScheduleProjection result;
    result.snapshot.revision = document.revision();
    std::vector<ScheduleRecord> records;
    for (const auto& [id, entity] : document.entities()) {
        (void)id;
        if (entity.type == "opening") add_opening(entity, records, result.diagnostics);
        else if (entity.type == "room" || entity.type == "room_boundary")
            add_room(entity, records, result.diagnostics);
        add_material(entity, records, result.diagnostics);
    }
    try {
        result.snapshot = build_schedule(records, document.revision());
    } catch (const std::exception& error) {
        result.diagnostics.push_back(std::string("schedule projection rejected: ") + error.what());
        result.snapshot = ScheduleSnapshot{document.revision(), {}};
    }
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    result.diagnostics.erase(std::unique(result.diagnostics.begin(), result.diagnostics.end()),
                             result.diagnostics.end());
    return result;
}

}  // namespace sketch

