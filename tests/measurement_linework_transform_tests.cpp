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
using sketch::MeasurementLinework;
using sketch::PlanarTransform;
using sketch::Vec2;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
void require_near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-12, message);
}
void expect_invalid(const std::function<void()>& operation, const char* message) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

MeasurementLinework measured_line() {
    MeasurementLinework model;
    model.stroke_id = "stroke";
    model.anchor = {2, 3};
    sketch::ConstructionReceipt receipt;
    receipt.segment_id = "edge0";
    receipt.kind = sketch::BoundaryConstructionKind::line_heading;
    receipt.start = model.anchor;
    receipt.distance = sketch::parse_quantity("6 ft 6 3/4 in");
    receipt.heading = sketch::parse_angle("0 deg");
    model.edges.push_back({"edge0", "vertex0", "vertex1", receipt});
    model.extensions = Json{{"vendor", Json{{"unchanged", "edge0"}}}};
    return model;
}

MeasurementLinework polyline(const std::vector<Vec2>& points,
                             const std::vector<std::string>& ids, bool closed = false) {
    MeasurementLinework model;
    model.stroke_id = "polyline";
    model.anchor = points.front();
    model.closed = closed;
    for (std::size_t i = 1; i < points.size(); ++i) {
        sketch::ConstructionReceipt receipt;
        receipt.segment_id = "edge" + std::to_string(i);
        receipt.kind = sketch::BoundaryConstructionKind::line_to_point;
        receipt.start = points[i - 1];
        receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, ids[i - 1], ids[i], receipt});
    }
    return model;
}

Json wire_transform() {
    return Json{{"version", 1}, {"pivot", Json::array({2, 3})},
        {"rotation_radians", std::numbers::pi / 2}, {"flip_horizontal", false},
        {"flip_vertical", false}, {"offset", Json::array({10, -4})}};
}

// These fail if the operation is omitted, applied in local space, or rewrites receipts.
void translation_rotation_and_order_preserve_local_inputs() {
    const auto original = measured_line();
    const auto v1 = sketch::encode_measurement_linework_model(original);
    const PlanarTransform rotation{{2, 3}, std::numbers::pi / 2, false, false, {10, -4}};
    const auto moved = sketch::transformed_measurement_linework(original, rotation);
    require(moved.schema_version == 2 && moved.replay_version == 2 && moved.transforms.size() == 1,
            "a real operation must promote to schema/replay two and append a frame");
    require(same_point(moved.anchor, original.anchor) && moved.edges == original.edges &&
            moved.extensions == original.extensions, "rigid operations must retain every local input");
    const auto replay = sketch::replay_measurement_linework(moved);
    require(replay.replay_version == 2 && same_point(replay.anchor, {12, -1}),
            "world anchor must rotate about pivot then translate");
    require_near(replay.edges[0].segment.end.x, 12, "line endpoint x must rotate");
    require_near(replay.edges[0].segment.end.y, 1.00025, "measured line must retain analytical length");
    require_near(sketch::segment_length(replay.edges[0].segment), 2.00025, "rigid line length must survive");
    require(replay.receipts[0] == original.edges[0].receipt, "replay receipts must remain local");
    const auto encoded = sketch::encode_measurement_linework_model(moved);
    require(encoded["segments"].dump() == v1["segments"].dump() && encoded["anchor"] == v1["anchor"],
            "encoded local receipts and anchor must remain byte exact");
    require(encoded["transforms"] == Json::array({wire_transform()}), "transform wire must be explicit version one");
    const auto roundtrip = sketch::decode_measurement_linework_model(Json::parse(encoded.dump()));
    require(roundtrip.supported() && sketch::encode_measurement_linework_model(*roundtrip.model) == encoded,
            "schema two must roundtrip strict transforms");
    const auto twice = sketch::transformed_measurement_linework(moved, {{}, 0, false, false, {1, 5}});
    require(twice.transforms.size() == 2, "world operations must append in order");
    require(same_point(sketch::replay_measurement_linework(twice).anchor, {13, 4}),
            "second operation must act on first operation's world output");
    require(sketch::encode_measurement_linework_model(original).dump() == v1.dump(),
            "transforming a copy must not mutate the source");
    const auto translated = sketch::transformed_measurement_linework(original, {{}, 0, false, false, {4, -6}});
    const auto translated_replay = sketch::replay_measurement_linework(translated);
    require(same_point(translated_replay.anchor, {6, -3}), "pure translation must move the anchor");
    require_near(translated_replay.edges[0].segment.end.x, 8.00025, "pure translation must move a measured endpoint");
    const auto reflected = sketch::transformed_measurement_linework(original, {{2, 3}, 0, true, false, {}});
    require_near(sketch::replay_measurement_linework(reflected).edges[0].segment.end.x, -0.00025,
                 "line reflection must reflect about its supplied pivot");
}

