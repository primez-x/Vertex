#include "sketch/reference_grid.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function>
void rejects(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid reference grid accepted");
}

void round_trip_and_lines() {
    ReferenceGridModel model;
    model.origin_m = {10.0, -4.0};
    model.rotation_radians = 0.25;
    model.spacing_x_m = 2.0;
    model.spacing_y_m = 3.0;
    model.count_x = 2;
    model.count_y = 1;
    model.major_every = 2;
    model.x_label = "Grid X";
    model.y_label = "Grid Y";
    model.visible = false;
    const auto encoded = model.to_json();
    require(ReferenceGridModel::from_json(encoded) == model,
            "Reference grid did not round-trip through versioned JSON");
    require(encoded == nlohmann::json{
                                {"version", 1},
                                {"origin_m", {10.0, -4.0}},
                                {"rotation_radians", 0.25},
                                {"spacing_x_m", 2.0},
                                {"spacing_y_m", 3.0},
                                {"count_x", 2},
                                {"count_y", 1},
                                {"major_every", 2},
                                {"x_label", "Grid X"},
                                {"y_label", "Grid Y"},
                                {"visible", false},
                            },
            "Reference grid JSON shape is not canonical");

    const auto lines = model.lines();
    require(lines.size() == (2 * model.count_x + 1) + (2 * model.count_y + 1),
            "Reference grid line count is not symmetric around its origin");
    require(lines.front().axis == ReferenceGridAxis::x && lines.front().index == -model.count_x &&
                lines.front().major,
            "Reference grid X lines are not ordered or classified deterministically");
    require(lines.back().axis == ReferenceGridAxis::y && lines.back().index == model.count_y,
            "Reference grid Y lines are not ordered deterministically");
    require(std::hypot(lines.front().start.x - lines.front().end.x,
                       lines.front().start.y - lines.front().end.y) > 0.0,
            "Reference grid line collapsed to a point");
}

void validation() {
    ReferenceGridModel model;
    for (auto invalid : {
             ReferenceGridModel{.origin_m = {0.0, 0.0}, .rotation_radians = 0.0,
                                .spacing_x_m = 0.0, .spacing_y_m = 1.0, .count_x = 1,
                                .count_y = 1, .major_every = 1, .x_label = "A", .y_label = "1",
                                .visible = true},
             ReferenceGridModel{.origin_m = {0.0, 0.0}, .rotation_radians = 0.0,
                                .spacing_x_m = 1.0, .spacing_y_m = 1.0, .count_x = 0,
                                .count_y = 1, .major_every = 1, .x_label = "A", .y_label = "1",
                                .visible = true},
             ReferenceGridModel{.origin_m = {0.0, 0.0}, .rotation_radians = 0.0,
                                .spacing_x_m = 1.0, .spacing_y_m = 1.0, .count_x = 1,
                                .count_y = 1, .major_every = 1, .x_label = "", .y_label = "1",
                                .visible = true},
         }) {
        rejects([&] { (void)invalid.to_json(); });
    }
    auto malformed = model.to_json();
    malformed["version"] = 2;
    rejects([&] { (void)ReferenceGridModel::from_json(malformed); });
    malformed = model.to_json();
    malformed["unexpected"] = true;
    rejects([&] { (void)ReferenceGridModel::from_json(malformed); });
    malformed = model.to_json();
    malformed["rotation_radians"] = std::numeric_limits<double>::infinity();
    rejects([&] { (void)ReferenceGridModel::from_json(malformed); });
    malformed = model.to_json();
    malformed["count_x"] = ReferenceGridModel::maximum_lines_from_origin + 1;
    rejects([&] { (void)ReferenceGridModel::from_json(malformed); });
}
}  // namespace

int main() {
    try {
        round_trip_and_lines();
        validation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "reference grid tests passed\n";
}
