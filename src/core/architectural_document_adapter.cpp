#include "sketch/architectural_document_adapter.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace sketch {
namespace {

using EntityState = std::map<std::string, Entity, std::less<>>;

Entity semantic_entity(const DocumentSnapshot& source, const std::string& id,
                       std::string_view expected_type, Revision expected_revision) {
    if (source.revision() != expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision, "semantic edit source revision is stale");
    const auto found = source.entities().find(id);
    if (found == source.entities().end())
        throw DocumentError(DocumentErrorCode::dangling_reference, "semantic edit target is missing");
    if (found->second.type != expected_type)
        throw DocumentError(DocumentErrorCode::invalid_entity, "semantic edit target has the wrong role");
    return found->second;
}

EntityState copy_entities(const DocumentSnapshot& source) {
    return source.entities();
}

std::vector<std::string> hosted_opening_ids(const EntityState& entities,
                                            const std::string& wall_id) {
    std::vector<std::string> result;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "opening" || !entity.properties.is_object()) continue;
        const auto found = entity.properties.find("wall_id");
        if (found != entity.properties.end() && found->is_string() &&
            found->get<std::string>() == wall_id) {
            result.push_back(id);
        }
    }
    return result;
}

std::string hosted_duplicate_id(const std::string& wall_id,
                                const std::string& opening_id) {
    auto result = wall_id + ":" + opening_id;
    // Document entity identities are capped at 128 bytes.  Fail while
    // constructing the detached transaction candidate so no partial command
    // can reach Document::apply.
    if (result.size() > 128) {
        throw std::invalid_argument("derived hosted opening identity is too long");
    }
    return result;
}

nlohmann::json properties_json(const std::map<std::string, std::string>& properties) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [key, value] : properties) {
        // ArchitecturalOperation is intentionally transport-friendly and keeps
        // property payloads as strings. Decode JSON text when possible so the
        // Document retains numeric, object, and array property types while
        // opaque semantic strings such as 4m remain strings.
        try {
            const auto parsed = nlohmann::json::parse(value);
            if (!parsed.is_discarded()) {
                result[key] = parsed;
                continue;
            }
        } catch (const nlohmann::json::exception&) {
            // Fall through to an opaque string value.
        }
        result[key] = value;
    }
    return result;
}

nlohmann::json transform_json(const ArchitecturalTransform& transform) {
    return { {"translation_m", {transform.x, transform.y, transform.z}},
             {"rotation_z_radians", transform.rotation_z_radians},
             {"uniform_scale", transform.scale} };
}

Vec3 transform_point(const Vec3& point, const ArchitecturalTransform& transform) {
    const auto cosine = std::cos(transform.rotation_z_radians);
    const auto sine = std::sin(transform.rotation_z_radians);
    const auto scaled_x = point.x * transform.scale;
    const auto scaled_y = point.y * transform.scale;
    return {cosine * scaled_x - sine * scaled_y + transform.x,
            sine * scaled_x + cosine * scaled_y + transform.y,
            point.z * transform.scale + transform.z};
}

Vec3 transform_direction(const Vec3& direction, const ArchitecturalTransform& transform) {
    const auto cosine = std::cos(transform.rotation_z_radians);
    const auto sine = std::sin(transform.rotation_z_radians);
    return {cosine * direction.x - sine * direction.y,
            sine * direction.x + cosine * direction.y,
            direction.z};
}

