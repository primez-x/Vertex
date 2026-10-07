#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace sketch {
namespace {

using EntityState = std::map<std::string, Entity, std::less<>>;

bool canonical_form(const Entity& entity, std::string_view type, int version, std::string_view form) {
    const auto& p=entity.properties;
    return entity.type==type && p.is_object() && p.contains("version") &&
        p.at("version").is_number_integer() && p.at("version")==version &&
        p.contains("form") && p.at("form").is_string() &&
        p.at("form").get_ref<const std::string&>()==form;
}

bool canonical_stair(const Entity& entity) {
    return canonical_form(entity,"stair",1,"straight_stair_flight") ||
        canonical_form(entity,"stair",2,"multi_flight_stair");
}

bool canonical_hosted_railing(const Entity& entity) {
    return canonical_form(entity,"railing",2,"stair_flight_railing") ||
        canonical_form(entity,"railing",3,"stair_landing_railing");
}

bool canonical_independent_building(const Entity& entity) {
    return canonical_form(entity,"column",1,"rectangular_column") ||
        canonical_form(entity,"column",1,"circular_column") ||
        canonical_form(entity,"beam",1,"straight_beam") ||
        canonical_form(entity,"railing",1,"straight_railing") ||
        canonical_form(entity,"roof",1,"sloped_roof_panel") ||
        canonical_form(entity,"roof",2,"sloped_roof_panel") ||
        canonical_form(entity,"roof",1,"gable_roof") ||
        canonical_form(entity,"roof",2,"gable_roof") ||
        canonical_form(entity,"roof",1,"hip_roof") ||
        canonical_form(entity,"roof",2,"hip_roof");
}

bool geometry_fields_changed(const nlohmann::json& original,
                             const nlohmann::json& current,
                             std::initializer_list<const char*> fields) {
    for (const auto* key : fields) {
        const auto old = original.find(key), value = current.find(key);
        if ((old == original.end()) != (value == current.end()) ||
            (old != original.end() && *old != *value)) return true;
    }
    return false;
}

bool building_property_geometry_changed(const Entity* before, const Entity& after) {
    // Retain the original owner's admission even if an edit corrupts both
    // schema markers. Incomplete/future transport records remain opaque.
    if (!canonical_independent_building(after) &&
        (!before || !canonical_independent_building(*before))) return false;
    if (!before || before->type != after.type ||
        canonical_independent_building(*before) != canonical_independent_building(after)) return true;
    const auto changed = [&](std::initializer_list<const char*> fields) {
        return geometry_fields_changed(before->properties, after.properties, fields);
    };
    if (changed({"version", "form", "vertical_placement", "layer_id", "floor_id",
                 "building_id", "property_id"})) return true;
    if (after.type == "column") {
        if (changed({"base_center_m", "height_m", "rotation_rad"})) return true;
        return after.properties.at("form") == "rectangular_column"
            ? changed({"width_m", "depth_m"}) : changed({"radius_m"});
    }
    if (after.type == "beam")
        return changed({"start_m", "end_m", "up_dir", "width_m", "depth_m"});
    if (after.type == "railing")
        return changed({"base_position_m", "orientation_rad", "length_m", "height_m",
                        "thickness_m", "post_spacing_m", "host"});
    if (changed({"base_position_m", "orientation_rad", "span_m", "rise_m", "pitch_rad",
                 "overhang_m", "thickness_m"})) return true;
    if (after.properties.at("form") == "sloped_roof_panel"
            ? changed({"run_m"}) : changed({"length_m"})) return true;
    const auto old = before->properties.find("roof_openings");
    const auto current = after.properties.find("roof_openings");
    if ((old == before->properties.end()) != (current == after.properties.end())) return true;
    if (old == before->properties.end()) return false;
    if (!old->is_array() || !current->is_array()) return *old != *current;
    if (old->size() != current->size()) return true;
    for (std::size_t index = 0; index < old->size(); ++index) {
        const auto& original = old->at(index);
        const auto& value = current->at(index);
        if (!original.is_object() || !value.is_object()) {
            if (original != value) return true;
        } else if (geometry_fields_changed(original, value, {"id", "x_m", "y_m", "width_m", "depth_m"})) {
            return true;
        }
    }
    return false; // Opaque nested opening metadata does not regenerate roof solids.
}

std::string join_member_type(ArchitecturalJoinKind kind) {
    switch (kind) {
    case ArchitecturalJoinKind::wall: return "wall";
    case ArchitecturalJoinKind::roof: return "roof";
    }
    throw std::invalid_argument("unknown architectural join kind");
}

void check_join_revision(const DocumentSnapshot& source, Revision expected_revision) {
    if (source.revision() != expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision, "architectural join source revision is stale");
}

std::vector<std::string> join_members(const Entity& entity, ArchitecturalJoinKind kind) {
    return kind == ArchitecturalJoinKind::wall
        ? parse_wall_join(entity.properties, entity.id).wall_ids
        : parse_roof_join(entity.properties, entity.id).roof_ids;
}

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
                value.rotation_radians = std::remainder(
                    value.rotation_radians + transform.rotation_z_radians,
                    2.0 * std::numbers::pi);
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
                for (auto& landing : value.landings) {
                    landing.depth *= transform.scale;
                    landing.thickness *= transform.scale;
                    landing.return_gap *= transform.scale;
                }
            } else if constexpr (std::is_same_v<Object, Railing>) {
                if (value.host || value.landing_host) throw std::invalid_argument(
                    "Hosted railing placement follows its stair; transform the host stair instead");
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
    const PlanarTransform rigid_transform{{0,0},transform.rotation_z_radians,false,false,{transform.x,transform.y}};
    wall.baseline = transform.scale == 1.0 ? transform_segment(wall.baseline,rigid_transform)
                                         : transform_plan_segment(wall.baseline, transform);
    wall.thickness *= transform.scale;
    wall.height *= transform.scale;
    wall.elevation = wall.elevation * transform.scale + transform.z;
    if (wall.slope_rise.has_value()) *wall.slope_rise *= transform.scale;
    if (wall.top_gradient_m_per_m) {
        const auto gradient = *wall.top_gradient_m_per_m;
        const auto cosine = std::cos(transform.rotation_z_radians);
        const auto sine = std::sin(transform.rotation_z_radians);
        wall.top_gradient_m_per_m = Vec2{cosine * gradient.x - sine * gradient.y,
                                        sine * gradient.x + cosine * gradient.y};
        const auto rotated = *wall.top_gradient_m_per_m;
        wall.slope_rise = rotated.x * (wall.baseline.end.x - wall.baseline.start.x) +
                          rotated.y * (wall.baseline.end.y - wall.baseline.start.y);
    }
    for (auto& layer : wall.layers) layer.thickness *= transform.scale;
    for (auto& opening : wall.openings) {
        opening.offset *= transform.scale;
        opening.width *= transform.scale;
        opening.sill *= transform.scale;
        opening.height *= transform.scale;
    }
    validate_wall_semantics(wall);

    Entity result = source;
    const auto receipt_section = result.extensions.find("constraint_authoring");
    if (transform.scale != 1.0 && receipt_section != result.extensions.end() &&
        receipt_section->is_object() && receipt_section->contains("last_length_entry")) {
        throw std::invalid_argument(
            "Wall length receipt requires a length-preserving transform; scaling measured walls is unsupported: " + source.id);
    }
    // Validate and rebase while result still has the original baseline. The
    // exact quantity and all unknown receipt fields remain measurement input.
    rebase_wall_length_receipt(result, wall.baseline);
    if (result.extensions.contains("curve_input")) {
        if (transform.scale != 1.0) {
            throw std::invalid_argument(
                "Curve input provenance requires a length-preserving transform; scaling measured curves is unsupported: " + source.id);
        }
        transform_wall_curve_input(result,rigid_transform);
    }
    update_segment_geometry(result.properties["baseline"], wall.baseline);
    result.properties["thickness_m"] = wall.thickness;
    if (result.properties.contains("thickness")) result.properties["thickness"] = wall.thickness;
    result.properties["height_m"] = wall.height;
    if (result.properties.contains("height")) result.properties["height"] = wall.height;
    result.properties["elevation_m"] = wall.elevation;
    if (result.properties.contains("elevation")) result.properties["elevation"] = wall.elevation;
    if (wall.slope_rise.has_value()) result.properties["slope_rise_m"] = *wall.slope_rise;
    else result.properties.erase("slope_rise_m");
    if (wall.top_gradient_m_per_m)
        result.properties["top_plane"] = wall_top_plane_json(*wall.top_gradient_m_per_m);
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
        if (opening.properties.contains("opening_assembly")) {
            auto assembly = parse_opening_assembly(opening.properties.at("opening_assembly"));
            assembly.frame_width_m *= transform.scale;
            assembly.frame_depth_m *= transform.scale;
            assembly.panel_thickness_m *= transform.scale;
            assembly.glazing_thickness_m *= transform.scale;
            assembly.inset_m *= transform.scale;
            opening.properties["opening_assembly"] = opening_assembly_json(assembly);
        }
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
    DocumentRoomFootprint room;
    std::string error;
    const bool complete = has_document_room_volume_fields(source);
    if (complete) {
        RoomVolume volume;
        if (!read_document_room(source, volume, error)) throw std::invalid_argument(error);
        room = {std::move(volume.boundary),std::move(volume.holes)};
    } else if (!read_document_room_footprint(source, room, error)) {
        throw std::invalid_argument(error);
    }
    if (transform.z != 0 && !source.properties.contains("elevation_m") &&
        !source.properties.contains("elevation"))
        throw std::invalid_argument("Room Z movement requires an authored elevation");
    room.boundary = transform_plan_boundary(room.boundary, transform);
    for (auto& hole : room.holes) hole = transform_plan_boundary(hole, transform);
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
    // Canonical-first decoding and alias synchronization match the existing
    // volume path. A missing measurement stays absent; aliases describe the
    // same authored number rather than independent dimensions.
    const auto transform_dimension = [&](const char* canonical, const char* legacy, bool height) {
        auto found = source.properties.find(canonical);
        if (found == source.properties.end()) found = source.properties.find(legacy);
        if (found == source.properties.end()) return;
        if (!found->is_number()) throw std::invalid_argument(std::string(canonical) + " must be finite");
        const double value = found->get<double>();
        const double transformed = value * transform.scale + (height ? 0 : transform.z);
        if (!std::isfinite(value) || !std::isfinite(transformed) ||
            (height && (value <= default_geometry_tolerance_metres ||
                        transformed <= default_geometry_tolerance_metres)))
            throw std::invalid_argument(std::string(canonical) + " must be finite and height positive");
        if (result.properties.contains(canonical)) result.properties[canonical] = transformed;
        if (result.properties.contains(legacy)) result.properties[legacy] = transformed;
    };
    transform_dimension("height_m", "height", true);
    transform_dimension("elevation_m", "elevation", false);
    if (complete) {
        RoomVolume volume;
        if (!read_document_room(result, volume, error)) throw std::invalid_argument(error);
    } else if (!read_document_room_footprint(result, room, error)) {
        throw std::invalid_argument(error);
    }
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
        !source.properties.at("level_connection").is_null() &&
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
    // Canonical codecs describe geometry, while child records may also carry
    // opaque source and quantity metadata. Keep those records and update only
    // the fields owned by the codec, without recursively rewriting strings.
    for (const auto* key : {"flights", "landings"}) {
        if (!canonical.properties.contains(key)) continue;
        auto records = source.properties.at(key);
        const auto& encoded = canonical.properties.at(key);
        for (std::size_t i=0; i<encoded.size(); ++i)
            for (const auto& [field,value] : encoded.at(i).items()) records.at(i)[field]=value;
        result.properties[key]=std::move(records);
    }
    if (source.type=="stair" && source.properties.contains("top_landing") && source.properties.at("top_landing").is_object() &&
        canonical.properties.at("top_landing").is_object()) {
        result.properties["top_landing"] = source.properties.at("top_landing");
        for (const auto& [key,value] : canonical.properties.at("top_landing").items())
            result.properties["top_landing"][key]=value;
    }
    if(source.type=="stair" && source.properties.contains("level_connection") &&
       source.properties.at("level_connection").is_object() && canonical.properties.contains("level_connection")) {
        result.properties["level_connection"]=source.properties.at("level_connection");
        for(const auto& [key,value]:canonical.properties.at("level_connection").items())
            result.properties["level_connection"][key]=value;
    }
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

std::vector<std::string> hosted_railing_ids(const EntityState& entities, const std::string& stair_id) {
    std::vector<std::string> result;
    for (const auto& [id,entity] : entities) {
        if (!canonical_hosted_railing(entity)) continue;
        const auto railing=decode_railing_properties(id,entity.properties);
        if ((railing.host?railing.host->stair_id:railing.landing_host->stair_id)==stair_id) result.push_back(id);
    }
    return result;
}

void invalidate_changed_receipts(const Entity& before, Entity& after) {
    const auto entries=before.properties.find("quantity_entries");
    if (entries==before.properties.end() || !entries->is_object()) return;
    for (const auto& [pointer,value] : entries->items()) {
        (void)value;
        try {
            const nlohmann::json::json_pointer path(pointer);
            if (before.properties.contains(path) &&
                (!after.properties.contains(path) || before.properties.at(path)!=after.properties.at(path)))
                after.properties["quantity_entries"].erase(pointer);
        } catch (const nlohmann::json::exception&) { /* Preserve opaque legacy paths. */ }
    }
}

EntityState apply_operations(const DocumentSnapshot& source,
                             const ArchitecturalTransaction& transaction) {
    auto entities = copy_entities(source);
    const auto& initial = transaction.existing_ids();
    for (const auto& id : initial) {
        if (!entities.contains(id)) throw std::invalid_argument("architectural transaction source is stale");
    }
    std::map<std::string,std::vector<ArchitecturalTransform>,std::less<>> transforms;
    for (const auto& operation : transaction.operations())
        if (operation.action==ArchitecturalAction::transform)
            transforms[operation.object_id].push_back(*operation.transform);
    std::set<std::string,std::less<>> cascade_deleted_rails;
    std::map<std::string,std::string,std::less<>> selected_rail_clones;
    std::map<std::string,std::size_t,std::less<>> stair_clone_counts;
    for (const auto& operation : transaction.operations()) {
        const auto original=entities.find(operation.object_id);
        if (operation.action==ArchitecturalAction::duplicate && original!=entities.end() &&
            canonical_form(original->second,"stair",2,"multi_flight_stair"))
            ++stair_clone_counts[operation.object_id];
    }
    for (const auto& operation : transaction.operations()) {
        const auto original=entities.find(operation.object_id);
        if (operation.action!=ArchitecturalAction::duplicate || original==entities.end() ||
            !canonical_hosted_railing(original->second)) continue;
        const auto rail=decode_railing_properties(original->first,original->second.properties);
        const auto host=stair_clone_counts.find(rail.host?rail.host->stair_id:rail.landing_host->stair_id);
        if (host==stair_clone_counts.end()) continue;
        if (host->second!=1 || !selected_rail_clones.emplace(operation.object_id,operation.duplicate_id).second)
            throw std::invalid_argument("Selected stair and railing clones require one unambiguous host clone");
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
            if (canonical_hosted_railing(found->second)) {
                const auto railing=decode_railing_properties(found->first,found->second.properties);
                const auto host=transforms.find(railing.host?railing.host->stair_id:railing.landing_host->stair_id);
                const auto own=transforms.find(found->first);
                if (host==transforms.end() || host->second.size()!=1 || own->second.size()!=1 ||
                    transform_json(host->second.front())!=transform_json(*operation.transform))
                    throw std::invalid_argument("Hosted railing placement follows its stair; select the host with the same transform");
                // Its host applies placement and scales authored rail dimensions once,
                // independently of selected-operation order.
                break;
            }
            if (canonical_stair(found->second)) {
                if (transforms.at(found->first).size()!=1)
                    throw std::invalid_argument("A stair requires one unambiguous transform per transaction");
                for (const auto& rail_id : hosted_railing_ids(entities,found->first)) {
                    auto& rail=entities.at(rail_id);
                    const auto before=rail;
                    for (const auto* field : {"height_m","thickness_m","post_spacing_m"})
                        scale_property(rail.properties,field,nullptr,operation.transform->scale);
                    invalidate_changed_receipts(before,rail);
                }
            }
            if (found->second.type == "assembly_instance") {
                auto value = decode_document_assembly_instance(found->second);
                const auto& movement = *operation.transform;
                const AssemblyTransform outer{{movement.x, movement.y, movement.z},
                    movement.rotation_z_radians, movement.scale};
                value.instance.root_transform = compose_assembly_transform(outer, *value.instance.root_transform);
                found->second = encode_document_assembly_instance(found->second, value);
            } else if (can_recognize_building_entity_type(found->second.type)) {
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
            // A selected dependent clone is created with the host, including
            // its remapped flight identity, regardless of selection order.
            if (selected_rail_clones.contains(operation.object_id)) break;
            auto found = entities.find(operation.object_id);
            if (found == entities.end()) throw std::invalid_argument("architectural duplicate source is missing");
            if (entities.contains(operation.duplicate_id)) throw std::invalid_argument("architectural duplicate ID already exists");
            auto copy = found->second;
            copy.id = operation.duplicate_id;
            if (copy.type == "assembly_instance") {
                auto value = decode_document_assembly_instance(found->second);
                value.instance.id = copy.id;
                copy = encode_document_assembly_instance(copy, value);
            }
            if (canonical_form(copy,"stair",2,"multi_flight_stair")) {
                std::map<std::string,std::string,std::less<>> children;
                for (const auto* key : {"flights","landings"}) {
                    for (auto& record : copy.properties.at(key)) {
                        const auto old=record.at("id").get<std::string>();
                        const auto fresh=make_stable_id();
                        children.emplace(old,fresh); record["id"]=fresh;
                    }
                }
                for (const auto& rail_id : hosted_railing_ids(entities,found->first)) {
                    auto rail=entities.at(rail_id);
                    const auto selected=selected_rail_clones.find(rail_id);
                    rail.id=selected==selected_rail_clones.end()?make_stable_id():selected->second;
                    if (entities.contains(rail.id)) throw std::invalid_argument("Hosted railing duplicate identity already exists");
                    auto& host=rail.properties.at("host");
                    host["stair_id"]=copy.id;
                    // Only canonical identity fields are remapped. Nested
                    // application metadata is retained verbatim.
                    if(canonical_form(rail,"railing",3,"stair_landing_railing")) {
                        host["incoming_flight_id"]=children.at(host.at("incoming_flight_id").get<std::string>());
                        if(host.at("role")=="connecting") {
                            host["landing_id"]=children.at(host.at("landing_id").get<std::string>());
                            host["outgoing_flight_id"]=children.at(host.at("outgoing_flight_id").get<std::string>());
                        }
                    } else host["flight_id"]=children.at(host.at("flight_id").get<std::string>());
                    const auto rail_id_copy=rail.id;
                    entities.emplace(rail_id_copy,std::move(rail));
                }
            }
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
            if (found==entities.end() && cascade_deleted_rails.contains(operation.object_id)) break;
            if (found == entities.end()) throw std::invalid_argument("architectural delete target is missing");
            if (canonical_stair(found->second)) {
                for (const auto& id : hosted_railing_ids(entities,found->first)) {
                    entities.erase(id); cascade_deleted_rails.insert(id);
                }
            }
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
            auto changed=after->second;
            if (before!=source.entities().end() &&
                (canonical_stair(changed) || canonical_hosted_railing(changed)))
                invalidate_changed_receipts(before->second,changed);
            command.entity_changes.push_back(EntityChange::upsert(std::move(changed)));
        }
    }
    if (!command.entity_changes.empty()) {
        const auto preview=Document::preview_command(source,command);
        validate_architectural_geometry_changes(source, preview);
        std::set<std::string, std::less<>> changed_roofs, changed_roof_joins;
        for (const auto& change : command.entity_changes) {
            if (change.kind != EntityChangeKind::upsert) continue;
            const auto& entity = preview.entities().at(change.entity.id);
            const auto found = source.entities().find(entity.id);
            const auto* before = found == source.entities().end() ? nullptr : &found->second;
            if (entity.type == "roof_join" && (!before || geometry_fields_changed(
                before->properties, entity.properties, {"style", "roof_ids"})))
                changed_roof_joins.insert(entity.id);
            if (!building_property_geometry_changed(before, entity)) continue;
            // The shared physical admission already decodes beams. Other
            // independent forms retain their native codec admission here;
            // hosted conversions retain the host-map admission below.
            if (entity.type != "beam")
                (void)decode_building_entity(resolve_vertical_placement(preview, entity));
            if (entity.type == "roof") changed_roofs.insert(entity.id);
        }
        if (!changed_roofs.empty() || !changed_roof_joins.empty()) {
            for (const auto& [id, entity] : preview.entities()) {
                if (entity.type != "roof_join") continue;
                const auto join = parse_roof_join(entity.properties, id);
                if (!changed_roof_joins.contains(id) && std::none_of(join.roof_ids.begin(), join.roof_ids.end(),
                    [&](const auto& member) { return changed_roofs.contains(member); })) continue;
                std::vector<TopoDS_Shape> members;
                members.reserve(join.roof_ids.size());
                for (const auto& member_id : join.roof_ids) {
                    const auto effective = resolve_vertical_placement(preview, preview.entities().at(member_id));
                    members.push_back(make_building_shape(decode_building_entity(effective)));
                }
                (void)make_roof_join(join, members);
            }
        }
        for (const auto& [id,entity] : preview.entities()) {
            (void)id;
            if (!canonical_stair(entity) && !canonical_hosted_railing(entity)) continue;
            const auto effective=resolve_vertical_placement(preview,entity);
            (void)make_building_shape(decode_building_entity(effective),preview.entities());
        }
    }
    return command;
}

}  // namespace

