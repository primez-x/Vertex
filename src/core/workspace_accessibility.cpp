#include "sketch/workspace_accessibility.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
void invalid(const char* message) { throw std::invalid_argument(message); }

void exact_keys(const nlohmann::json& value, const std::set<std::string>& keys) {
    if (!value.is_object() || value.size() != keys.size()) invalid("invalid accessibility JSON fields");
    for (const auto& key : keys) {
        if (!value.contains(key)) throw std::invalid_argument("missing accessibility field: " + key);
    }
}

WorkspaceTheme parse_theme(const nlohmann::json& value) {
    if (!value.is_string()) invalid("accessibility theme must be a string");
    const auto name = value.get<std::string>();
    if (name == "light") return WorkspaceTheme::light;
    if (name == "dark") return WorkspaceTheme::dark;
    if (name == "high_contrast") return WorkspaceTheme::high_contrast;
    invalid("unsupported accessibility theme");
    return WorkspaceTheme::light;
}

void validate_themes(const std::vector<WorkspaceTheme>& themes) {
    if (themes.empty()) invalid("accessibility themes cannot be empty");
    std::set<std::string> names;
    for (const auto theme : themes) {
        if (!names.insert(workspace_theme_name(theme)).second)
            invalid("duplicate accessibility theme");
    }
}

void validate_layout(const DpiLayoutQualification& layout) {
    if (layout.width_px < 1366 || layout.height_px < 768 || !std::isfinite(layout.scale) ||
        !(layout.scale == 1.0 || layout.scale == 1.5 || layout.scale == 2.0 || layout.scale == 4.0))
        invalid("accessibility layout is outside the supported qualification matrix");
}
}  // namespace

std::string workspace_theme_name(WorkspaceTheme theme) {
    switch (theme) {
    case WorkspaceTheme::light: return "light";
    case WorkspaceTheme::dark: return "dark";
    case WorkspaceTheme::high_contrast: return "high_contrast";
    }
    invalid("unknown accessibility theme");
    return {};
}

WorkspaceAccessibilityProfile WorkspaceAccessibilityProfile::production_baseline() {
    return {true, true, true, true, true, true, true,
            {WorkspaceTheme::light, WorkspaceTheme::dark, WorkspaceTheme::high_contrast},
            {{1366, 768, 1.0}, {1920, 1080, 1.5}, {3840, 2160, 2.0}, {3840, 2160, 4.0}}};
}

void WorkspaceAccessibilityProfile::validate() const {
    if (!keyboard_navigation || !predictable_focus || !accessible_properties || !high_contrast)
        invalid("production accessibility baseline requires keyboard, focus, properties, and high contrast");
    if (!pen_controls || !touch_controls || !measurement_keypad)
        invalid("production field baseline requires pen, touch, and measurement keypad controls");
    validate_themes(themes);
    if (std::find(themes.begin(), themes.end(), WorkspaceTheme::high_contrast) == themes.end())
        invalid("high-contrast theme is required");
    if (layouts.empty()) invalid("accessibility layouts cannot be empty");
    std::set<std::tuple<std::uint32_t, std::uint32_t, double>> seen;
    for (const auto& layout : layouts) {
        validate_layout(layout);
        if (!seen.emplace(layout.width_px, layout.height_px, layout.scale).second)
            invalid("duplicate accessibility layout");
    }
}

nlohmann::json WorkspaceAccessibilityProfile::to_json() const {
    validate();
    nlohmann::json theme_values = nlohmann::json::array();
    for (const auto theme : themes) theme_values.push_back(workspace_theme_name(theme));
    nlohmann::json layout_values = nlohmann::json::array();
    for (const auto& layout : layouts)
        layout_values.push_back({{"width_px", layout.width_px}, {"height_px", layout.height_px},
                                 {"scale", layout.scale}});
    return {{"schema", "sketch.workspace_accessibility"}, {"version", 1},
            {"keyboard_navigation", keyboard_navigation}, {"predictable_focus", predictable_focus},
            {"accessible_properties", accessible_properties}, {"high_contrast", high_contrast},
            {"pen_controls", pen_controls}, {"touch_controls", touch_controls},
            {"measurement_keypad", measurement_keypad}, {"themes", theme_values}, {"layouts", layout_values}};
}

WorkspaceAccessibilityProfile WorkspaceAccessibilityProfile::from_json(const nlohmann::json& value) {
    try {
        exact_keys(value, {"schema", "version", "keyboard_navigation", "predictable_focus",
                           "accessible_properties", "high_contrast", "pen_controls", "touch_controls",
                           "measurement_keypad", "themes", "layouts"});
        if (value.at("schema") != "sketch.workspace_accessibility" || !value.at("version").is_number_integer() ||
            value.at("version") != 1 || !value.at("themes").is_array() || !value.at("layouts").is_array())
            invalid("unsupported accessibility schema");
        WorkspaceAccessibilityProfile result;
        result.keyboard_navigation = value.at("keyboard_navigation").get<bool>();
        result.predictable_focus = value.at("predictable_focus").get<bool>();
        result.accessible_properties = value.at("accessible_properties").get<bool>();
        result.high_contrast = value.at("high_contrast").get<bool>();
        result.pen_controls = value.at("pen_controls").get<bool>();
        result.touch_controls = value.at("touch_controls").get<bool>();
        result.measurement_keypad = value.at("measurement_keypad").get<bool>();
        for (const auto& theme : value.at("themes")) result.themes.push_back(parse_theme(theme));
        for (const auto& encoded : value.at("layouts")) {
            exact_keys(encoded, {"width_px", "height_px", "scale"});
            result.layouts.push_back({encoded.at("width_px").get<std::uint32_t>(),
                                     encoded.at("height_px").get<std::uint32_t>(),
                                     encoded.at("scale").get<double>()});
        }
        result.validate();
        return result;
    } catch (const nlohmann::json::exception& error) {
        throw std::invalid_argument(std::string("invalid accessibility JSON: ") + error.what());
    }
}

}  // namespace sketch
