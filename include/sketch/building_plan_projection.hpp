#pragma once

#include "sketch/building_entity.hpp"
#include "sketch/geometry.hpp"
#include "sketch/assembly_model.hpp"

namespace sketch {

// Derive the visible, top-down plan edges for one canonical building object.
// The returned segments are in the document's world XY coordinates.  A
// nonzero sweep denotes an exact circular arc; zero sweep denotes a line.
// This is presentation geometry only.  It must not be used as an authoring or
// calculation boundary.
//
// HLR hidden edges are intentionally omitted.  Invalid object geometry,
// unavailable projection data, and projected curve types other than lines or
// circles are reported as std::invalid_argument.
[[nodiscard]] Boundary project_building_plan(const BuildingObject& object);

// `object` is already in its effective world placement (ordinary objects
// resolve_vertical_placement once before decoding; hosted railings decode
// directly). `entities` must be the original authored source map. The shape
// builder resolves a hosted stair from that source exactly once. Never pass
// a map containing previously level-resolved copies.
[[nodiscard]] Boundary project_building_plan(
    const BuildingObject& object,
    const std::map<std::string, Entity, std::less<>>& entities);

// Projects the complete transformed profile compound using hidden-line removal.
// An expansion without actual profiles is rejected. Semantic declarations never
// become inferred geometry. Use project_assembly_view for individual source paths.
[[nodiscard]] Boundary project_assembly_plan(const AssemblyExpansion& expansion);

// Project an already admitted actual solid with the same analytical HLR path
// as building and assembly plans. The caller retains geometry authority and
// must apply its construction/work admission before invoking this function.
[[nodiscard]] Boundary project_building_shape_plan(const TopoDS_Shape& shape);

}  // namespace sketch
