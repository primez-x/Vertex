#include "sketch/plan_axis_resize.hpp"

#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr double angular_tolerance = 1e-10;

bool equal_factors(double a, double b) {
    return std::abs(a-b) <= 1e-12 * std::max({1.0, std::abs(a), std::abs(b)});
}

struct Resize {
    double x;
    double y;
    Vec2 anchor;
    double angle;

    Vec2 point(Vec2 p) const {
        if (x == 1 && y == 1) return p;
        const double c = std::cos(angle), s = std::sin(angle);
        const double dx = p.x-anchor.x, dy = p.y-anchor.y;
        const double u = (c*dx+s*dy)*x, v = (-s*dx+c*dy)*y;
        const Vec2 result{anchor.x+c*u-s*v, anchor.y+s*u+c*v};
        if (!std::isfinite(result.x) || !std::isfinite(result.y))
            throw std::invalid_argument("Plan resize coordinates overflow");
        return result;
    }
    void point(Vec3& p) const {
        const auto xy = point(Vec2{p.x,p.y});
        p.x = xy.x; p.y = xy.y;
    }
    std::pair<double,double> local_factors(double natural_angle) const {
        if (equal_factors(x,y)) return {x,x};
        const double c = std::cos(natural_angle-angle);
        const double s = std::sin(natural_angle-angle);
        if (std::abs(s) <= angular_tolerance) return {x,y};
        if (std::abs(c) <= angular_tolerance) return {y,x};
        throw std::invalid_argument(
            "Plan resize axes must align with the object's physical dimensions; shearing is unsupported");
    }
    Segment segment(Segment value) const {
        if (value.sweep_radians != 0 && !equal_factors(x,y))
            throw std::invalid_argument(
                "Independent axis resizing would turn a circular arc into an unsupported ellipse");
        value.start = point(value.start); value.end = point(value.end);
        return value;
    }
    Boundary boundary(Boundary value) const {
        for (auto& segment_value : value) segment_value = segment(segment_value);
        return value;
    }
};

void update_segment(Json& value, const Segment& segment) {
    value["start"] = {segment.start.x,segment.start.y};
    value["end"] = {segment.end.x,segment.end.y};
    value["sweep_radians"] = segment.sweep_radians;
}
void update_boundary(Json& value, const Boundary& boundary) {
    if (!value.is_array() || value.size() != boundary.size())
        throw std::invalid_argument("Plan boundary source shape is invalid");
    for (std::size_t i=0; i<boundary.size(); ++i) update_segment(value[i],boundary[i]);
}
void update_footprint(Entity& entity, const Boundary& outer, const std::vector<Boundary>& holes) {
    for (const auto* key : {"boundary","segments"})
        if (entity.properties.contains(key)) update_boundary(entity.properties[key],outer);
    if (entity.properties.contains("holes"))
        for (std::size_t i=0; i<holes.size(); ++i)
            update_boundary(entity.properties["holes"][i],holes[i]);
}
void set_dimension(Json& properties, const char* name, const char* alias, double value) {
    properties[name] = value;
    if (properties.contains(alias)) properties[alias] = value;
}

// A programmatic resize invalidates only expressions for values it changed.
// Unknown receipt paths and opaque legacy records remain application metadata.
void invalidate_changed_quantity_entries(const Entity& before, Entity& after) {
    const auto found = before.properties.find("quantity_entries");
    if (found == before.properties.end()) return;
    if (!found->is_object()) throw std::invalid_argument("Quantity entries must be an object");
    Json retained = *found;
    for (const auto& [pointer, receipt] : found->items()) {
        (void)receipt;
        try {
            const Json::json_pointer path(pointer);
            if (before.properties.contains(path) &&
                (!after.properties.contains(path) || before.properties.at(path) != after.properties.at(path)))
                retained.erase(pointer);
        } catch (const Json::exception&) { /* Unknown legacy receipt pointer. */ }
    }
    after.properties["quantity_entries"] = std::move(retained);
}

std::vector<const Entity*> hosted_openings(const DocumentSnapshot& source, const std::string& wall_id) {
    std::vector<const Entity*> result;
    for (const auto& [id, entity] : source.entities()) {
        (void)id;
        if (entity.type != "opening") continue;
        std::string host, error;
        if (!read_document_wall_id(entity,host,error)) throw std::invalid_argument(error);
        if (host == wall_id) result.push_back(&entity);
    }
    return result;
}

