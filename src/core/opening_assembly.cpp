#include "sketch/opening_assembly.hpp"

#include "sketch/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sketch {
namespace {
constexpr double tolerance = default_geometry_tolerance_metres;

[[noreturn]] void reject(const char* message) {
    throw std::invalid_argument(message);
}

void positive(double value, const char* message) {
    if (!std::isfinite(value) || value <= tolerance || value > 10.0) reject(message);
}

void nonnegative(double value, const char* message) {
    if (!std::isfinite(value) || value < 0.0 || value > 10.0) reject(message);
}

bool canonical_window_descriptor(const OpeningAssembly& value) {
    return value.window_layout == WindowLayoutKind::fixed &&
           !value.window_hinge_at_end && value.window_open_left &&
           value.window_angle_degrees == 90.0 && value.window_slide_fraction == 0.0;
}
}  // namespace

void validate_opening_assembly(const OpeningAssembly& value) {
    if (value.kind != OpeningAssemblyKind::door &&
        value.kind != OpeningAssemblyKind::window) {
        reject("Opening assembly kind is unsupported");
    }
    positive(value.frame_width_m, "Opening assembly frame width must be positive");
    positive(value.frame_depth_m, "Opening assembly frame depth must be positive");
    positive(value.panel_thickness_m, "Opening assembly panel thickness must be positive");
    nonnegative(value.glazing_thickness_m,
                "Opening assembly glazing thickness must be nonnegative");
    if (!std::isfinite(value.inset_m) || std::abs(value.inset_m) > 10.0) {
        reject("Opening assembly inset must be finite and bounded");
    }
    if (value.panel_thickness_m > value.frame_depth_m + tolerance) {
        reject("Opening assembly panel is deeper than its frame");
    }
    if (value.glazing_thickness_m > value.panel_thickness_m + tolerance) {
        reject("Opening assembly glazing is deeper than its panel");
    }
    if (value.kind == OpeningAssemblyKind::window &&
        value.glazing_thickness_m <= tolerance) {
        reject("Window assemblies require positive glazing thickness");
    }
    if (window_layout_kind_name(value.window_layout) == "invalid")
        reject("Window assembly layout is unsupported");
    if (!std::isfinite(value.window_angle_degrees) || value.window_angle_degrees < 0.0 ||
        value.window_angle_degrees > 180.0)
        reject("Window casement angle must be finite and between 0 and 180 degrees");
    if (!std::isfinite(value.window_slide_fraction) || value.window_slide_fraction < 0.0 ||
        value.window_slide_fraction > 1.0)
        reject("Window slide fraction must be finite and between 0 and 1");
    if (value.kind == OpeningAssemblyKind::door && !canonical_window_descriptor(value))
        reject("Door assembly cannot carry a window descriptor");
    switch (value.window_layout) {
    case WindowLayoutKind::fixed:
    case WindowLayoutKind::double_fixed:
    case WindowLayoutKind::triple_fixed:
        if (value.window_hinge_at_end || !value.window_open_left ||
            value.window_angle_degrees != 90.0 || value.window_slide_fraction != 0.0)
            reject("Fixed window layouts cannot carry dormant movement fields");
        break;
    case WindowLayoutKind::casement:
        if (value.window_slide_fraction != 0.0)
            reject("Casement window cannot carry sliding travel");
        break;
    case WindowLayoutKind::sliding:
        if (value.window_angle_degrees != 90.0)
            reject("Sliding window cannot carry a dormant casement angle");
        break;
    }
}

std::string_view window_layout_kind_name(WindowLayoutKind kind) noexcept {
    switch (kind) {
    case WindowLayoutKind::fixed: return "fixed";
    case WindowLayoutKind::double_fixed: return "double_fixed";
    case WindowLayoutKind::triple_fixed: return "triple_fixed";
    case WindowLayoutKind::casement: return "casement";
    case WindowLayoutKind::sliding: return "sliding";
    }
    return "invalid";
}

std::optional<WindowLayoutKind> parse_window_layout_kind(std::string_view value) noexcept {
    if (value == "fixed") return WindowLayoutKind::fixed;
    if (value == "double_fixed") return WindowLayoutKind::double_fixed;
    if (value == "triple_fixed") return WindowLayoutKind::triple_fixed;
    if (value == "casement") return WindowLayoutKind::casement;
    if (value == "sliding") return WindowLayoutKind::sliding;
    return std::nullopt;
}

std::string_view opening_assembly_kind_name(OpeningAssemblyKind kind) noexcept {
    switch (kind) {
    case OpeningAssemblyKind::door:
        return "door";
    case OpeningAssemblyKind::window:
        return "window";
    }
    return "invalid";
}

