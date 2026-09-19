#include "sketch/desktop/main_window.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/boundary_entity.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/trusted_reference_fixture.hpp"
#include "reference_import.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QProcess>
#include <QDir>
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

void pdf_dimension_import_workflow(const QString& directory) {
    using namespace sketch;
    using json = nlohmann::json;
    QByteArray pdf("%PDF-1.4\n");
    std::vector<int> offsets{0};
    const QByteArray content("BT /F1 12 Tf 72 650 Td (Wall: 12 ft) Tj ET\n");
    const std::vector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream"};
    for (const auto& object : objects) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(offsets.size() - 1) + " 0 obj\n" + object + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i)
        pdf += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    const auto path = QDir(directory).filePath("dimensions.pdf");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(pdf) == pdf.size(), "PDF text fixture must save");
    file.close();

    // Direct codec execution verifies extraction independently of installation
    // qualification. The synthetic attestation below is ONLY a broker test seam.
    QProcess worker;
    worker.start(QDir(QCoreApplication::applicationDirPath()).filePath("property-studio-import-worker.exe"),
                 {"pdf", "0"});
    require(worker.waitForStarted(5000), "PDF text codec fixture must start");
    require(worker.write(pdf) == pdf.size(), "PDF fixture must be queued");
    worker.closeWriteChannel();
    if (!worker.waitForFinished(10000)) {
        worker.kill();
        worker.waitForFinished(5000);
        throw std::runtime_error("PDF text codec fixture exceeded its deadline");
    }
    require(worker.exitStatus() == QProcess::NormalExit && worker.exitCode() == 0,
            "PDF text codec fixture must decode");
    const auto bytes = worker.readAllStandardOutput();
    WindowsImportWorkerReport report;
    report.status = WindowsImportWorkerStatus::completed;
    report.completed = report.launched = report.app_container_verified = report.restricted_token_verified =
        report.network_denial_verified = report.job_limits_verified = report.parent_exit_kill_verified =
        report.brokered_handles_verified = report.private_temporary_root_verified =
        report.immutable_module_roots_verified = report.fixed_search_applied = report.proj_offline_applied = true;
    report.output.resize(static_cast<std::size_t>(bytes.size()));
    std::memcpy(report.output.data(), bytes.constData(), report.output.size());
    const auto decoded = desktop::decodeReferenceBytes(pdf, "pdf", 0, {}, [&](const auto&) { return report; });
    require(decoded.source_text == "Wall: 12 ft" && decoded.text_runs.size() == 1,
            "normal PDF codec path must extract the dimension text and selection");

    desktop::MainWindow window;
    const auto original_revision = window.document().revision();
    auto id = window.importReferenceImage(path);
    const bool qualified_import = !id.isEmpty();
    if (!qualified_import) {
        require(window.lastError().contains("Isolated reference import is unavailable") &&
                    window.document().revision() == original_revision,
                "unqualified PDF import must fail closed before document mutation");
        id = testing::importOrSeedTrustedReferenceFixture(window, path, decoded.image);
    }
    auto reference = window.document().snapshot().entities().at(id.toStdString());
    json runs = json::array();
    for (const auto& run : decoded.text_runs)
        runs.push_back({{"offset", run.offset}, {"length", run.length}, {"x", run.bounds.x()},
            {"y", run.bounds.y()}, {"width", run.bounds.width()}, {"height", run.bounds.height()}});
    if (qualified_import) {
        require(reference.properties.at("source_text") == decoded.source_text.toStdString() &&
                    reference.properties.at("source_text_runs") == runs &&
                    reference.properties.at("source_text_version") == 1,
                "normal import must persist broker-validated PDF text metadata");
    } else {
        reference.properties["source_text"] = decoded.source_text.toStdString();
        reference.properties["source_text_runs"] = runs;
        reference.properties["source_text_version"] = 1;
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(reference)}, {}, "Seed codec-validated text fixture"});
    }
    window.setAssistanceEnabled(true);
    require(window.calibrateReference(id, "0", "0", "100", "0", "1 m"),
            "PDF dimension reference must calibrate");
    const auto proposals = window.suggestReferenceAssistance(id, AssistanceKind::dimension_extraction);
    require(proposals.size() == 1 && proposals.front().source.original_text == "12 ft" &&
                std::abs(proposals.front().preview.arguments.at("length_metres").get<double>() - 3.6576) < 1e-9,
            "imported PDF embedded text must produce a parsed dimension proposal");
    const auto bounds = decoded.text_runs.front().bounds;
    const auto& source = proposals.front().source;
    require(source.x == bounds.x() && source.y == bounds.y() && source.width == bounds.width() &&
                source.height == bounds.height(), "PDF proposal must preserve its real selection bounds");
    const auto project = QDir(directory).filePath("dimensions.sketch");
    require(window.saveProjectAs(project) && window.openProject(project), "PDF text metadata must round-trip");
    window.setAssistanceEnabled(true);
    require(window.suggestReferenceAssistance(id, AssistanceKind::dimension_extraction) == proposals,
            "reopened PDF must reproduce deterministic source bounds and proposals");
}

sketch::AssistanceRaster dimension_fixture() {
    sketch::AssistanceRaster raster;
    raster.reference_id = "reference-1";
    raster.source_text = "Measured wall: 12 ft";
    raster.text_runs = {{0, raster.source_text.size(), 0.1, 0.2, 0.4, 0.05}};
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
        pdf_dimension_import_workflow(temporary.path());
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
        require(window.suggestReferenceAssistance(reference_id, AssistanceKind::dimension_extraction).empty(),
                "raster-only imported reference must yield no text dimension proposals");
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
