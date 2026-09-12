#include "sketch/architectural_schedule.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"

#include <Standard_Failure.hxx>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sketch {
namespace {
DocumentScheduleProjection augment(const DocumentSnapshot& document, DocumentScheduleProjection projection) {
    std::map<std::string, std::vector<const Entity*>> openings;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.type != "opening") continue;
        std::string host, error;
        if (read_document_wall_id(entity, host, error)) openings[host].push_back(&entity);
    }
    for (auto& row : projection.snapshot.rows) {
        if (row.kind != ScheduleRowKind::material) continue;
        constexpr std::string_view suffix = ":material";
        const auto id = row.object_id.substr(0, row.object_id.size() - suffix.size());
        const auto& entity = document.entities().at(id);
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
