#include "sketch/boundary_dimension.hpp"
#include "sketch/measurement_linework.hpp"

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

void test_open_measured_stroke_dimensions_follow_replayed_stable_targets() {
    sketch::MeasurementLinework model;
    model.stroke_id = "measured-stroke";
    model.anchor = {-1, 0};
    sketch::ConstructionReceipt arc;
    arc.segment_id = "measured-arc";
    arc.kind = sketch::BoundaryConstructionKind::arc_chord_angle;
    arc.start = {-1, 0}; arc.chord_end = Vec2{1, 0};
    arc.angle = sketch::parse_angle("180 deg");
    sketch::ConstructionReceipt tail;
    tail.segment_id = "measured-tail";
    tail.kind = sketch::BoundaryConstructionKind::line_to_point;
    tail.start = {1, 0}; tail.chord_end = Vec2{3, 0};
    model.edges = {{arc.segment_id, "measured-start", "measured-join", arc},
                   {tail.segment_id, "measured-join", "measured-end", tail}};
    sketch::BoundaryGeometryEdit resize;
    resize.boundary_id = model.stroke_id;
    resize.kind = sketch::BoundaryGeometryEditKind::resize_segment;
    resize.target_id = tail.segment_id; resize.target_length_metres = 4;
    model = sketch::edited_measurement_linework(model, resize, sketch::parse_quantity("4 m"));
    model = sketch::transformed_measurement_linework(model, {{}, 0, false, false, {7, 11}});
    const Entity stroke{model.stroke_id, "measurement_linework",
        {{"model", sketch::encode_measurement_linework_model(model)}}, true};
    const auto original_stroke = stroke;
    const auto view = sketch::resolve_dimension_geometry_owner(stroke);
    require(view.id == stroke.id && view.type == stroke.type && view.segments.size() == 2 &&
                view.segments.back().end_vertex_id == "measured-end",
            "shared measured dimension adapter must retain the open terminal vertex identity");
    auto length = manual_dimension(arc.segment_id);
    length.boundary_id = model.stroke_id;
    try {
        const auto resolved = length.resolve(stroke);
        require(std::abs(resolved.segment_length() - std::numbers::pi) < 1e-12 &&
                    resolved.segment.sweep_radians == std::numbers::pi,
                "open measured arc dimension must use physical arc length rather than its chord");
        length.segment_id = tail.segment_id;
        const auto terminal = length.resolve(stroke);
        require(terminal.segment_length() == 4 && terminal.segment.end.x == 12 && terminal.segment.end.y == 11,
                "terminal edge dimension must follow retained IDs through edit and transform replay");
        auto angle = length;
        angle.kind = sketch::BoundaryDimensionKind::angle;
        angle.segment_id = arc.segment_id;
        angle.secondary_segment_id = tail.segment_id;
        angle.vertex_id = "measured-join";
        require(std::abs(angle.resolve(stroke).angle() - std::numbers::pi / 2) < 1e-12,
                "open measured angle must use outgoing tangents at the stable shared vertex");
        angle.vertex_id = "measured-start";
        rejected([&] { (void)angle.resolve(stroke); },
                 "existing edges without the selected shared vertex must reject");
        angle.vertex_id = "missing-vertex";
        rejected([&] { (void)angle.resolve(stroke); }, "missing measured angle vertex must reject");
        length.segment_id = "missing-edge";
        rejected([&] { (void)length.resolve(stroke); }, "missing measured segment must reject");
        require(stroke == original_stroke, "dimension resolution must preserve measured source JSON exactly");
        auto reflected = stroke;
        reflected.properties["model"] = sketch::encode_measurement_linework_model(
            sketch::transformed_measurement_linework(model, {{}, 0, true, false, {}}));
        length.segment_id = arc.segment_id;
        const auto reflected_arc = length.resolve(reflected);
        require(reflected_arc.segment.sweep_radians == -std::numbers::pi &&
                    std::abs(reflected_arc.segment_length() - std::numbers::pi) < 1e-12,
                "reflected measured arc must retain signed sweep and physical dimension length");
    } catch (const std::exception& error) {
        fail(std::string("supported open measured dimension resolution failed: ") + error.what());
    }
}

