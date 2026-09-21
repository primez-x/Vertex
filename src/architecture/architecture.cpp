#include "sketch/architecture.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <map>
#include <set>
#include <stdexcept>
#include <span>
#include <utility>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;

template <typename Adjacent>
bool members_form_connected_component(std::size_t count, Adjacent&& adjacent) {
    if (count == 0) return false;
    std::vector<bool> reached(count, false);
    std::vector<std::size_t> pending{0};
    reached[0] = true;
    for (std::size_t next = 0; next < pending.size(); ++next) {
        for (std::size_t candidate = 0; candidate < count; ++candidate) {
            if (!reached[candidate] && adjacent(pending[next], candidate)) {
                reached[candidate] = true;
                pending.push_back(candidate);
            }
        }
    }
    return pending.size() == count;
}

void positive(double value, const char* what) {
    if (!std::isfinite(value) || value <= tolerance) throw std::invalid_argument(what);
}

Vec2 centre(const Segment& segment) {
    const double dx = segment.end.x - segment.start.x;
    const double dy = segment.end.y - segment.start.y;
    const double factor = 0.5 / std::tan(segment.sweep_radians * 0.5);
    return {(segment.start.x + segment.end.x) * 0.5 - dy * factor,
            (segment.start.y + segment.end.y) * 0.5 + dx * factor};
}

Vec2 point_at(const Segment& segment, double fraction) {
    if (segment.sweep_radians == 0) {
        return {std::lerp(segment.start.x, segment.end.x, fraction),
                std::lerp(segment.start.y, segment.end.y, fraction)};
    }
    const auto origin = centre(segment);
    const double angle = segment.sweep_radians * fraction;
    const double dx = segment.start.x - origin.x;
    const double dy = segment.start.y - origin.y;
    return {origin.x + dx * std::cos(angle) - dy * std::sin(angle),
            origin.y + dx * std::sin(angle) + dy * std::cos(angle)};
}

struct OpeningFrame {
    Vec2 origin;
    Vec2 along;
    Vec2 left;
};

OpeningFrame opening_frame(const Segment& baseline, double fraction) {
    const auto origin = point_at(baseline, fraction);
    Vec2 along;
    if (baseline.sweep_radians == 0.0) {
        const auto length = segment_length(baseline);
        along = {(baseline.end.x - baseline.start.x) / length,
                 (baseline.end.y - baseline.start.y) / length};
    } else {
        const auto centre_point = centre(baseline);
        const auto radial = Vec2{origin.x - centre_point.x, origin.y - centre_point.y};
        const auto radius = std::hypot(radial.x, radial.y);
        if (!std::isfinite(radius) || radius <= tolerance) {
            throw std::invalid_argument("Opening host tangent is not representable");
        }
        along = baseline.sweep_radians > 0.0
                    ? Vec2{-radial.y / radius, radial.x / radius}
                    : Vec2{radial.y / radius, -radial.x / radius};
    }
    if (!std::isfinite(along.x) || !std::isfinite(along.y)) {
        throw std::invalid_argument("Opening host tangent is not finite");
    }
    return {origin, along, Vec2{-along.y, along.x}};
}

gp_Pnt opening_point(const OpeningFrame& frame, double along, double across,
                     double elevation) {
    return {frame.origin.x + frame.along.x * along + frame.left.x * across,
            frame.origin.y + frame.along.y * along + frame.left.y * across,
            elevation};
}

TopoDS_Shape opening_box(const OpeningFrame& frame, double along, double across,
                         double length, double depth, double height, double elevation,
                         const char* message) {
    if (!std::isfinite(length) || length <= tolerance || !std::isfinite(depth) ||
        depth <= tolerance || !std::isfinite(height) || height <= tolerance) {
        throw std::invalid_argument(message);
    }
    try {
        const auto origin = opening_point(frame, along, across, elevation);
        const gp_Ax2 axes(origin, gp_Dir(0.0, 0.0, 1.0),
                          gp_Dir(frame.along.x, frame.along.y, 0.0));
        BRepPrimAPI_MakeBox builder(axes, length, depth, height);
        builder.Build();
        if (!builder.IsDone() || builder.Shape().IsNull() ||
            !BRepCheck_Analyzer(builder.Shape()).IsValid()) {
            throw std::invalid_argument(message);
        }
        return builder.Shape();
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string(message) + ": " + error.what());
    }
}

