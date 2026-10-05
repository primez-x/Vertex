#pragma once

#include "sketch/pinc_project_import.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/project_organization.hpp"

#include <span>
#include <string_view>

namespace sketch {
struct PincPageGeometryContext {
    std::size_t page_index{};
    DrawingContext calculation;
    DrawingContext interior;
};
struct PincGeometryAdmissionLimits {
    PincImportLimits source;
    std::size_t max_graph_edges{65'536};
    std::size_t max_face_edge_uses{100'000};
    std::uint64_t max_correspondence_work{250'000};
};
struct PincSourceGeometryMapping {
    PincSourceReference source;
    std::string stroke_id;
    std::string segment_id;
    std::string start_vertex_id;
    std::string end_vertex_id;
    // For a legacy alias, source traversal may reverse native segment traversal.
    bool reversed{};
};
struct PincAssignmentCorrespondence {
    std::size_t assignment_index{};
    PincSourceReference source;
    std::optional<std::size_t> face_index;
    std::string code; // Correspondence diagnostic code, not area classification.
    std::string reason;
};
struct PincAdmittedPageGeometry {
    std::size_t page_index{};
    DrawingContext calculation_context;
    DrawingContext interior_context;
    std::vector<std::string> calculation_stroke_ids;
    std::vector<std::string> interior_stroke_ids;
    MeasurementAreaGraph graph;
    std::vector<PincAssignmentCorrespondence> assignments;
};
struct PincGeometryAdmission {
    std::vector<Entity> entities;
    std::vector<PincSourceGeometryMapping> source_mappings;
    std::vector<PincAdmittedPageGeometry> pages;
    std::vector<PincImportDiagnostic> diagnostics;
};

// Geometry-only detached preparation; no mutation/publication, area ownership,
// classification actions, appraisal facts, physical wall inference or assets.
// Desktop must obtain source from the validated/attested Pinc broker protocol.
// This checks geometry/identity/budgets again, not broker attestation or full
// presentation transport validity. Source identifiers never become native IDs.
// Contexts must cover every page exactly once and use separate empty calculation/
// interior layers in the same reviewed floor. Pages may share a reviewed floor;
// every page/lane still has a distinct complete drawing context.
// Base is independently revalidated. Namespace (1..101 ASCII alnum/_/-) must be
// freshly allocated by the caller; existing entity/asset/topology collisions
// refuse atomically. This reserves the longest suffix within native's 128-byte
// ID ceiling. Every imported segment becomes one required open single-edge stroke.
// Modern correspondence independently nodes the original calculation sources,
// requires unique exact source-use membership, then matches its complete analytic
// cycle to one native graph face. Legacy correspondence requires ordered exact
// source intervals covering its complete original area cycle; junction splits
// preserve correspondence while partitions do not. Assignment ownership must be
// exclusive. Nested outlines retain separate
// gross areas; containment does not establish holes or deductions. Matching never
// uses source cached area/anchor, centroids, or a source key alone.
// Returned face indices are transient review handles: subsequent area authoring
// must rederive through prepare_measurement_area_definition on a private preview.
// Limits can only reduce hard defaults; over-budget admission throws before
// expensive correspondence expansion. Both graphs charge the aggregate pair,
// derived-edge and face-traversal budgets. All errors are invalid_argument.
[[nodiscard]] PincGeometryAdmission admit_pinc_geometry(
    const PincImportProject& source,
    const DocumentSnapshot& private_base,
    std::span<const PincPageGeometryContext> contexts,
    std::string_view fresh_namespace,
    const PincGeometryAdmissionLimits& limits = {});
} // namespace sketch
