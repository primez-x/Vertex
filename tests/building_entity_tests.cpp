#include "sketch/architecture.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

using sketch::BuildingObject;
using sketch::Entity;
using sketch::GableRoof;
using sketch::SlopedRoofPanel;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(message) + ": expected "
                                 + std::to_string(expected) + ", got "
                                 + std::to_string(actual));
    }
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

template <typename Object>
void require_roundtrip(Object object, std::string_view type, std::string_view form,
                       double expected_volume) {
    using namespace sketch;
    const auto metadata = nlohmann::json{
        {"ui", {{"color", "red"}}},
        {"vendor_extension", {1, "future", true}},
    };
    const BuildingObject original = std::move(object);
    const auto entity = encode_building_entity(original, metadata);
    require(entity.type == type, "canonical entity type");
    require(entity.id == std::visit([](const auto& value) { return value.id; }, original),
            "entity id must come from the semantic object");
    require(entity.extensions == metadata, "metadata must be retained as extensions");
    require(entity.properties.is_object(), "building properties must be an object");
    require(entity.properties.at("version") == 1, "building schema version");
    require(entity.properties.at("form") == std::string(form),
            "building form discriminator");

    const auto decoded = decode_building_entity(entity);
    const auto canonical = encode_building_entity(decoded);
    require(canonical.type == entity.type, "roundtrip entity type");
    require(canonical.properties == entity.properties,
            "roundtrip properties must be canonical and deterministic");
    require(canonical.extensions.is_object() && canonical.extensions.empty(),
            "unrelated metadata is not part of semantic geometry");
    near(solid_volume(make_building_shape(decoded)), expected_volume, 1e-8,
         "roundtrip building solid volume");
}

