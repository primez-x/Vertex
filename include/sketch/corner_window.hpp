#pragma once

#include "sketch/opening_assembly.hpp"
#include "sketch/wall_semantics.hpp"

#include <array>
#include <map>
#include <string>

namespace sketch {

struct Entity;

// One manufactured fixed window owns two ordinary, bare wall cuts. Widths
// extend away from the selected shared endpoint of each directed baseline.
// The owner retains the assembly; its opening children never retain a second
// assembly or an operating mechanism.
struct CornerWindow {
    std::string id;
    std::array<std::string, 2> wall_ids;
    std::array<std::string, 2> opening_ids;
    std::array<bool, 2> at_start;
    std::array<double, 2> widths;
    double sill{};
    double height{};
    OpeningAssembly assembly;

    bool operator==(const CornerWindow&) const = default;
};

// Version-one required fields are decoded without rejecting unrelated owner
// metadata. To edit an existing entity, merge these known properties into its
// retained properties instead of replacing the complete properties object.
[[nodiscard]] CornerWindow parse_corner_window(const Entity& entity);
[[nodiscard]] nlohmann::json corner_window_properties(const CornerWindow& value);
// Hosts must carry compatible physical elevations. No document or host
// is mutated. Throws std::invalid_argument if either leg cannot be admitted.
[[nodiscard]] std::array<HostedOpening, 2> corner_window_cuts(
    const CornerWindow& value, const std::array<Wall, 2>& walls);
// Complete snapshot admission includes child ownership, actual host geometry,
// raw context/level placement and every saved phase alternative, including
// inactive ones. Version one conservatively requires equal raw host elevations
// and identical retained vertical-placement bindings.
void validate_corner_window_state(
    const std::map<std::string, Entity, std::less<>>& entities);

} // namespace sketch
