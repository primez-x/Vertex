#include "sketch/physical_wall_spaces.hpp"

namespace sketch {
PhysicalWallSpaces detect_physical_wall_spaces(const DocumentSnapshot& document,
    std::string_view selected_wall_id) {
    return detect_physical_wall_spaces(document.entities(),selected_wall_id);
}
PhysicalWallSpaces detect_physical_wall_spaces(const DocumentSnapshot& document,
    const DrawingContext& context, double effective_elevation_m) {
    return detect_physical_wall_spaces(document.entities(),context,effective_elevation_m);
}
PhysicalWallSpaces detect_physical_wall_spaces(const DocumentSnapshot& document,
    const DrawingContext& context, double effective_elevation_m,
    const PhysicalWallPhaseSelection& selection) {
    return detect_physical_wall_spaces(document.entities(),context,effective_elevation_m,selection);
}
} // namespace sketch
