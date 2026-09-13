#include "sketch/constraint_integrity.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr double linear_tolerance = constraint_linear_tolerance_metres;
constexpr double angular_tolerance = constraint_angular_tolerance_radians;

struct IndexedOpening {
    std::string id;
    const Entity* entity = nullptr;
};

using OpeningIndex = std::map<std::string, std::vector<IndexedOpening>, std::less<>>;

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument(message);
}

double number(const json& value, const char* description) {
    if (!value.is_number()) invalid(std::string(description) + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string(description) + " must be finite");
    return result;
}

Vec2 point(const json& value, const char* description) {
    if (!value.is_array() || value.size() != 2)
        invalid(std::string(description) + " must have two coordinates");
    return {number(value[0], description), number(value[1], description)};
}

OpeningIndex index_openings(const Entities& entities) {
    OpeningIndex result;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "opening" || !entity.properties.is_object()) continue;
        const auto host = entity.properties.find("wall_id");
        if (host == entity.properties.end() || !host->is_string()) continue;
        result[host->get_ref<const std::string&>()].push_back({id, &entity});
    }
    return result;
}

Wall read_wall(const std::string& owner, const Entities& entities,
               const OpeningIndex& openings_by_wall) {
    const auto found = entities.find(owner);
    if (found == entities.end() || found->second.type != "wall")
        invalid("Constraint owner is not an existing wall: " + owner);
    const auto& properties = found->second.properties;
    const auto& baseline = properties.at("baseline");
    Wall wall{owner,
        {point(baseline.at("start"), "Wall start"), point(baseline.at("end"), "Wall end"),
         number(baseline.at("sweep_radians"), "Wall sweep")},
        number(properties.at("thickness_m"), "Wall thickness"),
        number(properties.at("height_m"), "Wall height"),
        number(properties.at("elevation_m"), "Wall elevation"), {}};
    if (const auto layers = properties.find("layers"); layers != properties.end()) {
        wall.layers = parse_wall_layers(layers.value(), wall.thickness);
    }
    if (const auto slope = properties.find("slope_rise_m"); slope != properties.end()) {
        if (!slope->is_number()) invalid("Wall slope_rise_m must be a finite number");
        wall.slope_rise = slope->get<double>();
    }
    const auto hosted = openings_by_wall.find(owner);
    if (hosted != openings_by_wall.end()) {
        wall.openings.reserve(hosted->second.size());
        for (const auto& indexed : hosted->second) {
            const auto& p = indexed.entity->properties;
            wall.openings.push_back({indexed.id, number(p.at("offset_m"), "Opening offset"),
                number(p.at("width_m"), "Opening width"),
                number(p.at("sill_m"), "Opening sill"),
                number(p.at("height_m"), "Opening height")});
        }
    }
    validate_wall_semantics(wall);
    return wall;
}

void require_straight_wall(const Wall& wall) {
    if (wall.baseline.sweep_radians != 0.0)
        invalid("Constraint v1 requires a straight wall: " + wall.id);
}

std::vector<std::string> typed_wall_ids(const Entity& entity) {
    const auto& value = entity.properties.at("wall_ids");
    std::vector<std::string> result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(item.get_ref<const std::string&>());
    return result;
}

Vec2 difference(Vec2 first, Vec2 second) {
    const Vec2 result{first.x - second.x, first.y - second.y};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        invalid("Constraint displacement exceeds the supported numeric range");
    return result;
}

double length(Vec2 value) {
    const auto result = std::hypot(value.x, value.y);
    if (!std::isfinite(result)) invalid("Constraint length exceeds the supported numeric range");
    return result;
}

Vec2 direction(Vec2 start, Vec2 end) {
    const auto delta = difference(end, start);
    const auto magnitude = length(delta);
    if (magnitude <= default_geometry_tolerance_metres)
        invalid("Constraint direction endpoints must be distinct");
    return {delta.x / magnitude, delta.y / magnitude};
}

bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

WallEndpointRole reverse_role(WallEndpointRole role) {
    return role == WallEndpointRole::start ? WallEndpointRole::end : WallEndpointRole::start;
}
} // namespace