BuildingObject transform_building_object(BuildingObject object,
                                         const ArchitecturalTransform& transform) {
    return std::visit(
        [&transform](auto value) -> BuildingObject {
            using Object = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Object, RectangularColumn>) {
                value.base_center = transform_point(value.base_center, transform);
                value.width *= transform.scale;
                value.depth *= transform.scale;
                value.height *= transform.scale;
                value.rotation_radians += transform.rotation_z_radians;
            } else if constexpr (std::is_same_v<Object, CircularColumn>) {
                value.base_center = transform_point(value.base_center, transform);
                value.radius *= transform.scale;
                value.height *= transform.scale;
            } else if constexpr (std::is_same_v<Object, Beam>) {
                value.start = transform_point(value.start, transform);
                value.end = transform_point(value.end, transform);
                value.up = transform_direction(value.up, transform);
                value.width *= transform.scale;
                value.depth *= transform.scale;
            } else if constexpr (std::is_same_v<Object, StairFlight>) {
                value.base_position = transform_point(value.base_position, transform);
                value.orientation_radians += transform.rotation_z_radians;
                value.total_rise *= transform.scale;
                value.going *= transform.scale;
                value.width *= transform.scale;
                if (value.top_landing) {
                    value.top_landing->depth *= transform.scale;
                    value.top_landing->thickness *= transform.scale;
                }
            } else if constexpr (std::is_same_v<Object, Railing>) {
                value.base_position = transform_point(value.base_position, transform);
                value.orientation_radians += transform.rotation_z_radians;
                value.length *= transform.scale;
                value.height *= transform.scale;
                value.thickness *= transform.scale;
                value.post_spacing *= transform.scale;
            } else if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
                value.base_position = transform_point(value.base_position, transform);
                value.orientation_radians += transform.rotation_z_radians;
                value.run *= transform.scale;
                value.span *= transform.scale;
                value.rise *= transform.scale;
                value.overhang *= transform.scale;
                value.thickness *= transform.scale;
                for (auto& opening : value.openings) {
                    opening.x *= transform.scale;
                    opening.y *= transform.scale;
                    opening.width *= transform.scale;
                    opening.depth *= transform.scale;
                }
            } else if constexpr (std::is_same_v<Object, GableRoof> ||
                                 std::is_same_v<Object, HipRoof>) {
                value.base_position = transform_point(value.base_position, transform);
                value.orientation_radians += transform.rotation_z_radians;
                value.length *= transform.scale;
                value.span *= transform.scale;
                value.rise *= transform.scale;
                value.overhang *= transform.scale;
                value.thickness *= transform.scale;
                for (auto& opening : value.openings) {
                    opening.x *= transform.scale;
                    opening.y *= transform.scale;
                    opening.width *= transform.scale;
                    opening.depth *= transform.scale;
                }
            }
            return value;
        },
        std::move(object));
}

Vec2 transform_plan_point(const Vec2& point, const ArchitecturalTransform& transform) {
    const auto cosine = std::cos(transform.rotation_z_radians);
    const auto sine = std::sin(transform.rotation_z_radians);
    const auto scaled_x = point.x * transform.scale;
    const auto scaled_y = point.y * transform.scale;
    const Vec2 result{cosine * scaled_x - sine * scaled_y + transform.x,
                      sine * scaled_x + cosine * scaled_y + transform.y};
    if (!std::isfinite(result.x) || !std::isfinite(result.y)) {
        throw std::invalid_argument("Architectural plan transform exceeds the supported range");
    }
    return result;
}

Segment transform_plan_segment(Segment segment, const ArchitecturalTransform& transform) {
    segment.start = transform_plan_point(segment.start, transform);
    segment.end = transform_plan_point(segment.end, transform);
    // A positive uniform scale and a proper Z rotation preserve the signed
    // sweep of a circular arc.  Keeping the defining sweep avoids replacing
    // analytical curves with sampled screen geometry.
    return segment;
}

Boundary transform_plan_boundary(const Boundary& boundary,
                                 const ArchitecturalTransform& transform) {
    Boundary result;
    result.reserve(boundary.size());
    for (const auto& segment : boundary) {
        result.push_back(transform_plan_segment(segment, transform));
    }
    return result;
}

nlohmann::json point_json(const Vec2& point) {
    return nlohmann::json::array({point.x, point.y});
}

