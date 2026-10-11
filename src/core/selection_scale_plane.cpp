#include "sketch/selection_scale_plane.hpp"

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document_wall.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
using Ids = std::set<std::string, std::less<>>;
using ContextKey = std::tuple<std::string, std::string, std::string, std::string>;
using PlaneKey = std::tuple<ContextKey, SiteFrameMode, std::string, std::string>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument("Selection scaling plane: " + message);
}
const Entity& actual(const SiteFrameEntities& entities, const std::string& id) {
    const auto found = entities.find(id);
    require(found != entities.end() && found->first == found->second.id,
        "missing or inconsistent actual source '" + id + "'.");
    return found->second;
}
ContextKey context_key(const DrawingContext& context) {
    return {context.property_id, context.building_id, context.floor_id, context.level_id};
}
bool explicit_context(const Entity& entity) {
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "level_id", "wall_id"})
        if (entity.properties.contains(key)) return true;
    if (entity.type == "railing" &&
        (entity.properties.contains("host") || entity.properties.contains("landing_host"))) return true;
    return false;
}
DrawingContext qualified_context(const Entity& entity, const ProjectOrganization& organization,
    const SitePresentationPlacement& placement) {
    const auto require_declared_level = [&](const DrawingContext& context) {
        const auto declared = entity.properties.find("level_id");
        if (declared == entity.properties.end()) return;
        require(declared->is_string() && declared->get<std::string>() == context.level_id,
            "declared level contradicts the actual floor binding for '" + entity.id + "'.");
    };
    if (const auto context = organization.drawing_context(entity.id)) {
        require(context_key(*context) == context_key(placement.drawing_context),
            "organization and physical placement disagree for '" + entity.id + "'.");
        require_declared_level(*context);
        return *context;
    }
    // Site resolution explicitly inherits a hosted railing's real stair pose
    // and validates its own redundant references. No arbitrary parent/target
    // relationship can confer a physical floor here.
    if (entity.type == "railing" && entity.properties.contains("host") &&
        placement.drawing_context.complete()) {
        require_declared_level(placement.drawing_context);
        return placement.drawing_context;
    }
    require(!explicit_context(entity), "unresolved explicit drawing context for '" + entity.id + "'.");
    require(placement.drawing_context.property_id.empty() &&
        placement.drawing_context.building_id.empty() && placement.drawing_context.floor_id.empty() &&
        placement.drawing_context.layer_id.empty() && placement.drawing_context.level_id.empty() &&
        placement.source_frame == SiteFrameIdentity{},
        "source '" + entity.id + "' has no qualified physical floor context.");
    return {};
}
PlaneKey plane_key(const DrawingContext& context, const SitePresentationPlacement& placement,
    SelectionScalePlaneDomain domain) {
    if (domain == SelectionScalePlaneDomain::presentation_world)
        return {context_key(context), SiteFrameMode::world, {}, {}};
    return {context_key(context), placement.source_frame.mode,
        placement.source_frame.property_id, placement.source_frame.building_id};
}
struct WallPlane {
    double comparison_z_m{};
    bool conflicting{};
    Ids wall_ids;
    Ids dependencies;
};
struct FloorDatum {
    std::optional<VerticalLevelBinding> binding;
    double elevation_m{};
};
FloorDatum floor_datum(const SiteFrameEntities& entities, const DrawingContext& context) {
    const auto& floor = actual(entities, context.floor_id);
    require(floor.type == "floor" && floor.properties.is_object(), "invalid actual floor '" + floor.id + "'.");
    const auto encoded = floor.properties.find("vertical_level_binding");
    if (encoded == floor.properties.end()) {
        require(context.level_id.empty(), "unbound floor has a conflicting resolved level.");
        return {};
    }
    const auto binding = VerticalLevelBinding::from_json(*encoded);
    require(binding.level_id == context.level_id, "floor binding differs from its resolved context.");
    const auto& graph = actual(entities, binding.graph_entity_id);
    require(graph.type == "vertical_levels" && graph.properties.is_object() && graph.properties.contains("model"),
        "bound floor references an invalid actual vertical level graph.");
    const auto levels = VerticalLevelGraph::from_json(graph.properties.at("model"));
    const auto level = std::find_if(levels.levels().begin(), levels.levels().end(),
        [&](const auto& candidate) { return candidate.id == binding.level_id; });
    require(level != levels.levels().end(), "bound floor references a missing actual level.");
    require(std::isfinite(level->elevation_m), "floor datum is not finite.");
    return {binding, level->elevation_m};
}
} // namespace

