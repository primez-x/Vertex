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
};

// A revision-bound projection of one property's declared appraisal workflow.
// The report is present only when every participating building boundary is
// qualified. Site boundaries are deliberately outside this projection.
struct AppraisalDocumentReport {
    Revision revision{};
    std::string property_id;
    bool configured{};
    bool qualified{};
    std::optional<AppraisalPolicy> policy;
    std::vector<AppraisalBoundaryStatus> boundaries;
    std::vector<std::string> issues;
    std::optional<AppraisalCalculationReport> calculation;
    unsigned display_decimal_places{2};
};

// Appraisal eligibility always uses the built-in policy. Persisted display
// settings contribute only decimal_places (integer 0..6, default 2); the caller
// chooses the unit. Malformed display settings throw invalid_argument.
[[nodiscard]] CalculationProfile appraisal_display_profile(
    const nlohmann::json& property_properties,
    AreaUnit display_unit = AreaUnit::square_foot);

// visible_entity_ids is a semantic design-phase mask. Presentation filters
// must not be supplied here because hiding an item in the workspace cannot
// change appraisal totals.
[[nodiscard]] AppraisalDocumentReport build_appraisal_document_report(
    const DocumentSnapshot& document, const std::string& property_id,
    AreaUnit display_unit = AreaUnit::square_foot,
    const std::set<std::string, std::less<>>* visible_entity_ids = nullptr);

} // namespace sketch
