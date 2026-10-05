#include "sketch/physical_wall_spaces.hpp"

namespace sketch {
PhysicalWallSpaces detect_physical_wall_spaces(const DocumentSnapshot& document,
    std::string_view selected_wall_id) {
    return detect_physical_wall_spaces(document.entities(),selected_wall_id);
}
} // namespace sketch
