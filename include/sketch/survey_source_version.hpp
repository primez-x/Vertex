#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace sketch {
// Structural admission only. Stored calls are archival provenance, not current
// geometry authority. Consumers must rebuild_survey_report before using them.
struct SurveySourceAdmission {
    bool modern_reader{};
    std::optional<std::string> unsupported;
};
[[nodiscard]] SurveySourceAdmission inspect_survey_source(const nlohmann::json& source);
}
