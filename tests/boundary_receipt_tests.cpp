#include "sketch/boundary_receipt.hpp"

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
using sketch::AngleInput;
using sketch::BoundaryConstructionKind;
using sketch::BoundaryConstructionRecord;
using sketch::ConstructionReceipt;
using sketch::ConstructionTopologyEdge;
using sketch::Quantity;
using sketch::Segment;
using sketch::Vec2;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Quantity q(const char* expression) {
    return sketch::parse_quantity(expression);
}

AngleInput angle(const char* expression) {
    return sketch::parse_angle(expression);
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

ConstructionReceipt line_heading(std::string id, Vec2 start, const char* distance,
                                 const char* heading) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::line_heading;
    result.start = start;
    result.distance = q(distance);
    result.heading = angle(heading);
    return result;
}

ConstructionReceipt rise_run(std::string id, Vec2 start, const char* rise,
                             const char* run) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::line_rise_run;
    result.start = start;
    result.rise = q(rise);
    result.run = q(run);
    return result;
}

ConstructionReceipt relative_turn(std::string id, Vec2 start, const char* distance,
                                  const char* turn) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::line_relative_turn;
    result.start = start;
    result.distance = q(distance);
    result.turn = angle(turn);
    return result;
}

ConstructionReceipt closure(std::string id, Vec2 start, Vec2 delta) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::line_closure;
    result.start = start;
    result.closure_delta = delta;
    return result;
}

ConstructionReceipt line_to_point(std::string id, Vec2 start, Vec2 end) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::line_to_point;
    result.start = start;
    result.chord_end = end;
    return result;
}

ConstructionReceipt chord_angle(std::string id, Vec2 start, Vec2 end, const char* sweep) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::arc_chord_angle;
    result.start = start;
    result.chord_end = end;
    result.angle = angle(sweep);
    return result;
}

ConstructionReceipt chord_height(std::string id, Vec2 start, Vec2 end, const char* height) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::arc_chord_height;
    result.start = start;
    result.chord_end = end;
    result.height = q(height);
    return result;
}

ConstructionReceipt chord_length(std::string id, Vec2 start, Vec2 end, const char* length,
                                 bool clockwise) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::arc_chord_length;
    result.start = start;
    result.chord_end = end;
    result.arc_length = q(length);
    result.clockwise = clockwise;
    return result;
}

ConstructionReceipt start_tangent(std::string id, Vec2 start, const char* tangent,
                                  const char* length, const char* sweep) {
    ConstructionReceipt result;
    result.segment_id = std::move(id);
    result.kind = BoundaryConstructionKind::arc_start_tangent;
    result.start = start;
    result.tangent = angle(tangent);
    result.arc_length = q(length);
    result.sweep = angle(sweep);
    return result;
}

BoundaryConstructionRecord base_record() {
    BoundaryConstructionRecord result;
    result.boundary_id = "boundary-receipt-test";
    result.anchor = {0.0, 0.0};
    result.extensions = Json{{"vendor", Json{{"keep", true}, {"count", 2}}}};
    return result;
}

void add_edge(BoundaryConstructionRecord& record, std::size_t index,
              std::string segment_id, std::string start_vertex_id, std::string end_vertex_id,
              ConstructionReceipt receipt) {
    require(receipt.segment_id == segment_id, "fixture receipt identity must match edge");
    require(index == record.edges.size(), "fixture edges must be appended in order");
    record.edges.push_back({std::move(segment_id), std::move(start_vertex_id),
                            std::move(end_vertex_id), std::move(receipt)});
}

