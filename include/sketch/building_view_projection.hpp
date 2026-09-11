#pragma once

#include "sketch/building_entity.hpp"
#include "sketch/geometry.hpp"

#include <TopoDS_Shape.hxx>

namespace sketch {

// A view frame uses model metres. direction points from the viewer toward the
// model; up is the paper-up direction. Both vectors must be unit length and
// orthogonal. Projection geometry is derived presentation data and never an
// authoring or calculation boundary.
struct BuildingViewFrame {
    Vec3 origin{};
    Vec3 direction{0.0, 0.0, -1.0};
    Vec3 up{0.0, 1.0, 0.0};
};

enum class BuildingViewKind { plan, elevation, section };

// Derive visible line and analytic arc geometry for one supported building
// object in the requested frame. Plan and elevation use hidden-line removal;
// section intersects the solid with the frame plane before extracting its
// edges. Unsupported derived curve types fail closed instead of being
// approximated into measurement-looking pixels.
[[nodiscard]] Boundary project_building_view(const BuildingObject& object,
                                             BuildingViewKind kind,
                                             const BuildingViewFrame& frame = {});

// Shape-level counterpart for semantic objects that have a dedicated model
// (for example walls with hosted openings or slabs with holes). The shape is
// still derived input; callers must keep the semantic entity authoritative.
[[nodiscard]] Boundary project_shape_view(const TopoDS_Shape& shape,
                                          BuildingViewKind kind,
                                          const BuildingViewFrame& frame = {});

}  // namespace sketch
