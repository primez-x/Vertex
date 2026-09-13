#include "sketch/wall_semantics.hpp"

#include <cmath>
#include <algorithm>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using sketch::HostedOpening;
using sketch::Segment;
using sketch::Wall;
using sketch::WallLayer;
using sketch::WallLayerMaterial;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function> void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void require_same(const Wall& actual, const Wall& expected) {
    require(actual.id == expected.id && actual.baseline.start.x == expected.baseline.start.x &&
                actual.baseline.start.y == expected.baseline.start.y &&
                actual.baseline.end.x == expected.baseline.end.x &&
                actual.baseline.end.y == expected.baseline.end.y &&
                actual.baseline.sweep_radians == expected.baseline.sweep_radians &&
                actual.thickness == expected.thickness && actual.height == expected.height &&
                actual.elevation == expected.elevation && actual.openings.size() == expected.openings.size() &&
                actual.layers.size() == expected.layers.size(),
            "wall validation must not mutate scalar or baseline fields");
    for (std::size_t index = 0; index < actual.openings.size(); ++index) {
        const auto& left = actual.openings[index];
        const auto& right = expected.openings[index];
        require(left.id == right.id && left.offset == right.offset && left.width == right.width &&
                    left.sill == right.sill && left.height == right.height,
                "wall validation must not mutate hosted openings");
    }
    for (std::size_t index = 0; index < actual.layers.size(); ++index) {
        require(actual.layers[index] == expected.layers[index],
                "wall validation must not mutate composite layers");
    }
}

Wall straight_wall() {
    return {"straight", {{0.0, 0.0}, {4.0, 0.0}, 0.0}, 0.2, 3.0, 1.5, {}};
}

void test_valid_straight_and_curved_walls() {
    auto straight = straight_wall();
    straight.openings = {
        {"door", 0.5, 1.0, 0.0, 2.1},
        {"window", 2.5, 1.0, 1.0, 1.0},
    };
    const auto straight_before = straight;
    sketch::validate_wall_semantics(straight);
    require_same(straight, straight_before);

    Wall curved{"curved", {{2.0, 0.0}, {0.0, 2.0}, std::numbers::pi / 2.0}, 0.2, 3.0, -2.0,
                {{"window", 0.3, 1.0, 0.8, 1.2}}};
    const auto curved_before = curved;
    sketch::validate_wall_semantics(curved);
    require_same(curved, curved_before);
}

void test_openings_follow_host_length_and_tolerance() {
    auto shortened = straight_wall();
    shortened.openings.push_back({"door", 0.5, 1.0, 0.0, 2.1});
    shortened.baseline.end = {1.0, 0.0};
    rejected([&] { sketch::validate_wall_semantics(shortened); },
             "an opening from the old host must be rejected after shortening the host");

    auto edge_touch = straight_wall();
    edge_touch.openings = {
        {"left", 0.0, 1.0, 0.0, 1.0},
        {"right", 1.0, 1.0, 0.0, 1.0},
    };
    sketch::validate_wall_semantics(edge_touch);

    auto overlap = edge_touch;
    overlap.openings[1].offset = 1.0 - 2.0 * sketch::default_geometry_tolerance_metres;
    rejected([&] { sketch::validate_wall_semantics(overlap); },
             "openings with overlap beyond tolerance must be rejected");
}

void test_invalid_and_nonrepresentable_wall_values() {
    auto degenerate = straight_wall();
    degenerate.baseline.end = degenerate.baseline.start;
    rejected([&] { sketch::validate_wall_semantics(degenerate); },
             "coincident wall baseline endpoints must be rejected");

    auto nonfinite = straight_wall();
    nonfinite.baseline.start.x = std::numeric_limits<double>::quiet_NaN();
    rejected([&] { sketch::validate_wall_semantics(nonfinite); },
             "nonfinite baseline coordinates must be rejected");

    auto dimensions = straight_wall();
    dimensions.thickness = sketch::default_geometry_tolerance_metres;
    rejected([&] { sketch::validate_wall_semantics(dimensions); },
             "wall thickness at tolerance must be rejected");
    dimensions = straight_wall();
    dimensions.openings.push_back({"bad", 0.0, 0.0, 0.0, 1.0});
    rejected([&] { sketch::validate_wall_semantics(dimensions); },
             "zero-width openings must be rejected");

    auto overflow = straight_wall();
    overflow.elevation = std::numeric_limits<double>::max();
    overflow.height = std::numeric_limits<double>::max();
    rejected([&] { sketch::validate_wall_semantics(overflow); },
             "unrepresentable wall elevation plus height must be rejected");

    overflow = straight_wall();
    overflow.baseline.end = {std::numeric_limits<double>::max(),
                             std::numeric_limits<double>::max()};
    rejected([&] { sketch::validate_wall_semantics(overflow); },
             "unrepresentable baseline length must be rejected");

    overflow = straight_wall();
    overflow.openings.push_back({"huge", std::numeric_limits<double>::max(), 1.0, 0.0, 1.0});
    rejected([&] { sketch::validate_wall_semantics(overflow); },
             "unrepresentable opening extent must be rejected");

    auto invalid_arc = straight_wall();
    invalid_arc.baseline.sweep_radians = 2.0 * std::numbers::pi;
    rejected([&] { sketch::validate_wall_semantics(invalid_arc); },
             "full-turn arc baselines must be rejected");

    Wall crossing{"crossing", {{2.0, 0.0}, {0.0, 2.0}, std::numbers::pi / 2.0}, 4.0, 3.0, 0.0,
                   {}};
    rejected([&] { sketch::validate_wall_semantics(crossing); },
             "arc wall thickness crossing its centre must be rejected");
}

