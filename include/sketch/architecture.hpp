#pragma once

#include "sketch/wall_semantics.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/door_operation.hpp"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <optional>
#include <span>
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

// A room volume is a semantic architectural enclosure.  It deliberately
// remains separate from appraisal/measurement boundaries: the boundary and
// optional holes define the plan footprint, while height and elevation define
// the derived 3D volume.  Materials and room classification remain properties
// of the owning document entity rather than of this geometry kernel value.
struct RoomVolume {
    std::string id;
    Boundary boundary;
    std::vector<Boundary> holes;
    double height{};
    double elevation{};
};

// Shapes are derived caches. Persist semantic parameters, never replace the
// authoritative wall/boundary/host relationships with these solids.
[[nodiscard]] TopoDS_Shape make_wall(const Wall& wall);
// Build the derived solid for a first-class fused wall join.  Every source
// wall remains authoritative; this result is a coordinated-view cache and
// does not replace the individual wall entities or their quantities.
[[nodiscard]] TopoDS_Shape make_wall_join(const WallJoin& join,
                                          std::span<const Wall> walls);
// Build the derived frame, leaf/sash, and glazing solids for one hosted
// opening. The wall cut and opening dimensions remain authoritative; this
// result is a coordinated-view presentation only.
[[nodiscard]] TopoDS_Shape make_opening_assembly(
    const Wall& wall, const HostedOpening& opening,
    const OpeningAssembly& assembly,
    const std::optional<DoorOperation>& door_operation = std::nullopt);
// Build the derived solid for a first-class fused roof join.  Source roof
// solids remain authoritative semantic objects; this result is only the
// coordinated-view union.
[[nodiscard]] TopoDS_Shape make_roof_join(const RoofJoin& join,
                                          std::span<const TopoDS_Shape> roofs);
[[nodiscard]] TopoDS_Shape make_slab(const Slab& slab);
[[nodiscard]] TopoDS_Shape make_room_volume(const RoomVolume& room);
// Build a derived triangulated terrain surface from the validated local TIN
// model. The result is a displayable compound of real OCCT faces; the
// semantic points/triangles remain authoritative in the document.
[[nodiscard]] TopoDS_Shape make_terrain_surface(const TerrainSurface& surface);
[[nodiscard]] double solid_volume(const TopoDS_Shape& shape);
// Shared analytical line/arc profile construction for solids and area booleans.
[[nodiscard]] TopoDS_Face make_planar_face(const Boundary& boundary, double elevation = 0);
[[nodiscard]] double surface_area(const TopoDS_Shape& shape);

} // namespace sketch
