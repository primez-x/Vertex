#pragma once

#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/wall_semantics.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

// True when two finite straight baselines have a non-parallel finite contact
// with at least one contact station strictly inside its baseline. Pure shared
// endpoints, collinear overlap, and unsupported numeric cases return false.
[[nodiscard]] bool wall_baselines_have_interior_contact(
    const Segment& first, const Segment& second) noexcept;

// Subtracts contacted walls' closed material polygons from visible straight
// wall strokes. Contacts are unordered wall-ID pairs already qualified by the
// document layer. Footprints and all unsupported plans remain unchanged.
void apply_wall_plan_junction_strokes(
    std::map<std::string, WallPlanGeometry, std::less<>>& plans,
    const std::map<std::string, Wall, std::less<>>& walls,
    const std::vector<std::pair<std::string, std::string>>& contacts);

} // namespace sketch
