#include "sketch/measurement_linework.hpp"

#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using sketch::BoundaryConstructionKind;
using sketch::ConstructionReceipt;
using sketch::MeasurementLinework;
using sketch::Vec2;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool same_point(Vec2 left, Vec2 right) {
    return left.x == right.x && left.y == right.y;
}

void require_near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-12, message);
}

void expect_invalid(const std::function<void()>& operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, message);
}

ConstructionReceipt point_line(std::string id, Vec2 start, Vec2 end) {
    ConstructionReceipt receipt;
    receipt.segment_id = std::move(id);
    receipt.kind = BoundaryConstructionKind::line_to_point;
    receipt.start = start;
    receipt.chord_end = end;
    return receipt;
}

MeasurementLinework point_stroke(std::string id, Vec2 start, Vec2 end) {
    MeasurementLinework model;
    model.stroke_id = std::move(id);
    model.anchor = start;
    const auto segment_id = model.stroke_id + ":e0";
    model.edges.push_back({segment_id, model.stroke_id + ":v0", model.stroke_id + ":v1",
                           point_line(segment_id, start, end)});
    return model;
}

MeasurementLinework measured_line() {
    auto model = point_stroke("measured", {2, 3}, {4, 3});
    auto& receipt = model.edges.front().receipt;
    receipt.kind = BoundaryConstructionKind::line_heading;
    receipt.chord_end.reset();
    receipt.distance = sketch::parse_quantity("6 ft 6 3/4 in");
    receipt.heading = sketch::parse_angle("0 deg");
    return model;
}

// Fixture JSON is assembled independently of the codec under test, allowing
// decode rejection tests to exercise decode even while encode is a RED stub.
Json fixture_json(const MeasurementLinework& model) {
    Json segments = Json::array();
    for (const auto& edge : model.edges) {
        segments.push_back(Json{{"segment_id", edge.segment_id},
                                {"start_vertex_id", edge.start_vertex_id},
                                {"end_vertex_id", edge.end_vertex_id},
                                {"receipt", sketch::encode_construction_receipt(edge.receipt)}});
    }
    return Json{{"version", model.schema_version}, {"replay_version", model.replay_version},
                {"stroke_id", model.stroke_id}, {"anchor", Json::array({model.anchor.x, model.anchor.y})},
                {"closed", model.closed}, {"segments", std::move(segments)},
                {"extensions", model.extensions}};
}

void test_independent_open_measurement_line_preserves_exact_inputs() {
    const auto model = measured_line();
    const auto replay = sketch::replay_measurement_linework(model);
    require(replay.stroke_id == "measured" && !replay.closed && replay.edges.size() == 1,
            "one measured line must replay without an area or closing edge");
    require(same_point(replay.anchor, {2, 3}) && same_point(replay.edges[0].segment.start, {2, 3}),
            "open stroke must retain its captured anchor");
    // 78.75 inches is exactly 2.00025 metres.
    require_near(replay.edges[0].segment.end.x, 4.00025, "measured endpoint must retain converted fractional length");
    require(replay.edges[0].segment.end.y == 3 &&
                replay.edges[0].segment.sweep_radians == 0,
            "heading receipt must derive the measured line endpoint");
    require(replay.edges[0].segment_id == "measured:e0" &&
                replay.edges[0].start_vertex_id == "measured:v0" &&
                replay.edges[0].end_vertex_id == "measured:v1",
            "replay must retain exact topology identities");
    require(replay.receipts.size() == 1 && replay.receipts[0] == model.edges[0].receipt &&
                replay.receipts[0].distance->exact_metres == sketch::ExactRational{8001, 4000} &&
                replay.receipts[0].distance->original_expression == "6 ft 6 3/4 in",
            "measurement inputs must retain exact fractions and original expressions");
}