double wall_frame(const Entity& entity) {
    Wall wall;
    std::string error;
    if (!read_document_wall(entity,{},wall,error)) throw std::invalid_argument(error);
    return std::atan2(wall.baseline.end.y-wall.baseline.start.y,
                      wall.baseline.end.x-wall.baseline.start.x);
}

double building_frame(const BuildingObject& object) {
    return std::visit([](const auto& value) -> double {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,RectangularColumn> || std::is_same_v<T,CircularColumn>)
            return value.rotation_radians;
        else if constexpr (std::is_same_v<T,Beam>)
            return std::atan2(value.end.y-value.start.y,value.end.x-value.start.x);
        else return value.orientation_radians;
    },object);
}

Entity resize_building(const Entity& original, const Resize& resize) {
    auto object = decode_building_entity(original);
    if (const auto* railing=std::get_if<Railing>(&object); railing && railing->host)
        throw std::invalid_argument("Hosted railing plan dimensions follow its stair; resize the host stair instead");
    if (const auto* stair=std::get_if<StairFlight>(&object);
        stair && (stair->flights.size()>1 || !stair->landings.empty()))
        throw std::invalid_argument("Multi-flight and turned stairs require family dimension edits; independent plan-axis resizing is unsupported");
    std::optional<Bounds2> original_bounds;
    if (original.type == "roof" || original.type == "railing")
        original_bounds = plan_axis_resize_bounds(original);
    const auto positive_core = [](double value) {
        if (!std::isfinite(value) || value <= default_geometry_tolerance_metres)
            throw std::invalid_argument("Requested plan footprint is smaller than its retained overhang or section dimensions");
        return value;
    };
    std::visit([&](auto& value) {
        using T = std::decay_t<decltype(value)>;
        const auto [along, across] = resize.local_factors(building_frame(object));
        if constexpr (std::is_same_v<T,RectangularColumn>) {
            resize.point(value.base_center);
            value.width *= along; value.depth *= across;
        } else if constexpr (std::is_same_v<T,CircularColumn>) {
            if (!equal_factors(resize.x,resize.y))
                throw std::invalid_argument("Circular columns require equal plan factors; elliptical columns are unsupported");
            resize.point(value.base_center); value.radius *= resize.x;
        } else if constexpr (std::is_same_v<T,Beam>) {
            const double dx=value.end.x-value.start.x, dy=value.end.y-value.start.y;
            const double length=std::hypot(dx,dy);
            // The v1 beam section follows a 3D up vector. XY scaling of an
            // oblique section would require a skewed section unavailable here.
            if (std::abs(value.end.z-value.start.z) > default_geometry_tolerance_metres ||
                !(length > default_geometry_tolerance_metres) ||
                std::abs(value.up.x*dy-value.up.y*dx) > angular_tolerance*length*std::max(1.0,std::abs(value.up.z)))
                throw std::invalid_argument("Plan resize currently requires a horizontal beam with a vertical section frame");
            resize.point(value.start); resize.point(value.end); value.width *= across;
        } else if constexpr (std::is_same_v<T,StairFlight>) {
            resize.point(value.base_position);
            value.going *= along; value.width *= across;
            if (value.top_landing) value.top_landing->depth *= along;
        } else if constexpr (std::is_same_v<T,Railing>) {
            resize.point(value.base_position);
            const double old_length = value.length;
            value.thickness *= across;
            const double full_length = original_bounds->maximum.x-original_bounds->minimum.x;
            value.length = along == 1 && across == 1 ? old_length
                : positive_core(full_length*along-value.thickness);
            value.post_spacing *= value.length/old_length;
        } else {
            resize.point(value.base_position);
            const double old_span = value.span;
            const double full_x = original_bounds->maximum.x-original_bounds->minimum.x;
            const double full_y = original_bounds->maximum.y-original_bounds->minimum.y;
            value.span = across == 1 ? old_span : positive_core(full_y*across-2*value.overhang);
            double along_core_factor;
            if constexpr (std::is_same_v<T,SlopedRoofPanel>) {
                const double old_run = value.run;
                if (along != 1) {
                    // The bottom face's normal thickness adds sin(pitch)*t to
                    // the run footprint. Pitch changes with run while rise is
                    // retained, so solve that exact physical projection.
                    const double target = positive_core(full_x*along-2*value.overhang);
                    if (value.rise == 0) value.run = target;
                    else {
                        if (target <= value.thickness)
                            throw std::invalid_argument("Requested roof run is smaller than its retained normal-thickness projection");
                        double low=0, high=target;
                        for (int i=0; i<80; ++i) {
                            const double mid=(low+high)*.5;
                            const double projection=mid+value.thickness*value.rise/std::hypot(mid,value.rise);
                            if (projection < target) low=mid; else high=mid;
                        }
                        value.run = positive_core((low+high)*.5);
                    }
                }
                along_core_factor = value.run/old_run;
                value.pitch_radians = std::atan2(value.rise,value.run);
            } else {
                const double old_length = value.length;
                value.length = along == 1 ? old_length : positive_core(full_x*along-2*value.overhang);
                along_core_factor = value.length/old_length;
                value.pitch_radians = std::atan2(value.rise,value.span*.5);
            }
            for (auto& opening : value.openings) {
                opening.x *= along_core_factor; opening.y *= value.span/old_span;
                opening.width *= along_core_factor; opening.depth *= value.span/old_span;
            }
        }
    },object);
    const auto canonical = encode_building_entity(object,original.extensions);
    Entity result = original;
    for (const auto& [key,value] : canonical.properties.items()) {
        if (key=="flights" || key=="landings") continue; // Stable records and their metadata are unchanged.
        if (key=="top_landing" && result.properties.contains(key) && result.properties.at(key).is_object() && value.is_object()) {
            for (const auto& [field,dimension] : value.items()) result.properties[key][field]=dimension;
        } else result.properties[key] = value;
    }
    result.properties.erase("transform");
    return result;
}

