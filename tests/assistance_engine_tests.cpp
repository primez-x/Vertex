#include "sketch/assistance_engine.hpp"
#include "sketch/assistance_contract.hpp"
#include "sketch/quantity.hpp"
#include "support/noninteractive_errors.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void invalid(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid assistance input was accepted");
}

sketch::AssistanceRaster fixture() {
    sketch::AssistanceRaster raster;
    raster.reference_id = "reference-1";
    raster.source_text = "Plan notes: exterior wall 12 ft, interior wall 3.5 m";
    raster.text_runs = {{0, raster.source_text.size(), 0.2, 0.4, 0.6, 0.08}};
    raster.width = 24;
    raster.height = 16;
    raster.luminance.assign(raster.width * raster.height, 255);
    for (std::size_t y = 3; y <= 12; ++y) {
        raster.luminance[y * raster.width + 4] = 0;
        raster.luminance[y * raster.width + 19] = 0;
    }
    for (std::size_t x = 4; x <= 19; ++x) {
        raster.luminance[3 * raster.width + x] = 0;
        raster.luminance[12 * raster.width + x] = 0;
    }
    return raster;
}

double polygon_area(const nlohmann::json& points) {
    double twice_area = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& first = points.at(index);
        const auto& second = points.at((index + 1) % points.size());
        twice_area += first.at(0).get<double>() * second.at(1).get<double>() -
                      second.at(0).get<double>() * first.at(1).get<double>();
    }
    return std::abs(twice_area) * 0.5;
}