std::map<std::string, SelectionScalePlane, std::less<>> resolve_selection_scale_planes(
    const SiteFrameEntities& entities, std::span<const std::string> architectural_target_ids,
    std::span<const std::string> selected_wall_ids, SelectionScalePlaneDomain domain) {
    require(domain == SelectionScalePlaneDomain::source_local ||
        domain == SelectionScalePlaneDomain::presentation_world, "unknown gesture coordinate domain.");
    require(architectural_target_ids.size() <= 1000 && selected_wall_ids.size() <= 1000,
        "target count exceeds 1000.");
    Ids all, targets, walls;
    for (const auto& id : architectural_target_ids) {
        const auto& entity = actual(entities, id);
        require(can_transform_architectural_entity_type(entity.type), "unsupported architectural target '" + id + "'.");
        require(targets.insert(id).second, "duplicate architectural target '" + id + "'.");
        all.insert(id);
    }
    for (const auto& id : selected_wall_ids) {
        require(actual(entities, id).type == "wall", "selected wall ID is not an actual wall.");
        require(walls.insert(id).second, "duplicate selected wall '" + id + "'.");
        all.insert(id);
    }
    require(all.size() <= 1000, "combined target count exceeds 1000.");
    if (targets.empty()) return {};

    const std::vector<std::string> ids(all.begin(), all.end());
    const auto organization = organize_project(entities);
    const auto placements = resolve_site_presentations(entities, ids);
    // Also validates explicit absolute/level contracts, without changing raw
    // source coordinates or using the object's own offset as a floor datum.
    const auto effective = resolve_vertical_placements(entities, ids);
    std::map<std::string, DrawingContext, std::less<>> contexts;
    std::map<std::string, FloorDatum, std::less<>> floors;
    for (const auto& id : ids) {
        auto context = qualified_context(actual(entities, id), organization, placements.at(id));
        if (!context.floor_id.empty() && !floors.contains(context.floor_id))
            floors.emplace(context.floor_id, floor_datum(entities, context));
        contexts.emplace(id, std::move(context));
    }
    std::vector<std::string> bound_floor_ids;
    for (const auto& [id, datum] : floors)
        if (datum.binding) bound_floor_ids.push_back(id);
    const auto floor_placements = resolve_site_presentations(entities, bound_floor_ids);

    std::map<PlaneKey, WallPlane> wall_planes;
    for (const auto& id : walls) {
        Wall wall;
        std::string diagnostic;
        const bool decoded = read_document_wall(effective.at(id), {}, wall, diagnostic);
        require(decoded, "selected wall '" + id + "' has no valid effective base: " + diagnostic);
        const auto& placement = placements.at(id);
        const double base = wall.elevation + (domain == SelectionScalePlaneDomain::presentation_world
            ? placement.forward.translation_m.z : 0.0);
        require(std::isfinite(base), "selected wall base is not finite.");
        const auto key = plane_key(contexts.at(id), placement, domain);
        const auto [found, inserted] = wall_planes.try_emplace(key, WallPlane{base, false, {}, {}});
        if (!inserted && found->second.comparison_z_m != base) found->second.conflicting = true;
        found->second.wall_ids.insert(id);
        found->second.dependencies.insert(placement.dependency_ids.begin(), placement.dependency_ids.end());
    }

    std::map<std::string, SelectionScalePlane, std::less<>> result;
    for (const auto& id : targets) {
        const auto& placement = placements.at(id);
        const auto& context = contexts.at(id);
        SelectionScalePlane plane;
        plane.drawing_context = context;
        plane.source_frame = placement.source_frame;
        plane.source_to_world = placement.forward;
        Ids dependencies(placement.dependency_ids.begin(), placement.dependency_ids.end());
        dependencies.insert(id);
        if (!context.floor_id.empty()) plane.floor_binding = floors.at(context.floor_id).binding;
        const auto key = plane_key(context, placement, domain);
        if (const auto selected = wall_planes.find(key); selected != wall_planes.end()) {
            require(!selected->second.conflicting,
                "selected walls have conflicting effective base elevations on this floor.");
            plane.origin = SelectionScalePlaneOrigin::selected_wall;
            plane.effective_source_z_m = selected->second.comparison_z_m -
                (domain == SelectionScalePlaneDomain::presentation_world ? placement.forward.translation_m.z : 0.0);
            plane.selected_wall_ids.assign(selected->second.wall_ids.begin(), selected->second.wall_ids.end());
            dependencies.insert(selected->second.dependencies.begin(), selected->second.dependencies.end());
        } else {
            require(std::none_of(wall_planes.begin(), wall_planes.end(), [&](const auto& entry) {
                return std::get<0>(entry.first) == context_key(context);
            }), "architectural target uses a different source frame from its selected wall plane.");
            if (plane.floor_binding) {
                const auto& floor_placement = floor_placements.at(context.floor_id);
                // A horizontal floor plane survives yaw. Convert its actual
                // source elevation through the floor's captured Site pose and
                // the target's inverse pose. This maps only a pivot plane: it
                // never enrolls independent target geometry in a floor frame.
                const double world_z = floors.at(context.floor_id).elevation_m +
                    floor_placement.forward.translation_m.z;
                require(std::isfinite(world_z), "bound floor world datum is not finite.");
                plane.origin = SelectionScalePlaneOrigin::floor_level;
                plane.effective_source_z_m = world_z - placement.forward.translation_m.z;
                dependencies.insert(floor_placement.dependency_ids.begin(), floor_placement.dependency_ids.end());
            }
            // An unbound floor or context-free world source retains its
            // historical effective-local zero pivot, explicitly marked above.
        }
        if (plane.floor_binding) dependencies.insert(plane.floor_binding->graph_entity_id);
        require(std::isfinite(plane.effective_source_z_m), "resolved source scaling plane is not finite.");
        plane.dependency_ids.assign(dependencies.begin(), dependencies.end());
        result.emplace(id, std::move(plane));
    }
    return result;
}

std::map<std::string, SelectionScalePlane, std::less<>> resolve_selection_scale_planes(
    const DocumentSnapshot& snapshot, std::span<const std::string> architectural_target_ids,
    std::span<const std::string> selected_wall_ids, SelectionScalePlaneDomain domain) {
    return resolve_selection_scale_planes(snapshot.entities(), architectural_target_ids, selected_wall_ids, domain);
}
} // namespace sketch
