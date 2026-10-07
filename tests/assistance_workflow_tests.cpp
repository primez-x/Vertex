#include "support/detached_document_snapshot.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_organization.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/trusted_reference_fixture.hpp"
#include "reference_import.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QProcess>
#include <QDir>
#include <QTransform>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void desktop_proposal_guards() {
    using namespace sketch;
    using namespace sketch::desktop;
    for (const bool labels : {false, true}) {
        MainWindow window;
        window.setAssistanceEnabled(true);
        const auto boundary = window.createBoundary(Boundary{
            {{12, -7}, {16, -7}, 0}, {{16, -7}, {16, -4}, 0},
            {{16, -4}, {12, -4}, 0}, {{12, -4}, {12, -7}, 0}});
        require(!boundary.isEmpty(), "proposal guard fixture must create named geometry");
        auto named_boundary = window.document().snapshot().entities().at(boundary.toStdString());
        named_boundary.properties["name"] = "Guarded named boundary";
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(std::move(named_boundary))},
            {AssetChange::upsert(Asset::create("guard-asset", "application/octet-stream", {std::byte{1}}))},
            "Seed guard asset"});
        const auto generate = [&] {
            const auto proposals = labels ? window.suggestLabelAssistance()
                : window.parseAssistanceCommand("label Guarded entry at 1, 2");
            if (proposals.size() != 1)
                std::cerr << "Desktop " << (labels ? "label" : "command") << " guard fixture proposal count: "
                          << proposals.size() << "; status: " << window.lastError().toStdString() << '\n';
            require(proposals.size() == 1, "one explicitly named geometry or command must produce one guarded proposal");
            return proposals.front();
        };
        const auto proposal = generate();
        const auto source = window.document().snapshot();
        require(proposal.preview.arguments.at("source_document_digest") == document_snapshot_digest(source) &&
                proposal.preview.arguments.at("source_workspace") == static_cast<int>(window.workspace()),
                "every desktop label and command proposal must bind the full current snapshot and workspace");
        const auto refuse = [&](const AssistanceProposal& rejected) {
            const auto before = document_snapshot_digest(window.document().snapshot());
            const auto workspace = window.workspace();
            require(!window.acceptAssistanceProposal(rejected) && window.workspace() == workspace &&
                    document_snapshot_digest(window.document().snapshot()) == before,
                    "missing, malformed or stale desktop guards must refuse without any document or workspace mutation");
        };
        for (int malformed = 0; malformed != 6; ++malformed) {
            auto invalid = proposal;
            auto& args = invalid.preview.arguments;
            if (malformed == 0) args.erase("source_document_digest");
            else if (malformed == 1) args.erase("source_workspace");
            else if (malformed == 2) args["source_document_digest"] = 12;
            else if (malformed == 3) args["source_document_digest"] = "";
            else if (malformed == 4) args["source_workspace"] = double(static_cast<int>(window.workspace()));
            else args["source_workspace"] = "measurement";
            refuse(invalid);
        }
        for (int changed = 0; changed != 4; ++changed) {
            sketch::test::DetachedDocumentSnapshotFixture replacement(source);
            auto& history = replacement.history();
            if (changed == 0) history.back().entities.at(boundary.toStdString()).extensions["same_revision_replacement"] = true;
            else if (changed == 1) {
                auto& organization = history.back().entities.at(window.activeLayerId().toStdString());
                organization.properties["name"] = "Replaced organization at same revision";
            } else if (changed == 2) {
                auto& asset = history.back().assets.at("guard-asset");
                asset = Asset::create(asset.id, asset.media_type, {std::byte{2}});
            } else history.back().action = "Replaced historical source";
            window.document() = Document::fork(replacement);
            require(window.document().revision() == source.revision() &&
                    window.document().snapshot().document_id() == source.document_id(),
                    "replacement fixture must retain the same document ID and revision");
            refuse(proposal);
            window.document() = Document::fork(source);
        }
        window.setWorkspace(Workspace::architectural);
        refuse(proposal);
        window.setWorkspace(Workspace::measurement);
        require(window.acceptAssistanceProposal(generate()) && window.document().revision() == source.revision() + 1,
                "a freshly generated current proposal must remain normally applicable");
        require(window.undoCommand() && window.document().snapshot().entities() == source.entities(),
                "guarded label acceptance must undo as one ordinary document operation");
    }
    MainWindow drawing;
    drawing.setAssistanceEnabled(true);
    require(drawing.beginMeasurementLinework(), "workspace refusal fixture must start an active drawing");
    const auto command = drawing.parseAssistanceCommand("set workspace architectural");
    const auto before = document_snapshot_digest(drawing.document().snapshot());
    require(command.size() == 1 && !drawing.acceptAssistanceProposal(command.front()) &&
            drawing.workspace() == Workspace::measurement &&
            document_snapshot_digest(drawing.document().snapshot()) == before,
            "a workspace operation blocked by an active drawing must report refusal without mutation");
}

