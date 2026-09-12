#include "sketch/architectural_schedule.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"

#include <Standard_Failure.hxx>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace sketch {
namespace {

constexpr std::string_view material_suffix = ":material";

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

void append_material_summaries(const DocumentSnapshot& document,
                               DocumentScheduleProjection& projection) {
    std::map<std::string, MaterialGroup, std::less<>> groups;
    for (const auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material ||
            row.object_id.size() <= material_suffix.size() ||
            !row.object_id.ends_with(material_suffix)) {
            continue;
        }
        const auto source_id = row.object_id.substr(
            0, row.object_id.size() - material_suffix.size());
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
        if (assignment != source->second.properties.end() && assignment->is_object()) {
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

DocumentScheduleProjection augment(const DocumentSnapshot& document, DocumentScheduleProjection projection) {
    std::map<std::string, std::vector<const Entity*>> openings;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "opening") continue;
        std::string host, error;
        if (read_document_wall_id(entity, host, error)) openings[host].push_back(&entity);
    }
    for (auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material) continue;
        if (row.object_id.size() <= material_suffix.size() ||
            !row.object_id.ends_with(material_suffix)) continue;
        const auto id = row.object_id.substr(0, row.object_id.size() - material_suffix.size());
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
    append_material_summaries(document, projection);
    std::sort(projection.diagnostics.begin(), projection.diagnostics.end());
    projection.diagnostics.erase(std::unique(projection.diagnostics.begin(), projection.diagnostics.end()),
        projection.diagnostics.end());
    return projection;
}
} // namespace

DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document) {
    return augment(document, build_document_schedules(document));
}
DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids) {
    return augment(document, build_document_schedules(document, visible_entity_ids));
}
} // namespace sketch
