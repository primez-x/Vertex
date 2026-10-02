#include "sketch/appraisal_document.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace {

using sketch::Entity;
using nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void near(double actual, double expected, double tolerance, std::string_view message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

Entity entity(std::string id, std::string type, json properties = json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, json::object()};
}

json square(double x, double y, double side) {
    return json::array({
        {{"start", {x, y}}, {"end", {x + side, y}}, {"sweep_radians", 0.0}},
        {{"start", {x + side, y}}, {"end", {x + side, y + side}}, {"sweep_radians", 0.0}},
        {{"start", {x + side, y + side}}, {"end", {x, y + side}}, {"sweep_radians", 0.0}},
        {{"start", {x, y + side}}, {"end", {x, y}}, {"sweep_radians", 0.0}},
    });
}

json policy() {
    return {{"policy_kind", "residential_declared"}, {"version", 1},
            {"property_kind", "detached_single_family"},
            {"measurement_basis", "exterior"}};
}

json facts(std::string use = "dwelling", std::string role = "measured_area") {
    return {{"finish", "finished"}, {"access", "direct_interior"},
            {"ceiling_eligibility", "standard"}, {"area_use", std::move(use)},
            {"boundary_role", std::move(role)}};
}

std::vector<Entity> fixture_entities() {
    return {
        entity("property-1", "property", {{"calculation_workflow", "appraisal"},
                                             {"appraisal_policy", policy()}}),
        entity("building-1", "building", {{"property_id", "property-1"}}),
        entity("floor-1", "floor", {{"building_id", "building-1"},
                                       {"appraisal_facts", {{"grade", "above"}}}}),
        entity("layer-1", "layer", {{"floor_id", "floor-1"}}),
        entity("area-1", "measurement_boundary",
               {{"property_id", "property-1"}, {"building_id", "building-1"},
                {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
                {"boundary", square(0, 0, 3.048)}, {"calculation_scope", "building"},
                {"appraisal_facts", facts()}}),
    };
}

const sketch::AppraisalBoundaryStatus& status(const sketch::AppraisalDocumentReport& report,
                                             std::string_view id) {
    const auto found = std::find_if(report.boundaries.begin(), report.boundaries.end(),
        [&](const auto& value) { return value.boundary_id == id; });
    require(found != report.boundaries.end(), "expected boundary status must be inspectable");
    return *found;
}

void qualified_document_recalculates_from_geometry() {
    auto entities = fixture_entities();
    entities.push_back(entity("room-1", "room_boundary",
        {{"property_id", "property-1"}, {"building_id", "building-1"},
         {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
         {"boundary", square(0.5, 0.5, 1.0)}}));
    const auto document = sketch::Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(
        document.snapshot(), "property-1", sketch::AreaUnit::square_foot);
    require(report.configured && report.qualified && report.calculation.has_value() &&
                report.issues.empty() && report.boundaries.size() == 1,
            "declared appraisal document must qualify without treating room geometry as an appraisal area");
    near(report.calculation->property.gla().total.square_metres, 9.290304, 1e-8,
         "document projection must use authoritative geometry");
    require(report.calculation->property.gla().total.display.text == "100.00" &&
                report.calculation->property.gla().area_ids ==
                    std::vector<std::string>{"area-1"},
            "automatic GLA must retain square-foot output and source provenance");
    require(report.display_decimal_places == 2,
            "legacy appraisal properties must retain two display decimals");
    const auto& trace = status(report, "area-1").measurement;
    require(trace && trace->area_id == "area-1" && trace->building_id == "building-1" &&
                trace->floor_id == "floor-1" && trace->classification == "above_grade_finished" &&
                trace->profile_id == "vertex-appraisal" && trace->profile_version == 1 &&
                trace->factor.numerator == 1 && trace->factor.denominator == 1 &&
                trace->deductions.empty() && trace->display.text == "100.00",
            "qualified status must expose the aggregate's full measured trace and provenance");
    near(trace->base_square_metres, 9.290304, 1e-8, "trace must retain gross geometry");
    near(trace->perimeter_metres, 12.192, 1e-8, "trace must retain analytical perimeter");
    near(trace->net_square_metres, 9.290304, 1e-8, "trace must retain physical net");
    near(trace->factored_square_metres, 9.290304, 1e-8, "unity factor must retain physical amount");
}

void display_precision_rounds_aggregate_from_physical_amounts() {
    constexpr double square_foot_metres = 0.09290304;
    const double side = std::sqrt(1.51 * square_foot_metres);
    for (const auto& [precision, area_text, total_text] :
         std::vector<std::tuple<unsigned, std::string, std::string>>{
             {0, "2", "3"}, {1, "1.5", "3.0"}, {6, "1.510000", "3.020000"}}) {
        auto entities = fixture_entities();
        entities.front().properties["calculation_profile"] = {{"decimal_places", precision}};
        entities.back().properties["boundary"] = square(0, 0, side);
        auto second = entities.back();
        second.id = "area-2";
        second.properties["boundary"] = square(2, 0, side);
        entities.push_back(std::move(second));
        const auto document = sketch::Document::create(std::move(entities));
        const auto before = document.snapshot();
        const auto report = sketch::build_appraisal_document_report(before, "property-1");
        require(report.qualified && report.calculation && report.calculation->calculation.areas.size() == 2,
                "display precision must retain two qualified physical areas");
        const auto& total = report.calculation->property.gla().total;
        near(total.square_metres, 3.02 * square_foot_metres, 1e-10,
             "display precision must preserve the aggregate SI quantity");
        near(total.display.unrounded, 3.02, 1e-9,
             "aggregate must use the unrounded physical areas");
        require(total.display.text == total_text,
                "appraisal totals must honor persisted precision and round the aggregate once");
        require(report.display_decimal_places == precision,
                "report presentation metadata must expose the resolved property precision");
        for (const auto& area : report.calculation->calculation.areas) {
            near(area.net_square_metres, 1.51 * square_foot_metres, 1e-10,
                 "display precision must preserve each physical area");
            require(area.display.text == area_text,
                    "individual appraisal areas must use the same display precision");
            const auto& trace = status(report, area.area_id).measurement;
            require(trace && trace->display.text == area_text &&
                        trace->display.unit == sketch::AreaUnit::square_foot,
                    "boundary traces must honor persisted precision without rounding physical quantities");
            near(trace->display.unrounded, 1.51, 1e-9, "trace display must retain unrounded amount");
            near(trace->display.rounding_delta, trace->display.rounded - 1.51, 1e-9,
                 "trace must expose its presentation rounding delta");
        }
        require(document.snapshot().entities() == before.entities() &&
                    document.snapshot().revision() == before.revision(),
                "reporting must leave the exact document snapshot unchanged");
    }
}

void declaration_and_factor_failures_retain_physical_traces() {
    for (const bool nonunity : {false, true}) {
        auto entities = fixture_entities();
        entities.back().properties["boundary"] = square(0, 0, 3);
        if (nonunity) {
            entities.back().properties["factor_numerator"] = 2;
            entities.back().properties["factor_denominator"] = 3;
        } else entities.back().properties["appraisal_facts"].erase("finish");
        const auto document = sketch::Document::create(std::move(entities));
        const auto before = document.snapshot();
        const auto report = sketch::build_appraisal_document_report(before, "property-1", sketch::AreaUnit::square_metre);
        const auto& boundary = status(report, "area-1");
        require(!report.qualified && !report.calculation && !boundary.qualification.qualified &&
                    !boundary.qualification.derived_category && boundary.measurement &&
                    boundary.measurement->classification == "unqualified",
                "valid physical trace must not qualify missing declarations or an adjusted factor");
        const auto& trace = *boundary.measurement;
        near(trace.base_square_metres, 9, 1e-9, "unqualified trace retains gross area");
        near(trace.perimeter_metres, 12, 1e-9, "unqualified trace retains perimeter");
        near(trace.net_square_metres, 9, 1e-9, "unqualified trace retains physical net");
        near(trace.factored_square_metres, nonunity ? 6 : 9, 1e-9, "unqualified trace exposes adjustment separately");
        require(trace.factor.numerator == (nonunity ? 2 : 1) && trace.factor.denominator == (nonunity ? 3 : 1) &&
                    trace.display.text == (nonunity ? "6.00" : "9.00") &&
                    boundary.qualification.physical_square_metres && boundary.qualification.adjusted_square_metres,
                "exact factor, adjusted display and existing qualification diagnostics must remain inspectable");
        near(*boundary.qualification.physical_square_metres, 9, 1e-9, "qualification physical diagnostic stays unchanged");
        near(*boundary.qualification.adjusted_square_metres, nonunity ? 6 : 9, 1e-9, "qualification adjusted diagnostic stays unchanged");
        require(std::any_of(boundary.qualification.issues.begin(), boundary.qualification.issues.end(),
                    [&](const auto& issue) { return issue.code == (nonunity ? "factor_not_unity" : "undeclared"); }),
                "trace must retain the reason totals were withheld");
        require(document.snapshot().entities() == before.entities(), "trace reporting must remain a pure projection");
    }
}

void overlapping_void_deductions_expose_requested_and_applied_amounts() {
    auto entities = fixture_entities();
    entities.back().properties["boundary"] = square(0, 0, 4);
    entities.back().properties["deduction_ids"] = json::array({"void-b", "void-a"});
    for (const auto& [id, x] : std::vector<std::pair<std::string, double>>{{"void-a", .5}, {"void-b", 1.5}})
        entities.push_back(entity(id, "measurement_boundary",
            {{"property_id", "property-1"}, {"building_id", "building-1"}, {"floor_id", "floor-1"},
             {"layer_id", "layer-1"}, {"boundary", square(x, .5, 2)},
             {"appraisal_facts", {{"boundary_role", "other_void"}}}}));
    const auto document = sketch::Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1", sketch::AreaUnit::square_metre);
    require(report.qualified && report.calculation && report.calculation->calculation.areas.size() == 1 &&
                report.calculation->property.gla().area_ids == std::vector<std::string>{"area-1"},
            "linked void traces must never become standalone aggregate contributions");
    require(status(report, "area-1").measurement.has_value(), "parent must retain its deduction trace");
    const auto& measured = *status(report, "area-1").measurement;
    require(measured.deductions.size() == 2 && measured.deductions[0].id == "void-a" && measured.deductions[1].id == "void-b",
            "deduction trace must expose deterministic marginal attribution independent of input order");
    near(measured.deductions[0].requested_square_metres, 4, 1e-9, "first void requested area");
    near(measured.deductions[0].applied_square_metres, 4, 1e-9, "first void applied area");
    near(measured.deductions[1].requested_square_metres, 4, 1e-9, "overlapping void requested area");
    near(measured.deductions[1].applied_square_metres, 2, 1e-9, "overlapping void applies only remaining area");
    near(measured.deducted_square_metres, 6, 1e-9, "overlap must be deducted only once");
    near(measured.net_square_metres, 10, 1e-9, "parent trace retains physical net after union deductions");
    near(report.calculation->property.gla().total.square_metres, 10, 1e-9, "void traces do not inflate GLA");
    for (const auto* id : {"void-a", "void-b"}) {
        const auto& excluded = status(report, id);
        require(excluded.exclusion && excluded.qualification.qualified && excluded.measurement &&
                    excluded.measurement->display.text == "4.00", "each linked exclusion remains independently inspectable");
        near(excluded.measurement->net_square_metres, 4, 1e-9, "void's own physical trace remains complete");
    }
}

void exclusion_only_report_retains_measurement_without_totals() {
    auto entities = fixture_entities();
    entities.back().properties["appraisal_facts"] = {{"boundary_role", "other_void"}};
    const auto document = sketch::Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
    const auto& excluded = status(report, "area-1");
    require(!report.qualified && !report.calculation && excluded.exclusion &&
                excluded.qualification.qualified && excluded.measurement && excluded.measurement->display.text == "100.00",
            "an unlinked exclusion remains measurable but cannot supply property totals");
    near(excluded.measurement->net_square_metres, 9.290304, 1e-8, "exclusion-only report retains valid physical geometry");
    require(std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
        return issue.find("exclusion must be linked") != std::string::npos;
    }), "individual measurement cannot bypass the exclusion linkage requirement");
}

