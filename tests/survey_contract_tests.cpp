#include "sketch/survey_contract.hpp"
#include "sketch/survey_report.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
using namespace sketch;
void require(bool ok) { if (!ok) throw std::runtime_error("survey assertion failed"); }
template<class F> void rejects(F f) { try { f(); } catch (const std::invalid_argument&) { return; } throw std::runtime_error("invalid survey accepted"); }
void curved_reports() {
    const auto rectangle = [](std::string call) {
        return build_survey_report({"NE,0,10 m\n" + call + "\nSE,0,10 m\nSW,90,20 m", "fixture", "0.01 m", Unit::metre});
    };
    const auto angle = rectangle("CURVE,NE,90,20 m,-180");
    require(angle.at("version") == 2 && angle.at("input_provenance").at("version") == 2);
    require(std::abs(angle.at("diagnostics").at("area_m2").get<double>() - (200 + 50 * std::numbers::pi)) < 1e-10);
    require(std::abs(angle.at("diagnostics").at("perimeter_m").get<double>() - (40 + 10 * std::numbers::pi)) < 1e-10);
    const auto rebuilt = rebuild_survey_report(angle);
    require(rebuilt.legs()[1].distance_m == 20 && rebuilt.measured_segments()[1].sweep_radians == -std::numbers::pi);
    require(make_survey_boundary(rebuilt, SurveyClosureMode::retain_measured_calls).boundary.size() == 4);
    const auto height = rectangle("ARC_HEIGHT,NE,90,20 m,-10 m");
    const auto length = rectangle("ARC_LENGTH,NE,90,20 m,31.41592653589793 m,CW");
    for (const auto* report : {&height, &length}) {
        require(std::abs(report->at("diagnostics").at("area_m2").get<double>() - (200 + 50 * std::numbers::pi)) < 1e-8);
        require(rebuild_survey_report(*report).measured_segments()[1].sweep_radians < 0);
    }
    require(height.at("input_provenance").at("curves")[0].at("exact_metres").at("numerator") == -10);
    auto decorated = height;
    decorated["vertices"] = "untrusted";
    decorated["diagnostics"] = nullptr;
    decorated["input_provenance"]["vendor"] = {{"preserve", 7}};
    decorated["input_provenance"]["curves"][0]["vendor"] = "preserve";
    const auto safe = rebuild_survey_report(decorated);
    require(safe.report().at("diagnostics").at("area_m2") == height.at("diagnostics").at("area_m2"));
    require(safe.report().at("input_provenance") == decorated.at("input_provenance"));
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto bad = height;
        if (mutation == 0) bad["input_provenance"]["curves"][0]["construction_kind"] = "future_curve";
        if (mutation == 1) bad["input_provenance"]["curves"][0]["exact_metres"]["numerator"] = -9;
        if (mutation == 2) bad["legs"][1]["sweep_radians"] = 0;
        if (mutation == 3) bad["input_provenance"]["distances"][1]["original_expression"] = "21 m";
        if (mutation == 4) bad["input_provenance"]["curves"][0]["version"] = 2;
        if (mutation == 5) bad["closure_tolerance_m"] = 1;
        rejects([&] { (void)rebuild_survey_report(bad); });
    }
    // Reverse the same outside-top semicircle: east, north, west curve, south.
    // A west-directed positive sweep bulges north and reverses its traversal.
    const auto reversed = build_survey_report({"NE,90,20\nNE,0,10\nCURVE,SW,90,20,180\nSE,0,10", "reverse", "0 m", Unit::metre});
    require(std::abs(reversed.at("diagnostics").at("area_m2").get<double>() - (200 + 50 * std::numbers::pi)) < 1e-10);
    require(reversed.at("legs")[2].at("sweep_radians") == std::numbers::pi);
    require(signed_area(rebuild_survey_report(reversed).measured_segments()) > 0 &&
        signed_area(rebuilt.measured_segments()) < 0);
    rejects([] { (void)build_survey_report({"NE,0,10\nNE,90,20\nSE,0,10\nCURVE,SW,90,20,180",
        "inward semicircle touches opposite side", "0 m", Unit::metre}); });
    // Two semicircles enclose a disk even though there are only two calls.
    const auto lens = build_survey_report({"CURVE,NE,90,20,180\nCURVE,SW,90,20,180", "disk", "0 m", Unit::metre});
    require(std::abs(lens.at("diagnostics").at("area_m2").get<double>() - 100 * std::numbers::pi) < 1e-10);
    const auto major = build_survey_report({"CURVE,NE,90,20,270\nSW,90,20", "major", "0 m", Unit::metre});
    require(std::abs(major.at("diagnostics").at("area_m2").get<double>() - (100 + 150 * std::numbers::pi)) < 1e-9);
    const auto open = build_survey_report({"CURVE,NE,90,20,-180", "open", "0 m", Unit::metre});
    require(open.at("diagnostics").at("area_m2").is_null());
    rejects([&] { (void)make_survey_boundary(rebuild_survey_report(open), SurveyClosureMode::retain_measured_calls); });
    for (const auto* call : {"CURVE,NE,90,20,0", "CURVE,NE,90,20,360", "CURVE,NE,90,20,nan", "CURVE,NE,90,20,inf",
                            "CURVE,NE,90,20,1e-9", "ARC_HEIGHT,NE,90,20,0", "ARC_LENGTH,NE,90,20,20,CCW",
                            "ARC_LENGTH,NE,90,20,30,future"})
        rejects([&] { (void)build_survey_report({call, "bad open curve", "0 m", Unit::metre}); });
    rejects([] { (void)SurveyTraverse("bad open", {{"a", BearingQuadrant::north_east, 90, 1, 1e-320}}, 0); });
    rejects([] { (void)SurveyTraverse("bad open", {{"a", BearingQuadrant::north_east, 90, 1, std::numeric_limits<double>::quiet_NaN()}}, 0); });
    rejects([] { (void)build_survey_report({"CURVE,NE,90,20,180\nCURVE,SW,90,20,-180", "overlapping arcs", "0 m", Unit::metre}); });
    // The semicircle passes through the opposite rectangle side.
    rejects([] { (void)build_survey_report({"NE,0,5\nCURVE,NE,90,20,180\nSE,0,5\nSW,90,20", "line arc crossing", "0 m", Unit::metre}); });
    // Closure tolerance cannot authorize a degenerate explicit closing edge.
    for (const auto* residual : {"0.00000005", "0.0000002"}) {
        const auto closing = build_survey_report({std::string("SE,0,10\nSW,90,20\nNE,0,10\nCURVE,NE,90,20.") +
            (std::string(residual) == "0.00000005" ? "00000005" : "0000002") + ",-180",
            "tiny residual", "0.01 m", Unit::metre});
        const auto validated = rebuild_survey_report(closing);
        rejects([&] { (void)make_survey_boundary(validated, SurveyClosureMode::adjust_final_endpoint); });
        if (std::string(residual) == "0.0000002")
            require(make_survey_boundary(validated, SurveyClosureMode::retain_measured_calls).added_closing_segment);
        else rejects([&] { (void)make_survey_boundary(validated, SurveyClosureMode::retain_measured_calls); });
    }
    // At the exact binary geometry tolerance, closure cannot turn a measured
    // degenerate arc into a valid area (this also avoids decimal cancellation).
    rejects([] { (void)build_survey_report({"CURVE,NE,90,0.0000001,-180", "equal tolerance residual", "0.01 m", Unit::metre}); });
}
void straight_closure_cutoff() {
    // A small westward component on the final southward call produces the
    // residual directly, avoiding subtraction of two nearly equal lengths.
    for (const auto* angle : {"0.00000028647889756541162", "0.00000057295779513082324", "0.0000011459155902616465"}) {
        const auto input = build_survey_report({std::string("NE,90,20 m\nNE,0,10 m\nSW,90,20 m\nSW,") + angle + ",10 m",
            "Closure cutoff", "0.01 m", Unit::metre});
        const auto report = rebuild_survey_report(input);
        const auto residual = input.at("diagnostics").at("linear_error_m").get<double>();
        require(input.at("diagnostics").at("closed") == true && !input.at("diagnostics").at("area_m2").is_null());
        if (std::string_view(angle) == "0.00000057295779513082324")
            require(residual == default_geometry_tolerance_metres); // Actual insertion equality, not a degenerate leg.
        const auto adjusted = make_survey_boundary(report, SurveyClosureMode::adjust_final_endpoint);
        require(adjusted.adjusted_final_endpoint && !adjusted.added_closing_segment && adjusted.boundary.size() == 4);
        if (residual <= default_geometry_tolerance_metres)
            rejects([&] { (void)make_survey_boundary(report, SurveyClosureMode::retain_measured_calls); });
        else require(make_survey_boundary(report, SurveyClosureMode::retain_measured_calls).added_closing_segment);
    }
}
int main() {
    try {
        curved_reports();
        straight_closure_cutoff();
        require(parse_survey_angle(" 45.5 ") == 45.5);
        require(parse_survey_angle("45:30:0") == 45.5);
        require(std::abs(parse_survey_angle("12 : 34 : 56.25") - (12 + 34.0/60 + 56.25/3600)) < 1e-12);
        require(parse_survey_angle("90:0:0") == 90 && parse_survey_angle("0:0:0") == 0);
        for (const auto* invalid : {"", "-1", "nan", "1e1", "91", "90:0:0.1", "1:60:0",
                                   "1:0:60", "1.5:0:0", "1:2.5:0", "1:2", "1:2:3:4", "1::3"})
            rejects([&] { (void)parse_survey_angle(invalid); });
        const std::vector<SurveyLeg> square{{"a", BearingQuadrant::north_east, 0, 100}, {"b", BearingQuadrant::north_east, 90, 100}, {"c", BearingQuadrant::south_east, 0, 100}, {"d", BearingQuadrant::south_west, 90, 100}};
        const SurveyTraverse survey("entered fixture", square, 1e-6);
        require(survey.diagnostics().closed && std::abs(*survey.diagnostics().area_m2 - 10000) < 1e-7);
        require(std::abs(*survey.diagnostics().acres - 10000 / 4046.8564224) < 1e-10);
        const auto j = nlohmann::json::parse(survey.serialize());
        require(j["legs"][0]["id"] == "a" && j["provenance"] == "entered fixture");
        require(j.at("version") == 1 && !j.at("legs")[0].contains("sweep_radians"));
        require(survey.serialize() == SurveyTraverse("entered fixture", square, 1e-6).serialize());
        auto zero=square; zero[0].angle_degrees=-0.0;
        require(survey.serialize() == SurveyTraverse("entered fixture",zero,1e-6).serialize());
        auto reverse=square; reverse[0].quadrant=BearingQuadrant::south_west; reverse[1].quadrant=BearingQuadrant::north_west; reverse[2].quadrant=BearingQuadrant::north_east; reverse[3].quadrant=BearingQuadrant::south_east;
        require(std::abs(*SurveyTraverse("reverse",reverse).diagnostics().area_m2-10000)<1e-7);
        require(!SurveyTraverse("open", {square[0], square[1]}, 0).diagnostics().area_m2);
        rejects([] { (void)SurveyTraverse("bad", {{"a", BearingQuadrant::north_east, 91, 1}}); });
        rejects([] { (void)SurveyTraverse("bad", {{"a", static_cast<BearingQuadrant>(99), 0, 1}}); });
        rejects([] { (void)SurveyTraverse("bad", {{"a", BearingQuadrant::north_east, 0, -1}}); });
        rejects([] { (void)SurveyTraverse("bad", {{"a", BearingQuadrant::north_east, 0, std::numeric_limits<double>::infinity()}}); });
        rejects([&] { auto legs = square; legs[1].id = "a"; (void)SurveyTraverse("bad", legs); });
        rejects([] { (void)SurveyTraverse("bad", {}, -1); });
        rejects([] { (void)SurveyTraverse("bad", {{"a",BearingQuadrant::north_east,0,1}}, std::numeric_limits<double>::quiet_NaN()); });
        rejects([] { (void)SurveyTraverse("bad", std::vector<SurveyLeg>(SurveyTraverse::maximum_legs+1)); });
        rejects([&] { (void)SurveyTraverse(std::string(1,static_cast<char>(0xff)),square); });
        rejects([] { (void)SurveyTraverse("bowtie", {{"a",BearingQuadrant::north_east,45,std::sqrt(2.0)}, {"b",BearingQuadrant::south_west,90,1}, {"c",BearingQuadrant::south_east,45,std::sqrt(2.0)}, {"d",BearingQuadrant::north_west,90,1}}); });
        rejects([] { (void)SurveyTraverse("overlap", {{"a",BearingQuadrant::north_east,0,2}, {"b",BearingQuadrant::south_east,0,1}, {"c",BearingQuadrant::south_east,0,1}}); });
        rejects([] { (void)SurveyTraverse("bad", {{"a", BearingQuadrant::north_east, 0, 1e308}, {"b", BearingQuadrant::north_east, 0, 1e308}}); });
        std::cout << "survey contract tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