void test_measured_revisited_vertices_and_area_refusal() {
    sketch::MeasurementLinework model;
    model.stroke_id = "revisited";
    const std::vector<Vec2> points{{0, 0}, {2, 0}, {2, 2}, {2, 0}, {4, 0}};
    const std::vector<std::string> vertices{"v0", "v1", "v2", "v1", "terminal"};
    for (std::size_t i = 1; i < points.size(); ++i) {
        sketch::ConstructionReceipt receipt;
        receipt.segment_id = "e" + std::to_string(i);
        receipt.kind = sketch::BoundaryConstructionKind::line_to_point;
        receipt.start = points[i - 1]; receipt.chord_end = points[i];
        model.edges.push_back({receipt.segment_id, vertices[i - 1], vertices[i], receipt});
    }
    Entity stroke{model.stroke_id, "measurement_linework",
        {{"model", sketch::encode_measurement_linework_model(model)}}, true};
    auto angle = manual_dimension("e1");
    angle.boundary_id = model.stroke_id;
    angle.kind = sketch::BoundaryDimensionKind::angle;
    angle.secondary_segment_id = "e3"; angle.vertex_id = "v1";
    require(std::abs(angle.resolve(stroke).angle() - std::numbers::pi / 2) < 1e-12,
            "nonconsecutive measured edges must resolve a retained revisited shared vertex");
    auto area = manual_dimension(); area.boundary_id = model.stroke_id;
    area.kind = sketch::BoundaryDimensionKind::area; area.segment_id.clear();
    rejected([&] { (void)area.resolve(stroke); }, "open measured stroke must refuse area dimensions");
    // A closed retrace has no area/winding invariant, but remains measurable.
    model.edges.resize(2); model.closed = true;
    model.edges[1].end_vertex_id = "v0";
    model.edges[1].receipt.chord_end = Vec2{0, 0};
    stroke.properties["model"] = sketch::encode_measurement_linework_model(model);
    auto length = manual_dimension("e2"); length.boundary_id = model.stroke_id;
    require(length.resolve(stroke).segment_length() == 2,
            "closed zero-area retrace must retain measured segment semantics");
    rejected([&] { (void)area.resolve(stroke); }, "closed measured stroke must refuse area dimensions");
    stroke.properties["model"] = {{"version", 999}, {"opaque", {1, 2, 3}}};
    bool area_refused_first = false;
    try { (void)area.resolve(stroke); }
    catch (const std::invalid_argument& error) {
        area_refused_first = std::string(error.what()) == "area dimensions cannot target measured strokes";
    }
    require(area_refused_first, "future measured area target must reject before opaque model decoding");
    rejected([&] { (void)sketch::resolve_dimension_geometry_owner(stroke); },
             "future measured dimension geometry must remain unresolved");
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

    auto exterior_automatic = automatic;
    exterior_automatic.automatic_placement_version = 2;
    const auto exterior_entity = sketch::encode_boundary_dimension_entity(exterior_automatic);
    require(exterior_entity.properties.at("automatic_placement_version") == 2 &&
                sketch::decode_boundary_dimension_entity(exterior_entity).dimension == exterior_automatic,
            "exterior-aware automatic placement must round-trip as version two");

    auto automatic_wrong_version = automatic_entity;
    automatic_wrong_version.properties["automatic_placement_version"] = 3;
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
    future_kind.properties["dimension_kind"] = "future_kind";
    const auto kind_result = sketch::decode_boundary_dimension_entity(future_kind);
    require(!kind_result.supported() && kind_result.original_entity == future_kind &&
                kind_result.kind == "future_kind",
            "unsupported dimension kinds must remain opaque");
}

void test_angle_and_area_dimensions_are_semantic_kinds() {
    auto angle_entity = sketch::encode_boundary_dimension_entity(manual_dimension("segment-a"));
    angle_entity.properties["dimension_kind"] = "angle";
    angle_entity.properties["target"]["vertex_id"] = "vertex-a";
    angle_entity.properties["target"]["second_segment_id"] = "segment-d";
    const auto angle = sketch::decode_boundary_dimension_entity(angle_entity);
    require(angle.supported(), "angle dimension kind must decode as a supported semantic dimension");
    const auto angle_resolution = angle.dimension->resolve(
        sketch::encode_identified_boundary_entity(rectangle_model()));
    require(angle.dimension->kind == sketch::BoundaryDimensionKind::angle &&
                angle.dimension->vertex_id == "vertex-a" &&
                angle.dimension->secondary_segment_id == "segment-d" &&
                std::abs(angle_resolution.angle_radians - std::numbers::pi * 0.5) < 1e-12,
            "angle dimension must resolve the included angle at its stable vertex");

    auto area_entity = sketch::encode_boundary_dimension_entity(manual_dimension("segment-a"));
    area_entity.properties["dimension_kind"] = "area";
    area_entity.properties["target"].erase("segment_id");
    const auto area = sketch::decode_boundary_dimension_entity(area_entity);
    require(area.supported(), "area dimension kind must decode as a supported semantic dimension");
    const auto area_resolution = area.dimension->resolve(
        sketch::encode_identified_boundary_entity(rectangle_model()));
    require(area.dimension->kind == sketch::BoundaryDimensionKind::area &&
                area.dimension->segment_id.empty() &&
                std::abs(area_resolution.area_square_metres - 12.0) < 1e-12,
            "area dimension must resolve the analytical boundary area");

    require(sketch::decode_boundary_dimension_entity(
                sketch::encode_boundary_dimension_entity(*angle.dimension))
                .dimension == angle.dimension,
            "angle dimension must round-trip through its canonical target fields");
    require(sketch::decode_boundary_dimension_entity(
                sketch::encode_boundary_dimension_entity(*area.dimension))
                .dimension == area.dimension,
            "area dimension must round-trip through its canonical target fields");

    auto bad_angle = angle_entity;
    bad_angle.properties["target"].erase("vertex_id");
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad_angle); },
             "angle dimension without a vertex must reject");
    auto bad_area = area_entity;
    bad_area.properties["target"]["segment_id"] = "segment-a";
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad_area); },
             "area dimension with a segment target must reject");
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