void global_overlap_withholds_totals_but_retains_individual_traces() {
    auto entities = fixture_entities();auto second = entities.back();second.id = "area-2";
    second.properties["boundary"] = square(1, 0, 3.048);entities.push_back(std::move(second));
    const auto document = sketch::Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
    require(!report.qualified && !report.calculation && std::any_of(report.issues.begin(), report.issues.end(),
        [](const auto& issue) { return issue.find("overlap") != std::string::npos; }), "same-floor overlap must withhold property totals");
    for (const auto* id : {"area-1", "area-2"}) {
        const auto& boundary = status(report, id);
        require(boundary.qualification.qualified && boundary.measurement && boundary.measurement->display.text == "100.00",
            "global aggregation failure must retain valid individual qualification and measurement");
        near(boundary.measurement->net_square_metres, 9.290304, 1e-8, "global overlap must not corrupt individual geometry");
    }
}

void stale_sources_and_invalid_dependencies_expose_no_current_trace() {
    auto entities = fixture_entities();
    std::vector<std::string> ids;
    for (const auto& edge : square(0, 0, 3.048)) {
        const auto id = "wall-" + std::to_string(ids.size());ids.push_back(id);
        entities.push_back(entity(id, "wall", {{"baseline", edge}, {"thickness_m", .2}, {"height_m", 3}, {"elevation_m", 0},
            {"property_id", "property-1"}, {"building_id", "building-1"}, {"floor_id", "floor-1"}, {"layer_id", "layer-1"}}));
    }
    const auto walls = sketch::Document::create(entities);
    const auto derived = sketch::derive_exterior_wall_measurement(walls.snapshot(), ids);
    auto& area = entities[4];area.properties["boundary"] = json::array();
    for (const auto& edge : derived.boundary) area.properties["boundary"].push_back(
        {{"start", {edge.start.x, edge.start.y}}, {"end", {edge.end.x, edge.end.y}}, {"sweep_radians", edge.sweep_radians}});
    area.properties["wall_measurement_source"] = derived.source;
    const auto current = sketch::Document::create(entities);
    require(status(sketch::build_appraisal_document_report(current.snapshot(), "property-1"), "area-1").measurement.has_value(),
        "current wall-derived outline must expose an inspectable trace");
    std::set<std::string, std::less<>> visible;
    for (const auto& item : entities) if (item.id != ids.front()) visible.insert(item.id);
    const auto phase = sketch::build_appraisal_document_report(current.snapshot(), "property-1", sketch::AreaUnit::square_foot, &visible);
    require(!phase.qualified && !phase.calculation && !status(phase, "area-1").measurement,
        "phase-hidden source wall must not expose stale numeric measurement");
    entities[5].properties["thickness_m"] = .4;
    const auto stale = sketch::Document::create(entities);
    const auto report = sketch::build_appraisal_document_report(stale.snapshot(), "property-1");
    const auto& invalid = status(report, "area-1");
    require(!report.qualified && !report.calculation && !invalid.measurement && !invalid.qualification.qualified &&
        !invalid.qualification.physical_square_metres && !invalid.qualification.adjusted_square_metres,
        "stale wall source must never advertise numeric traces or physical diagnostics as current");
    auto parent = fixture_entities().back();parent.id = "parent-area";
    parent.properties["boundary"] = square(-1, -1, 6);
    parent.properties["deduction_ids"] = json::array({"area-1"});
    entities[4].properties["appraisal_facts"] = {{"boundary_role", "other_void"}};
    entities.push_back(std::move(parent));
    const auto stale_child = sketch::Document::create(std::move(entities));
    const auto child_report = sketch::build_appraisal_document_report(stale_child.snapshot(), "property-1");
    require(!child_report.qualified && !child_report.calculation && !status(child_report, "area-1").measurement &&
        std::none_of(child_report.boundaries.begin(), child_report.boundaries.end(), [](const auto& boundary) {
            return boundary.boundary_id == "parent-area" && boundary.measurement.has_value();
        }) && std::any_of(child_report.issues.begin(), child_report.issues.end(), [](const auto& issue) {
            return issue.find("deduction area-1") != std::string::npos && issue.find("stale") != std::string::npos;
        }), "a stale deduction dependency must not yield plausible parent physical net");
    for (const bool bad_geometry : {false, true}) {
        auto broken = fixture_entities();
        if (bad_geometry) broken.back().properties["boundary"][0]["sweep_radians"] = "bad";
        else broken.back().properties["deduction_ids"] = json::array({"missing-deduction"});
        const auto document = sketch::Document::create(std::move(broken));
        const auto failure = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
        const auto& boundary = status(failure, "area-1");
        require(!failure.qualified && !failure.calculation && !failure.issues.empty() && !boundary.measurement &&
            !boundary.qualification.qualified && !boundary.qualification.derived_category &&
            !boundary.qualification.physical_square_metres && !boundary.qualification.adjusted_square_metres &&
            !boundary.qualification.issues.empty(),
            "malformed candidates must retain an actionable source row without plausible numeric qualification");
    }
}

