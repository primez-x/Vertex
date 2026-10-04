#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sketch {
struct MeasurementLineworkSourceCheck {
    bool current{};
    std::string diagnostic;
    // A unique same-topology face, ordered to match the retained area's edges.
    // Its stable identities and metadata are owned by the document adapter.
    std::optional<Boundary> proposed_boundary;
    nlohmann::json proposed_lineage;
    // Group refreshes replace boundary, outer lineage and canonical member
    // record together. Indices identify the distinct current graph faces.
    nlohmann::json proposed_group;
    std::vector<std::size_t> group_face_indices;
};
// Derived geometry is rebuilt once per complete drawing context. Ordinary
// boundaries without measured-line lineage are omitted. No inputs are changed.
[[nodiscard]] std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>
measurement_linework_source_checks(const std::map<std::string,Entity,std::less<>>& entities,
    const std::set<std::string,std::less<>>* semantic_visible=nullptr);
[[nodiscard]] bool measurement_linework_sources_visible(const Entity& area,
    const std::set<std::string,std::less<>>* semantic_visible);
[[nodiscard]] inline bool measurement_linework_source_current(
    const std::map<std::string,MeasurementLineworkSourceCheck,std::less<>>& checks,const Entity& area) {
    if(!area.extensions.contains("measurement_linework_sources") &&
       !(area.type=="measurement_boundary" && area.extensions.contains("measurement_linework_group"))) return true;
    const auto found=checks.find(area.id);
    return found!=checks.end() && found->second.current;
}
} // namespace sketch
