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
    if (!value.is_object() || value.size() != 7 ||
        !value.contains("version") || !value.contains("kind") ||
        !value.contains("frame_width_m") || !value.contains("frame_depth_m") ||
        !value.contains("panel_thickness_m") ||
        !value.contains("glazing_thickness_m") || !value.contains("inset_m")) {
        reject("Opening assembly properties must contain exactly version, kind, frame_width_m, frame_depth_m, panel_thickness_m, glazing_thickness_m, and inset_m");
    }
    const auto& version = value.at("version");
    if ((!version.is_number_integer() && !version.is_number_unsigned()) || version != 1) {
        reject("Opening assembly version must be 1");
    }
    const auto& kind = value.at("kind");
    if (!kind.is_string()) reject("Opening assembly kind must be a string");
    const auto parsed_kind = parse_opening_assembly_kind(kind.get<std::string>());
    if (!parsed_kind.has_value()) reject("Opening assembly kind is unsupported");
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
    validate_opening_assembly(result);
    return result;
}

nlohmann::json opening_assembly_json(const OpeningAssembly& value) {
    validate_opening_assembly(value);
    return {{"version", 1},
            {"kind", opening_assembly_kind_name(value.kind)},
            {"frame_width_m", value.frame_width_m},
            {"frame_depth_m", value.frame_depth_m},
            {"panel_thickness_m", value.panel_thickness_m},
            {"glazing_thickness_m", value.glazing_thickness_m},
            {"inset_m", value.inset_m}};
}

}  // namespace sketch
