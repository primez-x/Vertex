#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace sketch {

// A hosted opening keeps its wall cut as the authoritative geometry.  This
// profile describes the replaceable manufactured parts presented inside that
// cut. The same schema serves doors, windows and framed passages. The
// `panel_thickness_m` is the door leaf thickness or window sash depth; positive glazing
// thickness adds a real pane to a door or window. A passage has only a frame.
enum class OpeningAssemblyKind { door, window, passage };
enum class WindowLayoutKind { fixed, double_fixed, triple_fixed, casement, sliding, bay, bow };

struct OpeningAssembly {
    OpeningAssemblyKind kind{OpeningAssemblyKind::door};
    double frame_width_m{0.08};
    double frame_depth_m{0.12};
    double panel_thickness_m{0.04};
    double glazing_thickness_m{0.0};
    // Signed offset from the wall centreline toward its left-hand normal.
    double inset_m{0.0};
    // Window-only descriptor. A sliding window's hinge flag selects the
    // moving end half, and open_left selects its positive-normal track.
    // Canonical defaults retain the legacy v1 persistence representation.
    WindowLayoutKind window_layout{WindowLayoutKind::fixed};
    bool window_hinge_at_end{false};
    bool window_open_left{true};
    double window_angle_degrees{90.0};
    double window_slide_fraction{0.0};
    // Bay-only: depth beyond the selected wall face and front width/mouth
    // width ratio. open_left selects the projecting side of the host.
    double window_bay_projection_m{0.0};
    double window_bay_front_fraction{0.5};
    // Bow-only: five fixed panes project beyond the selected wall face.
    // open_left selects the projecting side of the host.
    double window_bow_projection_m{0.0};

    bool operator==(const OpeningAssembly&) const = default;
};

void validate_opening_assembly(const OpeningAssembly& value);
[[nodiscard]] std::string_view opening_assembly_kind_name(OpeningAssemblyKind kind) noexcept;
// Legacy entity family inference intentionally leaves "opening" bare. A framed
// passage is admitted only by an explicit version-four assembly profile.
[[nodiscard]] std::optional<OpeningAssemblyKind>
parse_opening_assembly_kind(std::string_view value) noexcept;
[[nodiscard]] std::string_view window_layout_kind_name(WindowLayoutKind kind) noexcept;
[[nodiscard]] std::optional<WindowLayoutKind>
parse_window_layout_kind(std::string_view value) noexcept;
[[nodiscard]] OpeningAssembly default_opening_assembly(OpeningAssemblyKind kind);
[[nodiscard]] OpeningAssembly parse_opening_assembly(const nlohmann::json& value);
[[nodiscard]] nlohmann::json opening_assembly_json(const OpeningAssembly& value);

}  // namespace sketch
