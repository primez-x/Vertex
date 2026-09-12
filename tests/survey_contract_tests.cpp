#include "sketch/survey_contract.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace sketch;
void require(bool ok) { if (!ok) throw std::runtime_error("survey assertion failed"); }
template<class F> void rejects(F f) { try { f(); } catch (const std::invalid_argument&) { return; } throw std::runtime_error("invalid survey accepted"); }
int main() {
    try {
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
