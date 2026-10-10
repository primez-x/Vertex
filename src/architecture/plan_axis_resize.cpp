#include "sketch/plan_axis_resize.hpp"

#include "sketch/architectural_document_adapter.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/phase_roof_resize.hpp"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
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
    bool retain_distinct_factors = false;

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
    Vec2 top_gradient(Vec2 gradient) const {
        const double c = std::cos(angle), s = std::sin(angle);
        // A plan resize leaves every corresponding top height unchanged.
        // A plane gradient therefore follows the inverse transpose of its
        // XY transform, rather than scaling like a point or direction.
        const double u = (c*gradient.x+s*gradient.y)/x;
        const double v = (-s*gradient.x+c*gradient.y)/y;
        const Vec2 result{c*u-s*v,s*u+c*v};
        if (!std::isfinite(result.x) || !std::isfinite(result.y))
            throw std::invalid_argument("Plan resize wall top gradient overflows");
        return result;
    }
    std::pair<double,double> local_factors(double natural_angle) const {
        if (x == y || (!retain_distinct_factors && equal_factors(x,y))) return {x,x};
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

double solve_sloped_roof_run(const SlopedRoofPanel& source, double target) {
    if (source.rise == 0) return target;
    const auto projection = [&](double run) {
        return run + source.thickness * (source.rise/std::hypot(run,source.rise));
    };
    if (target == projection(source.run)) return source.run;

    // f(r)=r+t*h/hypot(r,h), so f'(r)=1-g(r), where g has its sole
    // maximum at h/sqrt(2). If t/h > 3*sqrt(3)/2, f has two folds and
    // up to three roots. Keep the monotone branch containing the admitted
    // source run; jumping across a fold would change pitch discontinuously.
    const auto derivative_term = [&](double run) {
        const double hyp = std::hypot(run,source.rise);
        return (source.thickness/hyp)*(source.rise/hyp)*(run/hyp);
    };
    double low=default_geometry_tolerance_metres, high=target;
    bool increasing=true;
    const double peak=source.rise/std::sqrt(2.0);
    if (derivative_term(peak) > 1) {
        const auto fold = [&](double a, double b, bool term_increases) {
            for (int i=0; i<128; ++i) {
                const double mid=a+(b-a)*.5;
                if (mid == a || mid == b) break;
                if ((derivative_term(mid) < 1) == term_increases) a=mid;
                else b=mid;
            }
            return std::pair{a,b};
        };
        const auto maximum=fold(0,peak,true);
        // g(r) < t*h/r^2, hence this upper bracket has g(r) < 1/4.
        const auto minimum=fold(peak,2*std::sqrt(source.thickness)*std::sqrt(source.rise),false);
        const double source_term=derivative_term(source.run);
        if (source.run <= maximum.first && source_term < 1) high=std::min(high,maximum.first);
        else if (source.run >= maximum.second && source.run <= minimum.first && source_term > 1) {
            low=std::max(low,maximum.second);
            high=std::min(high,minimum.first);
            increasing=false;
        } else if (source.run >= minimum.second && source_term < 1) low=std::max(low,minimum.second);
        else throw std::invalid_argument(
            "Roof panel source run is at or numerically indistinguishable from a footprint fold; the resize has ambiguous continuous branches");
    }
    if (low > high)
        throw std::invalid_argument("Requested roof footprint is outside the source panel's continuous run branch");
    const double low_projection=projection(low), high_projection=projection(high);
    const double minimum=std::min(low_projection,high_projection);
    const double maximum=std::max(low_projection,high_projection);
    const double roundoff=64*std::numeric_limits<double>::epsilon()*
        std::max({1.0,target,maximum});
    if ((target < minimum && minimum-target > roundoff) ||
        (target > maximum && target-maximum > roundoff))
        throw std::invalid_argument(
            "Requested roof footprint cannot be reached on the source panel's continuous run branch with retained rise and thickness");
    for (int i=0; i<128; ++i) {
        const double mid=low+(high-low)*.5;
        if (mid == low || mid == high) break;
        if ((projection(mid) < target) == increasing) low=mid;
        else high=mid;
    }
    // Choose the closest representable run on this branch. The lower native
    // dimension boundary is open; it must never become an admitted zero grip.
    const double run=low > default_geometry_tolerance_metres &&
        std::abs(projection(low)-target) < std::abs(projection(high)-target) ? low : high;
    if (!std::isfinite(run) || run <= default_geometry_tolerance_metres)
        throw std::invalid_argument("Requested roof run is below the native positive-dimension tolerance");
    if (std::abs(projection(run)-target) > default_geometry_tolerance_metres+roundoff)
        throw std::invalid_argument("Roof panel footprint cannot be resolved to native geometry tolerance");
    return run;
}

void resize_stair(StairFlight& stair, const Resize& resize, double along, double across) {
    const auto before = derive_stair_layout(stair);
    resize.point(stair.base_position);
    stair.going *= along;
    stair.width *= across;
    // Defaults follow the first flight's frame. Every ordered flight follows
    // its own physical axes; quarter turns swap the requested plan factors.
    // Keep inherited fields absent only when the new default is exactly the
    // required dimension, and preserve every explicitly authored override.
    for (std::size_t i=0; i<stair.flights.size(); ++i) {
        auto& record=stair.flights[i];
        const auto& flight=before.flights[i];
        const auto [flight_along,flight_across]=resize.local_factors(flight.orientation_radians);
        const double going=flight.going*flight_along,width=flight.width*flight_across;
        if (record.going || going!=stair.going) record.going=going;
        if (record.width || width!=stair.width) record.width=width;
    }
    for (std::size_t i=0; i<stair.landings.size(); ++i) {
        const auto [landing_along,landing_across] =
            resize.local_factors(before.flights[i].orientation_radians);
        stair.landings[i].depth *= landing_along;
        stair.landings[i].return_gap *= landing_across;
    }
    if (stair.top_landing) {
        const auto factors = resize.local_factors(before.flights.back().orientation_radians);
        stair.top_landing->depth *= factors.first;
    }

    // Prove the typed reconstruction is the requested affine plan edit. This
    // checks placement and contacts, including right turns and repeated returns,
    // rather than accepting only a matching overall bounding rectangle.
    const auto after = derive_stair_layout(stair);
    const auto check_point = [&](Vec3 original, Vec3 actual) {
        resize.point(original);
        const double roundoff = 32 * std::numeric_limits<double>::epsilon() *
            std::max({1.0,std::abs(original.x),std::abs(original.y),
                      std::abs(actual.x),std::abs(actual.y)});
        if (std::hypot(original.x-actual.x,original.y-actual.y) >
                default_geometry_tolerance_metres+roundoff || original.z != actual.z)
            throw std::invalid_argument(
                "Requested plan resize cannot preserve this stair's flight and landing geometry");
    };
    for (std::size_t i=0; i<before.flights.size(); ++i) {
        check_point(before.flights[i].base_position,after.flights[i].base_position);
        check_point(before.flights[i].end_position,after.flights[i].end_position);
        for (std::size_t corner=0; corner<4; ++corner)
            check_point(before.flights[i].footprint[corner],after.flights[i].footprint[corner]);
    }
    for (std::size_t i=0; i<before.landings.size(); ++i)
        for (std::size_t corner=0; corner<4; ++corner)
            check_point(before.landings[i].footprint[corner],after.landings[i].footprint[corner]);
}

Entity resize_building(const Entity& original, const Resize& resize) {
    auto object = decode_building_entity(original);
    if (const auto* railing=std::get_if<Railing>(&object); railing && (railing->host || railing->landing_host))
        throw std::invalid_argument("Hosted railing plan dimensions follow its stair; resize the host stair instead");
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
            resize_stair(value,resize,along,across);
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
                    value.run = solve_sloped_roof_run(value,target);
                }
                along_core_factor = value.run/old_run;
                if (value.run != old_run)
                    value.pitch_radians = std::atan2(value.rise,value.run);
            } else {
                const double old_length = value.length;
                value.length = along == 1 ? old_length : positive_core(full_x*along-2*value.overhang);
                along_core_factor = value.length/old_length;
                if (value.span != old_span)
                    value.pitch_radians = std::atan2(value.rise,value.span*.5);
            }
            for (auto& opening : value.openings) {
                // Resize the historical roof-local reference rectangle. Its
                // actual surface rotation stays authored; native facet and
                // overlap admission checks the resulting oriented mouth.
                opening.x *= along_core_factor; opening.y *= value.span/old_span;
                opening.width *= along_core_factor; opening.depth *= value.span/old_span;
            }
        }
    },object);
    const auto canonical = encode_building_entity(object,original.extensions);
    Entity result = original;
    if (original.type == "roof") {
        // The native codec establishes resized geometry, not authority over
        // source receipts, schema/roster identity or opaque authoring data.
        for (const auto* key : {"run_m","length_m","span_m","pitch_rad"})
            if (canonical.properties.contains(key) &&
                result.properties.at(key) != canonical.properties.at(key))
                result.properties.at(key) = canonical.properties.at(key);
        for (std::size_t coordinate=0; coordinate<2; ++coordinate)
            if (result.properties.at("base_position_m").at(coordinate) !=
                canonical.properties.at("base_position_m").at(coordinate))
                result.properties.at("base_position_m").at(coordinate) =
                    canonical.properties.at("base_position_m").at(coordinate);
        if (canonical.properties.contains("roof_openings")) {
            auto& openings = result.properties.at("roof_openings");
            const auto& resized = canonical.properties.at("roof_openings");
            for (std::size_t i=0; i<resized.size(); ++i)
                for (const auto* key : {"x_m","y_m","width_m","depth_m"})
                    if (openings.at(i).at(key) != resized.at(i).at(key))
                        openings.at(i).at(key) = resized.at(i).at(key);
        }
        return result;
    }
    if (original.type == "column" || original.type == "beam") {
        // Preserve the actual source envelope, including unchanged scalar
        // representations, Z coordinates and opaque authoring metadata.
        for (const auto* key : {"width_m","depth_m","radius_m"})
            if (canonical.properties.contains(key) &&
                result.properties.at(key) != canonical.properties.at(key))
                result.properties.at(key) = canonical.properties.at(key);
        for (const auto* key : {"base_center_m","start_m","end_m"}) {
            if (!canonical.properties.contains(key)) continue;
            for (std::size_t coordinate=0; coordinate<2; ++coordinate)
                if (result.properties.at(key).at(coordinate) !=
                    canonical.properties.at(key).at(coordinate))
                    result.properties.at(key).at(coordinate) =
                        canonical.properties.at(key).at(coordinate);
        }
        result.properties.erase("transform");
        return result;
    }
    for (const auto& [key,value] : canonical.properties.items()) {
        if (key=="flights") {
            // Patch only owned dimensions. Stable child identities, counts,
            // order and opaque per-flight authoring metadata stay authored.
            auto& flights=result.properties.at(key);
            for (std::size_t i=0; i<value.size(); ++i)
                for (const auto* dimension : {"going_m","width_m"}) {
                    if (value[i].contains(dimension)) flights[i][dimension]=value[i].at(dimension);
                    else flights[i].erase(dimension);
                }
            continue;
        }
        if (key=="version" && original.type=="stair" &&
            ((value==2 && original.properties.at("version")==3) ||
             ((value==2 || value==3) && original.properties.at("version")==4))) continue;
        if (key=="landings") {
            // Update only dimensions: child IDs, turns, straight alignment,
            // thicknesses, order and opaque authoring data stay authored.
            auto& landings = result.properties.at(key);
            for (std::size_t i=0; i<value.size(); ++i) {
                landings[i]["depth_m"] = value[i].at("depth_m");
                landings[i]["return_gap_m"] = value[i].at("return_gap_m");
            }
            continue;
        }
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
    const double x=base[0].get<double>()+c*dx-s*dy;
    const double y=base[1].get<double>()+s*dx+c*dy;
    if (original.type != "roof" || base[0] != x) base[0] = x;
    if (original.type != "roof" || base[1] != y) base[1] = y;
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

void validate_dependent_geometry(const DocumentSnapshot& source,
                                 const DocumentSnapshot& preview, const std::string& changed_id) {
    const auto& changed = preview.entities().at(changed_id);
    if (changed.type == "wall") {
        // The shared boundary admits every sibling's explicit/default frame
        // and affected fused wall joins against the completed candidate.
        validate_architectural_geometry_changes(source,preview,{changed_id});
        return;
    }
    if (changed.type == "room" && !has_document_room_volume_fields(changed)) {
        DocumentRoomFootprint footprint;
        std::string error;
        if (!read_document_room_footprint(changed,footprint,error))
            throw std::invalid_argument(error);
        return;
    }
    (void)entity_shape(resolve_vertical_placement(preview,changed));

    // Roof-dependent admission remains on its existing native path.
    for (const auto& [id,entity] : preview.entities()) {
        if (changed.type == "roof" && entity.type == "roof_join") {
            const auto join = parse_roof_join(entity.properties,id);
            if (std::find(join.roof_ids.begin(),join.roof_ids.end(),changed_id) == join.roof_ids.end()) continue;
            std::vector<TopoDS_Shape> shapes;
            for (const auto& member : join.roof_ids)
                shapes.push_back(entity_shape(resolve_vertical_placement(preview,preview.entities().at(member))));
            validate_roof_join_skylights(join, preview.entities());
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
            if (has_document_room_volume_fields(entity)) {
                RoomVolume room;
                if (!read_document_room(entity,room,error)) throw std::invalid_argument(error);
                boundary = std::move(room.boundary);
            } else {
                DocumentRoomFootprint footprint;
                if (!read_document_room_footprint(entity,footprint,error)) throw std::invalid_argument(error);
                boundary = std::move(footprint.boundary);
            }
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
    if (entity.type == "room" && !has_document_room_volume_fields(entity)) {
        DocumentRoomFootprint footprint;
        std::string error;
        if (!read_document_room_footprint(entity,footprint,error)) throw std::invalid_argument(error);
        const PlanarTransform rotation{{0,0},-angle,false,false,{0,0}};
        for (auto& edge : footprint.boundary) edge = transform_segment(edge,rotation);
        return boundary_bounds(footprint.boundary);
    }
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0,0,0),gp_Dir(0,0,1)),-angle);
    const auto shape = BRepBuilderAPI_Transform(entity_shape(entity),rotation,true).Shape();
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(shape,bounds,false,false);
    double xmin,ymin,zmin,xmax,ymax,zmax;
    bounds.Get(xmin,ymin,zmin,xmax,ymax,zmax);
    return {{xmin,ymin},{xmax,ymax}};
}

