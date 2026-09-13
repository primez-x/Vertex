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
#include <utility>

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
