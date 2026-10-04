#include "sketch/desktop/symbol_svg_palette.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using sketch::SymbolSvgPalette;
using sketch::desktop::colored_symbol_svg;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const QByteArray simple = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" data-vertex-style="white-outline-2" viewBox="0 0 100 100"><rect x="20" y="20" width="60" height="60" fill="#ffffff" stroke="#111111" stroke-width="4"/><path d="M30 50 L70 50" fill="none" stroke="#b8b8b8"/><circle cx="50" cy="65" r="3" fill="#333333"/></svg>)SVG";

struct Node {
    QString name;
    QMap<QString, QString> attributes;
    QString gradient;
    bool operator==(const Node&) const = default;
};

std::vector<Node> nodes(const QByteArray& document) {
    QXmlStreamReader xml(document);
    std::vector<Node> result;
    std::vector<QString> gradients;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name().toString();
            const auto gradient = name.endsWith(QStringLiteral("Gradient"))
                ? xml.attributes().value(QStringLiteral("id")).toString().section(QStringLiteral("--"), -1)
                : gradients.empty() ? QString{} : gradients.back();
            gradients.push_back(gradient);
            Node node{name, {}, gradient};
            for (const auto& attribute : xml.attributes())
                node.attributes.insert(attribute.qualifiedName().toString(), attribute.value().toString());
            result.push_back(std::move(node));
        } else if (xml.isEndElement()) {
            gradients.pop_back();
        }
    }
    require(!xml.hasError(), "test XML snapshot could not parse artwork");
    return result;
}

QByteArray asset(const QString& relative) {
    QFile file(QStringLiteral(VERTEX_SYMBOL_PALETTE_ASSET_ROOT) + QLatin1Char('/') + relative);
    require(file.open(QIODevice::ReadOnly), "catalog fixture missing");
    return file.readAll();
}

void structuralContract(const QByteArray& source) {
    // Removing role-specific stop coloring or changing geometry/opacity must
    // fail this comparison across every catalog asset.
    const auto original = source;
    const auto digest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
    const SymbolSvgPalette palette{"white-outline-2", "#204060", "#804020"};
    const auto derivative = colored_symbol_svg(source, palette);
    auto expected = nodes(source);
    const QStringList tinted{"ceramic", "rim", "steel", "upholstery", "cushion", "linen",
                             "wood", "counter", "foliage", "leaf", "tread"};
    for (auto& node : expected) {
        if (node.attributes.value(QStringLiteral("stroke")) == QStringLiteral("#111111"))
            node.attributes[QStringLiteral("stroke")] = QStringLiteral("#204060");
        const auto fill = node.attributes.value(QStringLiteral("fill"));
        if (fill == QStringLiteral("#ffffff")) node.attributes[QStringLiteral("fill")] = QStringLiteral("#804020");
        if (fill == QStringLiteral("#f2f2f2")) node.attributes[QStringLiteral("fill")] = QStringLiteral("#793d1e");
        if (node.name == QStringLiteral("stop") && tinted.contains(node.gradient)) {
            const auto stop = node.attributes.value(QStringLiteral("stop-color"));
            if (stop == QStringLiteral("#ffffff")) node.attributes[QStringLiteral("stop-color")] = QStringLiteral("#804020");
            if (stop == QStringLiteral("#f2f2f2")) node.attributes[QStringLiteral("stop-color")] = QStringLiteral("#793d1e");
        }
    }
    require(nodes(derivative) == expected, "paint derivative changed protected colors, geometry, gradients, or opacity");
    require(source == original && QCryptographicHash::hash(source, QCryptographicHash::Sha256) == digest,
            "paint copy altered original artwork identity");
    QSvgRenderer renderer(derivative);
    require(renderer.isValid(), "colored catalog artwork cannot render");
}

void rejects(QByteArray document, SymbolSvgPalette palette = {}) {
    bool rejected = false;
    try { (void)colored_symbol_svg(document, palette); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "unsupported palette or unsafe artwork was silently accepted");
}

