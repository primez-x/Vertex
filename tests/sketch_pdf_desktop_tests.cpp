#include "sketch/desktop/main_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/output_fingerprint.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/sketch_pdf_output.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QPdfDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read retained PDF or fingerprint");
    return file.readAll();
}
struct Output {
    QSizeF points;
    QImage image;
    QString text;
};
Output output(const QString& path) {
    QPdfDocument pdf;
    require(pdf.load(path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
            "actual sketch PDF reopens as one page");
    const auto points = pdf.pagePointSize(0);
    const auto size = points.scaled(QSizeF(1300, 1100), Qt::KeepAspectRatio).toSize();
    auto image = pdf.render(0, size);
    require(!image.isNull(), "actual sketch PDF renders");
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (x >= 3 && y >= 3 && x < image.width() - 3 && y < image.height() - 3) continue;
            const auto color = image.pixelColor(x, y);
            require(color.red() > 245 && color.green() > 245 && color.blue() > 245,
                    "actual rendered PDF retains clear padding instead of cutting ink at page edges");
        }
    }
    return {points, image, pdf.getAllText(0).text()};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({
        {"p", "property", {{"name", "Sketch output"}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}, {"name", "Plan"}}, false},
        make_annotation_entity("annotations", AnnotationState{})
    }));
}
void checks() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary output directory exists");
    MainWindow window(fixture(), nullptr, directory.filePath("text-library.json"));
    window.setMetricUnits(false);
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900);
    window.show();
    QCoreApplication::processEvents();
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas, "actual measurement canvas exists");
    const auto empty = directory.filePath("empty.pdf");
    require(!window.exportSketchPdf(empty) && !QFileInfo::exists(empty),
            "empty sketch is refused without publishing an empty PDF");
    const Boundary room{
        {{0, 0}, {4, 0}, 0}, {{4, 0}, {4, 3}, 0},
        {{4, 3}, {0, 3}, 0}, {{0, 3}, {0, 0}, 0}};
    const auto room_id = window.createBoundary(room, "living");
    require(!room_id.isEmpty(), "create actual room");
    require(!window.createAreaDimension(room_id, {2, 2.5}).isEmpty(),
            "create actual associative area dimension for output and units checks");
    if (window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat", {2, 1.5}).isEmpty())
        throw std::runtime_error("create actual bundled SVG sofa: " + window.lastError().toStdString());
    const auto label = window.createAnnotationLabel("note", "Sketch crop text", {5, 2});
    require(!label.isEmpty(), "create retained text");
    if (!window.editAnnotation(label, "Sketch crop text", "5", "2", "35", "1", true,
                              "Inter", "8", "#111111", "#ffffff", true, false, true))
        throw std::runtime_error("edit rotated large retained text: " + window.lastError().toStdString());
    const auto project = directory.filePath("sketch.bldproj");
    require(window.saveProjectAs(project), "save retained drawing before output");
    const auto snapshot = window.document().snapshot();
    const auto base = directory.filePath("sketch.pdf");
    if (!window.exportSketchPdf(base))
        throw std::runtime_error("actual sketch export: " + window.lastError().toStdString());
    const auto original = read(base);
    require(original.startsWith("%PDF-") && original.contains("%%EOF"), "PDF is finalized");
    require(!QRegularExpression(QStringLiteral("/Subtype\\s*/Image"))
                 .match(QString::fromLatin1(original)).hasMatch(),
            "vector sketch does not embed a raster screenshot or underlay");
    const auto first = output(base);
    require(first.text.contains("Sketch crop text"), "rotated text remains selectable PDF text");
    const auto recorded = canvas->recordSketchContent();
    require(recorded.has_value(), "shared renderer records committed drawing");
    const auto expected_mm = QSizeF(recorded->ink_bounds.width() / recorded->pixels_per_mm + 4,
                                   recorded->ink_bounds.height() / recorded->pixels_per_mm + 4);
    require(std::abs(first.points.width() * 25.4 / 72 - expected_mm.width()) < 0.5 &&
            std::abs(first.points.height() * 25.4 / 72 - expected_mm.height()) < 0.5,
            "actual PDF page matches actual ink bounds plus two millimetres per edge");
    const auto sidecar = nlohmann::json::parse(read(base + ".fingerprint.json"));
    const auto stored = deserialize_output_fingerprint(sidecar.at("fingerprint"));
    require(stored.has_value() && sidecar.at("output_kind") == "sketch-pdf" &&
            sidecar.at("composition").at("architectural_scale_certified") == false,
            "content-only composition has a valid explicit fingerprint");
    require(sidecar.at("output_sha256") == QCryptographicHash::hash(original, QCryptographicHash::Sha256)
                .toHex().toStdString(), "fingerprint binds finalized actual PDF bytes");

    require(window.selectEntity(label), "select retained text");
    window.setSketchCompositionGuideEnabled(true);
    require(canvas->sketchCompositionGuideEnabled() && canvas->sketchCompositionGuideRect(),
            "guide displays exact nonprinting crop on canvas");
    auto* guide_action = window.findChild<QAction*>("sketchCompositionGuide");
    require(guide_action && guide_action->isChecked(), "guide command reflects public setter");
    canvas->setViewTransform({30, -20}, 350);
    canvas->setBoundaryPreview({{50, 50}, {100, 100}, {200, 50}});
    const auto changed = directory.filePath("navigation.pdf");
    require(window.exportSketchPdf(changed), "export while zoomed with selection guide and draft");
    const auto second = output(changed);
    require(first.points == second.points && first.image == second.image && first.text == second.text,
            "navigation selection guide and draft cannot change committed sketch PDF");
    require(window.document().snapshot().entities() == snapshot.entities() &&
            window.document().revision() == snapshot.revision(),
            "export and guide do not mutate project history");
    const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        canvas->clearPreview(); canvas->fitView();
        require(first.image.save(QDir(capture).filePath("sketch-pdf-render.png")), "capture real PDF");
        require(window.grab().save(QDir(capture).filePath("sketch-composition-guide.png")),
                "capture real native guide before adding tracing fixture");
        require(QFile::copy(base, QDir(capture).filePath("sketch.pdf")), "retain actual vector PDF");
    }

    QImage trace(500, 400, QImage::Format_ARGB32_Premultiplied);
    trace.fill(QColor(255, 0, 0));
    const auto trace_path = directory.filePath("trace.png");
    require(trace.save(trace_path), "save actual tracing asset fixture");
    // The fixture is an application-authored trusted PNG. Import-sandbox
    // attestation is separately qualified; this check exercises output of a
    // retained reference and must not weaken that importer to run in a build.
    const auto trace_bytes = read(trace_path);
    std::vector<std::byte> trace_asset_bytes;
    for (const auto value : trace_bytes) trace_asset_bytes.push_back(static_cast<std::byte>(value));
    auto asset = Asset::create("trace-asset", "image/png", std::move(trace_asset_bytes));
    Entity reference{"trace-reference", "reference_asset", {
        {"asset_id", "trace-asset"}, {"render_asset_id", "trace-asset"},
        {"position_m", {0, 0}}, {"metres_per_source_unit", .01}, {"scale", 1.0},
        {"rotation_degrees", 0.0}, {"visible", true}, {"intensity", .72},
        {"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"}, {"layer_id", "l"}
    }, false};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(reference)}, {AssetChange::upsert(asset)}, "Add retained reference output fixture"});
    const auto with_trace = directory.filePath("trace-excluded.pdf");
    require(window.exportSketchPdf(with_trace), "export with retained underlay");
    const auto third = output(with_trace);
    require(first.points == third.points && first.image == third.image,
            "visible tracing underlay cannot expand crop or enter sketch PDF");
    window.setMetricUnits(true);
    const auto metric_path = directory.filePath("metric.pdf");
    require(window.exportSketchPdf(metric_path), "metric sketch PDF exports");
    const auto metric = output(metric_path);
    require(metric.text.contains("Sketch crop text") && metric.text != third.text,
            "actual metric output retains text and updates dimensional presentation");
    window.setMetricUnits(false);
    PlanCanvas oversize;
    CanvasEntity long_line;
    long_line.id = "oversize"; long_line.type = "annotation_line";
    long_line.segments = {{{0, 0}, {250, 10}, 0}};
    oversize.setEntities({long_line});
    auto too_large = oversize.recordSketchContent();
    QString limit_diagnostic;
    require(too_large && make_sketch_pdf(std::move(*too_large), &limit_diagnostic).isEmpty() &&
            limit_diagnostic.contains("page limit"),
            "actual oversized vector composition refuses the PDF page limit instead of scaling silently");

    const auto fingerprint_path = base + ".fingerprint.json";
    require(QFile::remove(fingerprint_path) && QDir().mkdir(fingerprint_path),
            "block sidecar destination");
    require(!window.exportSketchPdf(base) && read(base) == original,
            "sidecar preflight failure preserves previous PDF bytes");
    const auto absent = directory.filePath("absent.pdf");
    require(QDir().mkdir(absent + ".fingerprint.json") && !window.exportSketchPdf(absent) &&
            !QFileInfo::exists(absent), "failed output preflight cannot publish a new PDF");
    require(!window.exportSketchPdf(directory.filePath("missing/drawing.pdf")),
            "missing destination directory reports failure");
    require(window.saveProject() && window.openProject(project), "save and reopen actual source");
    const auto reopen = directory.filePath("reopened.pdf");
    require(window.exportSketchPdf(reopen), "reopened source exports");
    const auto fourth = output(reopen);
    require(first.points == fourth.points && first.image == fourth.image,
            "native save and reopen preserve sketch composition exactly");
}
}  // namespace
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    application.setFont(QFont(QStringLiteral("Inter"), 10));
    try { checks(); std::cout << "sketch PDF desktop checks passed\n"; return 0; }
    catch (const std::exception& error) {
        std::cerr << "sketch_pdf_desktop_tests: " << error.what() << '\n'; return 1;
    }
}
