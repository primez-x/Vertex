#pragma once

#include "sketch/geometry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace sketch {

struct MeasurementGraphSource {
    std::string owner_id;
    std::string segment_id;
    Segment geometry;
};

// Parameters are an ascending interval in the original directed segment.
// reversed means the derived edge traverses that interval backwards.
struct MeasurementSourceUse {
    std::string owner_id;
    std::string segment_id;
    double parameter_start{};
    double parameter_end{};
    bool reversed{};
};

struct DerivedMeasurementEdge {
    Segment geometry;
    std::vector<MeasurementSourceUse> source_uses;
};

struct MeasurementFaceEdgeUse {
    std::size_t edge_index{};
    bool reversed{};
};

struct DerivedMeasurementFace {
    Boundary boundary;
    std::vector<MeasurementFaceEdgeUse> edge_uses;
    double area_square_metres{};
    // Immediate strict containing outline, indexed in the final faces vector.
    // Roots have no parent. This relationship does not subtract area or turn
    // the exact gross boundary into an outline with holes.
    std::optional<std::size_t> parent_face_index;
};

struct MeasurementAreaGraph {
    std::vector<DerivedMeasurementEdge> edges;
    std::vector<DerivedMeasurementFace> faces;
};

// Nodes analytical intersections and overlaps without modifying measured
// sources. Edge geometry, source intervals and bounded faces are deterministic
// under source permutation. No tessellation or tolerance-based snapping.
// Throws invalid_argument for invalid IDs/geometry/tolerance, indeterminate
// contacts, or pieces too small for reliable face extraction. O(n^2) contacts;
// At most 2048 sources, 16384 represented stations/derived edges and 65536
// processed contacts are supported; budgets reject before unbounded noding.
// Disconnected nested outlines retain their exact gross boundaries and receive
// immediate containment parents. Shared-edge faces remain separate graph
// regions; touching or ambiguous separate outlines and ill-conditioned arcs
// fail explicitly. The metre
// tolerance bounds minimum usable edge length; it never merges near geometry.
[[nodiscard]] MeasurementAreaGraph build_measurement_area_graph(
    const std::vector<MeasurementGraphSource>& sources,
    double tolerance_metres = default_geometry_tolerance_metres);

} // namespace sketch
