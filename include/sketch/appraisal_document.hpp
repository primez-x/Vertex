#pragma once

#include "sketch/calculations.hpp"
#include "sketch/document.hpp"

#include <optional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

enum class AppraisalReportingContract { legacy_uad_2_6, uad_3_6 };
enum class AppraisalRoomUse { bedroom, bathroom_full, bathroom_half, other };
struct AppraisalReportingSettings {
    AppraisalReportingContract contract{AppraisalReportingContract::uad_3_6};
    bool room_inventory_complete{}; // User assertion, never a geometry proof.
};
struct AppraisalRoomDeclaration {
    std::string room_id;
    AppraisalRoomUse use{AppraisalRoomUse::other};
    std::optional<bool> legacy_total_room;
};
struct AppraisalAreaReportingFacts {
    // Current geometry, deductions, owner context and appraisal observations.
    // Persisted V1 field name is retained; reporting/presentation is excluded.
    std::string source_geometry_sha256;
    std::optional<bool> contained_within_primary;
    std::vector<AppraisalRoomDeclaration> rooms;
};
struct AppraisalRoomCounts {
    unsigned total_rooms{}, bedrooms{}, bathrooms_full{}, bathrooms_half{};
};
struct AppraisalReportingRoom {
    std::string room_id, boundary_id, floor_id;
    AppraisalRoomUse use{};
    AppraisalAreaCategory category{};
    DwellingIdentity identity{DwellingIdentity::primary};
    bool included_in_primary_counts{};
};
struct AppraisalReportingEvidence { std::string url, edition, sections; };
struct AppraisalFormProjection {
    AppraisalReportingContract contract{};
    bool configuration_valid{};
    bool area_fields_available{}, room_counts_available{}, room_summaries_available{};
    double primary_above_grade_finished_square_metres{};
    std::map<AppraisalAreaCategory,double> primary_area_fields;
    // Legacy above-grade counts and UAD 3.6 all-grade primary counts.
    AppraisalRoomCounts primary_counts, below_grade_counts, noncontinuous_counts, adu_counts;
    std::vector<AppraisalReportingRoom> rooms;
    std::vector<std::string> issues;
    std::vector<AppraisalReportingEvidence> evidence;
};
// Strict version-one semantic JSON; absent properties leave old projects in
// measurement-summary mode. These serializers never infer facts from names.
[[nodiscard]] nlohmann::json appraisal_reporting_json(const AppraisalReportingSettings& value);
[[nodiscard]] nlohmann::json appraisal_reporting_json(const AppraisalAreaReportingFacts& value);
[[nodiscard]] AppraisalReportingSettings parse_appraisal_reporting_settings(const nlohmann::json& value);
[[nodiscard]] AppraisalAreaReportingFacts parse_appraisal_area_reporting_facts(const nlohmann::json& value);
[[nodiscard]] std::string appraisal_reporting_source_digest(const DocumentSnapshot& source, const std::string& boundary_id);
// Root applies these typed changes in one undoable command, after checking the
// full snapshot fingerprint. The dialog itself never mutates the Document.
struct AppraisalReportingChanges {
    Revision revision{};
    std::string source_document_id, source_snapshot_sha256, property_id;
    AppraisalReportingSettings settings;
    std::vector<std::pair<std::string, AppraisalAreaReportingFacts>> areas;
};
// The optional mask is semantic design-phase scope, never presentation hiding.
void validate_appraisal_reporting_changes(const DocumentSnapshot& source, const AppraisalReportingChanges& changes,
    const std::set<std::string, std::less<>>* visible_entity_ids = nullptr);

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
    std::optional<AppraisalFormProjection> reporting;
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