void test_relative_turn_uses_previous_line_and_retains_inputs() {
    auto model = point_stroke("relative", {0, 0}, {2, 0});
    ConstructionReceipt receipt;
    receipt.segment_id = "relative:e1";
    receipt.kind = BoundaryConstructionKind::line_relative_turn;
    receipt.start = {2, 0};
    receipt.distance = sketch::parse_quantity("3 m");
    receipt.turn = sketch::parse_angle("90 deg");
    model.edges.push_back({receipt.segment_id, "relative:v1", "relative:v2", receipt});
    const auto replay = sketch::replay_measurement_linework(model);
    require(replay.edges.size() == 2 && same_point(replay.edges[1].segment.start, {2, 0}) &&
                same_point(replay.edges[1].segment.end, {2, 3}),
            "relative input must use the preceding analytical segment");
    require(replay.receipts[1] == receipt, "relative input expressions must survive replay");
    auto missing_previous = model;
    missing_previous.anchor = {2, 0};
    missing_previous.edges.erase(missing_previous.edges.begin());
    expect_invalid([&] { (void)sketch::replay_measurement_linework(missing_previous); },
                   "a first-edge relative turn must reject missing prior context");
}

void test_rise_run_preserves_signed_measurements() {
    auto model = point_stroke("slope", {4, 5}, {1, 7});
    auto& receipt = model.edges[0].receipt;
    receipt.kind = BoundaryConstructionKind::line_rise_run;
    receipt.chord_end.reset();
    receipt.rise = sketch::parse_quantity("2 m");
    receipt.run = sketch::parse_quantity("-3 m");
    const auto replay = sketch::replay_measurement_linework(model);
    require(same_point(replay.edges[0].segment.end, {1, 7}) && replay.receipts[0] == receipt,
            "rise/run measurements must construct signed geometry and retain both expressions");
    const auto decoded = sketch::decode_measurement_linework_model(
        sketch::encode_measurement_linework_model(model));
    require(decoded.supported() && decoded.model->edges[0].receipt == receipt,
            "rise/run model must retain both original exact measurements");
}

void test_relative_turn_uses_previous_arc_end_tangent() {
    auto model = point_stroke("arc-relative", {0, 0}, {2, 0});
    auto& arc = model.edges[0].receipt;
    arc.kind = BoundaryConstructionKind::arc_chord_height;
    arc.height = sketch::parse_quantity("1 m");
    ConstructionReceipt relative;
    relative.segment_id = "arc-relative:e1";
    relative.kind = BoundaryConstructionKind::line_relative_turn;
    relative.start = {2, 0};
    relative.distance = sketch::parse_quantity("1 m");
    relative.turn = sketch::parse_angle("-90 deg");
    model.edges.push_back({relative.segment_id, "arc-relative:v1", "arc-relative:v2", relative});
    const auto replay = sketch::replay_measurement_linework(model);
    require(replay.edges.size() == 2 && same_point(replay.edges[1].segment.start, {2, 0}),
            "line relative to an arc must retain its exact analytical join");
    require_near(replay.edges[1].segment.end.x, 3, "relative turn must use the arc's end tangent");
    require_near(replay.edges[1].segment.end.y, 0, "relative arc turn must not use its chord heading");
    require(replay.receipts[1] == relative, "arc-relative input must retain original expressions");
}