TopoDS_Shape rotate_opening_part(const TopoDS_Shape& source, const gp_Pnt& hinge,
                                 double angle, const char* message) {
    if (source.IsNull() || !std::isfinite(angle)) throw std::invalid_argument(message);
    try {
        gp_Trsf transform;
        transform.SetRotation(gp_Ax1(hinge, gp_Dir(0.0, 0.0, 1.0)), angle);
        BRepBuilderAPI_Transform rotated(source, transform, true);
        if (!rotated.IsDone() || rotated.Shape().IsNull() ||
            !BRepCheck_Analyzer(rotated.Shape()).IsValid()) {
            throw std::invalid_argument(message);
        }
        return rotated.Shape();
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string(message) + ": " + error.what());
    }
}

TopoDS_Wire wire(const Boundary& boundary, double elevation) {
    if (!validate_boundary(boundary).empty()) throw std::invalid_argument("Invalid solid profile boundary");
    auto oriented = boundary;
    if (signed_area(oriented) < 0) {
        std::reverse(oriented.begin(), oriented.end());
        for (auto& edge : oriented) {
            std::swap(edge.start, edge.end);
            edge.sweep_radians = -edge.sweep_radians;
        }
    }
    BRepBuilderAPI_MakeWire builder;
    for (const auto& segment : oriented) {
        const gp_Pnt start(segment.start.x, segment.start.y, elevation);
        const gp_Pnt end(segment.end.x, segment.end.y, elevation);
        if (segment.sweep_radians == 0) {
            builder.Add(BRepBuilderAPI_MakeEdge(start, end));
        } else {
            const auto halfway = point_at(segment, 0.5);
            GC_MakeArcOfCircle arc(start, gp_Pnt(halfway.x, halfway.y, elevation), end);
            if (!arc.IsDone()) throw std::invalid_argument("Arc profile construction failed");
            builder.Add(BRepBuilderAPI_MakeEdge(arc.Value()));
        }
    }
    if (!builder.IsDone()) throw std::invalid_argument("Profile wire could not be constructed");
    return builder.Wire();
}

TopoDS_Shape extrude(const Boundary& boundary, double elevation, double height) {
    BRepPrimAPI_MakePrism prism(make_planar_face(boundary, elevation), gp_Vec(0, 0, height), true);
    prism.Build();
    if (!prism.IsDone() || !BRepCheck_Analyzer(prism.Shape()).IsValid()) {
        throw std::invalid_argument("Profile extrusion did not produce a valid solid");
    }
    return prism.Shape();
}

TopoDS_Shape extrude_polygon(const std::vector<gp_Pnt>& points,
                             const gp_Vec& direction,
                             const char* what) {
    if (points.size() < 3) throw std::invalid_argument(what);
    BRepBuilderAPI_MakePolygon polygon;
    for (const auto& point : points) {
        if (!std::isfinite(point.X()) || !std::isfinite(point.Y()) ||
            !std::isfinite(point.Z())) {
            throw std::invalid_argument(what);
        }
        polygon.Add(point);
    }
    polygon.Close();
    if (!polygon.IsDone()) throw std::invalid_argument(what);
    BRepBuilderAPI_MakeFace face(polygon.Wire(), true);
    if (!face.IsDone()) throw std::invalid_argument(what);
    BRepPrimAPI_MakePrism prism(face.Face(), direction, true);
    prism.Build();
    if (!prism.IsDone() || prism.Shape().IsNull() ||
        !BRepCheck_Analyzer(prism.Shape()).IsValid()) {
        throw std::invalid_argument(what);
    }
    return prism.Shape();
}