void missing_policy_retains_diagnostics_without_inventing_declared_policy() {
    auto entities = fixture_entities();entities.front().properties.erase("appraisal_policy");
    entities.back().properties["factor_numerator"] = 1;entities.back().properties["factor_denominator"] = 2;
    entities.back().properties["deduction_ids"] = json::array({"void-1"});
    entities.push_back(entity("void-1", "measurement_boundary",
        {{"property_id", "property-1"}, {"building_id", "building-1"}, {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
         {"boundary", square(1,1,1)}, {"appraisal_facts", {{"boundary_role", "other_void"}}}}));
    const auto document = sketch::Document::create(std::move(entities));const auto before = document.snapshot();
    const auto report = sketch::build_appraisal_document_report(before, "property-1", sketch::AreaUnit::square_metre);
    require(report.configured && !report.qualified && !report.calculation && !report.policy && report.boundaries.size() == 2,
        "absent property policy must retain source rows but never infer declared policy or qualified totals");
    const auto& parent = status(report, "area-1");
    require(parent.measurement && !parent.qualification.qualified && !parent.qualification.derived_category &&
        parent.qualification.policy_id.empty() && parent.qualification.policy_version == 0 &&
        parent.qualification.physical_square_metres && parent.qualification.adjusted_square_metres &&
        parent.measurement->factor.numerator == 1 && parent.measurement->factor.denominator == 2 &&
        parent.measurement->classification == "unqualified" && parent.measurement->display.text == "4.15",
        "missing policy leaves valid physical/adjusted geometry diagnostic with exact factor and rounding");
    near(parent.measurement->base_square_metres, 9.290304, 1e-8, "missing-policy trace gross area");
    near(parent.measurement->deducted_square_metres, 1, 1e-8, "missing-policy trace retains linked deduction");
    near(parent.measurement->net_square_metres, 8.290304, 1e-8, "missing-policy trace physical net");
    near(parent.measurement->factored_square_metres, 4.145152, 1e-8, "missing-policy trace adjusted amount");
    require(std::any_of(parent.qualification.issues.begin(), parent.qualification.issues.end(), [](const auto& issue) {
        return issue.code == "undeclared_policy";
    }) && std::any_of(parent.qualification.issues.begin(), parent.qualification.issues.end(), [](const auto& issue) {
        return issue.code == "factor_not_unity";
    }), "diagnostic trace must retain undeclared-policy and exact-unity blockers");
    const auto& excluded = status(report, "void-1");
    require(excluded.exclusion && excluded.measurement && !excluded.qualification.qualified &&
        !excluded.qualification.derived_category && excluded.qualification.policy_id.empty(),
        "missing-policy void remains inspectable without positive qualification or standalone contribution");
    require(document.snapshot().entities() == before.entities() && document.snapshot().revision() == before.revision(),
        "diagnostic projection must not insert a default policy into authoritative data");

    for (const auto& partial : std::vector<json>{json::object(), {{"version",1}}, {{"policy_kind","residential_declared"}}}) {
        auto incomplete = fixture_entities();incomplete.front().properties["appraisal_policy"] = partial;
        const auto partial_document = sketch::Document::create(std::move(incomplete));
        const auto partial_report = sketch::build_appraisal_document_report(partial_document.snapshot(), "property-1");
        require(!partial_report.policy && !partial_report.qualified && !partial_report.calculation &&
            status(partial_report, "area-1").measurement && !status(partial_report, "area-1").qualification.qualified,
            "incomplete policy kind/version must not be filled from defaults");
    }
}

void malformed_candidates_have_source_rows_without_numeric_traces() {
    for (int fault = 0; fault < 11; ++fault) {
        auto entities = fixture_entities();
        switch (fault) {
        case 0: entities.front().properties["appraisal_policy"] = "bad"; break;
        case 1: entities.front().properties["appraisal_policy"]["version"] = 2; break;
        case 2: entities.front().properties["appraisal_policy"]["policy_kind"] = "bad"; break;
        case 3: entities.back().properties["boundary"][0]["sweep_radians"] = "bad"; break;
        case 4: entities.back().properties["deduction_ids"] = json::array({"missing-void"}); break;
        // Document rejects malformed/dangling canonical references before a
        // snapshot exists. Missing context and conflicting valid references
        // exercise the report's ownership diagnostics through supported input.
        case 5: entities.back().properties.erase("floor_id"); break;
        case 6: entities[2].properties.erase("building_id"); break;
        case 7:
            entities.push_back(entity("building-other", "building", {{"property_id","property-1"}}));
            entities[4].properties["building_id"] = "building-other";
            break;
        case 8: entities.back().properties["appraisal_facts"]["finish"] = "bad"; break;
        case 9: entities.back().properties["boundary"][0]["end"] = json::array({3.048,3.048}); break;
        case 10: entities.front().properties["calculation_profile"] = {{"decimal_places",7}}; break;
        }
        const auto document = sketch::Document::create(std::move(entities));
        const auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
        const auto& boundary = status(report, "area-1");
        require(!report.qualified && !report.calculation && report.boundaries.size() == 1 && !boundary.measurement &&
            !boundary.qualification.qualified && !boundary.qualification.derived_category &&
            !boundary.qualification.physical_square_metres && !boundary.qualification.adjusted_square_metres &&
            !boundary.qualification.issues.empty() && !boundary.qualification.issues.front().message.empty(),
            "each invalid context/declaration/geometry/dependency retains its source ID with actionable issues and no numeric trace");
        require(std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {return issue.find("area-1:") != std::string::npos;}),
            "global issue list must identify the same invalid source");
        if (fault < 3) require(!report.policy, "malformed policy must not advertise a default declared policy");
    }
}

