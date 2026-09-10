#pragma once

namespace sketch {

// The solver preview and persisted hard-relation validator use one contract.
inline constexpr double constraint_linear_tolerance_metres = 1e-6;
inline constexpr double constraint_angular_tolerance_radians = 1e-8;

} // namespace sketch
