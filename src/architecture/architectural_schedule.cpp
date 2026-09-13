#include "sketch/architectural_schedule.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"

#include <Standard_Failure.hxx>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <map>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {
namespace {

constexpr std::string_view material_suffix = ":material";
constexpr std::string_view layer_marker = ":layer:";

struct MaterialGroup {
    std::string key;
    std::string display_name;
    std::int64_t count{};
    double volume{};
    bool complete_volume{true};
    std::vector<ScheduleSourceRef> sources;
};

std::string normalized_material_name(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            if (!result.empty()) pending_space = true;
            continue;
        }
        if (pending_space) result.push_back(' ');
        result.push_back(static_cast<char>(std::tolower(byte)));
        pending_space = false;
    }
    return result;
}

std::string material_group_digest(std::string_view key) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(key.data()), key.size());
    return sha256_hex(bytes);
}

void append_sources(std::vector<ScheduleSourceRef>& destination,
                    const std::vector<ScheduleSourceRef>& sources) {
    destination.insert(destination.end(), sources.begin(), sources.end());
}

void normalize_sources(std::vector<ScheduleSourceRef>& sources) {
    std::sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
        return std::pair{left.object_id, left.property} <
               std::pair{right.object_id, right.property};
    });
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
}

std::string material_source_id(std::string_view object_id) {
    if (object_id.size() <= material_suffix.size() ||
        !object_id.ends_with(material_suffix)) {
        return {};
    }
    const auto stem = object_id.substr(0, object_id.size() - material_suffix.size());
    const auto marker = stem.find(layer_marker);
    return marker == std::string_view::npos ? std::string(stem) :
                                                std::string(stem.substr(0, marker));
}

bool is_layer_material_row(const ScheduleRow& row) {
    return row.cells.contains("layer_id") && row.kind == ScheduleRowKind::material;
}

