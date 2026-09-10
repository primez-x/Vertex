#include "sketch/boundary_entity.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using json = nlohmann::json;
using sketch::BoundaryEntityFormat;
using sketch::Entity;
using sketch::IdentifiedBoundary;
using sketch::IdentifiedSegment;
using sketch::LegacyBoundaryIdentityOptions;
using sketch::Segment;
using sketch::Vec2;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "boundary_entity_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    fail(message);
}

Segment line(Vec2 start, Vec2 end) { return Segment{start, end, 0.0}; }
bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

IdentifiedBoundary rectangle_model(std::string type = "measurement_boundary") {
    return IdentifiedBoundary{
        .id = "boundary-1",
        .type = std::move(type),
        .segments = {
            {"segment-a", "vertex-a", "vertex-b", line({0.0, 0.0}, {4.0, 0.0})},
            {"segment-b", "vertex-b", "vertex-c", line({4.0, 0.0}, {4.0, 3.0})},
            {"segment-c", "vertex-c", "vertex-d", line({4.0, 3.0}, {0.0, 3.0})},
            {"segment-d", "vertex-d", "vertex-a", line({0.0, 3.0}, {0.0, 0.0})},
        },
    };
}

void test_lines_and_analytic_arcs_round_trip() {
    auto rectangle = rectangle_model();
    const auto entity = sketch::encode_identified_boundary_entity(rectangle);
    require(entity.id == rectangle.id, "entity id must be retained");
    require(entity.type == rectangle.type, "entity type must be retained");
    require(entity.properties.at("boundary_model_version") == 1,
            "identified boundary version must be one");
    require(entity.properties.at("segments").size() == 4,
            "all line segments must be encoded");
    require(entity.properties.at("segments").at(0).at("segment_id") == "segment-a",
            "segment id must be encoded");
    require(entity.properties.at("segments").at(0).at("start_vertex_id") == "vertex-a",
            "start vertex id must be encoded");
    require(entity.properties.at("segments").at(0).at("end_vertex_id") == "vertex-b",
            "end vertex id must be encoded");
    require(entity.properties.at("segments").at(0).at("sweep_radians") == 0.0,
            "line sweep must be encoded explicitly");

    const auto decoded = sketch::decode_identified_boundary_entity(entity);
    require(decoded == rectangle, "line boundary must decode exactly");

    const auto arc = sketch::arc_from_chord_angle({-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi);
    const IdentifiedBoundary with_arc{
        .id = "boundary-arc",
        .type = "room_boundary",
        .segments = {
            {"arc", "a", "b", arc},
            {"chord", "b", "a", line({1.0, 0.0}, {-1.0, 0.0})},
        },
    };
    const auto arc_entity = sketch::encode_identified_boundary_entity(with_arc);
    const auto arc_decoded = sketch::decode_identified_boundary_entity(arc_entity);
    require(arc_decoded == with_arc, "analytic arc must round-trip exactly");
    require(arc_decoded.segments.front().segment.sweep_radians == std::numbers::pi,
            "arc sweep must remain analytical");
}

void test_metadata_preserving_update_is_keyed_by_stable_segment_id() {
    auto original = sketch::encode_identified_boundary_entity(rectangle_model());
    original.required = true;
    original.properties["classification"] = "living";
    original.properties["future_property"] = {{"keep", true}};
    original.extensions["future_extension"] = {1, "opaque", false};
    original.properties["segments"][0]["future_segment_metadata"] = {{"slot", "first"}};

    auto changed = rectangle_model();
    changed.segments[0].segment.end.x = 4.5;
    changed.segments[1].segment.start.x = 4.5;
    changed.segments[1].segment.end.x = 4.0;
    std::rotate(changed.segments.begin(), changed.segments.begin() + 1, changed.segments.end());
    const auto merged = sketch::encode_identified_boundary_entity(changed, &original);
    require(merged.required, "entity required flag must be preserved");
    require(merged.properties.at("classification") == "living",
            "unrelated properties must be preserved");
    require(merged.properties.at("future_property") == original.properties.at("future_property"),
            "future properties must be preserved");
    require(merged.extensions == original.extensions,
            "entity extensions must be preserved");
    require(merged.properties.at("segments").at(3).at("future_segment_metadata") ==
                original.properties.at("segments").at(0).at("future_segment_metadata"),
            "per-segment metadata must follow stable segment id");
    require(merged.properties.at("segments").at(3).at("start").at(0) == 0.0,
            "canonical geometry must be updated");
    require(merged.properties.at("segments").at(3).at("end").at(0) == 4.5,
            "canonical geometry update must not be shadowed by metadata");
}

void test_version_inspection_distinguishes_legacy_supported_and_future() {
    auto supported = sketch::encode_identified_boundary_entity(rectangle_model());
    const auto supported_info = sketch::inspect_boundary_entity_version(supported);
    require(supported_info.format == BoundaryEntityFormat::identified_v1,
            "v1 boundary must be identified");
    require(supported_info.version == std::uint64_t{1},
            "supported version must be exposed");

    auto legacy = supported;
    legacy.properties.erase("boundary_model_version");
    for (auto& segment : legacy.properties.at("segments")) {
        segment.erase("segment_id");
        segment.erase("start_vertex_id");
        segment.erase("end_vertex_id");
    }
    const auto legacy_info = sketch::inspect_boundary_entity_version(legacy);
    require(legacy_info.format == BoundaryEntityFormat::anonymous_legacy,
            "anonymous legacy boundary must be classified separately");
    require(!legacy_info.version.has_value(), "legacy boundary has no version");
    auto vendor_legacy = legacy;
    vendor_legacy.properties["segments"][0]["segment_id"] = "vendor-opaque-token";
    require(sketch::inspect_boundary_entity_version(vendor_legacy).format ==
                BoundaryEntityFormat::anonymous_legacy,
            "unversioned legacy metadata must not silently acquire identity semantics");
    rejected([&] { (void)sketch::upgrade_legacy_boundary_entity(vendor_legacy); },
             "upgrade must not overwrite colliding legacy metadata");

    auto future = supported;
    future.properties["boundary_model_version"] = 9;
    const auto future_info = sketch::inspect_boundary_entity_version(future);
    require(future_info.format == BoundaryEntityFormat::unsupported_version,
            "future boundary version must be opaque");
    require(future_info.version == std::uint64_t{9},
            "future boundary version must be exposed");
    require(future_info.diagnostic.find("unsupported") != std::string::npos,
            "future version must provide a diagnostic");
    rejected([&] { (void)sketch::decode_identified_boundary_entity(future); },
             "typed mutation must reject an unsupported version");
}

void test_strict_decode_rejects_missing_wrong_and_malformed_fields() {
    const auto base = sketch::encode_identified_boundary_entity(rectangle_model());
    const std::vector<std::pair<std::string, json>> malformed = {
        {"missing version", [&] { auto value = base.properties; value.erase("boundary_model_version"); return value; }()},
        {"wrong version type", [&] { auto value = base.properties; value["boundary_model_version"] = "1"; return value; }()},
        {"missing segments", [&] { auto value = base.properties; value.erase("segments"); return value; }()},
        {"segments not array", [&] { auto value = base.properties; value["segments"] = json::object(); return value; }()},
        {"segment not object", [&] { auto value = base.properties; value["segments"][0] = 3; return value; }()},
        {"missing segment id", [&] { auto value = base.properties; value["segments"][0].erase("segment_id"); return value; }()},
        {"missing start vertex id", [&] { auto value = base.properties; value["segments"][0].erase("start_vertex_id"); return value; }()},
        {"missing end vertex id", [&] { auto value = base.properties; value["segments"][0].erase("end_vertex_id"); return value; }()},
        {"missing start", [&] { auto value = base.properties; value["segments"][0].erase("start"); return value; }()},
        {"start wrong length", [&] { auto value = base.properties; value["segments"][0]["start"] = {0.0}; return value; }()},
        {"end wrong type", [&] { auto value = base.properties; value["segments"][0]["end"] = "point"; return value; }()},
        {"missing sweep", [&] { auto value = base.properties; value["segments"][0].erase("sweep_radians"); return value; }()},
        {"sweep wrong type", [&] { auto value = base.properties; value["segments"][0]["sweep_radians"] = "0"; return value; }()},
        {"nonfinite coordinate", [&] { auto value = base.properties; value["segments"][0]["start"][0] = std::numeric_limits<double>::infinity(); return value; }()},
        {"nonfinite sweep", [&] { auto value = base.properties; value["segments"][0]["sweep_radians"] = std::numeric_limits<double>::quiet_NaN(); return value; }()},
    };
    for (const auto& [name, properties] : malformed) {
        auto entity = base;
        entity.properties = properties;
        rejected([&] { (void)sketch::decode_identified_boundary_entity(entity); }, name);
    }
}

void test_duplicate_ids_and_inconsistent_repeated_vertices_reject() {
    auto entity = sketch::encode_identified_boundary_entity(rectangle_model());
    entity.properties["segments"][1]["segment_id"] =
        entity.properties["segments"][0]["segment_id"];
    rejected([&] { (void)sketch::decode_identified_boundary_entity(entity); },
             "duplicate segment ids must reject");

    entity = sketch::encode_identified_boundary_entity(rectangle_model());
    entity.properties["segments"][1]["start_vertex_id"] = "vertex-a";
    rejected([&] { (void)sketch::decode_identified_boundary_entity(entity); },
             "non-adjacent endpoint identity misuse must reject");

    entity = sketch::encode_identified_boundary_entity(rectangle_model());
    entity.properties["segments"][1]["start"] = {4.125, 0.0};
    rejected([&] { (void)sketch::decode_identified_boundary_entity(entity); },
             "repeated vertex id with inconsistent coordinates must reject");

    entity = sketch::encode_identified_boundary_entity(rectangle_model());
    entity.properties["segments"][0]["segment_id"] = "bad/id";
    rejected([&] { (void)sketch::decode_identified_boundary_entity(entity); },
             "document-incompatible segment ids must reject");
}

void test_disconnected_open_and_self_intersecting_cycles_reject() {
    auto model = rectangle_model();
    model.segments[1].segment.start = {8.0, 0.0};
    rejected([&] { (void)sketch::encode_identified_boundary_entity(model); },
             "disconnected cycle must reject");

    model = rectangle_model();
    model.segments[3].end_vertex_id = "vertex-z";
    rejected([&] { (void)sketch::encode_identified_boundary_entity(model); },
             "open identity cycle must reject");

    model.segments[3].end_vertex_id = "vertex-a";
    model.segments[2].segment = line({4.0, 3.0}, {0.0, 0.0});
    model.segments[2].end_vertex_id = "vertex-a";
    model.segments[3].start_vertex_id = "vertex-a";
    model.segments[3].segment = line({0.0, 0.0}, {0.0, 0.0});
    rejected([&] { (void)sketch::encode_identified_boundary_entity(model); },
             "degenerate or self-intersecting cycle must reject");
}

void test_legacy_upgrade_preserves_metadata_and_never_hashes_coordinates() {
    auto identified = sketch::encode_identified_boundary_entity(rectangle_model());
    auto legacy = identified;
    legacy.properties.erase("boundary_model_version");
    legacy.properties["future_property"] = {{"preserve", "yes"}};
    legacy.extensions["future_extension"] = {{"keep", 7}};
    for (auto& segment : legacy.properties.at("segments")) {
        segment.erase("segment_id");
        segment.erase("start_vertex_id");
        segment.erase("end_vertex_id");
        segment["vendor_data"] = {"untouched"};
    }
    const auto upgraded = sketch::upgrade_legacy_boundary_entity(
        legacy, LegacyBoundaryIdentityOptions{{"s0", "s1", "s2", "s3"},
                                              {"v0", "v1", "v2", "v3"}});
    require(upgraded.id == legacy.id, "legacy entity id must remain stable");
    require(upgraded.properties.at("future_property") ==
                legacy.properties.at("future_property"),
            "legacy unrelated properties must be preserved");
    require(upgraded.extensions == legacy.extensions,
            "legacy extensions must be preserved");
    require(upgraded.properties.at("boundary_model_version") == 1,
            "legacy upgrade must produce identified v1");
    require(!upgraded.properties.contains("boundary"),
            "legacy geometry key must not remain as a second authority");
    require(upgraded.properties.at("segments").at(0).at("vendor_data") ==
                json({"untouched"}),
            "legacy per-segment metadata must survive upgrade");
    const auto upgraded_model = sketch::decode_identified_boundary_entity(upgraded);
    require(upgraded_model.segments.at(0).segment.start.x == 0.0,
            "legacy coordinates must remain authoritative");
    require(upgraded_model.segments.at(0).segment_id == "s0" &&
                upgraded_model.segments.at(0).start_vertex_id == "v0",
            "supplied IDs must follow stored topology order");

    auto generated = legacy;
    const auto generated_entity = sketch::upgrade_legacy_boundary_entity(generated);
    const auto generated_model = sketch::decode_identified_boundary_entity(generated_entity);
    require(generated_model.segments.at(0).segment_id !=
                std::to_string(generated_model.segments.at(0).segment.start.x),
            "generated IDs must not be coordinate hashes");
    require(generated_model.segments.at(0).segment_id !=
                generated_model.segments.at(1).segment_id,
            "generated segment IDs must be unique");
    const auto independently_upgraded = sketch::decode_identified_boundary_entity(
        sketch::upgrade_legacy_boundary_entity(legacy));
    require(independently_upgraded.segments.front().segment_id != generated_model.segments.front().segment_id,
            "independent upgrades of identical coordinates must have independent identities");
}

void test_legacy_upgrade_rejects_ambiguity_and_tolerance_only_join() {
    auto identified = sketch::encode_identified_boundary_entity(rectangle_model());
    auto ambiguous = identified;
    ambiguous.properties.erase("boundary_model_version");
    ambiguous.properties["boundary"] = ambiguous.properties.at("segments");
    for (auto* key : {"segments", "boundary"}) {
        for (auto& segment : ambiguous.properties.at(key)) {
            segment.erase("segment_id");
            segment.erase("start_vertex_id");
            segment.erase("end_vertex_id");
        }
    }
    rejected([&] { (void)sketch::upgrade_legacy_boundary_entity(ambiguous); },
             "two canonical legacy arrays must be ambiguous");

    auto tolerance_only = identified;
    tolerance_only.properties.erase("boundary_model_version");
    for (auto& segment : tolerance_only.properties.at("segments")) {
        segment.erase("segment_id");
        segment.erase("start_vertex_id");
        segment.erase("end_vertex_id");
    }
    tolerance_only.properties["segments"][1]["start"][0] = 4.0 + 5e-8;
    rejected([&] { (void)sketch::upgrade_legacy_boundary_entity(tolerance_only); },
             "legacy upgrade must not silently snap tolerance-close joins");
}

void test_reversal_preserves_ids_and_double_reversal() {
    auto model = rectangle_model();
    model.segments[0].segment.sweep_radians = 0.25;
    // Use a curved cycle whose remaining edges close at the analytical arc end.
    model.segments[0].segment = sketch::arc_from_chord_angle(
        {0.0, 0.0}, {4.0, 0.0}, 0.25);
    const auto reversed = sketch::reverse_identified_boundary(model);
    require(reversed.id == model.id && reversed.type == model.type,
            "reversal must preserve entity identity");
    for (std::size_t index = 0; index < model.segments.size(); ++index) {
        const auto& before = model.segments[model.segments.size() - 1 - index];
        const auto& after = reversed.segments[index];
        require(after.segment_id == before.segment_id,
                "reversal must preserve segment IDs");
        require(after.start_vertex_id == before.end_vertex_id &&
                    after.end_vertex_id == before.start_vertex_id,
                "reversal must swap endpoint identities");
        require(same_point(after.segment.start, before.segment.end) &&
                    same_point(after.segment.end, before.segment.start) &&
                    after.segment.sweep_radians == -before.segment.sweep_radians,
                "reversal must reverse analytical direction");
    }
    require(sketch::reverse_identified_boundary(reversed) == model,
            "double reversal must restore the exact typed boundary");

    auto entity = sketch::encode_identified_boundary_entity(rectangle_model());
    entity.properties["segments"][0]["receipt"] = {{"version", 99}};
    rejected([&] { (void)sketch::reverse_identified_boundary_entity(entity); },
             "reversal must reject unsupported directional receipt state");
}

void test_no_anonymous_downgrade_and_bad_entity_envelopes() {
    const auto entity = sketch::encode_identified_boundary_entity(rectangle_model());
    require(!sketch::can_recognize_boundary_entity_type("future_boundary"),
            "unknown boundary type must not be recognized");
    auto wrong_type = entity;
    wrong_type.type = "wall";
    rejected([&] { (void)sketch::decode_identified_boundary_entity(wrong_type); },
             "non-boundary entity must reject");
    auto bad_extensions = entity;
    bad_extensions.extensions = json::array();
    rejected([&] { (void)sketch::decode_identified_boundary_entity(bad_extensions); },
             "non-object extensions must reject");
    auto bad_id = entity;
    bad_id.id = "bad/id";
    rejected([&] { (void)sketch::decode_identified_boundary_entity(bad_id); },
             "invalid entity id must reject");
    // The public API has no encoder from an identified model to an anonymous
    // entity; every encode operation writes the v1 identity envelope.
    require(sketch::encode_identified_boundary_entity(rectangle_model())
                .properties.contains("boundary_model_version"),
            "identified encoding must never downgrade to anonymous");
}

}  // namespace

int main() {
    test_lines_and_analytic_arcs_round_trip();
    test_metadata_preserving_update_is_keyed_by_stable_segment_id();
    test_version_inspection_distinguishes_legacy_supported_and_future();
    test_strict_decode_rejects_missing_wrong_and_malformed_fields();
    test_duplicate_ids_and_inconsistent_repeated_vertices_reject();
    test_disconnected_open_and_self_intersecting_cycles_reject();
    test_legacy_upgrade_preserves_metadata_and_never_hashes_coordinates();
    test_legacy_upgrade_rejects_ambiguity_and_tolerance_only_join();
    test_reversal_preserves_ids_and_double_reversal();
    test_no_anonymous_downgrade_and_bad_entity_envelopes();
    return 0;
}
