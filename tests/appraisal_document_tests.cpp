#include "sketch/appraisal_document.hpp"
#include "sketch/document_digest.hpp"
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

sketch::Boundary rectangle_geometry(double x, double y, double w, double h) {
    return {{{x, y}, {x + w, y}, 0}, {{x + w, y}, {x + w, y + h}, 0},
        {{x + w, y + h}, {x, y + h}, 0}, {{x, y + h}, {x, y}, 0}};
}

std::vector<Entity> ansi_fixture_entities() {
    auto entities = fixture_entities();
    entities.front().properties["appraisal_policy"]["policy_kind"] = "ansi_z765_2021";
    entities.front().properties["appraisal_policy"]["ansi"] = {
        {"interior_inspected", true}, {"direct_measurement", true}, {"acquisition_increment", "inch"}};
    entities[2].properties["appraisal_facts"]["ansi"] = {{"any_part_below_grade", false}};
    entities.back().properties["appraisal_facts"].erase("ceiling_eligibility");
    entities.back().properties["appraisal_facts"]["ansi"] = {
        {"year_round_suitable", true}, {"finish_matches_dwelling", true}, {"dwelling_identity", "primary"},
        {"ceiling", {{"kind", "flat"}, {"minimum_height_m", 2.1336}}}};
    return entities;
}

void ansi_declarations_and_canonical_reporting() {
    auto entities = ansi_fixture_entities();
    entities.front().properties["calculation_profile"] = {{"decimal_places", 6}};
    auto report = sketch::build_appraisal_document_report(sketch::Document::create(entities).snapshot(), "property-1", sketch::AreaUnit::square_metre);
    require(report.qualified && report.calculation && report.policy && report.policy->kind == sketch::AppraisalPolicyKind::ansi_z765_2021 &&
        report.ansi_measurement && report.policy_evidence.size() == 3 && !report.policy_limitations.empty(),
        "ANSI report must expose actual declarations, public evidence and verification limits");
    require(report.display_decimal_places == 0 && report.calculation->property.gla().total.display.text == "100" &&
        report.calculation->property.gla().total.display.unit == sketch::AreaUnit::square_foot && status(report, "area-1").facts,
        "ANSI canonical whole-square-foot report remains independent of metric diagnostic caller and persisted decimals");
    auto changed = entities;
    changed.front().properties["appraisal_policy"]["ansi"].erase("interior_inspected");
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified && status(report, "area-1").measurement, "Missing ANSI declaration retains valid physical diagnostic but blocks totals");
    changed = entities;
    changed.front().properties["appraisal_policy"]["ansi"]["direct_measurement"] = "yes";
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified && !status(report, "area-1").measurement, "Malformed boolean evidence cannot acquire a plausible trace");
    changed = entities;
    changed.front().properties["appraisal_policy"]["measurement_basis"] = "plans";
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified, "Plans require a limitations declaration");
    changed.front().properties["appraisal_policy"]["ansi"]["limitations_statement"] = "Measured from supplied building plans.";
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified, "Supplemental generic notes cannot replace a condition-specific plans declaration");
    changed.front().properties["appraisal_policy"]["ansi"]["limitation_declarations"] = json::array({
        {{"kind", "based_on_plans"}, {"statement", "Measured from supplied building plans."}}});
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(report.qualified, "Explicit plans limitations can satisfy rule checks");
    auto bad_declaration=changed;
    bad_declaration.front().properties["appraisal_policy"]["ansi"]["limitation_declarations"][0].erase("statement");
    report=sketch::build_appraisal_document_report(sketch::Document::create(bad_declaration).snapshot(),"property-1");
    require(!report.qualified,"Missing typed declaration statement cannot pass the document parser");
    changed = entities;
    changed[2].properties["appraisal_facts"]["ansi"]["any_part_below_grade"] = true;
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified, "Grade contradiction is actionable");
    changed[2].properties["appraisal_facts"].erase("grade");
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(report.qualified && report.calculation->property.gla().total.square_metres == 0 &&
        report.calculation->property.by_category.at(sketch::AppraisalAreaCategory::below_grade_finished).total.square_metres > 0,
        "Whole-floor below-grade fact replaces arbitrary elevation inference");
    changed = entities;
    changed.front().properties["appraisal_policy"]["policy_kind"] = "residential_declared";
    changed.back().properties["appraisal_facts"]["ceiling_eligibility"] = "standard";
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(report.qualified && !report.ansi_measurement && report.calculation->calculation.profile_id == "vertex-appraisal",
        "Switching legacy preserves stored ANSI evidence without changing declared-v1 qualification");
    changed.back().properties["appraisal_facts"].erase("ceiling_eligibility");
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified, "Stored ANSI evidence cannot substitute for missing strict legacy declarations");
}

