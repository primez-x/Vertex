#include "sketch/boundary_dimension.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using sketch::BoundaryDimension;
using sketch::BoundaryDimensionFormat;
using sketch::BoundaryDimensionPlacement;
using sketch::Entity;
using sketch::IdentifiedBoundary;
using sketch::Segment;
using sketch::Vec2;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "boundary_dimension_tests: " << message << '\n';
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

IdentifiedBoundary rectangle_model() {
    return IdentifiedBoundary{
        .id = "boundary-1",
        .type = "measurement_boundary",
        .segments = {
            {"segment-a", "vertex-a", "vertex-b", line({0.0, 0.0}, {4.0, 0.0})},
            {"segment-b", "vertex-b", "vertex-c", line({4.0, 0.0}, {4.0, 3.0})},
            {"segment-c", "vertex-c", "vertex-d", line({4.0, 3.0}, {0.0, 3.0})},
            {"segment-d", "vertex-d", "vertex-a", line({0.0, 3.0}, {0.0, 0.0})},
        },
    };
}

BoundaryDimension manual_dimension(std::string segment_id = "segment-a") {
    return BoundaryDimension{
        .id = "dimension-1",
        .boundary_id = "boundary-1",
        .segment_id = std::move(segment_id),
        .text_position = {2.0, 0.75},
        .placement = BoundaryDimensionPlacement::manual,
        .automatic_placement_version = std::nullopt,
    };
}

void test_straight_resolution_derives_length_from_canonical_geometry() {
    const auto boundary = sketch::encode_identified_boundary_entity(rectangle_model());
    const auto encoded = sketch::encode_boundary_dimension_entity(
        manual_dimension("segment-b"));
    const auto decoded = sketch::decode_boundary_dimension_entity(encoded);
    require(decoded.supported() && decoded.dimension.has_value(),
            "known v1 dimension must decode as supported");

    const auto resolved = decoded.dimension->resolve(boundary);
    require(resolved.segment.start.x == 4.0 && resolved.segment.start.y == 0.0 &&
                resolved.segment.end.x == 4.0 && resolved.segment.end.y == 3.0,
            "resolution must return the canonical segment geometry");
    require(resolved.segment_length_metres == 3.0,
            "straight dimension must derive its independent expected length");
    require(sketch::resolve_boundary_dimension(*decoded.dimension, boundary)
                    .segment_length_metres == 3.0,
            "free resolve helper must use the same analytical result");
    require(!encoded.properties.contains("length_m") &&
                !encoded.properties.contains("segment_length_metres"),
            "dimension encoding must not store an authoritative measurement");
}

void test_arc_resolution_uses_analytic_segment_length() {
    const auto arc = sketch::arc_from_chord_angle(
        {-1.0, 0.0}, {1.0, 0.0}, std::numbers::pi);
    const IdentifiedBoundary boundary_model{
        .id = "boundary-arc",
        .type = "room_boundary",
        .segments = {
            {"arc-edge", "vertex-a", "vertex-b", arc},
            {"closing-edge", "vertex-b", "vertex-a", line({1.0, 0.0}, {-1.0, 0.0})},
        },
    };
    auto dimension = manual_dimension("arc-edge");
    dimension.boundary_id = boundary_model.id;
    const auto resolved = dimension.resolve(
        sketch::encode_identified_boundary_entity(boundary_model));
    require(resolved.segment.sweep_radians == std::numbers::pi,
            "arc resolution must retain the analytical sweep");
    require(std::abs(resolved.segment_length_metres - std::numbers::pi) < 1e-12,
            "arc dimension must derive the analytic arc length independently");
}

void test_reordering_and_geometry_edits_follow_stable_segment_id() {
    const auto original_model = rectangle_model();
    const auto original_boundary = sketch::encode_identified_boundary_entity(original_model);

    auto changed_model = original_model;
    changed_model.segments[0].segment.end = {5.0, 0.0};
    changed_model.segments[1].segment.start = {5.0, 0.0};
    changed_model.segments[1].segment.end = {5.0, 3.0};
    changed_model.segments[2].segment.start = {5.0, 3.0};
    std::rotate(changed_model.segments.begin(), changed_model.segments.begin() + 2,
                changed_model.segments.end());
    const auto changed_boundary = sketch::encode_identified_boundary_entity(
        changed_model, &original_boundary);

    auto dimension = manual_dimension();
    const auto resolved = dimension.resolve(changed_boundary);
    require(resolved.segment.start.x == 0.0 && resolved.segment.end.x == 5.0,
            "reordering must not change which stable segment is measured");
    require(resolved.segment_length_metres == 5.0,
            "derived length must update when canonical geometry changes");
}