void validate_architectural_geometry_changes(
    const DocumentSnapshot& source, const DocumentSnapshot& candidate,
    const std::vector<std::string>& required_ids) {
    const auto& entities = candidate.entities();
    std::set<std::string, std::less<>> host_ids, required_hosts, slab_ids, room_ids, full_room_ids, beam_ids;
    const auto include = [&](const Entity& entity, bool required) {
        const auto& p = entity.properties;
        if (entity.type == "wall" && (required || p.contains("baseline"))) {
            host_ids.insert(entity.id);
            if (required) required_hosts.insert(entity.id);
        } else if (entity.type == "opening" && (required || p.contains("wall_id"))) {
            std::string host_id, error;
            if (!read_document_wall_id(entity, host_id, error))
                throw std::invalid_argument("Opening " + entity.id + ": " + error);
            host_ids.insert(host_id);
            if (required) required_hosts.insert(std::move(host_id));
        } else if (entity.type == "slab" && (required || p.contains("boundary"))) {
            slab_ids.insert(entity.id);
        } else if (entity.type == "room" &&
                   (required || p.contains("boundary") || p.contains("segments"))) {
            room_ids.insert(entity.id);
            if (required || has_document_room_volume_fields(entity)) full_room_ids.insert(entity.id);
        } else if (entity.type == "beam" &&
                   (required || canonical_form(entity, "beam", 1, "straight_beam"))) {
            beam_ids.insert(entity.id);
        }
    };
    const auto changed = [](const Entity* before, const Entity& after,
                            std::initializer_list<const char*> fields) {
        if (!before || before->type != after.type) return true;
        for (const auto* key : fields) {
            const auto old = before->properties.find(key);
            const auto current = after.properties.find(key);
            if ((old == before->properties.end()) != (current == after.properties.end()) ||
                (old != before->properties.end() && *old != *current)) return true;
        }
        return false;
    };
    for (const auto& [id, entity] : entities) {
        const auto found = source.entities().find(id);
        const auto* before = found == source.entities().end() ? nullptr : &found->second;
        bool physical_change = false;
        if (entity.type == "wall")
            physical_change = changed(before, entity, {"baseline", "thickness_m", "thickness",
                "height_m", "height", "elevation_m", "elevation", "slope_rise_m", "slope_rise", "top_plane",
                "layers", "vertical_placement", "layer_id", "floor_id", "building_id", "property_id"});
        else if (entity.type == "opening")
            physical_change = changed(before, entity, {"wall_id", "offset_m", "offset", "width_m",
                "width", "sill_m", "sill", "height_m", "height", "opening_kind",
                "opening_assembly", "door_operation"});
        else if (entity.type == "slab")
            physical_change = changed(before, entity, {"boundary", "holes", "thickness_m", "thickness",
                "elevation_m", "elevation", "layers", "element_kind", "vertical_placement", "layer_id",
                "floor_id", "building_id", "property_id"});
        else if (entity.type == "room")
            physical_change = changed(before, entity, {"boundary", "segments", "holes", "height_m",
                "height", "elevation_m", "elevation", "vertical_placement", "layer_id",
                "floor_id", "building_id", "property_id"});
        else if (entity.type == "beam" ||
                 (before && canonical_form(*before, "beam", 1, "straight_beam")))
            physical_change = building_property_geometry_changed(before, entity);
        if (!physical_change) continue;
        if (entity.type == "opening" && before && before->properties.contains("wall_id") &&
            !entity.properties.contains("wall_id"))
            throw std::invalid_argument("The edited opening lost its wall host: " + id);
        include(entity, false);
        if (before) include(*before, false);
    }
    for (const auto& [id, entity] : source.entities())
        if (!entities.contains(id)) include(entity, false);
    for (const auto& id : required_ids) {
        const auto found = entities.find(id);
        if (found == entities.end())
            throw std::invalid_argument("The edited physical object is missing: " + id);
        const auto& type = found->second.type;
        if (type != "wall" && type != "opening" && type != "slab" && type != "room" && type != "beam")
            throw std::invalid_argument("The edited object has no supported physical descriptor: " + id);
        include(found->second, true);
    }

    std::vector<WallJoin> affected_joins;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "wall_join") continue;
        const auto join = parse_wall_join(entity.properties, id);
        const auto before = source.entities().find(id);
        const bool edited = before == source.entities().end() || before->second.properties != entity.properties;
        if (!edited && std::none_of(join.wall_ids.begin(), join.wall_ids.end(),
            [&](const auto& member) { return host_ids.contains(member); })) continue;
        affected_joins.push_back(join);
        // A fused join is a physical relationship; all of its members must
        // remain admitted even if only one member or its opening was edited.
        host_ids.insert(join.wall_ids.begin(), join.wall_ids.end());
        required_hosts.insert(join.wall_ids.begin(), join.wall_ids.end());
    }

    std::map<std::string, std::vector<const Entity*>, std::less<>> openings_by_host;
    if (!host_ids.empty()) {
        for (const auto& [id, entity] : entities) {
            (void)id;
            if (entity.type != "opening") continue;
            const auto host = entity.properties.find("wall_id");
            if (host != entity.properties.end() && host->is_string() &&
                host_ids.contains(host->get_ref<const std::string&>()))
                openings_by_host[host->get_ref<const std::string&>()].push_back(&entity);
        }
    }
    std::map<std::string, Wall, std::less<>> admitted_join_walls;
    for (const auto& host_id : host_ids) {
        const auto host = entities.find(host_id);
        const auto& openings = openings_by_host[host_id];
        // Deleting a wall and its hosted graph is valid. A surviving opening
        // must still resolve to a physical wall in the completed candidate.
        if (host == entities.end()) {
            if (required_hosts.contains(host_id) || !openings.empty())
                throw std::invalid_argument("The edited opening has no host wall: " + host_id);
            continue;
        }
        if (host->second.type != "wall")
            throw std::invalid_argument("The edited opening host is not a wall: " + host_id);
        const auto before = source.entities().find(host_id);
        if (!required_hosts.contains(host_id) && !host->second.properties.contains("baseline") &&
            (before == source.entities().end() || !before->second.properties.contains("baseline")))
            continue; // Preserve the existing incomplete legacy transport contract.
        Wall wall;
        std::string error;
        const auto effective = resolve_vertical_placement(candidate, host->second);
        if (!read_document_wall(effective, openings, wall, error))
            throw std::invalid_argument("Wall " + host_id + ": " +
                (error.empty() ? "a physical baseline is required" : error));
        (void)make_wall(wall);
        for (const auto* opening : openings) {
            std::optional<OpeningAssembly> assembly;
            const auto explicit_assembly = opening->properties.find("opening_assembly");
            const auto kind = opening->properties.find("opening_kind");
            if (explicit_assembly != opening->properties.end())
                assembly = parse_opening_assembly(*explicit_assembly);
            else if (kind != opening->properties.end() && kind->is_string()) {
                const auto parsed = parse_opening_assembly_kind(kind->get_ref<const std::string&>());
                if (parsed) assembly = default_opening_assembly(*parsed);
            }
            if (!assembly) continue; // Bare cuts and opaque historical records have no manufactured frame.
            const auto hosted = std::find_if(wall.openings.begin(), wall.openings.end(),
                [&](const auto& item) { return item.id == opening->id; });
            if (hosted == wall.openings.end())
                throw std::invalid_argument("The edited opening lost its wall host: " + opening->id);
            std::optional<DoorOperation> operation;
            if (opening->properties.contains("door_operation"))
                operation = decode_door_operation(opening->properties.at("door_operation"));
            (void)make_opening_assembly(wall, *hosted, *assembly, operation);
        }
        if (!affected_joins.empty()) admitted_join_walls.emplace(host_id, std::move(wall));
    }
    for (const auto& join : affected_joins) {
        std::vector<Wall> members;
        members.reserve(join.wall_ids.size());
        for (const auto& member_id : join.wall_ids) members.push_back(admitted_join_walls.at(member_id));
        (void)make_wall_join(join, members);
    }
    for (const auto& id : slab_ids) {
        const auto found = entities.find(id);
        if (found == entities.end()) continue;
        Slab slab;
        std::string error;
        if (!read_document_slab(resolve_vertical_placement(candidate, found->second), slab, error))
            throw std::invalid_argument("Slab " + id + ": " + error);
        (void)make_slab(slab);
    }
    for (const auto& id : room_ids) {
        const auto found = entities.find(id);
        if (found == entities.end()) continue;
        std::string error;
        if (full_room_ids.contains(id)) {
            RoomVolume room;
            if (!read_document_room(resolve_vertical_placement(candidate, found->second), room, error))
                throw std::invalid_argument("Room " + id + ": " + error);
        } else {
            DocumentRoomFootprint footprint;
            if (!read_document_room_footprint(found->second, footprint, error))
                throw std::invalid_argument("Room " + id + ": " + error);
        }
    }
    for (const auto& id : beam_ids) {
        const auto found = entities.find(id);
        if (found == entities.end()) continue;
        // The canonical codec admits the native beam, including a stable
        // section frame, after placement resolves against the full candidate.
        const auto object = decode_building_entity(resolve_vertical_placement(candidate, found->second));
        if (!std::holds_alternative<Beam>(object))
            throw std::invalid_argument("The edited beam lost its physical descriptor: " + id);
    }
}

