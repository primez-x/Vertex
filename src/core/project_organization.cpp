#include "sketch/project_organization.hpp"
#include "sketch/vertical_levels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace sketch {
namespace {

struct OptionalReference {
    bool present = false;
    bool valid = true;
    std::string id;
};

struct Resolution {
    bool valid = false;
    DrawingContext context;
    std::string parent_id;
    std::vector<std::string> issues;
};

const nlohmann::json* property_value(const Entity& entity, std::string_view key) {
    if (!entity.properties.is_object()) {
        return nullptr;
    }
    const auto iterator = entity.properties.find(std::string(key));
    if (iterator == entity.properties.end()) {
        return nullptr;
    }
    return &iterator.value();
}

OptionalReference read_reference(const Entity& entity, std::string_view key) {
    const auto* value = property_value(entity, key);
    if (value == nullptr) {
        return {};
    }

    OptionalReference result;
    result.present = true;
    if (!value->is_string()) {
        result.valid = false;
        return result;
    }

    result.id = value->get<std::string>();
    result.valid = !result.id.empty();
    return result;
}

std::string display_name(const Entity& entity) {
    const auto* value = property_value(entity, "name");
    if (value != nullptr && value->is_string()) {
        const auto& name = value->get_ref<const std::string&>();
        if (!name.empty()) {
            return name;
        }
    }
    if (!entity.type.empty()) {
        return entity.type + " " + entity.id;
    }
    return entity.id;
}

void add_issue(std::vector<std::string>& issues, std::string message) {
    if (std::find(issues.begin(), issues.end(), message) == issues.end()) {
        issues.push_back(std::move(message));
    }
}

void add_issues(std::vector<std::string>& destination,
                const std::vector<std::string>& source) {
    for (const auto& issue : source) {
        add_issue(destination, issue);
    }
}

bool is_placeable_type(std::string_view type) noexcept {
    static constexpr std::array<std::string_view, 13> placeable{
        "boundary", "measurement_boundary", "room_boundary", "wall", "opening", "room",
        "slab", "roof", "stair", "railing", "column", "beam", "terrain_surface"};
    return std::find(placeable.begin(), placeable.end(), type) != placeable.end();
}

class Resolver final {
public:
    explicit Resolver(const std::map<std::string, Entity, std::less<>>& entities)
        : entities_(entities) {}

    Resolution resolve(std::string_view id) {
        const auto cached = resolved_.find(id);
        if (cached != resolved_.end()) {
            return cached->second;
        }

        const std::string key(id);
        if (visiting_.contains(key)) {
            Resolution cycle;
            add_issue(cycle.issues, "organization cycle includes entity '" + key + "'");
            return cycle;
        }

        const auto entity = entities_.find(id);
        if (entity == entities_.end()) {
            Resolution missing;
            add_issue(missing.issues, "organization reference targets missing entity '" + key + "'");
            return missing;
        }

        visiting_.insert(key);
        Resolution result = resolve_entity(entity->second);
        visiting_.erase(key);

        if (!result.valid) {
            result.context = {};
            result.parent_id.clear();
        }
        const auto [inserted, unused] = resolved_.emplace(key, std::move(result));
        (void)unused;
        return inserted->second;
    }

private:
    const std::map<std::string, Entity, std::less<>>& entities_;
    std::map<std::string, Resolution, std::less<>> resolved_;
    std::set<std::string, std::less<>> visiting_;

    static std::string entity_label(const Entity& entity) {
        return entity.type + " '" + entity.id + "'";
    }

    const Entity* find_target(const OptionalReference& reference,
                              std::string_view expected_type,
                              const Entity& entity,
                              std::string_view key,
                              std::vector<std::string>& issues) const {
        if (!reference.valid) {
            add_issue(issues, entity_label(entity) + " has malformed " + std::string(key));
            return nullptr;
        }
        const auto target = entities_.find(reference.id);
        if (target == entities_.end()) {
            add_issue(issues, entity_label(entity) + " references missing " +
                               std::string(expected_type) + " '" + reference.id + "' via " +
                               std::string(key));
            return nullptr;
        }
        if (target->second.type != expected_type) {
            add_issue(issues, entity_label(entity) + " references '" + reference.id + "' via " +
                               std::string(key) + ", expected type " +
                               std::string(expected_type) + " but found " + target->second.type);
            return nullptr;
        }
        return &target->second;
    }