std::vector<BoundaryConstructionRecord> all_forms() {
    std::vector<BoundaryConstructionRecord> result;

    auto heading = base_record();
    add_edge(heading, 0, "heading-0", "v0", "v1",
             line_heading("heading-0", {0, 0}, "1 m", "0 deg"));
    add_edge(heading, 1, "heading-1", "v1", "v2",
             line_heading("heading-1", {1, 0}, "1 m", "90 deg"));
    add_edge(heading, 2, "heading-2", "v2", "v3",
             line_heading("heading-2", {1, 1}, "1 m", "180 deg"));
    add_edge(heading, 3, "heading-3", "v3", "v0",
             line_heading("heading-3", {0, 1}, "1 m", "270 deg"));
    result.push_back(std::move(heading));

    auto rise_run_record = base_record();
    add_edge(rise_run_record, 0, "rise-0", "v0", "v1",
             rise_run("rise-0", {0, 0}, "0 m", "2 m"));
    add_edge(rise_run_record, 1, "rise-1", "v1", "v2",
             rise_run("rise-1", {2, 0}, "1 m", "0 m"));
    add_edge(rise_run_record, 2, "rise-2", "v2", "v3",
             rise_run("rise-2", {2, 1}, "0 m", "-2 m"));
    add_edge(rise_run_record, 3, "rise-3", "v3", "v0",
             rise_run("rise-3", {0, 1}, "-1 m", "0 m"));
    result.push_back(std::move(rise_run_record));

    auto relative = base_record();
    add_edge(relative, 0, "relative-0", "v0", "v1",
             line_heading("relative-0", {0, 0}, "1 m", "0 deg"));
    add_edge(relative, 1, "relative-1", "v1", "v2",
             relative_turn("relative-1", {1, 0}, "1 m", "90 deg"));
    add_edge(relative, 2, "relative-2", "v2", "v3",
             relative_turn("relative-2", {1, 1}, "1 m", "90 deg"));
    add_edge(relative, 3, "relative-3", "v3", "v0",
             relative_turn("relative-3", {0, 1}, "1 m", "90 deg"));
    result.push_back(std::move(relative));

    auto line_closure = base_record();
    add_edge(line_closure, 0, "closure-0", "v0", "v1",
             line_heading("closure-0", {0, 0}, "1 m", "0 deg"));
    add_edge(line_closure, 1, "closure-1", "v1", "v2",
             line_heading("closure-1", {1, 0}, "1 m", "90 deg"));
    add_edge(line_closure, 2, "closure-2", "v2", "v0",
             closure("closure-2", {1, 1}, {-1, -1}));
    result.push_back(std::move(line_closure));

    auto arc_angle = base_record();
    add_edge(arc_angle, 0, "arc-angle-0", "v0", "v1",
             chord_angle("arc-angle-0", {0, 0}, {1, 0}, "90 deg"));
    add_edge(arc_angle, 1, "arc-angle-1", "v1", "v0",
             closure("arc-angle-1", {1, 0}, {-1, 0}));
    result.push_back(std::move(arc_angle));

    auto arc_height = base_record();
    add_edge(arc_height, 0, "arc-height-0", "v0", "v1",
             chord_height("arc-height-0", {0, 0}, {1, 0}, "0.25 m"));
    add_edge(arc_height, 1, "arc-height-1", "v1", "v0",
             closure("arc-height-1", {1, 0}, {-1, 0}));
    result.push_back(std::move(arc_height));

    auto arc_length = base_record();
    add_edge(arc_length, 0, "arc-length-0", "v0", "v1",
             chord_length("arc-length-0", {0, 0}, {1, 0}, "1.5 m", true));
    add_edge(arc_length, 1, "arc-length-1", "v1", "v0",
             closure("arc-length-1", {1, 0}, {-1, 0}));
    result.push_back(std::move(arc_length));

    const auto tangent_arc = sketch::arc_from_start_tangent(
        {0.0, 0.0}, 0.0, 1.0, std::numbers::pi / 2.0);
    auto arc_tangent = base_record();
    add_edge(arc_tangent, 0, "arc-tangent-0", "v0", "v1",
             start_tangent("arc-tangent-0", {0, 0}, "0 deg", "1 m", "90 deg"));
    add_edge(arc_tangent, 1, "arc-tangent-1", "v1", "v0",
             closure("arc-tangent-1", tangent_arc.end,
                     {-tangent_arc.end.x, -tangent_arc.end.y}));
    result.push_back(std::move(arc_tangent));

    return result;
}

BoundaryConstructionRecord point_record() {
    // These coordinates deliberately exceed the range in which a casual
    // decimal reconstruction would be harmless. The point-native receipt
    // must copy each endpoint bit-for-bit and close on the captured anchor.
    const Vec2 points[]{{123456789.125, -987654321.75},
                       {123456792.625, -987654321.75},
                       {123456792.625, -987654318.25},
                       {123456789.125, -987654318.25}};
    auto result = base_record();
    result.schema_version = sketch::boundary_receipt_schema_version_v2;
    result.anchor = points[0];
    for (std::size_t index = 0; index < 4; ++index) {
        const auto next = (index + 1) % 4;
        const auto segment_id = "point-" + std::to_string(index);
        const auto start_id = "point-v" + std::to_string(index);
        const auto end_id = "point-v" + std::to_string(next);
        add_edge(result, index, segment_id, start_id, end_id,
                 line_to_point(segment_id, points[index], points[next]));
    }
    return result;
}