sketch::desktop::CanvasEntity entity(QString id, double x,
                                    std::optional<SymbolSvgPalette> palette = std::nullopt) {
    sketch::desktop::CanvasEntity result;
    result.id = std::move(id);
    result.type = QStringLiteral("symbol");
    sketch::desktop::CanvasSvgSymbol symbol;
    symbol.document = simple;
    symbol.artwork_sha256 = QCryptographicHash::hash(simple, QCryptographicHash::Sha256).toHex();
    symbol.view_box = symbol.footprint_view_box = QRectF(0, 0, 100, 100);
    symbol.position = {x, 0};
    symbol.width_metres = symbol.depth_metres = 1.0;
    symbol.svg_palette = std::move(palette);
    result.svg_symbol = std::move(symbol);
    return result;
}

QImage output(sketch::desktop::PlanCanvas& canvas) {
    QImage image(600, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    canvas.renderSceneAt(painter, QRectF(image.rect()), 150, {0, 0}, Qt::transparent);
    return image;
}

void renderingContract() {
    // A cache keyed only by original artwork will wrongly reuse the first
    // instance's palette for the second. Exercise both population orders.
    const SymbolSvgPalette red{"white-outline-2", "#111111", "#ff0000"};
    const SymbolSvgPalette blue{"white-outline-2", "#111111", "#0000ff"};
    const auto left = entity(QStringLiteral("red"), -1, red);
    const auto right = entity(QStringLiteral("blue"), 1, blue);
    sketch::desktop::PlanCanvas canvas;
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setEntities({left, right});
    const auto first = output(canvas);
    require(first.pixelColor(150, 130) == QColor(Qt::red) && first.pixelColor(450, 130) == QColor(Qt::blue),
            "same artwork instances reused another palette");
    sketch::desktop::PlanCanvas reversed;
    reversed.setGridEnabled(false);
    reversed.setEntities({right, left});
    require(output(reversed) == first, "palette cache depends on scene order");
    canvas.setSelectedIds({left.id, right.id});
    require(output(canvas) == first, "selection changed shared output artwork");
    require(first.pixelColor(150, 85).alpha() == 0, "palette added a rectangular selection/background fill");
    const auto retained = canvas.entities()[0].svg_symbol;
    require(retained->document == simple && retained->svg_palette->surface_color == "#ff0000",
            "retained scene lost source bytes or explicit palette");

    canvas.setEntities({entity(QStringLiteral("legacy"), -1), right});
    const auto absent = output(canvas);
    require(absent.pixelColor(150, 130) == QColor(Qt::white), "absent palette changed legacy artwork");
    require(canvas.entities()[0].svg_symbol->document == simple, "legacy source bytes changed");
    canvas.setEntities({entity(QStringLiteral("invalid"), -1,
                        SymbolSvgPalette{"wrong", "#111111", "#ffffff"})});
    (void)output(canvas); // Paint callbacks must contain validation errors.

    canvas.resize(600, 300);
    canvas.setEntities({left, right});
    canvas.fitView();
    canvas.setSelectedIds({});
    const auto screen = canvas.grab().toImage();
    canvas.setSelectedId(left.id);
    const auto selected = canvas.grab().toImage();
    const auto frame = canvas.selectionBounds();
    require(frame.has_value(), "colored symbol selection has no frame");
    const auto interior = frame->adjusted(12, 12, -12, -12).toAlignedRect().intersected(screen.rect());
    require(!interior.isEmpty() && selected.copy(interior) == screen.copy(interior),
            "selection obscured colored artwork or transparent interior");
    int red_pixels = 0;
    for (int y = interior.top(); y <= interior.bottom(); ++y)
        for (int x = interior.left(); x <= interior.right(); ++x) {
            const auto pixel = screen.pixelColor(x, y);
            if (pixel.red() > 220 && pixel.green() < 20 && pixel.blue() < 20) ++red_pixels;
        }
    require(red_pixels > 100, "interactive canvas did not use the explicit palette");

    QTemporaryDir directory;
    require(directory.isValid(), "PDF fixture directory unavailable");
    const auto path = directory.filePath(QStringLiteral("palette.pdf"));
    { QPdfWriter pdf(path);
      pdf.setResolution(72);
      pdf.setPageSize(QPageSize(QSizeF(600, 300), QPageSize::Point));
      pdf.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
      QPainter painter(&pdf);
      require(painter.isActive(), "PDF painter failed");
      canvas.renderSceneAt(painter, QRectF(0, 0, 600, 300), 150, {0, 0}, Qt::transparent); }
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.readAll().startsWith("%PDF"),
            "shared colored scene did not reach PDF output");
    file.close();
    QPdfDocument pdf;
    require(pdf.load(path) == QPdfDocument::Error::None, "shared colored PDF could not reopen");
    const auto page = pdf.render(0, QSize(600, 300));
    require(!page.isNull(), "colored PDF could not rasterize");
    const auto red_pixel = page.pixelColor(150, 130);
    const auto blue_pixel = page.pixelColor(450, 130);
    require(red_pixel.red() > 220 && red_pixel.green() < 20 && red_pixel.blue() < 20 &&
            blue_pixel.blue() > 220 && blue_pixel.red() < 20 && blue_pixel.green() < 20,
            "PDF output lost per-instance palette colors");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    try {
        structuralContract(simple);
        QDirIterator files(QStringLiteral(VERTEX_SYMBOL_PALETTE_ASSET_ROOT),
                           QStringList{QStringLiteral("*.svg")}, QDir::Files, QDirIterator::Subdirectories);
        int count = 0;
        while (files.hasNext()) {
            QFile file(files.next());
            require(file.open(QIODevice::ReadOnly), "catalog SVG unavailable");
            structuralContract(file.readAll());
            ++count;
        }
        require(count == 342, "complete supplied and Pinc-adoption SVG catalog was not exercised");
        for (const auto& path : {"04_living/sofa-three-seat.svg", "01_bathroom/basin-round.svg",
                                 "02_kitchen/cooktop-gas-four.svg", "25_drafting_symbols/north-arrow.svg"})
            structuralContract(asset(QString::fromLatin1(path)));
        auto malformed = simple;
        malformed.replace("</svg>", "</g>"); rejects(malformed);
        auto unknown = simple;
        unknown.replace("white-outline-2", "arbitrary"); rejects(unknown);
        rejects(simple, {"other", "#111111", "#ffffff"});
        rejects(simple, {"white-outline-2", "red", "#ffffff"});
        rejects(simple, {"white-outline-2", "#111111", "#80ffffff"});
        rejects("<!DOCTYPE svg [<!ENTITY secret SYSTEM 'file:///private'>]>" + simple);
        rejects("<?xml-stylesheet href='https://example.com/style.css'?>" + simple);
        unknown = simple;
        unknown.replace("<rect", "<image href='https://example.com/art.svg'/><rect"); rejects(unknown);
        unknown = simple;
        unknown.replace("<rect", "<defs><linearGradient id='x--unknown'><stop stop-color='#ffffff'/></linearGradient></defs><rect");
        rejects(unknown);
        unknown = simple;
        unknown.replace("fill=\"#ffffff\"", "style=\"fill:#ffffff\""); rejects(unknown);
        unknown = simple;
        unknown.replace("<rect", "<script>alert(1)</script><rect"); rejects(unknown);
        unknown = simple;
        unknown.replace("fill=\"#ffffff\"", "fill=\"url(https://example.com/paint.svg)\""); rejects(unknown);
        unknown = simple;
        unknown.replace("fill=\"#ffffff\"", "fill=\"url(#missing-gradient)\""); rejects(unknown);
        unknown = simple;
        unknown.replace("http://www.w3.org/2000/svg", "https://example.com/custom"); rejects(unknown);
        renderingContract();
        std::cout << "symbol SVG palette checks passed (342 catalog assets)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "symbol_svg_palette_tests: " << error.what() << '\n';
        return 1;
    }
}
