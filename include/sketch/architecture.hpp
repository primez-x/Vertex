#pragma once

#include "sketch/wall_semantics.hpp"
#include "sketch/slab_semantics.hpp"
#include "sketch/terrain_surface.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/door_operation.hpp"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <cstddef>
#include <cstddef>
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
// Already admitted actual wall solids and semantic walls have the same authored
// order. Uses the historic join's baseline junction AND solid-contact rule.
// Bounded to 32 members, 128 openings and 32 layers per wall; components retain
// authored order, including singletons. Invalid geometry throws.
[[nodiscard]] std::vector<std::vector<std::size_t>> wall_shape_connected_components(
    std::span<const Wall> walls, std::span<const TopoDS_Shape> wall_shapes);
// Build the derived frame, leaf/sash, and glazing solids for one hosted
// opening. The wall cut and opening dimensions remain authoritative; this
// result is a coordinated-view presentation only.
[[nodiscard]] TopoDS_Shape make_opening_assembly(
    const Wall& wall, const HostedOpening& opening,
    const OpeningAssembly& assembly,
    const std::optional<DoorOperation>& door_operation = std::nullopt);
// The same admitted manufactured shape with its physical clear-leaf swing.
// The arc uses the actual leaf hinge, inset and extent computed by the solid
// factory, including the fitted planar leaf in a curved frame.
struct OpeningAssemblyGeometry {
    TopoDS_Shape shape;
    std::optional<Segment> door_swing;
    // Physical clear-leaf arcs for every hinged leaf. door_swing remains the
    // legacy single-hinged convenience value; sliding operations have no arcs.
    std::vector<Segment> door_swings;
};
[[nodiscard]] OpeningAssemblyGeometry make_opening_assembly_geometry(
    const Wall& wall, const HostedOpening& opening,
    const OpeningAssembly& assembly,
    const std::optional<DoorOperation>& door_operation = std::nullopt);
// Build the derived solid for a first-class fused roof join.  Source roof
// solids remain authoritative semantic objects; this result is only the
// coordinated-view union.
[[nodiscard]] TopoDS_Shape make_roof_join(const RoofJoin& join,
                                          std::span<const TopoDS_Shape> roofs);
// Validates every actual positive-mass roof solid and uses exactly the join's
// shape-distance/tolerance contact rule. Components and their member indices
// retain input authored order, including singletons. Geometry failures throw.
[[nodiscard]] std::vector<std::vector<std::size_t>> roof_shape_connected_components(
    std::span<const TopoDS_Shape> roofs);
// roof_ids and source shapes have the same authored order. Earlier members
// own shared material. Gross volumes include each source's own openings;
// net regions subtract all earlier members, with no waste allowance.
struct RoofJoinRegion {
    std::string source_roof_id;
    TopoDS_Shape shape; // null only for a completely occluded, zero-volume member
    double gross_volume{};
    double net_volume{};
};
struct RoofJoinPartition {
    TopoDS_Shape shape; // authoritative fused geometry, independent of appearance
    double fused_volume{};
    std::vector<RoofJoinRegion> regions;
};
// Returns a complete validated partition or throws; never publishes partial
// regions. Preserves the connected positive-mass compound join contract.
[[nodiscard]] RoofJoinPartition make_roof_join_partition(
    const RoofJoin& join, std::span<const TopoDS_Shape> roofs);
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
