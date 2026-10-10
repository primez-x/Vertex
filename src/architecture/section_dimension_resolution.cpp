#include "sketch/section_dimension_resolution.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"

#include <Standard_Failure.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <Bnd_Box.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <sstream>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;

const Entity& entity(const DocumentSnapshot& source, const std::string& id,
                     const std::string& type = {}) {
    const auto found = source.entities().find(id);
    if (found == source.entities().end()) throw std::invalid_argument("source object is missing: " + id);
    if (!type.empty() && found->second.type != type)
        throw std::invalid_argument("source object has the wrong semantic type: " + id);
    return found->second;
}

void require_active_source(const DocumentSnapshot& source, const std::string& id,
                           const ConstraintPhaseScope& scope) {
    if (scope.inactive_owner_ids.contains(id))
        throw std::invalid_argument("source object is inactive in the saved design: " + id);
    if (scope.inactive_owner_ids.empty()) return;
    const auto found = source.entities().find(id);
    if (found == source.entities().end()) return;
    const auto& input = found->second;
    if (input.type == "opening") {
        std::string host_id, error;
        if (!read_document_wall_id(input, host_id, error)) throw std::invalid_argument(error);
        if (scope.inactive_owner_ids.contains(host_id))
            throw std::invalid_argument("source opening host is inactive in the saved design: " + host_id);
    }
}

Wall resolved_wall(const DocumentSnapshot& source, const Entity& input,
                   const ConstraintPhaseScope& scope) {
    require_active_source(source, input.id, scope);
    std::vector<const Entity*> openings;
    for (const auto& [id, candidate] : source.entities()) {
        if (candidate.type != "opening" || scope.inactive_owner_ids.contains(id)) continue;
        std::string host_id, error;
        if (read_document_wall_id(candidate, host_id, error) && host_id == input.id)
            openings.push_back(&candidate);
    }
    Wall wall;
    std::string error;
    if (!read_document_wall(resolve_vertical_placement(source, input), openings, wall, error))
        throw std::invalid_argument(error);
    return wall;
}

// Shared line/arc semantic parameterization, independent of BRep edge order.
struct Circle {
    Vec2 center;
    double radius;
    double start_angle;
};
Circle circle(const Segment& segment) {
    const double dx = segment.end.x - segment.start.x, dy = segment.end.y - segment.start.y;
    const double chord = std::hypot(dx, dy);
    const double half_sweep = segment.sweep_radians * 0.5;
    const bool half_turn = std::abs(segment.sweep_radians) == std::numbers::pi;
    const double offset = half_turn ? 0.0 : chord / (2 * std::tan(half_sweep));
    const Vec2 center{std::midpoint(segment.start.x, segment.end.x) - dy / chord * offset,
                      std::midpoint(segment.start.y, segment.end.y) + dx / chord * offset};
    return {center, chord / (2 * (half_turn ? 1.0 : std::sin(std::abs(half_sweep)))),
        std::atan2(segment.start.y - center.y, segment.start.x - center.x)};
}
Vec2 point_at(const Segment& segment, double fraction) {
    if (segment.sweep_radians == 0)
        return {segment.start.x + (segment.end.x - segment.start.x) * fraction,
                segment.start.y + (segment.end.y - segment.start.y) * fraction};
    const auto arc = circle(segment);
    const auto angle = arc.start_angle + segment.sweep_radians * fraction;
    return {arc.center.x + arc.radius * std::cos(angle), arc.center.y + arc.radius * std::sin(angle)};
}