Entity stage_structural_plan_axis_resize_entity(
    const Entity& actual_source, double scale_x, double scale_y, Vec2 anchor,
    double frame_rotation_radians) {
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x <= 0 || scale_y <= 0 ||
        !std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(frame_rotation_radians))
        throw std::invalid_argument("Plan resize factors must be positive and all frame coordinates finite");
    const auto& properties = actual_source.properties;
    if (!properties.is_object() || !properties.contains("version") ||
        !properties.at("version").is_number_integer() || properties.at("version") != 1 ||
        !properties.contains("form") || !properties.at("form").is_string() ||
        !((actual_source.type == "column" &&
           (properties.at("form") == "rectangular_column" || properties.at("form") == "circular_column")) ||
          (actual_source.type == "beam" && properties.at("form") == "straight_beam")))
        throw std::invalid_argument("Plan structural resize requires a canonical v1 column or straight beam");
    (void)entity_shape(actual_source);
    if (scale_x == 1 && scale_y == 1) return actual_source;
    if (properties.at("form") == "circular_column" && scale_x != scale_y)
        throw std::invalid_argument("Circular columns require equal plan factors; elliptical columns are unsupported");
    const Resize resize{scale_x,scale_y,anchor,frame_rotation_radians,true};
    auto result = resize_building(actual_source,resize);
    compensate_physical_footprint_anchor(actual_source,result,resize);
    (void)entity_shape(result);
    const auto before = plan_axis_resize_bounds(actual_source);
    const auto after = plan_axis_resize_bounds(result);
    const auto [along,across] = resize.local_factors(plan_axis_resize_frame(actual_source));
    const auto check_extent = [](double old_min, double old_max, double new_min,
                                 double new_max, double factor) {
        const double expected = (old_max-old_min)*factor;
        const double actual = new_max-new_min;
        const double roundoff = 64*std::numeric_limits<double>::epsilon()*
            std::max({1.0,std::abs(old_min),std::abs(old_max),std::abs(new_min),
                      std::abs(new_max),std::abs(expected)});
        if (!std::isfinite(expected) || !std::isfinite(actual) || actual <= 0 ||
            std::abs(actual-expected) > default_geometry_tolerance_metres+roundoff)
            throw std::invalid_argument("Generated structural footprint does not meet the requested plan resize within native tolerance");
    };
    check_extent(before.minimum.x,before.maximum.x,after.minimum.x,after.maximum.x,along);
    check_extent(before.minimum.y,before.maximum.y,after.minimum.y,after.maximum.y,across);
    invalidate_changed_quantity_entries(actual_source,result);
    return result;
}

