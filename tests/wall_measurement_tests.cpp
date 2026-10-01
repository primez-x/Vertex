#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
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

void open_duplicate_crossed_and_curved_wall_loops_are_rejected() {
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

    auto curved = rectangle_walls();
    curved[0].baseline.sweep_radians = std::numbers::pi / 2.0;
    auto curved_document = Document::create(base_entities(curved));
    rejects([&] { (void)derive_exterior_wall_measurement(
                curved_document.snapshot(), wall_ids(curved)); },
            "curved source walls must be explicitly rejected by the straight-only outline");
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
}

} // namespace

int main() {
    try {
        rectangle_offsets_outward_and_records_sources();
        shuffled_and_reversed_walls_keep_the_same_outline();
        stable_wall_identity_seeds_output_order_across_coordinate_edits();
        concave_l_outline_and_per_wall_thickness_are_respected();
        equal_offset_collinear_wall_continuations_remain_valid();
        source_guard_detects_changed_or_missing_walls_but_ignores_openings();
        source_guard_accepts_legacy_boundaries_and_rejects_malformed_or_edited_sources();
        appraisal_withholds_stale_wall_measured_totals();
        open_duplicate_crossed_and_curved_wall_loops_are_rejected();
        invalid_thickness_and_out_of_envelope_coordinates_are_rejected();
        std::cout << "wall_measurement_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wall_measurement_tests: " << error.what() << '\n';
        return 1;
    }
}