TopoDS_Shape source_shape(const DocumentSnapshot& source, const Entity& input,
                         const ConstraintPhaseScope& scope) {
    require_active_source(source, input.id, scope);
    if (input.type == "wall") return make_wall(resolved_wall(source, input, scope));
    if (input.type == "opening") {
        std::string host_id, error;
        if (!read_document_wall_id(input, host_id, error)) throw std::invalid_argument(error);
        const auto host = resolved_wall(source, entity(source, host_id, "wall"), scope);
        // Validate the complete host and all of its active hosted cuts, regardless of
        // which IDs or display visibility appear in the coordinated view.
        (void)make_wall(host);
        const auto opening = std::find_if(host.openings.begin(), host.openings.end(),
            [&](const auto& candidate) { return candidate.id == input.id; });
        if (opening == host.openings.end()) throw std::invalid_argument("opening is absent from its host");
        const auto length = segment_length(host.baseline);
        const Segment span{point_at(host.baseline, opening->offset / length),
            point_at(host.baseline, (opening->offset + opening->width) / length),
            host.baseline.sweep_radians * opening->width / length};
        // Opening dimensions refer to the authoritative wall-cut envelope.
        // Replaceable frames and an open door leaf do not redefine this void.
        return make_wall(Wall{input.id, span, host.thickness, opening->height,
            host.elevation + opening->sill, {}});
    }
    if (input.type == "assembly_instance") {
        AssemblyExpansionBudget budget;
        return make_assembly_geometry(expand_document_assembly_instance(input, source.entities(), budget)).shape;
    }
    if (input.type == "wall_join") {
        const auto join = parse_wall_join(input.properties, input.id);
        std::vector<Wall> walls;
        for (const auto& id : join.wall_ids) walls.push_back(resolved_wall(source, entity(source, id, "wall"), scope));
        return make_wall_join(join, walls);
    }
    if (input.type == "roof_join") {
        const auto join = parse_roof_join(input.properties, input.id);
        validate_roof_join_skylights(join, source.entities());
        std::vector<TopoDS_Shape> roofs;
        for (const auto& id : join.roof_ids) require_active_source(source, id, scope);
        for (const auto& id : join.roof_ids) {
            const auto& roof = entity(source, id, "roof");
            roofs.push_back(make_building_shape(decode_building_entity(resolve_vertical_placement(source, roof))));
        }
        return make_roof_join(join, roofs);
    }
    const auto resolved = resolve_vertical_placement(source, input);
    if (input.type == "slab") {
        Slab slab; std::string error;
        if (!read_document_slab(resolved, slab, error)) throw std::invalid_argument(error);
        return make_slab(slab);
    }
    if (input.type == "room") {
        RoomVolume room; std::string error;
        if (!read_document_room(resolved, room, error)) throw std::invalid_argument(error);
        return make_room_volume(room);
    }
    if (can_recognize_building_entity_type(input.type)) {
        const auto decoded = decode_building_entity(resolved);
        if (const auto* railing = std::get_if<Railing>(&decoded)) {
            if (railing->host) require_active_source(source, railing->host->stair_id, scope);
            if (railing->landing_host) require_active_source(source, railing->landing_host->stair_id, scope);
        }
        return make_building_shape(decoded, source.entities());
    }
    throw std::invalid_argument("source object has no supported architectural geometry: " + input.id);
}

TopoDS_Shape in_view_frame(const TopoDS_Shape& shape, const CoordinatedView& view) {
    // SheetViewModel already validates the orthonormal frame. Use the same
    // right = cross(up, -direction) convention as native view projection.
    const Vec3 normal{-view.direction[0], -view.direction[1], -view.direction[2]};
    const Vec3 right{view.up[1] * normal.z - view.up[2] * normal.y,
        view.up[2] * normal.x - view.up[0] * normal.z,
        view.up[0] * normal.y - view.up[1] * normal.x};
    const auto translation = [&](Vec3 axis) {
        return -(axis.x * view.origin_m[0] + axis.y * view.origin_m[1] + axis.z * view.origin_m[2]);
    };
    gp_Trsf transform;
    transform.SetValues(right.x, right.y, right.z, translation(right),
        view.up[0], view.up[1], view.up[2], translation({view.up[0], view.up[1], view.up[2]}),
        normal.x, normal.y, normal.z, translation(normal));
    BRepBuilderAPI_Transform operation(shape, transform, true);
    if (!operation.IsDone() || operation.Shape().IsNull())
        throw std::invalid_argument("source geometry could not be transformed to the owning view frame");
    return operation.Shape();
}

bool horizontal_plan_frame(const CoordinatedView& view) {
    return view.kind == CoordinatedViewKind::plan &&
        std::abs(view.direction[0]) <= 1e-12 && std::abs(view.direction[1]) <= 1e-12 &&
        std::abs(std::abs(view.direction[2]) - 1.0) <= 1e-12 && std::abs(view.up[2]) <= 1e-12;
}

Boundary plan_room_boundary(const Entity& input, const CoordinatedView& view) {
    DocumentRoomFootprint footprint;
    std::string error;
    if (!read_document_room_footprint(input, footprint, error)) throw std::invalid_argument(error);
    // Match the plan renderer's normalized XY up/right basis, including the
    // horizontal reflection and signed arc reversal in an upward-looking plan.
    // There is no Z coordinate to resolve or fabricate for an analytical room.
    const PlanarTransform transform{{view.origin_m[0], view.origin_m[1]},
        std::atan2(view.up[0], view.up[1]), view.direction[2] > 0.0, false,
        {-view.origin_m[0], -view.origin_m[1]}};
    for (auto& edge : footprint.boundary) edge = transform_segment(edge, transform);
    // The decoder validates every hole's geometry and containment. Interior
    // holes cannot extend the complete outer footprint's support bounds.
    return std::move(footprint.boundary);
}