void ansi_flat_ceiling_precision_changes_contributions() {
    using Category = sketch::AppraisalAreaCategory;
    struct HeightCase { double feet; const char* increment; bool standard; };
    const HeightCase cases[]{
        {6.96, "tenth_foot", true}, {6.85, "tenth_foot", false},
        {6.96, "inch", true}, {6.85, "inch", false},
        {6.951, "tenth_foot", true}, {6.951, "inch", false}};
    for (const auto& item : cases) {
        auto entities = ansi_fixture_entities();
        const double observed = item.feet * 0.3048;
        entities.front().properties["appraisal_policy"]["ansi"]["acquisition_increment"] = item.increment;
        entities.back().properties["appraisal_facts"]["ansi"]["ceiling"]["minimum_height_m"] = observed;
        const auto document = sketch::Document::create(std::move(entities));
        const auto before = document.snapshot();
        const auto report = sketch::build_appraisal_document_report(before, "property-1");
        const auto& boundary = status(report, "area-1");
        const auto category = item.standard ? Category::above_grade_finished : Category::above_grade_nonstandard_finished;
        require(report.qualified && report.calculation && report.ansi_measurement &&
            boundary.qualification.qualified && boundary.qualification.derived_category == category &&
            boundary.facts && boundary.facts->ansi,
            "Declared acquisition precision must determine the flat-height contribution category");
        near(report.calculation->property.gla().total.square_metres, item.standard ? 9.290304 : 0.0, 1e-8,
            "Rounded standard ceilings contribute to GLA and nonstandard ceilings contribute zero GLA");
        near(report.calculation->property.by_category.at(category).total.square_metres, 9.290304, 1e-8,
            "Ceiling classification must retain the complete physical area in its derived category");
        near(*boundary.qualification.physical_square_metres, 9.290304, 1e-8,
            "Acquisition height rounding must not alter measured geometry");
        require(boundary.facts->ansi->ceiling.minimum_height_m == observed &&
            sketch::acquisition_increment_name(*report.ansi_measurement->acquisition_increment) == item.increment &&
            document.snapshot().entities() == before.entities(),
            "Reporting must retain the original height and declared increment without rewriting source facts");
    }
    auto entities = ansi_fixture_entities();
    entities.back().properties["appraisal_facts"]["ansi"]["ceiling"]["minimum_height_m"] = 6.96 * 0.3048;
    entities.front().properties["appraisal_policy"]["ansi"].erase("acquisition_increment");
    const auto report = sketch::build_appraisal_document_report(sketch::Document::create(entities).snapshot(), "property-1");
    require(!report.qualified && !report.calculation && status(report, "area-1").measurement &&
        !status(report, "area-1").qualification.derived_category,
        "A valid low raw height with missing acquisition precision retains diagnostics while withholding totals");
    for (const auto& invalid : std::vector<json>{-1.0, nullptr, "6.96"}) {
        auto malformed = ansi_fixture_entities();
        malformed.back().properties["appraisal_facts"]["ansi"]["ceiling"]["minimum_height_m"] = invalid;
        const auto invalid_report = sketch::build_appraisal_document_report(
            sketch::Document::create(std::move(malformed)).snapshot(), "property-1");
        require(!invalid_report.qualified && !invalid_report.calculation &&
            !status(invalid_report, "area-1").qualification.derived_category,
            "Malformed stored height observations cannot create an eligible contribution");
    }
}

void ansi_finished_room_v2_uses_current_exclusion_geometry() {
    auto entities = ansi_fixture_entities();
    entities.front().properties["appraisal_policy"]["version"] = 2;
    auto& room = entities.back();
    room.properties["boundary"] = square(0, 0, 10);
    room.properties["deduction_ids"] = {"low-v2"};
    room.properties["appraisal_facts"]["ansi"]["ceiling"] = {
        {"kind", "sloped"}, {"at_least_7ft_area_m2", 35}, {"room_floor_area_m2", 100},
        {"complete_room_observed", true}, {"below_5ft_deduction_ids", {"low-v2"}},
        {"room_boundary_id", room.id}, {"source_geometry_sha256", sketch::appraisal_ceiling_geometry_digest(
            rectangle_geometry(0, 0, 10, 10), {{"low-v2", rectangle_geometry(1, 1, 6, 6)}})}};
    auto low = room;
    low.id = "low-v2";
    low.properties["boundary"] = square(1, 1, 6);
    low.properties.erase("deduction_ids");
    low.properties["appraisal_facts"] = {{"boundary_role", "other_void"}};
    entities.push_back(low);
    const auto snapshot = sketch::Document::create(entities).snapshot();
    auto report = sketch::build_appraisal_document_report(snapshot, "property-1");
    require(report.qualified && report.calculation && report.policy->version == 2 &&
        status(report, "area-1").qualification.derived_category == sketch::AppraisalAreaCategory::above_grade_finished,
        "V2 must use 35/64 finished room area, not 35/100 gross footprint");
    near(report.calculation->property.gla().total.square_metres, 64, 1e-7,
        "Actual low-height region must be excluded exactly once from V2 GLA");
    require(status(report, "area-1").measurement->profile_id == "vertex-ansi-z765-2021-v2" &&
        status(report, "area-1").measurement->profile_version == 2,
        "V2 calculation provenance must not advertise the previous rule");
    for (const auto confirmation : {nlohmann::json(), nlohmann::json(false), nlohmann::json("true")}) {
        auto changed = entities;
        auto& evidence = changed[4].properties["appraisal_facts"]["ansi"]["ceiling"];
        evidence.erase("complete_room_observed");
        if (!confirmation.is_null()) evidence["complete_room_observed"] = confirmation;
        report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
        require(!report.qualified && !report.calculation,
            "V2 cannot reuse unconfirmed or malformed complete-room observations");
    }
    auto partitioned = entities;
    auto& child = partitioned.back();
    child.properties["appraisal_facts"] = entities[4].properties["appraisal_facts"];
    child.properties["appraisal_facts"]["ansi"]["ceiling"] = {{"kind", "flat"}, {"minimum_height_m", 2.4384}};
    partitioned[4].properties["appraisal_facts"]["ansi"]["ceiling"]["below_5ft_deduction_ids"] = nlohmann::json::array();
    report = sketch::build_appraisal_document_report(sketch::Document::create(partitioned).snapshot(), "property-1");
    require(!report.qualified && !report.calculation,
        "A sloped parent cannot mistake a measured child ownership partition for an excluded part of the physical room");
    for(const auto role:{"measured_area","stair_footprint"}) {
        auto wrapped=entities;
        wrapped[4].properties["appraisal_facts"]["ansi"]["ceiling"]["below_5ft_deduction_ids"]=nlohmann::json::array();
        wrapped.back().properties["deduction_ids"]={"measured-grandchild"};
        auto grandchild=wrapped[4];grandchild.id="measured-grandchild";
        grandchild.properties["boundary"]=square(2,2,2);grandchild.properties.erase("deduction_ids");
        grandchild.properties["appraisal_facts"]["boundary_role"]=role;
        grandchild.properties["appraisal_facts"]["ansi"]["ceiling"]=std::string(role)=="stair_footprint" ?
            nlohmann::json{{"kind","stairs"},{"stair_from_floor_id",grandchild.properties.at("floor_id")}} :
            nlohmann::json{{"kind","flat"},{"minimum_height_m",2.4384}};
        wrapped.push_back(grandchild);
        report=sketch::build_appraisal_document_report(sketch::Document::create(wrapped).snapshot(),"property-1");
        require(!report.qualified && !report.calculation,
            "An exclusion wrapper must not conceal a measured/stair grandchild in a V2 complete-room ceiling test");
    }
    auto legacy = entities;
    legacy.front().properties["appraisal_policy"]["version"] = 1;
    legacy[4].properties["appraisal_facts"]["ansi"]["ceiling"].erase("complete_room_observed");
    report = sketch::build_appraisal_document_report(sketch::Document::create(legacy).snapshot(), "property-1");
    require(report.qualified && report.calculation &&
        status(report, "area-1").qualification.derived_category == sketch::AppraisalAreaCategory::above_grade_nonstandard_finished,
        "Opening an existing V1 project must retain its recorded whole-room interpretation");
    require(snapshot.entities() == sketch::Document::create(entities).snapshot().entities(),
        "Reporting must not rewrite saved observations or migrate a policy implicitly");
}