nlohmann::json segment_json(const Segment& segment) {
    return { {"start", point_json(segment.start)},
             {"end", point_json(segment.end)},
             {"sweep_radians", segment.sweep_radians} };
}

void update_segment_geometry(nlohmann::json& target, const Segment& segment) {
    const auto encoded = segment_json(segment);
    if (!target.is_object()) {
        target = encoded;
        return;
    }
    target["start"] = encoded.at("start");
    target["end"] = encoded.at("end");
    target["sweep_radians"] = encoded.at("sweep_radians");
}

nlohmann::json updated_boundary_geometry(const nlohmann::json& original,
                                         const Boundary& boundary) {
    auto result = nlohmann::json::array();
    for (std::size_t index = 0; index < boundary.size(); ++index) {
        nlohmann::json segment = nlohmann::json::object();
        if (original.is_array() && index < original.size()) segment = original.at(index);
        update_segment_geometry(segment, boundary.at(index));
        result.push_back(std::move(segment));
    }
    return result;
}

void scale_property(nlohmann::json& properties, const char* canonical,
                    const char* legacy, double scale) {
    const auto update = [&](const char* key) {
        const auto found = properties.find(key);
        if (found == properties.end()) return;
        if (!found->is_number()) {
            throw std::invalid_argument(std::string("Architectural property ") + key +
                                        " must be numeric");
        }
        const auto value = found->get<double>() * scale;
        if (!std::isfinite(value)) {
            throw std::invalid_argument(std::string("Architectural property ") + key +
                                        " exceeds the supported range");
        }
        properties[key] = value;
    };
    update(canonical);
    if (legacy != nullptr) update(legacy);
}

Entity transform_wall_entity(EntityState& entities, const Entity& source,
                             const ArchitecturalTransform& transform) {
    std::vector<const Entity*> openings;
    for (const auto& id : hosted_opening_ids(entities, source.id)) {
        const auto found = entities.find(id);
        if (found != entities.end()) openings.push_back(&found->second);
    }
    Wall wall;
    std::string error;
    if (!read_document_wall(source, openings, wall, error)) {
        throw std::invalid_argument(error);
    }
    wall.baseline = transform_plan_segment(wall.baseline, transform);
    wall.thickness *= transform.scale;
    wall.height *= transform.scale;
    wall.elevation = wall.elevation * transform.scale + transform.z;
    if (wall.slope_rise.has_value()) *wall.slope_rise *= transform.scale;
    for (auto& layer : wall.layers) layer.thickness *= transform.scale;
    validate_wall_semantics(wall);

    Entity result = source;
    update_segment_geometry(result.properties["baseline"], wall.baseline);
    result.properties["thickness_m"] = wall.thickness;
    if (result.properties.contains("thickness")) result.properties["thickness"] = wall.thickness;
    result.properties["height_m"] = wall.height;
    if (result.properties.contains("height")) result.properties["height"] = wall.height;
    result.properties["elevation_m"] = wall.elevation;
    if (result.properties.contains("elevation")) result.properties["elevation"] = wall.elevation;
    if (wall.slope_rise.has_value()) result.properties["slope_rise_m"] = *wall.slope_rise;
    else result.properties.erase("slope_rise_m");
    if (result.properties.contains("slope_rise")) {
        if (wall.slope_rise.has_value()) result.properties["slope_rise"] = *wall.slope_rise;
        else result.properties.erase("slope_rise");
    }
    if (result.properties.contains("layers")) result.properties["layers"] = wall_layers_json(wall.layers);
    result.properties.erase("transform");

    for (const auto& opening_id : hosted_opening_ids(entities, source.id)) {
        // The returned wall owns the opening's local station and dimensions;
        // translation and rotation act on the host baseline, while a positive
        // uniform scale updates all station/height quantities.
        auto& opening = entities.at(opening_id);
        scale_property(opening.properties, "offset_m", "offset", transform.scale);
        scale_property(opening.properties, "width_m", "width", transform.scale);
        scale_property(opening.properties, "sill_m", "sill", transform.scale);
        scale_property(opening.properties, "height_m", "height", transform.scale);
    }
    return result;
}

