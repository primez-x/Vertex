#include "sketch/wall_plan_network.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace sketch;
using Json = nlohmann::json;
using Walls = std::map<std::string, Wall, std::less<>>;
using Domains = std::map<std::string, Json, std::less<>>;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool close(double actual, double expected) {
    return std::abs(actual - expected) < 1e-9;
}

bool close(Vec2 actual, Vec2 expected) {
    return close(actual.x, expected.x) && close(actual.y, expected.y);
}

bool close(const Segment& actual, const Segment& expected) {
    return close(actual.start, expected.start) && close(actual.end, expected.end) &&
        close(actual.sweep_radians, expected.sweep_radians);
}

void require_boundary(const Boundary& actual, const Boundary& expected,
    std::string_view message) {
    require(actual.size() == expected.size(), message);
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(close(actual[i], expected[i]), message);
}

bool contains_segment(const Boundary& actual, const Segment& expected) {
    for (const auto& segment : actual) if (close(segment, expected)) return true;
    return false;
}

Wall wall(std::string id, Segment baseline, double thickness = .2, double elevation = 0) {
    return {std::move(id), baseline, thickness, 3, elevation, {}, {}, std::nullopt};
}

Walls corner() {
    return {{"a", wall("a", {{0, 0}, {4, 0}, 0}, .2)},
        {"b", wall("b", {{4, 0}, {4, 3}, 0}, .4)}};
}

Domains shared_domain(const Walls& walls) {
    Domains result;
    for (const auto& [id, value] : walls) {
        (void)value;
        result.emplace(id, "one-contact-domain");
    }
    return result;
}

void require_capped(const Walls& walls, const Domains& domains) {
    const auto plans = wall_plan_network_geometry(walls, domains);
    for (const auto& [id, value] : walls) {
        const auto expected = wall_plan_footprint(value.baseline, value.openings, value.thickness);
        require_boundary(plans.at(id).footprint, expected, "ineligible footprint must stay capped");
        require_boundary(plans.at(id).strokes, expected, "ineligible strokes must stay capped");
        require(!plans.at(id).joined_start && !plans.at(id).joined_end,
            "capped endpoints must not advertise a join");
    }
}