void append_material_summaries(const DocumentSnapshot& document,
                               DocumentScheduleProjection& projection) {
    std::map<std::string, MaterialGroup, std::less<>> groups;
    for (const auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material ||
            row.object_id.size() <= material_suffix.size() ||
            !row.object_id.ends_with(material_suffix)) {
            continue;
        }
        const auto source_id = material_source_id(row.object_id);
        const auto source = document.entities().find(source_id);
        const auto name_cell = row.cells.find("name");
        if (source == document.entities().end() || name_cell == row.cells.end() ||
            !std::holds_alternative<std::string>(name_cell->second.value)) {
            projection.diagnostics.push_back(
                source_id + ": material summary source is incomplete");
            continue;
        }
        const auto display_name = std::get<std::string>(name_cell->second.value);
        std::string group_key;
        const auto assignment = source->second.properties.find("material_assignment");
        if (!is_layer_material_row(row) && assignment != source->second.properties.end() && assignment->is_object()) {
            const auto catalog = assignment->find("catalog_id");
            const auto material = assignment->find("material_id");
            if (catalog != assignment->end() && material != assignment->end() &&
                catalog->is_string() && material->is_string() &&
                !catalog->get<std::string>().empty() && !material->get<std::string>().empty()) {
                group_key = "assigned\n" + catalog->get<std::string>() + "\n" +
                            material->get<std::string>();
            }
        }
        if (group_key.empty()) {
            const auto catalog = row.cells.find("catalog_id");
            const auto material = row.cells.find("material_id");
            if (catalog != row.cells.end() && material != row.cells.end() &&
                std::holds_alternative<std::string>(catalog->second.value) &&
                std::holds_alternative<std::string>(material->second.value)) {
                const auto& catalog_id = std::get<std::string>(catalog->second.value);
                const auto& material_id = std::get<std::string>(material->second.value);
                if (!catalog_id.empty() && !material_id.empty()) {
                    group_key = "assigned\n" + catalog_id + "\n" + material_id;
                }
            }
        }
        if (group_key.empty()) {
            const auto normalized = normalized_material_name(display_name);
            if (normalized.empty()) continue;
            group_key = "explicit\n" + normalized;
        }
        auto [group, inserted] = groups.try_emplace(
            group_key, MaterialGroup{group_key, display_name});
        if (!inserted && group->second.display_name.empty() && !display_name.empty())
            group->second.display_name = display_name;
        auto& aggregate = group->second;
        const auto count = row.cells.find("count");
        if (count != row.cells.end() && std::holds_alternative<std::int64_t>(count->second.value))
            aggregate.count += std::get<std::int64_t>(count->second.value);
        else
            ++aggregate.count;
        append_sources(aggregate.sources, name_cell->second.sources);
        if (count != row.cells.end()) append_sources(aggregate.sources, count->second.sources);

        const auto volume = row.cells.find("volume");
        if (volume == row.cells.end()) {
            aggregate.complete_volume = false;
            continue;
        }
        if (!std::holds_alternative<ScheduleQuantity>(volume->second.value) ||
            std::get<ScheduleQuantity>(volume->second.value).unit != ScheduleUnit::cubic_metre ||
            !std::isfinite(std::get<ScheduleQuantity>(volume->second.value).value) ||
            std::get<ScheduleQuantity>(volume->second.value).value <= 0.0) {
            aggregate.complete_volume = false;
            continue;
        }
        aggregate.volume += std::get<ScheduleQuantity>(volume->second.value).value;
        append_sources(aggregate.sources, volume->second.sources);
    }

    for (auto& [key, aggregate] : groups) {
        normalize_sources(aggregate.sources);
        const auto digest = material_group_digest(key);
        ScheduleRow summary;
        summary.object_id = "material-summary:" + digest;
        summary.mark = "MS-" + digest;
        summary.kind = ScheduleRowKind::material_summary;
        const auto source_count = aggregate.count;
        const auto source_label = source_count == 1 ? "source object" : "source objects";
        summary.cells.emplace("mark", ScheduleCell{
            summary.mark, false, aggregate.sources,
            "Stable mark for this grouped material summary"});
        summary.cells.emplace("name", ScheduleCell{
            aggregate.display_name, false, aggregate.sources,
            "Grouped material identity/name across " + std::to_string(source_count) +
                " " + source_label});
        summary.cells.emplace("count", ScheduleCell{
            source_count, false, aggregate.sources,
            "Count of source objects contributing to this material summary"});
        if (aggregate.complete_volume) {
            summary.cells.emplace("volume", ScheduleCell{
                ScheduleQuantity{aggregate.volume, ScheduleUnit::cubic_metre}, false,
                aggregate.sources,
                "Net quantity is the sum of source solid volumes"});
        } else {
            projection.diagnostics.push_back(
                summary.object_id + ": grouped material volume is incomplete; "
                "every source must have a valid net volume");
        }
        projection.snapshot.rows.push_back(std::move(summary));
    }
}

