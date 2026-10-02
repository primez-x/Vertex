#pragma once

#include "sketch/calculations.hpp"
#include "sketch/document.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sketch {

struct AppraisalBoundaryStatus {
    std::string boundary_id;
    bool exclusion{};
    AppraisalQualification qualification;
    // Current validated geometry, including deductions and presentation rounding.
    // Its presence does not imply qualification or an aggregate contribution.
    // Stale sources and invalid geometry/dependencies never expose a trace.
    std::optional<AreaCalculation> measurement{};
    std::optional<AppraisalFacts> facts;
};

// A revision-bound projection of one property's declared appraisal workflow.
// All in-scope visible building boundaries retain a status, including invalid
// inputs. Aggregate calculation is present only when every participant is
// qualified. Site boundaries are deliberately outside this projection.
struct AppraisalDocumentReport {
    Revision revision{};
    std::string property_id;
    bool configured{};
    bool qualified{};
    // Absent until the property explicitly declares a valid policy kind/version.
    std::optional<AppraisalPolicy> policy;
    std::vector<AppraisalBoundaryStatus> boundaries;
    std::vector<std::string> issues;
    std::optional<AppraisalCalculationReport> calculation;
    unsigned display_decimal_places{2};
    // Provenance for the calculation's actual inputs. Every geometry, hierarchy,
    // policy/fact, display and phase record lives in the entity map. Assets and
    // history are not inputs to this numeric projection.
    std::string source_document_id;
    std::string source_entities_sha256;
    std::optional<AnsiMeasurementDeclarations> ansi_measurement;
    std::vector<std::string> policy_evidence;
    std::vector<std::string> policy_limitations;
};

// Declared-v1 eligibility uses the unchanged built-in policy. Its persisted
// display settings contribute only decimal_places (0..6, default 2). ANSI uses
// the separate canonical whole-square-foot profile regardless of caller unit.
[[nodiscard]] CalculationProfile appraisal_display_profile(
    const nlohmann::json& property_properties,
    AreaUnit display_unit = AreaUnit::square_foot);

// Geometry-only binding for ceiling observations. Sorts deduction IDs and
// includes their shapes; independent of presentation and the evidence itself.
[[nodiscard]] std::string appraisal_ceiling_geometry_digest(
    const Boundary& boundary, const std::vector<AreaDeduction>& deductions);

// visible_entity_ids is a semantic design-phase mask. Presentation filters
// must not be supplied here because hiding an item in the workspace cannot
// change appraisal totals.
[[nodiscard]] AppraisalDocumentReport build_appraisal_document_report(
    const DocumentSnapshot& document, const std::string& property_id,
    AreaUnit display_unit = AreaUnit::square_foot,
    const std::set<std::string, std::less<>>* visible_entity_ids = nullptr);

} // namespace sketch
