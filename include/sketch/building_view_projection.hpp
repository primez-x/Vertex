#pragma once

#include "sketch/building_entity.hpp"
#include "sketch/geometry.hpp"

#include <TopoDS_Shape.hxx>

#include <limits>

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

// Model-metre bounds in the view frame, unbounded along the viewing direction.
// Horizontal is cross(up, -direction); vertical is up. Bounds must be finite,
// within +/-1e6 metres, and span more than 1e-6 metres on each axis.
struct BuildingViewCrop {
    BuildingViewFrame frame;
    double min_horizontal_m;
    double max_horizontal_m;
    double min_vertical_m;
    double max_vertical_m;
};

// A depth range used by view adapters before deriving linework. The origin and
// direction use the same model-space convention as BuildingViewFrame. The
// bounds filter is conservative; clip_shape_to_view_depth performs the exact
// far-plane operation for crossing solids. Infinity keeps the helper useful
// for views that have no depth limit.
struct BuildingViewDepth {
    Vec3 origin{};
    Vec3 direction{0.0, 0.0, -1.0};
    double far_depth_m{std::numeric_limits<double>::infinity()};
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

// Return whether any part of a derived solid can occur at or before the
// requested far depth.  This is intentionally a conservative, object-level
// test based on a conservative BRep bounding box: partially crossing objects
// remain visible so callers can apply the exact clip below.
[[nodiscard]] bool shape_intersects_view_depth(const TopoDS_Shape& shape,
                                               const BuildingViewDepth& depth);

// Clip a derived solid to the finite near side of the requested far-depth
// plane. Objects wholly before the plane are returned unchanged; objects
// wholly beyond it return a null shape; crossing objects are intersected with
// an OCCT half-space. The semantic source remains authoritative and is never
// modified. Infinity returns the original shape.
[[nodiscard]] TopoDS_Shape clip_shape_to_view_depth(const TopoDS_Shape& shape,
                                                   const BuildingViewDepth& depth);

// Conservative bounding-box filter; true does not guarantee an intersection.
// Invalid crops throw std::invalid_argument, including for null input shapes.
[[nodiscard]] bool shape_intersects_view_crop(const TopoDS_Shape& shape,
                                              const BuildingViewCrop& crop);

// Intersect a derived solid or face-based surface with the four view-aligned
// crop half-spaces before projection. Fully contained shapes are returned
// unchanged; disjoint shapes return null. Source geometry is never modified.
// OCCT failures throw std::invalid_argument. No tessellation or projected-curve
// approximation occurs.
[[nodiscard]] TopoDS_Shape clip_shape_to_view_crop(const TopoDS_Shape& shape,
                                                  const BuildingViewCrop& crop);

}  // namespace sketch