void test_transform_frames_preserve_local_inputs_and_compose() {
    auto records = all_forms();
    records.push_back(point_record());
    const std::vector<sketch::PlanarTransform> operations{
        {{2, -1}, std::numbers::pi / 3, false, false, {}},
        {{-3, 2}, 0, true, false, {}},
        {{1, 4}, 0, false, true, {}},
        {{}, 0, true, true, {8, -4}},
        {{}, 0, false, false, {0.125, -0.25}}};
    for (auto original : records) {
        original.extensions["boundary_id"] = original.boundary_id;
        original.extensions["segment_id"] = original.edges.front().segment_id;
        const auto original_json = sketch::encode_boundary_receipt_envelope(original);
        require(!original_json.contains("transforms"), "legacy encoding must omit transforms");
        auto transformed = original;
        auto expected = sketch::replay_boundary_construction(original);
        for (const auto& operation : operations) {
            transformed = sketch::transformed_boundary_construction(transformed, operation);
            expected.anchor = sketch::transform_point(expected.anchor, operation);
            for (auto& edge : expected.edges) {
                edge.segment = sketch::transform_segment(edge.segment, operation);
            }
            require(sketch::replay_boundary_construction(transformed) == expected,
                    "all receipt kinds must replay then apply ordered transforms exactly");
            require(transformed.edges == original.edges &&
                        transformed.anchor.x == original.anchor.x &&
                        transformed.anchor.y == original.anchor.y &&
                        transformed.extensions == original.extensions,
                    "transforms must preserve every local coordinate, expression and extension");
            const auto encoded = sketch::encode_boundary_receipt_envelope(transformed);
            require(encoded.at("segments") == original_json.at("segments") &&
                        encoded.at("anchor") == original_json.at("anchor"),
                    "schema three must serialize original local receipts");
            const auto decoded = sketch::decode_boundary_receipt_envelope(Json::parse(encoded.dump()));
            require(decoded.supported() && *decoded.record == transformed,
                    "transform frames must roundtrip exactly through JSON text");
        }
        require(transformed.schema_version == sketch::boundary_receipt_schema_version_v3 &&
                    transformed.transforms.size() == operations.size(),
                "transform operations must opt into schema three and append frames");
        std::map<std::string, std::string, std::less<>> identities;
        identities[original.boundary_id] = "copy-boundary";
        for (const auto& edge : original.edges) {
            for (const auto& id : {edge.segment_id, edge.start_vertex_id, edge.end_vertex_id}) {
                identities[id] = "copy-" + id;
            }
        }
        auto expected_copy = transformed;
        expected_copy.transforms.push_back({});
        expected_copy.boundary_id = identities.at(expected_copy.boundary_id);
        for (auto& edge : expected_copy.edges) {
            edge.segment_id = identities.at(edge.segment_id);
            edge.start_vertex_id = identities.at(edge.start_vertex_id);
            edge.end_vertex_id = identities.at(edge.end_vertex_id);
            edge.receipt.segment_id = identities.at(edge.receipt.segment_id);
        }
        require(sketch::transformed_boundary_construction(transformed, {}, identities) == expected_copy,
                "reidentification must remap typed IDs only, preserving opaque extensions");
        require(sketch::encode_boundary_receipt_envelope(original) == original_json,
                "transforming must leave legacy source encoding unchanged");
    }
    require(sketch::boundary_receipt_latest_schema_version == 2,
            "ordinary current authoring must continue to use schema two");
}