void unequal_miter_and_reversal() {
    auto walls = corner();
    auto plans = wall_plan_network_geometry(walls, shared_domain(walls));
    const Boundary horizontal{
        {{0, .1}, {3.8, .1}, 0}, {{3.8, .1}, {4.2, -.1}, 0},
        {{4.2, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0}};
    const Boundary vertical{
        {{3.8, .1}, {3.8, 3}, 0}, {{3.8, 3}, {4.2, 3}, 0},
        {{4.2, 3}, {4.2, -.1}, 0}, {{4.2, -.1}, {3.8, .1}, 0}};
    require_boundary(plans.at("a").footprint, horizontal, "horizontal unequal miter coordinates");
    require_boundary(plans.at("b").footprint, vertical, "vertical unequal miter coordinates");
    require_boundary(plans.at("a").strokes, {horizontal[0], horizontal[2], horizontal[3]},
        "horizontal shared cap must be removed");
    require_boundary(plans.at("b").strokes, {vertical[0], vertical[1], vertical[2]},
        "vertical shared cap must be removed");
    require(plans.at("a").joined_end && plans.at("b").joined_start, "mutual corner joins");
    std::swap(walls.at("a").baseline.start, walls.at("a").baseline.end);
    plans = wall_plan_network_geometry(walls, shared_domain(walls));
    require_boundary(plans.at("a").footprint,
        {{{4.2, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0},
         {{0, .1}, {3.8, .1}, 0}, {{3.8, .1}, {4.2, -.1}, 0}},
        "reversed baseline retains physical corner");
    require_boundary(plans.at("b").footprint, vertical, "reversal does not move neighbor");
    require(plans.at("a").joined_start && !plans.at("a").joined_end,
        "reversal moves the joined endpoint");
}

void domains_and_elevation() {
    auto walls = corner();
    auto domains = shared_domain(walls);
    domains.at("b") = "another-domain";
    require_capped(walls, domains);
    // The adapter's [presence,value] token must distinguish absent and null.
    domains.at("a") = Json::array({false, nullptr});
    domains.at("b") = Json::array({true, nullptr});
    require_capped(walls, domains);
    // Preserve JSON equality rather than introducing dump-string equality.
    domains.at("a") = Json::array({true, 1});
    domains.at("b") = Json::array({true, 1.0});
    const auto equivalent = wall_plan_network_geometry(walls, domains);
    require(equivalent.at("a").joined_end && equivalent.at("b").joined_start,
        "numerically equal JSON domains must retain prior compatibility");
    domains = shared_domain(walls);
    walls.at("b").elevation = default_geometry_tolerance_metres * .5;
    const auto within = wall_plan_network_geometry(walls, domains);
    require(within.at("a").joined_end && within.at("b").joined_start,
        "within-tolerance elevation must retain joins");
    walls.at("b").elevation = default_geometry_tolerance_metres * 2;
    require_capped(walls, domains);
}

void openings_and_mutual_short_run_fallback() {
    auto walls = corner();
    walls.at("b").openings = {{"flush", 0, .75, 0, 2.1}};
    require_capped(walls, shared_domain(walls));
    walls.at("b").openings = {{"full", 0, 3, 0, 2.1}};
    require_capped(walls, shared_domain(walls));
    const auto cut = wall_plan_network_geometry(walls, shared_domain(walls));
    require(cut.at("b").footprint.empty() && cut.at("b").strokes.empty(),
        "fully cut plan neighbor remains empty");
    walls = corner();
    walls.at("a").openings = {{"short-run", 3.9, .05, 0, 2.1}};
    require_capped(walls, shared_domain(walls));
    // Near-parallel acute endpoints exceed the existing guarded miter limit.
    walls = {{"a", wall("a", {{0, 0}, {4, 0}, 0})},
        {"b", wall("b", {{4, 0}, {0, .01}, 0})}};
    require_capped(walls, shared_domain(walls));
}

void partition_and_cross_material() {
    Walls walls{{"host", wall("host", {{0, 0}, {4, 0}, 0})},
        {"branch", wall("branch", {{2, 0}, {2, 3}, 0}, .4)}};
    auto plans = wall_plan_network_geometry(walls, shared_domain(walls));
    require(contains_segment(plans.at("host").strokes, {{0, .1}, {1.8, .1}, 0}) &&
        contains_segment(plans.at("host").strokes, {{2.2, .1}, {4, .1}, 0}),
        "T host face splits across actual partition thickness");
    require(contains_segment(plans.at("host").strokes, {{4, -.1}, {0, -.1}, 0}),
        "T exterior host face stays continuous");
    require(contains_segment(plans.at("branch").strokes, {{1.8, .1}, {1.8, 3}, 0}) &&
        contains_segment(plans.at("branch").strokes, {{2.2, 3}, {2.2, .1}, 0}),
        "T branch strokes start at physical host face");
    require_boundary(plans.at("branch").footprint,
        wall_plan_footprint(walls.at("branch").baseline, {}, .4), "T retains full branch footprint");
    walls.at("host").openings = {{"void", 1.5, 1, 0, 2.1}};
    plans = wall_plan_network_geometry(walls, shared_domain(walls));
    require_boundary(plans.at("branch").strokes, plans.at("branch").footprint,
        "partition in host opening stays capped");
    walls.at("host").openings.clear();
    walls.at("branch").baseline = {{2, -3}, {2, 3}, 0};
    plans = wall_plan_network_geometry(walls, shared_domain(walls));
    require(contains_segment(plans.at("host").strokes, {{4, -.1}, {2.2, -.1}, 0}) &&
        contains_segment(plans.at("host").strokes, {{1.8, -.1}, {0, -.1}, 0}),
        "X clips both sides of crossed host material");
    require(contains_segment(plans.at("branch").strokes, {{1.8, -3}, {1.8, -.1}, 0}) &&
        contains_segment(plans.at("branch").strokes, {{1.8, .1}, {1.8, 3}, 0}),
        "X clips crossed partition face only inside the host");
}

void curved_caps_and_source_preservation() {
    Walls walls{{"arc", wall("arc", {{0, 0}, {1, 1}, std::numbers::pi / 2})},
        {"line", wall("line", {{1, 1}, {1, 4}, 0}, .3)}};
    const auto arc = walls.at("arc").baseline;
    const auto line = walls.at("line").baseline;
    const auto domains = shared_domain(walls);
    require_capped(walls, domains);
    require(close(walls.at("arc").baseline, arc) && close(walls.at("line").baseline, line) &&
        walls.at("arc").thickness == .2 && walls.at("line").thickness == .3,
        "network does not modify admitted sources");
    require(wall_plan_network_geometry({}, {}).empty(), "empty admitted network stays empty");
}

void multiway_and_near_miter_partner_material() {
    auto walls = corner();
    walls.emplace("c", wall("c", {{4, 0}, {5, 1}, 0}, .3));
    auto plans = wall_plan_network_geometry(walls, shared_domain(walls));
    for (const auto& [id, value] : walls) {
        require_boundary(plans.at(id).footprint,
            wall_plan_footprint(value.baseline, {}, value.thickness),
            "ambiguous three-way endpoint retains each full capped footprint");
        require(!plans.at(id).joined_start && !plans.at(id).joined_end,
            "three-way endpoints do not claim a pairwise miter");
    }
    require(!contains_segment(plans.at("a").strokes, {{4, .1}, {4, -.1}, 0}),
        "three-way material contact removes the original internal cap stroke");
    walls = corner();
    walls.at("b").thickness = .2;
    walls.emplace("partition", wall("partition", {{3.95, 0}, {3.95, -3}, 0}));
    plans = wall_plan_network_geometry(walls, shared_domain(walls));
    const auto& strokes = plans.at("partition").strokes;
    require(!contains_segment(strokes, {{4.0, 0}, {4.05, 0}, 0}) &&
        !contains_segment(strokes, {{4.05, 0}, {4.05, -.05}, 0}),
        "near-miter partition clips material of both qualified corner partners");
    require(contains_segment(strokes, {{4.05, -.1}, {4.05, -3}, 0}) &&
        contains_segment(strokes, {{3.85, -3}, {3.85, -.1}, 0}),
        "near-miter partition retains its real exterior faces");
    require_boundary(plans.at("partition").footprint,
        wall_plan_footprint(walls.at("partition").baseline, {}, .2),
        "near-miter stroke clipping keeps complete partition picking geometry");
}
} // namespace

int main() {
    const std::pair<std::string_view, void (*)()> cases[]{
        {"unequal_miter_and_reversal", unequal_miter_and_reversal},
        {"domains_and_elevation", domains_and_elevation},
        {"openings_and_mutual_short_run_fallback", openings_and_mutual_short_run_fallback},
        {"partition_and_cross_material", partition_and_cross_material},
        {"curved_caps_and_source_preservation", curved_caps_and_source_preservation},
        {"multiway_and_near_miter_partner_material", multiway_and_near_miter_partner_material}};
    for (const auto& [name, run] : cases) {
        try { run(); }
        catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "Wall plan network tests passed\n";
    return 0;
}
