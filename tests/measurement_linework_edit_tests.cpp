#include "sketch/measurement_linework.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using namespace sketch;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double value, double expected, const char* message) {
    require(std::abs(value - expected) < 1e-9, message);
}
bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
void expect_invalid(const std::function<void()>& fn, const char* message) {
    bool threw = false;
    try { fn(); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, message);
}
MeasurementLinework stroke(std::vector<Vec2> points, std::vector<std::string> ids, bool closed = false) {
    MeasurementLinework result;
    result.stroke_id = "stroke"; result.anchor = points.front(); result.closed = closed;
    result.extensions = {{"future", Json::array({"untouched", 7})}};
    for (std::size_t i = 1; i < points.size(); ++i) {
        ConstructionReceipt receipt;
        receipt.segment_id = "e" + std::to_string(i);
        receipt.kind = BoundaryConstructionKind::line_to_point;
        receipt.start = points[i - 1]; receipt.chord_end = points[i];
        result.edges.push_back({receipt.segment_id, ids[i - 1], ids[i], receipt});
    }
    return result;
}
BoundaryGeometryEdit move(std::string vertex, Vec2 position) {
    BoundaryGeometryEdit result; result.boundary_id = "stroke";
    result.target_id = std::move(vertex); result.target_position = position; return result;
}
BoundaryGeometryEdit resize(std::string edge, double length, BoundaryFixedEndpoint fixed, bool connected = false) {
    BoundaryGeometryEdit result; result.boundary_id = "stroke";
    result.kind = BoundaryGeometryEditKind::resize_segment; result.target_id = std::move(edge);
    result.target_length_metres = length; result.fixed_endpoint = fixed; result.move_connected = connected;
    return result;
}
void assert_original(const MeasurementLinework& edited, const MeasurementLinework& original) {
    require(edited.edges == original.edges && same_point(edited.anchor, original.anchor) &&
        edited.extensions == original.extensions && edited.stroke_id == original.stroke_id,
        "derivation must preserve immutable original inputs, identities and extensions");
    const auto encoded = encode_measurement_linework_model(edited);
    const auto decoded = decode_measurement_linework_model(Json::parse(encoded.dump()));
    require(decoded.supported() && encode_measurement_linework_model(*decoded.model) == encoded,
        "saved edit must decode and encode deterministically");
    require(decoded.model->edges == original.edges, "save must preserve receipts rather than rewrite inputs");
}
// Catches a missing edit, receipt rewrite, or failure to move the last open endpoint.
void open_and_closed_vertex_edits() {
    const auto open = stroke({{0,0},{2,0},{2,3}}, {"a","b","c"});
    const auto edited = edited_measurement_linework(open, move("c", {4,5}));
    const auto replay = replay_measurement_linework(edited);
    require(same_point(replay.edges[1].segment.end, {4,5}) && !replay.closed,
        "open final vertex must move without closed boundary admission");
    require(edited.schema_version == 3 && edited.operations.size() == 1 && edited.transforms.empty(),
        "edit must promote to ordered v3 derivation");
    assert_original(edited, open);
    auto closed = stroke({{0,0},{2,0},{2,2},{0,0}}, {"a","b","c","a"}, true);
    auto& closure = closed.edges.back().receipt;
    closure.kind = BoundaryConstructionKind::line_closure;
    closure.chord_end.reset(); closure.closure_delta = Vec2{-2,-2};
    const auto shifted = edited_measurement_linework(closed, move("a", {-1,1}));
    const auto world = replay_measurement_linework(shifted);
    require(world.closed && same_point(world.anchor, {-1,1}) &&
        same_point(world.edges.front().segment.start, world.edges.back().segment.end),
        "closed anchor identity must move in both occurrences");
    assert_original(shifted, closed);
}
void repeated_retraced_and_crossing_vertices() {
    const auto source = stroke({{0,0},{2,0},{2,2},{2,0},{3,0}}, {"a","b","c","b","d"});
    const auto edited = edited_measurement_linework(source, move("b", {4,1}));
    const auto replay = replay_measurement_linework(edited);
    require(same_point(replay.edges[0].segment.end, {4,1}) &&
        same_point(replay.edges[1].segment.start, {4,1}) &&
        same_point(replay.edges[2].segment.end, {4,1}) && same_point(replay.edges[3].segment.start, {4,1}),
        "all revisits and retraced endpoints must share the edited stable vertex");
    const auto crossing = stroke({{0,0},{2,2},{0,2},{2,0},{0,0}}, {"a","b","c","d","a"}, true);
    require(replay_measurement_linework(edited_measurement_linework(crossing, move("b", {3,3}))).edges.size() == 4,
        "self-crossing linework edits must stay admissible");
    assert_original(edited, source);
}
void anchored_typed_lengths_and_connected_policy() {
    auto source = stroke({{0,0},{2,0},{2,3}}, {"a","b","c"});
    auto& receipt = source.edges[0].receipt;
    receipt.kind = BoundaryConstructionKind::line_heading; receipt.chord_end.reset();
    receipt.distance = parse_quantity("2 m"); receipt.heading = parse_angle("0 deg");
    const auto quantity = parse_quantity("6 ft 6 3/4 in");
    const auto edited = edited_measurement_linework(source, resize("e1", quantity.metres, BoundaryFixedEndpoint::start), quantity);
    const auto replay = replay_measurement_linework(edited);
    require(same_point(replay.edges[0].segment.start, {0,0}), "fixed start must remain exact");
    require_near(replay.edges[0].segment.end.x, 2.00025, "typed fractional length must derive new endpoint");
    const auto& event = std::get<MeasurementLineworkEdit>(edited.operations[0]);
    require(event.authored_length && event.authored_length->original_expression == "6 ft 6 3/4 in" &&
        event.authored_length->exact_metres == ExactRational{8001,4000}, "edit must retain exact typed quantity");
    const auto fixed_end = replay_measurement_linework(edited_measurement_linework(source,
        resize("e1", 4, BoundaryFixedEndpoint::end)));
    require(same_point(fixed_end.anchor, {-2,0}) && same_point(fixed_end.edges[0].segment.end, {2,0}),
        "fixed end resize must move the starting vertex and anchor");
    const auto connected = replay_measurement_linework(edited_measurement_linework(source,
        resize("e1", 4, BoundaryFixedEndpoint::start, true)));
    require(same_point(connected.edges[0].segment.end, {4,0}) && same_point(connected.edges[1].segment.end, {4,3}),
        "connected policy must translate all other vertices equally");
    assert_original(edited, source);
}
void arcs_and_mixed_transform_edit_order() {
    auto source = stroke({{0,0},{2,0}}, {"a","b"});
    auto& receipt = source.edges[0].receipt;
    receipt.kind = BoundaryConstructionKind::arc_chord_angle; receipt.angle = parse_angle("-90 deg");
    const auto resized = edited_measurement_linework(source,
        resize("e1", 2 * std::numbers::pi, BoundaryFixedEndpoint::start));
    const auto curve = replay_measurement_linework(resized).edges[0].segment;
    require_near(segment_length(curve), 2 * std::numbers::pi, "arc resize must use analytical arc length");
    require(curve.sweep_radians == -std::numbers::pi / 2, "arc edit must preserve signed sweep");
    const auto moved_curve = replay_measurement_linework(edited_measurement_linework(source, move("a", {0,1}))).edges[0].segment;
    require(same_point(moved_curve.start, {0,1}) && moved_curve.sweep_radians == -std::numbers::pi/2,
        "vertex move must retain the analytical arc and its signed sweep");
    auto world = transformed_measurement_linework(source, {{},0,false,false,{5,0}});
    world = edited_measurement_linework(world, move("b", {9,0}));
    world = transformed_measurement_linework(world, {{},std::numbers::pi/2,false,false,{0,1}});
    const auto replay = replay_measurement_linework(world);
    require(world.operations.size() == 3 && world.transforms.empty(), "v2 promotion and later transform must share one ordered stream");
    require_near(replay.edges[0].segment.start.x, 0, "mixed replay start x");
    require_near(replay.edges[0].segment.start.y, 6, "mixed replay start y");
    require_near(replay.edges[0].segment.end.y, 10, "edit point must be interpreted in its current world frame");
    assert_original(world, source);
}
void no_op_invalid_and_precision_are_atomic() {
    const auto source = stroke({{0.1,0},{2.1,0},{2.1,3}}, {"a","b","c"});
    const auto before = encode_measurement_linework_model(source);
    require(encode_measurement_linework_model(edited_measurement_linework(source, move("b", {2.1,0}))) == before,
        "no-op must retain historical v1 bytes");
    require(encode_measurement_linework_model(edited_measurement_linework(source,
        resize("e1",2,BoundaryFixedEndpoint::start))) == before, "no-op length must retain dialect");
    std::vector<BoundaryGeometryEdit> invalids{move("missing",{1,1}), move("b",{0.1,0}),
        move("b",{std::numeric_limits<double>::infinity(),0}),
        resize("missing",3,BoundaryFixedEndpoint::start), resize("e1",0,BoundaryFixedEndpoint::start),
        resize("e1",1e16,BoundaryFixedEndpoint::start,true),
        resize("e1",3,static_cast<BoundaryFixedEndpoint>(99))};
    auto irrelevant = move("b",{3,0}); irrelevant.new_vertex_id = "unused"; invalids.push_back(irrelevant);
    auto wrong_owner = move("b",{3,0}); wrong_owner.boundary_id = "other"; invalids.push_back(wrong_owner);
    for (const auto& edit : invalids) expect_invalid([&]{ (void)edited_measurement_linework(source,edit); }, "invalid edit must reject");
    expect_invalid([&]{ (void)edited_measurement_linework(source,resize("e1",3,BoundaryFixedEndpoint::start),parse_quantity("4 m")); },
        "authored length must exactly agree with semantic target");
    expect_invalid([&]{ (void)edited_measurement_linework(source,move("b",{3,0}),parse_quantity("3 m")); },
        "vertex edits must reject irrelevant authored quantity");
    require(encode_measurement_linework_model(source) == before, "failure must leave source immutable");
}
// Catches opposite rounding tails that individually fit tolerance but change
// a connected chain edge by a full ULP, greater than the geometry tolerance.
void connected_precision_rejects_shape_drift() {
    constexpr double ulp = 0x1p-23;
    const auto source = stroke({{0,0},{2,0},{1e9+2*ulp,0},{1e9+3*ulp,0}}, {"a","b","c","d"});
    const auto before = encode_measurement_linework_model(source);
    expect_invalid([&]{ (void)edited_measurement_linework(source,
        resize("e1",2+ulp/2,BoundaryFixedEndpoint::start,true)); },
        "connected translation must reject rounding that changes an unanchored edge beyond tolerance");
    require(encode_measurement_linework_model(source) == before, "precision rejection must retain source receipts");
}
void strict_v3_and_historical_codecs() {
    const auto source = stroke({{0,0},{2,0}}, {"a","b"});
    const auto v1 = encode_measurement_linework_model(source);
    const auto v2model = transformed_measurement_linework(source, {{},0,false,false,{1,2}});
    const auto v2 = encode_measurement_linework_model(v2model);
    const auto edited = edited_measurement_linework(v2model,resize("e1",3,BoundaryFixedEndpoint::start),parse_quantity("3 m"));
    const auto valid = encode_measurement_linework_model(edited);
    require(valid["version"] == 3 && valid["replay_version"] == 3 && valid.contains("operations") && !valid.contains("transforms"),
        "v3 must persist only one ordered derivation authority");
    require(inspect_measurement_linework_model(valid).format == MeasurementLineworkFormat::supported_v3,
        "v3 must be recognized by inspection");
    std::vector<Json> malformed;
    auto bad = valid; bad["transforms"] = Json::array(); malformed.push_back(bad);
    bad = valid; bad.erase("operations"); malformed.push_back(bad);
    bad = valid; bad["operations"] = Json::object(); malformed.push_back(bad);
    bad = valid; bad["operations"][1]["type"] = "unknown"; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["unused"] = true; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["segment_id"] = "missing"; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["position"] = Json::array({3,0}); malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["target_length_metres"] = 2; bad["operations"][1]["authored_length"] = nullptr; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["boundary_id"] = "other"; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["fixed_endpoint"] = "unknown"; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["move_connected"] = 1; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["version"] = 1.0; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["edit"]["kind"] = "unknown"; malformed.push_back(bad);
    bad = valid; bad["operations"][1].erase("authored_length"); malformed.push_back(bad);
    bad = valid; bad["operations"][1]["authored_length"]["original_expression"] = "4 m"; malformed.push_back(bad);
    bad = valid; bad["operations"][1]["authored_length"]["extra"] = true; malformed.push_back(bad);
    bad = valid; bad["operations"][0]["transform"]["scale"] = 2; malformed.push_back(bad);
    for (const auto& wire : malformed) expect_invalid([&]{ (void)decode_measurement_linework_model(wire); }, "malformed known v3 must fail closed");
    for (const auto& wire : {v1,v2}) require(encode_measurement_linework_model(*decode_measurement_linework_model(wire).model) == wire,
        "historical v1/v2 codecs must retain exact dialect");
    const Json future{{"version",3},{"replay_version",99},{"opaque",true}};
    const auto decoded = decode_measurement_linework_model(future);
    require(!decoded.supported() && decoded.original_model == std::optional<Json>{future}, "unknown replay must remain exact opaque JSON");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    const std::vector<std::pair<const char*,std::function<void()>>> tests{
        {"open and closed vertex edits",open_and_closed_vertex_edits},
        {"repeated retraced crossing topology",repeated_retraced_and_crossing_vertices},
        {"anchored exact length and connected",anchored_typed_lengths_and_connected_policy},
        {"arcs and ordered transforms",arcs_and_mixed_transform_edit_order},
        {"atomic invalid and no-op",no_op_invalid_and_precision_are_atomic},
        {"connected precision preserves shape",connected_precision_rejects_shape_drift},
        {"strict v3 and historical codecs",strict_v3_and_historical_codecs}};
    std::size_t failures = 0;
    for (const auto& [name,fn] : tests) try { fn(); } catch (const std::exception& e) { ++failures; std::cerr << name << ": " << e.what() << '\n'; }
    if (failures) return 1;
    std::cout << "Measurement linework edit tests passed\n"; return 0;
}