void test_transform_frame_validation() {
    const auto square = [](double extent) {
        auto record = base_record();
        record.schema_version = sketch::boundary_receipt_schema_version_v2;
        const Vec2 points[]{{0,0},{extent,0},{extent,extent},{0,extent}};
        for (std::size_t i=0; i<4; ++i) {
            const auto id = "precision-edge-" + std::to_string(i);
            add_edge(record,i,id,"precision-v"+std::to_string(i),"precision-v"+std::to_string((i+1)%4),
                line_to_point(id,points[i],points[(i+1)%4]));
        }
        return record;
    };
    expect_invalid([&] { (void)sketch::transformed_boundary_construction(square(3),
        sketch::PlanarTransform{{},0,false,false,{1e16,0}}); },
        "finite transform must reject a three-metre edge rounded into four metres");
    auto accumulated = sketch::transformed_boundary_construction(square(3),
        sketch::PlanarTransform{{},0,false,false,{1e8,1e8}});
    for (int i=0; i<100; ++i)
        accumulated.transforms.push_back({{1e8,1e8},1e-6,false,false,{}});
    expect_invalid([&] { (void)sketch::replay_boundary_construction(accumulated); },
        "small per-frame rounding errors must not accumulate into shape distortion");
    auto distant = sketch::transformed_boundary_construction(square(4),
        sketch::PlanarTransform{{},0,false,false,{1e16,0}});
    expect_invalid([&] { (void)sketch::transformed_boundary_construction(distant,
        sketch::PlanarTransform{{},0,false,false,{1,0}}); },
        "an offset larger than tolerance must not silently disappear below coordinate resolution");
    auto small_offset = sketch::transformed_boundary_construction(square(4),
        sketch::PlanarTransform{{},0,false,false,{1,0}});
    expect_invalid([&] { (void)sketch::transformed_boundary_construction(small_offset,
        sketch::PlanarTransform{{},0,false,false,{1e16,0}}); },
        "a large offset must not erase the previous anchor displacement");
    auto accumulated_offsets = sketch::transformed_boundary_construction(square(3),
        sketch::PlanarTransform{{},0,false,false,{1e8,0}});
    for (int i=0; i<100; ++i) accumulated_offsets.transforms.push_back({{},0,false,false,{0.1,0}});
    expect_invalid([&] { (void)sketch::replay_boundary_construction(accumulated_offsets); },
        "translation rounding must be bounded cumulatively");
    const auto original = all_forms().front();
    const auto framed = sketch::transformed_boundary_construction(original, {});
    const auto encoded = sketch::encode_boundary_receipt_envelope(framed);
    const auto reject = [](const Json& value) {
        expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(value); },
                       "malformed transform envelope must reject");
    };
    auto bad = encoded;
    bad.erase("transforms");
    reject(bad);
    bad = encoded;
    bad["transforms"] = Json::object();
    reject(bad);
    for (const auto* field : {"pivot", "rotation_radians", "flip_horizontal", "flip_vertical", "offset"}) {
        bad = encoded;
        bad["transforms"][0].erase(field);
        reject(bad);
        bad = encoded;
        bad["transforms"][0][field] = "wrong";
        reject(bad);
    }
    bad = encoded;
    bad["transforms"][0]["unexpected"] = true;
    reject(bad);
    bad = encoded;
    bad["transforms"][0]["flip_horizontal"] = 1;
    reject(bad);
    bad = encoded;
    bad["transforms"][0]["rotation_radians"] = std::numeric_limits<double>::infinity();
    reject(bad);
    for (const auto version : {1, 2}) {
        bad = encoded;
        bad["version"] = version;
        reject(bad);
        auto legacy = original;
        legacy.schema_version = version;
        legacy.transforms.push_back({});
        expect_invalid([&] { (void)sketch::replay_boundary_construction(legacy); },
                       "legacy records cannot carry transform frames");
    }
    bad = encoded;
    bad["version"] = 999;
    bad["transforms"] = "future";
    const auto opaque = sketch::decode_boundary_receipt_envelope(bad);
    require(!opaque.supported() && *opaque.original_envelope == bad,
            "future transform schemas must remain opaque");
    bad = encoded;
    bad["replay_version"] = 999;
    bad.erase("transforms");
    require(!sketch::decode_boundary_receipt_envelope(bad).supported(),
            "future replay dialect must remain opaque before frame parsing");
    for (const auto transform : {
             sketch::PlanarTransform{{std::numeric_limits<double>::infinity(), 0}, 0, false, false, {}},
             sketch::PlanarTransform{{}, std::numeric_limits<double>::quiet_NaN(), false, false, {}},
             sketch::PlanarTransform{{}, 0, false, false, {0, std::numeric_limits<double>::infinity()}},
             sketch::PlanarTransform{{}, 0, false, false, {std::numeric_limits<double>::max(), 0}}}) {
        expect_invalid([&] { (void)sketch::transformed_boundary_construction(original, transform); },
                       "nonfinite frames and finite frames collapsing geometry must reject");
    }
    expect_invalid([&] { (void)sketch::translated_boundary_construction(framed, {}); },
                   "legacy translation must reject schema three with a diagnostic");
    expect_invalid([&] {
        (void)sketch::transformed_boundary_construction(original, {}, {{"v0", "v1"}});
    }, "transform reidentification must reject vertex collisions");
    expect_invalid([&] {
        (void)sketch::transformed_boundary_construction(original, {}, {{original.boundary_id, ""}});
    }, "transform reidentification must reject empty identities");
    auto invalid_source = original;
    invalid_source.edges[0].receipt.segment_id = "wrong";
    expect_invalid([&] {
        (void)sketch::transformed_boundary_construction(
            invalid_source, {}, {{"wrong", original.edges[0].segment_id}});
    }, "transforms must validate the source before remapping can repair it");
}

