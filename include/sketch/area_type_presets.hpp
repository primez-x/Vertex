#pragma once
#include <array>
#include <string_view>

namespace sketch {
// Drawing types are descriptive. These flags belong only to the generic
// measurement profile; they never establish appraisal eligibility or floor facts.
struct AreaTypePreset {
    std::string_view code;
    std::string_view classification;
    std::string_view label;
    bool building_total;
    bool living_total;
};
inline constexpr std::array area_type_presets{
    AreaTypePreset{"GLA1", "first_floor", "First Floor", true, true},
    AreaTypePreset{"GLA2", "second_floor", "Second Floor", true, true},
    AreaTypePreset{"GLA3", "third_floor", "Third Floor", true, true},
    AreaTypePreset{"GLA4", "fourth_floor", "Fourth Floor", true, true},
    AreaTypePreset{"GBA", "gross_building_area", "Gross Building Area", true, false},
    AreaTypePreset{"BSMT-F", "finished_basement", "Finished Basement", true, false},
    AreaTypePreset{"BSMT-U", "unfinished_basement", "Unfinished Basement", true, false},
    AreaTypePreset{"GAR", "garage", "Garage", true, false},
    AreaTypePreset{"DGAR", "detached_garage", "Detached Garage", true, false},
    AreaTypePreset{"ADU", "accessory_dwelling", "Accessory Dwelling / ADU", true, false},
    AreaTypePreset{"OUT", "outbuilding", "Shed / Outbuilding", true, false},
    AreaTypePreset{"CAR", "carport", "Carport", false, false},
    AreaTypePreset{"PORCH", "porch", "Porch", false, false},
    AreaTypePreset{"PATIO", "patio", "Patio", false, false},
    AreaTypePreset{"DECK", "deck", "Wood Deck", false, false},
    AreaTypePreset{"BALC", "balcony", "Balcony", false, false},
    AreaTypePreset{"STG", "storage", "Storage", true, false},
    AreaTypePreset{"LOW", "low_ceiling", "Low Ceiling / Non-GLA", true, false},
    AreaTypePreset{"OPEN", "open_to_below", "Open to Below", false, false},
    AreaTypePreset{"NCA", "non_calculated", "Non-Calculated Area", false, false},
    AreaTypePreset{"SITE", "subject_site", "Subject Site", false, false},
    AreaTypePreset{"UND", "", "Undefined / Clear", false, false}
};
[[nodiscard]] inline const AreaTypePreset* area_type_for_classification(std::string_view value) {
    for (const auto& preset : area_type_presets)
        if (!preset.classification.empty() && preset.classification == value) return &preset;
    return nullptr;
}
[[nodiscard]] inline const AreaTypePreset* area_type_for_code(std::string_view value) {
    for (const auto& preset : area_type_presets) if (preset.code == value) return &preset;
    return nullptr;
}
} // namespace sketch