void report_excludes_other_property_site_and_phase_hidden_boundaries() {
    auto entities = fixture_entities();
    entities.push_back(entity("property-2", "property", {{"calculation_workflow","appraisal"},{"appraisal_policy",policy()}}));
    entities.push_back(entity("building-2", "building", {{"property_id","property-2"}}));
    entities.push_back(entity("floor-2", "floor", {{"building_id","building-2"},{"appraisal_facts",{{"grade","above"}}}}));
    auto other = entities[4];other.id = "other-area";other.properties["property_id"] = "property-2";
    other.properties["building_id"] = "building-2";other.properties["floor_id"] = "floor-2";
    other.properties["boundary"] = "malformed geometry outside requested property";entities.push_back(std::move(other));
    auto site = entities[4];site.id = "site-area";site.properties["calculation_scope"] = "site";
    site.properties.erase("floor_id");site.properties["boundary"] = "not building geometry";entities.push_back(std::move(site));
    auto hidden = entities[4];hidden.id = "hidden-area";hidden.properties["appraisal_facts"] = "bad";entities.push_back(std::move(hidden));
    std::set<std::string,std::less<>> visible;for (const auto& value : entities) if (value.id != "hidden-area") visible.insert(value.id);
    const auto document = sketch::Document::create(entities);
    const auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1", sketch::AreaUnit::square_foot, &visible);
    require(report.qualified && report.calculation && report.boundaries.size() == 1 && report.boundaries.front().boundary_id == "area-1" && report.issues.empty(),
        "valid other-property hierarchy, site outlines and phase-hidden sources are outside the appraisal report even with irrelevant malformed data");
    auto conflicting = entities[4];conflicting.id = "conflicting-area";conflicting.properties["property_id"] = "property-2";entities.push_back(std::move(conflicting));
    visible.insert("conflicting-area");const auto conflict_document = sketch::Document::create(std::move(entities));
    const auto conflict = sketch::build_appraisal_document_report(conflict_document.snapshot(), "property-1", sketch::AreaUnit::square_foot, &visible);
    require(!conflict.qualified && !conflict.calculation && !status(conflict, "conflicting-area").measurement &&
        !status(conflict, "conflicting-area").qualification.qualified,
        "conflicting explicit property and owning hierarchy must appear invalid instead of being silently assigned elsewhere");
}

