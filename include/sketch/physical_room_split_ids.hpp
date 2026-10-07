#pragma once

#include <string>
#include <vector>

namespace sketch {
// Frozen once by command preparation. Empty child lists are meaningful: a
// remote current owner in the captured plane needs descriptor continuation.
struct WallSplitPhysicalRoomIds {
    std::string boundary_id;
    std::vector<std::string> new_segment_ids;
    std::vector<std::string> new_vertex_ids;
};
} // namespace sketch
