#pragma once

#include "sketch/measurement_area_graph.hpp"
#include "sketch/wall_semantics.hpp"

#include <map>

namespace sketch {

struct PhysicalClearComponent {
    std::size_t baseline_face_index{};
    std::size_t component_index{};
    Boundary boundary;
    std::vector<Boundary> holes;
    double area_square_metres{};
};

struct PhysicalClearGeometry {
    MeasurementAreaGraph graph;
    std::vector<PhysicalClearComponent> spaces;
};

// Document-free geometry for walls admitted into one resolved contact domain
// and effective plane. Baselines retain their original directed graph sources;
// material walls may use the equivalent reversed representation. Uncut wall
// material gives hosted openings continuous virtual room limits. No entities,
// classifications, retained lineage or IDs are created. Source/context/phase
// admission belongs to the shared document adapter. Geometry, contacts and
// Boolean work have the same bounded policy as physical room discovery.
[[nodiscard]] PhysicalClearGeometry derive_physical_clear_geometry(
    const std::vector<MeasurementGraphSource>& baselines,
    const std::map<std::string, Wall, std::less<>>& material_walls);

} // namespace sketch