std::optional<std::string> validate_constraint_integrity(const Entities& entities) {
    std::optional<std::string> unsupported;
    std::map<std::string, Wall, std::less<>> owners;
    const auto openings_by_wall = index_openings(entities);
    for (const auto& [id, entity] : entities) {
        if (entity.type != "constraint") continue;
        try {
            const auto decoded = decode_constraint_entity(entity);
            for (const auto& owner_id : typed_wall_ids(entity)) {
                auto found = owners.find(owner_id);
                if (found == owners.end()) {
                    found = owners.emplace(owner_id,
                        read_wall(owner_id, entities, openings_by_wall)).first;
                }
                if (decoded.constraint) require_straight_wall(found->second);
            }
            if (!decoded.constraint) {
                if (!unsupported) unsupported = "Constraint " + id + ": " + decoded.unsupported_reason;
                continue;
            }
            const auto& constraint = *decoded.constraint;
            std::vector<Vec2> points;
            for (const auto& binding : constraint.bindings) {
                auto found = owners.find(binding.owner_id);
                if (found == owners.end())
                    found = owners.emplace(binding.owner_id,
                        read_wall(binding.owner_id, entities, openings_by_wall)).first;
                points.push_back(binding.role == WallEndpointRole::start
                    ? found->second.baseline.start : found->second.baseline.end);
            }
            bool satisfied = false;
            switch (constraint.relation) {
            case ConstraintRelationKind::horizontal:
                satisfied = std::abs(difference(points.at(1), points.at(0)).y) <= linear_tolerance;
                break;
            case ConstraintRelationKind::vertical:
                satisfied = std::abs(difference(points.at(1), points.at(0)).x) <= linear_tolerance;
                break;
            case ConstraintRelationKind::coincident:
                satisfied = length(difference(points.at(1), points.at(0))) <= linear_tolerance;
                break;
            case ConstraintRelationKind::fixed_length:
                satisfied = constraint.length.has_value() &&
                    std::abs(length(difference(points.at(1), points.at(0))) -
                             constraint.length->metres) <= linear_tolerance;
                break;
            case ConstraintRelationKind::fixed_anchor:
                satisfied = constraint.anchor.has_value() &&
                    length(difference(points.at(0), *constraint.anchor)) <= linear_tolerance;
                break;
            case ConstraintRelationKind::parallel:
            case ConstraintRelationKind::perpendicular: {
                const auto first = direction(points.at(0), points.at(1));
                const auto second = direction(points.at(2), points.at(3));
                const auto cross = std::abs(first.x * second.y - first.y * second.x);
                const auto dot = std::abs(first.x * second.x + first.y * second.y);
                const auto residual = constraint.relation == ConstraintRelationKind::parallel
                    ? std::atan2(cross, dot) : std::atan2(dot, cross);
                satisfied = std::isfinite(residual) && residual <= angular_tolerance;
                break;
            }
            }
            if (!satisfied) invalid("Persisted hard relation is not satisfied");
        } catch (const std::exception& error) {
            invalid("Constraint " + id + ": " + error.what());
        }
    }
    return unsupported;
}

void validate_constraint_transition(const Entities& before, const Entities& after) {
    std::set<std::string, std::less<>> reversed;
    for (const auto& [id, entity] : before) {
        if (entity.type != "constraint" || !after.contains(id) ||
            after.at(id).type != "constraint") continue;
        const auto decoded = decode_constraint_entity(entity);
        if (!decoded.constraint) continue; // Unsupported documents are read-only.
        for (const auto& binding : decoded.constraint->bindings) {
            if (reversed.contains(binding.owner_id) || !after.contains(binding.owner_id)) continue;
            const auto& old_baseline = before.at(binding.owner_id).properties.at("baseline");
            const auto& new_baseline = after.at(binding.owner_id).properties.at("baseline");
            if (same_point(point(old_baseline.at("start"), "Wall start"),
                           point(new_baseline.at("end"), "Wall end")) &&
                same_point(point(old_baseline.at("end"), "Wall end"),
                           point(new_baseline.at("start"), "Wall start")))
                reversed.insert(binding.owner_id);
        }
    }
    if (reversed.empty()) return;
    for (const auto& [id, entity] : before) {
        if (entity.type != "constraint" || !after.contains(id) ||
            after.at(id).type != "constraint") continue;
        const auto old_value = decode_constraint_entity(entity);
        if (!old_value.constraint) continue;
        const auto new_value = decode_constraint_entity(after.at(id));
        if (!new_value.constraint) invalid("Wall reversal cannot introduce unknown lock semantics");
        const auto& old_bindings = old_value.constraint->bindings;
        const auto& new_bindings = new_value.constraint->bindings;
        for (std::size_t index = 0; index < old_bindings.size(); ++index) {
            const auto& binding = old_bindings[index];
            if (!reversed.contains(binding.owner_id)) continue;
            if (index >= new_bindings.size() || new_bindings[index].owner_id != binding.owner_id ||
                new_bindings[index].role != reverse_role(binding.role))
                invalid("Wall reversal requires atomic remapping of constraint " + id);
        }
    }
}
} // namespace sketch
