#include "sketch/document_schedule_adapter.hpp"

#include "sketch/geometry.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/door_operation.hpp"

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
    if(*opening_kind == "door" && entity.properties.contains("door_operation")) {
        const auto operation = decode_door_operation(entity.properties.at("door_operation"));
        record.properties.emplace("hinge", std::string(operation.hinge_at_end?"end":"start"));
        record.properties.emplace("swing_side", std::string(operation.swing_left?"left":"right"));
        record.properties.emplace("swing_angle_degrees", operation.angle_degrees);
    }
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
    if (area) record.properties.emplace("area_m2", ScheduleQuantity{*area, ScheduleUnit::square_metre});
    if (const auto name = text_field(entity, "name")) record.properties.emplace("name", *name);
    if (const auto boundary_id = text_field(entity, "boundary_id"))
        record.properties.emplace("boundary_id", *boundary_id);
    record.calculated.emplace("gross_area", ScheduleCalculation{
        ScheduleQuantity{area_value, ScheduleUnit::square_metre},
        {{entity.id, area ? "area_m2" : "boundary"}},
        area ? "Area from the document area_m2 property"
             : "Area calculated from the closed room boundary"});
    records.push_back(std::move(record));
}

void add_material(const Entity& entity, const DocumentSnapshot& document,
                  std::map<std::string, AssemblyModel>& catalogs, std::vector<ScheduleRecord>& records,
                  std::vector<std::string>& diagnostics) {
    if (entity.properties.contains("material_assignment")) {
        const auto& assignment = entity.properties.at("material_assignment");
        const auto catalog_id = assignment.at("catalog_id").get<std::string>();
        const auto material_id = assignment.at("material_id").get<std::string>();
        if (!catalogs.contains(catalog_id))
            catalogs.emplace(catalog_id, AssemblyModel::from_json(
                document.entities().at(catalog_id).properties.at("model")));
        const auto& materials = catalogs.at(catalog_id).materials();
        const auto material = std::find_if(materials.begin(), materials.end(),
            [&](const auto& candidate) { return candidate.id == material_id; });
        ScheduleRecord record;
        record.object_id = entity.id + ":material";
        record.kind = ScheduleRowKind::material;
        record.mark = mark_for(entity, "M-", diagnostics);
        record.properties.emplace("name", material->name);
        record.properties.emplace("count", std::int64_t{1});
        records.push_back(std::move(record));
        return;
    }
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

DocumentScheduleProjection project_schedules(
    const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>* visible_entity_ids) {
    DocumentScheduleProjection result;
    result.snapshot.revision = document.revision();
    std::vector<ScheduleRecord> records;
    std::map<std::string, AssemblyModel> catalogs;
    for (const auto& [id, entity] : document.entities()) {
        if (visible_entity_ids && !visible_entity_ids->contains(id)) continue;
        if (entity.type == "opening") add_opening(entity, records, result.diagnostics);
        else if (entity.type == "room" || entity.type == "room_boundary")
            add_room(entity, records, result.diagnostics);
        add_material(entity, document, catalogs, records, result.diagnostics);
    }
    try {
        result.snapshot = build_schedule(records, document.revision());
        // Stored room measurements are primitive provenance for gross_area,
        // but the schedule editor only authorizes room names and marks.
        for (auto& row : result.snapshot.rows) {
            if(row.kind == ScheduleRowKind::door) {
                for(const auto* key : {"hinge","swing_side","swing_angle_degrees"}) {
                    const auto cell = row.cells.find(key);
                    if(cell == row.cells.end()) continue;
                    cell->second.editable = false;
                    cell->second.sources = {{row.object_id,"door_operation"}};
                    cell->second.explanation = "Door operation relative to the host wall direction";
                }
            }
            if (row.kind == ScheduleRowKind::material) {
                constexpr std::string_view suffix = ":material";
                const auto source_id = row.object_id.substr(0, row.object_id.size() - suffix.size());
                const auto& source = document.entities().at(source_id);
                if (source.properties.contains("material_assignment")) {
                    const auto& assignment = source.properties.at("material_assignment");
                    auto& name = row.cells.at("name");
                    name.editable = false;
                    name.sources = {{source_id, "material_assignment"},
                        {assignment.at("catalog_id").get<std::string>(), "model"}};
                    name.explanation = "Assigned material name from the Assembly catalog";
                    auto& count = row.cells.at("count");
                    count.editable = false;
                    count.sources = {{source_id, "material_assignment"}};
                    count.explanation = "One object with this material assignment; not a volume takeoff";
                }
            }
            if (row.kind != ScheduleRowKind::room) continue;
            const auto source_area = row.cells.find("area_m2");
            if (source_area == row.cells.end()) continue;
            source_area->second.editable = false;
            source_area->second.explanation = "Stored room area from the document";
        }
    } catch (const std::exception& error) {
        result.diagnostics.push_back(std::string("schedule projection rejected: ") + error.what());
        result.snapshot = ScheduleSnapshot{document.revision(), {}};
    }
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    result.diagnostics.erase(std::unique(result.diagnostics.begin(), result.diagnostics.end()),
                             result.diagnostics.end());
    return result;
}

}  // namespace

