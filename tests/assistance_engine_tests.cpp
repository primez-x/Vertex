#include "sketch/assistance_engine.hpp"
#include "sketch/assistance_contract.hpp"
#include "support/noninteractive_errors.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
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

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
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
            require(edge.preview.arguments.at("trace_mode") == "connected-components-v1",
                    "edge tracing mode was not recorded");
            require(edge.preview.arguments.at("points").size() >= 4,
                    "edge tracing contour is incomplete");
            sketch::validate_assistance_proposal(edge);
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
        require(dimensions.front().source.x == 0.2 && dimensions.front().source.y == 0.4 &&
                    dimensions.front().source.width == 0.6 && dimensions.front().source.height == 0.08,
                "dimension source rectangle must use the actual text run bounds");
        auto invalid_bounds = raster;
        invalid_bounds.text_runs.front().width = 1.0;
        invalid([&] { (void)sketch::extract_dimensions(invalid_bounds); });
        invalid_bounds = raster;
        invalid_bounds.text_runs.front().length = raster.source_text.size() + 1;
        invalid([&] { (void)sketch::extract_dimensions(invalid_bounds); });
        auto metric_raster = raster;
        metric_raster.source_text = "Reference dimension: 900 mm";
        metric_raster.text_runs.front().length = metric_raster.source_text.size();
        const auto metric_dimensions = sketch::extract_dimensions(metric_raster);
        require(metric_dimensions.size() == 1 &&
                    std::abs(metric_dimensions.front().preview.arguments.at("length_metres").get<double>() -
                             0.9) < 1e-9,
                "millimetre dimension was not parsed exactly");

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