void append_layer_material_rows(
    const DocumentSnapshot& document,
    const std::map<std::string, std::vector<const Entity*>, std::less<>>& openings,
    DocumentScheduleProjection& projection,
    const std::set<std::string, std::less<>>* visible_entity_ids) {
    static const std::vector<const Entity*> no_openings;
    std::map<std::string, AssemblyModel> catalogs;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "wall" ||
            (visible_entity_ids && !visible_entity_ids->contains(id)) ||
            !entity.properties.contains("layers")) {
            continue;
        }
        try {
            Wall wall;
            std::string error;
            const auto& wall_openings = openings.contains(id) ? openings.at(id) : no_openings;
            if (!read_document_wall(entity, wall_openings, wall, error)) {
                throw std::invalid_argument(error);
            }
            if (wall.layers.empty()) continue;
            for (const auto& layer : wall.layers) {
                if (!layer.material.has_value()) continue;
                const auto& assignment = *layer.material;
                if (!catalogs.contains(assignment.catalog_id)) {
                    const auto catalog = document.entities().find(assignment.catalog_id);
                    if (catalog == document.entities().end() || catalog->second.type != "assembly_model") {
                        throw std::invalid_argument("layer material catalog is missing");
                    }
                    const auto model = catalog->second.properties.find("model");
                    if (model == catalog->second.properties.end()) {
                        throw std::invalid_argument("layer material catalog has no model");
                    }
                    catalogs.emplace(assignment.catalog_id, AssemblyModel::from_json(model.value()));
                }
                const auto& materials = catalogs.at(assignment.catalog_id).materials();
                const auto material = std::find_if(materials.begin(), materials.end(),
                    [&](const auto& candidate) { return candidate.id == assignment.material_id; });
                if (material == materials.end()) {
                    throw std::invalid_argument("layer material is missing from its catalog");
                }

                ScheduleRow row;
                row.object_id = id + std::string(layer_marker) + layer.id +
                                std::string(material_suffix);
                row.mark = "M-" + id + "-" + layer.id;
                row.kind = ScheduleRowKind::material;
                const auto source_prefix = id + ":layers." + layer.id;
                row.cells.emplace("name", ScheduleCell{
                    material->name, false,
                    {{id, source_prefix + ".material_assignment"},
                     {assignment.catalog_id, "model"}},
                    "Assigned material name for this wall layer"});
                row.cells.emplace("count", ScheduleCell{
                    std::int64_t{1}, false, {{id, source_prefix}},
                    "One material layer instance"});
                row.cells.emplace("layer_id", ScheduleCell{
                    layer.id, false, {{id, "layers"}}, "Stable wall layer identity"});
                row.cells.emplace("thickness", ScheduleCell{
                    ScheduleQuantity{layer.thickness, ScheduleUnit::metre}, false,
                    {{id, source_prefix + ".thickness_m"}}, "Authored layer thickness"});
                row.cells.emplace("catalog_id", ScheduleCell{
                    assignment.catalog_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material catalog identity"});
                row.cells.emplace("material_id", ScheduleCell{
                    assignment.material_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material identity"});

                Wall homogeneous{id, wall.baseline, layer.thickness, wall.height,
                                wall.elevation, wall.openings};
                const auto shape = make_wall(homogeneous);
                const auto volume = solid_volume(shape);
                if (!std::isfinite(volume) || volume <= 0.0) {
                    throw std::invalid_argument("layer solid volume must be positive and finite");
                }
                std::vector<ScheduleSourceRef> volume_sources{{id, "geometry"}};
                for (const auto* opening : wall_openings) {
                    volume_sources.push_back({opening->id, "geometry"});
                }
                row.cells.emplace("volume", ScheduleCell{
                    ScheduleQuantity{volume, ScheduleUnit::cubic_metre}, false,
                    std::move(volume_sources),
                    "Net layer solid volume after hosted openings"});
                projection.snapshot.rows.push_back(std::move(row));
            }
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": layer material volume unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": layer material unavailable: " + error.what());
        }
    }
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "slab" ||
            (visible_entity_ids && !visible_entity_ids->contains(id)) ||
            !entity.properties.contains("layers")) {
            continue;
        }
        try {
            Slab slab;
            std::string error;
            if (!read_document_slab(entity, slab, error)) {
                throw std::invalid_argument(error);
            }
            if (slab.layers.empty()) continue;
            double layer_elevation = slab.elevation;
            for (const auto& layer : slab.layers) {
                if (!layer.material.has_value()) {
                    layer_elevation += layer.thickness;
                    continue;
                }
                const auto& assignment = *layer.material;
                if (!catalogs.contains(assignment.catalog_id)) {
                    const auto catalog = document.entities().find(assignment.catalog_id);
                    if (catalog == document.entities().end() || catalog->second.type != "assembly_model") {
                        throw std::invalid_argument("layer material catalog is missing");
                    }
                    const auto model = catalog->second.properties.find("model");
                    if (model == catalog->second.properties.end()) {
                        throw std::invalid_argument("layer material catalog has no model");
                    }
                    catalogs.emplace(assignment.catalog_id, AssemblyModel::from_json(model.value()));
                }
                const auto& materials = catalogs.at(assignment.catalog_id).materials();
                const auto material = std::find_if(materials.begin(), materials.end(),
                    [&](const auto& candidate) { return candidate.id == assignment.material_id; });
                if (material == materials.end()) {
                    throw std::invalid_argument("layer material is missing from its catalog");
                }

                ScheduleRow row;
                row.object_id = id + std::string(layer_marker) + layer.id +
                                std::string(material_suffix);
                row.mark = "M-" + id + "-" + layer.id;
                row.kind = ScheduleRowKind::material;
                const auto source_prefix = id + ":layers." + layer.id;
                row.cells.emplace("name", ScheduleCell{
                    material->name, false,
                    {{id, source_prefix + ".material_assignment"},
                     {assignment.catalog_id, "model"}},
                    "Assigned material name for this slab layer"});
                row.cells.emplace("count", ScheduleCell{
                    std::int64_t{1}, false, {{id, source_prefix}},
                    "One material layer instance"});
                row.cells.emplace("layer_id", ScheduleCell{
                    layer.id, false, {{id, "layers"}}, "Stable slab layer identity"});
                row.cells.emplace("thickness", ScheduleCell{
                    ScheduleQuantity{layer.thickness, ScheduleUnit::metre}, false,
                    {{id, source_prefix + ".thickness_m"}}, "Authored layer thickness"});
                row.cells.emplace("catalog_id", ScheduleCell{
                    assignment.catalog_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material catalog identity"});
                row.cells.emplace("material_id", ScheduleCell{
                    assignment.material_id, false,
                    {{id, source_prefix + ".material_assignment"}},
                    "Layer material identity"});

                Slab homogeneous{id, slab.boundary, slab.holes, layer.thickness,
                                layer_elevation, slab.element_kind, {}};
                const auto shape = make_slab(homogeneous);
                const auto volume = solid_volume(shape);
                if (!std::isfinite(volume) || volume <= 0.0) {
                    throw std::invalid_argument("layer solid volume must be positive and finite");
                }
                row.cells.emplace("volume", ScheduleCell{
                    ScheduleQuantity{volume, ScheduleUnit::cubic_metre}, false,
                    {{id, "geometry"}},
                    "Net slab layer solid volume after openings"});
                projection.snapshot.rows.push_back(std::move(row));
                layer_elevation += layer.thickness;
            }
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": layer material volume unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": layer material unavailable: " + error.what());
        }
    }
}

ScheduleValue assembly_quantity_value(const AssemblyQuantityProperty& quantity) {
    switch (quantity.unit) {
    case AssemblyQuantityUnit::count:
        if (quantity.value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("assembly count exceeds schedule integer range");
        }
        return ScheduleValue{static_cast<std::int64_t>(quantity.value)};
    case AssemblyQuantityUnit::metre:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::metre}};
    case AssemblyQuantityUnit::square_metre:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::square_metre}};
    case AssemblyQuantityUnit::cubic_metre:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::cubic_metre}};
    case AssemblyQuantityUnit::kilogram:
        return ScheduleValue{ScheduleQuantity{quantity.value, ScheduleUnit::kilogram}};
    }
    throw std::invalid_argument("unknown assembly quantity unit");
}