void test_independent_arcs_remain_analytical() {
    std::vector<ConstructionReceipt> receipts;
    ConstructionReceipt angle;
    angle.segment_id = "curve:e0";
    angle.start = {0, 0};
    angle.chord_end = Vec2{2, 0};
    angle.kind = BoundaryConstructionKind::arc_chord_angle;
    angle.angle = sketch::parse_angle("pi/2 rad");
    receipts.push_back(angle);
    auto height = angle;
    height.kind = BoundaryConstructionKind::arc_chord_height;
    height.angle.reset();
    height.height = sketch::parse_quantity("1 m");
    receipts.push_back(height);
    auto length = height;
    length.kind = BoundaryConstructionKind::arc_chord_length;
    length.height.reset();
    length.arc_length = sketch::parse_quantity("5 m");
    length.clockwise = true;
    receipts.push_back(length);
    auto tangent = length;
    tangent.kind = BoundaryConstructionKind::arc_start_tangent;
    tangent.chord_end.reset();
    tangent.clockwise = false;
    tangent.tangent = sketch::parse_angle("0 deg");
    tangent.arc_length = sketch::parse_quantity("3.141592653589793 m");
    tangent.sweep = sketch::parse_angle("pi rad");
    receipts.push_back(tangent);
    for (std::size_t index = 0; index < receipts.size(); ++index) {
        auto model = point_stroke("curve", {0, 0}, {2, 0});
        model.edges[0].receipt = receipts[index];
        const auto replay = sketch::replay_measurement_linework(model);
        require(replay.edges.size() == 1 && !replay.closed &&
                    replay.receipts[0] == receipts[index],
                "an independent open arc must retain all typed construction inputs");
        const auto& segment = replay.edges[0].segment;
        require(segment.sweep_radians != 0, "analytical arc must not become tessellated lines");
        if (index == 3) {
            require_near(segment.end.x, 0, "start-tangent semicircle must finish on the y axis");
            require_near(segment.end.y, 2, "start-tangent semicircle must have diameter two");
            require(segment.sweep_radians == std::numbers::pi,
                    "start-tangent arc must retain its signed sweep");
        } else {
            require(same_point(segment.end, {2, 0}), "chord arcs must retain exact captured endpoints");
            if (index == 0) require(segment.sweep_radians == std::numbers::pi / 2,
                                    "chord-angle arc must retain its exact sweep");
            if (index == 1) require_near(segment.sweep_radians, std::numbers::pi,
                                 "unit sagitta on a two-metre chord must form a semicircle");
            if (index == 2) {
                require(segment.sweep_radians < 0, "clockwise arc length must produce a negative sweep");
                require_near(sketch::segment_length(segment), 5, "arc-length receipt must produce the measured length");
            }
        }
        const auto encoded = sketch::encode_measurement_linework_model(model);
        const auto decoded = sketch::decode_measurement_linework_model(Json::parse(encoded.dump()));
        require(decoded.supported() && decoded.model->edges[0].receipt == receipts[index],
                "arc codecs must retain every original typed construction input");
    }
}

void test_closed_stroke_requires_exact_closure_and_anchor_identity() {
    auto model = point_stroke("closed", {0, 0}, {2, 0});
    model.closed = true;
    model.edges.push_back({"closed:e1", "closed:v1", "closed:v2",
                           point_line("closed:e1", {2, 0}, {2, 3})});
    ConstructionReceipt close;
    close.segment_id = "closed:e2";
    close.kind = BoundaryConstructionKind::line_closure;
    close.start = {2, 3};
    close.closure_delta = Vec2{-2, -3};
    model.edges.push_back({"closed:e2", "closed:v2", "closed:v0", close});
    const auto replay = sketch::replay_measurement_linework(model);
    require(replay.closed && replay.edges.size() == 3 &&
                same_point(replay.edges.back().segment.end, {0, 0}) &&
                replay.edges.back().end_vertex_id == replay.edges.front().start_vertex_id &&
                replay.receipts.back() == close,
            "closed stroke must use explicit anchor context and starting vertex identity");
    const auto decoded = sketch::decode_measurement_linework_model(
        Json::parse(sketch::encode_measurement_linework_model(model).dump()));
    require(decoded.supported() && decoded.model->closed && decoded.model->edges.size() == 3 &&
                decoded.model->edges.back().receipt == close &&
                decoded.model->edges.back().end_vertex_id == "closed:v0",
            "closed-stroke codec must retain explicit closure and its stable anchor identity");
    auto wrong_delta = model;
    wrong_delta.edges.back().receipt.closure_delta->x += 1e-10;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(wrong_delta); },
                   "tolerance-near closure delta must reject");
    auto wrong_identity = model;
    wrong_identity.edges.back().end_vertex_id = "closed:other";
    expect_invalid([&] { (void)sketch::replay_measurement_linework(wrong_identity); },
                   "closed stroke must reuse the anchor identity");
    auto open_closure = model;
    open_closure.closed = false;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(open_closure); },
                   "generated closure receipt must not be accepted on an open stroke");
    auto unfinished = point_stroke("unfinished", {0, 0}, {2, 0});
    unfinished.closed = true;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(unfinished); },
                   "closed marker cannot invent an omitted edge");
    auto early_closure = model;
    early_closure.edges.push_back({"closed:e3", "closed:v0", "closed:v3",
                                   point_line("closed:e3", {0, 0}, {1, 1})});
    early_closure.edges.push_back({"closed:e4", "closed:v3", "closed:v0",
                                   point_line("closed:e4", {1, 1}, {0, 0})});
    expect_invalid([&] { (void)sketch::replay_measurement_linework(early_closure); },
                   "generated closure receipt must be the final edge");
    auto open_loop = point_stroke("open-loop", {0, 0}, {2, 0});
    open_loop.edges.push_back({"open-loop:e1", "open-loop:v1", "open-loop:v0",
                               point_line("open-loop:e1", {2, 0}, {0, 0})});
    expect_invalid([&] { (void)sketch::replay_measurement_linework(open_loop); },
                   "an open stroke must not finish with the anchor identity");
}