void test_presentation_versioning_round_trip_and_metadata() {
    auto model = manual_dimension();
    auto original = sketch::encode_boundary_dimension_entity(model);
    require(original.properties.at("dimension_version") == 1 && !original.properties.contains("presentation"),
            "absent presentation must preserve version one encoding");
    original.required = true;
    original.properties["vendor"] = json{{"number",1.0}};
    original.properties["target"]["vendor"] = "keep";
    original.extensions["vendor"] = json{{"number",1.0}};
    model.presentation = sketch::BoundaryDimensionPresentation{4.5,"#Ab09fF",true,true,false,-0.37};
    const auto upgraded = sketch::encode_boundary_dimension_entity(model, &original);
    require(upgraded.properties.at("dimension_version") == 2 &&
            sketch::inspect_boundary_dimension_version(upgraded).format == BoundaryDimensionFormat::supported_v2,
            "presentation must explicitly upgrade model version");
    const auto decoded = sketch::decode_boundary_dimension_entity(upgraded);
    require(decoded.supported() && decoded.version == 2 && decoded.dimension == model,
            "presentation must round-trip exactly with case-preserved color");
    require(upgraded.required && upgraded.properties.at("vendor").dump() == original.properties.at("vendor").dump() &&
            upgraded.properties.at("target").at("vendor") == "keep" &&
            upgraded.extensions.dump() == original.extensions.dump(), "presentation upgrade must preserve metadata");
    auto moved = *decoded.dimension;
    moved.text_position = sketch::transform_point(moved.text_position, {{1,2},0.7,true,false,{3,4}});
    const auto moved_entity = sketch::encode_boundary_dimension_entity(moved, &upgraded);
    require(sketch::decode_boundary_dimension_entity(moved_entity).dimension == moved &&
            moved_entity.properties.at("presentation").dump() == upgraded.properties.at("presentation").dump(),
            "dimension geometry transformations must preserve presentation");
    require(model.resolve(sketch::encode_identified_boundary_entity(rectangle_model())).segment_length() == 4,
            "presentation must not affect canonical length resolution");
    auto document = sketch::Document::create({sketch::encode_identified_boundary_entity(rectangle_model()), original});
    document.apply(sketch::ApplyEntityChanges{0,{sketch::EntityChange::upsert(upgraded)},{},"Style dimension"});
    const auto snapshot = document.snapshot();
    require(sketch::Document::fork(snapshot).snapshot().entities().at(model.id).properties.dump() == upgraded.properties.dump(),
            "document restoration must retain version two presentation");
    document.undo(document.revision());
    require(document.snapshot().entities().at(model.id).properties.dump() == original.properties.dump(),
            "style undo must restore exact version one properties");
    document.redo(document.revision());
    require(document.snapshot().entities().at(model.id).properties.dump() == upgraded.properties.dump(),
            "style redo must restore exact version two properties");
    auto downgraded = model;
    downgraded.presentation.reset();
    require(!(downgraded == model), "presentation must participate in model equality");
    rejected([&] { (void)sketch::encode_boundary_dimension_entity(downgraded, &upgraded); },
             "encoder cannot silently downgrade version two");
    for (const auto& opaque : {json("vendor style"),json::object(),upgraded.properties.at("presentation")}) {
        auto collision = original;
        collision.properties["presentation"] = opaque;
        const auto legacy = sketch::decode_boundary_dimension_entity(collision);
        require(legacy.supported() && !legacy.dimension->presentation,
                "version one presentation-looking vendor property must remain opaque");
        const auto retained = sketch::encode_boundary_dimension_entity(*legacy.dimension, &collision);
        require(retained.properties.at("presentation").dump() == opaque.dump(),
                "version one encoding must retain colliding vendor property exactly");
        rejected([&] { (void)sketch::encode_boundary_dimension_entity(model, &collision); },
                 "upgrading may not reinterpret or overwrite even a matching vendor property");
    }
    auto future = upgraded;
    future.properties["dimension_version"] = 3;
    future.properties["presentation"] = "opaque future semantics";
    require(!sketch::decode_boundary_dimension_entity(future).supported() &&
            sketch::decode_boundary_dimension_entity(future).original_entity == future,
            "future presentation must remain fully opaque");
    future = upgraded;
    future.properties["dimension_kind"] = "future_kind";
    future.properties["presentation"] = "opaque future kind";
    require(!sketch::decode_boundary_dimension_entity(future).supported(),
            "future kinds must not acquire version two presentation semantics");
}