void test_duplicate_and_out_of_bounds_openings() {
    auto duplicate = straight_wall();
    duplicate.openings = {
        {"same", 0.0, 0.5, 0.0, 1.0},
        {"same", 2.0, 0.5, 0.0, 1.0},
    };
    rejected([&] { sketch::validate_wall_semantics(duplicate); },
             "opening IDs must be unique");

    auto out_of_bounds = straight_wall();
    out_of_bounds.openings.push_back({"outside", -0.1, 0.5, 0.0, 1.0});
    rejected([&] { sketch::validate_wall_semantics(out_of_bounds); },
             "negative opening offsets must be rejected");
    out_of_bounds.openings.front() = {"outside", 3.8, 0.5, 0.0, 1.0};
    rejected([&] { sketch::validate_wall_semantics(out_of_bounds); },
             "opening horizontal bounds must be enforced");
    out_of_bounds.openings.front() = {"outside", 0.0, 0.5, 2.5, 1.0};
    rejected([&] { sketch::validate_wall_semantics(out_of_bounds); },
             "opening vertical bounds must be enforced");

    auto vertical_overlap = straight_wall();
    vertical_overlap.openings = {
        {"lower", 0.0, 0.5, 0.0, 1.0},
        {"upper", 0.0, 0.5, 1.0 - 2.0 * sketch::default_geometry_tolerance_metres, 1.0},
    };
    rejected([&] { sketch::validate_wall_semantics(vertical_overlap); },
             "openings overlapping vertically must be rejected");
}

void test_openings_cannot_remove_entire_wall() {
    auto partial = straight_wall();
    partial.openings = {
        {"lower-left", 0.0, 2.0, 0.0, 1.5},
        {"upper-left", 0.0, 2.0, 1.5, 1.5},
        {"lower-right", 2.0, 2.0, 0.0, 1.5},
    };
    sketch::validate_wall_semantics(partial);

    auto full = partial;
    full.openings.push_back({"upper-right", 2.0, 2.0, 1.5, 1.5});
    rejected([&] { sketch::validate_wall_semantics(full); },
             "adjacent openings tiling the wall length and height must be rejected");
}

void test_sweep_exact_coverage_and_event_order() {
    auto tiled = straight_wall();
    tiled.openings = {{"a", 0, 2, 0, 1.5}, {"b", 0, 2, 1.5, 1.5},
                      {"c", 2, 2, 0, 1.5}, {"d", 2, 2, 1.5, 1.5}};
    // All event-order permutations must report the same exact complete union.
    do {
        rejected([&] { sketch::validate_wall_semantics(tiled); }, "coverage depended on opening order");
    } while (std::next_permutation(tiled.openings.begin(), tiled.openings.end(),
                                  [](const auto& left, const auto& right) { return left.id < right.id; }));

    const auto next_x = std::nextafter(2.0, 4.0);
    auto tiny_gap = straight_wall();
    tiny_gap.openings = {{"left", 0, 2, 0, 3}, {"right", next_x, 4.0 - next_x, 0, 3}};
    sketch::validate_wall_semantics(tiny_gap);
    const auto next_y = std::nextafter(1.5, 3.0);
    tiny_gap.openings = {{"lower", 0, 4, 0, 1.5}, {"upper", 0, 4, next_y, 3.0 - next_y}};
    sketch::validate_wall_semantics(tiny_gap);

    auto tolerated_overlap = straight_wall();
    tolerated_overlap.openings = {{"left", 0, 2.0 + sketch::default_geometry_tolerance_metres * 0.5, 0, 3},
                                   {"right", 2.0, 2.0, 0, 3}};
    rejected([&] { sketch::validate_wall_semantics(tolerated_overlap); },
             "tolerance overlap must not hide an exact complete union");
    for (auto& opening : tolerated_overlap.openings) opening.height = 2.5;
    sketch::validate_wall_semantics(tolerated_overlap);
}

void test_composite_wall_layers_are_typed_and_lossless() {
    auto wall = straight_wall();
    wall.layers = {
        WallLayer{"sheathing", 0.02, std::nullopt},
        WallLayer{"core", 0.16, WallLayerMaterial{"catalog", "brick"}},
        WallLayer{"finish", 0.02, WallLayerMaterial{"catalog", "paint"}},
    };
    const auto before = wall;
    sketch::validate_wall_semantics(wall);
    require_same(wall, before);

    const auto encoded = sketch::wall_layers_json(wall.layers);
    const auto decoded = sketch::parse_wall_layers(encoded, wall.thickness);
    require(decoded == wall.layers, "composite wall layers must round-trip deterministically");
    require(encoded.size() == 3 && encoded[1].at("material_assignment").at("material_id") == "brick",
            "layer material references must remain explicit in project JSON");

    auto invalid = wall;
    invalid.layers[1].thickness += 0.01;
    rejected([&] { sketch::validate_wall_semantics(invalid); },
             "layer thicknesses that do not sum to the wall must be rejected");
    invalid = wall;
    invalid.layers[2].id = invalid.layers[0].id;
    rejected([&] { sketch::validate_wall_semantics(invalid); },
             "duplicate wall layer IDs must be rejected");

    auto malformed = encoded;
    malformed[0]["material_assignment"] = {{"version", 1}, {"catalog_id", "catalog"}};
    rejected([&] { (void)sketch::parse_wall_layers(malformed, wall.thickness); },
             "incomplete layer material assignments must be rejected");
}

}  // namespace

int main() {
    try {
        test_valid_straight_and_curved_walls();
        test_openings_follow_host_length_and_tolerance();
        test_invalid_and_nonrepresentable_wall_values();
        test_duplicate_and_out_of_bounds_openings();
        test_openings_cannot_remove_entire_wall();
        test_sweep_exact_coverage_and_event_order();
        test_composite_wall_layers_are_typed_and_lossless();
        std::cout << "Wall semantic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