void test_disconnected_strokes_do_not_require_common_geometry_or_identity() {
    const auto first = sketch::replay_measurement_linework(point_stroke("first", {0, 0}, {2, 0}));
    const auto second = sketch::replay_measurement_linework(point_stroke("second", {100, -20}, {100, -17}));
    const auto branch = sketch::replay_measurement_linework(point_stroke("branch", {2, 0}, {2, 3}));
    require(same_point(second.anchor, {100, -20}) && first.stroke_id != second.stroke_id &&
                same_point(first.edges[0].segment.end, branch.edges[0].segment.start) &&
                first.edges[0].end_vertex_id != branch.edges[0].start_vertex_id,
            "separate strokes may be disconnected or share coordinates with independent identities");
}

void test_self_crossing_stroke_is_linework_without_area_admission() {
    auto model = point_stroke("crossing", {0, 0}, {2, 2});
    model.closed = true;
    model.edges.push_back({"crossing:e1", "crossing:v1", "crossing:v2",
                           point_line("crossing:e1", {2, 2}, {0, 2})});
    model.edges.push_back({"crossing:e2", "crossing:v2", "crossing:v3",
                           point_line("crossing:e2", {0, 2}, {2, 0})});
    model.edges.push_back({"crossing:e3", "crossing:v3", "crossing:v0",
                           point_line("crossing:e3", {2, 0}, {0, 0})});
    require(sketch::replay_measurement_linework(model).edges.size() == 4,
            "self-crossing closed linework must remain available for later face selection");
}

void test_explicit_vertex_revisit_requires_same_exact_coordinate() {
    auto model = point_stroke("revisit", {0, 0}, {2, 0});
    model.edges.push_back({"revisit:e1", "revisit:v1", "revisit:v2",
                           point_line("revisit:e1", {2, 0}, {2, 2})});
    model.edges.push_back({"revisit:e2", "revisit:v2", "revisit:v1",
                           point_line("revisit:e2", {2, 2}, {2, 0})});
    model.edges.push_back({"revisit:e3", "revisit:v1", "revisit:v3",
                           point_line("revisit:e3", {2, 0}, {3, 0})});
    require(sketch::replay_measurement_linework(model).edges.size() == 4,
            "explicit exact-coordinate vertex revisit must remain valid loose linework");
    auto conflict = model;
    conflict.edges[2].receipt.chord_end = Vec2{2, 0.01};
    conflict.edges[3].receipt.start = {2, 0.01};
    expect_invalid([&] { (void)sketch::replay_measurement_linework(conflict); },
                   "one stable vertex identity must not describe different coordinates");
}

void test_replay_rejects_malformed_geometry_identities_and_typed_inputs() {
    const auto original = measured_line();
    std::vector<MeasurementLinework> malformed;
    auto changed = original;
    changed.anchor.x = std::numeric_limits<double>::infinity();
    malformed.push_back(changed);
    changed = original;
    changed.edges.clear();
    malformed.push_back(changed);
    changed = original;
    changed.stroke_id.clear();
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].end_vertex_id.clear();
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].receipt.segment_id = "different";
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].segment_id = changed.stroke_id;
    changed.edges[0].receipt.segment_id = changed.stroke_id;
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].start_vertex_id = changed.stroke_id;
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].end_vertex_id = changed.stroke_id;
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].receipt.distance = sketch::parse_quantity("0 m");
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].receipt.distance->metres += 0.001;
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].receipt.heading->original_expression = "1 deg";
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].receipt.heading->normalized_expression.clear();
    malformed.push_back(changed);
    changed = original;
    changed.edges[0].receipt.height = sketch::parse_quantity("1 m");
    malformed.push_back(changed);
    changed = original;
    changed.extensions = Json::array();
    malformed.push_back(changed);
    changed = original;
    changed.schema_version = 999;
    malformed.push_back(changed);
    changed = original;
    changed.replay_version = 999;
    malformed.push_back(changed);
    for (const auto& model : malformed) {
        expect_invalid([&] { (void)sketch::replay_measurement_linework(model); },
                       "malformed model must reject without returning partial replay");
        expect_invalid([&] { (void)sketch::encode_measurement_linework_model(model); },
                       "encoding must not persist an invalid typed model");
    }
    for (double tolerance : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        expect_invalid([&] { (void)sketch::replay_measurement_linework(original, tolerance); },
                       "invalid construction tolerance must reject");
    }
}