void compensate_physical_footprint_anchor(const Entity& original, Entity& result, const Resize& resize) {
    if (original.type != "roof" && original.type != "railing") return;
    const double angle = plan_axis_resize_frame(original);
    const auto [along,across] = resize.local_factors(angle);
    const auto before = plan_axis_resize_bounds(original);
    const auto after = plan_axis_resize_bounds(result);
    const double c=std::cos(angle), s=std::sin(angle);
    const Vec2 local_anchor{c*resize.anchor.x+s*resize.anchor.y,
                            -s*resize.anchor.x+c*resize.anchor.y};
    const auto shift = [](double factor, double anchor, double old_min, double old_max,
                          double new_min, double new_max) {
        // An unchanged axis retains its centre; an edited axis pins the
        // selected opposite edge, which is the edge nearest the supplied anchor.
        if (factor == 1) return ((old_min+old_max)-(new_min+new_max))*.5;
        return std::abs(anchor-old_min) <= std::abs(anchor-old_max)
            ? old_min-new_min : old_max-new_max;
    };
    const double dx=shift(along,local_anchor.x,before.minimum.x,before.maximum.x,after.minimum.x,after.maximum.x);
    const double dy=shift(across,local_anchor.y,before.minimum.y,before.maximum.y,after.minimum.y,after.maximum.y);
    auto& base=result.properties.at("base_position_m");
    base[0] = base[0].get<double>()+c*dx-s*dy;
    base[1] = base[1].get<double>()+s*dx+c*dy;
}

TopoDS_Shape entity_shape(const Entity& entity) {
    std::string error;
    if (entity.type == "wall") {
        Wall value;
        if (!read_document_wall(entity,{},value,error)) throw std::invalid_argument(error);
        return make_wall(value);
    }
    if (entity.type == "room") {
        RoomVolume value;
        if (!read_document_room(entity,value,error)) throw std::invalid_argument(error);
        return make_room_volume(value);
    }
    if (entity.type == "slab") {
        Slab value;
        if (!read_document_slab(entity,value,error)) throw std::invalid_argument(error);
        return make_slab(value);
    }
    return make_building_shape(decode_building_entity(entity));
}

