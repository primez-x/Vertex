#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_catalog.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

sketch::desktop::CanvasEntity retainedSymbol(sketch::desktop::MainWindow& window,
                                            const QString& id) {
    const auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas != nullptr, "measurement canvas is missing");
    const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [&](const auto& entity) { return entity.id == id; });
    require(found != canvas->entities().end(), "placed SVG symbol is not retained by the canvas");
    return *found;
}

QByteArray checkedPayload(const sketch::desktop::CanvasEntity& entity) {
    require(entity.svg_symbol.has_value(), "placed canvas entity has no SVG payload");
    const auto& symbol = *entity.svg_symbol;
    require(symbol.catalog_id == QStringLiteral("svg-v2-04_living-sofa-three-seat") &&
                !symbol.document.isEmpty(), "retained SVG identity or document is missing");
    QSvgRenderer renderer(symbol.document);
    require(renderer.isValid(), "retained SVG payload cannot be rendered");
    require(symbol.view_box.isValid() && symbol.footprint_view_box.isValid() &&
                symbol.view_box == renderer.viewBoxF(),
            "retained SVG coordinate bounds must match the document");
    require(std::abs(symbol.width_metres - 2.25) < 1e-9 &&
                std::abs(symbol.depth_metres - 0.95) < 1e-9,
            "sofa nominal footprint must reach the renderer without padding");
    require(symbol.footprint_view_box == QRectF(0, 0, 2250, 950),
            "sofa SVG footprint coordinates were lost");
    require(symbol.position.x == 3.0 && symbol.position.y == 2.0 &&
                symbol.rotation_radians == 0.0,
            "retained SVG placement is wrong");
    return symbol.document;
}

