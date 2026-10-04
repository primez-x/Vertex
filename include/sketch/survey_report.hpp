#pragma once

#include "sketch/quantity.hpp"
#include "sketch/survey_contract.hpp"
#include <nlohmann/json.hpp>
#include <utility>

namespace sketch {
using Json = nlohmann::json;
struct SurveyInput {
    std::string legs_text;
    std::string source_text;
    std::string closure_tolerance_expression;
    Unit default_unit{default_input_unit};
};

// Derived display fields never establish geometry. Only reconstruction from
// entered calls and matching normalized receipts can create this value.
class RebuiltSurveyReport final {
public:
    [[nodiscard]] const Json& report() const noexcept { return report_; }
    [[nodiscard]] const std::vector<SurveyLeg>& legs() const noexcept { return legs_; }
    [[nodiscard]] const Boundary& measured_segments() const noexcept { return measured_segments_; }
private:
    RebuiltSurveyReport(Json report, std::vector<SurveyLeg> legs, Boundary measured_segments)
        : report_(std::move(report)), legs_(std::move(legs)), measured_segments_(std::move(measured_segments)) {}
    Json report_;
    std::vector<SurveyLeg> legs_;
    Boundary measured_segments_;
    friend RebuiltSurveyReport rebuild_survey_report(const Json&);
};

[[nodiscard]] Json build_survey_report(const SurveyInput& input);
[[nodiscard]] RebuiltSurveyReport rebuild_survey_report(const Json& report);
enum class SurveyClosureMode { retain_measured_calls, adjust_final_endpoint };
struct SurveyBoundaryResult {
    Boundary boundary;
    bool added_closing_segment{};
    bool adjusted_final_endpoint{};
    std::optional<Vec2> endpoint_adjustment_m;
};
[[nodiscard]] SurveyBoundaryResult make_survey_boundary(
    const RebuiltSurveyReport& report, SurveyClosureMode mode);
} // namespace sketch
