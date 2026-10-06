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
enum class AppraisalRoomUse { bedroom, bathroom_full, bathroom_half, other,
    breakfast_room, den, dining_room, family_room, kitchen, laundry_room,
    living_room, loft, media_room, mudroom, recreation_room, sunroom,
    utility_room, walk_in_pantry, workshop };
[[nodiscard]] std::string appraisal_room_use_name(AppraisalRoomUse use);
struct AppraisalLivingUnitLevelDeclaration {
    std::string floor_id;
    unsigned level_number{}; // 1..99; below-grade display uses the B prefix.
    std::optional<std::string> grade_level_type;
    std::optional<std::string> below_grade_access;
    std::optional<std::string> exterior_access;
    std::string exterior_access_description;
    std::string source_geometry_sha256;
};
struct AppraisalLivingUnit {
    std::string unit_id; // Stable technical identity, never a display label.
    std::string identifier; // Unique appraiser-assigned free-form label.
    DwellingIdentity role{DwellingIdentity::primary};
    std::vector<AppraisalLivingUnitLevelDeclaration> levels;
};
struct AppraisalReportingSettings {
    AppraisalReportingContract contract{AppraisalReportingContract::uad_3_6};
    bool room_inventory_complete{}; // User assertion, never a geometry proof.
    unsigned version{1};
    std::vector<AppraisalLivingUnit> living_units;
};
struct AppraisalRoomDeclaration {
    std::string room_id;
    AppraisalRoomUse use{AppraisalRoomUse::other};
    std::optional<bool> legacy_total_room;
    std::string other_description; // V2 Other requires an explicit description.
};
struct AppraisalAreaReportingFacts {
    // Current geometry, deductions, owner context and appraisal observations.
    // Persisted V1 field name is retained; reporting/presentation is excluded.
    std::string source_geometry_sha256;
    std::optional<bool> contained_within_primary;
    std::vector<AppraisalRoomDeclaration> rooms;
    unsigned version{1};
    std::optional<std::string> living_unit_id;
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
    std::optional<std::string> living_unit_id;
    std::string other_description;
};
struct AppraisalLivingUnitLevel {
    std::string floor_id;
    GradeStatus grade{GradeStatus::unknown};
    bool noncontinuous{};
    AppraisalRoomCounts counts;
    std::vector<std::string> boundary_ids;
    std::map<AppraisalRoomUse, unsigned> room_types;
    std::map<AppraisalAreaCategory, double> area_fields;
    std::optional<AppraisalLivingUnitLevelDeclaration> declaration;
    bool form_fields_available{};
    std::map<std::string, unsigned> other_room_descriptions;
};
struct AppraisalLivingUnitProjection {
    AppraisalLivingUnit living_unit;
    bool area_fields_available{}, room_counts_available{};
    std::map<AppraisalAreaCategory, double> area_fields;
    AppraisalRoomCounts counts;
    std::vector<std::string> boundary_ids;
    std::vector<AppraisalLivingUnitLevel> levels;
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
    bool individual_units_available{};
    std::vector<AppraisalLivingUnitProjection> living_units;
};
// Strict V1/V2 semantic JSON; absent properties leave old projects in
// measurement-summary mode. These serializers never infer facts from names.
[[nodiscard]] nlohmann::json appraisal_reporting_json(const AppraisalReportingSettings& value);
[[nodiscard]] nlohmann::json appraisal_reporting_json(const AppraisalAreaReportingFacts& value);
[[nodiscard]] AppraisalReportingSettings parse_appraisal_reporting_settings(const nlohmann::json& value);
[[nodiscard]] AppraisalAreaReportingFacts parse_appraisal_area_reporting_facts(const nlohmann::json& value);
[[nodiscard]] std::string appraisal_reporting_source_digest(const DocumentSnapshot& source, const std::string& boundary_id);
[[nodiscard]] std::string appraisal_reporting_level_source_digest(const DocumentSnapshot& source, const std::string& floor_id);
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