void test_exact_joins_and_unique_segment_identities_are_required() {
    auto model = point_stroke("joined", {0, 0}, {2, 0});
    model.edges.push_back({"joined:e1", "joined:v1", "joined:v2",
                           point_line("joined:e1", {2, 0}, {2, 2})});
    auto bad = model;
    bad.edges[1].receipt.start.x += 1e-10;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(bad); },
                   "tolerance-near start must not silently repair the join");
    bad = model;
    bad.edges[1].start_vertex_id = "unjoined";
    expect_invalid([&] { (void)sketch::replay_measurement_linework(bad); },
                   "exact geometry must not repair mismatched join identities");
    bad = model;
    bad.edges[1].segment_id = bad.edges[0].segment_id;
    bad.edges[1].receipt.segment_id = bad.edges[0].segment_id;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(bad); },
                   "duplicate segment identities must reject even when receipt identities agree");
    bad = model;
    bad.edges[1].end_vertex_id = bad.edges[0].start_vertex_id;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(bad); },
                   "duplicate vertex identity at different coordinates must reject");
    bad = model;
    bad.edges[1].end_vertex_id = bad.edges[0].segment_id;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(bad); },
                   "vertex identity must not collide with any segment identity");
    bad = model;
    bad.edges[1].segment_id = bad.edges[0].start_vertex_id;
    bad.edges[1].receipt.segment_id = bad.edges[1].segment_id;
    expect_invalid([&] { (void)sketch::replay_measurement_linework(bad); },
                   "segment identity must not collide with an earlier vertex identity");
    const auto degenerate = point_stroke("zero", {0, 0}, {0, 0});
    expect_invalid([&] { (void)sketch::replay_measurement_linework(degenerate); },
                   "zero-length point-native edge must reject");
}

void test_model_roundtrip_preserves_extensions_and_entered_expressions() {
    auto model = measured_line();
    model.extensions = Json{{"vendor", Json{{"stroke_id", "opaque-other-id"},
                                            {"nullable", nullptr},
                                            {"future", Json::array({1, "two", true})}}}};
    const auto expected = fixture_json(model);
    const auto encoded = sketch::encode_measurement_linework_model(model);
    require(encoded == expected && !encoded.contains("area") && !encoded.contains("geometry"),
            "model must persist receipts and opaque extensions without derived area or geometry");
    const auto decoded = sketch::decode_measurement_linework_model(Json::parse(encoded.dump()));
    require(decoded.supported() && !decoded.original_model && decoded.version == 1 &&
                decoded.model->extensions == model.extensions &&
                decoded.model->edges[0].receipt == model.edges[0].receipt,
            "typed model roundtrip must retain original inputs and extensions");
    require(sketch::encode_measurement_linework_model(*decoded.model) == expected,
            "reencoding must preserve the exact typed model");
}

void test_decode_fails_closed_on_malformed_known_schema() {
    const auto valid = fixture_json(measured_line());
    std::vector<Json> malformed;
    auto changed = valid;
    changed["unexpected"] = true;
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["unexpected"] = true;
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["receipt"]["unexpected"] = true;
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["segment_id"] = changed["stroke_id"];
    changed["segments"][0]["receipt"]["segment_id"] = changed["stroke_id"];
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["start_vertex_id"] = changed["stroke_id"];
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["end_vertex_id"] = changed["stroke_id"];
    malformed.push_back(changed);
    changed = valid;
    changed.erase("closed");
    malformed.push_back(changed);
    changed = valid;
    changed["closed"] = "false";
    malformed.push_back(changed);
    changed = valid;
    changed["anchor"] = Json::array({0, 0, 0});
    malformed.push_back(changed);
    changed = valid;
    changed["segments"] = Json::object();
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["receipt"]["distance"]["exact_metres"]["denominator"] = 0;
    malformed.push_back(changed);
    changed = valid;
    changed["segments"][0]["receipt"]["distance"]["original_expression"] = "8 m";
    malformed.push_back(changed);
    changed = valid;
    changed["extensions"] = nullptr;
    malformed.push_back(changed);
    for (const auto& model : malformed) {
        expect_invalid([&] { (void)sketch::decode_measurement_linework_model(model); },
                       "malformed known typed schema must fail closed");
    }
}