void calibrated_reference_transform_workflow(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    using json = nlohmann::json;
    QImage image(96, 56, QImage::Format_ARGB32);
    image.fill(Qt::white);
    for (int y = 6; y <= 36; ++y)
        for (int x = 6; x <= (y < 28 ? 23 : 55); ++x)
            image.setPixelColor(x, y, Qt::black);
    const std::vector<std::pair<QRect, QColor>> markers{
        {QRect(12, 10, 6, 6), QColor(200, 0, 0)},
        {QRect(12, 25, 6, 6), QColor(0, 0, 200)},
        {QRect(43, 29, 6, 6), QColor(0, 80, 0)}};
    for (const auto& [rect, color] : markers)
        for (int y = rect.top(); y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x)
                image.setPixelColor(x, y, color);
    const auto path = QDir(directory).filePath("asymmetric-reference.png");
    require(image.save(path, "PNG"), "asymmetric transform fixture must save");
    int fixture_index = 0;
    for (const auto& [horizontal, vertical] :
         std::vector<std::pair<bool, bool>>{{false, false}, {true, false},
                                          {false, true}, {true, true}, {false, false}}) {
        MainWindow window;
        const auto reference_id = testing::importOrSeedTrustedReferenceFixture(window, path, image);
        window.setAssistanceEnabled(true);
        require(window.calibrateReference(reference_id, "0", "0", "20", "0", "1 m"),
                "asymmetric reference must calibrate through normal API");
        const double degrees = fixture_index == 4 ? 0.0 : 37.0;
        require(window.editReferenceTransform(reference_id, "6", "-3", "0.05", "1.7",
                    QString::number(degrees), "1", horizontal, vertical, true),
                "reference transform must retain centered placement, scale and mirrors");
        require(window.selectEntity({}), "reference capture must clear selection overlays");
        const auto source = window.document().snapshot();
        const auto source_reference = source.entities().at(reference_id.toStdString());
        auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
        require(canvas != nullptr, "actual MainWindow measurement canvas must exist");
        canvas->setGridEnabled(false);
        canvas->setOverviewMapEnabled(false);
        canvas->setSelectionControlsVisible(false);
        QImage capture(800, 600, QImage::Format_ARGB32);
        capture.fill(Qt::white);
        {
            QPainter painter(&capture);
            canvas->renderSceneAt(painter, capture.rect(), 40, {6, -3}, Qt::white);
        }
        // Qt's real renderer independently witnesses source-Y and mirroring.
        // QTransform mirrors the persisted drawImage target rectangle only;
        // the engine helper is intentionally not used for these expectations.
        QTransform source_to_model;
        source_to_model.translate(6, -3);
        source_to_model.rotate(degrees);
        source_to_model.scale(horizontal ? -0.085 : 0.085, vertical ? -0.085 : 0.085);
        source_to_model.translate(-48, -28);
        for (const auto& [rect, color] : markers) {
            const auto model = source_to_model.map(QPointF(rect.x() + 3.0, rect.y() + 3.0));
            const auto screen = QPoint(qRound(400 + (model.x() - 6) * 40),
                                       qRound(300 - (model.y() + 3) * 40));
            require(capture.rect().contains(screen), "reference marker must be inside capture");
            const auto actual = capture.pixelColor(screen);
            require(std::abs(actual.red() - color.red()) < 12 &&
                        std::abs(actual.green() - color.green()) < 12 &&
                        std::abs(actual.blue() - color.blue()) < 12,
                    "actual canvas pixels disagree with calibrated centered mirrored source mapping");
        }
        for (const auto kind : {AssistanceKind::tracing, AssistanceKind::edge_tracing}) {
            const auto before = window.document().snapshot();
            const auto proposals = window.suggestReferenceAssistance(reference_id, kind);
            require(proposals.size() == 1, "asymmetric reference must generate one trace");
            const auto& proposal = proposals.front();
            const auto& arguments = proposal.preview.arguments;
            require(document_snapshot_digest(window.document().snapshot()) == document_snapshot_digest(before),
                    "generating calibrated trace must preserve the complete source");
            require(arguments.at("source_document_digest") == document_snapshot_digest(before) &&
                        arguments.at("source_pixel_bounds") == json::array({6, 6, 55, 36}) &&
                        arguments.at("centered_source") == true &&
                        arguments.at("flip_horizontal") == horizontal &&
                        arguments.at("flip_vertical") == vertical,
                    "desktop trace must retain raw source bounds, transform and full source proof");
            require(proposal.source.x == 6.0 / 96 && proposal.source.y == 6.0 / 56 &&
                        proposal.source.width == 50.0 / 96 && proposal.source.height == 31.0 / 56,
                    "reference mirror must not rewrite raw normalized source selection");
            const std::vector<QPointF> pixels = kind == AssistanceKind::tracing
                ? std::vector<QPointF>{{6, 6}, {55, 6}, {55, 36}, {6, 36}}
                : std::vector<QPointF>{{6, 6}, {24, 6}, {24, 28}, {56, 28}, {56, 37}, {6, 37}};
            const auto& points = arguments.at("points");
            require(points.size() == pixels.size(), "reference transform must preserve L contour topology");
            for (const auto pixel : pixels) {
                const auto expected = source_to_model.map(pixel);
                bool found = false;
                for (const auto& actual : points)
                    found |= std::abs(actual.at(0).get<double>() - expected.x()) < 1e-10 &&
                             std::abs(actual.at(1).get<double>() - expected.y()) < 1e-10;
                require(found, "MainWindow trace must coincide with real canvas source coordinates");
            }
            require(window.acceptAssistanceProposal(proposal) &&
                        window.document().revision() == before.revision() + 1,
                    "transformed trace must accept as one normal history command");
            const auto accepted = window.document().snapshot();
            const auto boundary = decode_identified_boundary_entity(accepted.entities().at(proposal.id));
            require(boundary.segments.size() == points.size(), "accepted reference trace must retain analytical contour");
            for (std::size_t index = 0; index < points.size(); ++index) {
                const auto& start = boundary.segments[index].segment.start;
                require(std::abs(start.x - points[index][0].get<double>()) < 1e-10 &&
                            std::abs(start.y - points[index][1].get<double>()) < 1e-10,
                        "normal accept must persist the actual transformed proposal geometry");
            }
            const auto& provenance = accepted.entities().at(proposal.id).extensions.at("assistance_provenance");
            require(provenance.at("proposal") == encode_assistance_proposal(proposal) &&
                        provenance.at("accepted_operation") == "add_boundary" &&
                        provenance.at("accepted_entity_id") == proposal.id,
                    "accepted trace must retain the exact reviewed proposal and source transform proof");
            require(accepted.assets() == source.assets() &&
                        accepted.entities().at(reference_id.toStdString()) == source_reference,
                    "trace acceptance must preserve original reference asset and calibration provenance");
            require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                        window.document().snapshot().assets() == before.assets(),
                    "transformed trace Undo must restore complete source entities and assets");
            require(window.redoCommand() && window.document().snapshot().entities() == accepted.entities() &&
                        window.document().snapshot().assets() == accepted.assets(),
                    "transformed trace Redo must restore exact geometry and source proof");
            const auto project = QDir(directory).filePath(QString("reference-transform-%1.sketch").arg(fixture_index));
            require(window.saveProjectAs(project) && window.openProject(project) &&
                        window.document().snapshot().entities() == accepted.entities() &&
                        window.document().snapshot().assets() == accepted.assets(),
                    "transformed trace must save and reopen exact geometry and reference provenance");
            window.setAssistanceEnabled(true);
        }
        const auto before_invalid = document_snapshot_digest(window.document().snapshot());
        require(window.suggestReferenceAssistance(reference_id, static_cast<AssistanceKind>(1234)).empty() &&
                    !window.lastError().isEmpty() &&
                    document_snapshot_digest(window.document().snapshot()) == before_invalid,
                "unrecognized reference assistance kind must refuse without source mutation");
        for (const QString invalid_number : {QString("nan"), QString("inf")}) {
            for (int field = 0; field < 4; ++field) {
                require(!window.editReferenceTransform(reference_id,
                            field == 0 ? invalid_number : "6", field == 1 ? invalid_number : "-3",
                            "0.05", field == 2 ? invalid_number : "1.7",
                            field == 3 ? invalid_number : QString::number(degrees),
                            "1", horizontal, vertical, true) &&
                            document_snapshot_digest(window.document().snapshot()) == before_invalid,
                        "nonfinite reference transforms must refuse without source mutation");
            }
        }
        ++fixture_index;
    }
}