void centered_reference_transform() {
    using namespace sketch;
    auto raster = fixture();
    raster.width = 32;
    raster.height = 18;
    raster.luminance.assign(raster.width * raster.height, 255);
    for (std::size_t y = 2; y <= 12; ++y)
        for (std::size_t x = 3; x <= (y < 9 ? 8u : 17u); ++x)
            raster.luminance[y * raster.width + x] = 0;
    const auto raw_bbox = suggest_tracing(raster).front();
    const auto raw_edge = suggest_edge_tracing(raster).front();
    const auto dimensions = extract_dimensions(raster);
    for (const bool horizontal : {false, true}) {
        for (const bool vertical : {false, true}) {
            AssistanceEngineOptions options{0.2, {6.0, -3.0}, 0.61, 2.0,
                                             true, horizontal, vertical};
            // Independent source-space expectations for this off-centre L.
            const auto expected = [&](Vec2 pixel) {
                const auto x = (pixel.x - 16.0) * 0.4 * (horizontal ? -1 : 1);
                const auto y = (pixel.y - 9.0) * 0.4 * (vertical ? -1 : 1);
                return Vec2{6.0 + std::cos(0.61) * x - std::sin(0.61) * y,
                            -3.0 + std::sin(0.61) * x + std::cos(0.61) * y};
            };
            const auto bbox = suggest_tracing(raster, options).front();
            const auto edge = suggest_edge_tracing(raster, options).front();
            const std::vector<Vec2> bbox_pixels{{3, 2}, {17, 2}, {17, 12}, {3, 12}};
            const std::vector<Vec2> edge_pixels{{3, 2}, {9, 2}, {9, 9},
                                                {18, 9}, {18, 13}, {3, 13}};
            for (const auto& [proposal, pixels] :
                 std::vector<std::pair<AssistanceProposal, std::vector<Vec2>>>{
                     {bbox, bbox_pixels}, {edge, edge_pixels}}) {
                const auto& points = proposal.preview.arguments.at("points");
                require(points.size() == pixels.size(), "reference transform changed contour topology");
                for (const auto pixel : pixels) {
                    const auto point = expected(pixel);
                    bool found = false;
                    for (const auto& actual : points)
                        found |= std::abs(actual.at(0).get<double>() - point.x) < 1e-10 &&
                                 std::abs(actual.at(1).get<double>() - point.y) < 1e-10;
                    require(found, "centered mirrored reference trace does not match raw source corners");
                    const auto mapped = assistance_source_point_to_model(pixel, 32, 18, options);
                    require(std::abs(mapped.x - point.x) < 1e-10 &&
                                std::abs(mapped.y - point.y) < 1e-10,
                            "shared reference selection mapping differs from trace geometry");
                }
                require(proposal.preview.arguments.at("centered_source") == true &&
                            proposal.preview.arguments.at("flip_horizontal") == horizontal &&
                            proposal.preview.arguments.at("flip_vertical") == vertical,
                        "trace must retain its centered source and mirror transform provenance");
            }
            require(bbox.source == raw_bbox.source && edge.source == raw_edge.source &&
                        bbox.preview.arguments.at("source_pixel_bounds") ==
                            raw_bbox.preview.arguments.at("source_pixel_bounds") &&
                        edge.preview.arguments.at("source_pixel_bounds") ==
                            raw_edge.preview.arguments.at("source_pixel_bounds"),
                    "mirror transforms must preserve raw unmirrored source selections");
            require(extract_dimensions(raster, options) == dimensions,
                    "reference transforms must preserve recognized text and raw text regions");
            require(bbox.id != raw_bbox.id && edge.id != raw_edge.id,
                    "transformed trace identity must include reference transform provenance");
        }
    }
    const auto top_left = assistance_source_point_to_model({3, 2}, 32, 18);
    require(std::abs(top_left.x - 0.03) < 1e-12 && std::abs(top_left.y - 0.02) < 1e-12,
            "public engine default must retain its top-left positive-Y mapping");
    const auto outlined = fixture();
    const auto raw_outline = suggest_edge_tracing(outlined).front();
    for (const bool horizontal : {false, true}) {
        for (const bool vertical : {false, true}) {
            const AssistanceEngineOptions options{0.2, {6, -3}, 0.61, 2,
                                                    true, horizontal, vertical};
            const auto mirrored = suggest_edge_tracing(outlined, options).front();
            require(mirrored.preview.arguments.at("holes").size() == 1,
                    "mirroring must preserve enclosed source voids");
            for (const auto& [raw_points, mapped_points] :
                 std::vector<std::pair<nlohmann::json, nlohmann::json>>{
                     {raw_outline.preview.arguments.at("points"), mirrored.preview.arguments.at("points")},
                     {raw_outline.preview.arguments.at("holes").at(0), mirrored.preview.arguments.at("holes").at(0)}}) {
                require(raw_points.size() == mapped_points.size(), "mirroring must preserve outer and void topology");
                for (std::size_t index = 0; index < raw_points.size(); ++index) {
                    const double x = (raw_points[index][0].get<double>() / 0.01 - 12) *
                                     0.4 * (horizontal ? -1 : 1);
                    const double y = (raw_points[index][1].get<double>() / 0.01 - 8) *
                                     0.4 * (vertical ? -1 : 1);
                    require(std::abs(mapped_points[index][0].get<double>() -
                                (6 + std::cos(0.61) * x - std::sin(0.61) * y)) < 1e-10 &&
                                std::abs(mapped_points[index][1].get<double>() -
                                (-3 + std::sin(0.61) * x + std::cos(0.61) * y)) < 1e-10,
                            "outer and enclosed void must share the same centered mirrored transform");
                }
            }
        }
    }
    for (const auto bad : {std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        for (int field = 0; field < 5; ++field) {
            AssistanceEngineOptions options;
            if (field == 0) options.metres_per_pixel = bad;
            if (field == 1) options.origin_metres.x = bad;
            if (field == 2) options.origin_metres.y = bad;
            if (field == 3) options.rotation_radians = bad;
            if (field == 4) options.image_scale = bad;
            invalid([&] { (void)suggest_tracing(raster, options); });
            invalid([&] { (void)suggest_edge_tracing(raster, options); });
            invalid([&] { (void)extract_dimensions(raster, options); });
            invalid([&] { (void)assistance_source_point_to_model({3, 2}, 32, 18, options); });
        }
    }
    invalid([&] { (void)assistance_source_point_to_model({-1, 2}, 32, 18); });
    invalid([&] { (void)assistance_source_point_to_model({33, 2}, 32, 18); });
    invalid([&] { (void)assistance_source_point_to_model({1, 19}, 32, 18); });
    invalid([&] { (void)assistance_source_point_to_model({1, 1}, 0, 18); });
    invalid([&] { (void)assistance_source_point_to_model({1, 1}, 8193, 18); });
    invalid([&] { (void)assistance_source_point_to_model({1, std::numeric_limits<double>::infinity()}, 32, 18); });
    invalid([&] { (void)assistance_source_point_to_model({std::numeric_limits<double>::quiet_NaN(), 1}, 32, 18); });
}

void wall_dimension_proposals() {
    auto raster = fixture();
    raster.text_producer = "offline-recognizer-v1";
    raster.text_runs.front().confidence = 0.23;
    const auto retained = raster.source_text;
    const auto wall = sketch::extract_wall_dimensions(raster, {}, "physical-wall");
    const auto boundary = sketch::extract_dimensions(raster, {}, "physical-wall");
    require(wall.size() == 2 && wall == sketch::extract_wall_dimensions(raster, {}, "physical-wall"),
            "wall dimension proposals must be deterministic recognized observations");
    require(wall.front().id != boundary.front().id && wall.front().id !=
                sketch::extract_wall_dimensions(raster, {}, "other-wall").front().id,
            "typed wall target and owner identity must affect proposal identity");
    for (std::size_t index = 0; index < wall.size(); ++index) {
        const auto& value = wall[index];
        const auto& args = value.preview.arguments;
        require(value.kind == sketch::AssistanceKind::dimension_extraction &&
                    value.preview.command_type == "add_wall_dimension_suggestion" &&
                    value.preview.affected_entity_ids == std::vector<std::string>{value.id, "physical-wall"},
                "wall operation must carry exactly the proposal and physical owner affected IDs");
        require(args.size() == 5 && args.at("target_wall_id") == "physical-wall" &&
                    args.at("length_metres") == boundary[index].preview.arguments.at("length_metres") &&
                    args.at("length_expression") == boundary[index].preview.arguments.at("length_expression") &&
                    args.at("source_text") == value.source.original_text && args.contains("source_offset"),
                "wall proposal must carry an observation and typed owner without invented segment geometry");
        require(value.source == boundary[index].source && value.producer == raster.text_producer &&
                    value.resources == boundary[index].resources && value.source.confidence == 0.23,
                "wall extraction must preserve raster source selection, confidence and provenance");
        require(sketch::decode_assistance_proposal(sketch::encode_assistance_proposal(value)) == value,
                "typed wall operation must round trip the proposal envelope");
        sketch::validate_assistance_proposal(value);
    }
    require(raster.source_text == retained, "wall proposal extraction must not mutate source observations");
    raster.text_runs.clear();
    require(sketch::extract_wall_dimensions(raster, {}, "physical-wall").empty(),
            "unlocated wall observations must not invent source bounds");
    invalid([&] { (void)sketch::extract_wall_dimensions(raster, {}, ""); });
    invalid([&] { (void)sketch::extract_wall_dimensions(raster, {}, "unsafe/wall"); });
    invalid([&] { (void)sketch::extract_wall_dimensions(raster, {}, std::string(129, 'x')); });
    sketch::AssistanceEngineOptions options;
    options.image_scale = 0;
    invalid([&] { (void)sketch::extract_wall_dimensions(raster, options, "physical-wall"); });
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        centered_reference_transform();
        wall_dimension_proposals();
        const auto raster = fixture();
        sketch::validate_assistance_raster(raster);

        const auto first_trace = sketch::suggest_tracing(raster);
        const auto second_trace = sketch::suggest_tracing(raster);
        require(first_trace.size() == 1, "fixture should produce one trace proposal");
        require(first_trace == second_trace, "trace suggestions must be deterministic");
        const auto& trace = first_trace.front();
        require(trace.kind == sketch::AssistanceKind::tracing, "trace kind was lost");
        require(trace.preview.command_type == "add_boundary", "trace command type changed");
        require(trace.preview.arguments.at("closed") == true, "trace is not closed");
        require(trace.preview.arguments.at("points").size() == 4, "trace rectangle is incomplete");
        require(trace.source.reference_id == "reference-1", "trace source reference was lost");
        sketch::validate_assistance_proposal(trace);
        require(trace.producer == "vertex-assisted-v1" &&
                    trace.resources.at(1).id == "Vertex-LICENSE",
                "new assistance must identify Vertex");
        auto legacy = sketch::encode_assistance_proposal(trace);
        legacy["producer"] = "property-studio-assisted-v1";
        legacy["resources"][1]["id"] = "Property-Studio-LICENSE";
        const auto upgraded = sketch::decode_assistance_proposal(legacy);
        require(upgraded == trace && sketch::missing_assistance_resources(
                    upgraded, sketch::default_assistance_resource_ids()).empty(),
                "legacy assistance must upgrade and resolve current resources");
        require(sketch::encode_assistance_proposal(upgraded)["producer"] == "vertex-assisted-v1",
                "re-encoded legacy assistance must use Vertex");
        legacy["producer"] = "third-party-engine";
        legacy["resources"][1]["id"] = "third-party-license";
        const auto external = sketch::decode_assistance_proposal(legacy);
        require(external.producer == "third-party-engine" &&
                    external.resources.at(1).id == "third-party-license",
                "third-party assistance provenance must remain unchanged");

        auto components = raster;
        components.width = 40;
        components.height = 24;
        components.luminance.assign(components.width * components.height, 255);
        for (std::size_t y = 3; y <= 10; ++y) {
            components.luminance[y * components.width + 2] = 0;
            components.luminance[y * components.width + 14] = 0;
            components.luminance[y * components.width + 24] = 0;
            components.luminance[y * components.width + 36] = 0;
        }
        for (std::size_t x = 2; x <= 14; ++x) {
            components.luminance[3 * components.width + x] = 0;
            components.luminance[10 * components.width + x] = 0;
        }
        for (std::size_t x = 24; x <= 36; ++x) {
            components.luminance[3 * components.width + x] = 0;
            components.luminance[10 * components.width + x] = 0;
        }
        const auto first_edges = sketch::suggest_edge_tracing(components);
        const auto second_edges = sketch::suggest_edge_tracing(components);
        require(first_edges.size() == 2, "edge tracing should produce one proposal per component");
        require(first_edges == second_edges, "edge tracing must be deterministic");
        for (const auto& edge : first_edges) {
            require(edge.kind == sketch::AssistanceKind::edge_tracing,
                    "edge tracing kind was lost");
            require(edge.preview.arguments.at("trace_mode") == "pixel-contours-v2",
                    "edge tracing mode was not recorded");
            require(edge.preview.arguments.at("points").size() >= 4,
                    "edge tracing contour is incomplete");
            require(edge.preview.arguments.at("holes").size() == 1 &&
                        edge.preview.arguments.at("hole_ids").size() == 1 &&
                        edge.preview.affected_entity_ids.size() == 2,
                    "outlined components must preserve their enclosed void");
            sketch::validate_assistance_proposal(edge);
        }

        auto concave = raster;
        concave.width = 32;
        concave.height = 32;
        concave.luminance.assign(concave.width * concave.height, 255);
        for (std::size_t y = 3; y <= 22; ++y) {
            for (std::size_t x = 3; x <= 8; ++x)
                concave.luminance[y * concave.width + x] = 0;
        }
        for (std::size_t y = 17; y <= 22; ++y) {
            for (std::size_t x = 3; x <= 24; ++x)
                concave.luminance[y * concave.width + x] = 0;
        }
        const auto concave_edges = sketch::suggest_edge_tracing(concave);
        require(concave_edges.size() == 1,
                "one connected concave region must produce one proposal");
        const auto& concave_arguments = concave_edges.front().preview.arguments;
        require(concave_arguments.at("points").size() == 6 &&
                    concave_arguments.at("holes").empty(),
                "an L-shaped region must retain its six-corner concavity");
        const auto bounds = concave_arguments.at("source_pixel_bounds");
        const auto bounds_area = static_cast<double>(bounds.at(2).get<std::size_t>() -
                                                      bounds.at(0).get<std::size_t>() + 1) *
                                 static_cast<double>(bounds.at(3).get<std::size_t>() -
                                                      bounds.at(1).get<std::size_t>() + 1) *
                                 0.0001;
        require(polygon_area(concave_arguments.at("points")) < bounds_area * 0.7,
                "edge tracing must not inflate a concave region to its envelope");

        auto diagonal_touch = raster;
        diagonal_touch.width = 20;
        diagonal_touch.height = 20;
        diagonal_touch.luminance.assign(diagonal_touch.width * diagonal_touch.height, 255);
        for (std::size_t y = 2; y <= 5; ++y)
            for (std::size_t x = 2; x <= 5; ++x)
                diagonal_touch.luminance[y * diagonal_touch.width + x] = 0;
        for (std::size_t y = 6; y <= 9; ++y)
            for (std::size_t x = 6; x <= 9; ++x)
                diagonal_touch.luminance[y * diagonal_touch.width + x] = 0;
        const auto diagonal_edges = sketch::suggest_edge_tracing(diagonal_touch);
        require(diagonal_edges.size() == 2 &&
                    diagonal_edges.front().preview.arguments.at("component_index") == 0 &&
                    diagonal_edges.back().preview.arguments.at("component_index") == 0,
                "diagonally touching pixels must remain one component with two valid contours");
        for (const auto& edge : diagonal_edges) {
            require(edge.preview.arguments.at("points").size() == 4 &&
                        edge.preview.arguments.at("holes").empty(),
                    "a diagonal touch must not create a self-touching boundary");
        }

        const auto dimensions = sketch::extract_dimensions(raster);
        auto unlocated = raster;
        unlocated.text_runs.clear();
        require(sketch::extract_dimensions(unlocated).empty(),
                "unlocated text must not invent a source rectangle");
        require(dimensions.size() == 2, "fixture should produce two dimension proposals");
        require(dimensions.front().preview.command_type == "add_dimension_suggestion",
                "dimension command type changed");
        require(dimensions.front().source.original_text == "12 ft",
                "dimension source text was not retained");
        require(std::abs(dimensions.front().preview.arguments.at("length_metres").get<double>() -
                         3.6576) < 1e-4,
                "imperial dimension was not parsed exactly");
        for (const auto& proposal : dimensions) sketch::validate_assistance_proposal(proposal);
        require(dimensions == sketch::extract_dimensions(raster), "dimensions must be deterministic");
        require(dimensions.front().source.confidence == 0.86,
                "embedded text compatibility confidence changed");
        auto recognized = raster;
        recognized.text_runs = {{0, 31, 0.2, 0.4, 0.6, 0.08, 0.23},
                                {31, raster.source_text.size() - 31, 0.1, 0.6, 0.7, 0.09, 0.91}};
        recognized.text_producer = "offline-recognizer-v1";
        recognized.text_resources = {{"recognizer-model", "assets/ocr/model.dat",
                                      "Offline recognition model", "Apache-2.0", true}};
        const auto recognized_dimensions = sketch::extract_dimensions(
            recognized, {}, "boundary-1", "segment-1");
        require(recognized_dimensions.size() == 2 &&
                    recognized_dimensions.front().source.confidence == 0.23 &&
                    recognized_dimensions.back().source.confidence == 0.91,
                "each dimension must retain its containing recognition run confidence");
        require(recognized_dimensions.front().source.original_text == "12 ft" &&
                    recognized_dimensions.front().source.x == 0.2 &&
                    recognized_dimensions.back().source.y == 0.6 &&
                    recognized_dimensions.front().producer == recognized.text_producer &&
                    recognized_dimensions.front().resources.back() == recognized.text_resources.front() &&
                    recognized_dimensions.front().resources.size() == 3,
                "recognition text, bounds, producer and resources must survive extraction");
        require(recognized_dimensions.front().preview.arguments.at("target_segment_id") == "segment-1" &&
                    recognized_dimensions.front().preview.affected_entity_ids.back() == "segment-1" &&
                    recognized_dimensions.front().id != sketch::extract_dimensions(
                        recognized, {}, "boundary-1", "segment-2").front().id,
                "segment association must remain typed and affect proposal identity");
        require(sketch::decode_assistance_proposal(sketch::encode_assistance_proposal(
                    recognized_dimensions.front())) == recognized_dimensions.front(),
                "recognition provenance and segment must survive envelope serialization");
        invalid([&] { (void)sketch::extract_dimensions(recognized, {}, {}, "segment-1"); });
        invalid([&] { (void)sketch::extract_dimensions(recognized, {}, "boundary-1", "unsafe/segment"); });
        for (const auto confidence : {0.0, 1.0}) {
            recognized.text_runs.front().confidence = confidence;
            require(sketch::extract_dimensions(recognized).front().source.confidence == confidence,
                    "confidence endpoints must remain exact");
        }
        for (const auto confidence : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::quiet_NaN()}) {
            auto malformed = recognized;
            malformed.text_runs.front().confidence = confidence;
            invalid([&] { sketch::validate_assistance_raster(malformed); });
        }
        const auto invalid_resource = [&](auto mutate) {
            auto malformed = recognized;
            malformed.source_text.clear();
            malformed.text_runs.clear();
            mutate(malformed);
            invalid([&] { (void)sketch::extract_dimensions(malformed); });
        };
        invalid_resource([](auto& r) { r.text_resources.front().relative_path = "../model.dat"; });
        invalid_resource([](auto& r) { r.text_resources.front().license.clear(); });
        invalid_resource([](auto& r) { r.text_resources.front().id = "assistance-engine-v1"; });
        invalid_resource([](auto& r) { r.text_resources.push_back(r.text_resources.front()); });
        invalid_resource([](auto& r) { r.text_resources.resize(63, r.text_resources.front()); });
        invalid_resource([](auto& r) { r.text_producer = "unsafe\nproducer"; });
        require(dimensions.front().source.x == 0.2 && dimensions.front().source.y == 0.4 &&
                    dimensions.front().source.width == 0.6 && dimensions.front().source.height == 0.08,
                "dimension source rectangle must use the actual text run bounds");
        auto invalid_bounds = raster;
        invalid_bounds.text_runs.front().width = 1.0;
        invalid([&] { (void)sketch::extract_dimensions(invalid_bounds); });
        invalid_bounds = raster;
        invalid_bounds.text_runs.front().length = raster.source_text.size() + 1;
        invalid([&] { (void)sketch::extract_dimensions(invalid_bounds); });
        invalid_bounds = recognized;
        invalid_bounds.text_runs.front().x = std::numeric_limits<double>::quiet_NaN();
        invalid([&] { (void)sketch::extract_dimensions(invalid_bounds); });
        auto metric_raster = raster;
        metric_raster.source_text = "Reference dimension: 900 mm";
        metric_raster.text_runs.front().length = metric_raster.source_text.size();
        const auto metric_dimensions = sketch::extract_dimensions(metric_raster);
        require(metric_dimensions.size() == 1 &&
                    std::abs(metric_dimensions.front().preview.arguments.at("length_metres").get<double>() -
                             0.9) < 1e-9,
                "millimetre dimension was not parsed exactly");
        for (const std::string expression : {"31' 6\"", "31 ft 6 in", "12'6\"",
                                             "12' 3 1/2\"", "2ft 3/4in", "6 1/2 in"}) {
            auto compound = recognized;
            const std::string prefix = "\xC3\x89tage dimension: ";
            compound.source_text = prefix + expression + "; room 104";
            compound.text_runs = {{0, compound.source_text.size(), 0.15, 0.25, 0.65, 0.1, 0.37}};
            const auto proposals = sketch::extract_dimensions(compound);
            require(proposals.size() == 1 &&
                        proposals.front().source.original_text == expression &&
                        proposals.front().preview.arguments.at("source_offset") == prefix.size() &&
                        proposals.front().source.x == 0.15 &&
                        proposals.front().source.width == 0.65 &&
                        proposals.front().source.confidence == 0.37 &&
                        proposals.front().preview.arguments.at("length_metres").get<double>() ==
                            sketch::parse_quantity(expression, sketch::Unit::metre).metres,
                    "explicit compound quantity must remain one exact located recognition");
        }
        for (const std::string note : {"Room 104, revision 31, scale 1:50", "12 models and 6 images",
                                      "-31' 6\"", "-12 ft", "12' 3 1/0\"", "codeA12ftB"}) {
            auto ambiguous = raster;
            ambiguous.source_text = note;
            ambiguous.text_runs = {{0, note.size(), 0.2, 0.4, 0.6, 0.08, 0.6}};
            require(sketch::extract_dimensions(ambiguous).empty(),
                    "negative, invalid or unqualified notes must not invent positive dimensions");
        }

        const std::vector<sketch::AssistanceAnchor> anchors{
            {"room-1", "Kitchen", {2.0, 3.0}},
            {"room-2", "Living room", {4.0, 5.0}},
        };
        const auto labels = sketch::suggest_label_placements(anchors);
        require(labels.size() == anchors.size(), "each anchor should receive one label suggestion");
        require(labels[0].preview.arguments.at("content") == "Kitchen",
                "label content was not retained");
        require(labels[0].preview.arguments.at("template_id") == "note",
                "label template is not explicit");
        require(labels[0].preview.affected_entity_ids.size() == 2,
                "label proposal must identify anchor and new annotation");

        const auto language = sketch::parse_natural_language("label Entry at 1.25, 2.5");
        require(language.size() == 1, "language command should produce one proposal");
        require(language.front().preview.command_type == "add_label",
                "language label command was not typed");
        require(language.front().preview.arguments.at("position") ==
                    nlohmann::json::array({1.25, 2.5}),
                "language coordinates were not parsed");
        const auto workspace = sketch::parse_natural_language("set workspace architectural");
        require(workspace.size() == 1 &&
                    workspace.front().preview.command_type == "set_workspace" &&
                    workspace.front().preview.arguments.at("workspace") == "architectural",
                "workspace language command was not typed");
        const auto rectangle = sketch::parse_natural_language("draw rectangle 12 ft x 8 ft");
        require(rectangle.size() == 1 &&
                    rectangle.front().preview.command_type == "add_boundary" &&
                    rectangle.front().preview.arguments.at("points").size() == 4,
                "rectangle language command was not typed");

        invalid([&] {
            auto malformed = raster;
            malformed.luminance.pop_back();
            sketch::validate_assistance_raster(malformed);
        });
        invalid([&] {
            auto malformed = raster;
            malformed.width = 0;
            sketch::validate_assistance_raster(malformed);
        });
        invalid([&] { (void)sketch::parse_natural_language("do something surprising"); });
        invalid([&] { (void)sketch::parse_natural_language("label unsafe\ntext at 1,2"); });
        invalid([&] {
            std::vector<sketch::AssistanceAnchor> duplicate{
                {"same", "A", {0.0, 0.0}}, {"same", "B", {1.0, 1.0}}};
            (void)sketch::suggest_label_placements(duplicate);
        });

        sketch::AssistanceSession session;
        session.set_enabled(true);
        const auto request = session.request_acceptance(
            trace, true, sketch::default_assistance_resource_ids());
        require(request.proposal == trace && request.requires_undo_transaction,
                "accepted proposal bypassed normal command requirements");

        std::cout << "assistance engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
