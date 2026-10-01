#include "sketch/wall_plan_junctions.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Plans = std::map<std::string, WallPlanGeometry, std::less<>>;
using Walls = std::map<std::string, Wall, std::less<>>;
using Contact = std::pair<std::string, std::string>;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool near(double first, double second, double tolerance = 1e-9) {
    return std::abs(first - second) <= tolerance;
}

bool near(Vec2 first, Vec2 second, double tolerance = 1e-9) {
    return near(first.x, second.x, tolerance) && near(first.y, second.y, tolerance);
}

bool near(const Segment& first, const Segment& second, double tolerance = 1e-9) {
    return near(first.start, second.start, tolerance) &&
        near(first.end, second.end, tolerance) &&
        near(first.sweep_radians, second.sweep_radians, tolerance);
}

bool contains(const Boundary& boundary, const Segment& expected,
              double tolerance = 1e-9) {
    for (const auto& segment : boundary)
        if (near(segment, expected, tolerance)) return true;
    return false;
}

void require_boundary(const Boundary& actual, const Boundary& expected,
                      std::string_view message, double tolerance = 1e-9) {
    require(actual.size() == expected.size(), message);
    for (std::size_t index = 0; index < expected.size(); ++index)
        require(near(actual[index], expected[index], tolerance), message);
}

bool same_boundary(const Boundary& first, const Boundary& second,
                   double tolerance = 1e-9) {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index)
        if (!near(first[index], second[index], tolerance)) return false;
    return true;
}

bool same_plans(const Plans& first, const Plans& second) {
    if (first.size() != second.size()) return false;
    auto a = first.begin(), b = second.begin();
    for (; a != first.end(); ++a, ++b)
        if (a->first != b->first ||
            !same_boundary(a->second.footprint, b->second.footprint) ||
            !same_boundary(a->second.strokes, b->second.strokes) ||
            a->second.joined_start != b->second.joined_start ||
            a->second.joined_end != b->second.joined_end)
            return false;
    return true;
}

Wall make_wall(std::string id, Segment baseline, double thickness = .2,
               std::vector<HostedOpening> openings = {}) {
    return {std::move(id), baseline, thickness, 3.0, 0.0,
            std::move(openings), {}, std::nullopt};
}

WallPlanGeometry make_plan(const Wall& wall) {
    return joined_wall_plan_geometry(wall.baseline, wall.openings,
                                     wall.thickness, {});
}

Plans plans_for(const Walls& walls) {
    Plans result;
    for (const auto& [id, wall] : walls) result.emplace(id, make_plan(wall));
    return result;
}

Vec2 rotate_translate(Vec2 point, double cosine, double sine, Vec2 offset) {
    return {offset.x + point.x * cosine - point.y * sine,
            offset.y + point.x * sine + point.y * cosine};
}

void test_t_junction_subtracts_only_interior_stroke_spans() {
    Walls walls{
        {"host", make_wall("host", {{0, 0}, {4, 0}, 0})},
        {"branch", make_wall("branch", {{2, 0}, {2, 3}, 0})}};
    auto plans = plans_for(walls);
    const auto host_footprint = plans.at("host").footprint;
    const auto branch_footprint = plans.at("branch").footprint;

    apply_wall_plan_junction_strokes(plans, walls, {{"host", "branch"}});

    require_boundary(plans.at("host").footprint, host_footprint,
                     "T-junction subtraction must preserve the closed host footprint");
    require_boundary(plans.at("branch").footprint, branch_footprint,
                     "T-junction subtraction must preserve the closed branch footprint");
    const Boundary host_strokes{
        {{0, .1}, {1.9, .1}, 0}, {{2.1, .1}, {4, .1}, 0},
        {{4, .1}, {4, -.1}, 0}, {{4, -.1}, {0, -.1}, 0},
        {{0, -.1}, {0, .1}, 0}};
    const Boundary branch_strokes{
        {{1.9, .1}, {1.9, 3}, 0}, {{1.9, 3}, {2.1, 3}, 0},
        {{2.1, 3}, {2.1, .1}, 0}};
    require_boundary(plans.at("host").strokes, host_strokes,
                     "the host loses only the branch-covered top-face interval");
    require_boundary(plans.at("branch").strokes, branch_strokes,
                     "the branch sides begin at the host exterior and its buried cap vanishes");
}