void oversized_reference_refusal(const QString& directory) {
    using namespace sketch;
    QImage image(8193, 1, QImage::Format_ARGB32);
    image.fill(Qt::white);
    const auto path = QDir(directory).filePath("oversized-reference.png");
    require(image.save(path, "PNG"), "bounded decoder refusal fixture must save");
    desktop::MainWindow window;
    const auto id = testing::importOrSeedTrustedReferenceFixture(window, path, image);
    require(window.calibrateReference(id, "0", "0", "20", "0", "1 m"),
            "oversized reference must retain valid calibration before decoder refusal");
    window.setAssistanceEnabled(true);
    const auto before = document_snapshot_digest(window.document().snapshot());
    for (const auto kind : {AssistanceKind::tracing, AssistanceKind::edge_tracing,
                            AssistanceKind::dimension_extraction}) {
        require(window.suggestReferenceAssistance(id, kind).empty() && !window.lastError().isEmpty() &&
                    document_snapshot_digest(window.document().snapshot()) == before,
                "oversized image header must refuse assistance without source mutation");
    }
}

void pdf_dimension_import_workflow(const QString& directory) {
    using namespace sketch;
    using json = nlohmann::json;
    QByteArray pdf("%PDF-1.4\n");
    std::vector<int> offsets{0};
    const QByteArray content("BT /F1 12 Tf 72 650 Td (Wall: 31 ft 6 in) Tj ET\n");
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
    worker.start(QDir(QCoreApplication::applicationDirPath()).filePath("vertex-import-worker.exe"),
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
        report.job_membership_verified =
        report.brokered_handles_verified = report.private_temporary_root_verified =
        report.immutable_module_roots_verified = report.fixed_search_applied = report.proj_offline_applied = true;
    report.output.resize(static_cast<std::size_t>(bytes.size()));
    std::memcpy(report.output.data(), bytes.constData(), report.output.size());
    const auto decoded = desktop::decodeReferenceBytes(pdf, "pdf", 0, {}, [&](const auto&) { return report; });
    require(decoded.source_text == "Wall: 31 ft 6 in" && decoded.text_runs.size() == 1,
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
    MeasurementLinework model;
    model.stroke_id = "reviewed-measured-line"; model.anchor = {0, 0};
    ConstructionReceipt receipt;
    receipt.segment_id = "reviewed-segment"; receipt.kind = BoundaryConstructionKind::line_to_point;
    receipt.start = model.anchor; receipt.chord_end = Vec2{10, 0};
    model.edges.push_back({receipt.segment_id, "reviewed-start", "reviewed-end", receipt});
    Entity target; target.id = model.stroke_id; target.type = "measurement_linework";
    target.required = true;
    target.properties["model"] = encode_measurement_linework_model(model);
    const auto context = organize_project(window.document().snapshot()).drawing_context(
        window.activeLayerId().toStdString());
    require(context && context->complete(), "workflow fixture must use a complete active drawing context");
    target.properties["property_id"] = context->property_id;
    target.properties["building_id"] = context->building_id;
    target.properties["floor_id"] = context->floor_id;
    target.properties["layer_id"] = context->layer_id;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(target)}, {}, "Seed measured line association target"});
    const auto generate = [&] { return window.suggestReferenceAssistance(id,
        AssistanceKind::dimension_extraction, QString::fromStdString(target.id),
        QString::fromStdString(receipt.segment_id)); };
    const auto proposals = generate();
    require(proposals.size() == 1 && proposals.front().source.original_text == "31 ft 6 in" &&
                std::abs(proposals.front().preview.arguments.at("length_metres").get<double>() - 9.6012) < 1e-9,
            "imported PDF embedded text must produce a parsed dimension proposal");
    require(proposals.front().preview.arguments.at("source_document_digest") ==
                document_snapshot_digest(window.document().snapshot()),
            "initial PDF proposal must bind the complete live source snapshot");
    const auto bounds = decoded.text_runs.front().bounds;
    const auto& source = proposals.front().source;
    require(source.x == bounds.x() && source.y == bounds.y() && source.width == bounds.width() &&
                source.height == bounds.height(), "PDF proposal must preserve its real selection bounds");
    const auto project = QDir(directory).filePath("dimensions.sketch");
    require(window.saveProjectAs(project) && window.openProject(project), "PDF text metadata must round-trip");
    window.setAssistanceEnabled(true);
    const auto reopened_proposals = generate();
    require(reopened_proposals.size() == 1 &&
                reopened_proposals.front().preview.arguments.at("source_document_digest") ==
                    document_snapshot_digest(window.document().snapshot()),
            "reopened PDF proposal must bind the complete current source snapshot");
    auto initial_content = proposals;
    auto reopened_content = reopened_proposals;
    // Saving changes saved_revision in the full snapshot digest. Compare every
    // other proposal field while independently checking both live digest fences.
    initial_content.front().preview.arguments.erase("source_document_digest");
    reopened_content.front().preview.arguments.erase("source_document_digest");
    require(reopened_content == initial_content,
            "reopened PDF must reproduce deterministic source bounds and proposals");
    const auto accepted_source = window.document().snapshot();
    const auto accepted_proposal = generate().front();
    const auto retained = encode_assistance_proposal(accepted_proposal);
    require(window.acceptAssistanceProposal(accepted_proposal), "reviewed measured-line association must accept");
    const auto accepted = window.document().snapshot();
    const auto& dimension = accepted.entities().at(accepted_proposal.id);
    const auto linked = decode_boundary_dimension_entity(dimension);
    require(linked.dimension && linked.dimension->boundary_id == target.id && linked.dimension->segment_id == receipt.segment_id &&
                accepted.entities().at(target.id) == accepted_source.entities().at(target.id) &&
                dimension.extensions.at("assistance_provenance").at("proposal") == retained &&
                dimension.extensions.at("assistance_provenance").at("linked_length_metres") == 10.0 &&
                std::abs(dimension.extensions.at("assistance_provenance").at("recognized_length_metres").get<double>() - 9.6012) < 1e-9,
            "association must retain the immutable proposal while preserving authoritative measured geometry");
    require(window.undoCommand() && !window.document().snapshot().entities().contains(accepted_proposal.id),
            "reviewed dimension association must undo normally");
    require(window.redoCommand() && window.document().snapshot().entities() == accepted.entities(),
            "reviewed association redo must restore exact entities and provenance");
    require(window.saveProjectAs(project) && window.openProject(project) &&
                window.document().snapshot().entities() == accepted.entities(),
            "reviewed association must save and reopen exact provenance");
    window.setAssistanceEnabled(true);
    // The saved head is a redo event and must exactly equal its source record.
    // Start an ordinary authored head before replacing its state at the same
    // ID/revision. Removing the reviewed annotation also makes the retained
    // proposal genuinely applicable if its complete-source fence is omitted.
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::erase(accepted_proposal.id)}, {}, "Prepare retained proposal source fixture"});
    const auto retained_source = window.document().snapshot();
    require(!retained_source.history().back().source_revision.has_value() &&
                !retained_source.entities().contains(accepted_proposal.id),
            "stale proposal fixture must use an authored head with no existing proposed dimension");
    for (const bool replace_asset : {false, true}) {
        window.document() = Document::fork(retained_source);
        const auto stale = generate().front();
        sketch::test::DetachedDocumentSnapshotFixture replacement(window.document().snapshot());
        auto& record = replacement.history().back();
        if (replace_asset) {
            require(!record.assets.empty(), "reference fixture must retain an asset for replacement fencing");
            auto& asset = record.assets.begin()->second;
            asset = Asset::create(asset.id, asset.media_type, {std::byte{42}});
        } else record.entities.at(target.id).extensions["same_revision_replacement"] = true;
        window.document() = Document::fork(replacement);
        const auto before = window.document().snapshot();
        require(!window.acceptAssistanceProposal(stale) &&
                    window.document().revision() == before.revision() &&
                    document_snapshot_digest(window.document().snapshot()) == document_snapshot_digest(before),
                "same-ID same-revision entity or asset replacement must reject retained proposals without mutation");
    }
}