void test_presentation_validation_is_strict() {
    auto model = manual_dimension();
    model.presentation = sketch::BoundaryDimensionPresentation{};
    const auto base = sketch::encode_boundary_dimension_entity(model);
    require(base.properties.at("presentation").size() == 6,
            "version two presentation must contain exactly the documented fields");
    auto missing = base;
    missing.properties.erase("presentation");
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(missing); }, "version two must require presentation");
    for (const auto* key : {"text_height_mm","color","bold","italic","visible","rotation_radians"}) {
        auto bad = base;
        bad.properties["presentation"].erase(key);
        rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad); }, "missing presentation field must reject");
        bad = base;
        bad.properties["presentation"][key] = nullptr;
        rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad); }, "null presentation field must reject");
    }
    const std::vector<std::pair<std::string,json>> invalid_fields{
        {"text_height_mm",0.49},{"text_height_mm",20.01},{"text_height_mm","2.5"},
        {"text_height_mm",std::numeric_limits<double>::infinity()},
        {"rotation_radians",std::numeric_limits<double>::quiet_NaN()}, {"rotation_radians","0"},
        {"color","#123"},{"color","#12345678"},{"color","123456"},{"color","#12gg34"},
        {"color",4},{"bold",1},{"italic","false"},{"visible",0}};
    for (const auto& [key,value] : invalid_fields) {
        auto bad = base;
        bad.properties["presentation"][key] = value;
        rejected([&] { (void)sketch::decode_boundary_dimension_entity(bad); }, "invalid presentation field must reject");
    }
    auto extra = base;
    extra.properties["presentation"]["vendor"] = true;
    rejected([&] { (void)sketch::decode_boundary_dimension_entity(extra); }, "unknown presentation fields must reject");
    for (double height : {0.5,20.0}) {
        model.presentation->text_height_mm = height;
        require(sketch::decode_boundary_dimension_entity(sketch::encode_boundary_dimension_entity(model)).dimension == model,
                "inclusive text height endpoints must be valid");
    }
    for (double height : {0.0,20.01,std::numeric_limits<double>::infinity()}) {
        model.presentation->text_height_mm = height;
        rejected([&] { (void)sketch::encode_boundary_dimension_entity(model); }, "invalid typed text height must reject");
    }
    model.presentation = sketch::BoundaryDimensionPresentation{};
    model.presentation->color = "red";
    rejected([&] { (void)sketch::encode_boundary_dimension_entity(model); }, "invalid typed color must reject");
    model.presentation = sketch::BoundaryDimensionPresentation{};
    model.presentation->rotation_radians = std::numeric_limits<double>::infinity();
    rejected([&] { (void)sketch::encode_boundary_dimension_entity(model); }, "nonfinite typed rotation must reject");
}

}  // namespace

int main() {
    test_open_measured_stroke_dimensions_follow_replayed_stable_targets();
    test_measured_revisited_vertices_and_area_refusal();
    test_straight_resolution_derives_length_from_canonical_geometry();
    test_arc_resolution_uses_analytic_segment_length();
    test_reordering_and_geometry_edits_follow_stable_segment_id();
    test_round_trip_preserves_outer_and_nested_unknown_metadata();
    test_automatic_and_manual_placement_semantics_are_strict();
    test_malformed_envelopes_and_canonical_fields_reject();
    test_unknown_versions_and_kinds_are_explicitly_opaque();
    test_angle_and_area_dimensions_are_semantic_kinds();
    test_source_boundary_binding_errors_reject();
    test_encoder_rejects_invalid_models_and_preserves_identity();
    test_presentation_versioning_round_trip_and_metadata();
    test_presentation_validation_is_strict();
    return 0;
}
