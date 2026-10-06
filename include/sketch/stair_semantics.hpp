#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

struct Vec3 { double x{}; double y{}; double z{}; };
struct StairLanding { double depth{}; double thickness{}; };
struct StairLevelConnection {
    std::string graph_entity_id, link_id, lower_level_id, upper_level_id;
    bool operator==(const StairLevelConnection&) const = default;
};
enum class StairTurn { straight, left_quarter, right_quarter, left_half, right_half };
struct StairFlightRecord { std::string id; std::size_t riser_count{}; };
struct StairConnectingLanding {
    std::string id;
    double depth{};
    double thickness{};
    StairTurn turn{StairTurn::straight};
    double return_gap{};
};
struct StairFlight {
    std::string id;
    Vec3 base_position{};
    double orientation_radians{};
    std::size_t riser_count{};
    double total_rise{};
    double going{};
    double width{};
    std::optional<StairLanding> top_landing;
    std::optional<StairLevelConnection> level_connection;
    // Empty topology is the unchanged version-1 straight flight.
    std::vector<StairFlightRecord> flights;
    std::vector<StairConnectingLanding> landings;
};
enum class StairRailingSide { left, right };
struct StairRailingHost {
    std::string stair_id;
    std::string flight_id;
    StairRailingSide side{StairRailingSide::left};
    double start_fraction{};
    double end_fraction{1.0};
};
struct Railing {
    std::string id;
    Vec3 base_position{};
    double orientation_radians{};
    double length{};
    double height{};
    double thickness{};
    double post_spacing{};
    std::optional<StairRailingHost> host;
};
using StairPolygon = std::array<Vec3, 4>;
struct StairTreadLayout {
    StairPolygon footprint;
    Vec3 nosing_start, nosing_end;
    double elevation{};
};
struct StairFlightLayout {
    std::string id;
    Vec3 base_position, end_position;
    double orientation_radians{}, run{}, rise{}, riser_height{};
    StairPolygon footprint;
    std::vector<StairTreadLayout> treads;
};
struct StairLandingLayout {
    std::string id;
    StairPolygon footprint;
    double elevation{}, thickness{};
};
struct StairLayout {
    std::vector<StairFlightLayout> flights;
    std::vector<StairLandingLayout> landings;
};
struct StairRailPostLayout { Vec3 base, top; };
struct HostedRailingLayout {
    Vec3 rail_start, rail_end;
    double orientation_radians{};
    std::vector<StairRailPostLayout> posts;
};

// All dimensions use metres. Throws invalid_argument on invalid authoring data.
void validate_stair(const StairFlight& stair);
void validate_railing(const Railing& railing);
[[nodiscard]] StairLayout derive_stair_layout(const StairFlight& stair);
// Fractions traverse x=[thickness/2,run-thickness/2] on the flight.
// The pitch line extends the true tread nosings z=riser+x*riser/going;
// a one-riser flight instead uses a horizontal line at its tread elevation.
// Posts stay wholly on tread footprints; spacing is along the 3D pitch line.
[[nodiscard]] HostedRailingLayout derive_hosted_railing_layout(
    const Railing& railing, const StairFlight& current_host);
[[nodiscard]] std::vector<std::string> stair_child_ids(const StairFlight& stair);
[[nodiscard]] nlohmann::json encode_stair_properties(const StairFlight& stair);
[[nodiscard]] StairFlight decode_stair_properties(std::string_view id, const nlohmann::json& properties);
[[nodiscard]] nlohmann::json encode_railing_properties(const Railing& railing);
[[nodiscard]] Railing decode_railing_properties(std::string_view id, const nlohmann::json& properties);

} // namespace sketch