DocumentScheduleProjection build_document_schedules(const DocumentSnapshot& document) {
    return project_schedules(document, nullptr);
}

DocumentScheduleProjection build_document_schedules(
    const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids) {
    return project_schedules(document, &visible_entity_ids);
}

ApplyEntityChanges make_document_schedule_edit(const DocumentSnapshot& document,
                                               const ScheduleEdit& edit) {
    if (edit.expected_revision != document.revision()) {
        throw DocumentError(DocumentErrorCode::stale_revision,
                            "Schedule edit was prepared from an older document revision");
    }

    const auto projection = build_document_schedules(document);
    // Re-run the schedule model validation against the captured row so a
    // hand-built edit cannot bypass calculated-cell or source checks.
    const auto validated = make_schedule_edit(projection.snapshot, edit.target.object_id,
                                              edit.target.property, edit.after);
    if (validated.before != edit.before) {
        throw std::invalid_argument("Schedule edit does not match the current source value");
    }

    std::string entity_id = edit.target.object_id;
    bool material_row = false;
    constexpr std::string_view material_suffix = ":material";
    if (entity_id.size() > material_suffix.size() &&
        entity_id.ends_with(material_suffix)) {
        material_row = true;
        entity_id.erase(entity_id.size() - material_suffix.size());
    }
    const auto entity = document.entities().find(entity_id);
    if (entity == document.entities().end())
        throw std::invalid_argument("Schedule source entity was not found");

    std::string property = edit.target.property;
    if (material_row) {
        if (entity->second.type.empty())
            throw std::invalid_argument("Material schedule source has no entity type");
        if (property == "name") property = "material_name";
        else if (property == "volume") property = "volume_m3";
        else if (property != "mark")
            throw std::invalid_argument("Material schedule property is not editable");
    } else if (entity->second.type == "opening") {
        if (property == "width" || property == "height" || property == "sill" ||
            property == "offset") {
            property += "_m";
        } else if (property != "mark" && property != "wall_id" && property != "description" &&
                   property != "fire_rated") {
            throw std::invalid_argument("Opening schedule property is not editable");
        }
    } else if (entity->second.type != "room" && entity->second.type != "room_boundary") {
        throw std::invalid_argument("Schedule source entity type is not editable through schedules");
    } else if (property != "mark" && property != "name") {
        throw std::invalid_argument("Room schedule property is not editable");
    }

    nlohmann::json replacement;
    if (const auto* string_value = std::get_if<std::string>(&edit.after)) replacement = *string_value;
    else if (const auto* bool_value = std::get_if<bool>(&edit.after)) replacement = *bool_value;
    else if (const auto* integer_value = std::get_if<std::int64_t>(&edit.after)) replacement = *integer_value;
    else if (const auto* double_value = std::get_if<double>(&edit.after)) replacement = *double_value;
    else if (const auto* quantity_value = std::get_if<ScheduleQuantity>(&edit.after))
        replacement = quantity_value->value;
    else
        throw std::invalid_argument("Unsupported schedule replacement value");

    auto updated = entity->second;
    updated.properties[property] = std::move(replacement);
    return ApplyEntityChanges{edit.expected_revision,
                              {EntityChange::upsert(std::move(updated))},
                              {},
                              "Edit schedule " + edit.target.object_id + "." + edit.target.property};
}

}  // namespace sketch
