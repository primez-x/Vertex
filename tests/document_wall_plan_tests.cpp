#include "sketch/document_wall_plan.hpp"

#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/wall_semantics.hpp"

#include <array>
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
using Entities = std::map<std::string, Entity, std::less<>>;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool near(double first, double second) {
    return std::abs(first - second) < 1e-9;
}

bool near(Vec2 first, Vec2 second) {
    return near(first.x, second.x) && near(first.y, second.y);
}

bool near(const Segment& first, const Segment& second) {
    return near(first.start, second.start) && near(first.end, second.end) &&
           near(first.sweep_radians, second.sweep_radians);
}

void require_boundary(const Boundary& actual, const Boundary& expected,
                      std::string_view message) {
    require(actual.size() == expected.size(), message);
    for (std::size_t index = 0; index < expected.size(); ++index)
        require(near(actual[index], expected[index]), message);
}

bool closed(const Boundary& boundary) {
    if (boundary.empty()) return false;
    for (std::size_t index = 0; index < boundary.size(); ++index)
        if (!near(boundary[index].end, boundary[(index + 1) % boundary.size()].start))
            return false;
    return true;
}

bool contains_segment(const Boundary& boundary, Segment expected) {
    for (const auto& segment : boundary)
        if (near(segment, expected)) return true;
    return false;
}

Entity wall_entity(std::string id, Segment baseline, double thickness = .2,
                   double elevation = 0) {
    Json properties{
        {"baseline", {{"start", {baseline.start.x, baseline.start.y}},
                      {"end", {baseline.end.x, baseline.end.y}},
                      {"sweep_radians", baseline.sweep_radians}}},
        {"thickness_m", thickness}, {"height_m", 3.0}, {"elevation_m", elevation},
        {"property_id", "property-a"}, {"building_id", "building-a"},
        {"floor_id", "floor-a"}, {"layer_id", "layer-a"}, {"phase_id", "phase-a"}};
    return {std::move(id), "wall", std::move(properties), false, Json::object()};
}

Entity opening_entity(std::string id, std::string wall_id, double offset, double width) {
    return {std::move(id), "opening",
            {{"wall_id", std::move(wall_id)}, {"offset_m", offset}, {"width_m", width},
             {"height_m", 2.1}, {"sill_m", 0.0}, {"visible", false}},
            false, Json::object()};
}

Entities corner_entities(Segment first, double first_thickness,
                         Segment second, double second_thickness) {
    Entities entities;
    entities.emplace("wall-a", wall_entity("wall-a", first, first_thickness));
    entities.emplace("wall-b", wall_entity("wall-b", second, second_thickness));
    return entities;
}

void require_capped_source(const Entities& entities, std::string_view first_id,
                           std::string_view second_id) {
    const auto result = document_wall_plan_geometry(entities);
    for (const auto id : {first_id, second_id}) {
        const auto& entity = entities.at(std::string(id));
        const auto& baseline = entity.properties.at("baseline");
        const Segment line{{baseline.at("start").at(0).get<double>(),
                             baseline.at("start").at(1).get<double>()},
                            {baseline.at("end").at(0).get<double>(),
                             baseline.at("end").at(1).get<double>()},
                            baseline.at("sweep_radians").get<double>()};
        const auto footprint = wall_plan_footprint(line, {},
            entity.properties.at("thickness_m").get<double>());
        require_boundary(result.at(std::string(id)).footprint, footprint,
                         "an ineligible corner must retain the original capped footprint");
        require_boundary(result.at(std::string(id)).strokes, footprint,
                         "an ineligible corner must retain its endpoint cap strokes");
    }
}

