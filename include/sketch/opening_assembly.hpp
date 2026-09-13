#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace sketch {

// A hosted opening keeps its wall cut as the authoritative geometry.  This
// profile describes the replaceable manufactured parts presented inside that
// cut.  The same schema serves doors and windows; `panel_thickness_m` is the
// door leaf thickness or the window sash depth, while a positive glazing
// thickness adds a real pane to either family.
enum class OpeningAssemblyKind { door, window };

struct OpeningAssembly {
    OpeningAssemblyKind kind{OpeningAssemblyKind::door};
    double frame_width_m{0.08};
    double frame_depth_m{0.12};
    double panel_thickness_m{0.04};
    double glazing_thickness_m{0.0};
    // Signed offset from the wall centreline toward its left-hand normal.
    double inset_m{0.0};

    bool operator==(const OpeningAssembly&) const = default;
};

void validate_opening_assembly(const OpeningAssembly& value);
[[nodiscard]] std::string_view opening_assembly_kind_name(OpeningAssemblyKind kind) noexcept;
[[nodiscard]] std::optional<OpeningAssemblyKind>
parse_opening_assembly_kind(std::string_view value) noexcept;
[[nodiscard]] OpeningAssembly default_opening_assembly(OpeningAssemblyKind kind);
[[nodiscard]] OpeningAssembly parse_opening_assembly(const nlohmann::json& value);
[[nodiscard]] nlohmann::json opening_assembly_json(const OpeningAssembly& value);

}  // namespace sketch