void test_round_trip_preserves_outer_and_nested_unknown_metadata() {
    auto original = sketch::encode_boundary_dimension_entity(manual_dimension());
    original.required = true;
    original.properties["future_property"] = {{"keep", true}};
    original.extensions["future_extension"] = {1, "opaque", false};
    original.properties["target"]["future_target_field"] = {"retained", 7};

    auto changed = manual_dimension();
    changed.text_position = {9.0, 10.0};
    changed.segment_id = "segment-b";
    const auto merged = sketch::encode_boundary_dimension_entity(changed, &original);
    require(merged.required, "entity required flag must be preserved");
    require(merged.properties.at("future_property") == original.properties.at("future_property"),
            "unknown outer properties must be preserved");
    require(merged.extensions == original.extensions,
            "unknown entity extensions must be preserved");
    require(merged.properties.at("target").at("future_target_field") ==
                original.properties.at("target").at("future_target_field"),
            "unknown nested target fields must be preserved");
    require(merged.properties.at("target").at("entity_id") == "boundary-1" &&
                merged.properties.at("target").at("segment_id") == "segment-b",
            "canonical target binding must be updated deliberately");
    require(merged.properties.at("text_position") == json({9.0, 10.0}),
            "canonical text position must be updated deliberately");
    const auto round_trip = sketch::decode_boundary_dimension_entity(merged);
    require(round_trip.supported() && round_trip.dimension == changed,
            "merged canonical semantics must round-trip exactly");
}

void test_automatic_and_manual_placement_semantics_are_strict() {
    auto automatic = manual_dimension();
    automatic.placement = BoundaryDimensionPlacement::automatic;
    automatic.automatic_placement_version = 1;
    const auto automatic_entity = sketch::encode_boundary_dimension_entity(automatic);
    require(automatic_entity.properties.at("placement_origin") == "automatic" &&
                automatic_entity.properties.at("automatic_placement_version") == 1,
            "automatic placement must carry version one");
    require(sketch::decode_boundary_dimension_entity(automatic_entity).dimension == automatic,
            "automatic placement must decode exactly");

    auto manual_with_version = manual_dimension();
    manual_with_version.automatic_placement_version = 1;
    rejected([&] { (void)sketch::encode_boundary_dimension_entity(manual_with_version); },
             "manual model must reject a conflicting automatic placement version");

    auto automatic_wrong_version = automatic_entity;
    automatic_wrong_version.properties["automatic_placement_version"] = 2;
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(automatic_wrong_version); },
             "automatic placement must reject unsupported placement versions");

    auto manual_entity = sketch::encode_boundary_dimension_entity(manual_dimension());
    manual_entity.properties["automatic_placement_version"] = 1;
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(manual_entity); },
             "manual entity must reject an automatic placement field");
}

void test_malformed_envelopes_and_canonical_fields_reject() {
    const auto base = sketch::encode_boundary_dimension_entity(manual_dimension());
    const std::vector<std::pair<std::string, json>> malformed = {
        {"missing version", [&] { auto value = base.properties; value.erase("dimension_version"); return value; }()},
        {"version wrong type", [&] { auto value = base.properties; value["dimension_version"] = "1"; return value; }()},
        {"version zero", [&] { auto value = base.properties; value["dimension_version"] = 0; return value; }()},
        {"version negative", [&] { auto value = base.properties; value["dimension_version"] = -1; return value; }()},
        {"missing kind", [&] { auto value = base.properties; value.erase("dimension_kind"); return value; }()},
        {"kind wrong type", [&] { auto value = base.properties; value["dimension_kind"] = 1; return value; }()},
        {"missing target", [&] { auto value = base.properties; value.erase("target"); return value; }()},
        {"target wrong type", [&] { auto value = base.properties; value["target"] = 4; return value; }()},
        {"target missing entity id", [&] { auto value = base.properties; value["target"].erase("entity_id"); return value; }()},
        {"target missing segment id", [&] { auto value = base.properties; value["target"].erase("segment_id"); return value; }()},
        {"target wrong entity id type", [&] { auto value = base.properties; value["target"]["entity_id"] = 4; return value; }()},
        {"target bad segment id", [&] { auto value = base.properties; value["target"]["segment_id"] = "bad/id"; return value; }()},
        {"missing text position", [&] { auto value = base.properties; value.erase("text_position"); return value; }()},
        {"text position wrong type", [&] { auto value = base.properties; value["text_position"] = "point"; return value; }()},
        {"text position wrong count", [&] { auto value = base.properties; value["text_position"] = {1.0}; return value; }()},
        {"text position nonfinite", [&] { auto value = base.properties; value["text_position"] = {1.0, std::numeric_limits<double>::infinity()}; return value; }()},
        {"missing placement origin", [&] { auto value = base.properties; value.erase("placement_origin"); return value; }()},
        {"placement origin wrong type", [&] { auto value = base.properties; value["placement_origin"] = 1; return value; }()},
        {"placement origin unsupported", [&] { auto value = base.properties; value["placement_origin"] = "inferred"; return value; }()},
    };
    for (const auto& [name, properties] : malformed) {
        auto entity = base;
        entity.properties = properties;
        rejected([&] { (void)sketch::decode_boundary_dimension_entity(entity); }, name);
    }

    auto bad_extensions = base;
    bad_extensions.extensions = json::array();
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad_extensions); },
             "dimension extensions must be an object");
    auto bad_type = base;
    bad_type.type = "label";
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad_type); },
             "dimension entity type must be dimension");
    auto bad_id = base;
    bad_id.id = "bad/id";
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad_id); },
             "dimension entity id must use the document identifier grammar");
}

