#include "sketch/project_visibility.hpp"

#include "sketch/project_organization.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/wall_semantics.hpp"

#include <algorithm>
#include <exception>
#include <vector>

namespace sketch {

std::set<std::string, std::less<>> derived_join_presentation_entities(
    const DocumentSnapshot& snapshot,
    const std::set<std::string, std::less<>>& visible_ids) {
    auto presentation_ids = visible_ids;
    for (const auto& [id, entity] : snapshot.entities()) {
        const bool wall_join = entity.type == "wall_join";
        if (!wall_join && entity.type != "roof_join") continue;
        std::vector<std::string> members;
        try {
            members = wall_join ? parse_wall_join(entity.properties, id).wall_ids
                                : parse_roof_join(entity.properties, id).roof_ids;
        } catch (const std::exception&) {
            // Keep malformed entities available to the view's diagnostic path,
            // without allowing them to suppress any source presentation.
            continue;
        }
        const bool fused = visible_ids.contains(id) &&
            std::all_of(members.begin(), members.end(), [&](const auto& member_id) {
                const auto member = snapshot.entities().find(member_id);
                return visible_ids.contains(member_id) && member != snapshot.entities().end() &&
                       member->second.type == (wall_join ? "wall" : "roof");
            });
        if (fused) {
            for (const auto& member_id : members) presentation_ids.erase(member_id);
        } else {
            presentation_ids.erase(id);
        }
    }
    return presentation_ids;
}

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