void test_translation_preserves_inputs_and_remaps_only_typed_ids() {
    auto records = all_forms();
    // Point-native closing inputs avoid recomputing a retained closure vector
    // after floating-point translation of the tangent arc endpoint.
    auto& tangent_record = records.back();
    tangent_record.schema_version = sketch::boundary_receipt_schema_version_v2;
    auto& tangent_close = tangent_record.edges.back();
    tangent_close.receipt = line_to_point(tangent_close.segment_id,
                                         tangent_close.receipt.start, tangent_record.anchor);
    records.push_back(point_record());
    const Vec2 offset{8.0, -4.0};
    for (auto& record : records) {
        record.extensions["identity"] = record.boundary_id;
        record.extensions["nested"] = Json{{"segment_id", record.edges[0].segment_id}};
        const auto original = record;
        std::map<std::string, std::string, std::less<>> identities;
        identities[record.boundary_id] = "copy-" + record.boundary_id;
        for (const auto& edge : record.edges) {
            for (const auto& id : {edge.segment_id, edge.start_vertex_id, edge.end_vertex_id}) {
                identities[id] = "copy-" + id;
            }
        }
        const auto translated = sketch::translated_boundary_construction(record, offset, identities);
        auto expected = original;
        expected.anchor = {original.anchor.x + offset.x, original.anchor.y + offset.y};
        expected.boundary_id = identities.at(original.boundary_id);
        for (auto& edge : expected.edges) {
            edge.segment_id = identities.at(edge.segment_id);
            edge.start_vertex_id = identities.at(edge.start_vertex_id);
            edge.end_vertex_id = identities.at(edge.end_vertex_id);
            edge.receipt.segment_id = identities.at(edge.receipt.segment_id);
            edge.receipt.start.x += offset.x;
            edge.receipt.start.y += offset.y;
            if (edge.receipt.chord_end) {
                edge.receipt.chord_end->x += offset.x;
                edge.receipt.chord_end->y += offset.y;
            }
        }
        require(translated == expected,
                "translation must retain every expression, closure vector and opaque extension");
        require(record == original, "translation must leave the source untouched");
        const auto before = sketch::replay_boundary_construction(original);
        const auto after = sketch::replay_boundary_construction(translated);
        for (std::size_t index = 0; index < before.edges.size(); ++index) {
            const auto& a = before.edges[index].segment;
            const auto& b = after.edges[index].segment;
            require(std::abs(b.start.x - (a.start.x + offset.x)) < 1e-7 &&
                        std::abs(b.start.y - (a.start.y + offset.y)) < 1e-7 &&
                        std::abs(b.end.x - (a.end.x + offset.x)) < 1e-7 &&
                        std::abs(b.end.y - (a.end.y + offset.y)) < 1e-7 &&
                        std::abs(a.sweep_radians - b.sweep_radians) < 1e-12,
                    "replayed analytical geometry must translate without changing shape");
        }
        require(sketch::translated_boundary_construction(original, {}) == original,
                "zero translation without replacements must preserve identities and input exactly");
    }
}

void test_translation_rejects_invalid_inputs_and_results() {
    const auto original = all_forms().front();
    const auto tangent_closure = all_forms().back();
    expect_invalid([&] {
        (void)sketch::translated_boundary_construction(tangent_closure, {8, -4});
    }, "translation must reject rounding that invalidates an exact retained closure vector");
    for (const auto offset : {Vec2{std::numeric_limits<double>::infinity(), 0},
                              Vec2{0, std::numeric_limits<double>::quiet_NaN()},
                              Vec2{std::numeric_limits<double>::max(), 0}}) {
        expect_invalid([&] { (void)sketch::translated_boundary_construction(original, offset); },
                       "nonfinite offsets and finite offsets destroying replay must reject");
    }
    auto invalid_source = original;
    invalid_source.edges[0].receipt.segment_id = "wrong";
    expect_invalid([&] {
        (void)sketch::translated_boundary_construction(
            invalid_source, {}, {{"wrong", original.edges[0].segment_id}});
    }, "translation must validate the source before replacements can repair it");
    expect_invalid([&] {
        (void)sketch::translated_boundary_construction(original, {}, {{original.boundary_id, ""}});
    }, "empty replacement identities must reject");
    expect_invalid([&] {
        (void)sketch::translated_boundary_construction(
            original, {}, {{original.edges[0].segment_id, original.edges[1].segment_id}});
    }, "replacement edge collisions must reject");
    expect_invalid([&] {
        (void)sketch::translated_boundary_construction(original, {}, {{"v0", "v1"}});
    }, "replacement vertex collisions must reject");
    require(original == all_forms().front(), "failed translations must leave the source untouched");
}

