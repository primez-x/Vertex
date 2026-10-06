#pragma once

#include "sketch/pinc_geometry_admission.hpp"

namespace sketch {
// Explicit native classification review. Imported category names never supply
// appraisal facts or deduction/physical-wall authority. One record per source
// assignment is required, including a deliberate reference-only disposition.
struct PincAreaReview {
    std::size_t page_index{};
    std::size_t assignment_index{};
    bool import_area{true};
    std::string classification;
};
struct PincNativeAreaMapping {
    PincSourceReference source;
    std::string area_id;
};
struct PincMeasurementAdmission {
    PincGeometryAdmission geometry;
    // Complete geometry plus source-derived area upserts against private_base.
    std::vector<Entity> entities;
    std::vector<PincNativeAreaMapping> area_mappings;
    std::vector<PincImportDiagnostic> diagnostics;
};

// Detached preparation. Rebuilds geometry and face ownership independently and
// previews one combined native command. Unresolved or declined assignments stay
// visible in diagnostics; no cached source area/anchor or face index is authority.
// Target is a newly scaffolded independent project: each reviewed property must
// explicitly use the generic measurement workflow, and the base must contain no
// source-derived measured areas. Unrelated scaffold entities remain unchanged.
// Reviews cover every assignment occurrence exactly once. Definitions accept
// only exact nonempty native descriptive area-type classifications; unknown
// source categories remain reference diagnostics even with an import review.
// UND requires an explicit non_calculated review to define a native area.
// Limits are downward-only. Aggregate counters conservatively partition native
// geometry and area-authoring work, reserving repeated graph passes in advance;
// near-ceiling inputs may be refused rather than expanding work beyond limits.
// Failure is atomic and reported as invalid_argument.
// No mutation, publication, presentation transport, assets or ANSI inference.
[[nodiscard]] PincMeasurementAdmission prepare_pinc_measurement_admission(
    const PincImportProject& source, const DocumentSnapshot& private_base,
    std::span<const PincPageGeometryContext> contexts, std::string_view fresh_namespace,
    std::span<const PincAreaReview> reviews,
    const PincGeometryAdmissionLimits& limits = {});
} // namespace sketch
