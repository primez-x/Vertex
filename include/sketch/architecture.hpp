#pragma once

#include "sketch/wall_semantics.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/terrain_surface.hpp"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// A slab footprint can represent the primary horizontal architectural
// assemblies.  The geometry is shared, while the semantic kind controls
// schedules, presentation, and future level/material rules.  `slab` keeps
// compatibility with existing generic slab records.
enum class SlabElementKind { slab, floor, ceiling, foundation };

[[nodiscard]] std::string_view slab_element_kind_name(SlabElementKind kind) noexcept;
[[nodiscard]] std::optional<SlabElementKind>
parse_slab_element_kind(std::string_view value) noexcept;

struct Slab {
    std::string id;
    Boundary boundary;
    std::vector<Boundary> holes;
    double thickness{};
    double elevation{};
    SlabElementKind element_kind{SlabElementKind::slab};
    std::vector<SlabLayer> layers;
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