void test_x_junction_keeps_only_four_exterior_arms() {
    Walls walls{
        {"horizontal", make_wall("horizontal", {{0, 0}, {4, 0}, 0})},
        {"vertical", make_wall("vertical", {{2, -2}, {2, 2}, 0})}};
    auto plans = plans_for(walls);

    apply_wall_plan_junction_strokes(plans, walls, {{"vertical", "horizontal"}});

    const Boundary expected_horizontal{
        {{0, .1}, {1.9, .1}, 0}, {{2.1, .1}, {4, .1}, 0},
        {{4, .1}, {4, -.1}, 0}, {{4, -.1}, {2.1, -.1}, 0},
        {{1.9, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0}};
    require_boundary(plans.at("horizontal").strokes, expected_horizontal,
                     "both horizontal faces are hidden through the crossing");
    require(plans.at("vertical").strokes.size() == 6,
            "the vertical crossing retains its two side faces and two end caps");
    require(contains(plans.at("vertical").strokes, {{1.9, -2}, {1.9, -.1}, 0}) &&
                contains(plans.at("vertical").strokes, {{1.9, .1}, {1.9, 2}, 0}) &&
                contains(plans.at("vertical").strokes, {{2.1, 2}, {2.1, .1}, 0}) &&
                contains(plans.at("vertical").strokes, {{2.1, -.1}, {2.1, -2}, 0}) &&
                contains(plans.at("vertical").strokes, {{1.9, 2}, {2.1, 2}, 0}) &&
                contains(plans.at("vertical").strokes, {{2.1, -2}, {1.9, -2}, 0}),
            "the vertical silhouette remains outside the horizontal wall material");
}

void test_three_endpoint_t_removes_the_internal_host_cap_and_branch_cap() {
    Walls walls{
        {"host-left", make_wall("host-left", {{0, 0}, {2, 0}, 0})},
        {"host-right", make_wall("host-right", {{2, 0}, {4, 0}, 0})},
        {"branch", make_wall("branch", {{2, 0}, {2, 3}, 0})}};
    auto plans = plans_for(walls);
    const std::vector<Contact> all_endpoint_pairs{
        {"host-left", "host-right"}, {"host-left", "branch"},
        {"host-right", "branch"}};

    apply_wall_plan_junction_strokes(plans, walls, all_endpoint_pairs);

    require(plans.at("host-left").strokes.size() == 3 &&
                contains(plans.at("host-left").strokes, {{0, .1}, {1.9, .1}, 0}) &&
                contains(plans.at("host-left").strokes, {{2, -.1}, {0, -.1}, 0}) &&
                contains(plans.at("host-left").strokes, {{0, -.1}, {0, .1}, 0}),
            "the left host keeps exterior faces after its shared end cap disappears");
    require(plans.at("host-right").strokes.size() == 3 &&
                contains(plans.at("host-right").strokes, {{2.1, .1}, {4, .1}, 0}) &&
                contains(plans.at("host-right").strokes, {{4, -.1}, {2, -.1}, 0}) &&
                contains(plans.at("host-right").strokes, {{4, .1}, {4, -.1}, 0}),
            "the right host keeps exterior faces after its shared start cap disappears");
    require(!contains(plans.at("host-left").strokes, {{2, .1}, {2, -.1}, 0}) &&
                !contains(plans.at("host-right").strokes, {{2, -.1}, {2, .1}, 0}),
            "opposite shared caps are both removed when material meets on each side");
    require(plans.at("branch").strokes.size() == 3 &&
                !contains(plans.at("branch").strokes, {{2.1, 0}, {1.9, 0}, 0}),
            "the branch cap inside the two host pieces is hidden");
}

void test_unequal_thickness_reversed_and_rotated_translated_junctions() {
    Walls reversed{
        {"host", make_wall("host", {{4, 0}, {0, 0}, 0}, .2)},
        {"branch", make_wall("branch", {{2, 0}, {2, 3}, 0}, .4)}};
    auto reversed_plans = plans_for(reversed);
    apply_wall_plan_junction_strokes(reversed_plans, reversed, {{"host", "branch"}});
    require(contains(reversed_plans.at("host").strokes, {{0, .1}, {1.8, .1}, 0}) &&
                contains(reversed_plans.at("host").strokes, {{2.2, .1}, {4, .1}, 0}) &&
                contains(reversed_plans.at("host").strokes, {{4, -.1}, {0, -.1}, 0}),
            "reversed host direction clips the correct unequal-thickness interval");
    require(contains(reversed_plans.at("branch").strokes, {{1.8, .1}, {1.8, 3}, 0}) &&
                contains(reversed_plans.at("branch").strokes, {{2.2, 3}, {2.2, .1}, 0}) &&
                !contains(reversed_plans.at("branch").strokes, {{2.2, 0}, {1.8, 0}, 0}),
            "the wide reversed-junction branch has exposed sides and no buried cap");

    constexpr double angle = .63;
    const double cosine = std::cos(angle), sine = std::sin(angle);
    const Vec2 offset{1'000'000.0, -2'000'000.0};
    const auto transform = [&](Segment line) {
        return Segment{rotate_translate(line.start, cosine, sine, offset),
                       rotate_translate(line.end, cosine, sine, offset), 0};
    };
    Walls transformed{
        {"host", make_wall("host", transform({{0, 0}, {4, 0}, 0}))},
        {"branch", make_wall("branch", transform({{2, 0}, {2, 3}, 0}))}};
    auto transformed_plans = plans_for(transformed);
    apply_wall_plan_junction_strokes(transformed_plans, transformed,
                                     {{"host", "branch"}});
    const auto expected_start = rotate_translate({0, .1}, cosine, sine, offset);
    const auto expected_split_left = rotate_translate({1.9, .1}, cosine, sine, offset);
    const auto expected_split_right = rotate_translate({2.1, .1}, cosine, sine, offset);
    const auto expected_end = rotate_translate({4, .1}, cosine, sine, offset);
    require(contains(transformed_plans.at("host").strokes,
                     {expected_start, expected_split_left, 0}, 1e-9) &&
                contains(transformed_plans.at("host").strokes,
                         {expected_split_right, expected_end, 0}, 1e-9),
            "rotated junction cuts retain exact local stations after large translation");
}

void test_opening_polygons_are_clipped_independently() {
    Walls walls{
        {"host", make_wall("host", {{0, 0}, {4, 0}, 0}, .2,
                           {{"opening", 1, .5, 0, 2.1}})},
        {"branch", make_wall("branch", {{2, 0}, {2, 3}, 0})}};
    auto plans = plans_for(walls);

    apply_wall_plan_junction_strokes(plans, walls, {{"host", "branch"}});

    require(contains(plans.at("host").strokes, {{0, .1}, {1, .1}, 0}) &&
                contains(plans.at("host").strokes, {{1.5, .1}, {1.9, .1}, 0}) &&
                contains(plans.at("host").strokes, {{2.1, .1}, {4, .1}, 0}) &&
                contains(plans.at("host").strokes, {{1, .1}, {1, -.1}, 0}) &&
                contains(plans.at("host").strokes, {{1.5, -.1}, {1.5, .1}, 0}),
            "the wall opening stays empty while the branch clips adjacent wall material");
    require(!contains(plans.at("branch").strokes, {{2.1, 0}, {1.9, 0}, 0}) &&
                contains(plans.at("branch").strokes, {{1.9, .1}, {1.9, 3}, 0}),
            "opening-separated footprint polygons still hide the buried branch cap");
}

void test_only_explicit_contact_pairs_are_processed() {
    Walls walls{
        {"host", make_wall("host", {{0, 0}, {4, 0}, 0})},
        {"branch", make_wall("branch", {{2, 0}, {2, 3}, 0})},
        {"unrelated", make_wall("unrelated", {{3, -2}, {3, 2}, 0})}};
    auto plans = plans_for(walls);
    const auto initial = plans;

    apply_wall_plan_junction_strokes(plans, walls, {{"branch", "host"}});

    require(plans.at("unrelated").strokes.size() == initial.at("unrelated").strokes.size() &&
                near(plans.at("unrelated").strokes[3], initial.at("unrelated").strokes[3]),
            "geometric overlap without an explicit eligible contact does not change strokes");
    require(plans.at("host").strokes.size() == 5 && plans.at("branch").strokes.size() == 3,
            "the explicit contact pair is still clipped");

    auto untouched = initial;
    apply_wall_plan_junction_strokes(untouched, walls, {});
    require(same_plans(untouched, initial),
            "an empty contact graph leaves all strokes byte-for-byte unchanged");
}

void test_unsupported_curved_and_malformed_inputs_fall_back_unchanged() {
    Walls curved{
        {"arc", make_wall("arc", {{0, 0}, {1, 1}, 1.5707963267948966})},
        {"line", make_wall("line", {{1, 1}, {1, 4}, 0})}};
    auto curved_plans = plans_for(curved);
    const auto curved_before = curved_plans;
    apply_wall_plan_junction_strokes(curved_plans, curved, {{"arc", "line"}});
    require(same_plans(curved_plans, curved_before),
            "curved baseline contacts retain their established plan strokes");

    Walls straight{
        {"host", make_wall("host", {{0, 0}, {4, 0}, 0})},
        {"branch", make_wall("branch", {{2, 0}, {2, 3}, 0})}};
    auto malformed_plans = plans_for(straight);
    auto& bad = malformed_plans.at("host");
    bad.footprint = {{{0, 0}, {4, 0}, 0}, {{4, 0}, {1, 1}, 0},
                     {{1, 1}, {0, 2}, 0}, {{0, 2}, {0, 0}, 0}};
    bad.strokes = bad.footprint;
    const auto malformed_before = malformed_plans;
    apply_wall_plan_junction_strokes(malformed_plans, straight, {{"host", "branch"}});
    require(same_plans(malformed_plans, malformed_before),
            "a nonconvex material polygon makes the pair fall back atomically");

    auto numeric_plans = plans_for(straight);
    numeric_plans.at("host").strokes[0].start.x = std::numeric_limits<double>::infinity();
    const auto original_bad_x = numeric_plans.at("host").strokes[0].start.x;
    const auto branch_before = numeric_plans.at("branch").strokes;
    apply_wall_plan_junction_strokes(numeric_plans, straight, {{"host", "branch"}});
    require(std::isinf(numeric_plans.at("host").strokes[0].start.x) &&
                numeric_plans.at("host").strokes[0].start.x == original_bad_x &&
                same_boundary(numeric_plans.at("branch").strokes, branch_before),
            "nonfinite source coordinates are retained without partially clipping a neighbor");
}

void test_interior_contact_classifier_excludes_endpoint_only_and_collinear_cases() {
    require(wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, 0}, {{2, 0}, {2, 3}, 0}),
            "a branch endpoint on a host interior is an interior contact");
    require(wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, 0}, {{2, -2}, {2, 2}, 0}),
            "a proper X crossing is an interior contact");
    require(!wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, 0}, {{4, 0}, {4, 3}, 0}),
            "a pure endpoint-to-endpoint corner is excluded for miter fallback");
    require(!wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, 0}, {{2, 0}, {6, 0}, 0}),
            "collinear overlap remains unsupported");
    require(!wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, 0}, {{5, -2}, {5, 2}, 0}),
            "disjoint finite segments are excluded");
    require(!wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, .2}, {{2, -2}, {2, 2}, 0}),
            "curved baselines are excluded");
    require(!wall_baselines_have_interior_contact(
                {{0, 0}, {4, 0}, 0},
                {{2, -2}, {std::numeric_limits<double>::infinity(), 2}, 0}),
            "nonfinite baselines are excluded");
}

