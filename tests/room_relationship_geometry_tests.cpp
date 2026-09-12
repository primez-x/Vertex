#include "sketch/room_relationship_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
using K = RoomReferenceKind;
using R = RoomRelationKind;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool approx(double left, double right, double tolerance = 1e-8) {
    return std::abs(left - right) <= tolerance;
}

bool approx(Vec2 left, Vec2 right, double tolerance = 1e-8) {
    return approx(left.x, right.x, tolerance) && approx(left.y, right.y, tolerance);
}

Segment line(Vec2 start, Vec2 end) { return {start, end, 0.0}; }

Boundary rectangle(double left, double bottom, double right, double top) {
    return {line({left, bottom}, {right, bottom}), line({right, bottom}, {right, top}),
            line({right, top}, {left, top}), line({left, top}, {left, bottom})};
}

std::vector<RelationshipGeometry> records(std::initializer_list<RelationshipGeometry> values) {
    return {values};
}

void require_contains(const std::vector<std::string>& diagnostics, const char* needle) {
    require(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& value) {
                return value.find(needle) != std::string::npos;
            }),
            needle);
}

void test_translation_and_chain() {
    const auto model = RoomRelationshipSnapshot::create(
        { {"measure", K::appraisal_measurement_boundary}, {"room", K::room_boundary},
          {"wall", K::architectural_wall} },
        { {"room", "wall", R::follows}, {"measure", "room", R::follows} });
    const auto room = rectangle(0.0, 0.0, 4.0, 3.0);
    const auto measure = rectangle(0.0, 0.0, 4.0, 3.0);
    const auto before = records({{"measure", K::appraisal_measurement_boundary, measure},
                                 {"room", K::room_boundary, room},
                                 {"wall", K::architectural_wall, {line({0.0, 0.0}, {4.0, 0.0})}}});
    const auto after = records({{"measure", K::appraisal_measurement_boundary, measure},
                                {"room", K::room_boundary, room},
                                {"wall", K::architectural_wall,
                                 {line({5.0, 2.0}, {9.0, 2.0})}}});
    const auto result = propose_room_relationship_geometry(model, before, after);
    require(!result.has_diagnostics(), "rigid translation should produce no diagnostics");
    require(result.changes.size() == 2, "dependency chain should propagate to both sources");
    require(result.changes[0].source_id == "measure" && result.changes[1].source_id == "room",
            "changes should be deterministic by source identity");
    require(approx(result.changes[1].geometry.front().start, {5.0, 2.0}),
            "room boundary did not follow translated wall");
    require(approx(result.changes[0].geometry.front().start, {5.0, 2.0}),
            "measurement boundary did not follow propagated room");
    require(result.changes[1].driver_ids == std::vector<std::string>{"wall"},
            "room driver identity was not retained");
    require(result.changes[0].driver_ids == std::vector<std::string>{"room"},
            "measurement driver identity was not retained");
    require(approx(before[1].geometry.front().start, {0.0, 0.0}),
            "proposal mutated the before snapshot");
}

void test_rotation() {
    const auto model = RoomRelationshipSnapshot::create(
        {{"room", K::room_boundary}, {"measure", K::appraisal_measurement_boundary}},
        {{"measure", "room", R::follows}});
    const auto room = rectangle(1.0, 0.0, 5.0, 2.0);
    const PlanarTransform rotate{{}, std::numbers::pi / 2.0, false, false, {10.0, -4.0}};
    Boundary moved;
    for (const auto& segment : room) moved.push_back(transform_segment(segment, rotate));
    const auto measure = rectangle(1.0, 0.0, 5.0, 2.0);
    const auto result = propose_room_relationship_geometry(
        model,
        records({{"measure", K::appraisal_measurement_boundary, measure}, {"room", K::room_boundary, room}}),
        records({{"measure", K::appraisal_measurement_boundary, measure}, {"room", K::room_boundary, moved}}));
    require(!result.has_diagnostics(), "rigid rotation should produce no diagnostics");
    require(result.changes.size() == 1, "rotation should produce one change");
    require(approx(result.changes.front().geometry.front().start, moved.front().start),
            "rotation proposal did not map the source start");
    require(approx(result.changes.front().geometry[2].end, moved[2].end),
            "rotation proposal did not map the source topology");
}