Boundary strip_range(const Segment& baseline, double inner_offset, double outer_offset) {
    if (!std::isfinite(inner_offset) || !std::isfinite(outer_offset) ||
        !(outer_offset > inner_offset + tolerance)) {
        throw std::invalid_argument("Wall layer offsets must form a positive range");
    }
    if (baseline.sweep_radians == 0) {
        const double length = segment_length(baseline);
        const double normal_x = -(baseline.end.y - baseline.start.y) / length;
        const double normal_y = (baseline.end.x - baseline.start.x) / length;
        const Vec2 a{baseline.start.x + normal_x * outer_offset,
                     baseline.start.y + normal_y * outer_offset};
        const Vec2 b{baseline.end.x + normal_x * outer_offset,
                     baseline.end.y + normal_y * outer_offset};
        const Vec2 c{baseline.end.x + normal_x * inner_offset,
                     baseline.end.y + normal_y * inner_offset};
        const Vec2 d{baseline.start.x + normal_x * inner_offset,
                     baseline.start.y + normal_y * inner_offset};
        return {{a, b, 0}, {b, c, 0}, {c, d, 0}, {d, a, 0}};
    }
    const auto origin = centre(baseline);
    const double radius = std::hypot(baseline.start.x - origin.x, baseline.start.y - origin.y);
    if (radius + inner_offset <= tolerance) throw std::invalid_argument("Wall layer crosses its arc centre");
    const auto radial = [&](Vec2 point, double offset) -> Vec2 {
        const double scale = (radius + offset) / radius;
        return {origin.x + (point.x - origin.x) * scale, origin.y + (point.y - origin.y) * scale};
    };
    const auto a = radial(baseline.start, outer_offset);
    const auto b = radial(baseline.end, outer_offset);
    const auto c = radial(baseline.end, inner_offset);
    const auto d = radial(baseline.start, inner_offset);
    return {{a, b, baseline.sweep_radians}, {b, c, 0},
            {c, d, -baseline.sweep_radians}, {d, a, 0}};
}

Boundary strip(const Segment& baseline, double thickness) {
    return strip_range(baseline, -thickness * 0.5, thickness * 0.5);
}

TopoDS_Shape sloped_layer(const Segment& baseline, double inner_offset,
                          double outer_offset, double elevation, double start_height,
                          double rise) {
    if (baseline.sweep_radians != 0.0 && std::abs(rise) > tolerance) {
        throw std::invalid_argument("Sloped wall layers require a straight baseline");
    }
    if (std::abs(rise) <= tolerance) {
        return extrude(strip_range(baseline, inner_offset, outer_offset),
                       elevation, start_height);
    }
    const double length = segment_length(baseline);
    const double dx = (baseline.end.x - baseline.start.x) / length;
    const double dy = (baseline.end.y - baseline.start.y) / length;
    const Vec2 normal{-dy, dx};
    const auto offset_point = [&](Vec2 point, double offset) {
        return Vec2{point.x + normal.x * offset, point.y + normal.y * offset};
    };
    const auto inner_start = offset_point(baseline.start, inner_offset);
    const auto inner_end = offset_point(baseline.end, inner_offset);
    const auto base_height = std::min(start_height, start_height + rise);
    const auto end_height = start_height + rise;
    if (!std::isfinite(base_height) || base_height <= tolerance ||
        !std::isfinite(end_height) || end_height <= tolerance) {
        throw std::invalid_argument("Sloped wall heights must remain positive");
    }

    // The lower prism carries the common vertical height. A triangular prism
    // adds the signed rise at the high end, keeping the bottom face level.
    auto result = extrude(strip_range(baseline, inner_offset, outer_offset),
                          elevation, base_height);
    std::vector<gp_Pnt> profile;
    if (rise > 0.0) {
        profile = {
            {inner_start.x, inner_start.y, elevation + base_height},
            {inner_end.x, inner_end.y, elevation + base_height},
            {inner_end.x, inner_end.y, elevation + end_height},
        };
    } else {
        profile = {
            {inner_start.x, inner_start.y, elevation + base_height},
            {inner_end.x, inner_end.y, elevation + base_height},
            {inner_start.x, inner_start.y, elevation + start_height},
        };
    }
    const auto across = gp_Vec(normal.x * (outer_offset - inner_offset),
                               normal.y * (outer_offset - inner_offset), 0.0);
    const auto wedge = extrude_polygon(profile, across, "Sloped wall wedge construction failed");
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, result);
    builder.Add(compound, wedge);
    if (!BRepCheck_Analyzer(compound).IsValid()) {
        throw std::invalid_argument("Sloped wall layer is invalid");
    }
    return compound;
}

