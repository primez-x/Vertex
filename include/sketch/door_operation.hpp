#pragma once
#include "sketch/geometry.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

namespace sketch {
enum class DoorOperationKind {
    hinged = 0, double_hinged = 1, sliding = 2, overhead_tilt_up = 3,
    barn_sliding = 4, pocket_sliding = 5, bifold = 6, double_bifold = 7
};
[[nodiscard]] std::string_view door_operation_kind_name(DoorOperationKind kind) noexcept;
// The historical sliding kind has two half-width panels. These mechanisms
// instead translate one full-width panel toward the selected destination jamb.
[[nodiscard]] bool is_single_panel_sliding_door(DoorOperationKind kind) noexcept;
// Fractional mechanisms require an explicit manufactured door assembly.
[[nodiscard]] bool uses_door_opening_fraction(DoorOperationKind kind) noexcept;
// Start/end jamb follows increasing distance along the host baseline. Swing
// side is left/right of that same direction, independent of the chosen hinge.
// Barn/pocket sliding uses the selected jamb as its travel destination; bifold
// uses it as the pinned jamb. Barn side selects the wall face, bifold side the
// fold side; pocket side is retained but its panel travels on the wall centre.
struct DoorOperation {
    bool hinge_at_end{};
    bool swing_left{true};
    double angle_degrees{90};
    DoorOperationKind kind{DoorOperationKind::hinged};
    // Sliding travel as a fraction of one half-width panel. The selected jamb
    // identifies the moving half; side selects its track. Other kinds use zero.
    double slide_fraction{};
    // Fractional opening of tilt-up, barn/pocket sliding and bifold doors:
    // zero is closed; one is horizontal/full-width travel/fully folded,
    // respectively. These kinds require angle_degrees=90 and slide_fraction=0.
    // Hinged and historical two-panel sliding kinds use zero.
    double opening_fraction{};
    bool operator==(const DoorOperation&) const = default;
};
[[nodiscard]] DoorOperation decode_door_operation(const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_door_operation(const DoorOperation& value);
// A planar leaf and analytic swing arc. Curved hosts use the straight chord
// between jambs; this is a plan symbol, not a manufactured door solid. Sliding
// track centre lines have schematic separation (2% of width); use the hosted
// assembly projection for the physical panel thickness and track spacing.
// The overhead fallback shows the top hinge and panel projection without a
// swing arc and requires the explicit panel height. Use the hosted assembly
// projection for the manufactured frame, panel thickness and hinge position.
// Barn, pocket and bifold require straight hosts. Their fallback centre lines
// follow the actual analytical travel/folding path, without swing arcs. Barn
// uses a schematic wall-face offset (2% of width); pocket uses the host centre
// line. The hosted assembly supplies physical panel thickness and track space.
[[nodiscard]] Boundary door_plan_symbol(const Segment& host, double offset, double width,
    const DoorOperation& operation, std::optional<double> opening_height_metres = std::nullopt);
} // namespace sketch