std::optional<OpeningAssemblyKind>
parse_opening_assembly_kind(std::string_view value) noexcept {
    if (value == "door") return OpeningAssemblyKind::door;
    if (value == "window") return OpeningAssemblyKind::window;
    return std::nullopt;
}

OpeningAssembly default_opening_assembly(OpeningAssemblyKind kind) {
    OpeningAssembly result;
    result.kind = kind;
    result.glazing_thickness_m = kind == OpeningAssemblyKind::window ? 0.02 : 0.0;
    validate_opening_assembly(result);
    return result;
}

OpeningAssembly parse_opening_assembly(const nlohmann::json& value) {
    if (!value.is_object() ||
        !value.contains("version") || !value.contains("kind") ||
        !value.contains("frame_width_m") || !value.contains("frame_depth_m") ||
        !value.contains("panel_thickness_m") ||
        !value.contains("glazing_thickness_m") || !value.contains("inset_m")) {
        reject("Opening assembly properties require version, kind, frame_width_m, frame_depth_m, panel_thickness_m, glazing_thickness_m, and inset_m");
    }
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) ||
        (version != 1 && version != 2)) {
        reject("Opening assembly version must be 1 or 2");
    }
    const bool descriptor = version == 2;
    if ((!descriptor && value.size() != 7) ||
        (descriptor && (value.size() != 12 || !value.contains("window_layout") ||
         !value.contains("window_hinge_at_end") || !value.contains("window_open_left") ||
         !value.contains("window_angle_degrees") || !value.contains("window_slide_fraction"))))
        reject("Opening assembly properties must match the exact versioned schema");
    const auto& kind = value.at("kind");
    if (!kind.is_string()) reject("Opening assembly kind must be a string");
    const auto parsed_kind = parse_opening_assembly_kind(kind.get<std::string>());
    if (!parsed_kind.has_value()) reject("Opening assembly kind is unsupported");
    if (descriptor && *parsed_kind != OpeningAssemblyKind::window)
        reject("Version 2 opening assembly descriptors require a window");
    OpeningAssembly result;
    result.kind = *parsed_kind;
    const auto read_number = [&](const char* key, double& output) {
        const auto& field = value.at(key);
        if (!field.is_number()) reject("Opening assembly dimensions must be finite numbers");
        output = field.get<double>();
        if (!std::isfinite(output)) reject("Opening assembly dimensions must be finite numbers");
    };
    read_number("frame_width_m", result.frame_width_m);
    read_number("frame_depth_m", result.frame_depth_m);
    read_number("panel_thickness_m", result.panel_thickness_m);
    read_number("glazing_thickness_m", result.glazing_thickness_m);
    read_number("inset_m", result.inset_m);
    if (descriptor) {
        const auto& layout = value.at("window_layout");
        if (!layout.is_string()) reject("Window assembly layout must be a string");
        const auto parsed = parse_window_layout_kind(layout.get<std::string>());
        if (!parsed) reject("Window assembly layout is unsupported");
        result.window_layout = *parsed;
        if (!value.at("window_hinge_at_end").is_boolean() ||
            !value.at("window_open_left").is_boolean())
            reject("Window assembly handing fields must be booleans");
        result.window_hinge_at_end = value.at("window_hinge_at_end").get<bool>();
        result.window_open_left = value.at("window_open_left").get<bool>();
        read_number("window_angle_degrees", result.window_angle_degrees);
        read_number("window_slide_fraction", result.window_slide_fraction);
    }
    validate_opening_assembly(result);
    return result;
}

nlohmann::json opening_assembly_json(const OpeningAssembly& value) {
    validate_opening_assembly(value);
    nlohmann::json result{{"version", 1},
            {"kind", opening_assembly_kind_name(value.kind)},
            {"frame_width_m", value.frame_width_m},
            {"frame_depth_m", value.frame_depth_m},
            {"panel_thickness_m", value.panel_thickness_m},
            {"glazing_thickness_m", value.glazing_thickness_m},
            {"inset_m", value.inset_m}};
    if (!canonical_window_descriptor(value)) {
        result["version"] = 2;
        result["window_layout"] = window_layout_kind_name(value.window_layout);
        result["window_hinge_at_end"] = value.window_hinge_at_end;
        result["window_open_left"] = value.window_open_left;
        result["window_angle_degrees"] = value.window_angle_degrees;
        result["window_slide_fraction"] = value.window_slide_fraction;
    }
    return result;
}

}  // namespace sketch