Entity transform_slab_entity(const Entity& source, const ArchitecturalTransform& transform) {
    Slab slab;
    std::string error;
    if (!read_document_slab(source, slab, error)) throw std::invalid_argument(error);
    slab.boundary = transform_plan_boundary(slab.boundary, transform);
    for (auto& hole : slab.holes) hole = transform_plan_boundary(hole, transform);
    slab.thickness *= transform.scale;
    slab.elevation = slab.elevation * transform.scale + transform.z;
    for (auto& layer : slab.layers) layer.thickness *= transform.scale;
    (void)make_slab(slab);

    Entity result = source;
    result.properties["boundary"] = updated_boundary_geometry(
        source.properties.at("boundary"), slab.boundary);
    if (result.properties.contains("holes")) {
        auto holes = nlohmann::json::array();
        const auto& source_holes = source.properties.at("holes");
        for (std::size_t index = 0; index < slab.holes.size(); ++index) {
            const auto original = source_holes.is_array() && index < source_holes.size()
                                      ? source_holes.at(index)
                                      : nlohmann::json::array();
            holes.push_back(updated_boundary_geometry(original, slab.holes.at(index)));
        }
        result.properties["holes"] = std::move(holes);
    }
    result.properties["thickness_m"] = slab.thickness;
    if (result.properties.contains("thickness")) result.properties["thickness"] = slab.thickness;
    result.properties["elevation_m"] = slab.elevation;
    if (result.properties.contains("elevation")) result.properties["elevation"] = slab.elevation;
    if (result.properties.contains("layers")) result.properties["layers"] = slab_layers_json(slab.layers);
    result.properties.erase("transform");
    return result;
}

Entity transform_room_entity(const Entity& source, const ArchitecturalTransform& transform) {
    RoomVolume room;
    std::string error;
    if (!read_document_room(source, room, error)) throw std::invalid_argument(error);
    room.boundary = transform_plan_boundary(room.boundary, transform);
    for (auto& hole : room.holes) hole = transform_plan_boundary(hole, transform);
    room.height *= transform.scale;
    room.elevation = room.elevation * transform.scale + transform.z;
    (void)make_room_volume(room);
    Entity result = source;
    if (result.properties.contains("boundary")) {
        result.properties["boundary"] = updated_boundary_geometry(
            source.properties.at("boundary"), room.boundary);
    }
    if (result.properties.contains("segments")) {
        result.properties["segments"] = updated_boundary_geometry(
            source.properties.at("segments"), room.boundary);
    }
    if (result.properties.contains("holes")) {
        auto holes = nlohmann::json::array();
        const auto& source_holes = source.properties.at("holes");
        for (std::size_t index = 0; index < room.holes.size(); ++index) {
            const auto original = source_holes.is_array() && index < source_holes.size()
                                      ? source_holes.at(index)
                                      : nlohmann::json::array();
            holes.push_back(updated_boundary_geometry(original, room.holes.at(index)));
        }
        result.properties["holes"] = std::move(holes);
    }
    result.properties["height_m"] = room.height;
    if (result.properties.contains("height")) result.properties["height"] = room.height;
    result.properties["elevation_m"] = room.elevation;
    if (result.properties.contains("elevation")) result.properties["elevation"] = room.elevation;
    result.properties.erase("transform");
    return result;
}

std::optional<Entity> try_transform_shared_solid(EntityState& entities,
                                                 const Entity& source,
                                                 const ArchitecturalTransform& transform) {
    // Incomplete generic architectural descriptors are still allowed to carry
    // a transport-level transform for compatibility with the existing
    // transaction contract. Once a canonical footprint is present, malformed
    // data fails closed instead of silently accepting a stale marker.
    if (source.type == "wall") {
        if (!source.properties.contains("baseline")) return std::nullopt;
        return transform_wall_entity(entities, source, transform);
    }
    if (source.type == "slab") {
        if (!source.properties.contains("boundary")) return std::nullopt;
        return transform_slab_entity(source, transform);
    }
    if (source.type == "room") {
        if (!source.properties.contains("boundary") && !source.properties.contains("segments")) {
            return std::nullopt;
        }
        return transform_room_entity(source, transform);
    }
    return std::nullopt;
}