    bool require_reference(const Entity& entity,
                           std::string_view key,
                           std::string_view expected_type,
                           std::string& parent_id,
                           Resolution& parent,
                           std::vector<std::string>& issues) {
        const auto reference = read_reference(entity, key);
        if (!reference.present) {
            add_issue(issues, entity_label(entity) + " is missing required " + std::string(key));
            return false;
        }
        const auto* target = find_target(reference, expected_type, entity, key, issues);
        if (target == nullptr) {
            return false;
        }

        parent_id = target->id;
        parent = resolve(target->id);
        if (!parent.valid) {
            add_issue(issues, entity_label(entity) + " depends on unresolved " +
                               std::string(key) + " '" + target->id + "'");
            add_issues(issues, parent.issues);
            return false;
        }
        return true;
    }

    void check_optional_matches(const Entity& entity,
                                std::string_view key,
                                std::string_view expected_type,
                                std::string_view expected_id,
                                std::vector<std::string>& issues) {
        const auto reference = read_reference(entity, key);
        if (!reference.present) {
            return;
        }
        const auto* target = find_target(reference, expected_type, entity, key, issues);
        if (target == nullptr) {
            return;
        }
        if (reference.id != expected_id) {
            add_issue(issues, entity_label(entity) + " has redundant " + std::string(key) +
                               " '" + reference.id + "' but hierarchy resolves to '" +
                               std::string(expected_id) + "'");
            return;
        }

        const auto target_resolution = resolve(target->id);
        if (!target_resolution.valid) {
            add_issue(issues, entity_label(entity) + " has unresolved " + std::string(key) +
                               " '" + target->id + "'");
            add_issues(issues, target_resolution.issues);
        }
    }

    void reject_reference(const Entity& entity,
                          std::string_view key,
                          std::string_view expected_type,
                          std::vector<std::string>& issues) {
        const auto reference = read_reference(entity, key);
        if (!reference.present) {
            return;
        }
        (void)find_target(reference, expected_type, entity, key, issues);
        add_issue(issues, entity_label(entity) + " cannot use " + std::string(key) +
                           " as an organizational parent");
    }

    bool resolve_floor_level(const Entity& entity, Resolution& result) {
        const auto* value = property_value(entity, "vertical_level_binding");
        if (value == nullptr) return true;
        try {
            const auto binding = VerticalLevelBinding::from_json(*value);
            const auto graph = entities_.find(binding.graph_entity_id);
            if (graph == entities_.end()) {
                add_issue(result.issues, entity_label(entity) +
                                        " references missing vertical level graph '" +
                                        binding.graph_entity_id + "'");
                return false;
            }
            if (graph->second.type != "vertical_levels") {
                add_issue(result.issues, entity_label(entity) +
                                        " vertical level graph '" + binding.graph_entity_id +
                                        "' has type " + graph->second.type);
                return false;
            }
            const auto model = VerticalLevelGraph::from_json(graph->second.properties.at("model"));
            const auto level = std::find_if(model.levels().begin(), model.levels().end(),
                [&](const auto& candidate) { return candidate.id == binding.level_id; });
            if (level == model.levels().end()) {
                add_issue(result.issues, entity_label(entity) +
                                        " references missing vertical level '" +
                                        binding.level_id + "'");
                return false;
            }
            result.context.level_id = binding.level_id;
            return true;
        } catch (const std::exception& error) {
            add_issue(result.issues, entity_label(entity) +
                                    " has an invalid vertical level binding: " + error.what());
            return false;
        }
    }

    Resolution resolve_property(const Entity& entity) {
        Resolution result;
        result.valid = true;
        result.context.property_id = entity.id;
        reject_reference(entity, "property_id", "property", result.issues);
        reject_reference(entity, "building_id", "building", result.issues);
        reject_reference(entity, "floor_id", "floor", result.issues);
        reject_reference(entity, "layer_id", "layer", result.issues);
        reject_reference(entity, "wall_id", "wall", result.issues);
        if (!result.issues.empty()) {
            result.valid = false;
        }
        return result;
    }

    Resolution resolve_building(const Entity& entity) {
        Resolution result;
        result.valid = true;
        reject_reference(entity, "building_id", "building", result.issues);
        reject_reference(entity, "floor_id", "floor", result.issues);
        reject_reference(entity, "layer_id", "layer", result.issues);
        reject_reference(entity, "wall_id", "wall", result.issues);

        std::string property_id;
        Resolution property;
        if (!require_reference(entity, "property_id", "property", property_id, property,
                                result.issues)) {
            result.valid = false;
            return result;
        }
        result.context = property.context;
        result.context.building_id = entity.id;
        result.parent_id = property_id;
        if (!result.issues.empty()) {
            result.valid = false;
        }
        return result;
    }