TopoDS_Shape cut(const TopoDS_Shape& base, const TopoDS_Shape& tool) {
    BRepAlgoAPI_Cut operation(base, tool);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors() || operation.Shape().IsNull()
            || !BRepCheck_Analyzer(operation.Shape()).IsValid()) {
        throw std::invalid_argument("Architectural opening could not be cut reliably");
    }
    return operation.Shape();
}

double common_volume(const TopoDS_Shape& a, const TopoDS_Shape& b) {
    BRepAlgoAPI_Common operation(a, b);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors()) throw std::invalid_argument("Solid containment is unresolved");
    return solid_volume(operation.Shape());
}

double endpoint_distance(const Vec2& first, const Vec2& second) {
    const auto distance = std::hypot(first.x - second.x, first.y - second.y);
    if (!std::isfinite(distance)) {
        throw std::invalid_argument("Wall join endpoint distance exceeds the supported range");
    }
    return distance;
}

TopoDS_Shape fuse_wall_shapes(const TopoDS_Shape& first, const TopoDS_Shape& second) {
    BRepAlgoAPI_Fuse operation(first, second);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors() || operation.Shape().IsNull() ||
        !BRepCheck_Analyzer(operation.Shape()).IsValid()) {
        throw std::invalid_argument("Wall join boolean union did not produce a valid solid");
    }
    return operation.Shape();
}

bool shapes_touch(const TopoDS_Shape& first, const TopoDS_Shape& second) {
    BRepExtrema_DistShapeShape distance(first, second);
    if (!distance.IsDone() || !std::isfinite(distance.Value())) {
        throw std::invalid_argument("Architectural join shape connectivity is unresolved");
    }
    return distance.Value() <= tolerance;
}

TopoDS_Shape fuse_shapes(const TopoDS_Shape& first, const TopoDS_Shape& second,
                         const char* context) {
    BRepAlgoAPI_Fuse operation(first, second);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors() || operation.Shape().IsNull() ||
        !BRepCheck_Analyzer(operation.Shape()).IsValid()) {
        throw std::invalid_argument(context);
    }
    return operation.Shape();
}
}

double solid_volume(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0;
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    if (!std::isfinite(properties.Mass())) throw std::invalid_argument("Solid volume exceeds the supported numeric range");
    return std::abs(properties.Mass());
}

TopoDS_Face make_planar_face(const Boundary& boundary, double elevation) {
    if (!std::isfinite(elevation)) throw std::invalid_argument("Profile elevation must be finite");
    try {
        BRepBuilderAPI_MakeFace face(wire(boundary, elevation), true);
        if (!face.IsDone() || !BRepCheck_Analyzer(face.Face()).IsValid()) {
            throw std::invalid_argument("Profile does not form a valid planar face");
        }
        return face.Face();
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Planar profile failed: ") + error.what());
    }
}

double surface_area(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0;
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(shape, properties);
    if (!std::isfinite(properties.Mass())) throw std::invalid_argument("Surface area exceeds the supported numeric range");
    return std::abs(properties.Mass());
}

std::string_view slab_element_kind_name(SlabElementKind kind) noexcept {
    switch (kind) {
    case SlabElementKind::slab: return "slab";
    case SlabElementKind::floor: return "floor";
    case SlabElementKind::ceiling: return "ceiling";
    case SlabElementKind::foundation: return "foundation";
    }
    return "invalid";
}

std::optional<SlabElementKind> parse_slab_element_kind(std::string_view value) noexcept {
    if (value == "slab") return SlabElementKind::slab;
    if (value == "floor") return SlabElementKind::floor;
    if (value == "ceiling") return SlabElementKind::ceiling;
    if (value == "foundation") return SlabElementKind::foundation;
    return std::nullopt;
}

