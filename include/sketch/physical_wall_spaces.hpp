#pragma once

#include "sketch/document.hpp"
#include "sketch/measurement_area_graph.hpp"
#include "sketch/project_organization.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace sketch {

struct PhysicalWallSpace {
    std::size_t baseline_face_index{};
    Boundary boundary;
    std::vector<Boundary> holes;
    double area_square_metres{};
    nlohmann::json source_lineage;
};

struct PhysicalWallSpaces {
    DrawingContext context;
    MeasurementAreaGraph graph;
    std::vector<PhysicalWallSpace> spaces;
};

// Pure analytical room discovery in the selected active wall's complete
// drawing context and effective elevation plane. Nodes original wall baselines,
// then subtracts uncut joined physical wall material, including open stubs and
// isolated walls. Hosted openings preserve continuous virtual room dividers.
// Nested baseline faces exclude their island and remain distinct rooms. A face
// can produce several clear components, sorted deterministically. Exact line/
// circle geometry and original parameter lineage are retained; no entities,
// IDs, classification or appraisal facts are produced. Invalid/collapsed,
// ambiguous, unsupported or budget-exceeding geometry throws invalid_argument.
// View visibility never participates. Detection is intended for a snapshot
// cache, not repeated Boolean work during pointer movement.
// Source/Boolean/edge checks are bounded; emitted source lineage is limited
// to 16 MiB encoded across the returned components.
[[nodiscard]] PhysicalWallSpaces detect_physical_wall_spaces(
    const DocumentSnapshot& document, std::string_view selected_wall_id);

// The same source admission and reconstruction for an immutable retained
// revision. Shared by snapshot discovery and document repair/history validation;
// enclosing document state must already satisfy its semantic admission rules.
[[nodiscard]] PhysicalWallSpaces detect_physical_wall_spaces(
    const std::map<std::string, Entity, std::less<>>& entities,
    std::string_view selected_wall_id);

} // namespace sketch