void append_assembly_rows(const DocumentSnapshot& document,
                          DocumentScheduleProjection& projection,
                          const std::set<std::string, std::less<>>* visible_entity_ids) {
    std::vector<ScheduleRecord> records;
    for (const auto& [catalog_id, entity] : document.entities()) {
        if (entity.type != "assembly_model" || !entity.properties.contains("model")) continue;
        try {
            const auto model = AssemblyModel::from_json(entity.properties.at("model"));
            for (const auto& instance : model.instances()) {
                if (visible_entity_ids) {
                    // A placed instance follows its host visibility. Unplaced
                    // catalog entries are scoped by the catalog entity itself.
                    if (instance.placement) {
                        if (!visible_entity_ids->contains(instance.placement->host_entity_id)) continue;
                    } else if (!visible_entity_ids->contains(catalog_id)) {
                        continue;
                    }
                }
                const auto resolved = model.resolve(instance.id);
                const auto type = std::find_if(model.types().begin(), model.types().end(),
                    [&](const auto& candidate) { return candidate.id == resolved.type_id; });
                if (type == model.types().end()) {
                    projection.diagnostics.push_back(catalog_id + ": assembly instance type is missing");
                    continue;
                }
                ScheduleRecord record;
                record.object_id = catalog_id + ":instance:" + instance.id;
                record.mark = "A-" + catalog_id + "-" + instance.id;
                record.kind = ScheduleRowKind::assembly;
                record.properties.emplace("catalog_id", catalog_id);
                record.properties.emplace("instance_id", instance.id);
                record.properties.emplace("type_id", resolved.type_id);
                record.properties.emplace("name", type->name);
                if (instance.placement) {
                    record.properties.emplace("host_entity_id", instance.placement->host_entity_id);
                    record.properties.emplace("rotation_radians", instance.placement->rotation_radians);
                    record.properties.emplace("scale", instance.placement->scale);
                    record.properties.emplace("translation_x_m",
                                              ScheduleQuantity{instance.placement->translation_m.x,
                                                               ScheduleUnit::metre});
                    record.properties.emplace("translation_y_m",
                                              ScheduleQuantity{instance.placement->translation_m.y,
                                                               ScheduleUnit::metre});
                }
                for (const auto& [key, quantity] : resolved.quantities) {
                    record.properties.emplace("quantity:" + key, assembly_quantity_value(quantity));
                }
                for (const auto& [slot, material_id] : resolved.materials)
                    record.properties.emplace("material:" + slot, material_id);
                records.push_back(std::move(record));
            }
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(catalog_id + ": assembly schedule unavailable: " + error.what());
        }
    }
    if (records.empty()) return;
    try {
        auto rows = build_schedule(records, document.revision()).rows;
        for (auto& row : rows) {
            for (auto& [name, cell] : row.cells) {
                cell.editable = false;
                cell.sources = {{row.object_id, name}};
                cell.explanation = "Resolved reusable assembly instance data from its catalog";
            }
            projection.snapshot.rows.push_back(std::move(row));
        }
    } catch (const std::exception& error) {
        projection.diagnostics.push_back(std::string("assembly schedule rejected: ") + error.what());
    }
}

