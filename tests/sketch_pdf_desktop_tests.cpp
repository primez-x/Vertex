#include "sketch/desktop/main_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/output_fingerprint.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/sketch_pdf_output.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QBuffer>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPainter>
#include <QPicture>
#include <QRawFont>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>
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
QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
nlohmann::json text_evidence(const QString& text) {
    auto units = nlohmann::json::array();
    for (const auto unit : text)
        units.push_back(QStringLiteral("%1").arg(static_cast<unsigned int>(unit.unicode()), 4, 16, QLatin1Char('0')).toStdString());
    return {{"text", text.toStdString()}, {"utf8_hex", text.toUtf8().toHex().toStdString()},
            {"utf16_units_hex", units}};
}
nlohmann::json font_evidence(const QFont& font) {
    const auto raw = QRawFont::fromFont(font);
    auto tables = nlohmann::json::object();
    for (const auto* tag : {"head", "name", "cmap", "OS/2", "GSUB"})
        tables[tag] = sha256(raw.fontTable(tag)).toStdString();
    const auto os2 = raw.fontTable("OS/2");
    const int fs_type = os2.size() >= 10
        ? (static_cast<unsigned char>(os2[8]) << 8) | static_cast<unsigned char>(os2[9]) : -1;
    return {{"requested", font.toString().toStdString()}, {"raw_valid", raw.isValid()},
            {"resolved_family", raw.familyName().toStdString()},
            {"resolved_style", raw.styleName().toStdString()}, {"weight", raw.weight()},
            {"pixel_size", raw.pixelSize()}, {"fs_type", fs_type}, {"tables_sha256", tables}};
}
struct TextControl {
    QByteArray bytes;
    nlohmann::json evidence;
};
TextControl text_control(const QFont& font, const QString& text, int dpi,
                         double degrees, bool via_picture) {
    TextControl result;
    QBuffer buffer(&result.bytes);
    bool started = buffer.open(QIODevice::WriteOnly);
    bool recorded = !via_picture;
    bool finished = false;
    if (started) {
        QPdfWriter writer(&buffer);
        writer.setResolution(dpi);
        QPainter painter(&writer);
        started = painter.isActive();
        if (started) {
            painter.translate(writer.width() / 2.0, writer.height() / 2.0);
            const auto draw = [&](QPainter& target) {
                target.setFont(font);
                target.setPen(Qt::black);
                target.rotate(degrees);
                target.drawText(QRectF(-500, -100, 1000, 200), Qt::AlignCenter, text);
            };
            if (via_picture) {
                QPicture picture;
                QPainter recorder(&picture);
                recorded = recorder.isActive();
                if (recorded) {
                    draw(recorder);
                    recorded = recorder.end() && picture.play(&painter);
                }
            } else {
                draw(painter);
            }
            finished = painter.end();
        }
    }
    buffer.close();
    QBuffer input(&result.bytes);
    const bool input_open = input.open(QIODevice::ReadOnly);
    QPdfDocument pdf;
    if (input_open) pdf.load(&input);
    const auto extracted = pdf.pageCount() == 1 ? pdf.getAllText(0).text() : QString{};
    result.evidence = {{"degrees", degrees}, {"via_picture", via_picture},
        {"started", started}, {"recorded", recorded}, {"finished", finished},
        {"pdf_error", static_cast<int>(pdf.error())}, {"page_count", pdf.pageCount()},
        {"pdf_sha256", sha256(result.bytes).toStdString()}, {"extracted", text_evidence(extracted)},
        {"expected_text_present", extracted.contains(text)}};
    return result;
}
void diagnose_selectable_text(const PlanCanvas& canvas, const QString& label_id,
                             const Output& actual, const QByteArray& pdf_bytes) {
    // Preserve the assertion. Retain the failed boundary before its temporary
    // directory unwinds; the controls do not change the authored scene.
    const auto expected = QStringLiteral("Sketch crop text");
    QFile resource(QStringLiteral(":/fonts/Inter.ttf"));
    const bool resource_open = resource.open(QIODevice::ReadOnly);
    const auto font_bytes = resource_open ? resource.readAll() : QByteArray{};
    nlohmann::json evidence{{"qt_runtime", qVersion()}, {"qt_compile", QT_VERSION_STR},
        {"platform", QGuiApplication::platformName().toStdString()},
        {"platform_environment", qEnvironmentVariable("QT_QPA_PLATFORM").toStdString()},
        {"arguments", nlohmann::json::array()}, {"library_paths", nlohmann::json::array()},
        {"application_font", font_evidence(QApplication::font())},
        {"canvas_font", font_evidence(canvas.font())},
        {"resource_open", resource_open}, {"resource_bytes", font_bytes.size()},
        {"resource_sha256", sha256(font_bytes).toStdString()},
        {"pdf_sha256", sha256(pdf_bytes).toStdString()},
        {"pdf_has_font_descriptor", pdf_bytes.contains("/FontDescriptor")},
        {"pdf_has_to_unicode", pdf_bytes.contains("/ToUnicode")},
        {"expected", text_evidence(expected)}, {"actual", text_evidence(actual.text)},
        {"expected_text_present", actual.text.contains(expected)},
        {"controls", nlohmann::json::array()}};
    for (const auto& argument : QCoreApplication::arguments())
        evidence["arguments"].push_back(argument.toStdString());
    for (const auto& library_path : QCoreApplication::libraryPaths())
        evidence["library_paths"].push_back(library_path.toStdString());
    std::vector<TextControl> controls;
    const auto recording = canvas.recordSketchContent();
    for (const auto& label : canvas.labels()) {
        if (label.id != label_id) continue;
        const int dpi = recording ? recording->picture.logicalDpiY() : canvas.logicalDpiY();
        auto font = canvas.font();
        if (!label.font_family.isEmpty()) font.setFamily(label.font_family);
        font.setPixelSize(static_cast<int>(std::lround(label.paper_height_mm * dpi / 25.4)));
        font.setBold(label.bold); font.setItalic(label.italic);
        font.setFeature("calt", 0); font.setFeature("case", 0);
        evidence["label"] = {{"text", label.text.toStdString()},
            {"paper_height_mm", label.paper_height_mm}, {"dpi", dpi},
            {"rotation_radians", label.rotation_radians}, {"font", font_evidence(font)}};
        for (const bool via_picture : {false, true}) {
            for (const double degrees : {0.0, -35.0}) {
                controls.push_back(text_control(font, label.text, dpi, degrees, via_picture));
                evidence["controls"].push_back(controls.back().evidence);
            }
        }
    }
    std::cerr << "sketch PDF selectable-text diagnostic: " << evidence.dump() << '\n';
    const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (capture.isEmpty()) return;
    if (!QDir().mkpath(capture)) {
        std::cerr << "sketch PDF diagnostic capture directory could not be created\n";
        return;
    }
    const auto retain = [&](const QString& name, const QByteArray& bytes) {
        QFile file(QDir(capture).filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
            std::cerr << "sketch PDF diagnostic capture failed: " << name.toStdString() << '\n';
    };
    retain(QStringLiteral("sketch-pdf-text-failure.pdf"), pdf_bytes);
    retain(QStringLiteral("sketch-pdf-text-failure.txt"), actual.text.toUtf8());
    retain(QStringLiteral("sketch-pdf-text-diagnostic.json"), QByteArray::fromStdString(evidence.dump(2)));
    retain(QStringLiteral("sketch-pdf-loaded-Inter.ttf"), font_bytes);
    for (std::size_t index = 0; index < controls.size(); ++index)
        retain(QStringLiteral("sketch-pdf-text-control-%1.pdf").arg(static_cast<qulonglong>(index)),
               controls[index].bytes);
    if (!actual.image.save(QDir(capture).filePath(QStringLiteral("sketch-pdf-text-failure.png"))))
        std::cerr << "sketch PDF diagnostic render capture failed\n";
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
    // PDFium may group rotated words into separate reading-order lines. Keep
    // every word and glyph mandatory while accepting extracted whitespace.
    const bool selectable_phrase = first.text.simplified().contains("Sketch crop text");
    if (!selectable_phrase ||
        qEnvironmentVariableIntValue("VERTEX_TEST_PDF_TEXT_DIAGNOSTICS") != 0)
        diagnose_selectable_text(*canvas, label, first, original);
    require(selectable_phrase, "rotated text remains selectable PDF text");
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
    require(metric.text.simplified().contains("Sketch crop text") && metric.text != third.text,
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
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled Inter application font loads");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        checks(); std::cout << "sketch PDF desktop checks passed\n"; return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "sketch_pdf_desktop_tests: " << error.what() << '\n'; return 1;
    }
}
