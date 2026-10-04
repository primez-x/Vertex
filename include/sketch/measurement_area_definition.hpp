#pragma once

#include "sketch/measurement_area_graph.hpp"
#include "sketch/project_organization.hpp"

namespace sketch {

enum class MeasurementAreaDisposition { reference_only, define_area, deduct_from_parent };

struct MeasurementAreaChoice {
    MeasurementAreaDisposition disposition{MeasurementAreaDisposition::reference_only};
    std::string classification;
    // Transient review token only. Equal tokens request one connected area.
    std::optional<std::size_t> combine_group;
};

struct DetectedMeasurementAreas {
    DrawingContext context;
    MeasurementAreaGraph graph;
    std::vector<std::optional<std::string>> existing_area_ids;
    // Existing combined owners are kept as a whole, never expanded into new
    // per-face definitions. Membership indices are transient graph projections.
    std::vector<std::optional<std::string>> existing_group_ids;
};

// Inspect analytical measured strokes in the selected stroke's context and
// active semantic design phase. View-only eye masks do not change calculations.
// Exact existing definitions are reused, including cyclic/reversed edge order.
// Conflicting definitions and stale source lineage require repair first.
[[nodiscard]] DetectedMeasurementAreas detect_measurement_areas(
    const DocumentSnapshot& source, std::string_view stroke_id);

struct MeasurementAreaDefinition {
    ApplyEntityChanges command;
    // Active definitions in graph order, including reused existing identities.
    std::vector<std::string> area_ids;
    // One entry per detected face for preview rows; combined members share ID.
    std::vector<std::optional<std::string>> face_area_ids;
};

// Pure preview preparation. Graph and existing assignments are recomputed from
// the captured source, never trusted from a UI proposal. Creates boundaries and
// explicit deduction links in one command. Existing facts/style/IDs are kept.
// An empty command is a reference-only/no-change review, not a history entry.
// ANSI appraisal links validate the complete nested partition using immediate
// child gross boundaries, including same-category measured floor/room areas.
// Other workflows retain their existing TYPE and one-level subtraction rules.
[[nodiscard]] MeasurementAreaDefinition prepare_measurement_area_definition(
    const DocumentSnapshot& source, std::string_view stroke_id,
    const std::vector<MeasurementAreaChoice>& choices);

} // namespace sketch
