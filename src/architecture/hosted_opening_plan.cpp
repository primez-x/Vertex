#include "sketch/hosted_opening_plan.hpp"

#include "sketch/architecture.hpp"
#include "sketch/building_view_projection.hpp"

#include <algorithm>

namespace sketch {

Boundary project_hosted_opening_plan(const Wall& wall, const HostedOpening& opening,
                                     const OpeningAssembly& assembly,
                                     const std::optional<DoorOperation>& operation) {
    Wall checked = wall;
    const auto hosted = std::find_if(checked.openings.begin(), checked.openings.end(),
        [&](const auto& item) { return item.id == opening.id; });
    if (hosted == checked.openings.end()) checked.openings.push_back(opening);
    else if (*hosted != opening)
        throw std::invalid_argument("Hosted plan has a conflicting opening identity");
    validate_hosted_opening_plan_source(checked);
    validate_opening_assembly(assembly);
    // The host and manufactured parts must both be admitted by the same solid
    // engine used for coordinated 3D presentation before claiming this view.
    (void)make_wall(checked);
    if (assembly.kind != OpeningAssemblyKind::door && operation)
        throw std::invalid_argument("Window plan cannot carry a door operation");
    const auto geometry = make_opening_assembly_geometry(checked, opening, assembly, operation);
    BuildingViewFrame frame;
    frame.origin.z = wall.elevation + opening.sill + opening.height * 0.5;
    auto result = project_shape_view(geometry.shape, BuildingViewKind::section, frame);
    if (geometry.door_swing) result.push_back(*geometry.door_swing);
    return result;
}

} // namespace sketch