void display_profile_preserves_fixed_appraisal_semantics() {
    require(sketch::appraisal_display_profile(json::object()).decimal_places == 2 &&
                sketch::appraisal_display_profile({{"calculation_profile", json::object()}})
                    .decimal_places == 2,
            "absent decimal settings must retain the legacy default");
    const auto builtin = sketch::builtin_appraisal_profile();
    for (const auto& value : std::vector<json>{0, 1, 6, std::uint64_t{0}, std::uint64_t{6}}) {
        const json properties = {{"calculation_profile",
            {{"decimal_places", value}, {"id", "custom"}, {"version", 42},
             {"display_unit", "acre"}, {"classifications", {{"garage", "living"}}}}},
            {"vendor_data", {{"untouched", true}}}};
        const auto before = properties;
        const auto profile = sketch::appraisal_display_profile(properties, sketch::AreaUnit::square_metre);
        require(profile.decimal_places == value.get<unsigned>() &&
                    profile.display_unit == sketch::AreaUnit::square_metre &&
                    profile.id == builtin.id && profile.version == builtin.version &&
                    profile.classifications.size() == builtin.classifications.size(),
                "appraisal display settings may change only precision and the caller's display unit");
        for (const auto& [name, rule] : builtin.classifications) {
            const auto& actual = profile.classifications.at(name);
            require(actual.building_total == rule.building_total &&
                        actual.living_total == rule.living_total &&
                        actual.appraisal_category == rule.appraisal_category,
                    "stored profile rules must not override appraisal eligibility");
        }
        require(properties == before, "resolving display settings must preserve source metadata");
    }
}