void validate_dependent_geometry(const DocumentSnapshot& preview, const std::string& changed_id) {
    const auto& changed = preview.entities().at(changed_id);
    const auto resolved = resolve_vertical_placement(preview,changed);
    if (changed.type == "wall") {
        const auto openings = hosted_openings(preview,changed_id);
        Wall wall;
        std::string error;
        if (!read_document_wall(resolved,openings,wall,error)) throw std::invalid_argument(error);
        (void)make_wall(wall);
        for (std::size_t i=0; i<openings.size(); ++i) {
            const auto assembly = openings[i]->properties.find("opening_assembly");
            if (assembly == openings[i]->properties.end()) continue;
            std::optional<DoorOperation> operation;
            if (openings[i]->properties.contains("door_operation"))
                operation = decode_door_operation(openings[i]->properties.at("door_operation"));
            (void)make_opening_assembly(wall,wall.openings[i],parse_opening_assembly(*assembly),operation);
        }
    } else (void)entity_shape(resolved);

    // A source-member resize must not leave a formerly valid fused join with
    // disconnected or otherwise inadmissible geometry.
    for (const auto& [id,entity] : preview.entities()) {
        if (changed.type == "wall" && entity.type == "wall_join") {
            const auto join = parse_wall_join(entity.properties,id);
            if (std::find(join.wall_ids.begin(),join.wall_ids.end(),changed_id) == join.wall_ids.end()) continue;
            std::vector<Wall> walls;
            for (const auto& member : join.wall_ids) {
                Wall wall;
                std::string error;
                const auto member_entity = resolve_vertical_placement(preview,preview.entities().at(member));
                if (!read_document_wall(member_entity,hosted_openings(preview,member),wall,error))
                    throw std::invalid_argument(error);
                walls.push_back(std::move(wall));
            }
            (void)make_wall_join(join,walls);
        } else if (changed.type == "roof" && entity.type == "roof_join") {
            const auto join = parse_roof_join(entity.properties,id);
            if (std::find(join.roof_ids.begin(),join.roof_ids.end(),changed_id) == join.roof_ids.end()) continue;
            std::vector<TopoDS_Shape> shapes;
            for (const auto& member : join.roof_ids)
                shapes.push_back(entity_shape(resolve_vertical_placement(preview,preview.entities().at(member))));
            (void)make_roof_join(join,shapes);
        }
    }
}
} // namespace

double plan_axis_resize_frame(const Entity& entity) {
    if (entity.type=="railing" && entity.properties.contains("host"))
        throw std::invalid_argument("Hosted railing plan frame follows its stair; select the host stair instead");
    if (entity.type == "wall") return wall_frame(entity);
    if (entity.type == "room" || entity.type == "slab") {
        Boundary boundary;
        std::string error;
        if (entity.type == "room") {
            RoomVolume room;
            if (!read_document_room(entity,room,error)) throw std::invalid_argument(error);
            boundary = std::move(room.boundary);
        } else {
            Slab slab;
            if (!read_document_slab(entity,slab,error)) throw std::invalid_argument(error);
            boundary = std::move(slab.boundary);
        }
        for (const auto& edge : boundary) {
            const auto dx = edge.end.x-edge.start.x, dy = edge.end.y-edge.start.y;
            if (std::hypot(dx,dy) > default_geometry_tolerance_metres) return std::atan2(dy,dx);
        }
        throw std::invalid_argument("Plan resize footprint has no nonzero boundary edge");
    }
    // Projection refreshes ask for the frame repeatedly. Decoding a canonical
    // BuildingObject also constructs OCCT geometry, so read only the frame's
    // authoritative fields here; resize admission still uses the full codec.
    const auto& p = entity.properties;
    const auto form = p.at("form").get<std::string>();
    if (entity.type == "column" && form == "circular_column" && !p.contains("rotation_rad")) return 0;
    if (entity.type == "beam" && form == "straight_beam") {
        const auto& a = p.at("start_m"); const auto& b = p.at("end_m");
        if (!a.is_array() || !b.is_array() || a.size()!=3 || b.size()!=3)
            throw std::invalid_argument("Beam resize frame requires canonical endpoints");
        const double dx=b.at(0).get<double>()-a.at(0).get<double>();
        const double dy=b.at(1).get<double>()-a.at(1).get<double>();
        if (!std::isfinite(dx) || !std::isfinite(dy))
            throw std::invalid_argument("Beam resize frame must be finite");
        return std::atan2(dy,dx);
    }
    const char* key = nullptr;
    if (entity.type == "column" && (form == "rectangular_column" || form == "circular_column"))
        key = "rotation_rad";
    else if ((entity.type == "stair" && (form == "straight_stair_flight" || form == "multi_flight_stair")) ||
             (entity.type == "railing" && form == "straight_railing") ||
             (entity.type == "roof" && (form == "sloped_roof_panel" || form == "gable_roof" || form == "hip_roof")))
        key = "orientation_rad";
    if (!key) throw std::invalid_argument("Unsupported architectural plan resize form");
    const double angle = p.at(key).get<double>();
    if (!std::isfinite(angle)) throw std::invalid_argument("Plan resize frame angle must be finite");
    return angle;
}

Bounds2 plan_axis_resize_bounds(const Entity& entity) {
    const double angle = plan_axis_resize_frame(entity);
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0,0,0),gp_Dir(0,0,1)),-angle);
    const auto shape = BRepBuilderAPI_Transform(entity_shape(entity),rotation,true).Shape();
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(shape,bounds,false,false);
    double xmin,ymin,zmin,xmax,ymax,zmax;
    bounds.Get(xmin,ymin,zmin,xmax,ymax,zmax);
    return {{xmin,ymin},{xmax,ymax}};
}

