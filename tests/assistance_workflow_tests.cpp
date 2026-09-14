#include "sketch/desktop/main_window.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/boundary_entity.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/trusted_reference_fixture.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

sketch::AssistanceRaster dimension_fixture() {
    sketch::AssistanceRaster raster;
    raster.reference_id = "reference-1";
    raster.source_text = "Measured wall: 12 ft";
    raster.width = 8;
    raster.height = 8;
    raster.luminance.assign(raster.width * raster.height, 255);
    return raster;
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        using namespace sketch;
        using json = nlohmann::json;
        desktop::MainWindow window;
        require(!window.assistanceEnabled(), "assistance must start disabled");
        require(window.suggestLabelAssistance().empty(),
                "disabled assistance must not generate suggestions");
        window.setAssistanceEnabled(true);

        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory must be available");
        const auto image_path = std::filesystem::path(temporary.path().toStdWString()) / "plan.png";
        QImage image(80, 60, QImage::Format_ARGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setPen(QPen(Qt::black, 6));
            painter.drawRect(8, 8, 64, 44);
        }
        require(image.save(QString::fromStdWString(image_path.wstring()), "PNG"),
                "reference fixture must save");
        const auto reference_id = testing::importOrSeedTrustedReferenceFixture(window,
            QString::fromStdWString(image_path.wstring()), image);
        require(!reference_id.isEmpty(), "reference fixture must import");
        require(window.selectEntity(reference_id), "reference fixture must be selected");
        const auto require_calibration_error = [&] {
            const auto revision = window.document().revision();
            for (const auto kind : {AssistanceKind::tracing, AssistanceKind::edge_tracing,
                                    AssistanceKind::dimension_extraction}) {
                window.setAssistanceEnabled(false);
                window.setAssistanceEnabled(true);
                require(window.suggestReferenceAssistance(reference_id, kind).empty(),
                        "uncalibrated or invalid reference must not generate trace suggestions");
                require(window.lastError().contains("Calibrate", Qt::CaseInsensitive),
                        "rejected trace must explain that reference calibration is required");
            }
            require(window.document().revision() == revision,
                    "rejected trace suggestions must not change the document");
        };
        require_calibration_error();
        require(window.calibrateReference(reference_id, "0", "0", "40", "0", "4 m"),
                "reference fixture must calibrate from a known distance");
        const auto calibrated = window.document().snapshot().entities().at(reference_id.toStdString());
        const auto replace_reference = [&](Entity replacement) {
            window.document().apply(ApplyEntityChanges{window.document().revision(),
                {EntityChange::upsert(std::move(replacement))}, {}, "Set calibration fixture"});
        };
        for (const auto* field : {"calibration_first_source", "calibration_second_source",
                                  "calibration_known_distance", "metres_per_source_unit"}) {
            auto invalid = calibrated;
            invalid.properties.erase(field);
            replace_reference(std::move(invalid));
            require_calibration_error();
        }
        for (const auto& [field, value] : std::vector<std::pair<std::string, json>>{
                 {"calibration_first_source", json::array({"bad", 0})},
                 {"calibration_second_source", json::array({0, 0})},
                 {"calibration_second_source", json::array({1e-310, 0})},
                 {"calibration_first_source", json::array({-1.7e308, -1.7e308})},
                 {"calibration_known_distance", "not a distance"},
                 {"calibration_known_distance", "0 m"},
                 {"calibration_known_distance", "-4 m"},
                 {"metres_per_source_unit", 0},
                 {"metres_per_source_unit", -1},
                 {"metres_per_source_unit", "0.1"},
                 {"metres_per_source_unit", 0.01}}) {
            auto invalid = calibrated;
            invalid.properties[field] = value;
            replace_reference(std::move(invalid));
            require_calibration_error();
        }
        replace_reference(calibrated);
        const auto trace = window.suggestReferenceAssistance(reference_id, AssistanceKind::tracing);
        require(trace.size() == 1 && trace.front().preview.command_type == "add_boundary",
                "desktop must expose deterministic trace suggestions");
        require(std::abs(trace.front().preview.arguments.at("metres_per_pixel").get<double>() -
                         0.1) < 1e-12,
                "trace suggestions must use the known-distance calibration");
        const auto edge_trace = window.suggestReferenceAssistance(reference_id,
                                                                   AssistanceKind::edge_tracing);
        require(edge_trace.size() == 1 &&
                    edge_trace.front().preview.arguments.at("trace_mode") ==
                        "connected-components-v1",
                "desktop must expose connected-component edge tracing");
        const auto revision_before_trace = window.document().revision();
        require(window.acceptAssistanceProposal(trace.front()),
                "accepted trace must use the desktop command path");
        require(window.document().revision() == revision_before_trace + 1,
                "accepted trace must create one history revision");
        const auto trace_id = trace.front().id;
        const auto traced_snapshot = window.document().snapshot();
        const auto traced = traced_snapshot.entities().find(trace_id);
        require(traced != traced_snapshot.entities().end() &&
                    traced->second.type == "measurement_boundary" &&
                    inspect_boundary_entity_version(traced->second).format ==
                        BoundaryEntityFormat::identified_v1,
                "accepted trace must be an identified analytical boundary");
        require(window.undoCommand() &&
                    !window.document().snapshot().entities().contains(trace_id),
                "accepted trace must be undoable");
        const auto edge_id = edge_trace.front().id;
        const auto revision_before_edge_trace = window.document().revision();
        require(window.acceptAssistanceProposal(edge_trace.front()),
                "accepted edge trace must use the desktop command path");
        require(window.document().revision() == revision_before_edge_trace + 1 &&
                    window.document().snapshot().entities().contains(edge_id),
                "accepted edge trace must create one identified boundary revision");
        require(window.undoCommand() &&
                    !window.document().snapshot().entities().contains(edge_id),
                "accepted edge trace must be undoable");

        const auto natural = window.parseAssistanceCommand("label Entry at 1.25, 2.5");
        require(natural.size() == 1, "desktop must expose the local language grammar");
        const auto annotation_revision = window.document().revision();
        require(window.acceptAssistanceProposal(natural.front()),
                "accepted language label must use the annotation command path");
        require(window.document().revision() == annotation_revision + 1,
                "accepted label must create one history revision");
        require(window.undoCommand(), "accepted label must be undoable");

        const auto legacy_boundary = window.createBoundary(Boundary{
            {{0.0, 0.0}, {4.0, 0.0}, 0.0}, {{4.0, 0.0}, {4.0, 3.0}, 0.0},
            {{4.0, 3.0}, {0.0, 3.0}, 0.0}, {{0.0, 3.0}, {0.0, 0.0}, 0.0}});
        require(!legacy_boundary.isEmpty(), "dimension target boundary must be created");
        auto dimension = extract_dimensions(dimension_fixture()).front();
        dimension.preview.arguments["target_boundary_id"] = legacy_boundary.toStdString();
        const auto dimension_revision = window.document().revision();
        require(window.acceptAssistanceProposal(dimension),
                "accepted dimension must upgrade and annotate its target boundary");
        require(window.document().revision() == dimension_revision + 1,
                "accepted dimension must be one atomic history command");
        const auto after_dimension = window.document().snapshot();
        require(after_dimension.entities().contains(dimension.id) &&
                    after_dimension.entities().at(dimension.id).type == "dimension" &&
                    inspect_boundary_entity_version(after_dimension.entities().at(
                        legacy_boundary.toStdString())).format == BoundaryEntityFormat::identified_v1,
                "accepted dimension must preserve an identified target reference");
        require(window.undoCommand() && !window.document().snapshot().entities().contains(dimension.id),
                "accepted dimension must be undoable as one command");

        window.setAssistanceEnabled(false);
        require(window.parseAssistanceCommand("label Disabled at 1,1").empty(),
                "disabling assistance must stop new proposals");
        std::cout << "assistance workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
