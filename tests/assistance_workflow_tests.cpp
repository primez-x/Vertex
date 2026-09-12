#include "sketch/desktop/main_window.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/boundary_entity.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

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
        const auto reference_id = window.importReferenceImage(
            QString::fromStdWString(image_path.wstring()));
        require(!reference_id.isEmpty(), "reference fixture must import");
        require(window.selectEntity(reference_id), "reference fixture must be selected");
        const auto trace = window.suggestReferenceAssistance(reference_id, AssistanceKind::tracing);
        require(trace.size() == 1 && trace.front().preview.command_type == "add_boundary",
                "desktop must expose deterministic trace suggestions");
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