std::array<double, 2> support(const Boundary& boundary, bool horizontal,
                            double extremum, bool maximum) {
    std::optional<Vec2> result;
    const auto coordinate = [&](Vec2 point) { return horizontal ? point.x : point.y; };
    const auto perpendicular = [&](Vec2 point) { return horizontal ? point.y : point.x; };
    const auto consider = [&](Vec2 point) {
        if (coordinate(point) != extremum) return;
        // Same stable handle policy as the native BRep path: exact support
        // ties choose the maximum perpendicular coordinate, not edge order.
        if (!result || perpendicular(point) > perpendicular(*result)) result = point;
    };
    for (const auto& edge : boundary) {
        const auto bounds = segment_bounds(edge);
        const auto local = coordinate(maximum ? bounds.maximum : bounds.minimum);
        if (local != extremum) continue;
        consider(edge.start);
        consider(edge.end);
        if (edge.sweep_radians == 0.0) continue;
        // segment_bounds already determines whether the cardinal extremum is
        // inside this signed arc, using its exact analytical geometry. A strict
        // extension beyond both endpoints therefore has one interior witness.
        const double endpoint = maximum ? std::max(coordinate(edge.start), coordinate(edge.end))
                                        : std::min(coordinate(edge.start), coordinate(edge.end));
        if (maximum ? local > endpoint : local < endpoint) {
            const auto arc = circle(edge);
            consider(horizontal ? Vec2{local, arc.center.y} : Vec2{arc.center.x, local});
        }
    }
    if (!result) throw std::invalid_argument("analytical room extent has no resolvable support point");
    return {result->x, result->y};
}

Bounds2 silhouette_bounds(const TopoDS_Shape& shape) {
    Bnd_Box box;
    // Underlying curve/surface extrema, with neither mesh approximations nor
    // shape-tolerance inflation. OCCT's geometric solver uses Confusion().
    BRepBndLib::AddOptimal(shape, box, false, false);
    if (box.IsVoid() || box.IsOpen()) throw std::invalid_argument("source geometry has no finite extent");
    double min_x, min_y, min_z, max_x, max_y, max_z;
    box.Get(min_x, min_y, min_z, max_x, max_y, max_z);
    return {{min_x, min_y}, {max_x, max_y}};
}

std::array<double, 2> support(const TopoDS_Shape& shape, bool horizontal,
                            double extremum, bool maximum) {
    // An exterior support face avoids tangency/intersection ambiguity.
    // Minimum distance from the full source BRep to it is the geometric
    // support extremum, including curved faces with no silhouette vertices.
    const double plane_coordinate = extremum + (maximum ? 1.0 : -1.0);
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false);
    double min_x, min_y, min_z, max_x, max_y, max_z;
    box.Get(min_x, min_y, min_z, max_x, max_y, max_z);
    const double radius = std::max({max_x - min_x, max_y - min_y, max_z - min_z}) + 1;
    const gp_Pnt origin = horizontal
        ? gp_Pnt(plane_coordinate, (min_y + max_y) * 0.5, (min_z + max_z) * 0.5)
        : gp_Pnt((min_x + max_x) * 0.5, plane_coordinate, (min_z + max_z) * 0.5);
    const gp_Dir normal = horizontal ? gp_Dir(1, 0, 0) : gp_Dir(0, 1, 0);
    // Bound the face outside the complete perpendicular source bounds.
    // OCCT's infinite-face distance path can return the opposite side of a
    // periodic cylindrical surface rather than the global minimum.
    BRepBuilderAPI_MakeFace plane(gp_Pln(origin, normal), -radius, radius, -radius, radius);
    if (!plane.IsDone()) throw std::invalid_argument("dimension support plane could not be constructed");
    BRepExtrema_DistShapeShape extrema(shape, plane.Face(), tolerance);
    if (!extrema.IsDone() || extrema.NbSolution() < 1 || !std::isfinite(extrema.Value()))
        throw std::invalid_argument("source silhouette extent has no resolvable support point");
    std::optional<gp_Pnt> result;
    for (int index = 1; index <= extrema.NbSolution(); ++index) {
        const auto& point = extrema.PointOnShape1(index);
        const double primary = horizontal ? point.X() : point.Y();
        const double perpendicular = horizontal ? point.Y() : point.X();
        if (std::abs(primary - extremum) > tolerance * 10) continue;
        // Choose the maximum perpendicular coordinate among exact witnesses
        // so reordered BRep topology does not select a different handle.
        if (!result || perpendicular > (horizontal ? result->Y() : result->X())) result = point;
    }
    if (!result) {
        std::ostringstream evidence;
        evidence.precision(17);
        evidence << "bound=" << extremum << " plane=" << plane_coordinate << " distance=" << extrema.Value();
        for (int index = 1; index <= std::min(extrema.NbSolution(), 4); ++index) {
            const auto& point = extrema.PointOnShape1(index);
            evidence << " witness=(" << point.X() << ',' << point.Y() << ',' << point.Z() << ')';
        }
        throw std::invalid_argument("source support point disagrees with its geometric extent: " + evidence.str());
    }
    return {result->X(), result->Y()};
}
void finite_coordinates(const std::array<double, 2>& point) {
    for (const auto value : point)
        if (!std::isfinite(value) || std::abs(value) > 1e6)
            throw std::invalid_argument("resolved dimension coordinate outside finite bounds");
}
} // namespace

