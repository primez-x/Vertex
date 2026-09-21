#include "sketch/appraisal_document.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>

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
        deductions_partition_categories_without_double_counting();
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
