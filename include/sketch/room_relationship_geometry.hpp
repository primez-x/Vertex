#pragma once

#include "sketch/geometry.hpp"
#include "sketch/room_relationships.hpp"

#include <string>
#include <vector>

namespace sketch {

class DocumentSnapshot;

// Geometry is supplied as a detached snapshot so callers can compare an
// edited document candidate without mutating the authoritative Document.
// Boundary roles require a valid closed Boundary; wall roles require exactly
// one valid Segment. Stable entity/edge/vertex identities remain owned by the
// caller and are preserved when a proposal is committed.
struct RelationshipGeometry {
    std::string id;
    RoomReferenceKind kind{};
    Boundary geometry;
};

struct RelationshipGeometryChange {
    std::string source_id;
    RoomReferenceKind source_kind{};
    PlanarTransform transform;
    Boundary geometry;
    std::vector<std::string> driver_ids;
};

struct RoomRelationshipGeometryResult {
    std::vector<RelationshipGeometryChange> changes;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool has_diagnostics() const noexcept { return !diagnostics.empty(); }
};

// Propose rigid geometry propagation for explicit room relationships. The
// operation compares the before snapshot with the edited after snapshot,
// resolves dependency chains in deterministic target-first order, and returns
// detached changes for confirmation. It never mutates either input. Only
// rotation and translation are inferred; scale, deformation, reflection,
// invalid geometry, missing records, and conflicting derived-from drivers are
// reported and produce no change for the affected source.
[[nodiscard]] RoomRelationshipGeometryResult propose_room_relationship_geometry(
    const RoomRelationshipSnapshot& relationships,
    const std::vector<RelationshipGeometry>& before,
    const std::vector<RelationshipGeometry>& after,
    double tolerance_metres = default_geometry_tolerance_metres);

} // namespace sketch