void test_roundtrip_all_forms_and_exact_values() {
    const auto records = all_forms();
    require(records.size() == 8, "all eight construction forms must be represented");
    for (const auto& record : records) {
        const auto replay = sketch::replay_boundary_construction(record);
        require(replay.edges.size() == record.edges.size(), "replay must retain edge count");
        const auto encoded = sketch::encode_boundary_receipt_envelope(record);
        const auto decoded = sketch::decode_boundary_receipt_envelope(encoded);
        require(decoded.supported() && decoded.record.has_value(),
                "known receipt envelope must decode as supported");
        require(*decoded.record == record, "receipt envelope must roundtrip exactly");
    }

    const auto exact = records[0].edges[0].receipt;
    require(exact.distance->exact_metres == sketch::ExactRational{1, 1},
            "exact quantity numerator/denominator must survive");
    require(exact.distance->original_expression == "1 m",
            "quantity original expression must survive");
    require(exact.heading->original_expression == "0 deg" &&
                exact.heading->normalized_expression == "0",
            "angle expressions must survive normalized form");

    const auto custom = line_heading("exact-custom", {0, 0}, "1 1/2 ft", "pi/2");
    require(sketch::normalize_exact_quantity(*custom.distance).exact_metres ==
                sketch::parse_quantity("1 1/2 ft").exact_metres,
            "mixed-unit exact quantity must remain exact");
    require(sketch::normalize_exact_angle(*custom.heading).radians ==
                std::numbers::pi / 2.0,
            "pi angle must retain its exact parsed radians");
    require(sketch::parse_angle("-1/2 rad").radians == -0.5,
            "signed fractional radians must remain supported");
    require(sketch::parse_angle("180 deg").radians == std::numbers::pi,
            "degree angle input must remain supported");
    expect_invalid([&] { (void)sketch::parse_angle("1e-400 rad"); },
                   "underflowing angle input must reject");
    require(sketch::parse_angle(" + 2 * pi / 4 rad ").radians == std::numbers::pi / 2.0,
            "whitespace around angle operators must remain supported");
    for (const auto* expression : {"1 1/2 rad", "1 2 deg", "1 e3 rad", "0 .5 rad"}) {
        expect_invalid([&] { (void)sketch::parse_angle(expression); },
                       "whitespace cannot concatenate separate numeric tokens");
    }
}

void test_point_native_schema_two_roundtrip_and_exact_endpoint_copy() {
    const auto record = point_record();
    const auto replay = sketch::replay_boundary_construction(record);
    const Vec2 expected[]{{123456789.125, -987654321.75},
                          {123456792.625, -987654321.75},
                          {123456792.625, -987654318.25},
                          {123456789.125, -987654318.25}};
    require(record.schema_version == sketch::boundary_receipt_schema_version_v2,
            "point-native records must use schema version two");
    require(replay.edges.size() == 4, "point-native replay must retain all edges");
    for (std::size_t index = 0; index < replay.edges.size(); ++index) {
        require(replay.edges[index].segment.start.x == expected[index].x &&
                    replay.edges[index].segment.start.y == expected[index].y &&
                    replay.edges[index].segment.end.x == expected[(index + 1) % 4].x &&
                    replay.edges[index].segment.end.y == expected[(index + 1) % 4].y,
                "point-native replay must copy awkward endpoints exactly");
        const auto& receipt = record.edges[index].receipt;
        require(receipt.kind == BoundaryConstructionKind::line_to_point &&
                    receipt.chord_end.has_value() && !receipt.distance.has_value() &&
                    !receipt.heading.has_value() && !receipt.rise.has_value() &&
                    !receipt.run.has_value() && !receipt.turn.has_value() &&
                    !receipt.angle.has_value() && !receipt.height.has_value() &&
                    !receipt.arc_length.has_value() && !receipt.tangent.has_value() &&
                    !receipt.sweep.has_value() && !receipt.closure_delta.has_value() &&
                    !receipt.clockwise,
                "point-native receipt must carry only its endpoint input");
    }

    const auto encoded = sketch::encode_boundary_receipt_envelope(record);
    require(encoded.at("version") == 2 && encoded.at("replay_version") == 1,
            "point-native envelope must emit schema two with replay version one");
    const auto& encoded_receipt = encoded.at("segments").at(0).at("receipt");
    require(encoded_receipt.at("kind") == "line_to_point" &&
                !encoded_receipt.contains("distance") && !encoded_receipt.contains("heading") &&
                !encoded_receipt.contains("rise") && !encoded_receipt.contains("run") &&
                !encoded_receipt.contains("turn") && !encoded_receipt.contains("angle") &&
                !encoded_receipt.contains("height") && !encoded_receipt.contains("arc_length") &&
                !encoded_receipt.contains("tangent") && !encoded_receipt.contains("sweep") &&
                !encoded_receipt.contains("closure_delta"),
            "point-native JSON receipt must omit Quantity and Angle fields");
    const auto decoded = sketch::decode_boundary_receipt_envelope(encoded);
    require(decoded.supported() && decoded.record.has_value() && *decoded.record == record,
            "schema two point-native envelope must roundtrip exactly");
}