void test_version_inspection_and_opaque_future_models() {
    const auto known = sketch::inspect_measurement_linework_model(Json{{"version", 1}, {"replay_version", 1}});
    require(known.format == sketch::MeasurementLineworkFormat::supported_v1 &&
                known.version == 1 && known.replay_version == 1,
            "version inspection must not require the known model payload");
    const auto future = Json{{"version", 99}, {"future_shape", Json{{"opaque", true}}}};
    const auto inspected = sketch::inspect_measurement_linework_model(future);
    const auto decoded = sketch::decode_measurement_linework_model(future);
    require(inspected.format == sketch::MeasurementLineworkFormat::unsupported_version &&
                inspected.version == 99 && !decoded.supported() &&
                decoded.original_model == std::optional<Json>{future} && decoded.version == 99,
            "future positive schemas must retain their exact opaque model");
    const auto future_replay = Json{{"version", 1}, {"replay_version", 99},
                                    {"future_inputs", Json::array({1, 2, 3})}};
    const auto opaque = sketch::decode_measurement_linework_model(future_replay);
    const auto replay_inspected = sketch::inspect_measurement_linework_model(future_replay);
    require(!opaque.supported() && opaque.original_model == std::optional<Json>{future_replay} &&
                opaque.version == 1 && opaque.replay_version == 99 &&
                replay_inspected.format == sketch::MeasurementLineworkFormat::unsupported_replay_version &&
                replay_inspected.replay_version == 99,
            "future replay dialect must stay opaque without decoding today's payload");
    for (const Json& version : std::vector<Json>{0, -1, "1", 1.0, true, nullptr}) {
        expect_invalid([&] { (void)sketch::inspect_measurement_linework_model(Json{{"version", version}}); },
                       "version must be a positive integral discriminator");
        expect_invalid([&] { (void)sketch::decode_measurement_linework_model(Json{{"version", 1},
                                                                               {"replay_version", version}}); },
                       "replay version must be a positive integral discriminator");
    }
    expect_invalid([&] { (void)sketch::inspect_measurement_linework_model(Json::array()); },
                   "model version inspection must reject non-object shapes");
    expect_invalid([&] { (void)sketch::decode_measurement_linework_model(Json{{"version", 1}}); },
                   "known model must supply a replay discriminator");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"independent open measurement", test_independent_open_measurement_line_preserves_exact_inputs},
        {"relative turn", test_relative_turn_uses_previous_line_and_retains_inputs},
        {"signed rise/run", test_rise_run_preserves_signed_measurements},
        {"relative arc tangent", test_relative_turn_uses_previous_arc_end_tangent},
        {"independent analytical arcs", test_independent_arcs_remain_analytical},
        {"exact closure", test_closed_stroke_requires_exact_closure_and_anchor_identity},
        {"disconnected strokes", test_disconnected_strokes_do_not_require_common_geometry_or_identity},
        {"self-crossing linework", test_self_crossing_stroke_is_linework_without_area_admission},
        {"explicit vertex revisit", test_explicit_vertex_revisit_requires_same_exact_coordinate},
        {"malformed replay", test_replay_rejects_malformed_geometry_identities_and_typed_inputs},
        {"join and identity validation", test_exact_joins_and_unique_segment_identities_are_required},
        {"typed model roundtrip", test_model_roundtrip_preserves_extensions_and_entered_expressions},
        {"strict known schema", test_decode_fails_closed_on_malformed_known_schema},
        {"version preservation", test_version_inspection_and_opaque_future_models},
    };
    std::size_t failures = 0;
    for (const auto& [name, operation] : tests) {
        try {
            operation();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) return 1;
    std::cout << "Measurement linework tests passed\n";
    return 0;
}