void physical_wall_dimension_assistance_workflow(const QString& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    using json = nlohmann::json;
    MainWindow window;
    QImage image(80, 60, QImage::Format_ARGB32);
    image.fill(Qt::white);
    const auto path = QDir(directory).filePath("wall-dimension-observation.png");
    require(image.save(path, "PNG"), "wall observation fixture must save");
    const auto reference_id = testing::importOrSeedTrustedReferenceFixture(window, path, image);
    // This fixture supplies persisted text observations only; it does not attest
    // OCR execution or worker isolation. MainWindow owns association and acceptance.
    auto reference = window.document().snapshot().entities().at(reference_id.toStdString());
    const std::string text = "Wall: 12 ft";
    reference.properties["source_text"] = text;
    reference.properties["source_text_version"] = 1;
    reference.properties["source_text_runs"] = json::array({{
        {"offset", std::size_t{0}}, {"length", text.size()},
        {"x", 0.1}, {"y", 0.2}, {"width", 0.4}, {"height", 0.05}}});
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(reference)}, {}, "Seed wall text observation"});
    require(window.calibrateReference(reference_id, "0", "0", "20", "0", "1 m"),
            "wall text reference must use normal calibration");
    window.setAssistanceEnabled(true);
    const auto straight = window.createStraightWall({0, 0}, {3, 4});
    const auto curved = window.createCurvedWall({8, 0}, {10, 0}, "180 deg");
    require(!straight.isEmpty() && !curved.isEmpty(), "wall association fixture needs real supported walls");
    const auto source = window.document().snapshot();
    const auto generate = [&](const QString& wall) {
        return window.suggestReferenceAssistance(reference_id, AssistanceKind::dimension_extraction, wall);
    };
    const auto initial = generate(straight);
    require(initial.size() == 1 && initial.front().preview.command_type == "add_wall_dimension_suggestion" &&
                initial.front().preview.arguments.size() == 7 &&
                initial.front().preview.arguments.at("target_wall_id") == straight.toStdString() &&
                initial.front().preview.arguments.at("source_document_digest") == document_snapshot_digest(source) &&
                initial.front().preview.arguments.at("source_workspace") == static_cast<int>(window.workspace()) &&
                initial.front().source.original_text == "12 ft" &&
                document_snapshot_digest(window.document().snapshot()) == document_snapshot_digest(source),
            "wall association must produce an owner-only guarded observation without mutation");
    const auto refuse = [&](const AssistanceProposal& proposal) {
        const auto before = document_snapshot_digest(window.document().snapshot());
        const auto workspace = window.workspace();
        require(!window.acceptAssistanceProposal(proposal) && window.workspace() == workspace &&
                    document_snapshot_digest(window.document().snapshot()) == before,
                "invalid wall observations must refuse without document or workspace mutation");
    };
    for (int malformed = 0; malformed != 9; ++malformed) {
        auto proposal = initial.front();
        auto& args = proposal.preview.arguments;
        if (malformed == 0) args.erase("source_document_digest");
        else if (malformed == 1) args.erase("source_workspace");
        else if (malformed == 2) args["source_document_digest"] = 12;
        else if (malformed == 3) args["source_workspace"] = "measurement";
        else if (malformed == 4) args["length_metres"] = 99.0;
        else if (malformed == 5) args["source_text"] = "99 ft";
        else if (malformed == 6) args["target_wall_id"] = reference_id.toStdString();
        else if (malformed == 7) args["source_offset"] = -1;
        else args["target_segment_id"] = "invented-wall-edge";
        refuse(proposal);
    }
    for (int changed = 0; changed != 5; ++changed) {
        sketch::test::DetachedDocumentSnapshotFixture replacement(source);
        auto& record = replacement.history().back();
        if (changed == 0) record.entities.at(straight.toStdString()).properties["baseline"]["end"] = {6, 8};
        else if (changed == 1) record.entities.at(reference_id.toStdString()).properties["source_text"] = "Wall: 13 ft";
        else if (changed == 2) record.entities.at(window.activeLayerId().toStdString()).properties["name"] = "Replaced layer";
        else if (changed == 3) {
            auto& asset = record.assets.begin()->second;
            asset = Asset::create(asset.id, asset.media_type, {std::byte{42}});
        } else record.action = "Replaced source history";
        window.document() = Document::fork(replacement);
        require(window.document().revision() == source.revision() &&
                    window.document().snapshot().document_id() == source.document_id(),
                "wall retained-source fixture must preserve identity and revision");
        refuse(initial.front());
        window.document() = Document::fork(source);
    }
    window.setWorkspace(Workspace::architectural);
    refuse(initial.front());
    window.setWorkspace(Workspace::measurement);
    require(window.suggestReferenceAssistance(reference_id, AssistanceKind::dimension_extraction,
                straight, "invented-wall-edge").empty() &&
                document_snapshot_digest(window.document().snapshot()) == document_snapshot_digest(source),
            "wall association cannot accept a boundary segment selector");
    for (int malformed = 0; malformed != 3; ++malformed) {
        sketch::test::DetachedDocumentSnapshotFixture replacement(source);
        auto& wall = replacement.history().back().entities.at(curved.toStdString());
        if (malformed == 0) wall.extensions["curve_input"]["version"] = 999;
        else if (malformed == 1) wall.extensions["curve_input"]["radians"] = 1.0;
        else wall.properties["thickness_m"] = -1.0;
        window.document() = Document::fork(replacement);
        const auto before = document_snapshot_digest(window.document().snapshot());
        require(generate(curved).empty() && document_snapshot_digest(window.document().snapshot()) == before,
                "unsupported, stale or malformed wall sources cannot generate an association");
        auto proposal = initial.front();
        proposal.preview.arguments["target_wall_id"] = curved.toStdString();
        proposal.preview.arguments["source_document_digest"] = before;
        refuse(proposal);
        window.document() = Document::fork(source);
    }
    for (const auto& wall_id : {straight, curved}) {
        const auto before = window.document().snapshot();
        const auto proposals = generate(wall_id);
        require(proposals.size() == 1, "supported physical walls must produce one reviewed observation");
        const auto proposal = proposals.front();
        require(window.acceptAssistanceProposal(proposal), "reviewed wall association must accept");
        const auto accepted = window.document().snapshot();
        const auto& entity = accepted.entities().at(proposal.id);
        const auto decoded = decode_boundary_dimension_entity(entity);
        const auto length = wall_id == straight ? 5.0 : std::numbers::pi;
        require(decoded.supported() && decoded.dimension->kind == BoundaryDimensionKind::wall_axis_length &&
                    decoded.dimension->segment_id.empty() &&
                    entity.properties.at("dimension_version") == 4 &&
                    entity.properties.at("target") == json{{"entity_id", wall_id.toStdString()}} &&
                    std::abs(decoded.dimension->resolve(accepted).segment_length_metres - length) < 1e-9 &&
                    entity.extensions.at("assistance_provenance").at("proposal") == encode_assistance_proposal(proposal) &&
                    entity.extensions.at("assistance_provenance").at("accepted_operation") == "add_linked_wall_axis_dimension" &&
                    std::abs(entity.extensions.at("assistance_provenance").at("linked_length_metres").get<double>() - length) < 1e-9 &&
                    std::abs(entity.extensions.at("assistance_provenance").at("recognized_length_metres").get<double>() - 3.6576) < 1e-9,
                "association must persist typed live axis length and keep OCR measurement solely as provenance");
        require(accepted.revision() == before.revision() + 1 && accepted.history().size() == before.history().size() + 1 &&
                    accepted.entities().size() == before.entities().size() + 1 && accepted.assets() == before.assets(),
                "accepted wall association must add only one dimension in one command");
        for (const auto& [id, original] : before.entities())
            require(accepted.entities().at(id) == original, "wall association must preserve every source entity");
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id", "phase_id"})
            if (before.entities().at(wall_id.toStdString()).properties.contains(key))
                require(entity.properties.at(key) == before.entities().at(wall_id.toStdString()).properties.at(key),
                        "assisted wall dimension must retain source organization");
        require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                    window.redoCommand() && window.document().snapshot().entities() == accepted.entities(),
                "assisted wall dimension must Undo and Redo as one exact history operation");
    }
    const auto accepted = window.document().snapshot();
    const auto project = QDir(directory).filePath("assisted-wall-dimensions.sketch");
    require(window.saveProjectAs(project) && window.openProject(project) && window.document().is_editable() &&
                window.document().snapshot().entities() == accepted.entities() &&
                window.document().snapshot().assets() == accepted.assets(),
            "wall association must reopen editable with exact dimensions, observations and reference assets");
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
        desktop_proposal_guards();
        calibrated_reference_transform_workflow(temporary.path());
        oversized_reference_refusal(temporary.path());
        pdf_dimension_import_workflow(temporary.path());
        physical_wall_dimension_assistance_workflow(temporary.path());
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
        const auto before_ocr = document_snapshot_digest(window.document().snapshot());
        require(window.suggestReferenceAssistance(reference_id, AssistanceKind::dimension_extraction).empty(),
                "raster-only imported reference must yield no text dimension proposals");
        require(document_snapshot_digest(window.document().snapshot()) == before_ocr,
                "blank recognition or isolation refusal must preserve the document");
        if (!window.lastError().isEmpty())
            require(window.lastError().contains("OCR", Qt::CaseInsensitive) ||
                        window.lastError().contains("isolation", Qt::CaseInsensitive) ||
                        window.lastError().contains("unavailable", Qt::CaseInsensitive),
                    "unqualified OCR must report explicit recognition or isolation refusal");
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
        auto edge_trace = window.suggestReferenceAssistance(reference_id,
                                                                   AssistanceKind::edge_tracing);
        require(edge_trace.size() == 1 &&
                    edge_trace.front().preview.arguments.at("trace_mode") ==
                        "pixel-contours-v2" &&
                    edge_trace.front().preview.arguments.at("holes").size() == 1,
                "desktop must expose topology-preserving edge tracing");
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
        edge_trace = window.suggestReferenceAssistance(reference_id, AssistanceKind::edge_tracing);
        require(edge_trace.size() == 1, "edge trace must regenerate against the current history source");
        const auto edge_id = edge_trace.front().id;
        const auto hole_id = edge_trace.front().preview.arguments.at("hole_ids").at(0)
                                 .get<std::string>();
        auto malformed_edge = edge_trace.front();
        malformed_edge.preview.arguments["hole_ids"] = nlohmann::json::array();
        const auto malformed_revision = window.document().revision();
        require(!window.acceptAssistanceProposal(malformed_edge) &&
                    window.document().revision() == malformed_revision,
                "mismatched assisted contour topology must fail before document mutation");
        const auto revision_before_edge_trace = window.document().revision();
        require(window.acceptAssistanceProposal(edge_trace.front()),
                "accepted edge trace must use the desktop command path");
        require(window.document().revision() == revision_before_edge_trace + 1 &&
                    window.document().snapshot().entities().contains(edge_id) &&
                    window.document().snapshot().entities().contains(hole_id) &&
                    window.document().snapshot().entities().at(edge_id).properties.at(
                        "deduction_ids") == nlohmann::json::array({hole_id}),
                "accepted edge trace must atomically retain and link enclosed voids");
        const auto accepted_edge = window.document().snapshot();
        for (const auto& id : {edge_id, hole_id}) {
            const auto& provenance = accepted_edge.entities().at(id).extensions.at("assistance_provenance");
            require(provenance.at("proposal") == encode_assistance_proposal(edge_trace.front()) &&
                        provenance.at("accepted_operation") == "add_boundary" &&
                        provenance.at("accepted_entity_id") == id,
                    "outer and void trace entities must retain the exact reviewed source proposal");
        }
        require(window.undoCommand() &&
                    !window.document().snapshot().entities().contains(edge_id) &&
                    !window.document().snapshot().entities().contains(hole_id),
                "accepted topology-preserving edge trace must be undoable as one revision");

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
        dimension.preview.arguments["source_document_digest"] = document_snapshot_digest(window.document().snapshot());
        dimension.preview.arguments["source_workspace"] = static_cast<int>(window.workspace());
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