Entity stage_roof_plan_axis_resize_entity(
    const Entity& actual_source, double scale_x, double scale_y, Vec2 anchor,
    double frame_rotation_radians) {
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x <= 0 || scale_y <= 0 ||
        !std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(frame_rotation_radians))
        throw std::invalid_argument("Plan resize factors must be positive and all frame coordinates finite");
    const auto& properties = actual_source.properties;
    if (actual_source.type != "roof" || !properties.is_object() ||
        !properties.contains("form") || !properties.at("form").is_string() ||
        (properties.at("form") != "sloped_roof_panel" &&
         properties.at("form") != "gable_roof" && properties.at("form") != "hip_roof"))
        throw std::invalid_argument("Plan roof resize requires a supported parametric roof");
    if (scale_x == 1 && scale_y == 1) {
        (void)decode_building_entity(actual_source);
        return actual_source;
    }
    const Resize resize{scale_x,scale_y,anchor,frame_rotation_radians,true};
    auto result = resize_building(actual_source,resize);
    compensate_physical_footprint_anchor(actual_source,result,resize);
    const auto before=plan_axis_resize_bounds(actual_source);
    const auto after=plan_axis_resize_bounds(result);
    const auto [along,across]=resize.local_factors(plan_axis_resize_frame(actual_source));
    const auto check_extent = [](double old_min, double old_max, double new_min,
                                 double new_max, double factor) {
        const double expected=(old_max-old_min)*factor;
        const double actual=new_max-new_min;
        const double roundoff=64*std::numeric_limits<double>::epsilon()*
            std::max({1.0,std::abs(old_min),std::abs(old_max),std::abs(new_min),
                      std::abs(new_max),std::abs(expected)});
        if (!std::isfinite(expected) || !std::isfinite(actual) || actual <= 0 ||
            std::abs(actual-expected) > default_geometry_tolerance_metres+roundoff)
            throw std::invalid_argument("Generated roof footprint does not meet the requested plan resize within native tolerance");
    };
    // Native bounds include the retained normal-thickness projection and
    // overhang. Admit the final translated solid, not just the scalar inverse.
    check_extent(before.minimum.x,before.maximum.x,after.minimum.x,after.maximum.x,along);
    check_extent(before.minimum.y,before.maximum.y,after.minimum.y,after.maximum.y,across);
    return result;
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
        if (wall.top_gradient_m_per_m) {
            wall.top_gradient_m_per_m = resize.top_gradient(*wall.top_gradient_m_per_m);
            const auto gradient = *wall.top_gradient_m_per_m;
            wall.slope_rise = gradient.x * (wall.baseline.end.x-wall.baseline.start.x) +
                              gradient.y * (wall.baseline.end.y-wall.baseline.start.y);
        }
        wall.thickness *= across;
        for (auto& layer : wall.layers) layer.thickness *= across;
        for (auto& opening : wall.openings) { opening.offset *= along; opening.width *= along; }
        (void)make_wall(wall);
        update_segment(result.properties["baseline"],wall.baseline);
        if (wall.top_gradient_m_per_m) {
            result.properties["top_plane"] = wall_top_plane_json(*wall.top_gradient_m_per_m);
            set_dimension(result.properties,"slope_rise_m","slope_rise",*wall.slope_rise);
        }
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
        if (has_document_room_volume_fields(original)) {
            RoomVolume room;
            if (!read_document_room(original,room,error)) throw std::invalid_argument(error);
            room.boundary = resize.boundary(room.boundary);
            for (auto& hole : room.holes) hole = resize.boundary(hole);
            (void)make_room_volume(room);
            update_footprint(result,room.boundary,room.holes);
        } else {
            DocumentRoomFootprint footprint;
            if (!read_document_room_footprint(original,footprint,error)) throw std::invalid_argument(error);
            footprint.boundary = resize.boundary(footprint.boundary);
            for (auto& hole : footprint.holes) hole = resize.boundary(hole);
            if (const auto invalid = validate_boundary_holes(footprint.boundary,footprint.holes))
                throw std::invalid_argument(*invalid);
            update_footprint(result,footprint.boundary,footprint.holes);
        }
    } else if (original.type == "slab") {
        Slab slab;
        if (!read_document_slab(original,slab,error)) throw std::invalid_argument(error);
        slab.boundary = resize.boundary(slab.boundary);
        for (auto& hole : slab.holes) hole = resize.boundary(hole);
        (void)make_slab(slab);
        update_footprint(result,slab.boundary,slab.holes);
    } else if (original.type == "roof") {
        result = replay_roof_plan_resize_entity(original,
            {entity_id, scale_x, scale_y, anchor, frame_rotation_radians});
    } else if (original.type == "column" || original.type == "beam") {
        result = stage_structural_plan_axis_resize_entity(original,scale_x,scale_y,anchor,frame_rotation_radians);
    } else result = resize_building(original,resize);
    if (original.type != "roof" && original.type != "column" && original.type != "beam") {
        compensate_physical_footprint_anchor(original,result,resize);
        result.properties.erase("transform");
        invalidate_changed_quantity_entries(original,result);
    }
    if (result != original) command.entity_changes.push_back(EntityChange::upsert(std::move(result)));
    if (!command.entity_changes.empty()) {
        const auto preview = Document::preview_command(source,command);
        validate_dependent_geometry(source,preview,entity_id);
        if (original.type=="stair") {
            for (const auto& [id,entity] : preview.entities()) {
                const auto& p=entity.properties;
                if (entity.type!="railing" || !p.is_object() || !p.contains("version") ||
                    !p.at("version").is_number_integer() || !p.contains("form") ||
                    !((p.at("version")==2 && p.at("form")=="stair_flight_railing") ||
                      (p.at("version")==3 && p.at("form")=="stair_landing_railing"))) continue;
                const auto rail=decode_railing_properties(id,entity.properties);
                if ((rail.host?rail.host->stair_id:rail.landing_host->stair_id)==entity_id)
                    (void)make_building_shape(rail,preview.entities());
            }
        }
    }
    return command;
}

