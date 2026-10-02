#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_authoring_session.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/model_phases.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

Entity entity(std::string id, std::string type, Json properties = Json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, Json::object()};
}

struct WallSpec {
    std::string id;
    Segment baseline;
    double thickness{0.2};
};

std::vector<WallSpec> rectangle_walls(double bottom = 0.2, double right = 0.2,
                                      double top = 0.2, double left = 0.2) {
    return {
        {"wall-bottom", {{0, 0}, {4, 0}, 0}, bottom},
        {"wall-right", {{4, 0}, {4, 3}, 0}, right},
        {"wall-top", {{4, 3}, {0, 3}, 0}, top},
        {"wall-left", {{0, 3}, {0, 0}, 0}, left},
    };
}

std::vector<WallSpec> capsule_walls(double thickness = 0.2) {
    return {
        {"wall-bottom", {{0, 0}, {4, 0}, 0}, thickness},
        {"wall-right-arc", {{4, 0}, {4, 3}, std::numbers::pi}, thickness},
        {"wall-top", {{4, 3}, {0, 3}, 0}, thickness},
        {"wall-left-arc", {{0, 3}, {0, 0}, std::numbers::pi}, thickness},
    };
}

Vec2 circle_center(const Segment& arc) {
    const auto dx = arc.end.x - arc.start.x;
    const auto dy = arc.end.y - arc.start.y;
    const auto offset = 0.5 / std::tan(arc.sweep_radians * 0.5);
    return {arc.start.x + dx * 0.5 - dy * offset,
            arc.start.y + dy * 0.5 + dx * offset};
}

Json segment_json(const Segment& segment) {
    return {{"start", {segment.start.x, segment.start.y}},
            {"end", {segment.end.x, segment.end.y}},
            {"sweep_radians", segment.sweep_radians}};
}

Json boundary_json(const Boundary& boundary) {
    Json result = Json::array();
    for (const auto& segment : boundary) result.push_back(segment_json(segment));
    return result;
}

std::vector<std::string> wall_ids(const std::vector<WallSpec>& specs) {
    std::vector<std::string> result;
    for (const auto& spec : specs) result.push_back(spec.id);
    return result;
}

std::vector<Entity> base_entities(const std::vector<WallSpec>& walls,
                                  bool appraisal = false, bool opening = false) {
    Json property{{"calculation_workflow", appraisal ? "appraisal" : "measurement"}};
    if (appraisal) {
        property["appraisal_policy"] = {{"policy_kind", "residential_declared"}, {"version", 1},
                                        {"property_kind", "detached_single_family"},
                                        {"measurement_basis", "exterior"}};
    }
    std::vector<Entity> result{
        entity("property-1", "property", property),
        entity("building-1", "building", {{"property_id", "property-1"}}),
        entity("floor-1", "floor", {{"building_id", "building-1"},
                                      {"appraisal_facts", {{"grade", "above"}}}}),
        entity("layer-1", "layer", {{"floor_id", "floor-1"}}),
    };
    for (const auto& wall : walls) {
        result.push_back(entity(wall.id, "wall",
            {{"baseline", segment_json(wall.baseline)}, {"thickness_m", wall.thickness},
             {"height_m", 3.0}, {"elevation_m", 0.0}, {"property_id", "property-1"},
             {"building_id", "building-1"}, {"floor_id", "floor-1"},
             {"layer_id", "layer-1"}}));
    }
    if (opening) {
        result.push_back(entity("opening-1", "opening",
            {{"wall_id", walls.front().id}, {"opening_kind", "window"}, {"mark", "W1"},
             {"offset_m", 0.5}, {"width_m", 0.5}, {"sill_m", 1.0}, {"height_m", 1.0}}));
    }
    return result;
}

Entity measurement_entity(const Boundary& boundary, const Json& source,
                          bool appraisal = false) {
    Json properties{{"property_id", "property-1"}, {"building_id", "building-1"},
                    {"floor_id", "floor-1"}, {"layer_id", "layer-1"},
                    {"boundary", boundary_json(boundary)},
                    {"wall_measurement_source", source}, {"calculation_scope", "building"}};
    if (appraisal) {
        properties["appraisal_facts"] = {{"finish", "finished"}, {"access", "direct_interior"},
            {"ceiling_eligibility", "standard"}, {"area_use", "dwelling"},
            {"boundary_role", "measured_area"}};
    }
    return entity("area-1", "measurement_boundary", std::move(properties));
}

using EdgeKey = std::tuple<double, double, double, double>;