ApplyEntityChanges plan_axis_resize_command(const DocumentSnapshot& source, const std::string& entity_id,
    double scale_x, double scale_y, Vec2 anchor, double frame_rotation_radians) {
    if (!source.is_editable()) throw DocumentError(DocumentErrorCode::read_only,source.read_only_reason());
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x <= 0 || scale_y <= 0 ||
        !std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(frame_rotation_radians))
        throw std::invalid_argument("Plan resize factors must be positive and all frame coordinates finite");
    const auto found = source.entities().find(entity_id);
    if (found == source.entities().end())
        throw DocumentError(DocumentErrorCode::dangling_reference,"Plan resize target is missing");
    const Entity& original = found->second;
    Entity result = original;
    const Resize resize{scale_x,scale_y,anchor,frame_rotation_radians};
    ApplyEntityChanges command{source.revision(),{}, {},"Resize plan dimensions"};
    std::string error;
    if (original.type == "wall") {
        const auto children = hosted_openings(source,entity_id);
        Wall wall;
        if (!read_document_wall(original,children,wall,error)) throw std::invalid_argument(error);
        const auto [along,across] = resize.local_factors(wall_frame(original));
        wall.baseline = resize.segment(wall.baseline);
        wall.thickness *= across;
        for (auto& layer : wall.layers) layer.thickness *= across;
        for (auto& opening : wall.openings) { opening.offset *= along; opening.width *= along; }
        (void)make_wall(wall);
        update_segment(result.properties["baseline"],wall.baseline);
        set_dimension(result.properties,"thickness_m","thickness",wall.thickness);
        if (result.properties.contains("layers")) result.properties["layers"] = wall_layers_json(wall.layers);
        for (std::size_t i=0; i<children.size(); ++i) {
            Entity opening = *children[i];
            set_dimension(opening.properties,"offset_m","offset",wall.openings[i].offset);
            set_dimension(opening.properties,"width_m","width",wall.openings[i].width);
            if (opening.properties.contains("opening_assembly")) {
                auto assembly = parse_opening_assembly(opening.properties.at("opening_assembly"));
                // Manufactured frame face width remains a physical component
                // dimension; only its host-normal depths follow wall thickness.
                assembly.frame_depth_m *= across; assembly.panel_thickness_m *= across;
                assembly.glazing_thickness_m *= across; assembly.inset_m *= across;
                opening.properties["opening_assembly"] = opening_assembly_json(assembly);
            }
            invalidate_changed_quantity_entries(*children[i],opening);
            if (opening != *children[i]) command.entity_changes.push_back(EntityChange::upsert(std::move(opening)));
        }
    } else if (original.type == "room") {
        RoomVolume room;
        if (!read_document_room(original,room,error)) throw std::invalid_argument(error);
        room.boundary = resize.boundary(room.boundary);
        for (auto& hole : room.holes) hole = resize.boundary(hole);
        (void)make_room_volume(room);
        update_footprint(result,room.boundary,room.holes);
    } else if (original.type == "slab") {
        Slab slab;
        if (!read_document_slab(original,slab,error)) throw std::invalid_argument(error);
        slab.boundary = resize.boundary(slab.boundary);
        for (auto& hole : slab.holes) hole = resize.boundary(hole);
        (void)make_slab(slab);
        update_footprint(result,slab.boundary,slab.holes);
    } else result = resize_building(original,resize);
    compensate_physical_footprint_anchor(original,result,resize);
    result.properties.erase("transform");
    invalidate_changed_quantity_entries(original,result);
    if (result != original) command.entity_changes.push_back(EntityChange::upsert(std::move(result)));
    if (!command.entity_changes.empty()) {
        const auto preview = Document::preview_command(source,command);
        validate_dependent_geometry(preview,entity_id);
        if (original.type=="stair") {
            for (const auto& [id,entity] : preview.entities()) {
                const auto& p=entity.properties;
                if (entity.type!="railing" || !p.is_object() || !p.contains("version") ||
                    !p.at("version").is_number_integer() || p.at("version")!=2 ||
                    !p.contains("form") || p.at("form")!="stair_flight_railing") continue;
                const auto rail=decode_railing_properties(id,entity.properties);
                if (rail.host->stair_id==entity_id)
                    (void)make_building_shape(rail,preview.entities());
            }
        }
    }
    return command;
}
} // namespace sketch