SectionDimensionResolution resolve_section_dimension(const DocumentSnapshot& source,
    const CoordinatedView& view, const SectionOverlay& overlay) {
    try {
        if (overlay.kind != SectionOverlayKind::dimension)
            throw std::invalid_argument("dimension resolution requires a coordinated view dimension");
        // Reuse the CAD-free persistence boundary to validate IDs, frame,
        // references and placement even for callers holding detached values.
        // Admit the complete owning view before validating the supplied overlay;
        // replacing its overlay list must not hide invalid unrelated content.
        (void)SheetViewModel::create({view}, {});
        auto candidate = view; candidate.overlays = {overlay};
        (void)SheetViewModel::create({std::move(candidate)}, {});
        const auto scope = constraint_phase_scope(source.entities());
        if (!overlay.object_id.empty()) require_active_source(source, overlay.object_id, scope);
        if (!overlay.dimension_binding) {
            const auto measured = std::hypot(overlay.end_m[0] - overlay.start_m[0],
                overlay.end_m[1] - overlay.start_m[1]);
            return {ResolvedSectionDimension{overlay.start_m, overlay.end_m,
                overlay.start_m, overlay.end_m, measured, false}, {}};
        }
        const auto& binding = *overlay.dimension_binding;
        require_active_source(source, binding.object_id, scope);
        const auto& input = entity(source, binding.object_id);
        std::optional<Boundary> analytical_boundary;
        TopoDS_Shape shape;
        if (input.type == "room" && !has_document_room_volume_fields(input) && horizontal_plan_frame(view))
            analytical_boundary = plan_room_boundary(input, view);
        else
            shape = in_view_frame(source_shape(source, input, scope), view);
        // A full source extent in view coordinates is independent of both
        // display clipping and the renderer's supported projected curves.
        const auto bounds = analytical_boundary ? boundary_bounds(*analytical_boundary) : silhouette_bounds(shape);
        const bool horizontal = binding.axis == SectionDimensionAxis::horizontal;
        const double minimum = horizontal ? bounds.minimum.x : bounds.minimum.y;
        const double maximum = horizontal ? bounds.maximum.x : bounds.maximum.y;
        const double measured = maximum - minimum;
        if (!std::isfinite(measured) || measured <= tolerance)
            throw std::invalid_argument("source object has a degenerate projected extent");
        const auto start = analytical_boundary ? support(*analytical_boundary, horizontal, minimum, false)
                                               : support(shape, horizontal, minimum, false);
        const auto end = analytical_boundary ? support(*analytical_boundary, horizontal, maximum, true)
                                             : support(shape, horizontal, maximum, true);
        const double location = (horizontal ? bounds.maximum.y : bounds.maximum.x) + binding.line_offset_m;
        const auto line_start = horizontal ? std::array<double, 2>{minimum, location}
                                          : std::array<double, 2>{location, minimum};
        const auto line_end = horizontal ? std::array<double, 2>{maximum, location}
                                        : std::array<double, 2>{location, maximum};
        for (const auto& point : {start, end, line_start, line_end}) finite_coordinates(point);
        return {ResolvedSectionDimension{start, end, line_start, line_end, measured, true}, {}};
    } catch (const Standard_Failure& error) {
        const auto message = error.what();
        return {std::nullopt, "coordinated view dimension " + overlay.id +
            (overlay.dimension_binding ? " source " + overlay.dimension_binding->object_id : "") + ": " +
            (message ? std::string(message) : std::string("architectural geometry failed"))};
    } catch (const std::exception& error) {
        return {std::nullopt, "coordinated view dimension " + overlay.id +
            (overlay.dimension_binding ? " source " + overlay.dimension_binding->object_id : "") + ": " + error.what()};
    }
}
} // namespace sketch