void ansi_nested_partitions_and_ceiling_binding() {
    auto entities = ansi_fixture_entities();
    entities.back().properties["boundary"] = square(0, 0, 10);
    entities.back().properties["deduction_ids"] = {"room-2"};
    auto room = entities.back();
    room.id = "room-2";
    room.properties["boundary"] = square(1, 1, 6);
    room.properties["deduction_ids"] = {"low-3"};
    room.properties["appraisal_facts"]["ansi"]["ceiling"] = {
        {"kind", "sloped"}, {"at_least_7ft_area_m2", 18}, {"room_floor_area_m2", 36},
        {"below_5ft_deduction_ids", {"low-3"}}, {"room_boundary_id", room.id},
        {"source_geometry_sha256", sketch::appraisal_ceiling_geometry_digest(rectangle_geometry(1, 1, 6, 6),
            {{"low-3", rectangle_geometry(1, 1, 2, 2)}})}};
    auto low = entities.back();
    low.id = "low-3";
    low.properties["boundary"] = square(1, 1, 2);
    low.properties.erase("deduction_ids");
    low.properties["appraisal_facts"] = {{"boundary_role", "other_void"}};
    entities.push_back(room);
    entities.push_back(low);
    auto report = sketch::build_appraisal_document_report(sketch::Document::create(entities).snapshot(), "property-1");
    require(report.qualified && report.calculation && !status(report, "room-2").qualification.rule_notes.empty(),
        "Nested ANSI floor/room/exclusion partition must carry ceiling interpretation caveat");
    near(report.calculation->property.gla().total.square_metres, 96, 1e-7,
        "Parent removes full child and child adds only its own net; below-five-foot geometry disappears exactly once");
    near(status(report, "area-1").measurement->net_square_metres, 64, 1e-7, "Parent owns remaining floor region");
    near(status(report, "room-2").measurement->net_square_metres, 32, 1e-7, "Sloped room physically removes below-five-foot strip");
    auto v2_nested = entities;
    v2_nested.front().properties["appraisal_policy"]["version"] = 2;
    v2_nested[5].properties["appraisal_facts"]["ansi"]["ceiling"]["complete_room_observed"] = true;
    const auto v2_report = sketch::build_appraisal_document_report(sketch::Document::create(v2_nested).snapshot(), "property-1");
    require(v2_report.qualified && v2_report.calculation,
        "V2 supports a flat floor containing a complete sloped room with actual low-height exclusions");
    near(v2_report.calculation->property.gla().total.square_metres, 96, 1e-7,
        "V2 nested room and floor must contribute their disjoint counted geometry once");
    auto changed = entities;
    changed[5].properties["boundary"] = square(0, 1, 6);
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified && std::any_of(status(report, "room-2").qualification.issues.begin(),
        status(report, "room-2").qualification.issues.end(), [](const auto& issue) { return issue.code == "stale_ceiling_evidence"; }),
        "Same-area valid shape movement invalidates bound ceiling observations");
    changed = entities;
    changed[6].properties["deduction_ids"] = {"room-2"};
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified && !report.calculation, "Nested deduction cycles cannot yield totals");
    std::set<std::string, std::less<>> visible_ids;
    for (const auto& value : entities) if (value.id != "low-3") visible_ids.insert(value.id);
    report = sketch::build_appraisal_document_report(sketch::Document::create(entities).snapshot(), "property-1",
        sketch::AreaUnit::square_foot, &visible_ids);
    require(!report.qualified && !status(report, "area-1").measurement && !status(report, "room-2").measurement,
        "Hidden nested deduction invalidates every ancestor geometry trace");
    changed = entities;
    auto sibling = room;
    sibling.id = "overlapping-room";
    sibling.properties.erase("deduction_ids");
    sibling.properties["appraisal_facts"]["ansi"]["ceiling"] = {{"kind", "flat"}, {"minimum_height_m", 2.1336}};
    changed[4].properties["deduction_ids"].push_back(sibling.id);
    changed.push_back(sibling);
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified && !report.calculation, "Overlapping sibling measured regions cannot double-count under nested partitions");
    changed = entities;
    changed[5].properties["appraisal_facts"]["ansi"]["ceiling"]["below_5ft_deduction_ids"] = {"area-1"};
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified, "Below-five-foot references must be actual contained exclusion deductions");
    changed = entities;
    changed.front().properties["appraisal_policy"]["policy_kind"] = "residential_declared";
    for (auto& value : changed) if (value.type == "measurement_boundary") value.properties["appraisal_facts"]["ceiling_eligibility"] = "standard";
    report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
    require(!report.qualified, "Nested deductions remain unsupported by frozen declared-v1 contract");
}

