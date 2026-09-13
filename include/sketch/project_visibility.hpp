#pragma once

#include "sketch/document.hpp"

#include <set>
#include <string>

namespace sketch {

// View-only floor and layer masks. The sets contain stable entity IDs and are
// intentionally independent from document state, history, and serialization.
struct ProjectViewFilter {
    std::set<std::string, std::less<>> hidden_floor_ids;
    std::set<std::string, std::less<>> hidden_layer_ids;

    bool operator==(const ProjectViewFilter&) const = default;
};

// Derives organization once and returns the stable IDs that presentation may
// display. Invalid or incomplete organization links remain visible so a view
// filter cannot conceal data whose placement is unresolved or contradictory.
[[nodiscard]] std::set<std::string, std::less<>> visible_project_entities(
    const DocumentSnapshot& snapshot, const ProjectViewFilter& filter);

// Use a fused wall/roof join only when it and every source member are visible.
// Otherwise retain individually visible members. This is presentation-only.
[[nodiscard]] std::set<std::string, std::less<>> derived_join_presentation_entities(
    const DocumentSnapshot& snapshot,
    const std::set<std::string, std::less<>>& visible_ids);

}  // namespace sketch