Entity transform_building_entity(const Entity& source,
                                 const ArchitecturalTransform& transform) {
    if (source.type == "stair" && source.properties.contains("level_connection") &&
        std::abs(transform.scale - 1.0) > 1e-9) {
        // A connected stair's total rise is tied to the graph's floor-to-floor
        // height.  Scaling only the stair would silently break that relation;
        // edit the level graph (or disconnect the stair) before changing size.
        throw std::invalid_argument(
            "Cannot uniformly scale a stair with a level connection; edit the connected levels first");
    }
    const auto transformed = transform_building_object(decode_building_entity(source), transform);
    const auto canonical = encode_building_entity(transformed, source.extensions);
    Entity result = source;
    result.properties = canonical.properties;
    // Retain application-owned properties such as marks, material
    // assignments, quantity receipts, and future extensions while replacing
    // every canonical building field with the transformed value.  A legacy
    // marker is deliberately dropped because it would describe stale geometry.
    for (const auto& item : source.properties.items()) {
        const auto& key = item.key();
        if (!canonical.properties.contains(key) && key != "transform") {
            result.properties[key] = item.value();
        }
    }
    return result;
}

EntityState apply_operations(const DocumentSnapshot& source,
                             const ArchitecturalTransaction& transaction) {
    auto entities = copy_entities(source);
    const auto& initial = transaction.existing_ids();
    for (const auto& id : initial) {
        if (!entities.contains(id)) throw std::invalid_argument("architectural transaction source is stale");
    }
    for (const auto& operation : transaction.operations()) {
        switch (operation.action) {
        case ArchitecturalAction::create: {
            if (entities.contains(operation.object_id)) throw std::invalid_argument("architectural create ID already exists");
            if (!is_known_entity_type(operation.semantic_type))
                throw std::invalid_argument("architectural semantic type is not supported by Document");
            Entity created = Entity::create(operation.semantic_type, properties_json(operation.properties));
            created.id = operation.object_id;
            entities.emplace(created.id, std::move(created));
            break;
        }
        case ArchitecturalAction::select:
            if (!entities.contains(operation.object_id)) throw std::invalid_argument("architectural selection target is missing");
            break;
        case ArchitecturalAction::property_edit: {
            auto found = entities.find(operation.object_id);
            if (found == entities.end()) throw std::invalid_argument("architectural edit target is missing");
            const auto decoded = properties_json(operation.properties);
            for (const auto& [key, value] : decoded.items()) found->second.properties[key] = value;
            break;
        }
        case ArchitecturalAction::transform: {
            auto found = entities.find(operation.object_id);
            if (found == entities.end() || !operation.transform)
                throw std::invalid_argument("architectural transform target is missing");
            if (can_recognize_building_entity_type(found->second.type)) {
                found->second = transform_building_entity(found->second, *operation.transform);
            } else if (const auto transformed =
                           try_transform_shared_solid(entities, found->second, *operation.transform)) {
                found->second = *transformed;
            } else {
                // Generic entities without a canonical solid descriptor retain
                // the transport marker for compatibility with older records.
                found->second.properties["transform"] = transform_json(*operation.transform);
            }
            break;
        }
        case ArchitecturalAction::duplicate: {
            auto found = entities.find(operation.object_id);
            if (found == entities.end()) throw std::invalid_argument("architectural duplicate source is missing");
            if (entities.contains(operation.duplicate_id)) throw std::invalid_argument("architectural duplicate ID already exists");
            auto copy = found->second;
            copy.id = operation.duplicate_id;
            entities.emplace(copy.id, std::move(copy));
            if (found->second.type == "wall") {
                const auto children = hosted_opening_ids(entities, operation.object_id);
                for (const auto& child_id : children) {
                    const auto child = entities.find(child_id);
                    if (child == entities.end()) continue;
                    auto child_copy = child->second;
                    child_copy.id = hosted_duplicate_id(operation.duplicate_id, child_id);
                    if (entities.contains(child_copy.id)) {
                        throw std::invalid_argument("hosted opening duplicate ID already exists");
                    }
                    child_copy.properties["wall_id"] = operation.duplicate_id;
                    entities.emplace(child_copy.id, std::move(child_copy));
                }
            }
            break;
        }
        case ArchitecturalAction::erase: {
            const auto found = entities.find(operation.object_id);
            if (found == entities.end()) throw std::invalid_argument("architectural delete target is missing");
            if (found->second.type == "wall") {
                for (const auto& child_id : hosted_opening_ids(entities, operation.object_id)) {
                    entities.erase(child_id);
                }
            }
            entities.erase(found);
            break;
        }
        }
    }
    return entities;
}