RoofPlanCornerResizeParameters roof_plan_corner_resize_parameters(
    const Entity& original, std::size_t corner_index, Vec2 proposed_position) {
    const auto& properties = original.properties;
    if (original.type != "roof" || !properties.is_object() ||
        !properties.contains("form") || !properties.at("form").is_string() ||
        (properties.at("form") != "sloped_roof_panel" &&
         properties.at("form") != "gable_roof" && properties.at("form") != "hip_roof"))
        throw DocumentError(DocumentErrorCode::invalid_entity,
            "Roof corner resize requires a supported parametric roof");
    if (corner_index >= 4)
        throw std::invalid_argument("Roof corner index is outside the captured footprint");
    if (!std::isfinite(proposed_position.x) || !std::isfinite(proposed_position.y))
        throw std::invalid_argument("Roof corner target must be finite");

    const double angle = plan_axis_resize_frame(original);
    const auto bounds = plan_axis_resize_bounds(original);
    const double width = bounds.maximum.x-bounds.minimum.x;
    const double depth = bounds.maximum.y-bounds.minimum.y;
    if (!std::isfinite(bounds.minimum.x) || !std::isfinite(bounds.minimum.y) ||
        !std::isfinite(bounds.maximum.x) || !std::isfinite(bounds.maximum.y) ||
        !std::isfinite(width) || !std::isfinite(depth) || width <= 0 || depth <= 0)
        throw std::invalid_argument("Roof corner resize requires finite positive footprint dimensions");
    const bool maximum_x = corner_index == 1 || corner_index == 2;
    const bool maximum_y = corner_index >= 2;
    const Vec2 corner{maximum_x ? bounds.maximum.x : bounds.minimum.x,
                      maximum_y ? bounds.maximum.y : bounds.minimum.y};
    const Vec2 opposite{maximum_x ? bounds.minimum.x : bounds.maximum.x,
                        maximum_y ? bounds.minimum.y : bounds.maximum.y};
    const double c = std::cos(angle), s = std::sin(angle);
    const auto world = [c,s](Vec2 p) { return Vec2{c*p.x-s*p.y,s*p.x+c*p.y}; };
    const auto original_corner = world(corner);
    const auto anchor = world(opposite);
    if (!std::isfinite(original_corner.x) || !std::isfinite(original_corner.y) ||
        !std::isfinite(anchor.x) || !std::isfinite(anchor.y))
        throw std::invalid_argument("Roof corner frame coordinates overflow");
    // Avoid a rotate/unrotate roundoff resize and any codec normalization or
    // receipt invalidation when the caller returns the exact original grip.
    if (proposed_position.x == original_corner.x && proposed_position.y == original_corner.y)
        return {1.0, 1.0, anchor, angle};

    // Project relative to the fixed world anchor to avoid subtracting two
    // independently projected large coordinates. Signed extents reject a grip
    // at or beyond either opposite edge instead of reflecting the roof.
    const double dx = proposed_position.x-anchor.x, dy = proposed_position.y-anchor.y;
    const double scale_x = (c*dx+s*dy)/(maximum_x ? width : -width);
    const double scale_y = (-s*dx+c*dy)/(maximum_y ? depth : -depth);
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y))
        throw std::invalid_argument("Roof corner resize factors overflow");
    if (scale_x <= 0 || scale_y <= 0)
        throw std::invalid_argument("Roof corner cannot cross either opposite footprint edge");
    return {scale_x, scale_y, anchor, angle};
}

ApplyEntityChanges roof_plan_corner_resize_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    std::size_t corner_index, Vec2 proposed_position, Revision expected_revision) {
    if (source.revision() != expected_revision)
        throw DocumentError(DocumentErrorCode::stale_revision,"Roof corner resize source revision is stale");
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only,source.read_only_reason());
    const auto found = source.entities().find(entity_id);
    if (found == source.entities().end())
        throw DocumentError(DocumentErrorCode::dangling_reference,"Roof corner resize target is missing");
    const auto resize = roof_plan_corner_resize_parameters(found->second, corner_index, proposed_position);
    if (resize.scale_x == 1.0 && resize.scale_y == 1.0)
        return {expected_revision, {}, {}, "Resize plan dimensions"};
    return plan_axis_resize_command(source, entity_id, resize.scale_x, resize.scale_y,
        resize.anchor, resize.frame_rotation_radians);
}
} // namespace sketch