void test_unequal_thickness_corner_miters_both_endpoint_orientations() {
    const auto entities = corner_entities({{0, 0}, {4, 0}, 0}, .2,
                                          {{4, 0}, {4, 3}, 0}, .4);
    const auto result = document_wall_plan_geometry(entities);
    const Boundary horizontal_footprint{
        {{0, .1}, {3.8, .1}, 0}, {{3.8, .1}, {4.2, -.1}, 0},
        {{4.2, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0}};
    const Boundary vertical_footprint{
        {{3.8, .1}, {3.8, 3}, 0}, {{3.8, 3}, {4.2, 3}, 0},
        {{4.2, 3}, {4.2, -.1}, 0}, {{4.2, -.1}, {3.8, .1}, 0}};
    const Boundary horizontal_strokes{
        horizontal_footprint[0], horizontal_footprint[2], horizontal_footprint[3]};
    const Boundary vertical_strokes{
        vertical_footprint[0], vertical_footprint[1], vertical_footprint[2]};

    require_boundary(result.at("wall-a").footprint, horizontal_footprint,
                     "unequal wall thicknesses must meet at the exact shared miter");
    require_boundary(result.at("wall-b").footprint, vertical_footprint,
                     "the neighboring footprint must use the same exact miter");
    require_boundary(result.at("wall-a").strokes, horizontal_strokes,
                     "the joined endpoint cap must be omitted from horizontal strokes");
    require_boundary(result.at("wall-b").strokes, vertical_strokes,
                     "the joined endpoint cap must be omitted from vertical strokes");
    require(closed(result.at("wall-a").footprint) && closed(result.at("wall-b").footprint),
            "each joined wall keeps a closed footprint");
    const Segment shared_cap{{3.8, .1}, {4.2, -.1}, 0};
    require(!contains_segment(result.at("wall-a").strokes, shared_cap) &&
                !contains_segment(result.at("wall-b").strokes,
                                  {{4.2, -.1}, {3.8, .1}, 0}),
            "the shared miter cap is not drawn twice as a stroke");

    // Reversing the horizontal baseline moves the common vertex to its start.
    const auto reversed = corner_entities({{4, 0}, {0, 0}, 0}, .2,
                                          {{4, 0}, {4, 3}, 0}, .4);
    const auto reversed_result = document_wall_plan_geometry(reversed);
    const Boundary reversed_horizontal_footprint{
        {{4.2, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0},
        {{0, .1}, {3.8, .1}, 0}, {{3.8, .1}, {4.2, -.1}, 0}};
    const Boundary reversed_horizontal_strokes{
        reversed_horizontal_footprint[0], reversed_horizontal_footprint[1],
        reversed_horizontal_footprint[2]};
    require_boundary(reversed_result.at("wall-a").footprint, reversed_horizontal_footprint,
                     "reversed baselines must preserve the exact unequal-thickness miter");
    require_boundary(reversed_result.at("wall-a").strokes, reversed_horizontal_strokes,
                     "a reversed shared start cap must also be omitted from strokes");
    require_boundary(reversed_result.at("wall-b").footprint, vertical_footprint,
                     "reversing its neighbor must not move the shared corner");
}

void test_context_and_elevation_mismatches_keep_endpoint_caps() {
    const Segment first{{0, 0}, {4, 0}, 0};
    const Segment second{{4, 0}, {4, 3}, 0};
    constexpr std::array context_fields{
        "property_id", "building_id", "floor_id", "layer_id", "phase_id"};
    for (const auto* field : context_fields) {
        auto entities = corner_entities(first, .2, second, .4);
        entities.at("wall-b").properties[field] = std::string("different-") + field;
        require_capped_source(entities, "wall-a", "wall-b");
    }

    auto different_elevation = corner_entities(first, .2, second, .4);
    different_elevation.at("wall-b").properties["elevation_m"] = .25;
    require_capped_source(different_elevation, "wall-a", "wall-b");
}

void test_three_way_junction_retains_footprints_and_removes_internal_strokes() {
    auto entities = corner_entities({{0, 0}, {4, 0}, 0}, .2,
                                    {{4, 0}, {4, 3}, 0}, .4);
    entities.emplace("wall-c", wall_entity("wall-c", {{4, 0}, {5, 1}, 0}, .3));
    const auto result = document_wall_plan_geometry(entities);
    require(result.size() == 3, "all valid walls at a multi-way junction are projected");
    for (const auto id : {"wall-a", "wall-b", "wall-c"}) {
        const auto& properties = entities.at(id).properties;
        const auto& baseline = properties.at("baseline");
        const Segment host{{baseline.at("start").at(0).get<double>(),
                            baseline.at("start").at(1).get<double>()},
                           {baseline.at("end").at(0).get<double>(),
                            baseline.at("end").at(1).get<double>()},
                           baseline.at("sweep_radians").get<double>()};
        const auto original = wall_plan_footprint(
            host, {}, properties.at("thickness_m").get<double>());
        require_boundary(result.at(id).footprint, original,
                         "a three-way endpoint must retain the complete capped footprint");
        require(!closed(result.at(id).strokes),
                "multi-way junction outlines omit internal material boundaries");
    }
}

void test_interior_partition_joins_keep_openings_and_contexts() {
    Entities entities;
    entities.emplace("host", wall_entity("host", {{0, 0}, {4, 0}, 0}, .2));
    entities.emplace("branch", wall_entity("branch", {{2, 0}, {2, 3}, 0}, .4));
    const auto original = entities;
    const auto result = document_wall_plan_geometry(entities);
    require(contains_segment(result.at("host").strokes, {{0, .1}, {1.8, .1}, 0}) &&
            contains_segment(result.at("host").strokes, {{2.2, .1}, {4, .1}, 0}),
            "host inside face splits only across the real partition thickness");
    require(contains_segment(result.at("host").strokes, {{4, -.1}, {0, -.1}, 0}),
            "the exterior host face stays continuous");
    require(contains_segment(result.at("branch").strokes, {{1.8, .1}, {1.8, 3}, 0}) &&
            contains_segment(result.at("branch").strokes, {{2.2, 3}, {2.2, .1}, 0}),
            "partition faces begin at the host face without an internal cap");
    require_boundary(result.at("branch").footprint,
        wall_plan_footprint({{2,0},{2,3},0},{},.4), "closed partition picking geometry is preserved");
    require(entities == original, "derived partitions do not change baselines or appraisal source geometry");
    entities.at("branch").properties["floor_id"] = "floor-b";
    const auto different_floor = document_wall_plan_geometry(entities);
    require_boundary(different_floor.at("host").strokes, different_floor.at("host").footprint,
        "another floor cannot remove the host outline");
    entities = original;
    entities.emplace("host-opening", opening_entity("host-opening", "host", 1.5, 1.0));
    const auto void_result = document_wall_plan_geometry(entities);
    require_boundary(void_result.at("branch").strokes, void_result.at("branch").footprint,
        "a partition ending inside an opening remains visibly capped");
}

void test_partition_near_mitered_corner_uses_both_wall_materials() {
    auto entities = corner_entities({{0, 0}, {4, 0}, 0}, .2,
                                    {{4, 0}, {4, 3}, 0}, .2);
    entities.emplace("partition", wall_entity("partition", {{3.95, 0}, {3.95, -3}, 0}, .2));
    const auto result = document_wall_plan_geometry(entities);
    const auto& strokes = result.at("partition").strokes;
    require(!contains_segment(strokes, {{4.0,0},{4.05,0},0}) &&
            !contains_segment(strokes, {{4.05,0},{4.05,-.05},0}),
        "a partition near a mitered corner removes strokes buried inside either qualified corner partner");
    require(contains_segment(strokes, {{4.05,-.1},{4.05,-3},0}) &&
            contains_segment(strokes, {{3.85,-3},{3.85,-.1},0}),
        "near-corner partition retains its two real exterior faces");
    require_boundary(result.at("partition").footprint,
        wall_plan_footprint({{3.95,0},{3.95,-3},0},{},.2),
        "combined corner and T clipping preserves the closed partition polygon");
}

void test_curved_endpoint_neighbor_keeps_both_walls_capped() {
    const Segment arc{{0, 0}, {1, 1}, std::numbers::pi / 2};
    const Segment tangent_line{{1, 1}, {1, 4}, 0};
    Entities entities;
    entities.emplace("wall-arc", wall_entity("wall-arc", arc, .2));
    entities.emplace("wall-line", wall_entity("wall-line", tangent_line, .3));
    require_capped_source(entities, "wall-arc", "wall-line");
}

void test_neighbor_flush_opening_does_not_create_a_join() {
    const Segment first{{0, 0}, {4, 0}, 0};
    const Segment second{{4, 0}, {4, 3}, 0};
    auto entities = corner_entities(first, .2, second, .4);
    entities.emplace("opening-at-neighbor-start",
                     opening_entity("opening-at-neighbor-start", "wall-b", 0, .75));

    const auto result = document_wall_plan_geometry(entities);
    require(result.size() == 2, "both walls remain projected when a neighbor has an opening");
    const auto original_first = wall_plan_footprint(first, {}, .2);
    require_boundary(result.at("wall-a").footprint, original_first,
                     "the wall without material in its neighbor retains its capped footprint");
    require_boundary(result.at("wall-a").strokes, original_first,
                     "the wall without material in its neighbor retains the endpoint cap stroke");
    require(contains_segment(result.at("wall-a").strokes, {{4, .1}, {4, -.1}, 0}),
            "a flush opening in the neighbor does not erase the other wall's end cap");

    const std::vector<HostedOpening> neighbor_openings{
        {"opening-at-neighbor-start", 0, .75, 0, 2.1}};
    const auto original_neighbor = wall_plan_footprint(second, neighbor_openings, .4);
    require_boundary(result.at("wall-b").footprint, original_neighbor,
                     "the neighbor's flush opening keeps its original cut geometry");
    require_boundary(result.at("wall-b").strokes, original_neighbor,
                     "the neighbor's opening and exposed ends retain their original strokes");
}

void test_fully_cut_neighbor_does_not_create_a_join() {
    const Segment first{{0, 0}, {4, 0}, 0};
    const Segment second{{4, 0}, {4, 3}, 0};
    auto entities = corner_entities(first, .2, second, .4);
    // The opening spans the complete plan length. Its shorter vertical extent
    // leaves a semantically valid wall above the opening while plan geometry is empty.
    entities.emplace("opening-through-neighbor",
                     opening_entity("opening-through-neighbor", "wall-b", 0, 3));

    const auto result = document_wall_plan_geometry(entities);
    require(result.size() == 2, "a fully cut plan neighbor remains a valid projected wall");
    const auto original_first = wall_plan_footprint(first, {}, .2);
    require_boundary(result.at("wall-a").footprint, original_first,
                     "a fully cut neighbor leaves the other wall's original footprint capped");
    require_boundary(result.at("wall-a").strokes, original_first,
                     "a fully cut neighbor leaves the other wall's endpoint cap stroke");
    require(contains_segment(result.at("wall-a").strokes, {{4, .1}, {4, -.1}, 0}),
            "a fully cut neighbor cannot remove the other wall's cap");
    require(result.at("wall-b").footprint.empty() && result.at("wall-b").strokes.empty(),
            "a wall cut across its full plan length remains fully empty");
}

void test_document_projection_uses_hidden_openings_and_validated_width_overrides() {
    auto entities = corner_entities({{0, 0}, {4, 0}, 0}, .2,
                                    {{4, 0}, {4, 3}, 0}, .4);
    entities.emplace("opening-hidden", opening_entity("opening-hidden", "wall-a", 1, 1));
    const auto original_entities = entities;
    const auto source_geometry = document_wall_plan_geometry(entities);
    const auto& source_wall = source_geometry.at("wall-a");
    const Boundary expected_source_footprint{
        {{0, .1}, {1, .1}, 0}, {{1, .1}, {1, -.1}, 0},
        {{1, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0},
        {{2, .1}, {3.8, .1}, 0}, {{3.8, .1}, {4.2, -.1}, 0},
        {{4.2, -.1}, {2, -.1}, 0}, {{2, -.1}, {2, .1}, 0}};
    require_boundary(source_wall.footprint, expected_source_footprint,
                     "hidden source openings cut geometry at their original physical stations");
    require(!contains_segment(source_wall.strokes, {{3.8, .1}, {4.2, -.1}, 0}),
            "the source opening does not break the neighboring wall join");

    Wall candidate{"wall-a", {{0, 0}, {4, 0}, 0}, .2, 3.0, 0.0,
                   {{"opening-hidden", 1, 1.5, 0, 2.1}}, {}, std::nullopt};
    validate_wall_semantics(candidate);
    const auto candidate_before = candidate;
    const std::map<std::string, Wall, std::less<>> overrides{{"wall-a", candidate}};
    const auto overridden_geometry = document_wall_plan_geometry(entities, overrides);
    const auto& overridden_wall = overridden_geometry.at("wall-a");
    const Boundary expected_overridden_footprint{
        {{0, .1}, {1, .1}, 0}, {{1, .1}, {1, -.1}, 0},
        {{1, -.1}, {0, -.1}, 0}, {{0, -.1}, {0, .1}, 0},
        {{2.5, .1}, {3.8, .1}, 0}, {{3.8, .1}, {4.2, -.1}, 0},
        {{4.2, -.1}, {2.5, -.1}, 0}, {{2.5, -.1}, {2.5, .1}, 0}};
    require_boundary(overridden_wall.footprint, expected_overridden_footprint,
                     "a validated opening-width candidate moves only its jamb station");
    require(!contains_segment(overridden_wall.strokes, {{3.8, .1}, {4.2, -.1}, 0}),
            "a validated opening-width override preserves the wall corner join");
    require(entities == original_entities && candidate.id == candidate_before.id &&
                near(candidate.baseline, candidate_before.baseline) &&
                candidate.thickness == candidate_before.thickness &&
                candidate.height == candidate_before.height &&
                candidate.elevation == candidate_before.elevation &&
                candidate.openings == candidate_before.openings &&
                candidate.layers == candidate_before.layers &&
                candidate.slope_rise == candidate_before.slope_rise,
            "projection leaves source entities and the supplied candidate unchanged");
}

void test_corner_fallback_is_symmetric_when_a_short_run_cannot_miter() {
    auto entities = corner_entities({{0, 0}, {4, 0}, 0}, .2,
                                    {{4, 0}, {4, 3}, 0}, .4);
    entities.emplace("near-corner", opening_entity("near-corner", "wall-a", 3.9, .05));
    const auto result = document_wall_plan_geometry(entities);
    const auto first = wall_plan_footprint({{0, 0}, {4, 0}, 0},
        {{"near-corner", 3.9, .05, 0, 2.1}}, .2);
    const auto second = wall_plan_footprint({{4, 0}, {4, 3}, 0}, {}, .4);
    require_boundary(result.at("wall-a").footprint, first,
        "a corner miter cannot invert the short wall run beside an opening");
    require_boundary(result.at("wall-b").footprint, second,
        "the partner retains its original cap when the first miter is unsupported");
    require_boundary(result.at("wall-b").strokes, second,
        "a guarded corner fallback cannot leave one side without a cap");
    require(!result.at("wall-a").joined_end && !result.at("wall-b").joined_start,
        "both guarded endpoints declare the same unjoined physical state");
}

void test_malformed_walls_are_skipped_without_poisoning_valid_geometry() {
    Entities entities;
    entities.emplace("wall-valid", wall_entity("wall-valid", {{0, 0}, {4, 0}, 0}, .2));
    entities.emplace("wall-malformed", Entity{
        "wall-malformed", "wall", {{"thickness_m", .2}, {"height_m", 3.0}},
        false, Json::object()});
    const auto result = document_wall_plan_geometry(entities);
    require(result.size() == 1 && result.contains("wall-valid") &&
                !result.contains("wall-malformed"),
            "malformed walls are omitted while valid walls remain available");
    const auto original = wall_plan_footprint({{0, 0}, {4, 0}, 0}, {}, .2);
    require_boundary(result.at("wall-valid").footprint, original,
                     "skipping a malformed wall leaves a valid neighbor's footprint unchanged");
    require_boundary(result.at("wall-valid").strokes, original,
                     "a skipped malformed wall cannot suppress a valid endpoint cap");
}
} // namespace

int main() {
    try {
        test_unequal_thickness_corner_miters_both_endpoint_orientations();
        test_context_and_elevation_mismatches_keep_endpoint_caps();
        test_three_way_junction_retains_footprints_and_removes_internal_strokes();
        test_interior_partition_joins_keep_openings_and_contexts();
        test_partition_near_mitered_corner_uses_both_wall_materials();
        test_curved_endpoint_neighbor_keeps_both_walls_capped();
        test_neighbor_flush_opening_does_not_create_a_join();
        test_fully_cut_neighbor_does_not_create_a_join();
        test_document_projection_uses_hidden_openings_and_validated_width_overrides();
        test_malformed_walls_are_skipped_without_poisoning_valid_geometry();
        test_corner_fallback_is_symmetric_when_a_short_run_cannot_miter();
        std::cout << "Document wall plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