void ansi_stairs_and_adu_totals() {
    auto entities = ansi_fixture_entities();
    entities.back().properties["boundary"] = square(0, 0, 10);
    entities.back().properties["deduction_ids"] = {"stairs"};
    auto stairs = entities.back();
    stairs.id = "stairs";
    stairs.properties["boundary"] = square(1, 1, 2);
    stairs.properties.erase("deduction_ids");
    stairs.properties["appraisal_facts"]["boundary_role"] = "stair_footprint";
    stairs.properties["appraisal_facts"]["ansi"]["ceiling"] = {{"kind", "stairs"}, {"stair_from_floor_id", "floor-1"}};
    entities.push_back(stairs);
    auto report = sketch::build_appraisal_document_report(sketch::Document::create(entities).snapshot(), "property-1");
    require(report.qualified && !status(report, "stairs").exclusion, "ANSI descending stairs remain a measured contribution");
    near(report.calculation->property.gla().total.square_metres, 100, 1e-7, "Stair footprint is included on descending source floor");
    for (const auto identity : {"attached_adu", "detached_adu", "detached_other"}) {
        auto changed = ansi_fixture_entities();
        changed.back().properties["appraisal_facts"]["ansi"]["dwelling_identity"] = identity;
        report = sketch::build_appraisal_document_report(sketch::Document::create(changed).snapshot(), "property-1");
        require(report.qualified && report.calculation->property.gla().total.square_metres == 0,
            "ADU and detached-structure contributions never inflate primary GLA");
    }
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

void reporting_projection_contracts() {
    using namespace sketch;
    auto entities = ansi_fixture_entities();
    auto adu = entities.back(); adu.id = "adu";
    adu.properties["boundary"] = square(10, 0, 3.048);
    adu.properties["appraisal_facts"]["ansi"]["dwelling_identity"] = "attached_adu";
    entities.push_back(adu);
    const auto bind = [&](std::vector<Entity>& values) {
        const auto source = Document::create(values).snapshot();
        for (auto& item : values) if (item.type == "measurement_boundary") {
            AppraisalAreaReportingFacts declaration;
            declaration.source_geometry_sha256 = appraisal_reporting_source_digest(source, item.id);
            declaration.contained_within_primary = item.id == "adu";
            declaration.rooms = {{"room-" + item.id, AppraisalRoomUse::bedroom, true}};
            item.properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
        }
    };
    const auto reconfirm = [](std::vector<Entity>& values) {
        const auto source = Document::create(values).snapshot();
        for (auto& item : values) if (item.properties.contains("appraisal_reporting") && item.type == "measurement_boundary")
            item.properties["appraisal_reporting"]["source_geometry_sha256"] = appraisal_reporting_source_digest(source, item.id);
    };
    const auto require_stale = [](const AppraisalDocumentReport& value) {
        require(value.reporting && !value.reporting->area_fields_available && !value.reporting->room_counts_available && !value.reporting->room_summaries_available &&
            std::any_of(value.reporting->issues.begin(), value.reporting->issues.end(), [](const auto& issue) {
                return issue.find("Reporting observations are stale") != std::string::npos;
            }), "Changed semantic observations require explicit reporting reconfirmation");
    };
    entities.front().properties["appraisal_reporting"] = appraisal_reporting_json(
        AppraisalReportingSettings{AppraisalReportingContract::legacy_uad_2_6, true});
    bind(entities);
    auto report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
    require(report.reporting && report.reporting->area_fields_available && !report.reporting->room_counts_available && report.reporting->room_summaries_available &&
        report.reporting->adu_counts.bedrooms == 1,
        "Explicit reporting declarations retain area and ADU detail while unresolved ADU primary-count mapping is withheld");
    near(report.reporting->primary_above_grade_finished_square_metres, 18.580608, 1e-7,
        "Legacy projection combines an explicitly contained above-grade interior-access ADU");
    near(report.calculation->property.gla().total.square_metres, 9.290304, 1e-7,
        "Reporting must not change canonical independent ADU measurement");
    entities.front().properties["appraisal_reporting"]["contract"] = "uad_3_6";
    report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
    near(report.reporting->primary_above_grade_finished_square_metres, 9.290304, 1e-7,
        "UAD 3.6 keeps the same contained ADU separate");
    require(report.reporting->room_counts_available && report.reporting->room_summaries_available &&
        report.reporting->primary_counts.bedrooms == 1 && report.reporting->adu_counts.bedrooms == 1,
        "UAD 3.6 primary bedroom counts are usable and exclude separately summarized ADU declarations");
    require(std::any_of(report.reporting->evidence.begin(), report.reporting->evidence.end(), [](const auto& item) {
        return item.url == "https://sf.freddiemac.com/docs/zip/requirements/appendix-f-1-urar-reference-guide.zip" &&
            item.edition.find("v1.4") != std::string::npos && item.sections.find("10.023") != std::string::npos;
    }) && std::any_of(report.reporting->evidence.begin(), report.reporting->evidence.end(), [](const auto& item) {
        return item.url == "https://sf.freddiemac.com/docs/zip/requirements/appendix-b-1-urar-implementation-guide.zip" &&
            item.sections.find("pp194") != std::string::npos;
    }), "Supported UAD 3.6 count projection retains primary guide edition, pages and field evidence");
    const auto valid_entities=entities;
    entities.front().properties["appraisal_reporting"]["room_inventory_complete"]=false;
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require(report.reporting->area_fields_available && !report.reporting->room_counts_available && !report.reporting->room_summaries_available && report.qualified,
        "Incomplete declared room inventory withholds counts while retaining independent area fields");
    entities=valid_entities;
    entities.front().properties["appraisal_reporting"]["contract"]="legacy_uad_2_6";
    entities.back().properties["appraisal_facts"]["access"]="noncontinuous";
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require_stale(report);
    reconfirm(entities);
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    near(report.reporting->primary_above_grade_finished_square_metres,9.290304,1e-7,
        "Exterior-only ADU access remains separately reported in legacy output");
    entities=valid_entities;
    entities[2].properties["appraisal_facts"]["grade"]="below";
    entities[2].properties["appraisal_facts"]["ansi"]["any_part_below_grade"]=true;
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require_stale(report);
    reconfirm(entities);
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require(report.reporting->primary_counts.bedrooms==1 && report.reporting->below_grade_counts.bedrooms==1,
        "Berm home's below-grade bedroom contributes to UAD 3.6 total and level summary");
    entities.front().properties["appraisal_reporting"]["contract"]="legacy_uad_2_6";
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require(report.reporting->primary_counts.bedrooms==0 && report.reporting->below_grade_counts.bedrooms==1 &&
        report.reporting->primary_above_grade_finished_square_metres==0,
        "Legacy berm home has zero above-grade rooms and no combined below-grade ADU area");
    entities=valid_entities;
    entities[4].properties["appraisal_facts"]["ansi"]["ceiling"]["minimum_height_m"]=1.9;
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require_stale(report);
    reconfirm(entities);
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require(report.reporting->primary_counts.bedrooms==1 && report.reporting->primary_above_grade_finished_square_metres==0,
        "Above-grade nonstandard bedroom remains in primary room counts");
    entities[4].properties["appraisal_facts"]["access"]="noncontinuous";
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require_stale(report);
    reconfirm(entities);
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require(report.reporting->primary_counts.bedrooms==0 && report.reporting->noncontinuous_counts.bedrooms==1,
        "Noncontinuous room contributes only to separate summary");
    entities=valid_entities;
    entities.front().properties["appraisal_reporting"]["contract"]="legacy_uad_2_6";
    entities.back().properties["appraisal_reporting"].erase("contained_within_primary");
    report=build_appraisal_document_report(Document::create(entities).snapshot(),"property-1");
    require(!report.reporting->area_fields_available && report.qualified,
        "Unknown eligible legacy ADU containment withholds projected area without losing canonical measurements");
    entities=valid_entities;
    entities.back().properties["appraisal_reporting"]["rooms"][0]["room_id"] = "room-area-1";
    report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
    require(!report.reporting->room_counts_available && !report.reporting->room_summaries_available && report.qualified,
        "Duplicate room membership withholds projected counts, not independent measurements");
    entities.back().properties["appraisal_reporting"]["rooms"][0]["room_id"] = "room-adu";
    entities.back().properties["boundary"] = square(10, 0, 4);
    report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
    require(!report.reporting->room_counts_available && !report.reporting->area_fields_available,
        "Changed geometry refuses old reporting facts");
    bool rejected = false;
    try { (void)parse_appraisal_reporting_settings({{"version", 1}, {"contract", "unknown"}, {"room_inventory_complete", true}}); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Unknown reporting contracts must be rejected");
    const auto source=Document::create(valid_entities).snapshot();
    AppraisalReportingChanges changes{source.revision(),source.document_id(),document_snapshot_digest(source),"property-1",
        {AppraisalReportingContract::uad_3_6,true},{}};
    validate_appraisal_reporting_changes(source,changes);
    changes.source_snapshot_sha256=std::string(64,'0');rejected=false;
    try{validate_appraisal_reporting_changes(source,changes);}catch(const std::exception&){rejected=true;}
    require(rejected,"Same document/revision with a replaced source fingerprint cannot apply reporting changes");
    auto bad=valid_entities.back().properties.at("appraisal_reporting");bad["unexpected"]=true;rejected=false;
    try{(void)parse_appraisal_area_reporting_facts(bad);}catch(const std::exception&){rejected=true;}
    require(rejected,"Unknown semantic room declaration fields are rejected");
    auto no_reporting=valid_entities;no_reporting.front().properties.erase("appraisal_reporting");
    report=build_appraisal_document_report(Document::create(no_reporting).snapshot(),"property-1");
    require(report.qualified && !report.reporting,"Older measurement-only projects retain their existing totals and no inferred report contract");
}

void reporting_unfinished_room_treatment() {
    using namespace sketch;
    for (const auto contract : {AppraisalReportingContract::legacy_uad_2_6, AppraisalReportingContract::uad_3_6}) {
        for (const bool below : {false, true}) {
            for (const auto use : {AppraisalRoomUse::bedroom, AppraisalRoomUse::bathroom_full,
                    AppraisalRoomUse::bathroom_half, AppraisalRoomUse::other}) {
                auto entities = ansi_fixture_entities();
                entities.front().properties["appraisal_reporting"] = appraisal_reporting_json(AppraisalReportingSettings{contract, true});
                entities[2].properties["appraisal_facts"]["grade"] = below ? "below" : "above";
                entities[2].properties["appraisal_facts"]["ansi"]["any_part_below_grade"] = below;
                entities.back().properties["appraisal_facts"]["finish"] = "unfinished";
                AppraisalAreaReportingFacts declaration;
                declaration.source_geometry_sha256 = appraisal_reporting_source_digest(Document::create(entities).snapshot(), "area-1");
                declaration.rooms = {{"unfinished-room", use, use != AppraisalRoomUse::other}};
                entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
                auto report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
                const auto category = below ? AppraisalAreaCategory::below_grade_unfinished : AppraisalAreaCategory::above_grade_unfinished;
                const bool uad3 = contract == AppraisalReportingContract::uad_3_6;
                require(report.qualified && report.reporting && report.reporting->area_fields_available && report.reporting->room_counts_available == uad3 &&
                    report.reporting->room_summaries_available && report.reporting->rooms.size() == 1 && report.reporting->rooms.front().included_in_primary_counts == uad3 &&
                    report.reporting->primary_counts.bedrooms == (uad3 && use == AppraisalRoomUse::bedroom ? 1U : 0U) &&
                    report.reporting->primary_counts.bathrooms_full == (uad3 && use == AppraisalRoomUse::bathroom_full ? 1U : 0U) &&
                    report.reporting->primary_counts.bathrooms_half == (uad3 && use == AppraisalRoomUse::bathroom_half ? 1U : 0U) &&
                    std::any_of(report.reporting->issues.begin(), report.reporting->issues.end(), [](const auto& issue) {
                        return issue.find("unfinished primary room") != std::string::npos;
                    }) == !uad3, "UAD 3.6 declared unfinished primary rooms count across all grades; unresolved legacy treatment withholds only primary form counts");
                near(report.reporting->primary_area_fields.at(category), 9.290304, 1e-8,
                    "Unfinished room counts and legacy withholding retain independently qualified area fields");
                declaration.rooms.clear();
                entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
                report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
                require(report.reporting->room_counts_available && report.reporting->rooms.empty() && report.reporting->primary_counts.bedrooms == 0,
                    "An explicit empty unfinished inventory permits confirmed zero room counts without guessed room use");
            }
        }
        for (const auto identity : {DwellingIdentity::attached_adu, DwellingIdentity::detached_adu, DwellingIdentity::detached_other}) {
            auto entities = ansi_fixture_entities();
            entities.front().properties["appraisal_reporting"] = appraisal_reporting_json(AppraisalReportingSettings{contract, true});
            entities.back().properties["appraisal_facts"]["ansi"]["dwelling_identity"] = std::string(dwelling_identity_name(identity));
            entities.back().properties["appraisal_facts"]["finish"] = "unfinished";
            AppraisalAreaReportingFacts declaration;
            declaration.source_geometry_sha256 = appraisal_reporting_source_digest(Document::create(entities).snapshot(), "area-1");
            declaration.contained_within_primary = false;
            declaration.rooms = {{"separate-bedroom", AppraisalRoomUse::bedroom, true}};
            entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
            const auto report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
            const bool adu = identity != DwellingIdentity::detached_other;
            const bool unresolved = adu && contract == AppraisalReportingContract::legacy_uad_2_6;
            require(report.qualified && report.reporting->area_fields_available && report.reporting->room_counts_available == !unresolved && report.reporting->room_summaries_available &&
                report.reporting->primary_counts.bedrooms == 0 && report.reporting->adu_counts.bedrooms == (adu ? 1U : 0U),
                "UAD 3.6 separate unfinished ADUs preserve available primary counts; legacy mapping withholds only primary counts; detached-other stays separate");
        }
    }
}

void reporting_observation_context_integrity() {
    using namespace sketch;
    auto baseline = ansi_fixture_entities();
    baseline.front().properties["appraisal_reporting"] = appraisal_reporting_json(
        AppraisalReportingSettings{AppraisalReportingContract::uad_3_6, true});
    const auto original_digest = appraisal_reporting_source_digest(Document::create(baseline).snapshot(), "area-1");
    AppraisalAreaReportingFacts declaration{original_digest, {}, {{"bedroom", AppraisalRoomUse::bedroom, true}}};
    baseline.back().properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
    const auto check_stale = [&](std::vector<Entity> values) {
        const auto source = Document::create(values).snapshot();
        require(appraisal_reporting_source_digest(source, "area-1") != original_digest,
            "Current semantic observations must participate in the reporting confirmation digest");
        const auto report = build_appraisal_document_report(source, "property-1");
        require(report.reporting && !report.reporting->area_fields_available && !report.reporting->room_counts_available && !report.reporting->room_summaries_available &&
            std::any_of(report.reporting->issues.begin(), report.reporting->issues.end(), [](const auto& issue) {
                return issue.find("Reporting observations are stale") != std::string::npos;
            }), "Previously confirmed room observations must be visibly stale after a semantic change");
        AppraisalReportingChanges changes{source.revision(), source.document_id(), document_snapshot_digest(source), "property-1",
            {AppraisalReportingContract::uad_3_6, true}, {{"area-1", declaration}}};
        bool rejected = false;
        try { validate_appraisal_reporting_changes(source, changes); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "A fresh transaction fingerprint cannot validate reporting declarations bound to old observations");
    };
    auto changed = baseline;
    changed.back().properties["appraisal_facts"]["finish"] = "unfinished"; check_stale(changed);
    changed = baseline;
    changed.back().properties["appraisal_facts"]["access"] = "through_unfinished"; check_stale(changed);
    changed = baseline;
    changed.back().properties["appraisal_facts"]["ansi"]["dwelling_identity"] = "detached_adu"; check_stale(changed);
    changed = baseline;
    changed.back().properties["appraisal_facts"]["ansi"]["ceiling"]["minimum_height_m"] = 1.9; check_stale(changed);
    changed = baseline;
    changed[2].properties["appraisal_facts"]["grade"] = "below";
    changed[2].properties["appraisal_facts"]["ansi"]["any_part_below_grade"] = true; check_stale(changed);
    changed = baseline;
    changed.front().properties["appraisal_policy"]["ansi"]["acquisition_increment"] = "tenth_foot"; check_stale(changed);
    changed = baseline;
    changed.front().properties["appraisal_policy"]["version"] = 2; check_stale(changed);
    changed = baseline;
    changed.front().properties["appraisal_policy"]["measurement_basis"] = "plans";
    changed.front().properties["appraisal_policy"]["ansi"]["limitation_declarations"] = json::array({
        {{"kind", "based_on_plans"}, {"statement", "Based on supplied plans."}}}); check_stale(changed);
    changed = baseline;
    changed.front().properties["appraisal_policy"]["ansi"]["direct_measurement"] = false;
    changed.front().properties["appraisal_policy"]["ansi"]["limitation_declarations"] = json::array({
        {{"kind", "direct_measurement_not_possible"}, {"statement", "The inaccessible area could not be directly measured."}}}); check_stale(changed);
    changed = baseline;
    for (auto& item : changed) { item.properties["name"] = "Presentation name"; item.properties["color"] = "#123456"; }
    changed.front().properties["calculation_profile"] = {{"decimal_places", 3}};
    changed.front().properties["appraisal_reporting"]["contract"] = "legacy_uad_2_6";
    changed.back().properties["appraisal_reporting"]["rooms"][0]["use"] = "bathroom_full";
    changed.back().properties["appraisal_reporting"]["contained_within_primary"] = false;
    const auto presentation_source = Document::create(changed).snapshot();
    require(appraisal_reporting_source_digest(presentation_source, "area-1") == original_digest,
        "Presentation, form contract and reporting output must not recursively invalidate observation confirmation");
    auto report = build_appraisal_document_report(presentation_source, "property-1");
    require(report.reporting->area_fields_available && report.reporting->room_counts_available && report.reporting->primary_counts.bathrooms_full == 1,
        "Supported reporting edits retain a usable unchanged observation binding");
    auto with_deduction = baseline;
    auto deduction = with_deduction.back(); deduction.id = "void";
    deduction.properties["boundary"] = square(0.5, 0.5, 0.5);
    deduction.properties["appraisal_facts"]["boundary_role"] = "other_void";
    deduction.properties.erase("appraisal_reporting");
    with_deduction.back().properties["deduction_ids"] = json::array({"void"});
    with_deduction.push_back(deduction);
    const auto deduction_digest = appraisal_reporting_source_digest(Document::create(with_deduction).snapshot(), "area-1");
    require(deduction_digest != original_digest, "Adding a deduction changes reporting source confirmation");
    changed = with_deduction;
    changed.back().properties["appraisal_facts"]["boundary_role"] = "open_to_below";
    require(appraisal_reporting_source_digest(Document::create(changed).snapshot(), "area-1") != deduction_digest,
        "Deduction observations belong to the exact reporting source context");
    changed = with_deduction;
    changed.back().properties["boundary"] = square(0.5, 0.5, 0.6);
    require(appraisal_reporting_source_digest(Document::create(changed).snapshot(), "area-1") != deduction_digest,
        "Deduction geometry changes must still invalidate room observation confirmation");
    auto current = Document::create(baseline);
    const auto before = current.snapshot();
    (void)appraisal_reporting_source_digest(before, "area-1");
    (void)build_appraisal_document_report(before, "property-1");
    require(current.snapshot().entities() == before.entities(),
        "Building current reporting output never modifies stored user declarations");
    changed = baseline;
    changed.push_back(entity("unrelated-property", "property", {{"appraisal_policy", policy()}}));
    require(appraisal_reporting_source_digest(Document::create(changed).snapshot(), "area-1") == original_digest,
        "Unrelated appraisal context does not invalidate current boundary observations");
    for (const bool cyclic : {false, true}) {
        changed = baseline;
        changed.back().properties["deduction_ids"] = json::array({cyclic ? "area-1" : "missing-deduction"});
        bool rejected = false;
        try { (void)appraisal_reporting_source_digest(Document::create(changed).snapshot(), "area-1"); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Unavailable or cyclic geometry dependencies cannot acquire a reporting confirmation digest");
    }
    changed = baseline;
    changed[2].properties["building_id"] = "unavailable-building";
    bool rejected = false;
    try { (void)appraisal_reporting_source_digest(Document::create(changed).snapshot(), "area-1"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "Unavailable appraisal owner context cannot acquire a reporting confirmation digest");
}

void reporting_adu_count_mapping_and_phase_scope() {
    using namespace sketch;
    for (const auto contract : {AppraisalReportingContract::legacy_uad_2_6, AppraisalReportingContract::uad_3_6}) {
        for (const auto identity : {DwellingIdentity::attached_adu, DwellingIdentity::detached_adu}) {
            for (const auto use : {AppraisalRoomUse::bedroom, AppraisalRoomUse::bathroom_full,
                    AppraisalRoomUse::bathroom_half, AppraisalRoomUse::other}) {
                for (const bool total_room : {false, true}) {
                    auto entities = ansi_fixture_entities();
                    entities.front().properties["appraisal_reporting"] = appraisal_reporting_json(AppraisalReportingSettings{contract, true});
                    entities.back().properties["appraisal_facts"]["ansi"]["dwelling_identity"] = std::string(dwelling_identity_name(identity));
                    AppraisalAreaReportingFacts declaration;
                    declaration.source_geometry_sha256 = appraisal_reporting_source_digest(Document::create(entities).snapshot(), "area-1");
                    declaration.contained_within_primary = false;
                    declaration.rooms = {{"adu-room", use, total_room}};
                    entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
                    const auto report = build_appraisal_document_report(Document::create(entities).snapshot(), "property-1");
                    const bool unresolved = contract == AppraisalReportingContract::legacy_uad_2_6 && (use != AppraisalRoomUse::other || total_room);
                    require(report.qualified && report.reporting->area_fields_available && report.reporting->room_counts_available == !unresolved &&
                        report.reporting->room_summaries_available &&
                        report.reporting->rooms.size() == 1 && !report.reporting->rooms.front().included_in_primary_counts &&
                        report.reporting->adu_counts.bedrooms == (use == AppraisalRoomUse::bedroom ? 1U : 0U) &&
                        report.reporting->adu_counts.bathrooms_full == (use == AppraisalRoomUse::bathroom_full ? 1U : 0U) &&
                        report.reporting->adu_counts.bathrooms_half == (use == AppraisalRoomUse::bathroom_half ? 1U : 0U) &&
                        std::any_of(report.reporting->issues.begin(), report.reporting->issues.end(), [](const auto& issue) {
                            return issue.find("ADU room mapping") != std::string::npos;
                        }) == unresolved,
                        "UAD 3.6 ADU counts stay separate; unresolved legacy ADU mapping withholds only primary counts while separate summaries and areas remain available");
                    declaration.rooms.clear();
                    entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
                    require(build_appraisal_document_report(Document::create(entities).snapshot(), "property-1").reporting->room_counts_available,
                        "Explicit empty ADU room membership has no unresolved nonzero primary-count mapping");
                }
            }
        }
    }
    auto multiple_units = ansi_fixture_entities();
    multiple_units.front().properties["appraisal_reporting"] = appraisal_reporting_json(
        AppraisalReportingSettings{AppraisalReportingContract::uad_3_6, true});
    auto adu_floor = multiple_units[2]; adu_floor.id = "adu-below-floor";
    adu_floor.properties["appraisal_facts"]["grade"] = "below";
    adu_floor.properties["appraisal_facts"]["ansi"]["any_part_below_grade"] = true;
    auto adu_layer = multiple_units[3]; adu_layer.id = "adu-below-layer";
    adu_layer.properties["floor_id"] = adu_floor.id;
    auto attached_adu = multiple_units.back(); attached_adu.id = "attached-adu";
    attached_adu.properties["floor_id"] = adu_floor.id;
    attached_adu.properties["layer_id"] = adu_layer.id;
    attached_adu.properties["boundary"] = square(10, 0, 3.048);
    attached_adu.properties["appraisal_facts"]["ansi"]["dwelling_identity"] = "attached_adu";
    auto detached_adu = multiple_units.back(); detached_adu.id = "detached-adu";
    detached_adu.properties["boundary"] = square(20, 0, 3.048);
    detached_adu.properties["appraisal_facts"]["ansi"]["dwelling_identity"] = "detached_adu";
    multiple_units.push_back(adu_floor);
    multiple_units.push_back(adu_layer);
    multiple_units.push_back(attached_adu);
    multiple_units.push_back(detached_adu);
    const auto unit_source = Document::create(multiple_units).snapshot();
    for (auto& item : multiple_units) if (item.type == "measurement_boundary") {
        AppraisalAreaReportingFacts declaration;
        declaration.source_geometry_sha256 = appraisal_reporting_source_digest(unit_source, item.id);
        declaration.contained_within_primary = false;
        declaration.rooms = {{"room-" + item.id, AppraisalRoomUse::bedroom, true}};
        item.properties["appraisal_reporting"] = appraisal_reporting_json(declaration);
    }
    const auto unit_report = build_appraisal_document_report(Document::create(multiple_units).snapshot(), "property-1");
    require(unit_report.qualified && unit_report.reporting->room_counts_available && unit_report.reporting->room_summaries_available &&
        unit_report.reporting->primary_counts.bedrooms == 1 && unit_report.reporting->below_grade_counts.bedrooms == 0 &&
        unit_report.reporting->adu_counts.bedrooms == 2,
        "Multiple ADUs have combined aggregate detail; below-grade ADU rooms enter neither primary per-unit nor primary below-grade totals");
    auto entities = ansi_fixture_entities();
    entities.front().properties["appraisal_reporting"] = appraisal_reporting_json(
        AppraisalReportingSettings{AppraisalReportingContract::uad_3_6, true});
    AppraisalAreaReportingFacts visible_facts;
    visible_facts.source_geometry_sha256 = appraisal_reporting_source_digest(Document::create(entities).snapshot(), "area-1");
    visible_facts.rooms = {{"shared-room-id", AppraisalRoomUse::bedroom, true}};
    entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(visible_facts);
    auto hidden = entities.back(); hidden.id = "hidden-area";
    hidden.properties["boundary"] = square(10, 0, 3.048);
    hidden.properties.erase("appraisal_reporting");
    entities.push_back(hidden);
    auto hidden_facts = visible_facts;
    hidden_facts.source_geometry_sha256 = appraisal_reporting_source_digest(Document::create(entities).snapshot(), "hidden-area");
    entities.back().properties["appraisal_reporting"] = appraisal_reporting_json(hidden_facts);
    const std::set<std::string, std::less<>> visible_ids{"property-1", "building-1", "floor-1", "layer-1", "area-1"};
    for (const bool malformed_hidden : {false, true}) {
        auto values = entities;
        if (malformed_hidden) values.back().properties["appraisal_reporting"]["rooms"] = "malformed";
        const auto source = Document::create(values).snapshot();
        AppraisalReportingChanges changes{source.revision(), source.document_id(), document_snapshot_digest(source), "property-1",
            {AppraisalReportingContract::uad_3_6, true}, {{"area-1", visible_facts}}};
        const auto report = build_appraisal_document_report(source, "property-1", AreaUnit::square_foot, &visible_ids);
        require(report.qualified && report.reporting->room_counts_available,
            "Hidden duplicate or malformed reporting declarations do not affect the visible phase projection");
        validate_appraisal_reporting_changes(source, changes, &visible_ids);
        bool rejected = false;
        try { validate_appraisal_reporting_changes(source, changes); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Unfiltered validation still detects duplicate or malformed in-scope declarations");
        changes.areas = {{"hidden-area", hidden_facts}};
        rejected = false;
        try { validate_appraisal_reporting_changes(source, changes, &visible_ids); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "A phase-filtered reporting transaction rejects changes to a hidden area");
    }
}

int main() {
    try {
        qualified_document_recalculates_from_geometry();
        reporting_projection_contracts();
        reporting_unfinished_room_treatment();
        reporting_observation_context_integrity();
        reporting_adu_count_mapping_and_phase_scope();
        ansi_declarations_and_canonical_reporting();
        ansi_flat_ceiling_precision_changes_contributions();
        ansi_finished_room_v2_uses_current_exclusion_geometry();
        ansi_nested_partitions_and_ceiling_binding();
        ansi_stairs_and_adu_totals();
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
