#include "sketch/appraisal_document.hpp"

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
        }
        require(document.snapshot().entities() == before.entities() &&
                    document.snapshot().revision() == before.revision(),
                "reporting must leave the exact document snapshot unchanged");
    }
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