void test_derived_drivers_and_conflict() {
    const auto model = RoomRelationshipSnapshot::create(
        {{"room", K::room_boundary}, {"wall-a", K::architectural_wall}, {"wall-b", K::architectural_wall}},
        {{"room", "wall-a", R::derived_from}, {"room", "wall-b", R::derived_from}});
    const auto room = rectangle(0.0, 0.0, 4.0, 3.0);
    const auto before = records({{"room", K::room_boundary, room},
                                 {"wall-a", K::architectural_wall, {line({0.0, 0.0}, {4.0, 0.0})}},
                                 {"wall-b", K::architectural_wall, {line({4.0, 0.0}, {4.0, 3.0})}}});
    const auto after = records({{"room", K::room_boundary, room},
                                {"wall-a", K::architectural_wall, {line({2.0, 1.0}, {6.0, 1.0})}},
                                {"wall-b", K::architectural_wall, {line({6.0, 1.0}, {6.0, 4.0})}}});
    auto result = propose_room_relationship_geometry(model, before, after);
    require(!result.has_diagnostics() && result.changes.size() == 1,
            "matching derived drivers should produce one change");
    require(approx(result.changes.front().geometry.front().start, {2.0, 1.0}),
            "derived room did not follow matching wall transforms");
    require(result.changes.front().driver_ids == std::vector<std::string>{"wall-a", "wall-b"},
            "derived driver list was not deterministic");

    auto conflicting = after;
    conflicting[2].geometry = {line({7.0, 1.0}, {7.0, 4.0})};
    result = propose_room_relationship_geometry(model, before, conflicting);
    require(result.changes.empty(), "conflicting drivers must not produce a change");
    require_contains(result.diagnostics, "conflicting driver transforms");
}

void test_arc_and_reflection_guard() {
    const auto model = RoomRelationshipSnapshot::create(
        {{"room", K::room_boundary}, {"wall", K::architectural_wall}},
        {{"room", "wall", R::follows}});
    const auto arc = arc_from_chord_angle({0.0, 0.0}, {4.0, 0.0}, std::numbers::pi / 2.0);
    const auto before = records({{"room", K::room_boundary, rectangle(0.0, 0.0, 4.0, 3.0)},
                                 {"wall", K::architectural_wall, {arc}}});
    const PlanarTransform move{{}, std::numbers::pi / 3.0, false, false, {2.0, -1.0}};
    Boundary moved;
    moved.push_back(transform_segment(arc, move));
    auto after = records({{"room", K::room_boundary, rectangle(0.0, 0.0, 4.0, 3.0)},
                          {"wall", K::architectural_wall, moved}});
    auto result = propose_room_relationship_geometry(model, before, after);
    require(result.diagnostics.empty() && result.changes.size() == 1,
            "curved rigid targets should propagate");
    const auto expected_room_start = transform_segment(before.front().geometry.front(), move).start;
    require(approx(result.changes.front().geometry.front().start, expected_room_start),
            "curved target transform did not propagate to the source");

    const auto room = rectangle(0.0, 0.0, 4.0, 3.0);
    const PlanarTransform reflect{{}, 0.0, true, false, {0.0, 0.0}};
    Boundary reflected;
    for (const auto& segment : room) reflected.push_back(transform_segment(segment, reflect));
    const auto boundary_model = RoomRelationshipSnapshot::create(
        {{"measure", K::appraisal_measurement_boundary}, {"room", K::room_boundary}},
        {{"measure", "room", R::follows}});
    const auto boundary_before = records({{"measure", K::appraisal_measurement_boundary, room},
                                          {"room", K::room_boundary, room}});
    const auto boundary_after = records({{"measure", K::appraisal_measurement_boundary, room},
                                         {"room", K::room_boundary, reflected}});
    result = propose_room_relationship_geometry(boundary_model, boundary_before, boundary_after);
    require(result.changes.empty(), "reflection must not be inferred as rigid propagation");
    require_contains(result.diagnostics, "non-rigid");
}

void test_rejects_unsafe_changes() {
    const auto model = RoomRelationshipSnapshot::create(
        {{"room", K::room_boundary}, {"wall", K::architectural_wall}},
        {{"room", "wall", R::follows}});
    const auto before = records({{"room", K::room_boundary, rectangle(0.0, 0.0, 4.0, 3.0)},
                                 {"wall", K::architectural_wall, {line({0.0, 0.0}, {4.0, 0.0})}}});
    auto scaled = before;
    scaled[1].geometry = {line({0.0, 0.0}, {8.0, 0.0})};
    auto result = propose_room_relationship_geometry(model, before, scaled);
    require(result.changes.empty(), "scaled targets must not be inferred as rigid motion");
    require_contains(result.diagnostics, "non-rigid");

    auto moved = before;
    moved[1].geometry = {line({5.0, 0.0}, {9.0, 0.0})};
    moved.erase(moved.begin());
    result = propose_room_relationship_geometry(model, before, moved);
    require(result.changes.empty(), "missing source geometry must not be committed");
    require_contains(result.diagnostics, "missing after geometry for source room");

    const auto independent = RoomRelationshipSnapshot::create(
        {{"room", K::room_boundary}, {"wall", K::architectural_wall}},
        {{"room", "wall", R::independent}});
    result = propose_room_relationship_geometry(independent, before, scaled);
    require(result.changes.empty() && result.diagnostics.empty(),
            "independent references must never propagate geometry");
}

void run() {
    test_translation_and_chain();
    test_rotation();
    test_derived_drivers_and_conflict();
    test_arc_and_reflection_guard();
    test_rejects_unsafe_changes();
}
} // namespace

int main() {
    try {
        run();
        std::cout << "Room relationship geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
