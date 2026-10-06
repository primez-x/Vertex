#include "sketch/boundary_dimension.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_room_data.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/vertical_levels.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-9,
            "dimension differs from independent analytical expectation");
}
template<class F> void rejected(F&& action) {
    try { action(); }
    catch (const std::invalid_argument& error) {
        require(std::string(error.what()).size() > 10, "withheld quantity has a useful diagnostic");
        return;
    }
    throw std::runtime_error("invalid or stale physical room dimension was resolved");
}
Entity entity(std::string id, std::string type, Json properties = Json::object()) {
    return {std::move(id), std::move(type), std::move(properties), false, Json::object()};
}
Json segment_json(const Segment& edge) {
    return {{"start", {edge.start.x, edge.start.y}}, {"end", {edge.end.x, edge.end.y}},
            {"sweep_radians", edge.sweep_radians}};
}
Entity wall(std::string id, Segment edge) {
    return entity(std::move(id), "wall", {{"baseline", segment_json(edge)},
        {"thickness_m", .2}, {"height_m", 3.0}, {"elevation_m", 0.0}, {"layer_id", "layer"}});
}
Entities fixture(bool obstacle = false, bool curved = false, bool level_based = false) {
    const std::vector<Entity> input{
        entity("property", "property"), entity("building", "building", {{"property_id", "property"}}),
        entity("floor", "floor", {{"building_id", "building"}}),
        entity("layer", "layer", {{"floor_id", "floor"}}),
        wall("bottom", {{0, 0}, {4, 0}, 0}), wall("right", {{4, 0}, {4, 3}, 0}),
        wall("top", {{4, 3}, {0, 3}, curved ? .6 : 0}), wall("left", {{0, 3}, {0, 0}, 0})};
    Entities result;
    for (const auto& value : input) result.emplace(value.id, value);
    if (obstacle) result.emplace("island", wall("island", {{1, 1}, {3, 1}, 0}));
    if (level_based) {
        const auto graph = VerticalLevelGraph({{"ground", 2.0}, {"upper", 5.0}},
                                             {{"storey", "ground", "upper"}});
        result.emplace("levels", entity("levels", "vertical_levels", {{"model", Json::parse(graph.serialize())}}));
        result.at("floor").properties["vertical_level_binding"] =
            {{"version", 1}, {"graph_id", "levels"}, {"level_id", "ground"}};
        for (auto& [id, value] : result) {
            (void)id;
            if (value.type == "wall") value.properties["vertical_placement"] =
                {{"version", 1}, {"mode", "level"}, {"offset_m", 0.0}};
        }
    }
    const auto fresh = detect_physical_wall_spaces(result, "bottom");
    require(fresh.spaces.size() == 1, "fixture has one actual clear room");
    const auto& space = fresh.spaces.front();
    Json edges = Json::array();
    for (const auto& edge : space.boundary) edges.push_back(segment_json(edge));
    Entity room{"room", "room_boundary", {{"layer_id", "layer"}, {"name", "Test room"},
        {"classification", "office"}, {"segments", std::move(edges)}}, false,
        {{"physical_wall_room", encode_physical_wall_room_descriptor({"bottom", space.source_lineage, space.holes})}}};
    room = upgrade_legacy_boundary_entity(room);
    // Persist deliberately different stable child IDs. Fresh detector indices
    // and newly generated identities cannot substitute for these targets.
    auto identified = decode_identified_boundary_entity(room);
    for (std::size_t index = 0; index < identified.segments.size(); ++index) {
        auto& edge = identified.segments[index];
        edge.segment_id = "mapped-edge-" + std::to_string(index);
        edge.start_vertex_id = "mapped-vertex-" + std::to_string(index);
        edge.end_vertex_id = "mapped-vertex-" + std::to_string((index + 1) % identified.segments.size());
    }
    room = encode_identified_boundary_entity(identified, &room);
    result.emplace(room.id, std::move(room));
    return result;
}
BoundaryDimension area_dimension() {
    BoundaryDimension result;
    result.id = "room-area"; result.boundary_id = "room"; result.kind = BoundaryDimensionKind::area;
    result.text_position = {2, 1.5};
    return result;
}
BoundaryDimension length_dimension(const IdentifiedSegment& edge) {
    auto result = area_dimension();
    result.id = "room-length"; result.kind = BoundaryDimensionKind::segment_length;
    result.segment_id = edge.segment_id;
    return result;
}
double analytical_length(const Segment& edge) {
    const auto chord = std::hypot(edge.end.x - edge.start.x, edge.end.y - edge.start.y);
    return edge.sweep_radians == 0 ? chord :
        chord * std::abs(edge.sweep_radians) / (2 * std::sin(std::abs(edge.sweep_radians) / 2));
}
void rectangle_and_net_hole() {
    auto input = fixture();
    const auto dimension = area_dimension();
    near(resolve_boundary_dimension(dimension, input).area(), 3.8 * 2.8);
    input.at("room").properties["area_m2"] = 9999;
    near(dimension.resolve(input).area(), 10.64);
    rejected([&] { (void)dimension.resolve(input.at("room")); });
    std::vector<Entity> values;
    for (const auto& [id, value] : input) { (void)id; values.push_back(value); }
    const auto document = Document::create(values);
    near(dimension.resolve(document.snapshot()).area(), 10.64);
    auto holed = fixture(true);
    near(dimension.resolve(holed).area(), 10.24);
    const auto descriptor = decode_physical_wall_room_descriptor(holed.at("room"));
    require(descriptor.holes.size() == 1, "net area fixture has an actual physical-wall hole");
    const auto before = holed;
    (void)dimension.resolve(holed);
    require(before == holed, "resolution never changes room data or physical sources");
}
void stable_length_angle_chain(bool curved) {
    auto input = fixture(false, curved);
    const auto boundary = decode_identified_boundary_entity(input.at("room"));
    std::size_t index = 0;
    if (curved) {
        const auto found = std::find_if(boundary.segments.begin(), boundary.segments.end(),
            [](const auto& edge) { return edge.segment.sweep_radians != 0; });
        require(found != boundary.segments.end(), "curved source remains an analytical clear-room arc");
        index = static_cast<std::size_t>(found - boundary.segments.begin());
    }
    const auto& first = boundary.segments[index];
    const auto& next = boundary.segments[(index + 1) % boundary.segments.size()];
    auto length = length_dimension(first);
    const auto encoded = encode_boundary_dimension_entity(length);
    const auto decoded = decode_boundary_dimension_entity(encoded);
    require(decoded.supported() && decoded.dimension->segment_id == first.segment_id,
            "persisted child dimension retains its exact mapped edge identity");
    input.emplace(encoded.id, encoded);
    near(decoded.dimension->resolve(input).segment_length(), analytical_length(first.segment));
    auto angle = area_dimension();
    angle.id = "room-angle"; angle.kind = BoundaryDimensionKind::angle;
    angle.segment_id = first.segment_id; angle.secondary_segment_id = next.segment_id;
    angle.vertex_id = first.end_vertex_id;
    const auto incoming = std::atan2(first.segment.end.y - first.segment.start.y,
        first.segment.end.x - first.segment.start.x) + first.segment.sweep_radians / 2;
    const auto outgoing = std::atan2(next.segment.end.y - next.segment.start.y,
        next.segment.end.x - next.segment.start.x) - next.segment.sweep_radians / 2;
    near(angle.resolve(input).angle(), std::abs(std::remainder(outgoing - incoming - std::numbers::pi,
                                                            2 * std::numbers::pi)));
    if (!curved) near(angle.resolve(input).angle(), std::numbers::pi / 2);
    length.segment_chain_ids = {first.segment_id, next.segment_id};
    near(length.resolve(input).segment_length(), analytical_length(first.segment) + analytical_length(next.segment));
    auto missing = length; missing.segment_chain_ids.back() = "absent-edge";
    rejected([&] { validate_boundary_dimension_target(missing, input.at("room")); });
    rejected([&] { (void)missing.resolve(input); });
    auto reversed = length;
    std::reverse(reversed.segment_chain_ids.begin(), reversed.segment_chain_ids.end());
    reversed.segment_id = reversed.segment_chain_ids.front();
    rejected([&] { validate_boundary_dimension_target(reversed, input.at("room")); });
    rejected([&] { (void)reversed.resolve(input); });
    angle.vertex_id = "absent-vertex";
    rejected([&] { validate_boundary_dimension_target(angle, input.at("room")); });
    rejected([&] { (void)angle.resolve(input); });
}
void source_failures() {
    const auto original = fixture(true);
    const auto dimension = area_dimension();
    const auto withhold = [&](Entities changed) {
        const auto before = changed;
        // Retained geometry remains structurally admissible even when live
        // physical sources no longer support a current numeric quantity.
        if (changed.at("room").id == dimension.boundary_id)
            validate_boundary_dimension_target(dimension, changed.at("room"));
        rejected([&] { (void)dimension.resolve(changed); });
        require(before == changed, "withholding never structurally invalidates retained stale room storage");
    };
    auto changed = original; changed.at("bottom").properties["thickness_m"] = .4; withhold(changed);
    changed = original; changed.erase("top"); withhold(changed);
    changed = original; changed.at("bottom").properties["elevation_m"] = 3.0; withhold(changed);
    changed = original;
    for (auto& [id, value] : changed) { (void)id; if (value.type == "wall") value.properties["elevation_m"] = 3.0; }
    withhold(changed);
    changed = original; changed.at("room").properties["elevation_m"] = 3.0; withhold(changed);
    changed = original;
    changed.at("room").properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0}};
    withhold(changed);
    changed = original; changed.at("room").properties["floor_id"] = "missing-floor"; withhold(changed);
    changed = original; changed.at("room").id = "different-room"; withhold(changed);
    const auto phases = ModelPhases::create({"bottom", "right", "top", "left", "island"},
        {"bottom", "right", "top", "left", "island"}, {{"remove", "Remove island", {"island"}, {}}}, "remove");
    changed = original; changed.emplace("phases", entity("phases", "model_phases", {{"model", phases.to_json()}})); withhold(changed);
    const auto room_phases = ModelPhases::create({"room"}, {"room"}, {{"remove", "Remove room", {"room"}, {}}}, "remove");
    changed = original; changed.emplace("phases", entity("phases", "model_phases", {{"model", room_phases.to_json()}})); withhold(changed);
    for (const auto& field : {"outer", "physical_sources", "context"}) {
        changed = original; changed.at("room").extensions["physical_wall_room"]["source_lineage"].erase(field); withhold(changed);
    }
    changed = original; changed.at("room").extensions["physical_wall_room"]["source_lineage"]["version"] = 2; withhold(changed);
    changed = original; changed.at("room").extensions["physical_wall_room"]["version"] = 2; withhold(changed);
    changed = original; changed.at("room").extensions["physical_wall_room"]["holes"] = Json::array(); withhold(changed);
    changed = original;
    auto identified = decode_identified_boundary_entity(changed.at("room"));
    for (auto& edge : identified.segments) { edge.segment.start.x += .01; edge.segment.end.x += .01; }
    changed.at("room") = encode_identified_boundary_entity(identified, &changed.at("room")); withhold(changed);
    auto invalid = dimension; invalid.kind = static_cast<BoundaryDimensionKind>(99);
    rejected([&] { validate_boundary_dimension_target(invalid, original.at("room")); });
    rejected([&] { (void)invalid.resolve(original); });
    invalid = dimension; invalid.boundary_id = "missing-room";
    rejected([&] { (void)invalid.resolve(original); });
}
void effective_level_plane() {
    auto input = fixture(false, false, true);
    const auto dimension = area_dimension();
    near(dimension.resolve(input).area(), 10.64);
    input.at("room").properties["elevation_m"] = 2.0;
    input.at("room").properties["vertical_placement"] =
        {{"version", 1}, {"mode", "absolute"}, {"offset_m", 0.0}};
    near(dimension.resolve(input).area(), 10.64);
    require(input.at("bottom").properties.at("elevation_m") == 0.0,
            "effective level placement does not overwrite the retained wall elevation");
    auto changed = input;
    changed.at("room").properties["elevation_m"] = 0.0;
    rejected([&] { (void)dimension.resolve(changed); });
    changed = input;
    const auto graph = VerticalLevelGraph::from_json(changed.at("levels").properties.at("model"));
    changed.at("levels").properties["model"] = Json::parse(graph.with_elevation("ground", 3.0).serialize());
    rejected([&] { (void)dimension.resolve(changed); });
    changed = input;
    changed.erase("levels");
    rejected([&] { (void)dimension.resolve(changed); });
}
} // namespace

int main() {
    try {
        rectangle_and_net_hole();
        stable_length_angle_chain(false);
        stable_length_angle_chain(true);
        source_failures();
        effective_level_plane();
        std::cout << "physical_room_dimension_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "physical_room_dimension_tests: " << error.what() << '\n';
        return 1;
    }
}