    Resolution resolve_floor(const Entity& entity) {
        Resolution result;
        result.valid = true;
        reject_reference(entity, "floor_id", "floor", result.issues);
        reject_reference(entity, "layer_id", "layer", result.issues);
        reject_reference(entity, "wall_id", "wall", result.issues);

        std::string building_id;
        Resolution building;
        if (!require_reference(entity, "building_id", "building", building_id, building,
                                result.issues)) {
            result.valid = false;
            return result;
        }
        result.context = building.context;
        result.context.floor_id = entity.id;
        result.parent_id = building_id;
        check_optional_matches(entity, "property_id", "property", result.context.property_id,
                               result.issues);
        if (!resolve_floor_level(entity, result)) {
            result.valid = false;
        }
        if (!result.issues.empty()) {
            result.valid = false;
        }
        return result;
    }

    Resolution resolve_layer(const Entity& entity) {
        Resolution result;
        result.valid = true;
        reject_reference(entity, "layer_id", "layer", result.issues);
        reject_reference(entity, "wall_id", "wall", result.issues);

        std::string floor_id;
        Resolution floor;
        if (!require_reference(entity, "floor_id", "floor", floor_id, floor, result.issues)) {
            result.valid = false;
            return result;
        }
        result.context = floor.context;
        result.context.layer_id = entity.id;
        result.parent_id = floor_id;
        check_optional_matches(entity, "building_id", "building", result.context.building_id,
                               result.issues);
        check_optional_matches(entity, "property_id", "property", result.context.property_id,
                               result.issues);
        if (!result.issues.empty()) {
            result.valid = false;
        }
        return result;
    }

    Resolution resolve_opening(const Entity& entity) {
        Resolution result;
        result.valid = true;

        std::string wall_id;
        Resolution wall;
        if (!require_reference(entity, "wall_id", "wall", wall_id, wall, result.issues)) {
            result.valid = false;
            return result;
        }
        result.context = wall.context;
        result.parent_id = wall_id;
        check_optional_matches(entity, "property_id", "property", result.context.property_id,
                               result.issues);
        check_optional_matches(entity, "building_id", "building", result.context.building_id,
                               result.issues);
        check_optional_matches(entity, "floor_id", "floor", result.context.floor_id,
                               result.issues);
        check_optional_matches(entity, "layer_id", "layer", result.context.layer_id,
                               result.issues);
        if (!result.issues.empty()) {
            result.valid = false;
        }
        return result;
    }

    Resolution resolve_object(const Entity& entity) {
        Resolution result;
        result.valid = true;
        reject_reference(entity, "wall_id", "wall", result.issues);
        const auto layer = read_reference(entity, "layer_id");
        const auto floor = read_reference(entity, "floor_id");
        const auto building = read_reference(entity, "building_id");
        const auto property = read_reference(entity, "property_id");

        if (layer.present) {
            std::string layer_id;
            Resolution layer_resolution;
            if (!require_reference(entity, "layer_id", "layer", layer_id, layer_resolution,
                                    result.issues)) {
                result.valid = false;
                return result;
            }
            result.context = layer_resolution.context;
            result.parent_id = layer_id;
            check_optional_matches(entity, "floor_id", "floor", result.context.floor_id,
                                   result.issues);
            check_optional_matches(entity, "building_id", "building",
                                   result.context.building_id, result.issues);
            check_optional_matches(entity, "property_id", "property",
                                   result.context.property_id, result.issues);
            if (!result.issues.empty()) {
                result.valid = false;
            }
            return result;
        }

        if (floor.present) {
            std::string floor_id;
            Resolution floor_resolution;
            if (!require_reference(entity, "floor_id", "floor", floor_id, floor_resolution,
                                    result.issues)) {
                result.valid = false;
                return result;
            }
            check_optional_matches(entity, "building_id", "building",
                                   floor_resolution.context.building_id, result.issues);
            check_optional_matches(entity, "property_id", "property",
                                   floor_resolution.context.property_id, result.issues);
            add_issue(result.issues, entity_label(entity) +
                                      " has a floor but no layer_id; drawing context is incomplete");
            result.valid = false;
            return result;
        }

        if (building.present) {
            std::string building_id;
            Resolution building_resolution;
            if (!require_reference(entity, "building_id", "building", building_id,
                                    building_resolution, result.issues)) {
                result.valid = false;
                return result;
            }
            check_optional_matches(entity, "property_id", "property",
                                   building_resolution.context.property_id, result.issues);
            add_issue(result.issues, entity_label(entity) +
                                      " has a building but no floor_id/layer_id; drawing context is incomplete");
            result.valid = false;
            return result;
        }

        if (property.present) {
            if (entity.type == "terrain_surface") {
                std::string property_id;
                Resolution property_resolution;
                if (!require_reference(entity, "property_id", "property", property_id,
                                        property_resolution, result.issues)) {
                    result.valid = false;
                    return result;
                }
                result.context = property_resolution.context;
                result.parent_id = property_id;
                if (!result.issues.empty()) {
                    result.valid = false;
                }
                return result;
            }
            std::string property_id;
            Resolution property_resolution;
            if (!require_reference(entity, "property_id", "property", property_id,
                                    property_resolution, result.issues)) {
                result.valid = false;
                return result;
            }
            add_issue(result.issues, entity_label(entity) +
                                      " has a property_id but no building_id/floor_id/layer_id; drawing context is incomplete");
            result.valid = false;
            return result;
        }

        if (is_placeable_type(entity.type)) {
            add_issue(result.issues, entity_label(entity) +
                                      " has no organizational placement; drawing context is unavailable");
            result.valid = false;
        }
        if (!result.issues.empty()) {
            result.valid = false;
        }
        return result;
    }

