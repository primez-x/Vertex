#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace sketch {

enum class WorkspaceTheme { light, dark, high_contrast };

[[nodiscard]] std::string workspace_theme_name(WorkspaceTheme theme);

struct DpiLayoutQualification {
    std::uint32_t width_px{};
    std::uint32_t height_px{};
    double scale{};
    bool operator==(const DpiLayoutQualification&) const = default;
};

// Product-level accessibility and field-input declaration. It is an
// acceptance contract; it does not claim that physical devices have passed.
struct WorkspaceAccessibilityProfile {
    bool keyboard_navigation{true};
    bool predictable_focus{true};
    bool accessible_properties{true};
    bool high_contrast{true};
    bool pen_controls{true};
    bool touch_controls{true};
    bool measurement_keypad{true};
    std::vector<WorkspaceTheme> themes;
    std::vector<DpiLayoutQualification> layouts;

    [[nodiscard]] static WorkspaceAccessibilityProfile production_baseline();
    void validate() const;
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static WorkspaceAccessibilityProfile from_json(const nlohmann::json& value);
};

}  // namespace sketch