void display_precision_cannot_qualify_facts_or_change_eligibility() {
    for (const unsigned precision : {0u, 1u, 6u}) {
        auto entities = fixture_entities();
        entities.front().properties["calculation_profile"] =
            {{"decimal_places", precision}, {"id", "custom"}, {"version", 42},
             {"classifications", {{"garage", {{"living_total", true}}}}}};
        entities.back().properties["appraisal_facts"] = facts("garage");
        auto document = sketch::Document::create(entities);
        auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
        require(report.qualified && report.calculation && report.boundaries.size() == 1 &&
                    report.boundaries.front().qualification.derived_category ==
                        sketch::AppraisalAreaCategory::garage &&
                    report.calculation->calculation.profile_id == "vertex-appraisal" &&
                    report.calculation->calculation.profile_version == 1,
                "precision must retain fixed appraisal category and policy provenance");
        near(report.calculation->property.gla().total.square_metres, 0.0, 1e-10,
             "a display profile must not count garage area as GLA");
        near(report.calculation->property.by_category.at(sketch::AppraisalAreaCategory::garage)
                 .total.square_metres, 9.290304, 1e-8,
             "precision must preserve the physical garage contribution");
        entities.back().properties["appraisal_facts"].erase("finish");
        document = sketch::Document::create(std::move(entities));
        report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
        require(report.configured && !report.qualified && !report.calculation &&
                    report.display_decimal_places == precision &&
                    std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                        return issue.find("Declare finish") != std::string::npos;
                    }),
                "valid display precision must not qualify undeclared appraisal facts");
    }
}

