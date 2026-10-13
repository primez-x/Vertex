#pragma once
#include "sketch/document.hpp"
#include "sketch/calculations.hpp"
#include <optional>
#include <string>
#include <vector>

namespace sketch {
[[nodiscard]] CalculationProfile default_calculation_profile();
// Only this versioned algorithm may add creation defaults. Absence retains
// the geometry-only meaning of historical boundary authoring records.
struct BoundaryAreaDefaultsIntent {
    unsigned version{1};
    std::optional<std::string> staged_category;
    bool operator==(const BoundaryAreaDefaultsIntent&) const = default;
};
void validate_boundary_area_defaults_intent(const BoundaryAreaDefaultsIntent&);
struct AreaTypeDefaultChanges {
    std::vector<EntityChange> changes;
    bool set_up{};
    std::optional<std::string> defaulted_type;
    std::vector<std::string> kept_notes;
};
[[nodiscard]] AreaTypeDefaultChanges boundary_area_default_changes(
    const DocumentSnapshot& geometry_candidate, const std::string& property_id,
    const std::vector<std::string>& created_boundary_ids, const BoundaryAreaDefaultsIntent&);
} // namespace sketch
