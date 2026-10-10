#include "sketch/architectural_document_adapter.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/corner_window_edit.hpp"
#include "sketch/corner_window_removal.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/structural_object_edit.hpp"
#include "sketch/stair_transform.hpp"
#include "sketch/wall_semantics.hpp"
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/phase_roof_edit.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
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
        canonical_form(entity,"stair",2,"multi_flight_stair") ||
        canonical_form(entity,"stair",3,"multi_flight_stair") ||
        canonical_form(entity,"stair",4,"multi_flight_stair");
}

bool canonical_multi_flight_stair(const Entity& entity) {
    return canonical_form(entity,"stair",2,"multi_flight_stair") ||
        canonical_form(entity,"stair",3,"multi_flight_stair") ||
        canonical_form(entity,"stair",4,"multi_flight_stair");
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

std::array<double, 2> planar_rotation(double angle) {
    // A pair of global flips is an exact half-turn. Avoid sin(pi) roundoff
    // multiplied by distant source coordinates or a shared model pivot.
    if (std::abs(angle) == std::numbers::pi) return {-1.0, 0.0};
    return {std::cos(angle), std::sin(angle)};
}

struct NormalizedGroupTransform {
    ArchitecturalTransform affine;
    bool flip_horizontal{};
    bool flip_vertical{};
    bool identity{};
};

NormalizedGroupTransform normalize_group_transform(const ArchitecturalGroupTransform& transform) {
    const auto finite_point=[](Vec3 point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    if (!finite_point(transform.pivot) || !finite_point(transform.offset) ||
        !std::isfinite(transform.rotation_z_radians) ||
        !std::isfinite(transform.scale) || !(transform.scale>0.0))
        throw std::invalid_argument("Architectural group transforms must be finite with a positive scale.");
    // Whole turns and double flips retain exact identity/half-turn operators.
    const bool reflected=transform.flip_horizontal!=transform.flip_vertical;
    const auto angle=std::remainder(std::remainder(transform.rotation_z_radians,
        2.0*std::numbers::pi) + (transform.flip_horizontal && transform.flip_vertical
            ? std::numbers::pi : 0.0), 2.0*std::numbers::pi);
    const auto [c,s]=planar_rotation(angle);
    const bool horizontal=reflected && transform.flip_horizontal;
    const bool vertical=reflected && transform.flip_vertical;
    const auto hx=horizontal?-1.0:1.0;
    const auto hy=vertical?-1.0:1.0;
    ArchitecturalTransform affine;
    // Difference form retains small offsets during pure translation.
    affine.x=transform.offset.x+(1.0-transform.scale*hx)*transform.pivot.x+
        transform.scale*hx*((1.0-c)*transform.pivot.x+s*transform.pivot.y);
    affine.y=transform.offset.y+(1.0-transform.scale*hy)*transform.pivot.y+
        transform.scale*hy*((1.0-c)*transform.pivot.y-s*transform.pivot.x);
    affine.z=transform.offset.z+(1.0-transform.scale)*transform.pivot.z;
    affine.rotation_z_radians=angle;
    affine.scale=transform.scale;
    if (!std::isfinite(affine.x) || !std::isfinite(affine.y) || !std::isfinite(affine.z))
        throw std::invalid_argument("The architectural group pivot exceeds the supported transform range.");
    const bool identity=transform.scale==1.0 && transform.offset.x==0.0 && transform.offset.y==0.0 &&
        transform.offset.z==0.0 && angle==0.0 && !reflected;
    return {affine,horizontal,vertical,identity};
}

bool equivalent_group_transform(const NormalizedGroupTransform& a, const NormalizedGroupTransform& b) {
    if ((a.flip_horizontal!=a.flip_vertical)!=(b.flip_horizontal!=b.flip_vertical)) return false;
    const auto coefficients=[](const NormalizedGroupTransform& intent) {
        const auto& t=intent.affine;
        const auto [c,s]=planar_rotation(t.rotation_z_radians);
        const auto hx=intent.flip_horizontal?-1.0:1.0;
        const auto hy=intent.flip_vertical?-1.0:1.0;
        return std::array<double,8>{t.scale*hx*c,-t.scale*hx*s,
            t.scale*hy*s,t.scale*hy*c,t.scale,t.x,t.y,t.z};
    };
    const auto left=coefficients(a), right=coefficients(b);
    for (std::size_t i=0; i<left.size(); ++i) {
        const auto tolerance=64.0*std::numeric_limits<double>::epsilon()*
            std::max({1.0,std::abs(left[i]),std::abs(right[i])});
        if (std::abs(left[i]-right[i])>tolerance) return false;
    }
    return true;
}

// Actual-source producers may share a hosted catalog while touching disjoint
// instance rows. Compose only those raw row changes; definitions, overrides,
// opaque data, order and every envelope field remain actual-source authority.
Entity merge_group_hosted_catalog(const Entity& source, const Entity& accumulated, const Entity& incoming) {
    const auto exact_json=[](const nlohmann::json& a,const nlohmann::json& b) {
        return a==b && a.dump()==b.dump();
    };
    const auto envelope=[&](const Entity& entity) {
        auto value=entity;
        value.properties.erase("model");
        return value;
    };
    const auto actual=envelope(source), left=envelope(accumulated), right=envelope(incoming);
    if (source.type!="assembly_model" || actual!=left || actual!=right ||
        !exact_json(actual.properties,left.properties) || !exact_json(actual.properties,right.properties) ||
        !exact_json(actual.extensions,left.extensions) || !exact_json(actual.extensions,right.extensions))
        throw std::invalid_argument("Architectural hosted producers disagree on the actual catalog envelope");
    auto result=accumulated;
    result.properties.at("model")=merge_disjoint_hosted_assembly_models(
        source.properties.at("model"),accumulated.properties.at("model"),incoming.properties.at("model"));
    return result;
}

Vec3 transform_point(const Vec3& point, const ArchitecturalTransform& transform,
                     bool flip_horizontal = false, bool flip_vertical = false) {
    const auto [cosine, sine] = planar_rotation(transform.rotation_z_radians);
    const auto scaled_x = point.x * transform.scale;
    const auto scaled_y = point.y * transform.scale;
    return {(flip_horizontal ? -1.0 : 1.0) * (cosine * scaled_x - sine * scaled_y) + transform.x,
            (flip_vertical ? -1.0 : 1.0) * (sine * scaled_x + cosine * scaled_y) + transform.y,
            point.z * transform.scale + transform.z};
}

Vec3 transform_direction(const Vec3& direction, const ArchitecturalTransform& transform,
                         bool flip_horizontal = false, bool flip_vertical = false) {
    const auto [cosine, sine] = planar_rotation(transform.rotation_z_radians);
    return {(flip_horizontal ? -1.0 : 1.0) * (cosine * direction.x - sine * direction.y),
            (flip_vertical ? -1.0 : 1.0) * (sine * direction.x + cosine * direction.y),
            direction.z};
}

BuildingObject transform_building_object(BuildingObject object,
                                         const ArchitecturalTransform& transform,
                                         bool flip_horizontal = false, bool flip_vertical = false) {
    return std::visit(
        [&transform, flip_horizontal, flip_vertical](auto value) -> BuildingObject {
            using Object = std::decay_t<decltype(value)>;
            const bool reflected = flip_horizontal != flip_vertical;
            const auto point = [&](Vec3 p) { return transform_point(p, transform, flip_horizontal, flip_vertical); };
            const auto heading = [&](double yaw) {
                const auto u = transform_direction({std::cos(yaw), std::sin(yaw), 0},
                    transform, flip_horizontal, flip_vertical);
                return std::atan2(u.y, u.x);
            };
            if constexpr (std::is_same_v<Object, RectangularColumn>) {
                value.base_center = point(value.base_center);
                value.width *= transform.scale;
                value.depth *= transform.scale;
                value.height *= transform.scale;
                value.rotation_radians = reflected ? heading(value.rotation_radians)
                    : value.rotation_radians + transform.rotation_z_radians;
            } else if constexpr (std::is_same_v<Object, CircularColumn>) {
                value.base_center = point(value.base_center);
                value.radius *= transform.scale;
                value.height *= transform.scale;
                if (reflected) value.rotation_radians = heading(value.rotation_radians);
                else if (transform.rotation_z_radians != 0.0)
                    value.rotation_radians = std::remainder(
                        value.rotation_radians + transform.rotation_z_radians,
                        2.0 * std::numbers::pi);
            } else if constexpr (std::is_same_v<Object, Beam>) {
                value.start = point(value.start);
                value.end = point(value.end);
                value.up = transform_direction(value.up, transform, flip_horizontal, flip_vertical);
                value.width *= transform.scale;
                value.depth *= transform.scale;
            } else if constexpr (std::is_same_v<Object, StairFlight>) {
                if (reflected) {
                    // Flights extend to +local Y. Reflection changes that side;
                    // the new origin is the image of the first flight's far side.
                    const auto width = value.flights.empty() ? value.width
                        : value.flights.front().width.value_or(value.width);
                    for (std::size_t i = 0; i < value.landings.size(); ++i) {
                        auto& landing = value.landings[i];
                        switch (landing.turn) {
                        case StairTurn::left_quarter: landing.turn = StairTurn::right_quarter; break;
                        case StairTurn::right_quarter: landing.turn = StairTurn::left_quarter; break;
                        case StairTurn::left_half: landing.turn = StairTurn::right_half; break;
                        case StairTurn::right_half: landing.turn = StairTurn::left_half; break;
                        case StairTurn::straight: landing.align_right = !landing.align_right; break;
                        }
                    }
                    const auto v = transform_direction({-std::sin(value.orientation_radians),
                        std::cos(value.orientation_radians), 0}, transform, flip_horizontal, flip_vertical);
                    value.base_position = point(value.base_position);
                    value.base_position.x += transform.scale * width * v.x;
                    value.base_position.y += transform.scale * width * v.y;
                    value.orientation_radians = heading(value.orientation_radians);
                } else {
                    value.base_position = point(value.base_position);
                    value.orientation_radians += transform.rotation_z_radians;
                }
                value.total_rise *= transform.scale;
                value.going *= transform.scale;
                value.width *= transform.scale;
                for (auto& flight : value.flights) {
                    if (flight.going) *flight.going *= transform.scale;
                    if (flight.width) *flight.width *= transform.scale;
                }
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
                value.base_position = point(value.base_position);
                value.orientation_radians = reflected ? heading(value.orientation_radians)
                    : value.orientation_radians + transform.rotation_z_radians;
                value.length *= transform.scale;
                value.height *= transform.scale;
                value.thickness *= transform.scale;
                value.post_spacing *= transform.scale;
            } else if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
                value.base_position = point(value.base_position);
                value.orientation_radians = reflected ? heading(value.orientation_radians)
                    : value.orientation_radians + transform.rotation_z_radians;
                if (reflected) {
                    // Corner origin; u'=F*u, v'=J*u'=-F*v.
                    value.base_position.x += transform.scale * value.span * std::sin(value.orientation_radians);
                    value.base_position.y -= transform.scale * value.span * std::cos(value.orientation_radians);
                    for (auto& opening : value.openings)
                        opening.y = value.span - opening.y - opening.depth;
                }
                value.run *= transform.scale;
                value.span *= transform.scale;
                value.rise *= transform.scale;
                value.overhang *= transform.scale;
                value.thickness *= transform.scale;
                for (auto& opening : value.openings) {
                    if (reflected && opening.rotation_radians != 0.0)
                        opening.rotation_radians = -opening.rotation_radians;
                    opening.x *= transform.scale;
                    opening.y *= transform.scale;
                    opening.width *= transform.scale;
                    opening.depth *= transform.scale;
                    if (opening.skylight) {
                        opening.skylight->frame_width *= transform.scale;
                        opening.skylight->curb_height *= transform.scale;
                        opening.skylight->glazing_thickness *= transform.scale;
                    }
                }
            } else if constexpr (std::is_same_v<Object, GableRoof> ||
                                 std::is_same_v<Object, HipRoof>) {
                value.base_position = point(value.base_position);
                value.orientation_radians = reflected ? heading(value.orientation_radians)
                    : value.orientation_radians + transform.rotation_z_radians;
                // Gable/hip builders use a centered footprint, unlike panels.
                if (reflected) for (auto& opening : value.openings)
                    opening.y = -opening.y - opening.depth;
                value.length *= transform.scale;
                value.span *= transform.scale;
                value.rise *= transform.scale;
                value.overhang *= transform.scale;
                value.thickness *= transform.scale;
                for (auto& opening : value.openings) {
                    if (reflected && opening.rotation_radians != 0.0)
                        opening.rotation_radians = -opening.rotation_radians;
                    opening.x *= transform.scale;
                    opening.y *= transform.scale;
                    opening.width *= transform.scale;
                    opening.depth *= transform.scale;
                    if (opening.skylight) {
                        opening.skylight->frame_width *= transform.scale;
                        opening.skylight->curb_height *= transform.scale;
                        opening.skylight->glazing_thickness *= transform.scale;
                    }
                }
            }
            return value;
        },
        std::move(object));
}

Vec2 transform_plan_point(const Vec2& point, const ArchitecturalTransform& transform,
                          bool flip_horizontal = false, bool flip_vertical = false) {
    const auto [cosine, sine] = planar_rotation(transform.rotation_z_radians);
    const auto scaled_x = point.x * transform.scale;
    const auto scaled_y = point.y * transform.scale;
    const Vec2 result{(flip_horizontal ? -1.0 : 1.0) * (cosine * scaled_x - sine * scaled_y) + transform.x,
                      (flip_vertical ? -1.0 : 1.0) * (sine * scaled_x + cosine * scaled_y) + transform.y};
    if (!std::isfinite(result.x) || !std::isfinite(result.y)) {
        throw std::invalid_argument("Architectural plan transform exceeds the supported range");
    }
    return result;
}

Segment transform_plan_segment(Segment segment, const ArchitecturalTransform& transform,
                               bool flip_horizontal = false, bool flip_vertical = false) {
    segment.start = transform_plan_point(segment.start, transform, flip_horizontal, flip_vertical);
    segment.end = transform_plan_point(segment.end, transform, flip_horizontal, flip_vertical);
    // Keep analytical endpoints and source order; orientation changes only
    // the signed sweep. Native slab wires normalize their winding internally.
    if (flip_horizontal != flip_vertical && segment.sweep_radians != 0.0)
        segment.sweep_radians = -segment.sweep_radians;
    return segment;
}

Boundary transform_plan_boundary(const Boundary& boundary,
                                 const ArchitecturalTransform& transform,
                                 bool flip_horizontal = false, bool flip_vertical = false) {
    Boundary result;
    result.reserve(boundary.size());
    for (const auto& segment : boundary) {
        result.push_back(transform_plan_segment(segment, transform, flip_horizontal, flip_vertical));
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
                             const ArchitecturalTransform& transform,
                             double managed_cut_source_scale) {
    // Managed cuts remain source-owned until both final hosts are staged.
    // Reconstruct their temporary roster at the host's cumulative prior scale
    // so repeated transforms do not reload original dimensions into a scaled
    // wall. Ordinary cuts already track each operation in entities.
    std::vector<Entity> managed_openings;
    const auto opening_ids = hosted_opening_ids(entities, source.id);
    managed_openings.reserve(opening_ids.size());
    std::vector<const Entity*> openings;
    for (const auto& id : opening_ids) {
        const auto found = entities.find(id);
        if (found == entities.end()) continue;
        if (found->second.properties.contains("corner_window_id")) {
            managed_openings.push_back(found->second);
            auto& cut = managed_openings.back();
            scale_property(cut.properties, "offset_m", "offset", managed_cut_source_scale);
            scale_property(cut.properties, "width_m", "width", managed_cut_source_scale);
            scale_property(cut.properties, "sill_m", "sill", managed_cut_source_scale);
            scale_property(cut.properties, "height_m", "height", managed_cut_source_scale);
            openings.push_back(&cut);
        } else openings.push_back(&found->second);
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
        // The coordinated owner derives managed cut stations from both final
        // hosts and replays their retained quantities once, after this loop.
        if (opening.properties.contains("corner_window_id")) continue;
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
            assembly.window_bow_projection_m *= transform.scale;
            opening.properties["opening_assembly"] = opening_assembly_json(assembly);
        }
    }
    return result;
}

Entity transform_slab_entity(const Entity& source, const ArchitecturalTransform& transform,
                             bool flip_horizontal = false, bool flip_vertical = false) {
    Slab slab;
    std::string error;
    if (!read_document_slab(source, slab, error)) throw std::invalid_argument(error);
    slab.boundary = transform_plan_boundary(slab.boundary, transform, flip_horizontal, flip_vertical);
    for (auto& hole : slab.holes) hole = transform_plan_boundary(hole, transform, flip_horizontal, flip_vertical);
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

Entity transform_room_entity(const Entity& source, const ArchitecturalTransform& transform,
                             bool flip_horizontal = false, bool flip_vertical = false) {
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
    room.boundary = transform_plan_boundary(room.boundary, transform, flip_horizontal, flip_vertical);
    for (auto& hole : room.holes) hole = transform_plan_boundary(hole, transform, flip_horizontal, flip_vertical);
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
                                                 const ArchitecturalTransform& transform,
                                                 double managed_cut_source_scale) {
    // Incomplete generic architectural descriptors are still allowed to carry
    // a transport-level transform for compatibility with the existing
    // transaction contract. Once a canonical footprint is present, malformed
    // data fails closed instead of silently accepting a stale marker.
    if (source.type == "wall") {
        if (!source.properties.contains("baseline")) return std::nullopt;
        return transform_wall_entity(entities, source, transform, managed_cut_source_scale);
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

Entity transform_building_entity(const EntityState& actual_entities, const Entity& source,
                                 const ArchitecturalTransform& transform,
                                 bool flip_horizontal = false, bool flip_vertical = false) {
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
    if (source.type == "roof" && transform.scale != 1.0) {
        const auto effective = resolve_vertical_placement(actual_entities, source);
        const auto datum = effective.properties.at("base_position_m").at(2).get<double>() -
            source.properties.at("base_position_m").at(2).get<double>();
        // Legacy transaction affine coordinates operate in the persisted local
        // frame. Convert that exact operator into a world-space intent using
        // the actual level datum. Group authoring retains its original pivot
        // through the separate actual-map producer below.
        return stage_roof_uniform_transform_entity(actual_entities, {source.id,
            {{}, {transform.x, transform.y, transform.z - (transform.scale - 1.0) * datum},
                transform.rotation_z_radians, transform.scale, flip_horizontal, flip_vertical}});
    }
#else
    (void)actual_entities;
#endif
    if (source.type == "roof" && transform.scale == 1.0) {
        // Actual movement math retires affected entered coordinates into a
        // retained derivation. Generic receipt invalidation must not discard
        // the original expressions or their opaque metadata.
        return replay_roof_rigid_transform_entity(source, {source.id,
            {{}, {transform.x, transform.y, transform.z}, transform.rotation_z_radians,
                1.0, flip_horizontal, flip_vertical}});
    }
    if (source.type == "stair" && source.properties.contains("level_connection") &&
        !source.properties.at("level_connection").is_null() &&
        transform.scale != 1.0) {
        // A connected stair's total rise is tied to the graph's floor-to-floor
        // height.  Scaling only the stair would silently break that relation;
        // edit the level graph (or disconnect the stair) before changing size.
        throw std::invalid_argument(
            "Cannot uniformly scale a stair with a level connection; edit the connected levels first");
    }
    const auto transformed = transform_building_object(decode_building_entity(source), transform,
        flip_horizontal, flip_vertical);
    const auto canonical = encode_building_entity(transformed, source.extensions);
    Entity result = source;
    for (const auto& [key,value] : canonical.properties.items())
        if (key != "version") result.properties[key] = value;
    // Right alignment is new authoring authority. Promote only when needed,
    // and retain version 4 after reflecting back to default left alignment.
    if (source.type == "stair" && canonical.properties.at("version") == 4)
        result.properties["version"] = 4;
    // Placement changes do not migrate a schema. In particular, an authored
    // version-3 stair without explicit per-flight overrides and a version-2
    // roof with an empty opening array must retain their original version.
    // Canonical codecs describe geometry, while child records may also carry
    // opaque source and quantity metadata. Keep those records and update only
    // the fields owned by the codec, without recursively rewriting strings.
    for (const auto* key : {"flights", "landings", "roof_openings"}) {
        if (!canonical.properties.contains(key)) continue;
        auto records = source.properties.at(key);
        const auto& encoded = canonical.properties.at(key);
        for (std::size_t i=0; i<encoded.size(); ++i) {
            if (source.type == "stair" && std::string_view(key) == "landings")
                records.at(i).erase("straight_alignment");
            for (const auto& [field,value] : encoded.at(i).items()) records.at(i)[field]=value;
        }
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
    // Retain application-owned raw properties and child metadata. A legacy
    // marker would describe stale geometry and must not remain authoritative.
    result.properties.erase("transform");
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

void reflect_stair_railings(EntityState& entities, const EntityState& original,
                            const Entity& before, const Entity& after,
                            const ArchitecturalTransform& transform,
                            bool flip_horizontal, bool flip_vertical) {
    const auto old_stair = decode_stair_properties(before.id, before.properties);
    const auto new_stair = decode_stair_properties(after.id, after.properties);
    const auto new_layout = derive_stair_layout(new_stair);
    const auto same_point = [](Vec3 a, Vec3 b) {
        const auto tolerance = default_geometry_tolerance_metres +
            64.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(a.x), std::abs(a.y), std::abs(a.z),
                std::abs(b.x), std::abs(b.y), std::abs(b.z)});
        return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
            std::abs(a.z - b.z) <= tolerance;
    };
    for (const auto& rail_id : hosted_railing_ids(original, before.id)) {
        auto& entity = entities.at(rail_id);
        const auto railing = decode_railing_properties(rail_id, original.at(rail_id).properties);
        auto& host = entity.properties.at("host");
        if (railing.host) {
            host["side"] = railing.host->side == StairRailingSide::left ? "right" : "left";
        } else {
            const auto& old_host = *railing.landing_host;
            const auto old_edge = derive_stair_landing_edge(old_stair, old_host);
            const auto a = transform_point(old_edge.edge_start, transform, flip_horizontal, flip_vertical);
            const auto b = transform_point(old_edge.edge_end, transform, flip_horizontal, flip_vertical);
            std::size_t landing_index = old_stair.landings.size();
            if (old_host.role == StairLandingRole::connecting) {
                const auto landing = std::find_if(old_stair.landings.begin(), old_stair.landings.end(),
                    [&](const auto& value) { return value.id == old_host.landing_id; });
                if (landing == old_stair.landings.end())
                    throw std::invalid_argument("Reflected landing railing has no canonical source landing");
                landing_index = static_cast<std::size_t>(landing - old_stair.landings.begin());
            }
            const auto& polygon = new_layout.landings.at(landing_index).footprint;
            std::optional<std::size_t> edge_index;
            bool reversed = false;
            for (std::size_t edge = 0; edge < polygon.size(); ++edge) {
                const bool forward = same_point(a, polygon[edge]) && same_point(b, polygon[(edge + 1) % 4]);
                const bool backward = same_point(b, polygon[edge]) && same_point(a, polygon[(edge + 1) % 4]);
                if (!forward && !backward) continue;
                if (edge_index) throw std::invalid_argument("Reflected landing railing edge is ambiguous");
                edge_index = edge;
                reversed = backward;
            }
            if (!edge_index)
                throw std::invalid_argument("Reflected stair topology cannot represent the original landing railing edge");
            host["edge_index"] = *edge_index;
            if (reversed) {
                host["start_fraction"] = 1.0 - old_host.end_fraction;
                host["end_fraction"] = 1.0 - old_host.start_fraction;
            }
        }
        for (const auto* field : {"height_m", "thickness_m", "post_spacing_m"})
            scale_property(entity.properties, field, nullptr, transform.scale);
    }
}

void invalidate_changed_receipts(const Entity& before, Entity& after) {
    const auto entries=before.properties.find("quantity_entries");
    if (entries==before.properties.end() || !entries->is_object()) return;
    const auto current_entries=after.properties.find("quantity_entries");
    if (current_entries==after.properties.end() || !current_entries->is_object()) return;
    for (const auto& [pointer,value] : entries->items()) {
        (void)value;
        try {
            if (canonical_multi_flight_stair(before) && canonical_multi_flight_stair(after)) {
                bool flight_dimension=false;
                const auto& old_flights=before.properties.at("flights");
                const auto& flights=after.properties.at("flights");
                for (std::size_t old_row=0; old_row<old_flights.size() && !flight_dimension; ++old_row) {
                    for (const auto* field : {"going_m","width_m"}) {
                        if (pointer!="/flights/"+std::to_string(old_row)+"/"+field) continue;
                        flight_dimension=true;
                        const auto& old=old_flights.at(old_row);
                        for (std::size_t row=0; row<flights.size(); ++row) {
                            const auto& current=flights.at(row);
                            if (current.at("id")!=old.at("id")) continue;
                            if (geometry_fields_changed(old,current,{field}))
                                current_entries->erase("/flights/"+std::to_string(row)+"/"+field);
                            break;
                        }
                        // A removed flight's position may now describe another
                        // child. Its editor already dropped/remapped receipts;
                        // never erase the surviving child's entry by old index.
                        break;
                    }
                }
                if (flight_dimension) continue;
            }
            const nlohmann::json::json_pointer path(pointer);
            if (before.properties.contains(path) &&
                (!after.properties.contains(path) || before.properties.at(path)!=after.properties.at(path)))
                current_entries->erase(pointer);
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
    std::map<std::string,double,std::less<>> wall_scales;
    std::map<std::string,std::string,std::less<>> selected_rail_clones;
    std::map<std::string,std::size_t,std::less<>> stair_clone_counts;
    for (const auto& operation : transaction.operations()) {
        const auto original=entities.find(operation.object_id);
        if (operation.action==ArchitecturalAction::duplicate && original!=entities.end() &&
            canonical_multi_flight_stair(original->second))
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
            if (found->second.type == "corner_window" ||
                (found->second.type == "opening" && found->second.properties.contains("corner_window_id")))
                throw std::invalid_argument("Corner-window placement follows both actual wall hosts");
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
            double managed_cut_source_scale = 1.0;
            if (found->second.type == "wall") {
                const auto scale = wall_scales.try_emplace(found->first,1.0).first;
                managed_cut_source_scale = scale->second;
                scale->second *= operation.transform->scale;
                if (!std::isfinite(scale->second) || scale->second <= 0.0)
                    throw std::invalid_argument("Architectural wall scale exceeds the supported range");
            }
            if (found->second.type == "assembly_instance") {
                auto value = decode_document_assembly_instance(found->second);
                const auto& movement = *operation.transform;
                const AssemblyTransform outer{{movement.x, movement.y, movement.z},
                    movement.rotation_z_radians, movement.scale};
                value.instance.root_transform = compose_assembly_transform(outer, *value.instance.root_transform);
                // The shared writer preserves untouched placement components
                // and override records while applying the intended transform.
                found->second = encode_document_assembly_instance(found->second, value);
            } else if (can_recognize_building_entity_type(found->second.type)) {
                found->second = transform_building_entity(entities, found->second, *operation.transform);
            } else if (const auto transformed =
                           try_transform_shared_solid(entities, found->second, *operation.transform,
                               managed_cut_source_scale)) {
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
            if (canonical_multi_flight_stair(copy)) {
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
    std::erase_if(wall_scales,[](const auto& item) { return item.second == 1.0; });
    complete_corner_window_geometry(source.entities(),entities,wall_scales);
    return entities;
}

ApplyEntityChanges make_candidate_command(const DocumentSnapshot& source, const EntityState& candidate,
                                          Revision expected_revision, const std::string& undo_label,
                                          std::span<const std::string> native_targets = {}) {
    ApplyEntityChanges command;
    command.expected_revision = expected_revision;
    command.message = undo_label;
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
                validate_roof_join_skylights(join, preview.entities());
                (void)make_roof_join(join, members);
            }
        }
        for (const auto& [id,entity] : preview.entities()) {
            (void)id;
            if (!canonical_stair(entity) && !canonical_hosted_railing(entity)) continue;
            const auto effective=resolve_vertical_placement(preview,entity);
            (void)make_building_shape(decode_building_entity(effective),preview.entities());
        }
        bool requires_assembly = false;
        for (const auto& id : native_targets) {
            const auto& entity = preview.entities().at(id);
            if (entity.type == "assembly_instance") requires_assembly = true;
            else if (canonical_independent_building(entity))
                (void)make_building_shape(decode_building_entity(resolve_vertical_placement(preview, entity)),
                    preview.entities());
        }
        if (requires_assembly) {
            AssemblyExpansionBudget budget;
            const auto expanded = expand_document_assembly_instances(preview.entities(), budget);
            for (const auto& id : native_targets)
                if (preview.entities().at(id).type == "assembly_instance")
                    (void)make_assembly_geometry(expanded.at(id));
        }
    }
    return command;
}

ApplyEntityChanges make_command(const DocumentSnapshot& source, const ArchitecturalTransaction& transaction,
                                Revision expected_revision) {
    return make_candidate_command(source, apply_operations(source, transaction),
        expected_revision, transaction.undo_label());
}

}  // namespace

void validate_architectural_geometry_changes(
    const DocumentSnapshot& source, const DocumentSnapshot& candidate,
    const std::vector<std::string>& required_ids) {
    const auto& entities = candidate.entities();
    const auto source_scope = constraint_phase_scope(source.entities());
    const auto candidate_scope = constraint_phase_scope(entities);
    std::set<std::string, std::less<>> host_ids, required_hosts, slab_ids, room_ids, full_room_ids, beam_ids, railing_ids, corner_ids;
    const auto include = [&](const Entity& entity, bool required, bool candidate_target) {
        if (candidate_target) {
            if (candidate_scope.inactive_owner_ids.contains(entity.id))
                throw std::invalid_argument("The edited physical object is inactive in the saved design: " + entity.id);
        } else if (source_scope.inactive_owner_ids.contains(entity.id) ||
                   candidate_scope.inactive_owner_ids.contains(entity.id)) {
            // Retained or deleted parked originals are not requirements of the
            // completed active design. Registry-only parking is not an edit.
            return;
        }
        const auto& p = entity.properties;
        if (entity.type == "wall" && (required || p.contains("baseline"))) {
            host_ids.insert(entity.id);
            if (required) required_hosts.insert(entity.id);
        } else if (entity.type == "opening" && (required || p.contains("wall_id"))) {
            std::string host_id, error;
            if (!read_document_wall_id(entity, host_id, error))
                throw std::invalid_argument("Opening " + entity.id + ": " + error);
            if (candidate_scope.inactive_owner_ids.contains(host_id)) {
                if (candidate_target)
                    throw std::invalid_argument("The edited opening host is inactive in the saved design: " + host_id);
                return;
            }
            host_ids.insert(host_id);
            if (required) required_hosts.insert(std::move(host_id));
        } else if (entity.type == "corner_window") {
            const auto corner = parse_corner_window(entity);
            host_ids.insert(corner.wall_ids.begin(), corner.wall_ids.end());
            required_hosts.insert(corner.wall_ids.begin(), corner.wall_ids.end());
            if (candidate_target) corner_ids.insert(entity.id);
        } else if (entity.type == "slab" && (required || p.contains("boundary"))) {
            slab_ids.insert(entity.id);
        } else if (entity.type == "room" &&
                   (required || p.contains("boundary") || p.contains("segments"))) {
            room_ids.insert(entity.id);
            if (required || has_document_room_volume_fields(entity)) full_room_ids.insert(entity.id);
        } else if (entity.type == "beam" &&
                   (required || canonical_form(entity, "beam", 1, "straight_beam"))) {
            beam_ids.insert(entity.id);
        } else if (entity.type == "railing" &&
                   (required || canonical_form(entity, "railing", 1, "straight_railing"))) {
            railing_ids.insert(entity.id);
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
        else if (entity.type == "corner_window")
            physical_change = !before || before->type != entity.type || before->properties != entity.properties;
        else if (entity.type == "slab")
            physical_change = changed(before, entity, {"boundary", "holes", "thickness_m", "thickness",
                "elevation_m", "elevation", "layers", "element_kind", "vertical_placement", "layer_id",
                "floor_id", "building_id", "property_id"});
        else if (entity.type == "room")
            physical_change = changed(before, entity, {"boundary", "segments", "holes", "height_m",
                "height", "elevation_m", "elevation", "vertical_placement", "layer_id",
                "floor_id", "building_id", "property_id"});
        else if (entity.type == "beam" ||
                 (before && canonical_form(*before, "beam", 1, "straight_beam")) ||
                 canonical_form(entity, "railing", 1, "straight_railing") ||
                 (before && canonical_form(*before, "railing", 1, "straight_railing")))
            physical_change = building_property_geometry_changed(before, entity);
        if (!physical_change) continue;
        if (entity.type == "opening" && before && before->properties.contains("wall_id") &&
            !entity.properties.contains("wall_id"))
            throw std::invalid_argument("The edited opening lost its wall host: " + id);
        include(entity, false, true);
        if (before) include(*before, false, false);
    }
    for (const auto& [id, entity] : source.entities())
        if (!entities.contains(id)) include(entity, false, false);
    for (const auto& id : required_ids) {
        const auto found = entities.find(id);
        if (found == entities.end())
            throw std::invalid_argument("The edited physical object is missing: " + id);
        const auto& type = found->second.type;
        if (type != "wall" && type != "opening" && type != "slab" && type != "room" &&
            type != "beam" && type != "railing" && type != "corner_window")
            throw std::invalid_argument("The edited object has no supported physical descriptor: " + id);
        include(found->second, true, true);
    }

    // A host edit must regenerate the shared manufactured corner, even when
    // its owner and the other wall are hidden or were not selected.
    for (const auto& [id, entity] : entities) {
        if (entity.type != "corner_window" || candidate_scope.inactive_owner_ids.contains(id)) continue;
        const auto corner = parse_corner_window(entity);
        if (corner_ids.contains(id) || std::any_of(corner.wall_ids.begin(), corner.wall_ids.end(),
            [&](const auto& host) { return host_ids.contains(host); })) {
            corner_ids.insert(id);
            host_ids.insert(corner.wall_ids.begin(), corner.wall_ids.end());
            required_hosts.insert(corner.wall_ids.begin(), corner.wall_ids.end());
        }
    }
    std::vector<WallJoin> affected_joins;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "wall_join") continue;
        const auto before = source.entities().find(id);
        const bool edited = before == source.entities().end() || before->second.properties != entity.properties;
        if (candidate_scope.inactive_owner_ids.contains(id)) {
            if (edited)
                throw std::invalid_argument("The edited wall join is inactive in the saved design: " + id);
            continue;
        }
        const auto join = parse_wall_join(entity.properties, id);
        // Match shared physical admission: a retained relationship containing
        // any parked/demolished wall is inactive as a whole.
        if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(),
            [&](const auto& member) { return candidate_scope.inactive_owner_ids.contains(member); })) {
            if (edited)
                throw std::invalid_argument("The edited wall join has an inactive member in the saved design: " + id);
            continue;
        }
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
            if (entity.type != "opening" || candidate_scope.inactive_owner_ids.contains(id)) continue;
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
        if (!affected_joins.empty() || !corner_ids.empty()) admitted_join_walls.emplace(host_id, std::move(wall));
    }
    for (const auto& join : affected_joins) {
        std::vector<Wall> members;
        members.reserve(join.wall_ids.size());
        for (const auto& member_id : join.wall_ids) members.push_back(admitted_join_walls.at(member_id));
        (void)make_wall_join(join, members);
    }
    for (const auto& id : corner_ids) {
        const auto corner = parse_corner_window(entities.at(id));
        const std::array<Wall, 2> walls{admitted_join_walls.at(corner.wall_ids[0]),
                                      admitted_join_walls.at(corner.wall_ids[1])};
        (void)make_corner_window(walls, corner_window_cuts(corner, walls), corner.assembly);
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
    for (const auto& id : railing_ids) {
        const auto found = entities.find(id);
        if (found == entities.end()) continue;
        const auto object = decode_building_entity(resolve_vertical_placement(candidate, found->second));
        const auto* railing = std::get_if<Railing>(&object);
        if (!railing)
            throw std::invalid_argument("The edited railing lost its physical descriptor: " + id);
        // Independent decoding builds the native solid. A separate semantic
        // host conversion still requires the complete current stair map.
        if (railing->host || railing->landing_host) (void)make_building_shape(object, entities);
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
        validate_roof_join_skylights(RoofJoin{join_id, member_ids}, source.entities());
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

Entity stage_structural_beam_endpoint_entity(const EntityState& actual_entities,
    const std::string& entity_id, const BeamEndpointEdit& edit) {
    const auto found = actual_entities.find(entity_id);
    if (found == actual_entities.end() || found->second.id != entity_id)
        throw std::invalid_argument("Beam endpoint source is missing or has inconsistent identity");
    const auto& original = found->second;
    auto entity = original;
    if (edit.endpoint != BeamEndpoint::start && edit.endpoint != BeamEndpoint::end)
        throw std::invalid_argument("Beam endpoint role is invalid");
    if (!std::isfinite(edit.proposed_position.x) || !std::isfinite(edit.proposed_position.y))
        throw std::invalid_argument("Beam endpoint target must be finite");
    if (!canonical_form(entity, "beam", 1, "straight_beam"))
        throw std::invalid_argument("Beam endpoint editing requires a canonical straight beam");
    validate_structural_object_source_entity(entity);
    (void)decode_building_entity(resolve_vertical_placement(actual_entities, entity));
    const auto beam = std::get<Beam>(decode_building_entity(entity));
    const auto& endpoint = edit.endpoint == BeamEndpoint::start ? beam.start : beam.end;
    if (endpoint.x == edit.proposed_position.x && endpoint.y == edit.proposed_position.y)
        return original;
    // Preserve the original three-coordinate array and its exact Z field;
    // re-encoding the whole beam would replace opaque source properties.
    auto& coordinates = entity.properties.at(edit.endpoint == BeamEndpoint::start ? "start_m" : "end_m");
    coordinates.at(0) = edit.proposed_position.x;
    coordinates.at(1) = edit.proposed_position.y;
    invalidate_changed_receipts(original, entity);
    validate_structural_object_source_entity(entity);
    (void)decode_building_entity(resolve_vertical_placement(actual_entities, entity));
    return entity;
}

ApplyEntityChanges beam_endpoint_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const BeamEndpointEdit& edit, Revision expected_revision) {
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    const auto original = semantic_entity(source, entity_id, "beam", expected_revision);
    auto entity = stage_structural_beam_endpoint_entity(source.entities(), entity_id, edit);
    if (entity == original)
        throw std::invalid_argument("Beam endpoint target makes no document change");
    ApplyEntityChanges command{expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Move beam endpoint"};
    const auto candidate = Document::preview_command(source, Command{command});
    validate_architectural_geometry_changes(source, candidate, {entity_id});
    return command;
}

ApplyEntityChanges railing_endpoint_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const RailingEndpointEdit& edit, Revision expected_revision) {
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    auto entity = semantic_entity(source, entity_id, "railing", expected_revision);
    if (edit.endpoint != RailingEndpoint::start && edit.endpoint != RailingEndpoint::end)
        throw std::invalid_argument("Railing endpoint role is invalid");
    if (canonical_hosted_railing(entity))
        throw std::invalid_argument("Hosted stair-flight and landing railing endpoints follow their stair; edit the host stair instead");
    if (!canonical_form(entity, "railing", 1, "straight_railing"))
        throw std::invalid_argument("Railing endpoint editing requires a canonical independent straight railing");
    const auto railing = decode_railing_properties(entity.id, entity.properties);
    const Vec2 start{railing.base_position.x, railing.base_position.y};
    const Vec2 end{start.x + railing.length * std::cos(railing.orientation_radians),
                   start.y + railing.length * std::sin(railing.orientation_radians)};
    const auto bounded = [](Vec2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y) &&
            std::abs(point.x) <= 1e9 && std::abs(point.y) <= 1e9;
    };
    if (!bounded(edit.proposed_position) || !bounded(end))
        throw std::invalid_argument("Railing endpoint coordinates must be finite and bounded");
    const double coordinate_scale = std::max({1.0, std::abs(start.x), std::abs(start.y),
        std::abs(end.x), std::abs(end.y), std::abs(edit.proposed_position.x),
        std::abs(edit.proposed_position.y)});
    const double tolerance = default_geometry_tolerance_metres +
        32 * std::numeric_limits<double>::epsilon() * coordinate_scale;
    const auto agrees = [tolerance](Vec2 a, Vec2 b) {
        return std::hypot(a.x-b.x, a.y-b.y) <= tolerance;
    };
    if (agrees(edit.endpoint == RailingEndpoint::start ? start : end, edit.proposed_position))
        throw std::invalid_argument("Railing endpoint target makes no document change within the analytical tolerance");
    const Vec2 proposed_start = edit.endpoint == RailingEndpoint::start ? edit.proposed_position : start;
    const Vec2 proposed_end = edit.endpoint == RailingEndpoint::end ? edit.proposed_position : end;
    auto replacement = railing;
    replacement.base_position.x = proposed_start.x;
    replacement.base_position.y = proposed_start.y;
    const double dx = proposed_end.x-proposed_start.x, dy = proposed_end.y-proposed_start.y;
    replacement.length = std::hypot(dx,dy);
    replacement.orientation_radians = std::atan2(dy,dx);
    // Coordinate subtraction and polar reconstruction may introduce roundoff.
    // Retain an unchanged authored length/heading and its receipt, including
    // pure rotation or same-direction stretching at large plan coordinates.
    // This uses only the roundoff allowance, without the analytical tolerance.
    const double roundoff = 32 * std::numeric_limits<double>::epsilon() *
        std::max(coordinate_scale, railing.length);
    if (std::isfinite(replacement.length) && std::abs(replacement.length-railing.length) <= roundoff)
        replacement.length = railing.length;
    const Vec2 end_at_original_heading{
        proposed_start.x + replacement.length * std::cos(railing.orientation_radians),
        proposed_start.y + replacement.length * std::sin(railing.orientation_radians)};
    if (std::hypot(end_at_original_heading.x-proposed_end.x,
                   end_at_original_heading.y-proposed_end.y) <= roundoff)
        replacement.orientation_radians = railing.orientation_radians;
    validate_railing(replacement); // Includes nondegenerate length and bounded post work.

    const Entity original = entity;
    if (edit.endpoint == RailingEndpoint::start) {
        auto& base = entity.properties.at("base_position_m");
        if (start.x != proposed_start.x) base.at(0) = proposed_start.x;
        if (start.y != proposed_start.y) base.at(1) = proposed_start.y;
    }
    const auto update_scalar = [&](const char* canonical, const char* alias, double value) {
        // Existing aliases denote the same authored quantity. Refuse a
        // conflicting retained value instead of silently replacing it.
        const auto old_value = original.properties.at(canonical).get<double>();
        if (entity.properties.contains(alias)) {
            const auto& old_alias = original.properties.at(alias);
            if (!old_alias.is_number() || old_alias.get<double>() != old_value)
                throw std::invalid_argument(std::string("Railing endpoint edit aliases disagree: ") + alias);
            if (old_value != value) entity.properties[alias] = value;
        }
        if (old_value != value) entity.properties[canonical] = value;
    };
    update_scalar("length_m", "length", replacement.length);
    update_scalar("orientation_rad", "orientation_radians", replacement.orientation_radians);
    invalidate_changed_receipts(original, entity);
    // Re-read the exact patched payload. The existing v1 polar shape must
    // represent both endpoints, without a new endpoint schema or host migration.
    const auto encoded = decode_railing_properties(entity.id, entity.properties);
    const Vec2 encoded_start{encoded.base_position.x, encoded.base_position.y};
    const Vec2 encoded_end{encoded_start.x + encoded.length * std::cos(encoded.orientation_radians),
                           encoded_start.y + encoded.length * std::sin(encoded.orientation_radians)};
    if (!bounded(encoded_end) || !agrees(encoded_start,proposed_start) || !agrees(encoded_end,proposed_end))
        throw std::invalid_argument("Railing endpoint reconstruction exceeds the analytical tolerance");
    if (entity == original)
        throw std::invalid_argument("Railing endpoint target makes no document change");
    ApplyEntityChanges command{expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Move railing endpoint"};
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
    const auto selected = ModelPhases::from_json(entity.properties.at("model"))
        .with_active(std::move(alternative));
    entity.properties["model"] = retain_model_phase_source(entity.properties.at("model"), selected);
    return {expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Select remodeling alternative"};
}

ApplyEntityChanges model_phase_alternative_update_command(const DocumentSnapshot& source,
    const std::string& entity_id, const std::string& alternative_id, std::string name,
    std::vector<std::string> demolished_ids, Revision expected_revision) {
    auto entity = semantic_entity(source, entity_id, "model_phases", expected_revision);
    const auto phases = ModelPhases::from_json(entity.properties.at("model"));
    const auto& alternatives = phases.alternatives();
    const auto found = std::find_if(alternatives.begin(), alternatives.end(),
        [&](const auto& candidate) { return candidate.id == alternative_id; });
    if (found == alternatives.end())
        throw std::invalid_argument("unknown remodeling alternative: " + alternative_id);
    RemodelingAlternative replacement = *found;
    replacement.name = std::move(name);
    replacement.demolished_ids = std::move(demolished_ids);
    entity.properties["model"] = retain_model_phase_source(entity.properties.at("model"),
        phases.with_updated_alternative(std::move(replacement)));
    return {expected_revision, {EntityChange::upsert(std::move(entity))}, {}, "Update remodeling alternative"};
}

ApplyEntityChanges architectural_transaction_command(const DocumentSnapshot& source,
                                                     const ArchitecturalTransaction& transaction,
                                                     Revision expected_revision) {
    return make_command(source, transaction, expected_revision);
}

ApplyEntityChanges architectural_group_transform_command(const DocumentSnapshot& source,
    std::span<const std::string> entity_ids, const ArchitecturalGroupTransform& transform,
    const std::string& transaction_id, Revision expected_revision) {
    if (source.revision()!=expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision,"The architectural group source changed.");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only,"The architectural group source is read-only.");
    if (entity_ids.empty() || entity_ids.size()>maximum_architectural_group_targets)
        throw std::invalid_argument("An architectural group requires between 1 and 1000 objects.");
    std::vector<ArchitecturalGroupTransformTarget> targets;
    targets.reserve(entity_ids.size());
    for (const auto& id : entity_ids) targets.push_back({id,transform});
    return architectural_group_transform_command(source, targets, transaction_id, expected_revision);
}

ApplyEntityChanges architectural_group_transform_command(const DocumentSnapshot& source,
    std::span<const ArchitecturalGroupTransformTarget> requested_targets,
    const std::string& transaction_id, Revision expected_revision) {
    if (source.revision()!=expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision,"The architectural group source changed.");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only,"The architectural group source is read-only.");
    if (requested_targets.empty() || requested_targets.size()>maximum_architectural_group_targets)
        throw std::invalid_argument("An architectural group requires between 1 and 1000 objects.");
    using Intent = NormalizedGroupTransform;
    std::set<std::string,std::less<>> selected;
    std::map<std::string,Intent,std::less<>> intents;
    std::vector<std::string> targets;
    std::vector<StairTransformIntent> stair_intents;
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
    std::vector<SlabGeometryEditIntent> slab_intents;
    slab_intents.reserve(requested_targets.size());
    std::vector<RoofEditIntent> roof_intents;
    roof_intents.reserve(requested_targets.size());
#endif
    targets.reserve(requested_targets.size());
    bool identity=true;
    for (const auto& target : requested_targets) {
        const auto& id=target.entity_id;
        const auto intent=normalize_group_transform(target.transform);
        const auto found=source.entities().find(id);
        if (!selected.insert(id).second)
            throw std::invalid_argument("An architectural group contains a duplicate target.");
        if (found==source.entities().end() || !can_transform_architectural_entity_type(found->second.type))
            throw std::invalid_argument("Every architectural group target must be a supported persisted object.");
        if (id != found->second.id)
            throw std::invalid_argument("An architectural group target has an inconsistent persisted identity.");
        if (found->second.type=="wall")
            throw std::invalid_argument("Planar wall groups require connected-wall transform authority.");
        targets.push_back(id);
        intents.emplace(id,intent);
        identity=identity && intent.identity;
        if (found->second.type=="stair" || found->second.type=="railing")
            stair_intents.push_back({id,target.transform});
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
        if (found->second.type == "roof") {
            RoofEditIntent edit;
            edit.roof_id = id;
            edit.coordinate_world_hosted_geometry = true;
            if (target.transform.scale == 1.0)
                edit.transform = RoofRigidTransformIntent{id, target.transform};
            else edit.uniform_transform = RoofUniformTransformIntent{id, target.transform};
            roof_intents.push_back(std::move(edit));
        }
        if (found->second.type=="slab" && !intent.identity) {
            // Replay the captured mathematical operation, never the affine
            // translation or level compensation used by the other families.
            const auto& transform=target.transform;
            SlabGeometryEditIntent slab_intent;
            slab_intent.slab_id=id;
            slab_intent.coordinate_world_hosted_geometry=true;
            if (transform.scale==1.0 && transform.offset.z==0.0) {
                slab_intent.kind=SlabGeometryEditKind::transform_plan;
                slab_intent.transform=PlanarTransform{{transform.pivot.x,transform.pivot.y},
                    transform.rotation_z_radians,transform.flip_horizontal,transform.flip_vertical,
                    {transform.offset.x,transform.offset.y}};
            } else {
                slab_intent.kind=SlabGeometryEditKind::transform_model;
                slab_intent.model_transform=SlabModelTransform{transform.pivot,transform.offset,
                    transform.rotation_z_radians,transform.scale,
                    transform.flip_horizontal,transform.flip_vertical};
            }
            slab_intents.push_back(std::move(slab_intent));
        }
#endif
    }
    // Validate real descriptors even for identity intent. This group boundary
    // never admits the transaction lane's legacy transport-marker fallback.
    std::map<std::string,double,std::less<>> level_shifts;
    std::set<std::string,std::less<>> hosted_targets;
    for (const auto& id : targets) {
        const auto& entity=source.entities().at(id);
        if (!entity.properties.is_object())
            throw std::invalid_argument("An architectural group requires physical object properties.");
        if (canonical_hosted_railing(entity)) {
            // Complete replay bounds the actual cohort before codec/layout
            // work and requires the selected actual host/equivalent operation.
            hosted_targets.insert(id);
            continue;
        }
        // The complete actual-source stair producer owns family/level/native
        // admission and raw datum compensation, including exact identity.
        if (entity.type=="stair" || entity.type=="railing") continue;
        const auto effective=resolve_vertical_placement(source,entity);
        if (entity.type=="slab") {
            Slab slab;
            std::string error;
            if (!read_document_slab(effective,slab,error))
                throw std::invalid_argument("Architectural group slab " + id + ": " + error);
            (void)make_slab(slab);
        } else if (entity.type=="room") {
            (void)transform_room_entity(effective,ArchitecturalTransform{});
        } else if (entity.type=="assembly_instance") {
            (void)decode_document_assembly_instance(entity);
        } else {
            // Dedicated codecs reject unsupported versions/forms explicitly.
            (void)decode_building_entity(effective);
        }
        const auto placement=entity.properties.find("vertical_placement");
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
        // The dedicated slab producer resolves raw/legacy elevation aliases
        // and the actual level shift itself, including exact source no-ops.
        if (entity.type=="slab") continue;
#endif
        if (placement!=entity.properties.end() && placement->at("mode")=="level") {
            // A level's world shift is outside its raw object coordinates.
            // Keep the binding and compensate it in the local affine Z term:
            // s*(raw+shift)+tz-shift = s*raw+tz+(s-1)*shift.
            double shift{};
            if (entity.type=="slab" || entity.type=="room")
                shift=effective.properties.at("elevation_m").get<double>()-
                    entity.properties.at("elevation_m").get<double>();
            else {
                const char* point=entity.type=="column"?"base_center_m":
                    (entity.type=="beam"?"start_m":"base_position_m");
                shift=effective.properties.at(point).at(2).get<double>()-
                    entity.properties.at(point).at(2).get<double>();
            }
            level_shifts.emplace(id,shift);
        }
    }
    std::vector<ArchitecturalOperation> operations;
    operations.reserve(targets.size());
    for (const auto& id : targets) {
        // Hosted selected members participate in validation/selection, while
        // the host operation alone updates all visible or hidden dependents.
        if (hosted_targets.contains(id)) continue;
        ArchitecturalOperation operation{ArchitecturalAction::transform,id};
        operation.transform=intents.at(id).affine;
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
        // Actual-map slab replay resolves its own model-space level shift.
        if (source.entities().at(id).type!="slab")
#endif
        if (const auto shift=level_shifts.find(id); shift!=level_shifts.end()) {
            operation.transform->z+=(operation.transform->scale-1.0)*shift->second;
            if (!std::isfinite(operation.transform->z))
                throw std::invalid_argument("The architectural group level placement exceeds the supported transform range.");
        }
        operations.push_back(std::move(operation));
    }
    auto candidate = copy_entities(source);
    auto transaction_targets=targets;
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
    if (!slab_intents.empty()) {
        candidate=replay_slab_geometry_with_hosted_entities(source.entities(),slab_intents);
        // Hosted catalogs are actual-source consequences, not independently
        // selected transform roots. Admit their identities without granting
        // the helper authority over any other unselected entity family.
        if (candidate.size()!=source.entities().size())
            throw std::invalid_argument("Architectural slab replay changed the actual entity inventory.");
        for (const auto& [id,after] : candidate) {
            const auto found=source.entities().find(id);
            if (found==source.entities().end() || after.id!=id || after.type!=found->second.type)
                throw std::invalid_argument("Architectural slab replay changed an actual entity identity.");
            if (after==found->second) continue;
            if (selected.contains(id) && after.type=="slab") continue;
            if (after.type!="assembly_model")
                throw std::invalid_argument("Architectural slab replay changed an unrelated actual object.");
            transaction_targets.push_back(id);
        }
    }
#endif
    // Each producer reads the same captured map. All selected roof operations
    // compose before final cohort admission, including mixed rigid/scaled
    // members. Roof changes cannot overwrite slab-hosted catalog consequences.
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
    if (!roof_intents.empty()) {
        const auto roofs = replay_roof_edit_entities(source.entities(), roof_intents);
        for (const auto& [id, after] : roofs) {
            const auto& before = source.entities().at(id);
            if (after == before && after.properties.dump() == before.properties.dump() &&
                after.extensions.dump() == before.extensions.dump()) continue;
            if (after.id==id && before.type=="assembly_model" && after.type=="assembly_model") {
                const auto& accumulated=candidate.at(id);
                if (accumulated==before && accumulated.properties.dump()==before.properties.dump() &&
                    accumulated.extensions.dump()==before.extensions.dump()) candidate.at(id)=after;
                else candidate.at(id)=merge_group_hosted_catalog(before,accumulated,after);
                if (std::find(transaction_targets.begin(),transaction_targets.end(),id)==transaction_targets.end())
                    transaction_targets.push_back(id);
                continue;
            }
            if (!selected.contains(id) || after.type != "roof" || before.type != "roof" || after.id != id)
                throw std::invalid_argument("Architectural roof replay changed an unrelated source owner.");
            candidate.at(id) = after;
        }
    }
#endif
    if (!stair_intents.empty()) {
        const auto stairs=replay_stair_transform_entities(source.entities(),stair_intents);
        if (stairs.size()!=source.entities().size())
            throw std::invalid_argument("Architectural stair replay changed the actual entity inventory");
        for (const auto& [id,after] : stairs) {
            const auto& before=source.entities().at(id);
            if (after==before && after.properties.dump()==before.properties.dump() &&
                after.extensions.dump()==before.extensions.dump()) continue;
            if (after.id!=id || after.type!=before.type)
                throw std::invalid_argument("Architectural stair replay changed an actual owner identity");
            if (after.type=="assembly_model") {
                const auto& accumulated=candidate.at(id);
                if (accumulated==before && accumulated.properties.dump()==before.properties.dump() &&
                    accumulated.extensions.dump()==before.extensions.dump()) candidate.at(id)=after;
                else candidate.at(id)=merge_group_hosted_catalog(before,accumulated,after);
                if (std::find(transaction_targets.begin(),transaction_targets.end(),id)==transaction_targets.end())
                    transaction_targets.push_back(id);
                continue;
            }
            if (after.type!="stair" && after.type!="railing")
                throw std::invalid_argument("Architectural stair replay changed an unrelated actual owner");
            candidate.at(id)=after;
        }
    }
    const auto transaction=ArchitecturalTransaction::create(transaction_id,std::to_string(source.revision()),
        std::move(transaction_targets),std::move(operations),"Transform architectural group");
    // Creating the validated transaction also checks transaction/target lexical
    // identities on the no-op path without re-encoding any source metadata.
    if (identity) return {expected_revision,{}, {},"Transform architectural group"};
    // Construct final descriptors directly from the captured source. An
    // intermediate proper move could fail world/level admission even though
    // its reflected final position is valid; it must never be published.
    for (const auto& operation : transaction.operations()) {
        const auto& id = operation.object_id;
        const auto& intent=intents.at(id);
        if (intent.identity) continue;
        const bool horizontal=intent.flip_horizontal, vertical=intent.flip_vertical;
        const bool reflected=horizontal!=vertical;
        const auto& before = source.entities().at(id);
        const auto& movement = *operation.transform;
        auto& after = candidate.at(id);
        if (before.type=="stair" || before.type=="railing")
            continue; // Complete source-derived physical/catalog replay already admitted this cohort.
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
        if (before.type == "roof")
            continue; // Actual source pivot, datum, openings and receipts are already complete.
#endif
        if (before.type == "slab")
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
            continue; // Source-derived geometry, receipts and hosted motion are already complete.
#else
            after = transform_slab_entity(before, movement, horizontal, vertical);
#endif
        else if (before.type == "room")
            after = transform_room_entity(before, movement, horizontal, vertical);
        else if (before.type == "assembly_instance") {
            auto value = decode_document_assembly_instance(before);
            auto& root = *value.instance.root_transform;
            if (reflected) {
                const auto position = transform_point({root.translation_m.x, root.translation_m.y,
                    root.translation_m.z}, movement, horizontal, vertical);
                const auto u = transform_direction({std::cos(root.rotation_radians),
                    std::sin(root.rotation_radians), 0}, movement, horizontal, vertical);
                root.translation_m = {position.x, position.y, position.z};
                root.rotation_radians = std::atan2(u.y, u.x);
                root.scale *= movement.scale;
                root.mirrored_y = !root.mirrored_y;
            } else {
                const AssemblyTransform outer{{movement.x, movement.y, movement.z},
                    movement.rotation_z_radians,movement.scale};
                root=compose_assembly_transform(outer,root);
            }
            after = encode_document_assembly_instance(after, value);
        } else {
            after = transform_building_entity(source.entities(), before, movement, horizontal, vertical);
        }
    }
    return make_candidate_command(source, candidate, expected_revision, transaction.undo_label(), targets);
}

AssemblyTransform architectural_group_assembly_transform(const ArchitecturalGroupTransform& transform) {
    const auto normalized=normalize_group_transform(transform);
    const auto& affine=normalized.affine;
    const bool reflected=normalized.flip_horizontal!=normalized.flip_vertical;
    AssemblyTransform result{{affine.x,affine.y,affine.z},
        std::remainder((reflected?-affine.rotation_z_radians:affine.rotation_z_radians)+
            (normalized.flip_horizontal?std::numbers::pi:0.0),2.0*std::numbers::pi),
        affine.scale,reflected};
    (void)transform_assembly_point({},result);
    return result;
}

EntityState stair_transform_detail::stage_geometry(const EntityState& actual_entities,
    std::span<const ArchitecturalGroupTransformTarget> targets) {
    if (targets.empty() || targets.size()>maximum_architectural_group_targets)
        throw std::invalid_argument("Stair transforms require between 1 and 1000 actual targets");
    source_bounds(actual_entities);
    std::map<std::string,NormalizedGroupTransform,std::less<>> intents;
    for (const auto& target : targets) {
        const auto found=actual_entities.find(target.entity_id);
        if (found==actual_entities.end() || found->first!=found->second.id ||
            (!canonical_stair(found->second) && !canonical_hosted_railing(found->second) &&
                !canonical_form(found->second,"railing",1,"straight_railing")))
            throw std::invalid_argument("Stair transform requires an actual canonical stair or railing");
        if (!intents.emplace(target.entity_id,normalize_group_transform(target.transform)).second)
            throw std::invalid_argument("Stair transform has duplicate targets");
        (void)decode_building_entity(found->second);
    }
    for (const auto& [id,intent] : intents) {
        const auto& entity=actual_entities.at(id);
        if (!canonical_hosted_railing(entity)) continue;
        const auto rail=decode_railing_properties(id,entity.properties);
        const auto& host=rail.host?rail.host->stair_id:rail.landing_host->stair_id;
        const auto selected=intents.find(host);
        if (selected==intents.end() || !canonical_stair(actual_entities.at(host)) ||
            !equivalent_group_transform(intent,selected->second))
            throw std::invalid_argument("Hosted rail requires its actual selected stair with affine-equivalent intent");
    }
    auto result=actual_entities;
    for (const auto& [id,intent] : intents) {
        const auto& before=actual_entities.at(id);
        if (canonical_hosted_railing(before) || intent.identity) continue;
        auto movement=intent.affine;
        const auto effective=resolve_vertical_placement(actual_entities,before);
        const auto placement=before.properties.find("vertical_placement");
        if (placement!=before.properties.end() && placement->at("mode")=="level") {
            const auto datum=effective.properties.at("base_position_m").at(2).get<double>()-
                before.properties.at("base_position_m").at(2).get<double>();
            movement.z+=(movement.scale-1.0)*datum;
            if (!std::isfinite(movement.z))
                throw std::invalid_argument("Stair transform level datum exceeds supported range");
        }
        auto& after=result.at(id);
        after=transform_building_entity(actual_entities,before,movement,intent.flip_horizontal,intent.flip_vertical);
        // The typed transform lane grants no legacy transport-marker authority.
        if (before.properties.contains("transform")) after.properties["transform"]=before.properties.at("transform");
        if (!canonical_stair(before)) continue;
        if (intent.flip_horizontal!=intent.flip_vertical)
            reflect_stair_railings(result,actual_entities,before,after,movement,intent.flip_horizontal,intent.flip_vertical);
        else if (movement.scale!=1.0)
            for (const auto& rail_id : hosted_railing_ids(actual_entities,id))
                for (const auto* key : {"height_m","thickness_m","post_spacing_m"})
                    scale_property(result.at(rail_id).properties,key,nullptr,movement.scale);
    }
    return result;
}

Entity stage_structural_group_transform_entity(const EntityState& actual_entities,
    const std::string& object_id, const ArchitecturalGroupTransform& transform) {
    const auto intent = normalize_group_transform(transform);
    const auto found = actual_entities.find(object_id);
    if (found == actual_entities.end() || found->first != found->second.id)
        throw std::invalid_argument("Structural transform source is missing or has inconsistent identity");
    const auto& source = found->second;
    validate_structural_object_source_entity(source);
    const auto effective = resolve_vertical_placement(actual_entities, source);
    (void)decode_building_entity(effective);
    if (intent.identity) return source;
    auto movement = intent.affine;
    const auto placement = source.properties.find("vertical_placement");
    if (placement != source.properties.end() && placement->at("mode") == "level") {
        const char* coordinate = source.type == "column" ? "base_center_m" : "start_m";
        const auto datum = effective.properties.at(coordinate).at(2).get<double>() -
            source.properties.at(coordinate).at(2).get<double>();
        // Apply G in world coordinates while retaining the real source binding:
        // s*(raw+datum)+tz-datum = s*raw+tz+(s-1)*datum.
        movement.z += (movement.scale - 1.0) * datum;
        if (!std::isfinite(movement.z))
            throw std::invalid_argument("Structural transform level datum exceeds the supported range");
    }
    auto result = transform_building_entity(actual_entities, source, movement,
        intent.flip_horizontal, intent.flip_vertical);
    invalidate_changed_receipts(source, result);
    validate_structural_object_source_entity(result);
    (void)decode_building_entity(resolve_vertical_placement(actual_entities, result));
    return result;
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

ApplyEntityChanges corner_window_upsert_command(const DocumentSnapshot& source,
    const Entity& replacement, Revision expected_revision) {
    if (!source.is_editable() || source.revision() != expected_revision)
        throw std::invalid_argument("Corner-window source is read-only or stale");
    const auto corner = parse_corner_window(replacement);
    const auto previous = source.entities().find(corner.id);
    if (previous != source.entities().end()) {
        const auto old = parse_corner_window(previous->second);
        if (old.opening_ids != corner.opening_ids)
            throw std::invalid_argument("A corner edit must retain both cut identities");
        auto candidate = copy_entities(source);
        candidate.at(corner.id) = replacement;
        complete_corner_window_geometry(source.entities(),candidate);
        std::vector<EntityChange> changes{EntityChange::upsert(candidate.at(corner.id))};
        for (const auto& id : corner.opening_ids)
            changes.push_back(EntityChange::upsert(candidate.at(id)));
        // The caller completes actual saved-phase memberships before preview.
        return {expected_revision,std::move(changes),{},"Edit corner window"};
    }
    std::array<Wall, 2> walls;
    for (std::size_t leg = 0; leg < 2; ++leg) {
        const auto host = source.entities().find(corner.wall_ids[leg]);
        if (host == source.entities().end() || host->second.type != "wall")
            throw std::invalid_argument("Corner window requires two actual walls");
        std::string error;
        // Raw baselines and station dimensions are persistent authority. Native
        // admission below separately resolves the actual level elevation.
        if (!read_document_wall(host->second, {}, walls[leg], error))
            throw std::invalid_argument("Corner-window wall: " + error);
    }
    const auto cuts = corner_window_cuts(corner, walls);
    std::vector<EntityChange> changes{EntityChange::upsert(replacement)};
    for (std::size_t leg = 0; leg < 2; ++leg) {
        Entity child{cuts[leg].id, "opening", nlohmann::json::object(), false, nlohmann::json::object()};
        if (source.entities().contains(child.id))
            throw std::invalid_argument("Corner-window cut identity is already owned");
        auto& p = child.properties;
        p["wall_id"] = corner.wall_ids[leg];
        p["corner_window_id"] = corner.id;
        if (!p.contains("corner_leg")) p["corner_leg"] = leg;
        p["opening_kind"] = "opening";
        const auto retain_dimension = [&](const char* canonical, const char* legacy, double value) {
            const bool had_canonical = p.contains(canonical), had_legacy = p.contains(legacy);
            if ((!had_canonical && !had_legacy) || (had_canonical && p.at(canonical).get<double>() != value))
                p[canonical] = value;
            if (had_legacy && p.at(legacy).get<double>() != value) p[legacy] = value;
        };
        retain_dimension("offset_m", "offset", cuts[leg].offset);
        retain_dimension("width_m", "width", cuts[leg].width);
        retain_dimension("sill_m", "sill", cuts[leg].sill);
        retain_dimension("height_m", "height", cuts[leg].height);
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "level_id"}) {
            if (replacement.properties.contains(key)) p[key] = replacement.properties.at(key);
            else p.erase(key);
        }
        changes.push_back(EntityChange::upsert(std::move(child)));
    }
    ApplyEntityChanges command{expected_revision, std::move(changes), {},
        "Create corner window"};
    // The caller completes actual saved-phase memberships before previewing
    // this staged command. A premature preview would reject valid new owners
    // against the unchanged hosts' registry, or deletion before registry cleanup.
    return command;
}

ApplyEntityChanges corner_window_remove_command(const DocumentSnapshot& source,
    const std::string& owner_id, Revision expected_revision) {
    if (!source.is_editable() || source.revision() != expected_revision)
        throw std::invalid_argument("Corner-window source is read-only or stale");
#ifdef VERTEX_HAS_HORIZONTAL_AUTHORING
    return prepare_corner_window_removal(source, {owner_id}, "Delete corner window");
#else
    const auto corner = parse_corner_window(source.entities().at(owner_id));
    // The minimal core has no complete phase retirement kernel. Retained
    // alternatives require that producer; never erase their corner originals.
    for (const auto& [id, entity] : source.entities()) {
        (void)id;
        if (entity.type != "model_phases") continue;
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        if (std::binary_search(model.entity_ids().begin(), model.entity_ids().end(), owner_id) &&
            !model.alternatives().empty())
            throw std::invalid_argument("Registered corner-window removal requires complete architectural authoring");
    }
    ApplyEntityChanges command{expected_revision,
        {EntityChange::erase(corner.id), EntityChange::erase(corner.opening_ids[0]),
         EntityChange::erase(corner.opening_ids[1])}, {}, "Delete corner window"};
    std::set<std::string,std::less<>> retired{corner.id,corner.opening_ids[0],corner.opening_ids[1]};
    for (const auto& [id,entity]:source.entities()) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto target=entity.properties.find("target");
        if (target==entity.properties.end() || !target->is_object() ||
            target->value("entity_id",nlohmann::json())!=corner.id) continue;
        const auto decoded=decode_boundary_dimension_entity(entity);
        if (!decoded.supported() || decoded.dimension->kind!=BoundaryDimensionKind::corner_window_leg_length || entity.required)
            throw std::invalid_argument("Corner-window removal has a protected or unsupported attached dimension: "+id);
        (void)decoded.dimension->resolve(source.entities());
        retired.insert(id); command.entity_changes.push_back(EntityChange::erase(id));
    }
    for (const auto& [id,entity]:source.entities()) {
        if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        auto properties=entity.properties;
        if (retired.contains(id)) properties.erase("target");
        const auto touches=[&](const nlohmann::json& root) {
            std::vector<const nlohmann::json*> pending{&root};
            while (!pending.empty()) {
                const auto* value=pending.back(); pending.pop_back();
                if (value->is_string() && retired.contains(value->get_ref<const std::string&>())) return true;
                if (value->is_object()) for (const auto& [key,child]:value->items()) {
                    if (retired.contains(key)) return true;
                    pending.push_back(&child);
                } else if (value->is_array()) for (const auto& child:*value) pending.push_back(&child);
            }
            return false;
        };
        if (touches(properties) || touches(entity.extensions))
            throw std::invalid_argument("Corner-window removal has an opaque retired dimension reference: "+id);
    }
    return command;
#endif
}

}  // namespace sketch