void malformed_display_configuration_withholds_totals() {
    std::vector<json> configurations;
    for (const auto& value : std::vector<json>{0.0, 2.0, 1.5, true, false, nullptr, -1, 7,
             std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::uint64_t>::max(), "2"})
        configurations.push_back({{"decimal_places", value}});
    for (const auto& value : std::vector<json>{nullptr, false, 2, "profile", json::array()})
        configurations.push_back(value);
    for (const auto& configuration : configurations) {
        auto entities = fixture_entities();
        entities.front().properties["calculation_profile"] = configuration;
        bool rejected = false;
        try {
            (void)sketch::appraisal_display_profile(entities.front().properties);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "malformed display settings must throw invalid_argument at the helper boundary");
        const auto document = sketch::Document::create(std::move(entities));
        const auto before = document.snapshot();
        const auto report = sketch::build_appraisal_document_report(before, "property-1");
        require(report.configured && !report.qualified && !report.calculation &&
                    std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                        return issue.find("Area display") != std::string::npos;
                    }),
                "malformed display settings must visibly withhold appraisal totals without fallback");
        require(document.snapshot().entities() == before.entities(),
                "malformed settings must not mutate the source document");
    }
}

void deductions_partition_categories_without_double_counting() {
    auto entities = fixture_entities();
    entities.back().properties["deduction_ids"] = std::vector<std::string>{"garage-1"};
    entities.push_back(entity("garage-1", "measurement_boundary",
        {{"property_id", "property-1"}, {"building_id", "building-1"},
         {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
         {"boundary", square(1, 1, 1)}, {"calculation_scope", "building"},
         {"appraisal_facts", facts("garage")}}));
    const auto document = sketch::Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
    require(report.qualified && report.calculation.has_value(),
            "categorized internal garage must retain a qualified report");
    near(report.calculation->property.gla().total.square_metres, 8.290304, 1e-8,
         "garage deduction must reduce GLA");
    near(report.calculation->property.by_category.at(sketch::AppraisalAreaCategory::garage)
             .total.square_metres, 1.0, 1e-8,
         "garage must contribute exactly once to its category");
}

void explicit_void_roles_do_not_require_dwelling_facts() {
    for (const auto* role : {"open_to_below","stair_footprint","other_void"}) {
        auto entities=fixture_entities();
        entities.back().properties["deduction_ids"]=std::vector<std::string>{"void-1"};
        entities.push_back(entity("void-1","measurement_boundary",
            {{"property_id","property-1"},{"building_id","building-1"},{"floor_id","floor-1"},{"layer_id","layer-1"},
             {"boundary",square(1,1,1)},{"appraisal_facts",{{"boundary_role",role}}}}));
        auto document=sketch::Document::create(entities);
        auto report=sketch::build_appraisal_document_report(document.snapshot(),"property-1");
        require(report.qualified && report.calculation && report.boundaries.size()==2,
            "explicit linked void must qualify without fabricated dwelling facts");
        near(report.calculation->property.gla().total.square_metres,8.290304,1e-8,
            "role-only void must subtract physical area without a standalone contribution");
        entities.back().properties["appraisal_facts"]["boundary_role"]="measured_area";
        document=sketch::Document::create(entities);
        report=sketch::build_appraisal_document_report(document.snapshot(),"property-1");
        require(!report.qualified && !report.calculation,"measured areas still require their declared dwelling facts");
        entities.back().properties["appraisal_facts"].erase("boundary_role");
        document=sketch::Document::create(entities);
        report=sketch::build_appraisal_document_report(document.snapshot(),"property-1");
        require(!report.qualified && !report.calculation,"a missing void role must not qualify implicitly");
        entities.back().properties["appraisal_facts"]["boundary_role"]=role;
        entities.back().properties["appraisal_facts"]["finish"]="bad-token";
        document=sketch::Document::create(entities);
        report=sketch::build_appraisal_document_report(document.snapshot(),"property-1");
        require(!report.qualified && !report.calculation,"supplied malformed void facts must remain invalid");
    }
}

void incomplete_or_hidden_facts_withhold_totals() {
    auto entities = fixture_entities();
    entities[2].properties.erase("appraisal_facts");
    auto document = sketch::Document::create(std::move(entities));
    auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
    require(report.configured && !report.qualified && !report.calculation.has_value() &&
                std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                    return issue.find("Declare grade") != std::string::npos;
                }),
            "missing declarations must explicitly withhold automatic totals");

    entities = fixture_entities();
    entities.back().properties["deduction_ids"] = std::vector<std::string>{"void-1"};
    entities.push_back(entity("void-1", "measurement_boundary",
        {{"property_id", "property-1"}, {"building_id", "building-1"},
         {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
         {"boundary", square(1, 1, 0.5)}, {"calculation_scope", "building"},
         {"appraisal_facts", facts("dwelling", "open_to_below")}}));
    document = sketch::Document::create(std::move(entities));
    const std::set<std::string, std::less<>> visible{
        "property-1", "building-1", "floor-1", "layer-1", "area-1"};
    report = sketch::build_appraisal_document_report(
        document.snapshot(), "property-1", sketch::AreaUnit::square_foot, &visible);
    require(!report.qualified && !report.calculation.has_value() &&
                std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                    return issue.find("hidden by the active design phase") != std::string::npos;
                }),
            "a hidden semantic deduction must make the report visibly unqualified");
}

