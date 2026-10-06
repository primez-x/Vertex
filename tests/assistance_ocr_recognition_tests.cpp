#include "assistance_ocr.hpp"
#include "assistance_ocr_recognizer.hpp"
#include <QGuiApplication>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QRawFont>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
sketch::AssistanceRaster rasterFrom(const QImage& image) {
    sketch::AssistanceRaster raster;
    raster.reference_id = "real-ocr-fixture";
    raster.width = image.width(); raster.height = image.height();
    const auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y)
        raster.luminance.insert(raster.luminance.end(), gray.constScanLine(y),
                                gray.constScanLine(y) + gray.width());
    return raster;
}
}
int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        using namespace sketch;
        using namespace sketch::desktop;
        require(argc == 2, "recognition fixture requires the source root containing pinned assets");
        // Deliberate direct codec evidence using source-root assets. This does
        // not qualify the production AppContainer broker or installed package.
        const auto root = std::filesystem::path(QString::fromUtf8(argv[1]).toStdWString());
        // Offscreen Qt may have no system font database. Use the repository's
        // real font explicitly so the fixture contains letters, not tofu boxes.
        const auto font_id = QFontDatabase::addApplicationFont(
            QString::fromStdWString((root / "assets/fonts/Inter.ttf").wstring()));
        require(font_id >= 0, "real OCR fixture font must load");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "real OCR fixture font must expose a family");
        QFont font(families.front()); font.setPixelSize(72);
        const auto raw_font = QRawFont::fromFont(font);
        const QString labels = "Wall: 31 ft 6 in Width: 4.25 m Room: Kitchen";
        require(raw_font.isValid(), "real OCR fixture font must resolve");
        for (const auto character : labels)
            require(raw_font.supportsCharacter(character), "real OCR fixture font must contain every rendered glyph");
        QImage image(1600, 650, QImage::Format_Grayscale8);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setFont(font); painter.setPen(Qt::black);
            painter.drawText(80, 150, "Wall: 31 ft 6 in");
            painter.drawText(80, 330, "Width: 4.25 m");
            painter.drawText(80, 510, "Room: Kitchen");
        }
        auto raster = rasterFrom(image);
        const auto result = validateAssistanceOcrReply(recognizeAssistanceOcrFrame(
            encodeAssistanceOcrFrame(raster), root));
        // Preserve the exact codec reply for failure diagnosis and optional
        // private visual review; never substitute text for the rendered pixels.
        const auto reply = encodeAssistanceOcrReply(result);
        std::cerr << "Actual OCR reply: "
                  << std::string(reinterpret_cast<const char*>(reply.data()), reply.size()) << '\n';
        const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture_directory.isEmpty()) {
            QDir directory(capture_directory);
            require(directory.mkpath("."), "OCR capture directory must be writable");
            require(image.save(directory.filePath("assistance-real-ocr.png"), "PNG"),
                    "real OCR raster capture must save");
            QFile capture(directory.filePath("assistance-real-ocr-reply.json"));
            require(capture.open(QIODevice::WriteOnly) &&
                    capture.write(reinterpret_cast<const char*>(reply.data()),
                                  static_cast<qint64>(reply.size())) == static_cast<qint64>(reply.size()),
                    "real OCR reply capture must save");
        }
        require(result.text.find("31 ft 6 in") != std::string::npos &&
                result.text.find("4.25 m") != std::string::npos &&
                result.text.find("Kitchen") != std::string::npos,
                "real recognizer must read compound imperial, metric and room note pixels");
        require(result.producer == assistanceOcrProducer && result.resources.size() == 2 &&
                !result.isolation_controls_attested, "codec evidence must preserve pinned provenance without sandbox attestation");
        require(result.runs.size() == 3, "three separate rendered lines must have real selection regions");
        for (std::size_t i = 0; i < result.runs.size(); ++i) {
            const auto& run = result.runs[i];
            require(run.confidence && *run.confidence > 0 && *run.confidence <= 1 &&
                    run.x >= 0.04 && run.x <= 0.08 && run.width > 0.2 && run.height > 0.05 &&
                    run.y > (double(i) * 180 + 50) / 650 &&
                    run.y < (double(i) * 180 + 150) / 650,
                    "recognizer must return measured confidence and bounds at rendered line positions");
        }
        raster.source_text = result.text; raster.text_runs = result.runs;
        raster.text_resources = result.resources; raster.text_producer = result.producer;
        const auto proposals = extract_dimensions(raster, {}, "wall-target", "wall-segment");
        require(proposals.size() == 2, "room note must not fabricate a dimension");
        for (std::size_t i = 0; i < proposals.size(); ++i) {
            const auto& proposal = proposals[i];
            const auto expected = i == 0 ? 9.6012 : 4.25;
            require(std::abs(proposal.preview.arguments.at("length_metres").get<double>() - expected) < 1e-9 &&
                    proposal.source.reference_id == raster.reference_id &&
                    proposal.source.confidence == *result.runs[i].confidence &&
                    proposal.source.x == result.runs[i].x && proposal.source.y == result.runs[i].y &&
                    proposal.producer == result.producer &&
                    std::all_of(result.resources.begin(), result.resources.end(), [&](const auto& resource) {
                        return std::find(proposal.resources.begin(), proposal.resources.end(), resource) != proposal.resources.end();
                    }) &&
                    proposal.preview.arguments.at("target_segment_id") == "wall-segment",
                    "dimension proposals must preserve actual recognized quantity, selection and provenance");
        }
        image.fill(Qt::white);
        auto blank = rasterFrom(image);
        const auto empty = validateAssistanceOcrReply(recognizeAssistanceOcrFrame(
            encodeAssistanceOcrFrame(blank), root));
        blank.source_text = empty.text; blank.text_runs = empty.runs;
        require(empty.runs.empty() && extract_dimensions(blank).empty(),
                "blank pixels must yield no fabricated text dimensions");
        std::cout << "actual OCR codec recognition tests passed; production isolation not qualified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