void test_all_forms_roundtrip_to_canonical_entities() {
    using namespace sketch;
    require_roundtrip(
        RectangularColumn{
            .id = "column-rect",
            .base_center = {1.0, -2.0, 0.75},
            .width = 0.4,
            .depth = 0.6,
            .height = 3.0,
            .rotation_radians = 0.25,
        },
        "column", "rectangular_column", 0.4 * 0.6 * 3.0);
    require_roundtrip(
        CircularColumn{
            .id = "column-round",
            .base_center = {-3.0, 4.0, -0.5},
            .radius = 0.25,
            .height = 2.0,
        },
        "column", "circular_column", std::numbers::pi * 0.25 * 0.25 * 2.0);
    require_roundtrip(
        Beam{
            .id = "beam-1",
            .start = {-1.0, 2.0, 0.5},
            .end = {2.0, 6.0, 3.5},
            .up = {0.0, 0.0, 1.0},
            .width = 0.2,
            .depth = 0.3,
        },
        "beam", "straight_beam", std::sqrt(34.0) * 0.2 * 0.3);

    const StairFlight stairs{
        .id = "stair-1",
        .base_position = {1.0, 2.0, 0.5},
        .orientation_radians = 0.1,
        .riser_count = 4,
        .total_rise = 2.0,
        .going = 0.25,
        .width = 1.5,
        .top_landing = StairLanding{.depth = 0.5, .thickness = 0.2},
    };
    const double stair_riser = stairs.total_rise
        / static_cast<double>(stairs.riser_count);
    const double stair_volume = stairs.width * stairs.going * stair_riser
        * static_cast<double>(stairs.riser_count * (stairs.riser_count + 1)) / 2.0
        + stairs.width * stairs.top_landing->depth * stairs.top_landing->thickness;
    require_roundtrip(stairs, "stair", "straight_stair_flight", stair_volume);

    const Railing railing{
        .id = "railing-1",
        .base_position = {2.0, -1.0, 0.25},
        .orientation_radians = 0.2,
        .length = 3.0,
        .height = 1.1,
        .thickness = 0.08,
        .post_spacing = 0.9,
    };
    const auto railing_posts = std::floor((railing.length - 1e-7) /
                                          railing.post_spacing) + 2.0;
    const auto railing_volume = railing.length * railing.thickness * railing.thickness +
        railing_posts * railing.thickness * railing.thickness * railing.height;
    require_roundtrip(railing, "railing", "straight_railing", railing_volume);

    const SlopedRoofPanel panel{
        .id = "roof-shed",
        .base_position = {0.0, 0.0, 4.0},
        .orientation_radians = 0.15,
        .run = 4.0,
        .span = 3.0,
        .rise = 1.0,
        .pitch_radians = std::atan(1.0 / 4.0),
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const double panel_slope = panel.rise / panel.run;
    const double panel_volume = (panel.run + 2.0 * panel.overhang)
        * std::sqrt(1.0 + panel_slope * panel_slope)
        * (panel.span + 2.0 * panel.overhang) * panel.thickness;
    require_roundtrip(panel, "roof", "sloped_roof_panel", panel_volume);

    const SlopedRoofPanel flat_panel{
        .id = "roof-flat",
        .base_position = {-2.0, 1.0, 4.5},
        .orientation_radians = -0.2,
        .run = 4.0,
        .span = 3.0,
        .rise = 0.0,
        .pitch_radians = 0.0,
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const double flat_panel_volume = (flat_panel.run + 2.0 * flat_panel.overhang)
        * (flat_panel.span + 2.0 * flat_panel.overhang) * flat_panel.thickness;
    require_roundtrip(flat_panel, "roof", "sloped_roof_panel", flat_panel_volume);

    const GableRoof gable{
        .id = "roof-gable",
        .base_position = {0.0, 0.0, 4.0},
        .orientation_radians = std::numbers::pi / 6.0,
        .length = 5.0,
        .span = 4.0,
        .rise = 1.0,
        .pitch_radians = std::atan(1.0 / 2.0),
        .overhang = 0.2,
        .thickness = 0.1,
    };
    const double half_span = gable.span / 2.0;
    const double gable_slope = gable.rise / half_span;
    const double sloped_half_length = (half_span + gable.overhang)
        * std::sqrt(1.0 + gable_slope * gable_slope);
    const double ridge_trim = (gable.length + 2.0 * gable.overhang)
        * gable_slope * gable.thickness * gable.thickness;
    const double gable_volume = 2.0 * (gable.length + 2.0 * gable.overhang)
        * sloped_half_length * gable.thickness - ridge_trim;
    require_roundtrip(gable, "roof", "gable_roof", gable_volume);
    const sketch::HipRoof hip{"hip-codec", {1, 2, 3}, 0.3, 6, 4, 1, std::atan(0.5), 0.2, 0.1};
    require_roundtrip(hip, "roof", "hip_roof", 6.4 * 4.4 * 0.1 * std::sqrt(1.25));
}

void test_recognized_type_is_distinct_from_decode_success() {
    using namespace sketch;
    require(can_recognize_building_entity_type("column"),
            "column type should be recognized");
    require(can_recognize_building_entity_type("beam"),
            "beam type should be recognized");
    require(can_recognize_building_entity_type("stair"),
            "stair type should be recognized");
    require(can_recognize_building_entity_type("railing"),
            "railing type should be recognized");
    require(can_recognize_building_entity_type("roof"),
            "roof type should be recognized");
    require(!can_recognize_building_entity_type("future_building"),
            "unknown building type should not be recognized");

    const Entity malformed = Entity{
        .id = "column-bad",
        .type = "column",
        .properties = nlohmann::json::object(),
    };
    require(can_recognize_building_entity_type(malformed.type),
            "recognition must not imply decodability");
    rejected([&] { (void)decode_building_entity(malformed); },
             "recognized type with missing properties should fail decode");
}

void test_unknown_versions_forms_and_metadata_are_handled_strictly() {
    using namespace sketch;
    auto entity = encode_building_entity(RectangularColumn{
        .id = "column-strict",
        .base_center = {},
        .width = 0.4,
        .depth = 0.6,
        .height = 3.0,
        .rotation_radians = 0.0,
    });
    entity.properties["unrelated_property"] = {"preserve", 7, false};
    entity.extensions["unrelated_metadata"] = {{"future", true}};
    (void)decode_building_entity(entity);
    const auto canonical = encode_building_entity(decode_building_entity(entity));
    require(!canonical.properties.contains("unrelated_property"),
            "re-encoding must contain only canonical semantic fields");
    require(canonical.extensions.empty(), "decoder must ignore unrelated metadata");

    auto unknown_version = entity;
    unknown_version.properties["version"] = 2;
    rejected([&] { (void)decode_building_entity(unknown_version); },
             "unknown building schema version should fail");
    auto unknown_form = entity;
    unknown_form.properties["form"] = "future_column";
    rejected([&] { (void)decode_building_entity(unknown_form); },
             "unknown building form should fail");
    auto wrong_type = entity;
    wrong_type.type = "future_building";
    rejected([&] { (void)decode_building_entity(wrong_type); },
             "unknown building entity type should fail");

    auto wrong_vector = entity;
    wrong_vector.properties["base_center_m"] = {0.0, 0.0};
    rejected([&] { (void)decode_building_entity(wrong_vector); },
             "coordinate arrays must have exactly three entries");
    auto nan_dimension = entity;
    nan_dimension.properties["height_m"] = std::numeric_limits<double>::quiet_NaN();
    rejected([&] { (void)decode_building_entity(nan_dimension); },
             "non-finite dimensions should fail");
    auto missing_dimension = entity;
    missing_dimension.properties.erase("width_m");
    rejected([&] { (void)decode_building_entity(missing_dimension); },
             "missing required dimensions should fail");
}

void test_integer_stairs_optional_landing_and_geometry_validation() {
    using namespace sketch;
    auto stairs_entity = encode_building_entity(StairFlight{
        .id = "stair-strict",
        .base_position = {},
        .orientation_radians = 0.0,
        .riser_count = 4,
        .total_rise = 2.0,
        .going = 0.25,
        .width = 1.5,
        .top_landing = std::nullopt,
    });
    require(stairs_entity.properties.at("top_landing").is_null(),
            "missing landing must encode as explicit null");
    const auto decoded = decode_building_entity(stairs_entity);
    require(std::holds_alternative<StairFlight>(decoded),
            "stair form must decode to StairFlight");
    require(!std::get<StairFlight>(decoded).top_landing.has_value(),
            "null landing must decode as absent");

    auto float_count = stairs_entity;
    float_count.properties["riser_count"] = 4.0;
    rejected([&] { (void)decode_building_entity(float_count); },
             "stair riser count must be an integer JSON value");
    auto negative_count = stairs_entity;
    negative_count.properties["riser_count"] = -1;
    rejected([&] { (void)decode_building_entity(negative_count); },
             "negative stair riser count should fail");
    auto oversized_count = stairs_entity;
    oversized_count.properties["riser_count"] = 10001;
    rejected([&] { (void)decode_building_entity(oversized_count); },
             "oversized stair riser count should fail");
    auto malformed_landing = stairs_entity;
    malformed_landing.properties["top_landing"] = 0.25;
    rejected([&] { (void)decode_building_entity(malformed_landing); },
             "landing must be null or an object");

    auto invalid_pitch = encode_building_entity(SlopedRoofPanel{
        .id = "roof-invalid",
        .base_position = {},
        .orientation_radians = 0.0,
        .run = 4.0,
        .span = 3.0,
        .rise = 1.0,
        .pitch_radians = std::atan(1.0 / 4.0),
        .overhang = 0.2,
        .thickness = 0.1,
    });
    invalid_pitch.properties["pitch_rad"] = std::atan(1.0 / 3.0);
    rejected([&] { (void)decode_building_entity(invalid_pitch); },
             "pitch disagreement must be rejected before command creation");
}

void test_empty_object_id_uses_entity_factory_id() {
    using namespace sketch;
    const auto entity = encode_building_entity(RectangularColumn{
        .base_center = {},
        .width = 0.4,
        .depth = 0.6,
        .height = 3.0,
        .rotation_radians = 0.0,
    });
    require(!entity.id.empty(), "an empty semantic id should use Entity::create id");
    const auto decoded = decode_building_entity(entity);
    require(std::visit([](const auto& object) { return object.id; }, decoded) == entity.id,
            "generated entity id must survive decode");
}

}  // namespace

void test_roof_opening_schema() {
    sketch::HipRoof roof{"opened", {0,0,3}, 0, 8,6,1.5,std::atan(0.5),0.2,0.15,
        {{"skylight", -0.5,-0.5,1,1}}};
    const auto entity = sketch::encode_building_entity(roof);
    require(entity.properties.at("version") == 2, "openings require a schema older readers reject");
    const auto decoded = sketch::decode_building_entity(entity);
    require(sketch::encode_building_entity(decoded).properties == entity.properties,
        "roof openings roundtrip exactly");
    auto downgraded = entity;
    downgraded.properties["version"] = 1;
    bool rejected = false;
    try { (void)sketch::decode_building_entity(downgraded); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "version one cannot silently accept opening geometry");
}

void test_room_volume_document_decoder_uses_shared_kernel() {
    using namespace sketch;
    const auto boundary = nlohmann::json::array({
        {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}}, {"sweep_radians", 0.0}},
        {{"start", {4.0, 0.0}}, {"end", {4.0, 3.0}}, {"sweep_radians", 0.0}},
        {{"start", {4.0, 3.0}}, {"end", {0.0, 3.0}}, {"sweep_radians", 0.0}},
        {{"start", {0.0, 3.0}}, {"end", {0.0, 0.0}}, {"sweep_radians", 0.0}},
    });
    const Entity entity{
        "room-document", "room",
        {{"boundary", boundary}, {"holes", nlohmann::json::array()},
         {"height_m", 2.4}, {"elevation_m", 0.0}},
        false, nlohmann::json::object()};
    RoomVolume decoded;
    std::string error;
    require(read_document_room(entity, decoded, error),
            "valid room volume must decode at the document boundary");
    near(solid_volume(make_room_volume(decoded)), 28.8, 1e-8,
         "decoded room volume must use the shared solid kernel");

    auto invalid_properties = entity.properties;
    invalid_properties["height_m"] = 0.0;
    const Entity invalid{"room-document-invalid", "room", invalid_properties,
                         false, nlohmann::json::object()};
    require(!read_document_room(invalid, decoded, error),
            "non-positive room height must fail closed at the document boundary");
}

int main() {
    try {
        test_all_forms_roundtrip_to_canonical_entities();
        test_recognized_type_is_distinct_from_decode_success();
        test_unknown_versions_forms_and_metadata_are_handled_strictly();
        test_integer_stairs_optional_landing_and_geometry_validation();
        test_empty_object_id_uses_entity_factory_id();
        test_room_volume_document_decoder_uses_shared_kernel();
        test_roof_opening_schema();
        std::cout << "Building entity codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