void inconsistent_container_identity_withholds_totals() {
    auto entities = fixture_entities();
    entities.push_back(entity("building-other", "building",
                              {{"property_id", "property-1"}}));
    auto& boundary = *std::find_if(entities.begin(), entities.end(), [](const auto& item) {
        return item.id == "area-1";
    });
    boundary.properties["building_id"] = "building-other";
    const auto document = sketch::Document::create(std::move(entities));
    const auto report = sketch::build_appraisal_document_report(
        document.snapshot(), "property-1");
    require(!report.qualified && !report.calculation.has_value() &&
                std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                    return issue.find("building_id disagrees with its floor") != std::string::npos;
                }),
            "conflicting boundary/floor ownership must withhold appraisal totals");
}

void malformed_projection_data_withholds_totals() {
    auto entities = fixture_entities();
    entities.back().properties["appraisal_facts"] = "malformed";
    auto document = sketch::Document::create(std::move(entities));
    auto report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
    require(!report.qualified && !report.calculation.has_value() &&
                std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                    return issue.find("appraisal_facts must be an object") != std::string::npos;
                }),
            "malformed appraisal declarations must become visible qualification issues");

    entities = fixture_entities();
    entities.back().properties["boundary"][0]["sweep_radians"] = "malformed";
    document = sketch::Document::create(std::move(entities));
    report = sketch::build_appraisal_document_report(document.snapshot(), "property-1");
    require(!report.qualified && !report.calculation.has_value() &&
                std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                    return issue.find("boundary sweep must be numeric") != std::string::npos;
                }),
            "malformed legacy geometry must become a visible qualification issue");
}

} // namespace

int main() {
    try {
        qualified_document_recalculates_from_geometry();
        declaration_and_factor_failures_retain_physical_traces();
        overlapping_void_deductions_expose_requested_and_applied_amounts();
        exclusion_only_report_retains_measurement_without_totals();
        global_overlap_withholds_totals_but_retains_individual_traces();
        stale_sources_and_invalid_dependencies_expose_no_current_trace();
        missing_policy_retains_diagnostics_without_inventing_declared_policy();
        malformed_candidates_have_source_rows_without_numeric_traces();
        report_excludes_other_property_site_and_phase_hidden_boundaries();
        display_precision_rounds_aggregate_from_physical_amounts();
        display_profile_preserves_fixed_appraisal_semantics();
        display_precision_cannot_qualify_facts_or_change_eligibility();
        malformed_display_configuration_withholds_totals();
        deductions_partition_categories_without_double_counting();
        explicit_void_roles_do_not_require_dwelling_facts();
        incomplete_or_hidden_facts_withhold_totals();
        inconsistent_container_identity_withholds_totals();
        malformed_projection_data_withholds_totals();
        std::cout << "appraisal_document_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "appraisal_document_tests: " << error.what() << '\n';
        return 1;
    }
}