// This fails if reflection leaves the analytical sweep unchanged or tessellates arcs.
void reflection_keeps_analytic_arc_and_reverses_sweep() {
    auto model = polyline({{0, 0}, {2, 0}}, {"a", "b"});
    auto& receipt = model.edges[0].receipt;
    receipt.kind = sketch::BoundaryConstructionKind::arc_chord_angle;
    receipt.angle = sketch::parse_angle("90 deg");
    const auto reflected = sketch::transformed_measurement_linework(model, {{}, 0, true, false, {3, 4}});
    const auto replay = sketch::replay_measurement_linework(reflected);
    require(same_point(replay.edges[0].segment.start, {3, 4}) &&
            same_point(replay.edges[0].segment.end, {1, 4}), "reflection must move exact arc endpoints");
    require(replay.edges[0].segment.sweep_radians == -std::numbers::pi / 2,
            "single reflection must reverse the signed sweep");
    require_near(sketch::segment_length(replay.edges[0].segment), std::numbers::pi / std::sqrt(2.0),
         "reflected circular arc must retain its analytical length");
    require(replay.receipts[0] == receipt, "reflection must not rewrite entered arc angle");
    const auto both = sketch::transformed_measurement_linework(model, {{}, 0, true, true, {}});
    require(sketch::replay_measurement_linework(both).edges[0].segment.sweep_radians == std::numbers::pi / 2,
            "two reflections must preserve sweep orientation");
    const auto rotated = sketch::transformed_measurement_linework(model, {{}, std::numbers::pi / 2, false, false, {}});
    const auto rotated_segment = sketch::replay_measurement_linework(rotated).edges[0].segment;
    require_near(rotated_segment.end.x, 0, "arc rotation must rotate the chord endpoint x");
    require_near(rotated_segment.end.y, 2, "arc rotation must rotate the chord endpoint y");
    require(rotated_segment.sweep_radians == std::numbers::pi / 2, "arc rotation must preserve sweep");
    const auto translated = sketch::transformed_measurement_linework(model, {{}, 0, false, false, {7, -3}});
    const auto translated_segment = sketch::replay_measurement_linework(translated).edges[0].segment;
    require(same_point(translated_segment.start, {7, -3}) && same_point(translated_segment.end, {9, -3}) &&
            translated_segment.sweep_radians == std::numbers::pi / 2, "arc translation must retain analytical shape");
}

// This fails if v2 accidentally invokes closed-boundary admission or loses stable joints.
void topology_revisits_retrace_and_self_cross_remain_exact() {
    const auto retrace = polyline({{0, 0}, {2, 0}, {2, 2}, {2, 0}, {3, 0}},
                                 {"a", "b", "c", "b", "d"});
    const auto crossing = polyline({{0, 0}, {2, 2}, {0, 2}, {2, 0}, {0, 0}},
                                  {"a", "b", "c", "d", "a"}, true);
    for (const auto& model : {retrace, crossing}) {
        auto moved = sketch::transformed_measurement_linework(model, {{0.3, -0.7}, 0.37, true, false, {10, 20}});
        moved = sketch::transformed_measurement_linework(moved, {{5, 7}, -0.19, false, true, {-2, 1}});
        const auto replay = sketch::replay_measurement_linework(moved);
        require(replay.edges.size() == 4 && moved.edges == model.edges, "loose topology must survive rigid transforms");
        for (std::size_t i = 1; i < replay.edges.size(); ++i) {
            require(same_point(replay.edges[i - 1].segment.end, replay.edges[i].segment.start),
                    "shared stable vertices must reuse exactly equal world coordinates");
        }
        if (model.closed) {
            require(same_point(replay.edges.back().segment.end, replay.anchor), "closed identity must reuse world anchor");
        } else {
            require(same_point(replay.edges[0].segment.end, replay.edges[2].segment.end) &&
                    same_point(replay.edges[2].segment.end, replay.edges[3].segment.start),
                    "revisited vertex must have one exact transformed coordinate");
        }
    }
}