TopoDS_Shape make_wall(const Wall& wall) {
    validate_wall_semantics(wall);
    const double length = segment_length(wall.baseline);
    try {
        const auto cut_openings = [&](TopoDS_Shape result) {
            for (const auto& opening : wall.openings) {
                const double from = opening.offset / length;
                const double to = (opening.offset + opening.width) / length;
                const Segment interval{point_at(wall.baseline, from), point_at(wall.baseline, to),
                                       wall.baseline.sweep_radians * (to - from)};
                // Cut through the full wall stack so every layer keeps the
                // same host-opening relationship.
                const auto tool = extrude(
                    strip_range(interval, -wall.thickness * 0.5,
                                wall.thickness * 0.5),
                    wall.elevation + opening.sill, opening.height);
                result = cut(result, tool);
            }
            if (solid_volume(result) <= tolerance * tolerance * tolerance) {
                throw std::invalid_argument("Openings remove the entire wall");
            }
            return result;
        };

        const auto rise = wall.slope_rise.value_or(0.0);
        if (wall.layers.empty()) {
            return cut_openings(sloped_layer(wall.baseline, -wall.thickness * 0.5,
                                             wall.thickness * 0.5, wall.elevation,
                                             wall.height, rise));
        }

        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        double inner_offset = -wall.thickness * 0.5;
        for (const auto& layer : wall.layers) {
            const auto outer_offset = inner_offset + layer.thickness;
            auto layer_shape = sloped_layer(wall.baseline, inner_offset, outer_offset,
                                            wall.elevation, wall.height, rise);
            layer_shape = cut_openings(std::move(layer_shape));
            builder.Add(compound, layer_shape);
            inner_offset = outer_offset;
        }
        if (compound.IsNull() || !BRepCheck_Analyzer(compound).IsValid() ||
            solid_volume(compound) <= tolerance * tolerance * tolerance) {
            throw std::invalid_argument("Composite wall did not produce valid solids");
        }
        return compound;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Wall geometry failed: ") + error.what());
    }
}