void test_schema_one_rejects_point_kind_and_replay_version_is_opaque_only_when_positive() {
    const auto point = point_record();
    auto schema_one = point;
    schema_one.schema_version = sketch::boundary_receipt_schema_version_v1;
    expect_invalid([&] { (void)sketch::replay_boundary_construction(schema_one); },
                   "schema one replay must reject the schema two point-native kind");
    expect_invalid([&] { (void)sketch::encode_boundary_receipt_envelope(schema_one); },
                   "schema one encoder must reject the schema two point-native kind");

    auto schema_one_json = sketch::encode_boundary_receipt_envelope(point);
    schema_one_json["version"] = sketch::boundary_receipt_schema_version_v1;
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(schema_one_json); },
                   "schema one decoder must reject the schema two point-native kind");

    auto unknown_replay = sketch::encode_boundary_receipt_envelope(point);
    unknown_replay["replay_version"] = 77;
    unknown_replay["future_replay_field"] = Json{{"opaque", true}};
    const auto opaque = sketch::decode_boundary_receipt_envelope(unknown_replay);
    require(!opaque.supported() && opaque.original_envelope.has_value() &&
                *opaque.original_envelope == unknown_replay && opaque.version == 2,
            "unknown positive replay versions must remain opaque");

    const auto sparse_unknown_replay =
        Json{{"version", 2}, {"replay_version", 77},
             {"future_payload", Json{{"opaque", true}}}};
    const auto sparse_opaque = sketch::decode_boundary_receipt_envelope(sparse_unknown_replay);
    require(!sparse_opaque.supported() && sparse_opaque.original_envelope.has_value() &&
                *sparse_opaque.original_envelope == sparse_unknown_replay &&
                sparse_opaque.version == 2,
            "future replay payloads must remain opaque without assuming known fields");

    auto missing = sketch::encode_boundary_receipt_envelope(point);
    missing.erase("replay_version");
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(missing); },
                   "missing replay version must reject as malformed");
    auto zero = sketch::encode_boundary_receipt_envelope(point);
    zero["replay_version"] = 0;
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(zero); },
                   "zero replay version must reject as malformed");
    auto text = sketch::encode_boundary_receipt_envelope(point);
    text["replay_version"] = "1";
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(text); },
                   "string replay version must reject as malformed");
}

void test_malformed_extra_missing_and_duplicate_fields_reject() {
    const auto original = sketch::encode_boundary_receipt_envelope(all_forms().front());

    auto missing_segments = original;
    missing_segments.erase("segments");
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(missing_segments); },
                   "missing top-level segments must reject");

    auto extra_top_level = original;
    extra_top_level["unexpected"] = true;
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(extra_top_level); },
                   "unknown top-level field must reject for known version");

    auto missing_receipt_field = original;
    missing_receipt_field["segments"][0]["receipt"].erase("heading");
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(missing_receipt_field); },
                   "missing kind-specific receipt field must reject");

    auto extra_receipt_field = original;
    extra_receipt_field["segments"][0]["receipt"]["future_action"] = 1;
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(extra_receipt_field); },
                   "unknown receipt field must reject for known version");

    auto duplicate_segment = original;
    duplicate_segment["segments"].push_back(duplicate_segment["segments"][0]);
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(duplicate_segment); },
                   "duplicate segment identity must reject");

    auto malformed_version = original;
    malformed_version["version"] = 0;
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(malformed_version); },
                   "zero schema version must reject");
    malformed_version["version"] = "1";
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(malformed_version); },
                   "string schema version must reject");

    auto malformed_quantity = original;
    malformed_quantity["segments"][0]["receipt"]["distance"]["exact_metres"]["denominator"] = 0;
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(malformed_quantity); },
                   "zero quantity denominator must reject");

    auto malformed_angle = original;
    malformed_angle["segments"][0]["receipt"]["heading"]["original_expression"] = "0.0001 rad";
    expect_invalid([&] { (void)sketch::decode_boundary_receipt_envelope(malformed_angle); },
                   "angle expression mismatch must reject");

}