void historical_v1_encoding_and_identity_are_preserved() {
    const auto model = measured_line();
    const auto original = sketch::encode_measurement_linework_model(model);
    require(original["version"] == 1 && original["replay_version"] == 1 && !original.contains("transforms"),
            "default models must retain the historical v1 wire shape");
    const auto identity = sketch::transformed_measurement_linework(model, {});
    require(sketch::encode_measurement_linework_model(identity).dump() == original.dump(),
            "effective identity must retain historical v1 bytes");
    auto illegal = model;
    illegal.transforms.push_back({});
    expect_invalid([&] { (void)sketch::replay_measurement_linework(illegal); }, "v1 typed transforms must reject");
    auto wire = original;
    wire["transforms"] = Json::array();
    expect_invalid([&] { (void)sketch::decode_measurement_linework_model(wire); }, "v1 wire transforms must reject");
}

// This fails if finite overflow/drift or malformed transforms are silently saved.
void invalid_operations_are_atomic_and_precision_checked() {
    const auto model = measured_line();
    const auto before = sketch::encode_measurement_linework_model(model).dump();
    for (const auto& transform : std::vector<PlanarTransform>{
            {{}, std::numeric_limits<double>::quiet_NaN(), false, false, {}},
            {{std::numeric_limits<double>::quiet_NaN(), 0}, 0, false, false, {}},
            {{}, 0, false, false, {std::numeric_limits<double>::infinity(), 0}},
            {{}, 0, false, false, {1e16, 0}},
            {{std::numeric_limits<double>::max(), 0}, 0, true, false, {}}}) {
        expect_invalid([&] { (void)sketch::transformed_measurement_linework(model, transform); },
                       "invalid or precision-losing transform must throw");
        require(sketch::encode_measurement_linework_model(model).dump() == before, "failed transform must leave source atomic");
    }
    const auto short_line = polyline({{0, 0}, {0.25, 0}}, {"a", "b"});
    expect_invalid([&] { (void)sketch::transformed_measurement_linework(short_line, {{}, 0, false, false, {1e16, 0}}); },
                   "translation that collapses an edge must reject");
    const auto fractional_anchor = polyline({{0.1, 0}, {2.1, 0}}, {"a", "b"});
    expect_invalid([&] { (void)sketch::transformed_measurement_linework(fractional_anchor,
        {{1e12,0},0,true,false,{-2e12,0}}); },
        "reflection about a large pivot must reject common world displacement even when length stays exact");
    expect_invalid([&] { (void)sketch::transformed_measurement_linework(fractional_anchor,
        {{1e12,0},std::numbers::pi/2,false,false,{-1e12,1e12}}); },
        "rotation about a large pivot must reject anchor precision lost through cancellation");
    expect_invalid([&] { (void)sketch::transformed_measurement_linework(fractional_anchor, {{}, 0, false, false, {1e12, 0}}); },
                   "translation anchor drift must reject even when chord length remains exact");
    auto malformed_local = model;
    malformed_local.edges[0].receipt.distance->original_expression = "9 m";
    expect_invalid([&] { (void)sketch::transformed_measurement_linework(malformed_local, {}); },
                   "identity operation must still validate local input");
}

