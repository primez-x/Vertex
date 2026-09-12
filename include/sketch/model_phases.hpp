#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

enum class ModelPhase { existing, demolished, proposed };
[[nodiscard]] std::string phase_name(ModelPhase phase);

struct RemodelingAlternative {
    std::string id;
    std::string name;
    std::vector<std::string> demolished_ids;
    std::vector<std::string> proposed_ids;
    bool operator==(const RemodelingAlternative&) const = default;
};

struct PhaseComparisonEntry {
    std::string entity_id;
    // Absent means the entity does not participate in that alternative.
    std::optional<ModelPhase> left;
    std::optional<ModelPhase> right;
    bool operator==(const PhaseComparisonEntry&) const = default;
};

struct PhaseComparison {
    // nullopt explicitly identifies the shared baseline, independent of active selection.
    std::optional<std::string> left_alternative;
    std::optional<std::string> right_alternative;
    std::vector<PhaseComparisonEntry> differences;
};

// Validated immutable semantic value. All operations return copies; no document,
// entity geometry, or caller-owned JSON is modified.
class ModelPhases final {
public:
    [[nodiscard]] static ModelPhases create(
        std::vector<std::string> model_entity_ids,
        std::vector<std::string> baseline_ids,
        std::vector<RemodelingAlternative> alternatives,
        std::optional<std::string> active_alternative = std::nullopt);
    [[nodiscard]] static ModelPhases from_json(const nlohmann::json& value);
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] ModelPhases with_active(std::optional<std::string> alternative) const;
    // Returns a validated copy with one additional remodeling alternative.
    // Existing alternatives and the active selection are preserved.  The
    // candidate is checked against the registry before it can be persisted.
    [[nodiscard]] ModelPhases with_alternative(RemodelingAlternative alternative) const;
    [[nodiscard]] const std::vector<std::string>& entity_ids() const noexcept;
    [[nodiscard]] const std::vector<std::string>& baseline_ids() const noexcept;
    [[nodiscard]] const std::vector<RemodelingAlternative>& alternatives() const noexcept;
    [[nodiscard]] const std::optional<std::string>& active_alternative() const noexcept;
    [[nodiscard]] std::map<std::string, ModelPhase, std::less<>> active_state() const;
    [[nodiscard]] std::map<std::string, ModelPhase, std::less<>> state(
        const std::optional<std::string>& alternative) const;
    [[nodiscard]] PhaseComparison compare(std::optional<std::string> left,
                                          std::optional<std::string> right) const;
private:
    ModelPhases() = default;
    std::vector<std::string> entity_ids_;
    std::vector<std::string> baseline_ids_;
    std::vector<RemodelingAlternative> alternatives_;
    std::optional<std::string> active_;
    void validate_selection(const std::optional<std::string>& alternative) const;
};

}  // namespace sketch