void test_unknown_versions_and_kinds_are_explicitly_opaque() {
    auto future = sketch::encode_boundary_dimension_entity(manual_dimension());
    future.properties["dimension_version"] = 99;
    const auto info = sketch::inspect_boundary_dimension_version(future);
    require(info.format == BoundaryDimensionFormat::unsupported_version &&
                info.version == std::uint64_t{99},
            "future dimension version must be distinguished by inspection");
    require(info.diagnostic.find("unsupported") != std::string::npos,
            "future dimension version must provide a diagnostic");
    const auto decoded = sketch::decode_boundary_dimension_entity(future);
    require(!decoded.supported() && decoded.original_entity == future &&
                decoded.version == std::uint64_t{99},
            "future dimension data must remain preserved and unrenderable as v1");
    rejected([&] {
        (void)sketch::encode_boundary_dimension_entity(manual_dimension(), &future);
    }, "encoding over an unsupported future dimension must reject");

    auto future_kind = sketch::encode_boundary_dimension_entity(manual_dimension());
    future_kind.properties["dimension_kind"] = "angle";
    const auto kind_result = sketch::decode_boundary_dimension_entity(future_kind);
    require(!kind_result.supported() && kind_result.original_entity == future_kind &&
                kind_result.kind == "angle",
            "unsupported dimension kinds must remain opaque");
}

void test_source_boundary_binding_errors_reject() {
    const auto boundary = sketch::encode_identified_boundary_entity(rectangle_model());
    auto dimension = manual_dimension();

    auto wrong_id = dimension;
    wrong_id.boundary_id = "other-boundary";
    rejected([&] { (void)wrong_id.resolve(boundary); },
             "dimension must reject a source entity with another stable id");

    auto wrong_type = boundary;
    wrong_type.type = "wall";
    rejected([&] { (void)dimension.resolve(wrong_type); },
             "dimension must reject a non-boundary source entity type");

    auto missing_edge = dimension;
    missing_edge.segment_id = "missing-edge";
    rejected([&] { (void)missing_edge.resolve(boundary); },
             "dimension must reject a missing stable segment id");

    auto legacy = boundary;
    legacy.properties.erase("boundary_model_version");
    rejected([&] { (void)dimension.resolve(legacy); },
             "dimension must reject an anonymous legacy source model");

    auto future = boundary;
    future.properties["boundary_model_version"] = 99;
    rejected([&] { (void)dimension.resolve(future); },
             "dimension must reject an unsupported source model version");
}

void test_encoder_rejects_invalid_models_and_preserves_identity() {
    auto invalid_id = manual_dimension();
    invalid_id.id = "bad/id";
    rejected([&] { (void)sketch::encode_boundary_dimension_entity(invalid_id); },
             "encoder must reject invalid dimension ids");
    auto invalid_position = manual_dimension();
    invalid_position.text_position.x = std::numeric_limits<double>::quiet_NaN();
    rejected([&] { (void)sketch::encode_boundary_dimension_entity(invalid_position); },
             "encoder must reject nonfinite text positions");

    auto original = sketch::encode_boundary_dimension_entity(manual_dimension());
    auto wrong_type = original;
    wrong_type.type = "label";
    rejected([&] {
        (void)sketch::encode_boundary_dimension_entity(manual_dimension(), &wrong_type);
    }, "encoder must reject an original entity with another type");
    auto wrong_id = original;
    wrong_id.id = "other-dimension";
    rejected([&] {
        (void)sketch::encode_boundary_dimension_entity(manual_dimension(), &wrong_id);
    }, "encoder must reject an original entity with another stable id");
}

}  // namespace

int main() {
    test_straight_resolution_derives_length_from_canonical_geometry();
    test_arc_resolution_uses_analytic_segment_length();
    test_reordering_and_geometry_edits_follow_stable_segment_id();
    test_round_trip_preserves_outer_and_nested_unknown_metadata();
    test_automatic_and_manual_placement_semantics_are_strict();
    test_malformed_envelopes_and_canonical_fields_reject();
    test_unknown_versions_and_kinds_are_explicitly_opaque();
    test_source_boundary_binding_errors_reject();
    test_encoder_rejects_invalid_models_and_preserves_identity();
    return 0;
}
