#pragma once

#include "sketch/wall_semantics.hpp"
#include "sketch/terrain_surface.hpp"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <string>
#include <vector>

namespace sketch {

struct Slab {
    std::string id;
    Boundary boundary;
    std::vector<Boundary> holes;
    double thickness{};
    double elevation{};
};

// Shapes are derived caches. Persist semantic parameters, never replace the
// authoritative wall/boundary/host relationships with these solids.
[[nodiscard]] TopoDS_Shape make_wall(const Wall& wall);
[[nodiscard]] TopoDS_Shape make_slab(const Slab& slab);
// Build a derived triangulated terrain surface from the validated local TIN
// model. The result is a displayable compound of real OCCT faces; the
// semantic points/triangles remain authoritative in the document.
[[nodiscard]] TopoDS_Shape make_terrain_surface(const TerrainSurface& surface);
[[nodiscard]] double solid_volume(const TopoDS_Shape& shape);
// Shared analytical line/arc profile construction for solids and area booleans.
[[nodiscard]] TopoDS_Face make_planar_face(const Boundary& boundary, double elevation = 0);
[[nodiscard]] double surface_area(const TopoDS_Shape& shape);

} // namespace sketch