ApplyEntityChanges architectural_join_create_command(const DocumentSnapshot& source,
    const std::string& join_id, const std::vector<std::string>& member_ids,
    ArchitecturalJoinKind kind, Revision expected_revision) {
    check_join_revision(source, expected_revision);
    const auto member_type = join_member_type(kind);
    const auto join_type = member_type + "_join";
    if (source.entities().contains(join_id))
        throw std::invalid_argument("architectural join identity already exists");
    const auto properties = kind == ArchitecturalJoinKind::wall
        ? wall_join_json(WallJoin{join_id, member_ids})
        : roof_join_json(RoofJoin{join_id, member_ids});
    // The semantic serializers check count and uniqueness before geometry work.
    for (const auto& member_id : member_ids)
        (void)semantic_entity(source, member_id, member_type, expected_revision);
    const std::set<std::string> selected(member_ids.begin(), member_ids.end());
    for (const auto& [id, entity] : source.entities()) {
        if (entity.type != join_type) continue;
        for (const auto& member : join_members(entity, kind)) {
            if (selected.contains(member))
                throw std::invalid_argument("architectural source already belongs to a join: " + member);
        }
    }
    if (kind == ArchitecturalJoinKind::wall) {
        std::vector<Wall> walls;
        for (const auto& member_id : member_ids) {
            const auto resolved = resolve_vertical_placement(source, source.entities().at(member_id));
            std::vector<const Entity*> openings;
            for (const auto& opening_id : hosted_opening_ids(source.entities(), member_id))
                openings.push_back(&source.entities().at(opening_id));
            Wall wall;
            std::string error;
            if (!read_document_wall(resolved, openings, wall, error))
                throw std::invalid_argument(error);
            walls.push_back(std::move(wall));
        }
        (void)make_wall_join(WallJoin{join_id, member_ids}, walls);
    } else {
        std::vector<TopoDS_Shape> roofs;
        for (const auto& member_id : member_ids) {
            const auto resolved = resolve_vertical_placement(source, source.entities().at(member_id));
            roofs.push_back(make_building_shape(decode_building_entity(resolved)));
        }
        (void)make_roof_join(RoofJoin{join_id, member_ids}, roofs);
    }
    auto entity = Entity::create(join_type, properties);
    entity.id = join_id;
    ApplyEntityChanges command{expected_revision, {EntityChange::upsert(std::move(entity))}, {},
        "Join " + member_type + "s"};
    (void)Document::preview_command(source, Command{command});
    return command;
}

