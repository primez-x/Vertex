#pragma once

#include "sketch/hosted_opening_geometry.hpp"

#include <map>
#include <string>

namespace sketch {

// Derived plan footprints and visible strokes for an admitted wall network.
// Every wall ID must match its map key and have a contact-domain entry. Domain
// values are opaque and compared with exact JSON equality; callers supply
// placement compatibility without exposing entities or document services.
// Elevations must also agree within default_geometry_tolerance_metres.
// Preserves capped curves/opening endpoints, symmetric guarded miter fallback,
// complete picking footprints and straight T/X internal-stroke subtraction.
// Does not admit malformed sources, resolve placement, or modify input walls.
[[nodiscard]] std::map<std::string, WallPlanGeometry, std::less<>>
wall_plan_network_geometry(
    const std::map<std::string, Wall, std::less<>>& walls,
    const std::map<std::string, nlohmann::json, std::less<>>& contact_domains);

} // namespace sketch