void test_unknown_positive_version_is_opaque_and_preserved() {
    auto future = sketch::encode_boundary_receipt_envelope(all_forms().front());
    future["version"] = 99;
    future["new_top_level_field"] = Json{{"future", true}};
    const auto inspected = sketch::inspect_boundary_receipt_envelope(future);
    require(inspected.format == sketch::BoundaryReceiptEnvelopeFormat::unsupported_version &&
                inspected.version == 99,
            "positive future schema version must be reported unsupported");
    const auto decoded = sketch::decode_boundary_receipt_envelope(future);
    require(!decoded.supported() && decoded.original_envelope.has_value() &&
                *decoded.original_envelope == future && decoded.version == 99,
            "unsupported envelope must preserve original JSON exactly");
}

void test_replay_tampering_and_context_fail_atomically() {
    const auto original = all_forms().front();

    auto bad_quantity = original;
    bad_quantity.edges[0].receipt.distance->metres += 0.01;
    expect_invalid([&] { (void)sketch::replay_boundary_construction(bad_quantity); },
                   "tampered quantity value must reject replay");

    auto bad_anchor = original;
    bad_anchor.anchor.x += 0.01;
    expect_invalid([&] { (void)sketch::replay_boundary_construction(bad_anchor); },
                   "tampered anchor must reject replay");

    auto bad_start = original;
    bad_start.edges[1].receipt.start.x += 0.01;
    expect_invalid([&] { (void)sketch::replay_boundary_construction(bad_start); },
                   "tampered captured start must reject replay");

    auto bad_topology = original;
    bad_topology.edges[1].start_vertex_id = "unjoined";
    expect_invalid([&] { (void)sketch::replay_boundary_construction(bad_topology); },
                   "broken topology join must reject replay");

    ConstructionReceipt heading = line_heading("single", {0, 0}, "1 m", "0 deg");
    expect_invalid(
        [&] {
            (void)sketch::replay_construction_receipt(
                heading, {Vec2{0, 0}, Segment{{0, 0}, {1, 0}, 0.0}, std::nullopt, 1e-7});
        },
        "irrelevant previous context must reject");

    ConstructionReceipt relative = relative_turn("relative", {1, 0}, "1 m", "90 deg");
    expect_invalid(
        [&] {
            (void)sketch::replay_construction_receipt(
                relative, {Vec2{1, 0}, std::nullopt, std::nullopt, 1e-7});
        },
        "relative receipt without previous context must reject");

    ConstructionReceipt close = closure("close", {1, 0}, {-1, 0});
    expect_invalid(
        [&] {
            (void)sketch::replay_construction_receipt(
                close, {Vec2{1, 0}, std::nullopt, std::nullopt, 1e-7});
        },
        "closure receipt without anchor context must reject");
}

void test_single_receipt_codec_preserves_open_inputs_and_rejects_loss() {
    const auto relative = relative_turn("open-relative", {1, 0}, "1/3 ft", "90 deg");
    const auto encoded = sketch::encode_construction_receipt(relative);
    require(sketch::decode_construction_receipt(Json::parse(encoded.dump())) == relative,
            "an open relative receipt must encode without a fabricated boundary or replay context");
    const auto rebuilt = sketch::replay_construction_receipt(
        relative, {Vec2{1, 0}, Segment{{0, 0}, {1, 0}, 0.0}, std::nullopt, 1e-7});
    require(rebuilt.receipt == relative,
            "semantic context must remain the separate authority for single-receipt replay");

    auto extra = relative;
    extra.height = q("2 m");
    expect_invalid([&] { (void)sketch::encode_construction_receipt(extra); },
                   "single-receipt encoding must not silently drop an extraneous typed input");
    auto invalid_identity = relative;
    invalid_identity.segment_id.clear();
    expect_invalid([&] { (void)sketch::encode_construction_receipt(invalid_identity); },
                   "single-receipt encoding must reject an invalid stable identity");
    auto extra_json = encoded;
    extra_json["future_input"] = 1;
    expect_invalid([&] { (void)sketch::decode_construction_receipt(extra_json); },
                   "known single-receipt fields must remain strict");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_roundtrip_all_forms_and_exact_values();
        test_transform_frames_preserve_local_inputs_and_compose();
        test_transform_frame_validation();
        test_translation_preserves_inputs_and_remaps_only_typed_ids();
        test_translation_rejects_invalid_inputs_and_results();
        test_point_native_schema_two_roundtrip_and_exact_endpoint_copy();
        test_schema_one_rejects_point_kind_and_replay_version_is_opaque_only_when_positive();
        test_malformed_extra_missing_and_duplicate_fields_reject();
        test_unknown_positive_version_is_opaque_and_preserved();
        test_replay_tampering_and_context_fail_atomically();
        test_single_receipt_codec_preserves_open_inputs_and_rejects_loss();
        std::cout << "Boundary receipt tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