    Resolution resolve_entity(const Entity& entity) {
        if (entity.type == "property") {
            return resolve_property(entity);
        }
        if (entity.type == "building") {
            return resolve_building(entity);
        }
        if (entity.type == "floor") {
            return resolve_floor(entity);
        }
        if (entity.type == "layer") {
            return resolve_layer(entity);
        }
        if (entity.type == "opening") {
            return resolve_opening(entity);
        }
        return resolve_object(entity);
    }
};

}  // namespace

bool DrawingContext::complete() const noexcept {
    return !property_id.empty() && !building_id.empty() && !floor_id.empty() &&
           !layer_id.empty();
}

std::optional<DrawingContext> ProjectOrganization::drawing_context(
    std::string_view entity_id) const {
    const auto entity = nodes.find(entity_id);
    if (entity == nodes.end() || !entity->second.issues.empty() ||
        !entity->second.context.complete()) {
        return std::nullopt;
    }
    return entity->second.context;
}

ProjectOrganization organize_project(const DocumentSnapshot& snapshot) {
    return organize_project(snapshot.entities());
}

ProjectOrganization organize_project(const std::map<std::string, Entity, std::less<>>& entities) {
    ProjectOrganization result;
    Resolver resolver(entities);

    for (const auto& [id, entity] : entities) {
        OrganizationNode node;
        node.id = id;
        node.type = entity.type;
        node.name = display_name(entity);
        result.nodes.emplace(id, std::move(node));
    }

    for (const auto& [id, entity] : entities) {
        const auto resolution = resolver.resolve(id);
        auto& node = result.nodes.at(id);
        node.context = resolution.context;
        node.parent_id = resolution.parent_id;
        node.issues = resolution.issues;
        if (!resolution.valid) {
            node.context = {};
            node.parent_id.clear();
        }
    }

    for (const auto& [id, unused] : result.nodes) {
        (void)unused;
        auto& node = result.nodes.at(id);
        if (node.parent_id.empty()) {
            result.roots.push_back(id);
            continue;
        }
        const auto parent = result.nodes.find(node.parent_id);
        if (parent == result.nodes.end()) {
            add_issue(node.issues, "organization parent '" + node.parent_id +
                                  "' is not present in the index");
            node.parent_id.clear();
            node.context = {};
            result.roots.push_back(id);
            continue;
        }
        parent->second.children.push_back(id);
    }

    std::sort(result.roots.begin(), result.roots.end());
    for (auto& [unused, node] : result.nodes) {
        (void)unused;
        std::sort(node.children.begin(), node.children.end());
    }
    return result;
}