void add_building_quantity(ScheduleRecord& record, const Entity& entity,
                           std::string_view source_name, std::string_view cell_name,
                           ScheduleUnit unit) {
    const auto found = entity.properties.find(std::string(source_name));
    if (found == entity.properties.end() || !found->is_number()) return;
    const auto value = found->get<double>();
    if (!std::isfinite(value)) return;
    record.properties.emplace(std::string(cell_name), ScheduleQuantity{value, unit});
}

std::optional<std::string> building_text_field(const Entity& entity, std::string_view name) {
    if (!entity.properties.is_object()) return std::nullopt;
    const auto found = entity.properties.find(std::string(name));
    if (found == entity.properties.end() || !found->is_string()) return std::nullopt;
    const auto value = found->get<std::string>();
    return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

std::string building_mark_for(const Entity& entity, std::string_view prefix,
                              std::vector<std::string>& diagnostics) {
    if (const auto mark = building_text_field(entity, "mark")) return *mark;
    diagnostics.push_back(entity.type + " " + entity.id +
                          ": mark is absent; the stable entity ID is used as the deterministic mark");
    return std::string(prefix) + entity.id;
}

void add_building_scalar(ScheduleRecord& record, const Entity& entity,
                         std::string_view source_name, std::string_view cell_name) {
    const auto found = entity.properties.find(std::string(source_name));
    if (found == entity.properties.end() || !found->is_number()) return;
    const auto value = found->get<double>();
    if (std::isfinite(value)) record.properties.emplace(std::string(cell_name), value);
}

void append_building_rows(const DocumentSnapshot& document,
                          DocumentScheduleProjection& projection,
                          const std::set<std::string, std::less<>>* visible_entity_ids) {
    std::vector<ScheduleRecord> records;
    for (const auto& [id, entity] : document.entities()) {
        if (!can_recognize_building_entity_type(entity.type) ||
            (visible_entity_ids && !visible_entity_ids->contains(id))) {
            continue;
        }
        try {
            const auto object = decode_building_entity(entity);
            ScheduleRecord record;
            record.object_id = id;
            record.kind = ScheduleRowKind::building;
            record.mark = building_mark_for(entity, "B-", projection.diagnostics);
            record.properties.emplace("type", entity.type);
            if (const auto form = building_text_field(entity, "form"))
                record.properties.emplace("form", *form);
            add_building_quantity(record, entity, "width_m", "width", ScheduleUnit::metre);
            add_building_quantity(record, entity, "depth_m", "depth", ScheduleUnit::metre);
            add_building_quantity(record, entity, "height_m", "height", ScheduleUnit::metre);
            add_building_quantity(record, entity, "radius_m", "radius", ScheduleUnit::metre);
            add_building_quantity(record, entity, "length_m", "length", ScheduleUnit::metre);
            add_building_quantity(record, entity, "run_m", "run", ScheduleUnit::metre);
            add_building_quantity(record, entity, "span_m", "span", ScheduleUnit::metre);
            add_building_quantity(record, entity, "rise_m", "rise", ScheduleUnit::metre);
            add_building_quantity(record, entity, "overhang_m", "overhang", ScheduleUnit::metre);
            add_building_quantity(record, entity, "thickness_m", "thickness", ScheduleUnit::metre);
            add_building_quantity(record, entity, "going_m", "going", ScheduleUnit::metre);
            add_building_quantity(record, entity, "post_spacing_m", "post_spacing", ScheduleUnit::metre);
            add_building_scalar(record, entity, "pitch_rad", "pitch_radians");
            add_building_scalar(record, entity, "rotation_rad", "rotation_radians");
            add_building_scalar(record, entity, "orientation_rad", "orientation_radians");
            if (const auto found = entity.properties.find("riser_count");
                found != entity.properties.end() && found->is_number_integer()) {
                record.properties.emplace("riser_count", found->get<std::int64_t>());
            }
            if (const auto found = entity.properties.find("roof_openings");
                found != entity.properties.end() && found->is_array()) {
                record.properties.emplace("opening_count",
                                          static_cast<std::int64_t>(found->size()));
            }
            if (const auto found = entity.properties.find("level_connection");
                found != entity.properties.end() && found->is_object()) {
                static constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
                    connection_fields{{
                        {"graph_id", "level_graph_id"},
                        {"link_id", "level_link_id"},
                        {"lower_level_id", "lower_level_id"},
                        {"upper_level_id", "upper_level_id"},
                    }};
                for (const auto [source_name, cell_name] : connection_fields) {
                    const auto value = found->find(std::string(source_name));
                    if (value != found->end() && value->is_string() &&
                        !value->get<std::string>().empty()) {
                        record.properties.emplace(std::string(cell_name),
                                                  value->get<std::string>());
                    }
                }
            }
            if (const auto* beam = std::get_if<Beam>(&object)) {
                const auto dx = beam->end.x - beam->start.x;
                const auto dy = beam->end.y - beam->start.y;
                const auto dz = beam->end.z - beam->start.z;
                const auto length = std::hypot(std::hypot(dx, dy), dz);
                if (std::isfinite(length))
                    record.properties.emplace("length", ScheduleQuantity{length, ScheduleUnit::metre});
            }
            const auto shape = make_building_shape(object);
            const auto volume = solid_volume(shape);
            if (!std::isfinite(volume) || volume <= 0.0)
                throw std::invalid_argument("building solid volume must be positive and finite");
            record.calculated.emplace("volume", ScheduleCalculation{
                ScheduleQuantity{volume, ScheduleUnit::cubic_metre},
                {{id, "geometry"}},
                "Net volume calculated from the canonical building solid"});
            records.push_back(std::move(record));
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": building schedule unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": building schedule unavailable: " + error.what());
        }
    }
    if (records.empty()) return;
    try {
        auto rows = build_schedule(records, document.revision()).rows;
        for (auto& row : rows) {
            for (auto& [name, cell] : row.cells) {
                cell.editable = false;
                cell.explanation = name == "volume"
                    ? "Derived from the canonical building solid"
                    : "Authored building-object property; edit the object in the architectural workspace";
            }
            projection.snapshot.rows.push_back(std::move(row));
        }
    } catch (const std::exception& error) {
        projection.diagnostics.push_back(std::string("building schedule rejected: ") + error.what());
    }
}

DocumentScheduleProjection augment(const DocumentSnapshot& document, DocumentScheduleProjection projection,
                                   const std::set<std::string, std::less<>>* visible_entity_ids) {
    std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "opening") continue;
        std::string host, error;
        if (read_document_wall_id(entity, host, error)) openings[host].push_back(&entity);
    }
    for (auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material) continue;
        if (row.object_id.size() <= material_suffix.size() ||
            !row.object_id.ends_with(material_suffix)) continue;
        if (is_layer_material_row(row)) continue;
        const auto id = material_source_id(row.object_id);
        const auto entity_it = document.entities().find(id);
        if (entity_it == document.entities().end()) {
            projection.diagnostics.push_back(id + ": material source entity was not found");
            continue;
        }
        const auto& entity = entity_it->second;
        if (!entity.properties.contains("material_assignment")) continue;
        try {
            TopoDS_Shape shape;
            std::string error;
            std::vector<ScheduleSourceRef> sources{{id, "material_assignment"}, {id, "geometry"}};
            if (entity.type == "wall") {
                Wall wall;
                if (!read_document_wall(entity, openings[id], wall, error)) throw std::invalid_argument(error);
                shape = make_wall(wall);
                for (const auto* opening : openings[id]) sources.push_back({opening->id, "geometry"});
            } else if (entity.type == "slab") {
                Slab slab;
                if (!read_document_slab(entity, slab, error)) throw std::invalid_argument(error);
                shape = make_slab(slab);
            } else if (can_recognize_building_entity_type(entity.type)) {
                shape = make_building_shape(decode_building_entity(entity));
            } else {
                throw std::invalid_argument("object has no material solid representation");
            }
            const auto volume = solid_volume(shape);
            if (!std::isfinite(volume) || volume <= 0) throw std::invalid_argument("solid volume must be positive and finite");
            row.cells.emplace("volume", ScheduleCell{ScheduleQuantity{volume, ScheduleUnit::cubic_metre},
                false, std::move(sources), "Net solid volume after openings; one homogeneous assigned material"});
        } catch (const Standard_Failure& error) {
            projection.diagnostics.push_back(id + ": material volume unavailable: " +
                (error.what() ? error.what() : "solid construction failed"));
        } catch (const std::exception& error) {
            projection.diagnostics.push_back(id + ": material volume unavailable: " + error.what());
        }
    }
    append_layer_material_rows(document, openings, projection, visible_entity_ids);
    append_assembly_rows(document, projection, visible_entity_ids);
    append_building_rows(document, projection, visible_entity_ids);
    append_material_summaries(document, projection);
    std::sort(projection.diagnostics.begin(), projection.diagnostics.end());
    projection.diagnostics.erase(std::unique(projection.diagnostics.begin(), projection.diagnostics.end()),
        projection.diagnostics.end());
    return projection;
}
} // namespace

DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document) {
    return augment(document, build_document_schedules(document), nullptr);
}
DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids) {
    return augment(document, build_document_schedules(document, visible_entity_ids),
                   &visible_entity_ids);
}
} // namespace sketch