ApplyEntityChanges architectural_join_remove_command(const DocumentSnapshot& source,
    const std::vector<std::string>& selected_ids, ArchitecturalJoinKind kind,
    Revision expected_revision) {
    check_join_revision(source, expected_revision);
    const auto member_type = join_member_type(kind);
    const auto join_type = member_type + "_join";
    if (selected_ids.empty()) throw std::invalid_argument("architectural join selection is empty");
    std::map<std::string, std::string> membership;
    for (const auto& [id, entity] : source.entities()) {
        if (entity.type != join_type) continue;
        for (const auto& member : join_members(entity, kind)) membership.emplace(member, id);
    }
    std::set<std::string> erase_ids;
    for (const auto& selected_id : selected_ids) {
        const auto found = source.entities().find(selected_id);
        if (found == source.entities().end())
            throw std::invalid_argument("architectural join selection target is missing");
        if (found->second.type == join_type) {
            erase_ids.insert(selected_id);
        } else if (found->second.type == member_type && membership.contains(selected_id)) {
            erase_ids.insert(membership.at(selected_id));
        } else {
            throw std::invalid_argument("architectural join selection contains an unrelated object");
        }
    }
    ApplyEntityChanges command{expected_revision, {}, {}, "Remove " + member_type + " joins"};
    for (const auto& id : erase_ids) command.entity_changes.push_back(EntityChange::erase(id));
    (void)Document::preview_command(source, Command{command});
    return command;
}