TopoDS_Shape make_opening_assembly(const Wall& wall, const HostedOpening& opening,
                                   const OpeningAssembly& assembly,
                                   const std::optional<DoorOperation>& door_operation) {
    validate_opening_assembly(assembly);
    Wall checked = wall;
    const auto existing = std::find_if(checked.openings.begin(), checked.openings.end(),
                                       [&](const HostedOpening& candidate) {
                                           return candidate.id == opening.id;
                                       });
    if (existing == checked.openings.end()) {
        checked.openings.push_back(opening);
    } else if (*existing != opening) {
        throw std::invalid_argument("Opening assembly host contains a different opening with the same ID");
    }
    validate_wall_semantics(checked);

    const auto wall_length = segment_length(wall.baseline);
    if (!std::isfinite(wall_length) || wall_length <= tolerance ||
        opening.offset < -tolerance || opening.offset + opening.width > wall_length + tolerance) {
        throw std::invalid_argument("Opening assembly dimensions do not fit the host wall");
    }
    if (assembly.frame_depth_m > wall.thickness + tolerance ||
        std::abs(assembly.inset_m) + assembly.frame_depth_m * 0.5 >
            wall.thickness * 0.5 + tolerance) {
        throw std::invalid_argument("Opening assembly frame does not fit the wall thickness");
    }
    if (assembly.frame_width_m * 2.0 >= opening.width - tolerance) {
        throw std::invalid_argument("Opening assembly frame leaves no clear opening width");
    }
    const bool window = assembly.kind == OpeningAssemblyKind::window;
    const double clear_height = opening.height - (window ? 2.0 : 1.0) * assembly.frame_width_m;
    if (!std::isfinite(clear_height) || clear_height <= tolerance) {
        throw std::invalid_argument("Opening assembly frame leaves no clear opening height");
    }
    if (window && assembly.frame_width_m * 2.0 >= opening.height - tolerance) {
        throw std::invalid_argument("Window assembly frame leaves no clear opening height");
    }

    const auto frame = opening_frame(wall.baseline, opening.offset / wall_length);
    const double base_elevation = wall.elevation + opening.sill;
    const double frame_across = assembly.inset_m - assembly.frame_depth_m * 0.5;
    const double panel_across = assembly.inset_m - assembly.panel_thickness_m * 0.5;
    const double frame_width = assembly.frame_width_m;
    const double clear_width = opening.width - 2.0 * frame_width;

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const auto add = [&](const TopoDS_Shape& part) {
        if (part.IsNull()) throw std::invalid_argument("Opening assembly contains an empty part");
        builder.Add(compound, part);
    };
    add(opening_box(frame, 0.0, frame_across, frame_width,
                   assembly.frame_depth_m, opening.height, base_elevation,
                   "Opening assembly jamb construction failed"));
    add(opening_box(frame, opening.width - frame_width, frame_across, frame_width,
                   assembly.frame_depth_m, opening.height, base_elevation,
                   "Opening assembly jamb construction failed"));
    add(opening_box(frame, frame_width, frame_across, clear_width,
                   assembly.frame_depth_m, frame_width,
                   base_elevation + opening.height - frame_width,
                   "Opening assembly head construction failed"));
    if (window) {
        add(opening_box(frame, frame_width, frame_across, clear_width,
                       assembly.frame_depth_m, frame_width, base_elevation,
                       "Window assembly sill construction failed"));
    }

    const double panel_depth = assembly.panel_thickness_m;
    const auto panel = [&](double along, double across, double length, double height) {
        return opening_box(frame, along, across, length, panel_depth, height, base_elevation,
                           "Opening assembly panel construction failed");
    };
    if (!window) {
        auto leaf = panel(frame_width, panel_across, clear_width, clear_height);
        std::optional<gp_Pnt> hinge;
        double swing_angle = 0.0;
        if (door_operation.has_value()) {
            // Reuse the analytical operation's validation so a native solid
            // can never silently accept a different handing contract.
            (void)door_plan_symbol(wall.baseline, opening.offset, opening.width,
                                   *door_operation);
            const double hinge_along = door_operation->hinge_at_end
                                           ? opening.width - frame_width
                                           : frame_width;
            hinge = opening_point(frame, hinge_along, panel_across + panel_depth * 0.5,
                                  base_elevation);
            swing_angle = door_operation->angle_degrees * std::numbers::pi / 180.0 *
                          (door_operation->swing_left ? 1.0 : -1.0) *
                          (door_operation->hinge_at_end ? -1.0 : 1.0);
            leaf = rotate_opening_part(leaf, *hinge, swing_angle,
                                       "Opening assembly leaf rotation failed");
        }
        add(leaf);
        if (assembly.glazing_thickness_m > tolerance) {
            const double glazing_width = clear_width * 0.65;
            const double glazing_height = clear_height * 0.65;
            auto glazing = opening_box(frame,
                                       frame_width + clear_width * 0.175,
                                       assembly.inset_m - assembly.glazing_thickness_m * 0.5,
                                       glazing_width, assembly.glazing_thickness_m,
                                       glazing_height,
                                       base_elevation + clear_height * 0.175,
                                       "Door assembly glazing construction failed");
            if (hinge.has_value()) {
                glazing = rotate_opening_part(glazing, *hinge, swing_angle,
                                              "Door assembly glazing rotation failed");
            }
            add(glazing);
        }
    } else {
        const double sash_bar = std::min(frame_width * 0.6, clear_width * 0.2);
        if (sash_bar <= tolerance || clear_height - 2.0 * sash_bar <= tolerance) {
            throw std::invalid_argument("Window assembly leaves no clear glazing pane");
        }
        add(opening_box(frame, frame_width, panel_across, sash_bar, panel_depth,
                        clear_height, base_elevation + frame_width,
                        "Window assembly sash construction failed"));
        add(opening_box(frame, opening.width - frame_width - sash_bar, panel_across,
                        sash_bar, panel_depth, clear_height, base_elevation + frame_width,
                        "Window assembly sash construction failed"));
        add(opening_box(frame, frame_width + sash_bar, panel_across,
                        clear_width - 2.0 * sash_bar, panel_depth, sash_bar,
                        base_elevation + frame_width,
                        "Window assembly sash construction failed"));
        add(opening_box(frame, frame_width + sash_bar, panel_across,
                        clear_width - 2.0 * sash_bar, panel_depth, sash_bar,
                        base_elevation + opening.height - frame_width - sash_bar,
                        "Window assembly sash construction failed"));
        add(opening_box(frame, frame_width + sash_bar,
                        assembly.inset_m - assembly.glazing_thickness_m * 0.5,
                        clear_width - 2.0 * sash_bar, assembly.glazing_thickness_m,
                        clear_height - 2.0 * sash_bar,
                        base_elevation + frame_width + sash_bar,
                        "Window assembly glazing construction failed"));
    }

    if (compound.IsNull() || !BRepCheck_Analyzer(compound).IsValid() ||
        solid_volume(compound) <= tolerance * tolerance * tolerance) {
        throw std::invalid_argument("Opening assembly did not produce valid solids");
    }
    return compound;
}

