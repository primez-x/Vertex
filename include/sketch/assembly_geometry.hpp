#pragma once

#include "sketch/assembly_model.hpp"
#include <TopoDS_Shape.hxx>

namespace sketch {

struct AssemblyProfileSolid {
    // Complete semantic provenance, including the untransformed local profile.
    AssemblyExpandedProfile source;
    TopoDS_Shape shape;
    double volume_m3{};
};

struct AssemblyGeometry {
    // Input expansion order is retained. No boolean union merges identities.
    std::vector<AssemblyProfileSolid> solids;
    // Null for an expansion without profiles; otherwise a compound of solids.
    TopoDS_Shape shape;
    double volume_m3{};
};

// Derived OCCT cache only. Volumes are measured from each transformed solid.
[[nodiscard]] AssemblyGeometry make_assembly_geometry(const AssemblyExpansion& expansion);

} // namespace sketch