Entity resized_room_volume_entity(const Entity& source, const RoomDimensionEdit& edit) {
    if (source.type != "room")
        throw std::invalid_argument("room dimension edit target must be a room");
    if (edit.width_metres.has_value() != edit.depth_metres.has_value())
        throw std::invalid_argument("room width and depth must be supplied together");
    if (!std::isfinite(edit.height_metres) || edit.height_metres <= 0 ||
        !std::isfinite(edit.elevation_metres))
        throw std::invalid_argument("room height must be positive and dimensions finite");
    double anchor_fraction{};
    switch (edit.anchor) {
    case RoomFootprintAnchor::first_corner: anchor_fraction = 0; break;
    case RoomFootprintAnchor::center: anchor_fraction = 0.5; break;
    case RoomFootprintAnchor::opposite_corner: anchor_fraction = 1; break;
    default: throw std::invalid_argument("unknown room footprint anchor");
    }
    RoomVolume room;
    std::string error;
    const bool source_is_volume = has_document_room_volume_fields(source);
    if (source_is_volume) {
        if (!read_document_room(source, room, error)) throw std::invalid_argument(error);
    } else {
        DocumentRoomFootprint footprint;
        if (!read_document_room_footprint(source, footprint, error)) throw std::invalid_argument(error);
        room.id = source.id;
        room.boundary = std::move(footprint.boundary);
        room.holes = std::move(footprint.holes);
        // Mandatory measured replacement fields explicitly promote the room.
        room.height = edit.height_metres;
        room.elevation = edit.elevation_metres;
    }
    Entity result = source;
    if (edit.width_metres) {
        const double width = *edit.width_metres, depth = *edit.depth_metres;
        if (!std::isfinite(width) || !std::isfinite(depth) || width <= 0 || depth <= 0)
            throw std::invalid_argument("room width and depth must be positive and finite");
        const auto dimensions = room_footprint_rectangle_dimensions({room.boundary,room.holes});
        if (!dimensions)
            throw std::invalid_argument("room footprint resize requires a closed straight rectangle without holes");
        const double old_width = dimensions->x, old_depth = dimensions->y;
        const auto origin = room.boundary[0].start;
        const auto& first = room.boundary[0];
        const auto& second = room.boundary[1];
        const Vec2 u{(first.end.x-first.start.x)/old_width, (first.end.y-first.start.y)/old_width};
        const Vec2 v{(second.end.x-second.start.x)/old_depth, (second.end.y-second.start.y)/old_depth};
        const auto corner = [&](double x, double y) {
            return Vec2{origin.x + u.x*x + v.x*y, origin.y + u.y*x + v.y*y};
        };
        const auto unchanged_length = [](double requested, double current) {
            const double scale = std::max({1.0, std::abs(requested), std::abs(current)});
            return std::abs(requested - current) <= 1e-13 * scale;
        };
        if (!unchanged_length(width, old_width) || !unchanged_length(depth, old_depth)) {
            const double offset_x = anchor_fraction * (old_width-width);
            const double offset_y = anchor_fraction * (old_depth-depth);
            const std::array<Vec2, 4> resized{corner(offset_x, offset_y),
                corner(offset_x+width, offset_y), corner(offset_x+width, offset_y+depth),
                corner(offset_x, offset_y+depth)};
            for (std::size_t i = 0; i < 4; ++i) {
                room.boundary[i].start = resized[i];
                room.boundary[i].end = resized[(i+1)%4];
            }
            for (const auto* key : {"boundary", "segments"}) {
                if (result.properties.contains(key))
                    result.properties[key] = updated_boundary_geometry(source.properties.at(key), room.boundary);
            }
        }
    }
    if (!source_is_volume || result.properties.contains("height_m") || !result.properties.contains("height"))
        result.properties["height_m"] = edit.height_metres;
    if (!source_is_volume || result.properties.contains("elevation_m") || !result.properties.contains("elevation"))
        result.properties["elevation_m"] = edit.elevation_metres;
    if (result.properties.contains("height")) result.properties["height"] = edit.height_metres;
    if (result.properties.contains("elevation")) result.properties["elevation"] = edit.elevation_metres;
    // Decode the exact returned JSON, including aliases, through the shared
    // solid admission path. No caller state changes if any validation fails.
    if (!read_document_room(result, room, error)) throw std::invalid_argument(error);
    return result;
}