TopoDS_Shape make_wall_join(const WallJoin& join, std::span<const Wall> walls) {
    validate_wall_join_semantics(join);
    if (walls.size() != join.wall_ids.size()) {
        throw std::invalid_argument("Wall join source wall count does not match wall_ids");
    }

    std::map<std::string_view, const Wall*> available;
    for (const auto& wall : walls) {
        if (!available.emplace(wall.id, &wall).second) {
            throw std::invalid_argument("Wall join source wall IDs must be unique");
        }
    }
    std::vector<const Wall*> resolved;
    resolved.reserve(join.wall_ids.size());
    for (const auto& wall_id : join.wall_ids) {
        const auto found = available.find(wall_id);
        if (found == available.end()) {
            throw std::invalid_argument("Wall join source wall is missing: " + wall_id);
        }
        resolved.push_back(found->second);
    }

    std::vector<TopoDS_Shape> wall_shapes;
    wall_shapes.reserve(resolved.size());
    for (const auto* wall : resolved) {
        validate_wall_semantics(*wall);
        wall_shapes.push_back(make_wall(*wall));
    }
    const bool connected = members_form_connected_component(
        resolved.size(), [&](std::size_t first, std::size_t second) {
            const auto& left = *resolved[first];
            const auto& right = *resolved[second];
            const std::array<Vec2, 2> left_endpoints{left.baseline.start, left.baseline.end};
            const std::array<Vec2, 2> right_endpoints{right.baseline.start, right.baseline.end};
            bool touches = false;
            for (const auto& left_endpoint : left_endpoints) {
                for (const auto& right_endpoint : right_endpoints) {
                    if (endpoint_distance(left_endpoint, right_endpoint) <= tolerance) {
                        touches = true;
                    }
                }
            }
            return touches && shapes_touch(wall_shapes[first], wall_shapes[second]);
        });
    if (!connected) {
        throw std::invalid_argument("Wall join walls must share connected endpoints");
    }

    try {
        auto result = wall_shapes.front();
        for (std::size_t index = 1; index < resolved.size(); ++index) {
            result = fuse_wall_shapes(result, wall_shapes[index]);
        }
        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid() ||
            solid_volume(result) <= tolerance * tolerance * tolerance) {
            throw std::invalid_argument("Wall join produced an empty solid");
        }
        return result;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Wall join geometry failed: ") + error.what());
    }
}

TopoDS_Shape make_roof_join(const RoofJoin& join, std::span<const TopoDS_Shape> roofs) {
    validate_roof_join_semantics(join);
    if (roofs.size() != join.roof_ids.size()) {
        throw std::invalid_argument("Roof join source roof count does not match roof_ids");
    }
    for (const auto& roof : roofs) {
        if (roof.IsNull() || !BRepCheck_Analyzer(roof).IsValid() ||
            !std::isfinite(solid_volume(roof)) ||
            solid_volume(roof) <= tolerance * tolerance * tolerance) {
            throw std::invalid_argument("Roof join source roof is not a valid solid");
        }
    }

    const bool connected = members_form_connected_component(
        roofs.size(), [&](std::size_t first, std::size_t second) {
            return shapes_touch(roofs[first], roofs[second]);
        });
    if (!connected) {
        throw std::invalid_argument("Roof join roofs must form one connected component");
    }

    try {
        auto result = roofs.front();
        for (std::size_t index = 1; index < roofs.size(); ++index) {
            result = fuse_shapes(result, roofs[index],
                                 "Roof join boolean union did not produce a valid solid");
        }
        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid() ||
            !std::isfinite(solid_volume(result)) ||
            solid_volume(result) <= tolerance * tolerance * tolerance) {
            throw std::invalid_argument("Roof join produced an empty solid");
        }
        return result;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Roof join geometry failed: ") + error.what());
    }
}

