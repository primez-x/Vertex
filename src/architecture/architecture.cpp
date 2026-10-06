#include "sketch/architecture.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/wall_plan_junctions.hpp"

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
#include <TopExp_Explorer.hxx>
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
    // Offsets always follow the directed baseline's left normal. For a
    // counterclockwise arc that normal points inward; for clockwise it is outward.
    const double direction = baseline.sweep_radians > 0.0 ? 1.0 : -1.0;
    const double first_radius = radius - direction * inner_offset;
    const double last_radius = radius - direction * outer_offset;
    const double minimum_radius = std::min(first_radius, last_radius);
    const double maximum_radius = std::max(first_radius, last_radius);
    if (minimum_radius <= tolerance || !std::isfinite(maximum_radius)) {
        throw std::invalid_argument("Wall layer crosses its arc centre");
    }
    const auto radial = [&](Vec2 point, double offset) -> Vec2 {
        const double scale = (radius - direction * offset) / radius;
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

TopoDS_Shape bay_window_parts(const Wall& host, const HostedOpening& opening,
                             const OpeningAssembly& assembly, const OpeningFrame& frame) {
    if (host.baseline.sweep_radians != 0.0)
        throw std::invalid_argument("Bay windows require a straight host until curved fitting is available");
    const double width = opening.width, bar = assembly.frame_width_m;
    const double depth = assembly.frame_depth_m, projection = assembly.window_bay_projection_m;
    const double height = opening.height - 2.0 * bar;
    const double base = host.elevation + opening.sill;
    const double side = assembly.window_open_left ? 1.0 : -1.0;
    // Coordinates below use an outward-positive normal, regardless of handing.
    // Projection starts at the physical wall face; inset locates the mounting
    // frame and cannot silently shorten or deepen the requested projecting bay.
    const double face = host.thickness * 0.5;
    const double rear = side * assembly.inset_m - depth * 0.5;
    const double mount_face = side * assembly.inset_m + depth * 0.5;
    const double front_start = width * (1.0 - assembly.window_bay_front_fraction) * 0.5;
    const double run = front_start - bar;
    const double side_length = std::hypot(run, projection);
    if (run <= tolerance || projection <= depth + tolerance || height <= 2.0 * bar + tolerance)
        throw std::invalid_argument("Bay projection and facets leave no framed glazing cavity");
    const double rear_inner = bar + depth * side_length / projection;
    const double front_inner = front_start + depth * (side_length - run) / projection;
    if (front_inner <= rear_inner + tolerance || width - 2.0 * front_inner <= 2.0 * bar + tolerance)
        throw std::invalid_argument("Bay mitered frame does not fit the projecting facets");
    const std::array<Vec2, 4> outer{{{bar, face}, {front_start, face + projection},
                                  {width - front_start, face + projection}, {width - bar, face}}};
    const std::array<Vec2, 4> inner{{{rear_inner, face}, {front_inner, face + projection - depth},
                                  {width - front_inner, face + projection - depth}, {width - rear_inner, face}}};
    const auto prism = [&](const std::vector<Vec2>& polygon, double z, double rise) {
        std::vector<gp_Pnt> points;
        for (const auto& p : polygon) points.push_back(opening_point(frame, p.x, side * p.y, z));
        return extrude_polygon(points, gp_Vec(0, 0, rise), "Bay frame profile extrusion failed");
    };
    TopoDS_Compound result;
    BRep_Builder builder; builder.MakeCompound(result);
    std::vector<TopoDS_Shape> parts;
    const auto add = [&](const TopoDS_Shape& part) { parts.push_back(part); builder.Add(result, part); };
    const double mount_across = assembly.inset_m - depth * 0.5;
    for (double x : {0.0, width - bar})
        add(opening_box(frame, x, mount_across, bar, depth, opening.height, base,
                        "Bay mounting jamb construction failed"));
    // Recessed mounting jambs need real returns to the host face. Their width
    // stays in the wall mouth, so they cannot clip material beside the opening.
    const double return_depth = face - mount_face;
    if (return_depth > tolerance) for (double x : {0.0, width - bar})
        add(opening_box(frame, x, side > 0.0 ? mount_face : -face, bar, return_depth,
                        opening.height, base, "Bay mounting return construction failed"));
    const std::vector<Vec2> plate{{bar, rear}, {width - bar, rear}, outer[3], outer[2], outer[1], outer[0]};
    add(prism(plate, base, bar));
    add(prism(plate, base + opening.height - bar, bar));
    for (std::size_t index = 0; index < 3; ++index) {
        const auto& a = outer[index]; const auto& b = outer[index + 1];
        const double length = std::hypot(b.x - a.x, b.y - a.y);
        const Vec2 along{(b.x - a.x) / length, (b.y - a.y) / length};
        const auto station = [&](const Vec2& point) {
            return (point.x - a.x) * along.x + (point.y - a.y) * along.y;
        };
        // Miter joins shorten the inner edge. Apertures are inset from both
        // inner and outer endpoints, leaving actual structural join material.
        const double first = std::max(0.0, station(inner[index])) + bar;
        const double last = std::min(length, station(inner[index + 1])) - bar;
        const double glass_width = last - first;
        const double glass_height = height - 2.0 * bar;
        if (glass_width <= 4.0 * tolerance || glass_height <= 4.0 * tolerance)
            throw std::invalid_argument("Bay facet leaves no clear glazing pane");
        const auto global_a = opening_point(frame, a.x, side * a.y, base);
        const auto global_b = opening_point(frame, b.x, side * b.y, base);
        const Vec2 direction{(global_b.X() - global_a.X()) / length,
                             (global_b.Y() - global_a.Y()) / length};
        const OpeningFrame facet{{global_a.X(), global_a.Y()}, direction, {-direction.y, direction.x}};
        const double inward_across = side > 0.0 ? -depth : 0.0;
        const auto aperture = opening_box(facet, first, inward_across - tolerance,
            glass_width, depth + 2.0 * tolerance, glass_height, base + 2.0 * bar,
            "Bay glazing aperture construction failed");
        add(cut(prism({a, b, inner[index + 1], inner[index]}, base + bar, height), aperture));
        // A finite construction clearance keeps pane material out of frame
        // and miter joints, including at the top and bottom aperture edges.
        add(opening_box(facet, first + 2.0 * tolerance,
            inward_across + (depth - assembly.glazing_thickness_m) * 0.5,
            glass_width - 4.0 * tolerance, assembly.glazing_thickness_m,
            glass_height - 4.0 * tolerance, base + 2.0 * bar + 2.0 * tolerance,
            "Bay glazing construction failed"));
    }
    // Each angled facet's rear cap is wider than the jamb. Without these
    // opaque butt adapters its connection to the mounting frame would be
    // only a vertical edge. The adapters share a full rear-cap mating face
    // with the facet and a finite side face with the jamb and any return.
    // Their clear-height span butts against the plates without shared volume.
    for (double x : {bar, width - rear_inner})
        add(opening_box(frame, x, side > 0.0 ? rear : -face,
            rear_inner - bar, face - rear, height, base + bar,
            "Bay mounting butt adapter construction failed"));
    const auto wall_shape = make_wall(host);
    const double overlap_limit = tolerance * tolerance * std::max(1.0, opening.height);
    for (std::size_t first = 0; first < parts.size(); ++first) {
        if (common_volume(parts[first], wall_shape) > overlap_limit)
            throw std::invalid_argument("Bay assembly intersects its host wall");
        for (std::size_t second = first + 1; second < parts.size(); ++second)
            if (common_volume(parts[first], parts[second]) > overlap_limit)
                throw std::invalid_argument("Bay facet or mounting parts share material");
    }
    if (!BRepCheck_Analyzer(result).IsValid() || solid_volume(result) <= tolerance * tolerance * tolerance)
        throw std::invalid_argument("Bay assembly did not produce valid material solids");
    return result;
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

OpeningAssemblyGeometry make_opening_assembly_geometry(const Wall& wall, const HostedOpening& opening,
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
    if (door_operation) {
        (void)encode_door_operation(*door_operation);
        if (window) throw std::invalid_argument("Window assembly cannot carry a door operation");
    }
    const double clear_height = opening.height - (window ? 2.0 : 1.0) * assembly.frame_width_m;
    if (!std::isfinite(clear_height) || clear_height <= tolerance) {
        throw std::invalid_argument("Opening assembly frame leaves no clear opening height");
    }
    if (window && assembly.frame_width_m * 2.0 >= opening.height - tolerance) {
        throw std::invalid_argument("Window assembly frame leaves no clear opening height");
    }

    const auto frame = opening_frame(wall.baseline, opening.offset / wall_length);
    if (window && assembly.window_layout == WindowLayoutKind::bay) {
        try {
            return {bay_window_parts(checked, opening, assembly, frame), std::nullopt, {}};
        } catch (const Standard_Failure& error) {
            throw std::invalid_argument(std::string("Bay geometry failed: ") + error.what());
        }
    }
    const double base_elevation = wall.elevation + opening.sill;
    const double frame_across = assembly.inset_m - assembly.frame_depth_m * 0.5;
    const double panel_across = assembly.inset_m - assembly.panel_thickness_m * 0.5;
    const double frame_width = assembly.frame_width_m;
    const double clear_width = opening.width - 2.0 * frame_width;
    std::optional<Segment> door_swing;
    std::vector<Segment> door_swings;

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const auto add = [&](const TopoDS_Shape& part) {
        if (part.IsNull()) throw std::invalid_argument("Opening assembly contains an empty part");
        builder.Add(compound, part);
    };
    std::vector<TopoDS_Shape> frame_parts;
    const auto add_frame = [&](const TopoDS_Shape& part) {
        frame_parts.push_back(part);
        add(part);
    };
    const bool curved = wall.baseline.sweep_radians != 0.0;
    const bool sliding = door_operation && door_operation->kind == DoorOperationKind::sliding;
    const bool window_sliding = window && assembly.window_layout == WindowLayoutKind::sliding;
    const bool window_casement = window && assembly.window_layout == WindowLayoutKind::casement;
    if (curved && (window_sliding || window_casement))
        throw std::invalid_argument("Moving window mechanisms require a straight host");
    if (window_sliding) {
        const double track_depth = 2.0 * assembly.panel_thickness_m + 2.0 * tolerance;
        if (track_depth > assembly.frame_depth_m + tolerance ||
            std::abs(assembly.inset_m) + track_depth * 0.5 > wall.thickness * 0.5 + tolerance)
            throw std::invalid_argument("Sliding window tracks do not fit the frame and wall thickness");
    }
    if (sliding) {
        // Parallel straight tracks cannot follow an annular frame. Admit only
        // straight hosts until a fitted track profile is available.
        if (curved) throw std::invalid_argument("Sliding door tracks require a straight host");
        const double track_depth = 2.0 * assembly.panel_thickness_m + 2.0 * tolerance;
        if (track_depth > assembly.frame_depth_m + tolerance ||
            std::abs(assembly.inset_m) + track_depth * 0.5 > wall.thickness * 0.5 + tolerance)
            throw std::invalid_argument("Sliding door tracks do not fit the frame and wall thickness");
    }
    const auto fitted_part = [&](double along, double across, double length,
                                 double depth, double height, double elevation,
                                 const char* message) -> TopoDS_Shape {
        if (!curved) {
            return opening_box(frame, along, across, length, depth, height, elevation, message);
        }
        positive(length, message);
        positive(depth, message);
        positive(height, message);
        try {
            const auto span = hosted_opening_span(wall.baseline, opening.offset + along, length);
            return extrude(strip_range(span, across, across + depth), elevation, height);
        } catch (const Standard_Failure& error) {
            throw std::invalid_argument(std::string(message) + ": " + error.what());
        }
    };
    add_frame(fitted_part(0.0, frame_across, frame_width,
                   assembly.frame_depth_m, opening.height, base_elevation,
                   "Opening assembly jamb construction failed"));
    add_frame(fitted_part(opening.width - frame_width, frame_across, frame_width,
                   assembly.frame_depth_m, opening.height, base_elevation,
                   "Opening assembly jamb construction failed"));
    add_frame(fitted_part(frame_width, frame_across, clear_width,
                   assembly.frame_depth_m, frame_width,
                   base_elevation + opening.height - frame_width,
                   "Opening assembly head construction failed"));
    if (window) {
        add_frame(fitted_part(frame_width, frame_across, clear_width,
                       assembly.frame_depth_m, frame_width, base_elevation,
                       "Window assembly sill construction failed"));
    }

    const double panel_depth = assembly.panel_thickness_m;
    if (!window) {
        auto leaf_frame = frame;
        double leaf_start = frame_width;
        double leaf_width = clear_width;
        double leaf_across = panel_across;
        const bool doubled = door_operation && door_operation->kind == DoorOperationKind::double_hinged;
        const double swing_side = door_operation && !door_operation->swing_left ? -1.0 : 1.0;
        const double selected_frame_face = assembly.inset_m + swing_side * assembly.frame_depth_m * 0.5;
        const double selected_wall_face = swing_side * wall.thickness * 0.5;
        double double_hinge_across = (swing_side > 0.0 ? std::max(selected_frame_face, selected_wall_face)
                                                     : std::min(selected_frame_face, selected_wall_face)) + swing_side * 2.0 * tolerance;
        if (curved) {
            const auto origin = centre(wall.baseline);
            const double radius = std::hypot(wall.baseline.start.x - origin.x,
                                            wall.baseline.start.y - origin.y);
            const double direction = wall.baseline.sweep_radians > 0.0 ? 1.0 : -1.0;
            const double inset_radius = radius - direction * assembly.inset_m;
            const double half_angle = clear_width / (2.0 * radius);
            if (!std::isfinite(half_angle) || half_angle <= 0.0 ||
                half_angle >= std::numbers::pi * 0.5) {
                throw std::invalid_argument("Curved door clear span cannot support a planar leaf");
            }
            const double apothem = inset_radius * std::cos(half_angle);
            const double half_leaf = (apothem - panel_depth * 0.5) * std::tan(half_angle)
                                     - 2.0 * tolerance;
            if (!std::isfinite(half_leaf) || half_leaf <= tolerance ||
                apothem - panel_depth * 0.5 <
                    inset_radius - assembly.frame_depth_m * 0.5 - tolerance ||
                std::hypot(half_leaf, apothem + panel_depth * 0.5) >
                    inset_radius + assembly.frame_depth_m * 0.5 + tolerance) {
                throw std::invalid_argument("Curved door leaf does not fit its radial jambs and frame head");
            }
            // The finite-thickness rectangle ends short of the clear station
            // centres so its inward corners fit the radial jamb support planes.
            const auto midpoint = opening_frame(wall.baseline,
                (opening.offset + opening.width * 0.5) / wall_length);
            const double scale = apothem / radius;
            const Vec2 chord_midpoint{origin.x + (midpoint.origin.x - origin.x) * scale,
                                     origin.y + (midpoint.origin.y - origin.y) * scale};
            leaf_frame = {{chord_midpoint.x - midpoint.along.x * half_leaf,
                           chord_midpoint.y - midpoint.along.y * half_leaf},
                          midpoint.along, midpoint.left};
            leaf_start = 0.0;
            leaf_width = 2.0 * half_leaf;
            leaf_across = -panel_depth * 0.5;
            // Bound the annular frame and local wall faces in the chord's
            // signed normal. A pivot beyond both gives both jambs the same
            // physical clearance contract for either curve and swing side.
            const double opening_half_angle = opening.width / (2.0 * radius);
            const double end_cosine = std::cos(opening_half_angle);
            const bool inward = direction * swing_side > 0.0;
            const double projected_frame_radius = inward
                ? (inset_radius + (end_cosine >= 0.0 ? -1.0 : 1.0) * assembly.frame_depth_m * 0.5) * end_cosine
                : inset_radius + assembly.frame_depth_m * 0.5;
            const double projected_wall_radius = inward
                ? (radius + (end_cosine >= 0.0 ? -1.0 : 1.0) * wall.thickness * 0.5) * end_cosine
                : radius + wall.thickness * 0.5;
            const double projected_radius = inward ? std::min(projected_frame_radius, projected_wall_radius)
                                                   : std::max(projected_frame_radius, projected_wall_radius);
            double_hinge_across = direction * (apothem - projected_radius) + swing_side * 2.0 * tolerance;
        }
        const int leaf_count = doubled || sliding ? 2 : 1;
        if (doubled) {
            // Keep each complete rectangle in its own jamb half-plane. With
            // an offset pivot its inward reach includes the pivot offset as
            // well as half the panel thickness. Past 90 degrees the hinge
            // corners, rather than the far corners, determine that reach.
            // This admits only the requested static pose, not continuous
            // motion from the closed pose through all intermediate angles.
            const double angle = door_operation->angle_degrees * std::numbers::pi / 180.0;
            const double pivot_offset = std::abs(double_hinge_across - (leaf_across + panel_depth * 0.5));
            if (leaf_width * (1.0 - std::max(0.0, std::cos(angle))) + tolerance <
                (2.0 * pivot_offset + panel_depth) * std::sin(angle))
                throw std::invalid_argument("Double door leaves cross their meeting clearance at the requested angle");
        }
        std::vector<TopoDS_Shape> posed_leaf_envelopes;
        // Use the admitted cut host, including material layers and slope, so a
        // recessed frame never permits its leaf to enter real wall material.
        const auto cut_host = doubled ? make_wall(checked) : TopoDS_Shape{};
        const auto require_clear = [&](const TopoDS_Shape& first, const TopoDS_Shape& second, const char* message) {
            try {
                BRepAlgoAPI_Common common(first, second);
                common.Build();
                if (!common.IsDone() || common.HasErrors())
                    throw std::invalid_argument("Double door clearance computation failed");
                if (solid_volume(common.Shape()) > tolerance * tolerance * std::max(1.0, clear_height))
                    throw std::invalid_argument(message);
            } catch (const Standard_Failure& error) {
                throw std::invalid_argument(std::string("Double door clearance failed: ") + error.what());
            }
        };
        for (int index = 0; index < leaf_count; ++index) {
            const bool at_end = door_operation && (index == 0 ? door_operation->hinge_at_end
                                                             : !door_operation->hinge_at_end);
            const double part_width = leaf_width / leaf_count;
            double part_start = leaf_start + (leaf_count == 2 && at_end ? part_width : 0.0);
            double part_across = leaf_across;
            if (sliding) {
                const double track_side = (door_operation->swing_left ? 1.0 : -1.0) * (index == 0 ? 1.0 : -1.0);
                part_across += track_side * (panel_depth * 0.5 + tolerance);
                if (index == 0) part_start += (at_end ? -1.0 : 1.0) * part_width * door_operation->slide_fraction;
            }
            auto leaf = opening_box(leaf_frame, part_start, part_across, part_width,
                                    panel_depth, clear_height, base_elevation,
                                    "Opening assembly panel construction failed");
            auto leaf_envelope = leaf;
            std::optional<TopoDS_Shape> glazing;
            if (assembly.glazing_thickness_m > tolerance) {
                const double glazing_width = part_width * 0.65;
                const double glazing_height = clear_height * 0.65;
                const double glazing_start = part_start + part_width * 0.175;
                const double glazing_elevation = base_elevation + clear_height * 0.175;
                // The pane replaces leaf material rather than overlapping it.
                // Extend well past the leaf faces: a tolerance-sized overrun can
                // make OCCT treat the tool face as coincident and retain its vertices.
                const double aperture_overrun = std::max(panel_depth, 100.0 * tolerance);
                const auto aperture = opening_box(leaf_frame, glazing_start,
                    part_across - aperture_overrun, glazing_width,
                    panel_depth + 2.0 * aperture_overrun,
                    glazing_height, glazing_elevation,
                    "Door assembly glazing aperture construction failed");
                leaf = cut(leaf, aperture);
                glazing = opening_box(leaf_frame, glazing_start,
                                       part_across + (panel_depth - assembly.glazing_thickness_m) * 0.5,
                                       glazing_width, assembly.glazing_thickness_m,
                                       glazing_height, glazing_elevation,
                                       "Door assembly glazing construction failed");
            }
            std::optional<gp_Pnt> hinge;
            double swing_angle = 0.0;
            if (door_operation && !sliding) {
                const double hinge_along = at_end ? part_start + part_width : part_start;
                hinge = opening_point(leaf_frame, hinge_along,
                                      doubled ? double_hinge_across : part_across + panel_depth * 0.5,
                                      base_elevation);
                swing_angle = door_operation->angle_degrees * std::numbers::pi / 180.0 *
                              (door_operation->swing_left ? 1.0 : -1.0) *
                              (at_end ? -1.0 : 1.0);
                const double far_along = at_end ? part_start : part_start + part_width;
                const auto closed = opening_point(leaf_frame, far_along,
                    part_across + panel_depth * 0.5, base_elevation);
                const double dx = closed.X() - hinge->X(), dy = closed.Y() - hinge->Y();
                const Vec2 opened{hinge->X() + dx * std::cos(swing_angle) - dy * std::sin(swing_angle),
                                  hinge->Y() + dx * std::sin(swing_angle) + dy * std::cos(swing_angle)};
                if (std::abs(swing_angle) > tolerance) {
                    door_swings.push_back(arc_from_chord_angle({closed.X(), closed.Y()}, opened, swing_angle));
                    if (!doubled) door_swing = door_swings.back();
                }
                leaf = rotate_opening_part(leaf, *hinge, swing_angle,
                                           "Opening assembly leaf rotation failed");
                if (doubled) {
                    leaf_envelope = rotate_opening_part(leaf_envelope, *hinge, swing_angle,
                                                       "Double door clearance rotation failed");
                    for (const auto& frame_part : frame_parts)
                        require_clear(leaf_envelope, frame_part, "Double door leaf intersects its frame");
                    require_clear(leaf_envelope, cut_host, "Double door leaf intersects its host wall");
                    for (const auto& other_leaf : posed_leaf_envelopes)
                        require_clear(leaf_envelope, other_leaf, "Double door leaves collide at the requested swing angle");
                    posed_leaf_envelopes.push_back(std::move(leaf_envelope));
                }
            }
            add(leaf);
            if (glazing.has_value()) {
                if (hinge.has_value()) {
                    *glazing = rotate_opening_part(*glazing, *hinge, swing_angle,
                                                  "Door assembly glazing rotation failed");
                }
                add(*glazing);
            }
        }
    } else if (assembly.window_layout == WindowLayoutKind::fixed) {
        // Keep the v1 construction arithmetic and operation order unchanged:
        // existing IFC native-profile admission compares the exact mesh.
        const double sash_bar = std::min(frame_width * 0.6, clear_width * 0.2);
        if (sash_bar <= tolerance || clear_height - 2.0 * sash_bar <= tolerance) {
            throw std::invalid_argument("Window assembly leaves no clear glazing pane");
        }
        add(fitted_part(frame_width, panel_across, sash_bar, panel_depth,
                        clear_height, base_elevation + frame_width,
                        "Window assembly sash construction failed"));
        add(fitted_part(opening.width - frame_width - sash_bar, panel_across,
                        sash_bar, panel_depth, clear_height, base_elevation + frame_width,
                        "Window assembly sash construction failed"));
        add(fitted_part(frame_width + sash_bar, panel_across,
                        clear_width - 2.0 * sash_bar, panel_depth, sash_bar,
                        base_elevation + frame_width,
                        "Window assembly sash construction failed"));
        add(fitted_part(frame_width + sash_bar, panel_across,
                        clear_width - 2.0 * sash_bar, panel_depth, sash_bar,
                        base_elevation + opening.height - frame_width - sash_bar,
                        "Window assembly sash construction failed"));
        add(fitted_part(frame_width + sash_bar,
                        assembly.inset_m - assembly.glazing_thickness_m * 0.5,
                        clear_width - 2.0 * sash_bar, assembly.glazing_thickness_m,
                        clear_height - 2.0 * sash_bar,
                        base_elevation + frame_width + sash_bar,
                        "Window assembly glazing construction failed"));
    } else {
        const int fixed_count = assembly.window_layout == WindowLayoutKind::double_fixed ? 2
                              : assembly.window_layout == WindowLayoutKind::triple_fixed ? 3 : 1;
        const int sash_count = window_sliding ? 2 : fixed_count;
        const double mullion_width = fixed_count > 1 ? frame_width : 0.0;
        const double sash_width = (clear_width - (fixed_count - 1) * mullion_width) / sash_count;
        const double sash_bar = std::min(frame_width * 0.6, sash_width * 0.2);
        if (!std::isfinite(sash_width) || sash_width <= tolerance || sash_bar <= tolerance ||
            sash_width - 2.0 * sash_bar <= tolerance || clear_height - 2.0 * sash_bar <= tolerance)
            throw std::invalid_argument("Window assembly leaves no clear glazing pane");
        for (int index = 1; index < fixed_count; ++index) {
            add_frame(fitted_part(frame_width + index * sash_width + (index - 1) * mullion_width,
                frame_across, mullion_width, assembly.frame_depth_m, clear_height,
                base_elevation + frame_width, "Window assembly mullion construction failed"));
        }

        // Admit the actual requested pose against the original cut host, frame,
        // sill and sibling sash envelopes. This is a static clearance test; it
        // does not promise that every intermediate swing angle is clear.
        const auto cut_host = window_casement || window_sliding ? make_wall(checked) : TopoDS_Shape{};
        const auto require_clear = [&](const TopoDS_Shape& first, const TopoDS_Shape& second,
                                       const char* message) {
            try {
                BRepAlgoAPI_Common common(first, second);
                common.Build();
                if (!common.IsDone() || common.HasErrors())
                    throw std::invalid_argument("Window clearance computation failed");
                if (solid_volume(common.Shape()) > tolerance * tolerance * std::max(1.0, clear_height))
                    throw std::invalid_argument(message);
            } catch (const Standard_Failure& error) {
                throw std::invalid_argument(std::string("Window clearance failed: ") + error.what());
            }
        };
        std::vector<TopoDS_Shape> posed_sash_envelopes;
        for (int index = 0; index < sash_count; ++index) {
            double sash_start = frame_width + index * (sash_width + mullion_width);
            double sash_across = panel_across;
            if (window_sliding) {
                const bool at_end = index == 0 ? assembly.window_hinge_at_end : !assembly.window_hinge_at_end;
                sash_start = frame_width + (at_end ? sash_width : 0.0);
                const double track_side = (assembly.window_open_left ? 1.0 : -1.0) * (index == 0 ? 1.0 : -1.0);
                sash_across += track_side * (panel_depth * 0.5 + tolerance);
                if (index == 0)
                    sash_start += (at_end ? -1.0 : 1.0) * sash_width * assembly.window_slide_fraction;
            }
            std::optional<gp_Pnt> hinge;
            double angle = 0.0;
            if (window_casement) {
                const double side = assembly.window_open_left ? 1.0 : -1.0;
                // The axis passes through the outward sash corner on the
                // clear jamb face. This keeps its physical attachment fixed
                // during rotation; an inset sash cannot use a remote wall-face
                // pivot to escape a genuinely colliding requested pose.
                const double pivot_across = assembly.inset_m + side * panel_depth * 0.5;
                hinge = opening_point(frame, assembly.window_hinge_at_end ? sash_start + sash_width : sash_start,
                                      pivot_across, base_elevation + frame_width);
                angle = assembly.window_angle_degrees * std::numbers::pi / 180.0 * side *
                        (assembly.window_hinge_at_end ? -1.0 : 1.0);
            }
            const auto pose = [&](const TopoDS_Shape& part) {
                return hinge ? rotate_opening_part(part, *hinge, angle,
                    "Window assembly sash rotation failed") : part;
            };
            if (window_casement || window_sliding) {
                const auto envelope = pose(fitted_part(sash_start, sash_across, sash_width, panel_depth,
                    clear_height, base_elevation + frame_width, "Window sash envelope construction failed"));
                for (const auto& frame_part : frame_parts)
                    require_clear(envelope, frame_part, "Window sash intersects its frame or sill");
                require_clear(envelope, cut_host, "Window sash intersects its host wall");
                for (const auto& sibling : posed_sash_envelopes)
                    require_clear(envelope, sibling, "Window sashes collide at the requested pose");
                posed_sash_envelopes.push_back(envelope);
            }
            // Four nonoverlapping bars form the sash aperture. The real pane
            // fills that removed region, never the sash's material volume.
            add(pose(fitted_part(sash_start, sash_across, sash_bar, panel_depth,
                clear_height, base_elevation + frame_width, "Window assembly sash construction failed")));
            add(pose(fitted_part(sash_start + sash_width - sash_bar, sash_across,
                sash_bar, panel_depth, clear_height, base_elevation + frame_width,
                "Window assembly sash construction failed")));
            add(pose(fitted_part(sash_start + sash_bar, sash_across, sash_width - 2.0 * sash_bar,
                panel_depth, sash_bar, base_elevation + frame_width,
                "Window assembly sash construction failed")));
            add(pose(fitted_part(sash_start + sash_bar, sash_across, sash_width - 2.0 * sash_bar,
                panel_depth, sash_bar, base_elevation + opening.height - frame_width - sash_bar,
                "Window assembly sash construction failed")));
            add(pose(fitted_part(sash_start + sash_bar,
                sash_across + (panel_depth - assembly.glazing_thickness_m) * 0.5,
                sash_width - 2.0 * sash_bar, assembly.glazing_thickness_m,
                clear_height - 2.0 * sash_bar, base_elevation + frame_width + sash_bar,
                "Window assembly glazing construction failed")));
        }
    }

    if (compound.IsNull() || !BRepCheck_Analyzer(compound).IsValid() ||
        solid_volume(compound) <= tolerance * tolerance * tolerance) {
        throw std::invalid_argument("Opening assembly did not produce valid solids");
    }
    return {compound, door_swing, std::move(door_swings)};
}

TopoDS_Shape make_opening_assembly(const Wall& wall, const HostedOpening& opening,
                                  const OpeningAssembly& assembly,
                                  const std::optional<DoorOperation>& door_operation) {
    return make_opening_assembly_geometry(wall, opening, assembly, door_operation).shape;
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
            bool touches = wall_baselines_have_interior_contact(left.baseline, right.baseline);
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
        throw std::invalid_argument(
            "Wall join walls must have connected baseline junctions with physical solid contact");
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

RoofJoinPartition make_roof_join_partition(const RoofJoin& join,
                                          std::span<const TopoDS_Shape> roofs) {
    RoofJoinPartition partition;
    partition.shape = make_roof_join(join, roofs);
    partition.fused_volume = solid_volume(partition.shape);
    partition.regions.reserve(roofs.size());
    try {
        TopoDS_Shape earlier;
        double total = 0.0;
        for (std::size_t index = 0; index < roofs.size(); ++index) {
            const auto gross = solid_volume(roofs[index]);
            auto region = roofs[index];
            double overlap = 0.0;
            if (!earlier.IsNull()) {
                BRepAlgoAPI_Common common(roofs[index], earlier);
                common.Build();
                if (!common.IsDone() || common.HasErrors() ||
                    (!common.Shape().IsNull() && !BRepCheck_Analyzer(common.Shape()).IsValid()))
                    throw std::invalid_argument("Roof material overlap calculation failed");
                overlap = solid_volume(common.Shape());
                BRepAlgoAPI_Cut cut(roofs[index], earlier);
                cut.Build();
                if (!cut.IsDone() || cut.HasErrors() ||
                    (!cut.Shape().IsNull() && !BRepCheck_Analyzer(cut.Shape()).IsValid()))
                    throw std::invalid_argument("Roof material partition subtraction failed");
                // Discard any lower-dimensional boolean remnants. Every
                // nonempty region is a real solid or compound of real solids.
                BRep_Builder builder;
                TopoDS_Compound solids;
                builder.MakeCompound(solids);
                std::size_t count = 0;
                for (TopExp_Explorer solid(cut.Shape(), TopAbs_SOLID); solid.More(); solid.Next()) {
                    builder.Add(solids, solid.Current());
                    ++count;
                }
                region = count == 0 ? TopoDS_Shape{} : TopoDS_Shape{solids};
            }
            const auto net = solid_volume(region);
            const auto limit = 1e-8 * std::max({1.0, gross, partition.fused_volume});
            if (!std::isfinite(net) || net < 0.0 || net > gross + limit ||
                std::abs(gross - overlap - net) > limit ||
                (!region.IsNull() && !BRepCheck_Analyzer(region).IsValid()))
                throw std::invalid_argument("Roof material partition volume is inconsistent");
            if (!earlier.IsNull() && !region.IsNull() && common_volume(region, earlier) > limit)
                throw std::invalid_argument("Roof material regions share positive volume");
            partition.regions.push_back({join.roof_ids[index], region, gross, net});
            total += net;
            earlier = earlier.IsNull() ? roofs[index] : fuse_shapes(earlier, roofs[index],
                "Roof material prefix union failed");
        }
        if (!std::isfinite(total) || std::abs(total - partition.fused_volume) >
            1e-8 * std::max(1.0, partition.fused_volume))
            throw std::invalid_argument("Roof material regions do not conserve fused volume");
        return partition;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Roof material partition failed: ") + error.what());
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
