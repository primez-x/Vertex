#include "sketch/architecture.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;

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

Boundary strip(const Segment& baseline, double thickness) {
    if (baseline.sweep_radians == 0) {
        const double length = segment_length(baseline);
        const double nx = -(baseline.end.y - baseline.start.y) * thickness / (2 * length);
        const double ny = (baseline.end.x - baseline.start.x) * thickness / (2 * length);
        const Vec2 a{baseline.start.x + nx, baseline.start.y + ny};
        const Vec2 b{baseline.end.x + nx, baseline.end.y + ny};
        const Vec2 c{baseline.end.x - nx, baseline.end.y - ny};
        const Vec2 d{baseline.start.x - nx, baseline.start.y - ny};
        return {{a, b, 0}, {b, c, 0}, {c, d, 0}, {d, a, 0}};
    }
    const auto origin = centre(baseline);
    const double radius = std::hypot(baseline.start.x - origin.x, baseline.start.y - origin.y);
    if (thickness * 0.5 >= radius - tolerance) throw std::invalid_argument("Wall thickness crosses its arc centre");
    const auto radial = [&](Vec2 point, double offset) -> Vec2 {
        const double scale = (radius + offset) / radius;
        return {origin.x + (point.x - origin.x) * scale, origin.y + (point.y - origin.y) * scale};
    };
    const auto a = radial(baseline.start, thickness * 0.5);
    const auto b = radial(baseline.end, thickness * 0.5);
    const auto c = radial(baseline.end, -thickness * 0.5);
    const auto d = radial(baseline.start, -thickness * 0.5);
    return {{a, b, baseline.sweep_radians}, {b, c, 0},
            {c, d, -baseline.sweep_radians}, {d, a, 0}};
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
        auto result = extrude(strip(wall.baseline, wall.thickness), wall.elevation, wall.height);
        for (const auto& opening : wall.openings) {
            const double from = opening.offset / length;
            const double to = (opening.offset + opening.width) / length;
            const Segment interval{point_at(wall.baseline, from), point_at(wall.baseline, to),
                                   wall.baseline.sweep_radians * (to - from)};
            const auto tool = extrude(strip(interval, wall.thickness), wall.elevation + opening.sill, opening.height);
            result = cut(result, tool);
        }
        if (solid_volume(result) <= tolerance * tolerance * tolerance) throw std::invalid_argument("Openings remove the entire wall");
        return result;
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Wall geometry failed: ") + error.what());
    }
}

TopoDS_Shape make_slab(const Slab& slab) {
    if (slab_element_kind_name(slab.element_kind) == "invalid") {
        throw std::invalid_argument("Slab element kind is invalid");
    }
    positive(slab.thickness, "Slab thickness must be positive");
    if (!std::isfinite(slab.elevation)) throw std::invalid_argument("Slab elevation must be finite");
    try {
        auto result = extrude(slab.boundary, slab.elevation, slab.thickness);
        const auto original = result;
        const auto outer_wire = wire(slab.boundary, slab.elevation);
        std::vector<TopoDS_Shape> previous;
        std::vector<TopoDS_Wire> previous_wires;
        for (const auto& hole : slab.holes) {
            auto tool = extrude(hole, slab.elevation, slab.thickness);
            auto hole_wire = wire(hole, slab.elevation);
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
    } catch (const Standard_Failure& error) {
        throw std::invalid_argument(std::string("Slab geometry failed: ") + error.what());
    }
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