TopoDS_Shape make_slab(const Slab& slab) {
    if (slab_element_kind_name(slab.element_kind) == "invalid") {
        throw std::invalid_argument("Slab element kind is invalid");
    }
    positive(slab.thickness, "Slab thickness must be positive");
    if (!std::isfinite(slab.elevation)) throw std::invalid_argument("Slab elevation must be finite");
    try {
        validate_slab_layers(slab.layers, slab.thickness);
        const auto make_layer = [&](double elevation, double thickness) {
            auto result = extrude(slab.boundary, elevation, thickness);
            const auto original = result;
            const auto outer_wire = wire(slab.boundary, elevation);
            std::vector<TopoDS_Shape> previous;
            std::vector<TopoDS_Wire> previous_wires;
            for (const auto& hole : slab.holes) {
                auto tool = extrude(hole, elevation, thickness);
                auto hole_wire = wire(hole, elevation);
                const double volume = solid_volume(tool);
                if (std::abs(common_volume(original, tool) - volume) > std::max(1e-10, volume * 1e-9)) {
                    throw std::invalid_argument("Slab opening is outside its boundary");
                }
                BRepExtrema_DistShapeShape outer_distance(outer_wire, hole_wire);
                if (!outer_distance.IsDone() || outer_distance.Value() <= tolerance) {
                    throw std::invalid_argument("Slab opening touches its outer boundary");
                }
                for (std::size_t i = 0; i < previous.size(); ++i) {
                    BRepExtrema_DistShapeShape separation(previous_wires[i], hole_wire);
                    if (common_volume(previous[i], tool) > 1e-10 || !separation.IsDone() || separation.Value() <= tolerance) {
                        throw std::invalid_argument("Slab openings overlap or touch");
                    }
                }
                result = cut(result, tool);
                previous.push_back(tool);
                previous_wires.push_back(hole_wire);
            }
            return result;
        };
        if (slab.layers.empty()) return make_layer(slab.elevation, slab.thickness);

        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        double layer_elevation = slab.elevation;
        for (const auto& layer : slab.layers) {
            if (!std::isfinite(layer_elevation)) {
                throw std::invalid_argument("Slab layer elevation exceeds the supported numeric range");
            }
            builder.Add(compound, make_layer(layer_elevation, layer.thickness));
            layer_elevation += layer.thickness;
        }
        if (!std::isfinite(layer_elevation) || !BRepCheck_Analyzer(compound).IsValid() ||
            solid_volume(compound) <= tolerance * tolerance * tolerance) {
            throw std::invalid_argument("Composite slab did not produce valid solids");
        }
        return compound;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Slab geometry failed: ") + error.what());
    }
}

TopoDS_Shape make_room_volume(const RoomVolume& room) {
    // Reuse the slab boolean/validation path so room footprints, analytical
    // arcs, holes, tolerances, and exact volume accounting cannot drift from
    // the horizontal-assembly implementation.  The semantic room type stays
    // distinct at the document boundary; only the derived solid is shared.
    if (!std::isfinite(room.height) || room.height <= tolerance) {
        throw std::invalid_argument("Room height must be positive");
    }
    return make_slab(Slab{room.id, room.boundary, room.holes, room.height,
                          room.elevation, SlabElementKind::slab, {}});
}

TopoDS_Shape make_terrain_surface(const TerrainSurface& surface) {
    try {
        const auto& points = surface.points();
        const auto& triangles = surface.triangles();
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const auto& triangle : triangles) {
            const auto& first = points[triangle.point_indices[0]];
            const auto& second = points[triangle.point_indices[1]];
            const auto& third = points[triangle.point_indices[2]];
            BRepBuilderAPI_MakePolygon polygon;
            polygon.Add(gp_Pnt(first.x_m, first.y_m, first.elevation_m));
            polygon.Add(gp_Pnt(second.x_m, second.y_m, second.elevation_m));
            polygon.Add(gp_Pnt(third.x_m, third.y_m, third.elevation_m));
            polygon.Close();
            if (!polygon.IsDone()) {
                throw std::invalid_argument("Terrain triangle wire construction failed");
            }
            BRepBuilderAPI_MakeFace face(polygon.Wire(), true);
            if (!face.IsDone() || !BRepCheck_Analyzer(face.Face()).IsValid()) {
                throw std::invalid_argument("Terrain triangle face construction failed");
            }
            builder.Add(compound, face.Face());
        }
        if (compound.IsNull() || !BRepCheck_Analyzer(compound).IsValid()) {
            throw std::invalid_argument("Terrain surface compound is invalid");
        }
        return compound;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Terrain surface geometry failed: ") + error.what());
    }
}
} // namespace sketch
