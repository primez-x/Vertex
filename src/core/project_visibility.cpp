#include "sketch/project_visibility.hpp"

#include "sketch/project_organization.hpp"

namespace sketch {

std::set<std::string, std::less<>> visible_project_entities(
    const DocumentSnapshot& snapshot, const ProjectViewFilter& filter) {
    const auto organization = organize_project(snapshot);
    std::set<std::string, std::less<>> visible_ids;

    for (const auto& [id, node] : organization.nodes) {
        // A diagnostic or incomplete placement is deliberately fail-open. A
        // filter only applies when organization resolved the entity's own or
        // inherited floor/layer context without issues.
        if (!node.issues.empty()) {
            visible_ids.insert(id);
            continue;
        }

        const bool hidden_by_floor = !node.context.floor_id.empty() &&
            filter.hidden_floor_ids.contains(node.context.floor_id);
        const bool hidden_by_layer = !node.context.layer_id.empty() &&
            filter.hidden_layer_ids.contains(node.context.layer_id);
        if (!hidden_by_floor && !hidden_by_layer) {
            visible_ids.insert(id);
        }
    }
    return visible_ids;
}

}  // namespace sketch