void requireInteriorDetail(const QImage& image, QRect interior) {
    std::set<QRgb> colors;
    int marked = 0;
    interior = interior.intersected(image.rect());
    require(interior.width() > 10 && interior.height() > 10, "empty symbol detail sample");
    for (int y = interior.top(); y <= interior.bottom(); ++y) {
        for (int x = interior.left(); x <= interior.right(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (color.alpha() > 200 && std::min({color.red(), color.green(), color.blue()}) < 240) {
                ++marked;
                colors.insert(color.rgb());
            }
        }
    }
    // An unfilled rectangle has no interior marks; a flat filled rectangle has
    // one color. The supplied sofa has cushion linework and shaded upholstery.
    require(marked > interior.width() * interior.height() / 8 && colors.size() > 16,
            "symbol rendering lost the SVG interior upholstery/cushion detail");
}

QImage renderSymbol(sketch::desktop::CanvasEntity entity) {
    entity.selected = false;
    sketch::desktop::PlanCanvas canvas;
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setEntities({std::move(entity)});
    QImage image(900, 500, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    canvas.renderSceneAt(painter, QRectF(image.rect()), 200.0, {3.0, 2.0}, Qt::white);
    painter.end();
    // At 200 px/m the known 2.25 x .95 m footprint is 450 x 190 px.
    // The crop excludes every footprint edge by 20 px, plus artwork padding.
    requireInteriorDetail(image, QRect(245, 175, 410, 150));
    return image;
}

void requireAllBundledSvgsRenderable() {
    std::size_t count = 0;
    for (const auto& definition : sketch::default_symbol_catalog()) {
        if (!definition.svg_asset.has_value()) continue;
        ++count;
        auto relative = QString::fromStdString(definition.svg_asset->relative_path);
        const auto prefix = QStringLiteral("symbols/architectural_v2/");
        require(relative.startsWith(prefix), "SVG asset path escaped the bundled catalog");
        QFile file(QStringLiteral(":/symbols/architectural_v2/") + relative.mid(prefix.size()));
        require(file.open(QIODevice::ReadOnly), "bundled SVG resource cannot be opened");
        const auto bytes = file.readAll();
        QSvgRenderer renderer(bytes);
        require(renderer.isValid(), "bundled SVG resource is not renderable");
        QImage sample(48, 48, QImage::Format_ARGB32_Premultiplied);
        sample.fill(Qt::transparent);
        QPainter painter(&sample);
        renderer.render(&painter, QRectF(sample.rect()));
        painter.end();
        bool visible = false;
        for (int y = 0; y < sample.height() && !visible; ++y) {
            for (int x = 0; x < sample.width(); ++x) {
                if (sample.pixelColor(x, y).alpha() != 0) {
                    visible = true;
                    break;
                }
            }
        }
        require(visible, "bundled SVG rendered no visible pixels");
    }
    require(count == 320, "desktop bundle must contain every supplied SVG symbol");
}

void requireSvgDropAccepted(const QString& symbol_id) {
    sketch::desktop::PlanCanvas canvas;
    canvas.resize(400, 300);
    canvas.setSnapEnabled(false);
    QString dropped_id;
    double dropped_scale = 0.0;
    sketch::Vec2 dropped_position{};
    canvas.setSymbolDropped([&](QString id, double scale, sketch::Vec2 position) {
        dropped_id = std::move(id);
        dropped_scale = scale;
        dropped_position = position;
    });
    QMimeData mime;
    mime.setData("application/x-vertex-symbol",
                 QJsonDocument(QJsonObject{{QStringLiteral("id"), symbol_id},
                                           {QStringLiteral("scale"), 1.25}})
                     .toJson(QJsonDocument::Compact));
    QDragEnterEvent enter(QPoint(200, 150), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &enter);
    require(enter.isAccepted(), "canvas rejected the SVG library drag payload");
    QDropEvent drop(QPointF(200.0, 150.0), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &drop);
    require(drop.isAccepted() && dropped_id == symbol_id &&
                std::abs(dropped_scale - 1.25) < 1e-12 &&
                std::isfinite(dropped_position.x) && std::isfinite(dropped_position.y),
            "canvas did not commit the intended SVG library drop");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-svg-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        const auto symbol_id = QStringLiteral("svg-v2-04_living-sofa-three-seat");
        sketch::desktop::MainWindow window;
        requireAllBundledSvgsRenderable();
        auto* categories = window.findChild<QComboBox*>(QStringLiteral("annotationSymbolCategory"));
        auto* search = window.findChild<QLineEdit*>(QStringLiteral("annotationSymbolSearch"));
        auto* library = window.findChild<QListWidget*>(QStringLiteral("symbolLibraryItems"));
        auto* sizes = window.findChild<QComboBox*>(QStringLiteral("annotationSymbolSize"));
        auto* status = window.findChild<QLabel*>(QStringLiteral("annotationEditorStatus"));
        require(categories && search && library && sizes && status,
                "visible component library controls are missing");
        categories->setCurrentIndex(0);
        search->clear();
        QApplication::processEvents();
        require(library->count() == 320,
                "visible library must contain the complete supplied SVG set only");
        for (int index = 0; index < library->count(); ++index) {
            require(library->item(index)->data(Qt::UserRole).toString().startsWith(
                        QStringLiteral("svg-v2-")),
                    "legacy procedural compatibility symbol leaked into the visible library");
        }
        require(status->text().contains(QStringLiteral("320 components")) &&
                    !status->text().contains(QStringLiteral("detailed SVG")),
                "library status must report the supplied component count without redundant tiers");
        require(library->dragEnabled() &&
                    library->dragDropMode() == QAbstractItemView::DragOnly &&
                    library->movement() == QListView::Free,
                "visible SVG library is not configured to initiate external drags");
        requireSvgDropAccepted(symbol_id);
        const auto category = categories->findData(QStringLiteral("04_living"));
        require(category >= 0, "SVG living-room category is missing");
        categories->setCurrentIndex(category);
        search->setText(QStringLiteral("Sofa Three Seat"));
        QApplication::processEvents();
        require(library->count() == 1, "human-name search must identify the supplied three-seat sofa");
        const auto* item = library->item(0);
        require(!item->isHidden() && item->text().contains(QStringLiteral("Sofa Three Seat"), Qt::CaseInsensitive),
                "SVG library must display the human name rather than the namespaced ID");
        const auto thumbnail = item->icon().pixmap(QSize(136, 108)).toImage();
        require(!thumbnail.isNull(), "SVG library thumbnail is missing");
        requireInteriorDetail(thumbnail, QRect(thumbnail.width() / 4, thumbnail.height() * 3 / 8,
                                               thumbnail.width() / 2, thumbnail.height() / 4));

        const auto walls_category = categories->findData(QStringLiteral("16_walls_openings"));
        require(walls_category >= 0, "SVG walls and openings category is missing");
        categories->setCurrentIndex(walls_category);
        search->setText(QStringLiteral("Arched Opening"));
        QApplication::processEvents();
        require(library->count() == 1 &&
                    library->item(0)->toolTip().contains(QStringLiteral("Default size")) &&
                    library->item(0)->toolTip().contains(QStringLiteral("adjust to suit")) &&
                    sizes->currentText().contains(QStringLiteral("Default size")) &&
                    sizes->currentText().contains(QStringLiteral("adjust to suit")),
                "dimensionless artwork must identify its editable default size instead of implying a sourced dimension");

        const auto instance_id = window.createAnnotationSymbol(symbol_id, {3.0, 2.0});
        require(!instance_id.isEmpty(), "MainWindow rejected the SVG catalog ID");
        const auto entity = retainedSymbol(window, instance_id);
        const auto bytes = checkedPayload(entity);
        require(bytes.contains("sofa-three-seat--title"), "placed payload belongs to another SVG asset");
        const auto before = renderSymbol(entity);
        const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture_directory.isEmpty()) {
            require(QDir().mkpath(capture_directory), "SVG capture directory cannot be created");
            require(thumbnail.save(QDir(capture_directory).filePath(
                        QStringLiteral("symbol-svg-sofa-thumbnail.png"))),
                    "SVG thumbnail capture could not be written");
            require(before.save(QDir(capture_directory).filePath(
                        QStringLiteral("symbol-svg-sofa-canvas.png"))),
                    "SVG canvas capture could not be written");
        }

        QTemporaryDir directory;
        require(directory.isValid(), "temporary project directory is unavailable");
        const auto path = directory.filePath(QStringLiteral("svg-sofa.bldproj"));
        require(window.saveProjectAs(path), "SVG placement project failed to save");
        require(window.openProject(path), "SVG placement project failed to reopen");
        const auto restored = retainedSymbol(window, instance_id);
        require(checkedPayload(restored) == bytes, "save/reopen lost or changed the SVG payload");
        require(renderSymbol(restored) == before, "save/reopen changed the SVG rendering");
        std::cout << "symbol SVG desktop checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "symbol_svg_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