ApplyEntityChanges room_dimension_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const RoomDimensionEdit& edit, Revision expected_revision) {
    auto entity = semantic_entity(source, entity_id, "room", expected_revision);
    entity = resized_room_volume_entity(entity, edit);
    ApplyEntityChanges command{expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Resize room volume"};
    const auto candidate = Document::preview_command(source, Command{command});
    // The supplied dimensions are local measurements. Admission also resolves
    // the completed floor/level placement before any caller can publish them.
    validate_architectural_geometry_changes(source, candidate, {entity_id});
    return command;
}

ApplyEntityChanges beam_endpoint_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const BeamEndpointEdit& edit, Revision expected_revision) {
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    auto entity = semantic_entity(source, entity_id, "beam", expected_revision);
    if (edit.endpoint != BeamEndpoint::start && edit.endpoint != BeamEndpoint::end)
        throw std::invalid_argument("Beam endpoint role is invalid");
    if (!std::isfinite(edit.proposed_position.x) || !std::isfinite(edit.proposed_position.y))
        throw std::invalid_argument("Beam endpoint target must be finite");
    if (!canonical_form(entity, "beam", 1, "straight_beam"))
        throw std::invalid_argument("Beam endpoint editing requires a canonical straight beam");
    const auto beam = std::get<Beam>(decode_building_entity(entity));
    const auto& endpoint = edit.endpoint == BeamEndpoint::start ? beam.start : beam.end;
    if (endpoint.x == edit.proposed_position.x && endpoint.y == edit.proposed_position.y)
        throw std::invalid_argument("Beam endpoint target makes no document change");
    // Preserve the original three-coordinate array and its exact Z field;
    // re-encoding the whole beam would replace opaque source properties.
    auto& coordinates = entity.properties.at(edit.endpoint == BeamEndpoint::start ? "start_m" : "end_m");
    coordinates.at(0) = edit.proposed_position.x;
    coordinates.at(1) = edit.proposed_position.y;
    ApplyEntityChanges command{expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Move beam endpoint"};
    const auto candidate = Document::preview_command(source, Command{command});
    validate_architectural_geometry_changes(source, candidate, {entity_id});
    return command;
}

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
