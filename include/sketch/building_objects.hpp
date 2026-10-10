#pragma once

#include "sketch/stair_semantics.hpp"

#include <TopoDS_Shape.hxx>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

// Three-dimensional semantic coordinates use metres.  The architectural
// solid builders keep these values separate from OCCT implementation types so
// callers can persist parameters without serialising a derived BRep shape.

struct RectangularColumn {
    std::string id;
    Vec3 base_center{};
    double width{};
    double depth{};
    double height{};
    double rotation_radians{};
};

struct CircularColumn {
    std::string id;
    Vec3 base_center{};
    double radius{};
    double height{};
    // The solid is rotationally symmetric; this angle retains the authoring
    // frame used by selection, rotation and dimension handles.
    double rotation_radians{};
};

[[nodiscard]] TopoDS_Shape make_rectangular_column(const RectangularColumn& column);
[[nodiscard]] TopoDS_Shape make_circular_column(const CircularColumn& column);

// The beam axis is the finite segment start -> end.  The supplied up vector
// is projected onto the plane normal to that axis and therefore controls the
// depth direction of the rectangular section.  A near-parallel up vector is
// rejected because it cannot define a stable local frame.
struct Beam {
    std::string id;
    Vec3 start{};
    Vec3 end{};
    Vec3 up{0.0, 0.0, 1.0};
    double width{};
    double depth{};
};

[[nodiscard]] TopoDS_Shape make_beam(const Beam& beam);

// base_position is the lower, front-left corner of the first stair tread.
// orientation_radians rotates the horizontal run counter-clockwise from +X;
// the flight width extends to the left of that run.  A top landing is an
// explicit slab at the finished flight elevation, with its depth along the
// run and its thickness along +Z.

// A stair level connection binds the lower and upper ends of a flight to one
// validated vertical-level graph.  The graph and link IDs are Document-owned;
// level IDs are resolved inside that graph at the Document validation boundary.
// This is persisted authoring data.  Geometry remains derived from the stair
// dimensions, and a connection never rewrites the stair's base coordinates.

[[nodiscard]] TopoDS_Shape make_stair_flight(const StairFlight& flight);

// A straight railing follows a horizontal baseline from base_position in the
// supplied orientation.  The top rail and vertical posts are real solids;
// post_spacing is a maximum spacing, with posts always placed at both ends.
// The shared thickness is used for the square rail and post sections.  This
// deliberately keeps the first railing form deterministic and editable while
// leaving curved/guard-specific profiles for a future schema version.
[[nodiscard]] TopoDS_Shape make_railing(const Railing& railing);
[[nodiscard]] TopoDS_Shape make_hosted_railing(const Railing& railing,
                                              const StairFlight& current_host);

// A sloped panel starts at base_position, whose XY location is the lower
// left corner of the un-overhung horizontal footprint and whose Z is the
// eave elevation.  run is the horizontal slope direction, span is transverse
// to it, and rise/pitch are both explicit and must agree.  Overhang extends
// the panel by that horizontal distance on all four footprint edges.  The
// returned prism thickness is measured normal to the sloped panel.
// A vertical through-opening defined in the roof's horizontal local frame.
// X/Y locate its lower-left corner; width/depth are horizontal projections.
struct RoofSkylight {
    double frame_width{0.06};
    double curb_height{0.15};
    double glazing_thickness{0.024};
};

struct RoofOpening {
    std::string id;
    double x{};
    double y{};
    double width{};
    double depth{};
    std::optional<RoofSkylight> skylight;
};

struct SlopedRoofPanel {
    std::string id;
    Vec3 base_position{};
    double orientation_radians{};
    double run{};
    double span{};
    double rise{};
    double pitch_radians{};
    double overhang{};
    double thickness{};
    std::vector<RoofOpening> openings;
};

[[nodiscard]] TopoDS_Shape make_sloped_roof_panel(const SlopedRoofPanel& panel);

// base_position is the centre of the un-overhung rectangular footprint at
// eave elevation.  length follows the ridge, span is the full horizontal
// eave-to-eave distance, and rise/pitch describe one half of that span.  The
// two planar panels are vertically clipped at the ridge before their
// normal offsets are applied.  They are returned as a valid compound of real
// solids that share the ridge face without positive-volume overlap; each
// panel's thickness is measured normal to its own slope.
struct GableRoof {
    std::string id;
    Vec3 base_position{};
    double orientation_radians{};
    double length{};
    double span{};
    double rise{};
    double pitch_radians{};
    double overhang{};
    double thickness{};
    std::vector<RoofOpening> openings;
};

[[nodiscard]] TopoDS_Shape make_gable_roof(const GableRoof& roof);

// Equal-pitch hip roof, centred at eave elevation. Length follows the ridge
// and must be at least span; equal values produce a pyramid. Thickness is
// measured normal to each slope, with vertical trims at eaves, hips and ridge.
struct HipRoof {
    std::string id;
    Vec3 base_position{};
    double orientation_radians{};
    double length{};
    double span{};
    double rise{};
    double pitch_radians{};
    double overhang{};
    double thickness{};
    std::vector<RoofOpening> openings;
};

[[nodiscard]] TopoDS_Shape make_hip_roof(const HipRoof& roof);

// Primitive roof builders above return only the cut roof material. These
// builders return a fixed skylight's curb, frame and glazing, with normal
// height/thickness and frame widths measured on the host plane. The footprint
// stays strictly inside its vertical opening. The opening must belong to a
// host already validated by its primitive builder.
[[nodiscard]] TopoDS_Shape make_roof_skylight(const SlopedRoofPanel& roof, const RoofOpening& opening);
[[nodiscard]] TopoDS_Shape make_roof_skylight(const GableRoof& roof, const RoofOpening& opening);
[[nodiscard]] TopoDS_Shape make_roof_skylight(const HipRoof& roof, const RoofOpening& opening);

}  // namespace sketch