void strict_v2_wire_and_future_opaque_pairs() {
    auto valid = sketch::encode_measurement_linework_model(measured_line());
    valid["version"] = 2;
    valid["replay_version"] = 2;
    valid["transforms"] = Json::array({wire_transform()});
    require(sketch::inspect_measurement_linework_model(valid).format == sketch::MeasurementLineworkFormat::supported_v2,
            "schema/replay two must be recognized");
    require(sketch::decode_measurement_linework_model(valid).supported(), "known v2 must decode");
    // The rotated edge is vertical. Its x coordinate and integer anchor remain
    // representable at this magnitude, so a large x offset need not lose precision.
    auto large_x_offset = valid;
    large_x_offset["transforms"][0]["offset"] = Json::array({1e16, 0});
    const auto large_x_decoded = sketch::decode_measurement_linework_model(large_x_offset);
    require(large_x_decoded.supported(), "representable large offset must remain a recognized model");
    const auto large_x_replay = sketch::replay_measurement_linework(*large_x_decoded.model);
    require(same_point(large_x_replay.anchor, {1e16 + 2, 3}),
            "representable large x offset must retain its world anchor");
    require_near(sketch::segment_length(large_x_replay.edges[0].segment), 2.00025,
                 "representable large x offset must preserve the vertical measured length");

    std::vector<std::pair<const char*, Json>> malformed;
    auto wire = valid; wire.erase("transforms"); malformed.emplace_back("missing transforms", wire);
    wire = valid; wire["transforms"] = Json::object(); malformed.emplace_back("non-array transforms", wire);
    wire = valid; wire["transforms"][0].erase("version"); malformed.emplace_back("missing transform version", wire);
    wire = valid; wire["transforms"][0]["version"] = 2; malformed.emplace_back("unsupported transform version", wire);
    wire = valid; wire["transforms"][0]["version"] = 1.0; malformed.emplace_back("floating transform version", wire);
    wire = valid; wire["transforms"][0]["scale"] = 2; malformed.emplace_back("unknown scaling field", wire);
    wire = valid; wire["transforms"][0]["flip_horizontal"] = 1; malformed.emplace_back("non-boolean flip", wire);
    wire = valid; wire["transforms"][0]["rotation_radians"] = "0"; malformed.emplace_back("non-numeric rotation", wire);
    wire = valid; wire["transforms"][0]["pivot"] = Json::array({0}); malformed.emplace_back("malformed pivot", wire);
    // A large y offset rounds away part of this vertical edge's measured chord.
    wire = valid; wire["transforms"][0]["offset"] = Json::array({0, 1e16}); malformed.emplace_back("precision-losing offset", wire);
    wire = valid; wire["transforms"][0]["rotation_radians"] = std::numeric_limits<double>::quiet_NaN(); malformed.emplace_back("non-finite rotation", wire);
    wire = valid; wire["transforms"][0]["offset"] = Json::array({0, std::numeric_limits<double>::infinity()}); malformed.emplace_back("non-finite offset", wire);
    for (const auto& [name, bad] : malformed) {
        const auto message = std::string("malformed recognized v2 must fail closed: ") + name;
        expect_invalid([&] { (void)sketch::decode_measurement_linework_model(bad); }, message.c_str());
    }
    auto empty_frames = valid;
    empty_frames["transforms"] = Json::array();
    const auto empty_decoded = sketch::decode_measurement_linework_model(empty_frames);
    require(empty_decoded.supported() && sketch::encode_measurement_linework_model(*empty_decoded.model) == empty_frames,
            "an explicit empty v2 stack must preserve its v2 dialect");
    for (const auto& future : std::vector<Json>{Json{{"version", 999}, {"opaque", true}},
            Json{{"version", 2}, {"replay_version", 999}, {"opaque", true}},
            Json{{"version", 1}, {"replay_version", 2}, {"opaque", true}},
            Json{{"version", 2}, {"replay_version", 1}, {"opaque", true}}}) {
        const auto decoded = sketch::decode_measurement_linework_model(future);
        require(!decoded.supported() && decoded.original_model == std::optional<Json>{future},
                "unknown positive schema/replay pairs must retain exact opaque payload");
    }
}
} // namespace

int main() {
    sketch::testing::noninteractive_errors();
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"translation rotation and operation order", translation_rotation_and_order_preserve_local_inputs},
        {"analytical arc reflection", reflection_keeps_analytic_arc_and_reverses_sweep},
        {"stable loose topology", topology_revisits_retrace_and_self_cross_remain_exact},
        {"historical v1 and identity", historical_v1_encoding_and_identity_are_preserved},
        {"atomic precision rejection", invalid_operations_are_atomic_and_precision_checked},
        {"strict v2 and future pairs", strict_v2_wire_and_future_opaque_pairs},
    };
    std::size_t failures = 0;
    for (const auto& [name, operation] : tests) {
        try { operation(); } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) return 1;
    std::cout << "Measurement linework transform tests passed\n";
    return 0;
}
