#pragma once
#include "sketch/geometry.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace sketch {
enum class DoorOperationKind { hinged, double_hinged, sliding, overhead_tilt_up };
// Start/end jamb follows increasing distance along the host baseline. Swing
// side is left/right of that same direction, independent of the chosen hinge.
struct DoorOperation {
    bool hinge_at_end{};
    bool swing_left{true};
    double angle_degrees{90};
    DoorOperationKind kind{DoorOperationKind::hinged};
    // Sliding travel as a fraction of one half-width panel. The selected jamb
    // identifies the moving half; side selects its track. Other kinds use zero.
    double slide_fraction{};
    // Rigid tilt-up travel about the top horizontal hinge: zero is closed,
    // one is horizontal. Side selects the host normal; other kinds use zero.
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
[[nodiscard]] Boundary door_plan_symbol(const Segment& host, double offset, double width,
    const DoorOperation& operation, std::optional<double> opening_height_metres = std::nullopt);
} // namespace sketch
