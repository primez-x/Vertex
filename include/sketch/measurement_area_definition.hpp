#pragma once

#include "sketch/measurement_area_graph.hpp"
#include "sketch/project_organization.hpp"

namespace sketch {

enum class MeasurementAreaDisposition { reference_only, define_area, deduct_from_parent };

struct MeasurementAreaChoice {
    MeasurementAreaDisposition disposition{MeasurementAreaDisposition::reference_only};
    std::string classification;
};

struct DetectedMeasurementAreas {
    DrawingContext context;
    MeasurementAreaGraph graph;
    std::vector<std::optional<std::string>> existing_area_ids;
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
};

// Pure preview preparation. Graph and existing assignments are recomputed from
// the captured source, never trusted from a UI proposal. Creates boundaries and
// explicit deduction links in one command. Existing facts/style/IDs are kept.
// An empty command is a reference-only/no-change review, not a history entry.
// Subtraction uses the owning workflow's existing TYPE and containment rules.
[[nodiscard]] MeasurementAreaDefinition prepare_measurement_area_definition(
    const DocumentSnapshot& source, std::string_view stroke_id,
    const std::vector<MeasurementAreaChoice>& choices);

} // namespace sketch