void test_unrepresentable_station_restores_every_affected_stroke() {
    const Vec2 offset{1e12, -1e12};
    const auto transform = [&](Vec2 point) {
        return rotate_translate(point, std::cos(.63), std::sin(.63), offset);
    };
    Walls walls{
        {"host", make_wall("host", {transform({0,0}), transform({4,0}), 0})},
        {"branch", make_wall("branch", {transform({2,0}), transform({2,3}), 0})}};
    auto plans = plans_for(walls);
    const auto original = plans;
    apply_wall_plan_junction_strokes(plans, walls, {{"host", "branch"}});
    require(same_plans(plans, original),
        "clipping stations that cannot preserve the model tolerance leave all source strokes intact");
    walls.emplace("normal-host", make_wall("normal-host", {{0,0},{4,0},0}));
    walls.emplace("normal-branch", make_wall("normal-branch", {{2,0},{2,3},0}));
    plans = plans_for(walls);
    const auto bad_host = plans.at("host");
    apply_wall_plan_junction_strokes(plans, walls,
        {{"host","branch"},{"normal-host","normal-branch"}});
    require(plans.at("normal-host").strokes.size() == 5 &&
            plans.at("normal-branch").strokes.size() == 3 &&
            same_boundary(plans.at("host").strokes, bad_host.strokes),
        "an unrepresentable disconnected component cannot cancel an ordinary T junction");
}

} // namespace

int main() {
    try {
        test_t_junction_subtracts_only_interior_stroke_spans();
        test_x_junction_keeps_only_four_exterior_arms();
        test_three_endpoint_t_removes_the_internal_host_cap_and_branch_cap();
        test_unequal_thickness_reversed_and_rotated_translated_junctions();
        test_opening_polygons_are_clipped_independently();
        test_only_explicit_contact_pairs_are_processed();
        test_unsupported_curved_and_malformed_inputs_fall_back_unchanged();
        test_interior_contact_classifier_excludes_endpoint_only_and_collinear_cases();
        test_unrepresentable_station_restores_every_affected_stroke();
        std::cout << "Wall plan junction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
