#pragma once
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_edit.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document.hpp"

namespace sketch::testing {
inline Document document_with_removed_automatic_angle() {
    IdentifiedBoundary outline{"angle-area", "measurement_boundary", {
        {"old-e0", "old-v0", "old-v1", {{0,0},{4,0},0}},
        {"old-e1", "old-v1", "old-v2", {{4,0},{4,3},0}},
        {"old-e2", "old-v2", "old-v3", {{4,3},{0,3},0}},
        {"old-e3", "old-v3", "old-v0", {{0,3},{0,0},0}}}};
    BoundaryDimension angle{"automatic-angle", outline.id, "old-e0", {4.5,0.5}};
    angle.kind = BoundaryDimensionKind::angle;
    angle.placement = BoundaryDimensionPlacement::automatic;
    angle.automatic_placement_version = 2;
    angle.secondary_segment_id = "old-e1";
    angle.vertex_id = "old-v1";
    auto owner = encode_identified_boundary_entity(outline);
    owner.extensions["preserve"] = {{"number", 1.0}};
    auto document = Document::create({owner, encode_boundary_dimension_entity(angle)});
    IdentifiedBoundary triangle{outline.id, outline.type, {
        {"new-e0", "new-v0", "new-v1", {{0,0},{4,0},0}},
        {"new-e1", "new-v1", "new-v2", {{4,0},{0,3},0}},
        {"new-e2", "new-v2", "new-v0", {{0,3},{0,0},0}}}};
    BoundaryGeometryEdit edit;
    edit.kind = BoundaryGeometryEditKind::redefine_boundary;
    edit.boundary_id = edit.target_id = owner.id;
    edit.replacement_segments = encode_identified_boundary_entity(triangle).properties.at("segments");
    edit.replacement_removed_reference_ids = {angle.id};
    edit.allow_automatic_angle_removal = true;
    (void)document.apply(EditBoundaryGeometry{document.revision(), edit});
    return document;
}
} // namespace sketch::testing
