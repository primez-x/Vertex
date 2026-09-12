#pragma once
#include "sketch/geometry.hpp"
#include <nlohmann/json.hpp>

namespace sketch {
// Start/end jamb follows increasing distance along the host baseline. Swing
// side is left/right of that same direction, independent of the chosen hinge.
struct DoorOperation {
    bool hinge_at_end{};
    bool swing_left{true};
    double angle_degrees{90};
    bool operator==(const DoorOperation&) const = default;
};
[[nodiscard]] DoorOperation decode_door_operation(const nlohmann::json& value);
[[nodiscard]] nlohmann::json encode_door_operation(const DoorOperation& value);
// A planar leaf and analytic swing arc. Curved hosts use the straight chord
// between jambs; this is a plan symbol, not a manufactured door solid.
[[nodiscard]] Boundary door_plan_symbol(const Segment& host, double offset, double width,
    const DoorOperation& operation);
} // namespace sketch
