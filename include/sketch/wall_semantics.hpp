#pragma once

#include "sketch/geometry.hpp"

#include <string>
#include <vector>

namespace sketch {

// Offsets and widths follow the hosted wall's centreline, including its arc.
struct HostedOpening {
    std::string id;
    double offset{};
    double width{};
    double sill{};
    double height{};
};

struct Wall {
    std::string id;
    Segment baseline;
    double thickness{};
    double height{};
    double elevation{};
    std::vector<HostedOpening> openings;
};

// Validate the shared semantic contract used by document editing and solid
// construction. The function throws std::invalid_argument on invalid input
// and never modifies the supplied wall.
void validate_wall_semantics(const Wall& wall);

}  // namespace sketch