Entity resolve_vertical_placement(const DocumentSnapshot& snapshot,
                                  const Entity& entity) {
    if (!entity.properties.is_object()) {
        return entity;
    }
    const auto property = entity.properties.find("vertical_placement");
    if (property == entity.properties.end()) {
        return entity;
    }
    const auto& placement = property.value();
    if (!placement.is_object() || placement.size() != 3 ||
        !placement.contains("version") || !placement.contains("mode") ||
        !placement.contains("offset_m") || !placement.at("version").is_number_integer() ||
        placement.at("version") != 1 || !placement.at("mode").is_string() ||
        !placement.at("offset_m").is_number()) {
        throw std::invalid_argument("vertical_placement must be version 1 with mode and offset_m");
    }
    const auto mode = placement.at("mode").get<std::string>();
    const auto offset = placement.at("offset_m").get<double>();
    if ((mode != "absolute" && mode != "level") || !std::isfinite(offset) ||
        std::abs(offset) > 1e9) {
        throw std::invalid_argument("vertical_placement has an invalid mode or offset_m");
    }
    if (mode == "absolute") {
        return entity;
    }
    if (entity.type == "opening" || entity.type == "boundary" ||
        entity.type == "measurement_boundary" || entity.type == "room_boundary") {
        throw std::invalid_argument("level placement is supported only for 3D objects and slabs");
    }

    const auto organization = organize_project(snapshot);
    const auto context = organization.drawing_context(entity.id);
    if (!context || context->floor_id.empty()) {
        throw std::invalid_argument("level placement requires a valid bound floor context");
    }
    const auto floor = snapshot.entities().find(context->floor_id);
    if (floor == snapshot.entities().end() || floor->second.type != "floor" ||
        !floor->second.properties.is_object() ||
        !floor->second.properties.contains("vertical_level_binding")) {
        throw std::invalid_argument("level placement requires a floor level binding");
    }
    VerticalLevelBinding binding;
    try {
        binding = VerticalLevelBinding::from_json(
            floor->second.properties.at("vertical_level_binding"));
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("floor level binding is invalid: ") + error.what());
    }
    const auto graph = snapshot.entities().find(binding.graph_entity_id);
    if (graph == snapshot.entities().end() || graph->second.type != "vertical_levels" ||
        !graph->second.properties.is_object() || !graph->second.properties.contains("model")) {
        throw std::invalid_argument("level placement references a missing vertical level graph");
    }
    VerticalLevelGraph levels;
    try {
        levels = VerticalLevelGraph::from_json(graph->second.properties.at("model"));
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("vertical level graph is invalid: ") + error.what());
    }
    const auto level = std::find_if(levels.levels().begin(), levels.levels().end(),
        [&](const auto& candidate) { return candidate.id == binding.level_id; });
    if (level == levels.levels().end()) {
        throw std::invalid_argument("level placement references a missing vertical level");
    }
    const double shift = level->elevation_m + offset;
    if (!std::isfinite(shift)) {
        throw std::invalid_argument("resolved level placement is not finite");
    }

    Entity result = entity;
    auto translate_vector_z = [&](const char* key) {
        auto value = result.properties.find(key);
        if (value == result.properties.end()) return false;
        if (!value->is_array() || value->size() != 3 ||
            !value->at(0).is_number() || !value->at(1).is_number() ||
            !value->at(2).is_number()) {
            throw std::invalid_argument(std::string("vertical placement coordinate ") + key +
                                        " must be a finite Vec3");
        }
        const auto x = value->at(0).get<double>();
        const auto y = value->at(1).get<double>();
        const auto z = value->at(2).get<double>();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            !std::isfinite(z + shift)) {
            throw std::invalid_argument(std::string("vertical placement coordinate ") + key +
                                        " is not finite");
        }
        (*value)[2] = z + shift;
        return true;
    };
    if (entity.type == "wall" || entity.type == "slab" || entity.type == "room") {
        auto value = result.properties.find("elevation_m");
        if (value == result.properties.end() || !value->is_number()) {
            throw std::invalid_argument("level placement requires a finite elevation_m");
        }
        const auto elevation = value->get<double>();
        if (!std::isfinite(elevation) || !std::isfinite(elevation + shift)) {
            throw std::invalid_argument("level placement elevation_m is not finite");
        }
        *value = elevation + shift;
        return result;
    }
    const bool translated = translate_vector_z("base_center_m") ||
                            translate_vector_z("base_position_m") ||
                            translate_vector_z("start_m") ||
                            translate_vector_z("end_m");
    if (!translated) {
        throw std::invalid_argument("level placement object has no supported Z coordinate");
    }
    // A beam has two endpoints and must move as one rigid object. The first
    // short-circuit above intentionally handles start_m; translate end_m too.
    if (entity.type == "beam") {
        (void)translate_vector_z("end_m");
    }
    return result;
}

}  // namespace sketch