std::vector<EdgeKey> edge_keys(const Boundary& boundary) {
    std::vector<EdgeKey> result;
    result.reserve(boundary.size());
    for (const auto& segment : boundary) {
        auto first = std::pair{segment.start.x, segment.start.y};
        auto second = std::pair{segment.end.x, segment.end.y};
        if (second < first) std::swap(first, second);
        result.emplace_back(first.first, first.second, second.first, second.second);
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool parallel(const Segment& left, const Segment& right) {
    const Vec2 a{left.end.x - left.start.x, left.end.y - left.start.y};
    const Vec2 b{right.end.x - right.start.x, right.end.y - right.start.y};
    const auto scale = std::hypot(a.x, a.y) * std::hypot(b.x, b.y);
    return scale > 0.0 && std::abs(a.x * b.y - a.y * b.x) <= scale * 1e-12;
}

void rectangle_offsets_outward_and_records_sources() {
    const auto specs = rectangle_walls();
    auto entities = base_entities(specs);
    const auto document = Document::create(std::move(entities));
    const auto result = derive_exterior_wall_measurement(document.snapshot(), wall_ids(specs));
    const auto bounds = boundary_bounds(result.boundary);
    near(std::abs(signed_area(result.boundary)), 13.44, 1e-10,
         "a 4 by 3 rectangle with 0.2 metre walls must measure 13.44 square metres outside");
    near(bounds.minimum.x, -0.1, 1e-10, "the exterior outline must offset left by half thickness");
    near(bounds.minimum.y, -0.1, 1e-10, "the exterior outline must offset down by half thickness");
    near(bounds.maximum.x, 4.1, 1e-10, "the exterior outline must offset right by half thickness");
    near(bounds.maximum.y, 3.1, 1e-10, "the exterior outline must offset up by half thickness");
    require(result.source.at("version") == 1 && result.source.at("basis") == "exterior" &&
                result.source.at("walls").size() == 4,
            "the derived source must identify version one exterior wall inputs");
    const auto& first = result.source.at("walls").front();
    require(first.contains("id") && first.contains("context") && !first.contains("baseline") &&
                !first.contains("thickness_m"),
            "source records must retain stable wall identity and context without freezing geometry");
    require(first.at("context").at("floor_id") == "floor-1" &&
                first.at("context").at("layer_id") == "layer-1" &&
                first.at("context").at("property_id") == "property-1" &&
                first.at("context").at("building_id") == "building-1",
            "source records must retain available floor, layer, property, and building context");
}

void shuffled_and_reversed_walls_keep_the_same_outline() {
    const auto original = rectangle_walls();
    auto changed_order = original;
    std::reverse(changed_order.begin(), changed_order.end());
    for (auto& wall : changed_order) std::swap(wall.baseline.start, wall.baseline.end);
    auto first_document = Document::create(base_entities(original));
    auto second_document = Document::create(base_entities(changed_order));
    const auto first = derive_exterior_wall_measurement(first_document.snapshot(), wall_ids(original));
    const auto second = derive_exterior_wall_measurement(second_document.snapshot(), wall_ids(changed_order));
    require(edge_keys(first.boundary) == edge_keys(second.boundary),
            "wall selection order and baseline direction must not change the exterior outline");
    require(signed_area(first.boundary) > 0.0 && signed_area(second.boundary) > 0.0,
            "derived outlines must use canonical positive winding");
}

void stable_wall_identity_seeds_output_order_across_coordinate_edits() {
    const auto make_walls = [](std::array<Vec2, 4> vertices) {
        return std::vector<WallSpec>{
            {"a-seed", {vertices[0], vertices[1], 0}, 0.2},
            {"b-next", {vertices[1], vertices[2], 0}, 0.2},
            {"c-next", {vertices[2], vertices[3], 0}, 0.2},
            {"d-last", {vertices[3], vertices[0], 0}, 0.2},
        };
    };
    auto original = make_walls({Vec2{0, 0}, Vec2{4, 1}, Vec2{3, 4}, Vec2{-1, 3}});
    auto original_document = Document::create(base_entities(original));
    const auto first = derive_exterior_wall_measurement(
        original_document.snapshot(), wall_ids(original));
    require(parallel(first.boundary[0], original[0].baseline) &&
                parallel(first.boundary[1], original[1].baseline),
            "positive outline winding must start at the lowest-ID wall and follow its next wall");

    auto moved = make_walls({Vec2{5, 0}, Vec2{4, 1}, Vec2{3, 4}, Vec2{-1, 3}});
    auto moved_document = Document::create(base_entities(moved));
    const auto second = derive_exterior_wall_measurement(moved_document.snapshot(), wall_ids(moved));
    require(signed_area(second.boundary) > 0.0 &&
                parallel(second.boundary[0], moved[0].baseline) &&
                parallel(second.boundary[1], moved[1].baseline),
            "an endpoint crossing the coordinate sort order must retain stable source-wall ordering");
}

void concave_l_outline_and_per_wall_thickness_are_respected() {
    const std::vector<Vec2> vertices{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
    std::vector<WallSpec> l_shape;
    for (std::size_t index = 0; index < vertices.size(); ++index)
        l_shape.push_back({"l-wall-" + std::to_string(index),
            {vertices[index], vertices[(index + 1) % vertices.size()], 0}, 0.2});
    auto l_document = Document::create(base_entities(l_shape));
    const auto l_result = derive_exterior_wall_measurement(l_document.snapshot(), wall_ids(l_shape));
    near(std::abs(signed_area(l_result.boundary)), 13.64, 1e-10,
         "a concave L outline must retain its re-entrant corner during outward offset");

    const auto varied = rectangle_walls(0.2, 0.4, 0.6, 0.8);
    auto varied_document = Document::create(base_entities(varied));
    const auto varied_result = derive_exterior_wall_measurement(
        varied_document.snapshot(), wall_ids(varied));
    near(std::abs(signed_area(varied_result.boundary)), 15.64, 1e-10,
         "each rectangle face must use its own half-thickness at the mitered corners");
}

void curved_exterior_is_analytical_reversible_and_current() {
    const auto shell = capsule_walls();
    auto with_partition = shell;
    with_partition.push_back({"partition", {{2, 0}, {2, 3}, 0}, 0.1});
    // This detached line is inside the true right-hand semicircular bulb but
    // outside the polygon formed by replacing that arc with its chord.
    with_partition.push_back({"bulb-interior", {{4.8, 0.5}, {4.8, 2.5}, 0}, 0.1});
    const auto source_entities = base_entities(with_partition);
    const auto source_document = Document::create(source_entities);
    const auto ids = exterior_wall_measurement_sources(source_document.snapshot(),
                                                        wall_ids(with_partition));
    require(ids == std::vector<std::string>{"wall-bottom", "wall-left-arc",
                "wall-right-arc", "wall-top"},
            "recognition must include the complete curved exterior and exclude a straight partition");

    const auto measured = derive_exterior_wall_measurement(source_document.snapshot(), ids);
    require(measured.boundary.size() == 4 && validate_boundary(measured.boundary).empty(),
            "a mixed straight and curved exterior must retain a valid four-edge analytical outline");
    require(std::count_if(measured.boundary.begin(), measured.boundary.end(), [](const Segment& edge) {
                return edge.sweep_radians != 0.0;
            }) == 2,
            "the exterior offset must retain both analytical semicircular walls");
    near(signed_area(measured.boundary), 12.8 + std::numbers::pi * 1.6 * 1.6, 1e-9,
         "the 0.2m offset capsule must have exact analytic exterior area");
    near(perimeter(measured.boundary), 8.0 + 2.0 * std::numbers::pi * 1.6, 1e-9,
         "the 0.2m offset capsule must have exact analytic exterior perimeter");

    auto reversed = with_partition;
    std::reverse(reversed.begin(), reversed.end());
    for (auto& wall : reversed) {
        std::swap(wall.baseline.start, wall.baseline.end);
        wall.baseline.sweep_radians = -wall.baseline.sweep_radians;
    }
    const auto reversed_document = Document::create(base_entities(reversed));
    const auto reversed_ids = exterior_wall_measurement_sources(reversed_document.snapshot(),
                                                                 wall_ids(reversed));
    require(reversed_ids == ids,
            "wall order and reversed directed arcs must preserve recognized source identities");
    const auto reversed_measurement = derive_exterior_wall_measurement(reversed_document.snapshot(), ids);
    require(boundary_json(reversed_measurement.boundary) == boundary_json(measured.boundary),
            "wall order and reversed directed arcs must preserve the canonical analytical exterior");

    auto area = measurement_entity(measured.boundary, measured.source);
    auto retained_entities = source_entities;
    retained_entities.push_back(area);
    const auto retained_document = Document::create(retained_entities);
    require(wall_measurement_source_current(retained_document.snapshot(),
                retained_document.snapshot().entities().at("area-1")),
            "a serialized curved exterior must remain current against unchanged wall sources");
    auto edited_outline = area;
    for (std::size_t index = 0; index < measured.boundary.size(); ++index) {
        if (measured.boundary[index].sweep_radians != 0.0) {
            auto edited_boundary = measured.boundary;
            edited_boundary[index].sweep_radians = -edited_boundary[index].sweep_radians;
            edited_outline.properties["boundary"] = boundary_json(edited_boundary);
            break;
        }
    }
    auto edited_outline_entities = source_entities;
    edited_outline_entities.push_back(edited_outline);
    const auto edited_outline_document = Document::create(edited_outline_entities);
    require(!wall_measurement_source_current(edited_outline_document.snapshot(),
                edited_outline_document.snapshot().entities().at("area-1")),
            "changing only the measured arc sweep with fixed endpoints must stale its source outline");
    for (auto& item : retained_entities) {
        if (item.id == "wall-right-arc")
            item.properties["baseline"]["sweep_radians"] = -std::numbers::pi;
    }
    const auto changed_arc_document = Document::create(retained_entities);
    require(!wall_measurement_source_current(changed_arc_document.snapshot(),
                changed_arc_document.snapshot().entities().at("area-1")),
            "changing only a source arc sweep with fixed endpoints must stale the measured boundary");
}

void concave_curved_wall_offsets_concentrically_and_analytically() {
    const std::vector<WallSpec> walls{
        {"bottom", {{0, 0}, {4, 0}, 0}, 0.2},
        {"right", {{4, 0}, {4, 3}, 0}, 0.2},
        {"top", {{4, 3}, {0, 3}, 0}, 0.2},
        // On the downward chord a negative sweep bows into the footprint,
        // giving this loop a concave curved side.
        {"inward-arc", {{0, 3}, {0, 0}, -std::numbers::pi / 2.0}, 0.2},
    };
    const auto document = Document::create(base_entities(walls));
    const auto measured = derive_exterior_wall_measurement(document.snapshot(), wall_ids(walls));
    require(validate_boundary(measured.boundary).empty() && signed_area(measured.boundary) > 0.0,
            "a concave analytical wall loop must produce a valid canonical exterior");
    const auto curve = std::find_if(measured.boundary.begin(), measured.boundary.end(),
        [](const Segment& segment) { return segment.sweep_radians != 0.0; });
    require(curve != measured.boundary.end() && curve->sweep_radians < 0.0,
            "outward offset must preserve the signed curvature of a concave exterior wall");
    const auto source_radius = 3.0 / (2.0 * std::sin(std::numbers::pi / 4.0));
    near(segment_length(*curve) / std::abs(curve->sweep_radians), source_radius - 0.1, 1e-9,
         "the concave wall offset must be concentric and reduce exterior-side radius by half-thickness");

    auto collapsed = walls;
    collapsed.back().thickness = 5.0;
    const auto collapsed_document = Document::create(base_entities(collapsed));
    rejects([&] { (void)derive_exterior_wall_measurement(collapsed_document.snapshot(),
                                                          wall_ids(collapsed)); },
            "an inward concave arc offset whose radius collapses must fail closed");
}

void major_arc_endpoint_tangent_order_keeps_partition_out_of_exterior() {
    const auto point_on_circle = [](double degrees) {
        const auto angle = degrees * std::numbers::pi / 180.0;
        return Vec2{2.0 * std::cos(angle), 2.0 * std::sin(angle)};
    };
    const auto start = point_on_circle(75.0);
    const auto end = point_on_circle(345.0);
    const auto split = point_on_circle(95.0);
    const std::vector<WallSpec> walls{
        {"shell-arc", {start, end, 3.0 * std::numbers::pi / 2.0}, 0.1},
        {"shell-end", {end, {0, 0}, 0}, 0.1},
        {"shell-start", {{0, 0}, start, 0}, 0.1},
        {"partition", {start, split, 0}, 0.1},
    };
    const auto document = Document::create(base_entities(walls));
    const auto ids = exterior_wall_measurement_sources(document.snapshot(), wall_ids(walls));
    require(ids == std::vector<std::string>{"shell-arc", "shell-end", "shell-start"},
            "major-arc tangent ordering must retain the complete exterior and exclude its chord partition");
    const auto measured = derive_exterior_wall_measurement(document.snapshot(), ids);
    require(validate_boundary(measured.boundary).empty() && signed_area(measured.boundary) > 0.0 &&
                perimeter(measured.boundary) > 0.0 && measured.source.at("walls").size() == 3,
            "a major-arc shell must derive valid analytical area/perimeter with exact shell provenance");

    auto reversed = walls;
    std::reverse(reversed.begin(), reversed.end());
    for (auto& wall : reversed) {
        std::swap(wall.baseline.start, wall.baseline.end);
        wall.baseline.sweep_radians = -wall.baseline.sweep_radians;
    }
    const auto reversed_document = Document::create(base_entities(reversed));
    const auto reversed_ids = exterior_wall_measurement_sources(reversed_document.snapshot(), wall_ids(reversed));
    require(reversed_ids == ids && boundary_json(derive_exterior_wall_measurement(
                reversed_document.snapshot(), ids).boundary) == boundary_json(measured.boundary),
            "major-arc face selection and analytical offset must be invariant to shuffled reversed sources");
}

void four_concentric_quarter_arcs_offset_as_one_exact_circle() {
    const std::array<Vec2, 4> points{{{2, 0}, {0, 2}, {-2, 0}, {0, -2}}};
    std::vector<WallSpec> walls;
    for (std::size_t index = 0; index < points.size(); ++index)
        walls.push_back({"quarter-" + std::to_string(index),
            {points[index], points[(index + 1) % points.size()], std::numbers::pi / 2.0}, 0.2});
    const auto document = Document::create(base_entities(walls));
    const auto ids = exterior_wall_measurement_sources(document.snapshot(), wall_ids(walls));
    require(ids == wall_ids(walls), "a four-arc circle must recognize every unique exterior wall");
    const auto measured = derive_exterior_wall_measurement(document.snapshot(), ids);
    require(measured.boundary.size() == 4 && validate_boundary(measured.boundary).empty() &&
                std::all_of(measured.boundary.begin(), measured.boundary.end(), [](const Segment& edge) {
                    return edge.sweep_radians > 0.0;
                }),
            "concentric quarter-arc joins must remain four analytical arcs");
    near(signed_area(measured.boundary), std::numbers::pi * 2.1 * 2.1, 1e-9,
         "the four-arc exterior must have the exact area of the expanded circle");
    near(perimeter(measured.boundary), 2.0 * std::numbers::pi * 2.1, 1e-9,
         "the four-arc exterior must have the exact circumference of the expanded circle");
    for (const auto& arc : measured.boundary) {
        const auto center = circle_center(arc);
        near(center.x, 0.0, 1e-9, "each offset quarter arc must retain the shared circle center x");
        near(center.y, 0.0, 1e-9, "each offset quarter arc must retain the shared circle center y");
    }

    auto reversed = walls;
    std::reverse(reversed.begin(), reversed.end());
    for (auto& wall : reversed) {
        std::swap(wall.baseline.start, wall.baseline.end);
        wall.baseline.sweep_radians = -wall.baseline.sweep_radians;
    }
    const auto reversed_document = Document::create(base_entities(reversed));
    const auto reversed_ids = exterior_wall_measurement_sources(reversed_document.snapshot(), wall_ids(reversed));
    const auto reversed_measurement = derive_exterior_wall_measurement(reversed_document.snapshot(), reversed_ids);
    require(reversed_ids == ids && boundary_json(reversed_measurement.boundary) ==
                boundary_json(measured.boundary),
            "quarter-arc recognition and offset must be invariant to reversed shuffled sources");
    auto area = measurement_entity(measured.boundary, measured.source);
    auto entities = base_entities(walls);
    entities.push_back(area);
    const auto retained = Document::create(entities);
    require(wall_measurement_source_current(retained.snapshot(), retained.snapshot().entities().at("area-1")),
            "the exact full-circle measurement must remain source-current after persistence round-trip");
}

void adjacent_convex_arcs_use_each_wall_thickness_and_analytic_miters() {
    const std::vector<WallSpec> walls{
        {"a-bottom-arc", {{0, 0}, {4, 0}, std::numbers::pi / 6.0}, 0.2},
        {"b-right-arc", {{4, 0}, {4, 3}, std::numbers::pi / 6.0}, 0.3},
        {"c-top", {{4, 3}, {0, 3}, 0}, 0.4},
        {"d-left", {{0, 3}, {0, 0}, 0}, 0.5},
    };
    const auto document = Document::create(base_entities(walls));
    const auto measured = derive_exterior_wall_measurement(document.snapshot(), wall_ids(walls));
    require(measured.boundary.size() == walls.size() && validate_boundary(measured.boundary).empty(),
            "adjacent convex arcs with distinct radii and wall thicknesses must form a valid analytical offset");
    const auto source_bottom_center = circle_center(walls[0].baseline);
    const auto source_right_center = circle_center(walls[1].baseline);
    const auto& bottom = measured.boundary[0];
    const auto& right = measured.boundary[1];
    require(bottom.sweep_radians > 0.0 && right.sweep_radians > 0.0,
            "convex offsets must retain each arc's positive sweep");
    const auto bottom_center = circle_center(bottom);
    const auto right_center = circle_center(right);
    near(bottom_center.x, source_bottom_center.x, 1e-8,
         "the bottom convex offset must remain concentric with its source wall");
    near(bottom_center.y, source_bottom_center.y, 1e-8,
         "the bottom convex offset must remain concentric with its source wall");
    near(right_center.x, source_right_center.x, 1e-8,
         "the right convex offset must remain concentric with its source wall");
    near(right_center.y, source_right_center.y, 1e-8,
         "the right convex offset must remain concentric with its source wall");
    const auto source_bottom_radius = segment_length(walls[0].baseline) /
                                      std::abs(walls[0].baseline.sweep_radians);
    const auto source_right_radius = segment_length(walls[1].baseline) /
                                     std::abs(walls[1].baseline.sweep_radians);
    near(segment_length(bottom) / bottom.sweep_radians,
         source_bottom_radius + walls[0].thickness * 0.5, 1e-8,
         "the bottom arc must use its own half-thickness for the outward radius");
    near(segment_length(right) / right.sweep_radians,
         source_right_radius + walls[1].thickness * 0.5, 1e-8,
         "the right arc must use its own half-thickness for the outward radius");
    near(measured.boundary[2].start.y, 3.2, 1e-8,
         "the top straight face must use its own 0.4m wall thickness");
    near(measured.boundary[2].end.y, 3.2, 1e-8,
         "the top straight face must remain a line at its half-thickness offset");
    near(measured.boundary[3].start.x, -0.25, 1e-8,
         "the left straight face must use its own 0.5m wall thickness");
    near(measured.boundary[3].end.x, -0.25, 1e-8,
         "the left straight face must remain a line at its half-thickness offset");
    require(std::isfinite(signed_area(measured.boundary)) && std::isfinite(perimeter(measured.boundary)),
            "the mixed convex analytical shell must provide finite area and perimeter");

    auto reversed = walls;
    std::reverse(reversed.begin(), reversed.end());
    for (auto& wall : reversed) {
        std::swap(wall.baseline.start, wall.baseline.end);
        wall.baseline.sweep_radians = -wall.baseline.sweep_radians;
    }
    const auto reversed_document = Document::create(base_entities(reversed));
    const auto reversed_measurement = derive_exterior_wall_measurement(
        reversed_document.snapshot(), wall_ids(reversed));
    require(boundary_json(reversed_measurement.boundary) == boundary_json(measured.boundary),
            "different-thickness analytical miter geometry must be invariant to source direction and order");
}

void equal_offset_collinear_wall_continuations_remain_valid() {
    const std::vector<WallSpec> split_rectangle{
        {"wall-bottom-a", {{0, 0}, {2, 0}, 0}, 0.2},
        {"wall-bottom-b", {{2, 0}, {4, 0}, 0}, 0.2},
        {"wall-right", {{4, 0}, {4, 3}, 0}, 0.2},
        {"wall-top", {{4, 3}, {0, 3}, 0}, 0.2},
        {"wall-left", {{0, 3}, {0, 0}, 0}, 0.2},
    };
    auto document = Document::create(base_entities(split_rectangle));
    const auto result = derive_exterior_wall_measurement(
        document.snapshot(), wall_ids(split_rectangle));
    near(std::abs(signed_area(result.boundary)), 13.44, 1e-10,
         "collinear continuations with the same offset must share a valid exterior outline");
    require(result.boundary.size() == split_rectangle.size(),
            "the exterior cycle must preserve one measured segment per source wall");
}

void source_guard_detects_changed_or_missing_walls_but_ignores_openings() {
    auto specs = rectangle_walls();
    auto base = base_entities(specs, false, true);
    auto document = Document::create(base);
    const auto result = derive_exterior_wall_measurement(document.snapshot(), wall_ids(specs));
    auto measured = measurement_entity(result.boundary, result.source);
    auto with_boundary = base;
    with_boundary.push_back(measured);
    document = Document::create(with_boundary);
    require(wall_measurement_source_current(document.snapshot(),
                document.snapshot().entities().at("area-1")),
            "a boundary matching its current source walls must remain current");

    auto changed_opening = with_boundary;
    for (auto& item : changed_opening)
        if (item.id == "opening-1") item.properties["width_m"] = 0.9;
    auto opening_document = Document::create(std::move(changed_opening));
    require(wall_measurement_source_current(opening_document.snapshot(),
                opening_document.snapshot().entities().at("area-1")),
            "changing a hosted opening must not stale the exterior wall outline");

    auto changed_wall = with_boundary;
    for (auto& item : changed_wall)
        if (item.id == "wall-bottom") item.properties["thickness_m"] = 0.4;
    auto wall_document = Document::create(std::move(changed_wall));
    require(!wall_measurement_source_current(wall_document.snapshot(),
                wall_document.snapshot().entities().at("area-1")),
            "a wall thickness change that alters the measured outline must make its source stale");

    auto missing_wall = with_boundary;
    std::erase_if(missing_wall, [](const auto& item) {
        return item.id == "wall-bottom" || item.id == "opening-1";
    });
    auto missing_document = Document::create(std::move(missing_wall));
    require(!wall_measurement_source_current(missing_document.snapshot(),
                missing_document.snapshot().entities().at("area-1")),
            "a source wall deletion must make the derived boundary stale");
}

void source_guard_accepts_legacy_boundaries_and_rejects_malformed_or_edited_sources() {
    const auto specs = rectangle_walls();
    auto entities = base_entities(specs);
    const auto document = Document::create(entities);
    auto result = derive_exterior_wall_measurement(document.snapshot(), wall_ids(specs));
    Entity legacy = entity("legacy", "measurement_boundary");
    require(wall_measurement_source_current(document.snapshot(), legacy),
            "a boundary with no wall measurement source must preserve legacy behavior");

    auto anonymous = measurement_entity(result.boundary, result.source);
    LegacyBoundaryIdentityOptions identities;
    for (std::size_t index = 0; index < result.boundary.size(); ++index) {
        identities.segment_ids.push_back("segment-" + std::to_string(index));
        identities.vertex_ids.push_back("vertex-" + std::to_string(index));
    }
    const auto identified = upgrade_legacy_boundary_entity(anonymous, identities);
    require(wall_measurement_source_current(document.snapshot(), identified),
            "identified boundaries refreshed through the document codec must remain current");

    auto malformed = measurement_entity(result.boundary, Json::array());
    require(!wall_measurement_source_current(document.snapshot(), malformed),
            "a malformed source payload must fail closed");
    malformed.properties["wall_measurement_source"] = {
        {"version", 99}, {"basis", "exterior"}, {"walls", Json::array()}};
    require(!wall_measurement_source_current(document.snapshot(), malformed),
            "an unknown source schema version must fail closed");

    auto edited = measurement_entity(result.boundary, result.source);
    edited.properties["boundary"] = boundary_json(
        {{ {10, 10}, {14, 10}, 0}, {{14, 10}, {14, 13}, 0},
          {{14, 13}, {10, 13}, 0}, {{10, 13}, {10, 10}, 0} });
    require(!wall_measurement_source_current(document.snapshot(), edited),
            "editing valid boundary geometry must make the measurement source stale");

    entities.push_back(entity("layer-other", "layer", {{"floor_id", "floor-1"}}));
    entities.push_back(measurement_entity(result.boundary, result.source));
    entities.back().properties["layer_id"] = "layer-other";
    auto moved_context_document = Document::create(std::move(entities));
    require(!wall_measurement_source_current(moved_context_document.snapshot(),
                moved_context_document.snapshot().entities().at("area-1")),
            "moving a measured boundary to another layer must make its wall source stale");
}

Json appraisal_facts() {
    return {{"finish", "finished"}, {"access", "direct_interior"},
            {"ceiling_eligibility", "standard"}, {"area_use", "dwelling"},
            {"boundary_role", "measured_area"}};
}

void appraisal_withholds_stale_wall_measured_totals() {
    const auto specs = rectangle_walls();
    auto entities = base_entities(specs, true);
    auto source_document = Document::create(entities);
    const auto derived = derive_exterior_wall_measurement(
        source_document.snapshot(), wall_ids(specs));
    auto measured = measurement_entity(derived.boundary, derived.source, true);
    measured.properties["appraisal_facts"] = appraisal_facts();
    entities.push_back(measured);
    auto current_document = Document::create(entities);
    const auto current_report = build_appraisal_document_report(
        current_document.snapshot(), "property-1");
    require(current_report.qualified && current_report.calculation.has_value(),
            "a current exterior wall measurement may contribute to declared appraisal totals");

    std::set<std::string, std::less<>> visible_ids{
        "property-1", "building-1", "floor-1", "layer-1", "area-1",
        "wall-right", "wall-top", "wall-left"};
    const auto phase_report = build_appraisal_document_report(
        current_document.snapshot(), "property-1", AreaUnit::square_foot, &visible_ids);
    require(!phase_report.qualified && !phase_report.calculation.has_value() &&
                std::any_of(phase_report.issues.begin(), phase_report.issues.end(), [](const auto& issue) {
                    return issue.find("refresh exterior measurement from source walls") != std::string::npos;
                }),
            "a source wall excluded by the active design phase must withhold appraisal totals");

    auto edited_boundary_entities = entities;
    for (auto& item : edited_boundary_entities) {
        if (item.id == "area-1") {
            item.properties["boundary"] = boundary_json(
                {{{10, 10}, {14, 10}, 0}, {{14, 10}, {14, 13}, 0},
                 {{14, 13}, {10, 13}, 0}, {{10, 13}, {10, 10}, 0}});
        }
    }
    auto edited_boundary_document = Document::create(std::move(edited_boundary_entities));
    const auto edited_boundary_report = build_appraisal_document_report(
        edited_boundary_document.snapshot(), "property-1");
    require(!edited_boundary_report.qualified && !edited_boundary_report.calculation.has_value() &&
                std::any_of(edited_boundary_report.issues.begin(), edited_boundary_report.issues.end(),
                    [](const auto& issue) {
                        return issue.find("refresh exterior measurement from source walls") != std::string::npos;
                    }),
            "manually edited appraisal geometry must not retain its old source qualification");

    for (auto& item : entities)
        if (item.id == "wall-bottom") item.properties["thickness_m"] = 0.4;
    auto stale_document = Document::create(std::move(entities));
    const auto stale_report = build_appraisal_document_report(
        stale_document.snapshot(), "property-1");
    require(!stale_report.qualified && !stale_report.calculation.has_value(),
            "stale exterior wall geometry must not produce appraisal totals");
    require(std::any_of(stale_report.issues.begin(), stale_report.issues.end(), [](const auto& issue) {
                return issue.find("refresh exterior measurement from source walls") != std::string::npos;
            }),
            "stale appraisal data must direct the user to refresh from source walls");
    require(stale_report.boundaries.size() == 1 &&
                !stale_report.boundaries.front().qualification.qualified,
            "a stale source must mark its area qualification unqualified");
}

void open_duplicate_and_crossed_wall_loops_are_rejected() {
    auto open = rectangle_walls();
    open.pop_back();
    auto open_document = Document::create(base_entities(open));
    rejects([&] { (void)derive_exterior_wall_measurement(open_document.snapshot(), wall_ids(open)); },
            "an open source chain must not produce an exterior outline");

    const auto duplicate_id = rectangle_walls();
    auto duplicate_id_document = Document::create(base_entities(duplicate_id));
    auto duplicate_ids = wall_ids(duplicate_id);
    duplicate_ids.push_back(duplicate_ids.front());
    rejects([&] { (void)derive_exterior_wall_measurement(duplicate_id_document.snapshot(), duplicate_ids); },
            "a source wall ID may not be selected twice");

    auto duplicate_geometry = rectangle_walls();
    duplicate_geometry.push_back({"wall-duplicate", duplicate_geometry.front().baseline, 0.2});
    auto duplicate_geometry_document = Document::create(base_entities(duplicate_geometry));
    rejects([&] { (void)derive_exterior_wall_measurement(
                duplicate_geometry_document.snapshot(), wall_ids(duplicate_geometry)); },
            "duplicate baseline geometry must not be accepted as a simple loop");

    const std::vector<Vec2> crossed_vertices{{0, 0}, {4, 3}, {0, 3}, {4, 0}};
    std::vector<WallSpec> crossed;
    for (std::size_t index = 0; index < crossed_vertices.size(); ++index)
        crossed.push_back({"crossed-" + std::to_string(index),
            {crossed_vertices[index], crossed_vertices[(index + 1) % crossed_vertices.size()], 0}, 0.2});
    auto crossed_document = Document::create(base_entities(crossed));
    rejects([&] { (void)derive_exterior_wall_measurement(
                crossed_document.snapshot(), wall_ids(crossed)); },
            "a self-crossing wall loop must not produce a measured outline");

}

void invalid_thickness_and_out_of_envelope_coordinates_are_rejected() {
    auto thick = rectangle_walls();
    thick.front().thickness = 1'000'001;
    auto thick_document = Document::create(base_entities(thick));
    rejects([&] { (void)derive_exterior_wall_measurement(
                thick_document.snapshot(), wall_ids(thick)); },
            "wall thickness outside the model geometry envelope must be rejected");
    auto far = rectangle_walls();
    for (auto& wall : far) {
        wall.baseline.start.x += 1'000'001;
        wall.baseline.end.x += 1'000'001;
    }
    auto far_document = Document::create(base_entities(far));
    rejects([&] { (void)derive_exterior_wall_measurement(far_document.snapshot(), wall_ids(far)); },
            "wall baselines outside the model coordinate envelope must be rejected");

    const std::vector<WallSpec> near_parallel_corner{
        {"bottom-a", {{0, 0}, {2, 0}, 0}, 0.2},
        {"bottom-b", {{2, 0}, {4, 2e-13}, 0}, 0.2},
        {"right", {{4, 2e-13}, {4, 3}, 0}, 0.2},
        {"top", {{4, 3}, {0, 3}, 0}, 0.2},
        {"left", {{0, 3}, {0, 0}, 0}, 0.2},
    };
    const auto near_parallel_document = Document::create(base_entities(near_parallel_corner));
    rejects([&] { (void)derive_exterior_wall_measurement(near_parallel_document.snapshot(),
                                                          wall_ids(near_parallel_corner)); },
            "a finite but near-parallel noncollinear wall turn must fail closed instead of being averaged");
}

void wall_network_recognition_excludes_partitions_and_keeps_authoritative_sources() {
    const auto outer = rectangle_walls();
    auto expected_ids = wall_ids(outer);
    std::sort(expected_ids.begin(), expected_ids.end());
    const std::vector<std::vector<WallSpec>> interiors{
        {{"t", {{2, 0}, {2, 1}, 0}, 0.1}},
        {{"corner", {{0, 0}, {1, 1}, 0}, 0.1}},
        {{"external-spur", {{0, 0}, {-1, -1}, 0}, 0.1}},
        {{"isolated-interior", {{1, 1}, {2, 1}, 0}, 0.1}},
        {{"chord", {{2, 0}, {2, 3}, 0}, 0.1}},
        {{"vertical", {{2, 0}, {2, 3}, 0}, 0.1},
         {"horizontal", {{0, 1.5}, {4, 1.5}, 0}, 0.1}},
        {{"diagonal-a", {{0, 0}, {4, 3}, 0}, 0.1},
         {"diagonal-b", {{4, 0}, {0, 3}, 0}, 0.1}},
        {{"link", {{0, 1}, {1, 1}, 0}, 0.1},
         {"inner-bottom", {{1, 1}, {3, 1}, 0}, 0.1},
         {"inner-right", {{3, 1}, {3, 2}, 0}, 0.1},
         {"inner-top", {{3, 2}, {1, 2}, 0}, 0.1},
         {"inner-left", {{1, 2}, {1, 1}, 0}, 0.1}},
        {{"inner-bottom", {{1, 1}, {3, 1}, 0}, 0.1},
         {"inner-right", {{3, 1}, {3, 2}, 0}, 0.1},
         {"inner-top", {{3, 2}, {1, 2}, 0}, 0.1},
         {"inner-left", {{1, 2}, {1, 1}, 0}, 0.1}},
    };
    for (const auto& interior : interiors) {
        auto network = outer;
        network.insert(network.end(), interior.begin(), interior.end());
        auto entities = base_entities(network, false, true);
        const auto document = Document::create(entities);
        const auto ids = exterior_wall_measurement_sources(document.snapshot(), wall_ids(network));
        require(ids == expected_ids, "recognition must select the complete exterior and exclude partitions");
        const auto measured = derive_exterior_wall_measurement(document.snapshot(), ids);
        near(signed_area(measured.boundary), 13.44, 1e-10,
             "partitions and openings must preserve the exact exterior appraisal area");
        for (const auto& original : entities)
            require(document.snapshot().entities().at(original.id).properties == original.properties,
                    "recognition must never split or alter authoritative wall entities");
        std::reverse(network.begin(), network.end());
        for (auto& wall : network) std::swap(wall.baseline.start, wall.baseline.end);
        const auto reversed = Document::create(base_entities(network));
        require(exterior_wall_measurement_sources(reversed.snapshot(), wall_ids(network)) == expected_ids,
                "network order and wall direction must not change perimeter source identity");
    }
}

void wall_network_recognition_handles_split_hosts_concavity_and_varied_thickness() {
    std::vector<WallSpec> split{
        {"bottom-a", {{0, 0}, {2, 0}, 0}, 0.2},
        {"bottom-b", {{2, 0}, {4, 0}, 0}, 0.2},
        {"right", {{4, 0}, {4, 3}, 0}, 0.2},
        {"top", {{4, 3}, {0, 3}, 0}, 0.2},
        {"left", {{0, 3}, {0, 0}, 0}, 0.2},
        {"partition", {{2, 0}, {2, 2}, 0}, 0.1},
    };
    const auto split_document = Document::create(base_entities(split));
    const auto split_ids = exterior_wall_measurement_sources(split_document.snapshot(), wall_ids(split));
    require(split_ids == std::vector<std::string>{"bottom-a", "bottom-b", "left", "right", "top"},
            "a split T host must retain both complete collinear perimeter walls");
    near(signed_area(derive_exterior_wall_measurement(split_document.snapshot(), split_ids).boundary),
         13.44, 1e-10, "split hosts must preserve the exact exterior area");

    const std::vector<Vec2> vertices{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
    std::vector<WallSpec> concave;
    for (std::size_t i = 0; i < vertices.size(); ++i)
        concave.push_back({"outer-" + std::to_string(i),
            {vertices[i], vertices[(i + 1) % vertices.size()], 0}, 0.2});
    concave.push_back({"interior", {{0, 1}, {4, 1}, 0}, 0.1});
    const auto concave_document = Document::create(base_entities(concave));
    const auto concave_ids = exterior_wall_measurement_sources(concave_document.snapshot(), wall_ids(concave));
    require(concave_ids.size() == 6, "concave recognition must exclude the internal chord");
    near(signed_area(derive_exterior_wall_measurement(concave_document.snapshot(), concave_ids).boundary),
         13.64, 1e-10, "recognition must preserve concave exterior geometry");

    auto varied = rectangle_walls(0.2, 0.4, 0.6, 0.8);
    varied.push_back({"partition", {{2, 0}, {2, 3}, 0}, 0.1});
    const auto varied_document = Document::create(base_entities(varied));
    const auto varied_ids = exterior_wall_measurement_sources(varied_document.snapshot(), wall_ids(varied));
    near(signed_area(derive_exterior_wall_measurement(varied_document.snapshot(), varied_ids).boundary),
         15.64, 1e-10, "recognition must preserve individual source thicknesses");

    auto rotated = rectangle_walls();
    rotated.push_back({"diagonal-a", {{0, 0}, {4, 3}, 0}, 0.1});
    rotated.push_back({"diagonal-b", {{4, 0}, {0, 3}, 0}, 0.1});
    const auto transform = [](Vec2 point) -> Vec2 {
        return {1000 + 0.6 * point.x - 0.8 * point.y,
                -1000 + 0.8 * point.x + 0.6 * point.y};
    };
    for (auto& wall : rotated) {
        wall.baseline.start = transform(wall.baseline.start);
        wall.baseline.end = transform(wall.baseline.end);
    }
    const auto rotated_document = Document::create(base_entities(rotated));
    const auto rotated_ids = exterior_wall_measurement_sources(rotated_document.snapshot(), wall_ids(rotated));
    require(rotated_ids == std::vector<std::string>{"wall-bottom", "wall-left", "wall-right", "wall-top"},
            "translated rotated crossing partitions must retain exact exterior identities");
    near(signed_area(derive_exterior_wall_measurement(rotated_document.snapshot(), rotated_ids).boundary),
         13.44, 1e-9, "analytical recognition must remain translation and rotation independent");
}

void wall_network_recognition_rejects_ambiguous_and_unsupported_inputs() {
    std::vector<std::vector<WallSpec>> invalid;
    auto open = rectangle_walls();
    open.pop_back();
    invalid.push_back(open);
    auto duplicate = rectangle_walls();
    duplicate.push_back({"duplicate", duplicate.front().baseline, 0.2});
    invalid.push_back(duplicate);
    auto overlap = rectangle_walls();
    overlap.push_back({"overlap", {{1, 0}, {3, 0}, 0}, 0.2});
    invalid.push_back(overlap);
    auto disconnected = rectangle_walls();
    disconnected.push_back({"isolated", {{5, 1}, {6, 1}, 0}, 0.1});
    invalid.push_back(disconnected);
    auto partial = rectangle_walls();
    partial.front().baseline.start = {-1, 0};
    invalid.push_back(partial);
    auto imprecise = rectangle_walls();
    imprecise.push_back({"near-overlap", {{1, 5e-8}, {3, 5e-8}, 0}, 0.1});
    invalid.push_back(imprecise);
    // Two cycles sharing a vertex have no simple containing perimeter.
    auto kissing = rectangle_walls();
    kissing.insert(kissing.end(), {
        {"other-bottom", {{4, 3}, {6, 3}, 0}, 0.2},
        {"other-right", {{6, 3}, {6, 5}, 0}, 0.2},
        {"other-top", {{6, 5}, {4, 5}, 0}, 0.2},
        {"other-left", {{4, 5}, {4, 3}, 0}, 0.2}});
    invalid.push_back(kissing);
    // A bridge between separate lobes must not make either lobe the exterior.
    auto lobes = rectangle_walls();
    lobes.insert(lobes.end(), {
        {"bridge", {{4, 0}, {6, 0}, 0}, 0.1},
        {"other-bottom", {{6, 0}, {8, 0}, 0}, 0.2},
        {"other-right", {{8, 0}, {8, 2}, 0}, 0.2},
        {"other-top", {{8, 2}, {6, 2}, 0}, 0.2},
        {"other-left", {{6, 2}, {6, 0}, 0}, 0.2}});
    invalid.push_back(lobes);
    invalid.push_back({
        {"a", {{0, 0}, {4, 3}, 0}, 0.2}, {"b", {{4, 3}, {0, 3}, 0}, 0.2},
        {"c", {{0, 3}, {4, 0}, 0}, 0.2}, {"d", {{4, 0}, {0, 0}, 0}, 0.2}});
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        const auto& walls = invalid[index];
        const auto document = Document::create(base_entities(walls));
        rejects([&] { (void)exterior_wall_measurement_sources(document.snapshot(), wall_ids(walls)); },
                ("ambiguous or unsupported wall network fixture " + std::to_string(index) +
                 " must fail closed").c_str());
    }
    auto entities = base_entities(rectangle_walls());
    for (auto& wall : entities)
        if (wall.id == "wall-bottom") wall.properties["phase_id"] = "phase-other";
    const auto mixed_phase = Document::create(entities);
    rejects([&] { (void)exterior_wall_measurement_sources(mixed_phase.snapshot(), wall_ids(rectangle_walls())); },
            "recognition must reject mixed phase context even with shared floor and layer");
    for (const auto& field : {"property_id", "building_id", "floor_id", "layer_id"}) {
        auto context_entities = base_entities(rectangle_walls());
        const auto original_id = std::string(field).substr(0, std::string(field).size() - 3) + "-1";
        const auto original = std::find_if(context_entities.begin(), context_entities.end(),
            [&](const auto& item) { return item.id == original_id; });
        auto other = *original;
        other.id = "other-context";
        context_entities.push_back(other);
        for (auto& wall : context_entities)
            if (wall.id == "wall-bottom") wall.properties[field] = other.id;
        const auto context_document = Document::create(context_entities);
        rejects([&] { (void)exterior_wall_measurement_sources(context_document.snapshot(), wall_ids(rectangle_walls())); },
                "recognition must isolate all property, building, floor and layer contexts");
    }
    for (const auto elevation : {Json(0.1), Json("malformed")}) {
        auto elevation_entities = base_entities(rectangle_walls());
        for (auto& wall : elevation_entities)
            if (wall.id == "wall-bottom") wall.properties["elevation_m"] = elevation;
        const auto elevation_document = Document::create(elevation_entities);
        rejects([&] { (void)exterior_wall_measurement_sources(elevation_document.snapshot(), wall_ids(rectangle_walls())); },
                "recognition must reject inconsistent or malformed elevation planes");
    }
    auto legacy_entities = base_entities(rectangle_walls());
    for (auto& item : legacy_entities) item.properties.erase("elevation_m");
    const auto legacy_elevation = Document::create(legacy_entities);
    require(exterior_wall_measurement_sources(legacy_elevation.snapshot(), wall_ids(rectangle_walls())).size() == 4,
            "legacy walls without elevations must default to the zero plane");
    const auto valid = Document::create(base_entities(rectangle_walls()));
    auto duplicate_ids = wall_ids(rectangle_walls());
    duplicate_ids.push_back(duplicate_ids.front());
    rejects([&] { (void)exterior_wall_measurement_sources(valid.snapshot(), duplicate_ids); },
            "recognition must reject repeated source IDs");
    auto missing_ids = wall_ids(rectangle_walls());
    missing_ids.front() = "missing-wall";
    rejects([&] { (void)exterior_wall_measurement_sources(valid.snapshot(), missing_ids); },
            "recognition must reject missing source IDs");
    auto excessive_ids = wall_ids(rectangle_walls());
    excessive_ids.resize(2049, "wall-bottom");
    rejects([&] { (void)exterior_wall_measurement_sources(valid.snapshot(), excessive_ids); },
            "recognition must bound input wall counts before constructing an arrangement");

    // A modest wall count can still create a quadratic planar arrangement.
    auto dense = rectangle_walls();
    for (int i = 1; i <= 180; ++i) {
        const auto x = 4.0 * i / 181.0;
        const auto y = 3.0 * i / 181.0;
        dense.push_back({"vertical-" + std::to_string(i), {{x, 0}, {x, 3}, 0}, 0.1});
        dense.push_back({"horizontal-" + std::to_string(i), {{0, y}, {4, y}, 0}, 0.1});
    }
    const auto dense_document = Document::create(base_entities(dense));
    rejects([&] { (void)exterior_wall_measurement_sources(dense_document.snapshot(), wall_ids(dense)); },
            "dense wall networks must reject excessive intersections without exhausting resources");
}

void wall_network_recognition_uses_resolved_elevation_planes_without_mutation() {
    const auto specs = rectangle_walls();
    const auto level_entities = [&] {
        auto entities = base_entities(specs);
        const auto graph = VerticalLevelGraph({{"ground", 0}, {"upper", 3}},
                                              {{"storey", "ground", "upper"}});
        entities.push_back(entity("levels", "vertical_levels", {{"model", Json::parse(graph.serialize())}}));
        for (auto& item : entities) {
            if (item.id == "floor-1")
                item.properties["vertical_level_binding"] = {
                    {"version", 1}, {"graph_id", "levels"}, {"level_id", "upper"}};
            if (item.type == "wall")
                item.properties["vertical_placement"] = {
                    {"version", 1}, {"mode", "level"}, {"offset_m", 0.0}};
        }
        return entities;
    };
    auto inconsistent_entities = level_entities();
    for (auto& item : inconsistent_entities)
        if (item.id == "wall-bottom") item.properties["vertical_placement"]["offset_m"] = 0.5;
    const auto inconsistent = Document::create(inconsistent_entities);
    rejects([&] { (void)exterior_wall_measurement_sources(inconsistent.snapshot(), wall_ids(specs)); },
            "equal raw elevations with unequal resolved level offsets must reject");

    auto consistent_entities = level_entities();
    for (auto& item : consistent_entities) {
        if (item.id == "wall-bottom") {
            item.properties["elevation_m"] = 1.0;
            item.properties["vertical_placement"]["offset_m"] = -1.0;
        }
    }
    const auto consistent = Document::create(consistent_entities);
    const auto ids = exterior_wall_measurement_sources(consistent.snapshot(), wall_ids(specs));
    require(ids == std::vector<std::string>{"wall-bottom", "wall-left", "wall-right", "wall-top"},
            "different raw elevations resolving to one actual plane must remain recognizable");
    near(signed_area(derive_exterior_wall_measurement(consistent.snapshot(), ids).boundary),
         13.44, 1e-10, "level resolution must preserve the exact planar exterior measurement");
    for (const auto& original : consistent_entities)
        require(consistent.snapshot().entities().at(original.id).properties == original.properties,
                "resolved plane checks must never rewrite authoritative wall or level properties");

    auto roundoff_entities = level_entities();
    for (auto& item : roundoff_entities) {
        if (item.type != "wall") continue;
        item.properties["elevation_m"] = item.id == "wall-bottom" ? 0.1 : 0.2;
        item.properties["vertical_placement"]["offset_m"] = item.id == "wall-bottom" ? -2.8 : -2.9;
    }
    const auto roundoff_document = Document::create(roundoff_entities);
    require(exterior_wall_measurement_sources(roundoff_document.snapshot(), wall_ids(specs)).size() == 4,
            "one resolved elevation plane must tolerate arithmetic roundoff in local elevation plus level offset");
    auto chained_entities = base_entities(specs);
    for (auto& item : chained_entities) {
        if (item.id == "wall-right") item.properties["elevation_m"] = 0.75 * default_geometry_tolerance_metres;
        if (item.id == "wall-top") item.properties["elevation_m"] = 1.5 * default_geometry_tolerance_metres;
    }
    const auto chained_document = Document::create(chained_entities);
    rejects([&] { (void)exterior_wall_measurement_sources(chained_document.snapshot(), wall_ids(specs)); },
            "elevation tolerance must bound the entire plane range rather than admit pairwise chained offsets");

    for (const auto legacy_elevation : {Json(0.5), Json("malformed")}) {
        auto legacy_entities = base_entities(specs);
        for (auto& item : legacy_entities) {
            if (item.type != "wall") continue;
            item.properties.erase("elevation_m");
            item.properties["elevation"] = item.id == "wall-bottom" ? legacy_elevation : Json(0.0);
        }
        const auto legacy = Document::create(legacy_entities);
        rejects([&] { (void)exterior_wall_measurement_sources(legacy.snapshot(), wall_ids(specs)); },
                "legacy elevation aliases must reject mismatched or malformed planes");
    }
}

void appraisal_withholds_overlapping_old_and_annex_exterior_owners() {
    const auto original_walls = rectangle_walls();
    auto original_entities = base_entities(original_walls, true);
    const auto original_document = Document::create(original_entities);
    const auto original = derive_exterior_wall_measurement(original_document.snapshot(), wall_ids(original_walls));
    auto original_owner = measurement_entity(original.boundary, original.source, true);
    original_owner.properties["appraisal_facts"] = appraisal_facts();

    auto annex_walls = original_walls;
    annex_walls.insert(annex_walls.end(), {
        {"annex-bottom", {{4, 0}, {6, 0}, 0}, 0.2},
        {"annex-right", {{6, 0}, {6, 3}, 0}, 0.2},
        {"annex-top", {{6, 3}, {4, 3}, 0}, 0.2}});
    auto annex_entities = base_entities(annex_walls, true);
    const auto annex_document = Document::create(annex_entities);
    const auto new_ids = exterior_wall_measurement_sources(annex_document.snapshot(), wall_ids(annex_walls));
    require(new_ids == std::vector<std::string>{"annex-bottom", "annex-right", "annex-top",
                "wall-bottom", "wall-left", "wall-top"},
            "the annex exterior must include six complete perimeter walls and exclude the old shared wall");
    const auto expanded = derive_exterior_wall_measurement(annex_document.snapshot(), new_ids);
    near(signed_area(expanded.boundary), 19.84, 1e-10,
         "the annex fixture must have its independent exact expanded exterior area");
    auto expanded_owner = measurement_entity(expanded.boundary, expanded.source, true);
    expanded_owner.id = "area-expanded";
    expanded_owner.properties["appraisal_facts"] = appraisal_facts();
    annex_entities.push_back(original_owner);
    annex_entities.push_back(expanded_owner);
    const auto retained = Document::create(annex_entities);
    require(wall_measurement_source_current(retained.snapshot(), retained.snapshot().entities().at("area-1")),
            "unchanged original perimeter sources remain strictly current after an annex is added");
    require(wall_measurement_source_current(retained.snapshot(), retained.snapshot().entities().at("area-expanded")),
            "the expanded owner must also have current source provenance in the overlap fixture");
    const auto report = build_appraisal_document_report(retained.snapshot(), "property-1");
    require(!report.qualified && !report.calculation.has_value() &&
                std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
                    return issue.find("overlap on the same floor") != std::string::npos;
                }),
            "same-floor overlap must withhold automatic totals even when both exterior owners remain current");
    require(report.boundaries.size() == 2 &&
                std::all_of(report.boundaries.begin(), report.boundaries.end(), [](const auto& boundary) {
                    return boundary.qualification.qualified;
                }),
            "the overlap guard must operate on owners with valid residential facts rather than missing qualification");
}

BoundaryGeometryEdit source_replacement_edit(const Entity& owner, const Boundary& outline,
                                             const std::vector<std::string>& ids) {
    auto replacement = decode_identified_boundary_entity(owner);
    require(replacement.segments.size() == outline.size(), "replacement fixture retains ordered child identities");
    for (std::size_t i = 0; i < outline.size(); ++i) replacement.segments[i].segment = outline[i];
    BoundaryGeometryEdit edit;
    edit.boundary_id = owner.id; edit.target_id = owner.id;
    edit.kind = BoundaryGeometryEditKind::redefine_boundary;
    edit.replacement_segments = encode_identified_boundary_entity(replacement).properties.at("segments");
    auto wire = encode_boundary_geometry_edit(edit);
    wire["version"] = 3;
    wire["replacement_wall_source_ids"] = ids;
    wire["replacement_child_mapping"] = Json::object();
    wire["replacement_removed_reference_ids"] = Json::array();
    return decode_boundary_geometry_edit(wire);
}

void reviewed_source_replacement_preserves_owner_and_proofs() {
    for (const bool curved : {false, true}) for (const bool authored : {false, true}) {
        const auto specs = curved ? capsule_walls() : rectangle_walls();
        auto initial_entities = base_entities(specs, true);
        auto initial_walls = Document::create(initial_entities);
        const auto measured = derive_exterior_wall_measurement(initial_walls.snapshot(), wall_ids(specs));
        auto owner = upgrade_legacy_boundary_entity(measurement_entity(measured.boundary, measured.source, true));
        if (authored) {
            BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
            (void)session.anchor(measured.boundary.front().start);
            for (const auto& edge : measured.boundary) {
                if (edge.sweep_radians == 0.0) (void)session.add_line_to(edge.end);
                else (void)session.add_arc_chord_angle(edge.end, angle_from_radians(edge.sweep_radians));
            }
            session.classify_current_chain("living");
            auto chain = session.close_chain();
            chain.boundary.id = owner.id;
            owner = encode_identified_boundary_entity(chain.boundary, &owner);
            owner.properties["boundary_authoring"] = boundary_construction_envelope(chain, session.options());
        }
        owner.properties["name"] = "Retained area";
        owner.properties["factor_numerator"] = 2;
        owner.properties["factor_denominator"] = 2;
        owner.properties["factor_expression"] = "2/2";
        owner.properties["vendor_metadata"] = Json{{"preserve", "opaque"}};
        owner.properties["deduction_ids"] = Json::array({"deduction-1"});
        owner.extensions["vendor_style"] = Json{{"color", "#123456"}};
        auto deduction = measurement_entity({{{1,1},{1.5,1},0},{{1.5,1},{1.5,1.5},0},
            {{1.5,1.5},{1,1.5},0},{{1,1.5},{1,1},0}}, Json::object());
        deduction.id = "deduction-1";
        deduction.properties.erase("wall_measurement_source");
        deduction.properties["appraisal_facts"] = Json{{"boundary_role", "other_void"}};
        initial_entities.push_back(owner); initial_entities.push_back(deduction);
        auto document = Document::create(initial_entities);
        const auto qualified_before = build_appraisal_document_report(document.snapshot(), "property-1");
        require(qualified_before.qualified && qualified_before.calculation, "source replacement fixture is appraisal-qualified");
        auto replacement_wall = document.snapshot().entities().at(specs.front().id);
        replacement_wall.id = "zz-replacement-wall";
        document.apply(ApplyEntityChanges{document.revision(),
            {EntityChange::erase(specs.front().id), EntityChange::upsert(replacement_wall)}, {}, "replace source wall"});
        const auto stale = document.snapshot();
        require(!wall_measurement_source_current(stale, stale.entities().at(owner.id)), "deleted exterior source makes the owner stale");
        auto ids = wall_ids(specs); ids.front() = replacement_wall.id;
        auto derived = derive_exterior_wall_measurement(stale, ids);
        std::rotate(derived.boundary.begin(), derived.boundary.begin() + 1, derived.boundary.end());
        const auto edit = source_replacement_edit(stale.entities().at(owner.id), derived.boundary, ids);
        const auto wire = encode_boundary_geometry_edit(edit);
        require(wire.at("version") == 3 && wire.at("replacement_wall_source_ids") == ids &&
            decode_boundary_geometry_edit(wire) == edit, "explicit source replacement must round-trip strict version three intent");
        auto bad_wire = wire; bad_wire["unknown"] = true;
        rejects([&] { (void)decode_boundary_geometry_edit(bad_wire); }, "unknown source replacement fields must reject");
        bad_wire = wire; bad_wire["replacement_wall_source_ids"] = Json::array();
        rejects([&] { (void)decode_boundary_geometry_edit(bad_wire); }, "version three requires explicit replacement source IDs");
        const EditBoundaryGeometry command{stale.revision(), edit};
        const auto preview = Document::preview_command(stale, command);
        require(document.snapshot().entities() == stale.entities() &&
            encode_boundary_geometry_edit(edit) == wire, "source replacement preview must mutate neither source nor caller intent");
        document.apply(command);
        const auto repaired = document.snapshot();
        require(repaired.revision() == stale.revision() + 1 && repaired.entities() == preview.entities() &&
            wall_measurement_source_current(repaired, repaired.entities().at(owner.id)), "reviewed redefinition must atomically repair the same measured owner");
        const auto& actual = repaired.entities().at(owner.id);
        for (const auto* key : {"name", "factor_numerator", "factor_denominator", "factor_expression", "appraisal_facts", "deduction_ids", "vendor_metadata"})
            require(actual.properties.at(key) == owner.properties.at(key), "source replacement must retain measurement/appraisal metadata and deductions");
        require(actual.extensions.at("vendor_style") == owner.extensions.at("vendor_style"), "source replacement must retain opaque style");
        for (const auto& [id, entity_value] : stale.entities()) if (id != owner.id)
            require(repaired.entities().at(id) == entity_value, "source replacement must preserve source walls, deductions and unrelated entities");
        const auto qualified_after = build_appraisal_document_report(repaired, "property-1");
        require(qualified_after.qualified && qualified_after.calculation &&
            qualified_after.boundaries.size() == qualified_before.boundaries.size(), "repaired source must restore qualified appraisal totals");
        require(qualified_after.boundaries.front().qualification.adjusted_square_metres.has_value() &&
            qualified_before.boundaries.front().qualification.adjusted_square_metres.has_value(),
            "qualified source replacement fixture exposes adjusted quantities");
        near(*qualified_after.boundaries.front().qualification.adjusted_square_metres,
            *qualified_before.boundaries.front().qualification.adjusted_square_metres, 1e-10,
            "equivalent source replacement retains qualified adjusted quantities");
        require(ProjectStore::required_format_version(repaired) == 16, "source replacement requires native reader sixteen");
        auto fork = Document::fork(repaired);
        require(fork.snapshot().entities() == repaired.entities(), "source replacement retained history must fork exactly");
        auto imported_entities = std::vector<Entity>{};
        for (const auto& [id, value] : repaired.entities()) { (void)id; imported_entities.push_back(value); }
        const auto imported = Document::create(imported_entities);
        require(ProjectStore::required_format_version(imported.snapshot()) == 16,
            "imported source replacement derivation requires reader sixteen without original command history");
        document.undo(document.revision());
        require(document.snapshot().entities() == stale.entities() && ProjectStore::required_format_version(document.snapshot()) == 16,
            "undo preserves stale original owner and retains source-replacement reader floor");
        document.redo(document.revision());
        require(document.snapshot().entities() == repaired.entities(), "source replacement redo restores exact reviewed state");
        const auto root = std::filesystem::temp_directory_path() / ("wall-source-rebind-" + make_stable_id());
        std::filesystem::create_directory(root);
        const auto path = root / "repaired.bldproj";
        (void)ProjectStore::save(path, document.snapshot());
        {
            auto loaded = ProjectStore::load(path);
            require(loaded.document.snapshot().entities() == repaired.entities(), "native save/reopen retains repaired source and metadata");
            extract_project(imported.snapshot(), root / "exchange");
            std::ifstream input(root / "exchange" / "project.json");
            require(Json::parse(input).at("exchange_version") == 14, "imported source replacement requires exchange fourteen");
        }
        std::filesystem::remove_all(root);
        const auto reject_edit = [&](const BoundaryGeometryEdit& bad, const char* reason) {
            bool refused = false;
            try { (void)Document::preview_command(stale, EditBoundaryGeometry{stale.revision(), bad}); }
            catch (const std::exception&) { refused = true; }
            require(refused && document.snapshot().entities() == repaired.entities(), reason);
        };
        auto bad = edit; bad.replacement_segments[0]["start"][0] = 999.0;
        reject_edit(bad, "requested source replacement geometry must exactly equal the derived analytical shell");
        for (const auto& invalid_ids : std::vector<std::vector<std::string>>{
            {"missing", ids[1], ids[2]}, {ids[1], ids[1], ids[2]}, {"deduction-1", ids[1], ids[2]},
            {ids[0], ids[1], ids[2]}}) {
            auto malformed = wire; malformed["replacement_wall_source_ids"] = invalid_ids;
            bool refused = false;
            try { reject_edit(decode_boundary_geometry_edit(malformed), "bad replacement sources must fail atomically"); refused = true; }
            catch (const std::invalid_argument&) { refused = true; }
            require(refused, "invalid source set must reject");
        }
        auto raw = stale.entities().at(owner.id); raw.properties["wall_measurement_source"] = derived.source;
        bool raw_refused = false;
        try { (void)Document::preview_command(stale, ApplyEntityChanges{stale.revision(),
            {EntityChange::upsert(raw)}, {}, "forge generic source metadata"}); }
        catch (const std::exception&) { raw_refused = true; }
        require(raw_refused, "generic metadata must not bypass reviewed typed source replacement");
        auto forged_entities = imported_entities;
        for (auto& value : forged_entities) if (value.id == owner.id)
            value.properties["wall_measurement_source"]["walls"][0]["id"] = "forged-source";
        bool forged_refused = false;
        try { (void)Document::create(forged_entities); } catch (const std::exception&) { forged_refused = true; }
        require(forged_refused, "imported final source IDs must reconcile with the retained reviewed proof");

        const auto refuse_map = [&](const std::map<std::string, Entity, std::less<>>& values,
                                     const char* reason) {
            rejects([&] { (void)edited_boundary_entities(values, edit); }, reason);
        };
        auto mixed = stale.entities(); mixed.at(ids.front()).properties["phase_id"] = "other-phase";
        refuse_map(mixed, "replacement sources cannot migrate the original design phase");
        mixed = stale.entities(); mixed.at(ids.front()).properties["building_id"] = "missing-building";
        refuse_map(mixed, "replacement source context must resolve consistently through actual hierarchy");
        mixed = stale.entities();
        mixed.at(owner.id).properties["wall_measurement_source"]["walls"][0]["context"]["property_id"] = "spoofed-property";
        refuse_map(mixed, "the first historical source record cannot spoof the target hierarchy");
        mixed = stale.entities(); mixed.at(ids.front()).properties["elevation_m"] = 1.0;
        refuse_map(mixed, "replacement sources must share the surviving original effective plane");
        mixed = stale.entities(); mixed.at(ids[1]).properties["elevation_m"] = 1.0;
        refuse_map(mixed, "a surviving original source cannot disagree with the replacement plane");
        mixed = stale.entities();
        mixed.emplace("phase-model", entity("phase-model", "model_phases",
            {{"model", ModelPhases::create(ids, ids,
                {{"demolition", "Demolition", {ids.front()}, {}}}, "demolition").to_json()}}));
        refuse_map(mixed, "demolished replacement source walls cannot participate in the active semantic model");
        mixed = stale.entities();
        for (const auto& id : ids) {
            mixed.at(id).properties["baseline"]["start"][0] = mixed.at(id).properties["baseline"]["start"][0].get<double>() + 10.0;
            mixed.at(id).properties["baseline"]["end"][0] = mixed.at(id).properties["baseline"]["end"][0].get<double>() + 10.0;
        }
        const auto shifted = derive_exterior_wall_measurement(mixed, ids);
        const auto shifted_edit = source_replacement_edit(mixed.at(owner.id), shifted.boundary, ids);
        rejects([&] { (void)edited_boundary_entities(mixed, shifted_edit); },
            "reviewed source replacement must refuse a retained deduction outside its newly derived parent");

        // The stored geometry proof survives later removal of its current
        // physical sources; appraisal then reports staleness rather than
        // attempting to reconstruct history from today's incomplete shell.
        document.apply(ApplyEntityChanges{document.revision(), {EntityChange::erase(ids.front())}, {},
            "delete repaired source later"});
        const auto later_stale = document.snapshot();
        require(!wall_measurement_source_current(later_stale, later_stale.entities().at(owner.id)) &&
            !build_appraisal_document_report(later_stale, "property-1").qualified,
            "later source deletion must withhold totals while retaining reviewed geometry");
        auto later_fork = Document::fork(later_stale);
        require(later_fork.snapshot().entities() == later_stale.entities(),
            "later stale source proof must fork without consulting missing walls");
        const auto stale_root = std::filesystem::temp_directory_path() / ("wall-source-stale-" + make_stable_id());
        std::filesystem::create_directory(stale_root);
        (void)ProjectStore::save(stale_root / "later-stale.bldproj", later_stale);
        {
            auto stale_loaded = ProjectStore::load(stale_root / "later-stale.bldproj");
            require(stale_loaded.document.snapshot().entities() == later_stale.entities(),
                "later stale source must save and reopen with its exact historical proof");
            stale_loaded.document.undo(stale_loaded.document.revision());
            require(stale_loaded.document.snapshot().entities() == repaired.entities(),
                "undo of later deletion restores the repaired source without redefinition");
            stale_loaded.document.redo(stale_loaded.document.revision());
            require(stale_loaded.document.snapshot().entities() == later_stale.entities(),
                "redo of later deletion preserves the stale historical geometry proof");
        }
        std::filesystem::remove_all(stale_root);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--wall-source-rebind-only") {
            reviewed_source_replacement_preserves_owner_and_proofs();
            std::cout << "Wall source rebind workflows passed\n";
            return 0;
        }
        rectangle_offsets_outward_and_records_sources();
        shuffled_and_reversed_walls_keep_the_same_outline();
        stable_wall_identity_seeds_output_order_across_coordinate_edits();
        concave_l_outline_and_per_wall_thickness_are_respected();
        curved_exterior_is_analytical_reversible_and_current();
        concave_curved_wall_offsets_concentrically_and_analytically();
        major_arc_endpoint_tangent_order_keeps_partition_out_of_exterior();
        four_concentric_quarter_arcs_offset_as_one_exact_circle();
        adjacent_convex_arcs_use_each_wall_thickness_and_analytic_miters();
        equal_offset_collinear_wall_continuations_remain_valid();
        source_guard_detects_changed_or_missing_walls_but_ignores_openings();
        source_guard_accepts_legacy_boundaries_and_rejects_malformed_or_edited_sources();
        appraisal_withholds_stale_wall_measured_totals();
        open_duplicate_and_crossed_wall_loops_are_rejected();
        invalid_thickness_and_out_of_envelope_coordinates_are_rejected();
        wall_network_recognition_excludes_partitions_and_keeps_authoritative_sources();
        wall_network_recognition_handles_split_hosts_concavity_and_varied_thickness();
        wall_network_recognition_rejects_ambiguous_and_unsupported_inputs();
        wall_network_recognition_uses_resolved_elevation_planes_without_mutation();
        appraisal_withholds_overlapping_old_and_annex_exterior_owners();
        reviewed_source_replacement_preserves_owner_and_proofs();
        std::cout << "wall_measurement_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wall_measurement_tests: " << error.what() << '\n';
        return 1;
    }
}