ApplyEntityChanges make_command(const DocumentSnapshot& source, const ArchitecturalTransaction& transaction,
                                Revision expected_revision) {
    const auto candidate = apply_operations(source, transaction);
    ApplyEntityChanges command;
    command.expected_revision = expected_revision;
    command.message = transaction.undo_label();
    std::set<std::string, std::less<>> ids;
    for (const auto& [id, entity] : source.entities()) ids.insert(id);
    for (const auto& [id, entity] : candidate) ids.insert(id);
    for (const auto& id : ids) {
        const auto before = source.entities().find(id);
        const auto after = candidate.find(id);
        if (before != source.entities().end() && after == candidate.end()) {
            command.entity_changes.push_back(EntityChange::erase(id));
        } else if (after != candidate.end() &&
                   (before == source.entities().end() || before->second != after->second)) {
            command.entity_changes.push_back(EntityChange::upsert(after->second));
        }
    }
    return command;
}

}  // namespace

ApplyEntityChanges assembly_type_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, AssemblyType replacement, Revision expected_revision) {
    auto entity = semantic_entity(source, entity_id, "assembly_model", expected_revision);
    entity.properties["model"] = AssemblyModel::from_json(entity.properties.at("model"))
        .with_type(std::move(replacement)).to_json();
    return {expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Update assembly type"};
}

ApplyEntityChanges model_phase_selection_command(const DocumentSnapshot& source,
    const std::string& entity_id, std::optional<std::string> alternative, Revision expected_revision) {
    auto entity = semantic_entity(source, entity_id, "model_phases", expected_revision);
    entity.properties["model"] = ModelPhases::from_json(entity.properties.at("model"))
        .with_active(std::move(alternative)).to_json();
    return {expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Select remodeling alternative"};
}

ApplyEntityChanges architectural_transaction_command(const DocumentSnapshot& source,
                                                     const ArchitecturalTransaction& transaction,
                                                     Revision expected_revision) {
    return make_command(source, transaction, expected_revision);
}

DocumentSnapshot preview_architectural_transaction(const DocumentSnapshot& source,
                                                   const ArchitecturalTransaction& transaction) {
    const auto command = architectural_transaction_command(source, transaction, source.revision());
    if (command.entity_changes.empty()) return source;
    return Document::preview_command(source, Command{command});
}

Revision apply_architectural_transaction(Document& document,
                                         const ArchitecturalTransaction& transaction,
                                         Revision expected_revision) {
    const auto source = document.snapshot();
    if (source.revision() != expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision, "architectural transaction revision is stale");
    const auto command = architectural_transaction_command(source, transaction, expected_revision);
    if (command.entity_changes.empty()) return document.revision();
    return document.apply(Command{command});
}

}  // namespace sketch
