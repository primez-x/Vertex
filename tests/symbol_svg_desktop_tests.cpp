#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_catalog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDragEnterEvent>
#include <QDrag>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QTimer>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QUuid>
#include <QWindow>
#include <QtTest/qtestmouse.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <array>
#include <stdexcept>

namespace {
void pointer(QWidget* widget, QEvent::Type type, QPoint point,
             Qt::MouseButton button, Qt::MouseButtons buttons) {
    const auto global = widget->mapToGlobal(point);
    auto* window = widget->window()->windowHandle();
    // Use the same QPA injection as QTest's QWindow overloads so Qt's global
    // button state and drag manager see a genuine pressed pointer sequence.
    static int timestamp = 1000;
    qt_handleMouseEvent(window, QPointF(window->mapFromGlobal(global)), QPointF(global),
                        buttons, button, type, Qt::NoModifier, timestamp += 100);
    QApplication::processEvents();
}

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

QByteArray recoloredArtwork(QByteArray document) {
    const auto original = document;
    document.replace("#e6e7e8", "#ff0000");
    require(document != original, "SVG artwork fixture could not be made visibly distinct");
    QSvgRenderer renderer(document);
    require(renderer.isValid(), "modified historical SVG fixture is invalid");
    return document;
}

void requireIndependentArtworkCache(sketch::desktop::CanvasEntity current,
                                    const QByteArray& historical_document) {
    current.selected = false;
    auto historical = current;
    current.svg_symbol->position = {2.0, 2.0};
    historical.svg_symbol->position = {5.0, 2.0};
    historical.svg_symbol->document = historical_document;
    historical.svg_symbol->artwork_sha256 = QCryptographicHash::hash(
        historical_document, QCryptographicHash::Sha256).toHex();
    sketch::desktop::PlanCanvas canvas;
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    const auto render = [&](std::vector<sketch::desktop::CanvasEntity> entities) {
        canvas.setEntities(std::move(entities));
        QImage image(1000, 500, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas.renderSceneAt(painter, QRectF(image.rect()), 180.0, {3.5, 2.0}, Qt::white);
        painter.end();
        return image;
    };
    const auto forward = render({current, historical});
    const auto reverse = render({historical, current});
    require(forward == reverse,
            "SVG renderer cache aliases different pinned artwork with one catalog ID");
    historical.svg_symbol->position = current.svg_symbol->position;
    require(renderSymbol(current) != renderSymbol(historical),
            "historical SVG fixture is not visually distinct from installed artwork");
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

QString dragVisibleLibraryItem(sketch::desktop::MainWindow& window, QListWidget* library) {
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("sidebarTabs"));
    require(tabs != nullptr, "sidebar tabs are missing");
    tabs->setCurrentWidget(window.findChild<QWidget*>(QStringLiteral("symbolsPanel")));
    QApplication::processEvents();
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>(
        QStringLiteral("measurementPlanCanvas")));
    require(canvas && canvas->isVisible() && library->isVisible(),
            "drag qualification requires the visible palette and canvas");
    const auto count = canvas->entities().size();
    library->scrollToItem(library->item(0));
    const auto source = library->visualItemRect(library->item(0)).center();
    const auto target = canvas->rect().center();
    require(library->viewport()->rect().contains(source), "SVG source item is outside the viewport");
    require(library->itemAt(source) == library->item(0), "SVG pointer origin does not hit the item");
    // QDrag owns a nested event loop. These pointer events traverse Qt's
    // platform drag implementation; no fabricated MIME or drop event is used.
    QTimer movement;
    movement.setSingleShot(true);
    QObject::connect(&movement, &QTimer::timeout, canvas, [=] {
        pointer(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
        pointer(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] { QDrag::cancel(); });
    pointer(library->viewport(), QEvent::MouseButtonPress, source, Qt::LeftButton, Qt::LeftButton);
    require(library->item(0)->isSelected(), "pointer press did not select the SVG library item");
    pointer(library->viewport(), QEvent::MouseMove, source + QPoint(1, 0),
            Qt::NoButton, Qt::LeftButton);
    movement.start(100);
    watchdog.start(3000);
    pointer(library->viewport(), QEvent::MouseMove,
            source + QPoint(QApplication::startDragDistance() + 8, 0),
            Qt::NoButton, Qt::LeftButton);
    QApplication::processEvents();
    watchdog.stop();
    require(canvas->entities().size() == count + 1,
            "pointer drag from the visible SVG palette did not place one entity");
    const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [](const auto& entity) { return entity.svg_symbol.has_value(); });
    require(found != canvas->entities().end(), "pointer-dropped entity has no SVG artwork");
    return found->id;
}

std::array<QImage, 3> renderExports(sketch::desktop::MainWindow& window,
                                   const QTemporaryDir& directory, const QString& stem) {
    const auto pdf_path = directory.filePath(stem + QStringLiteral(".pdf"));
    const auto svg_path = directory.filePath(stem + QStringLiteral(".svg"));
    const auto png_path = directory.filePath(stem + QStringLiteral(".png"));
    if (!window.exportDraftPdf(pdf_path) || !window.exportDraftSvg(svg_path) ||
        !window.exportDraftImage(png_path))
        throw std::runtime_error("SVG scene failed production PDF/SVG/PNG export: " +
                                 window.lastError().toStdString());
    QPdfDocument pdf;
    require(pdf.load(pdf_path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
            "SVG scene PDF cannot be decoded");
    QSvgRenderer svg(svg_path);
    require(svg.isValid(), "SVG scene vector export cannot be decoded");
    QImage svg_image(1680, 1188, QImage::Format_ARGB32_Premultiplied);
    svg_image.fill(Qt::white);
    QPainter painter(&svg_image);
    svg.render(&painter, QRectF(svg_image.rect()));
    painter.end();
    std::array<QImage, 3> result{pdf.render(0, QSize(1680, 1188)),
                               svg_image, QImage(png_path)};
    for (const auto& image : result) require(!image.isNull(), "export produced no rasterizable page");
    return result;
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    // Qt's offscreen QOffscreenDrag unconditionally ignores every drag.
    // Minimal remains headless but exercises QSimpleDrag's real event loop.
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"))
        qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-svg-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        const auto symbol_id = QStringLiteral("svg-v2-04_living-sofa-three-seat");
        sketch::desktop::MainWindow window;
        window.resize(1500, 1000);
        window.show();
        QApplication::processEvents();
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
        const auto dragged_id = dragVisibleLibraryItem(window, library);
        require(window.selectEntity(dragged_id), "pointer-dropped SVG cannot be selected");
        const auto set_property = [&](const char* name, const char* value) {
            auto* control = window.findChild<QLineEdit*>(QString::fromLatin1(name));
            require(control != nullptr, "annotation inspector control is missing");
            control->setText(QString::fromLatin1(value));
        };
        set_property("annotationX", "3");
        set_property("annotationY", "2");
        set_property("annotationRotation", "30");
        set_property("annotationScale", "1.5");
        auto* apply = window.findChild<QPushButton*>(QStringLiteral("applyAnnotation"));
        require(apply && apply->isEnabled(), "annotation inspector apply is unavailable");
        apply->click();
        const auto transformed = retainedSymbol(window, dragged_id);
        require(transformed.svg_symbol &&
                    std::abs(transformed.svg_symbol->position.x - 3.0) < 1e-9 &&
                    std::abs(transformed.svg_symbol->position.y - 2.0) < 1e-9 &&
                    std::abs(transformed.svg_symbol->rotation_radians - std::acos(-1.0) / 6.0) < 1e-9 &&
                    std::abs(transformed.svg_symbol->width_metres - 3.375) < 1e-9,
                "inspector move, resize, and rotate did not reach retained SVG artwork");

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
        const auto historical_bytes = recoloredArtwork(bytes);
        requireIndependentArtworkCache(entity, historical_bytes);
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
        const auto output_before = renderExports(window, directory, QStringLiteral("before"));
        require(window.openProject(path), "SVG placement project failed to reopen");
        const auto restored = retainedSymbol(window, instance_id);
        require(checkedPayload(restored) == bytes, "save/reopen lost or changed the SVG payload");
        require(renderSymbol(restored) == before, "save/reopen changed the SVG rendering");
        const auto restored_drag = retainedSymbol(window, dragged_id);
        require(restored_drag.svg_symbol &&
                    restored_drag.svg_symbol->document == transformed.svg_symbol->document &&
                    restored_drag.svg_symbol->position.x == transformed.svg_symbol->position.x &&
                    restored_drag.svg_symbol->position.y == transformed.svg_symbol->position.y &&
                    restored_drag.svg_symbol->rotation_radians == transformed.svg_symbol->rotation_radians &&
                    restored_drag.svg_symbol->width_metres == transformed.svg_symbol->width_metres &&
                    restored_drag.svg_symbol->depth_metres == transformed.svg_symbol->depth_metres,
                "save/reopen changed the pointer-dropped SVG transform or artwork");
        const auto output_after = renderExports(window, directory, QStringLiteral("after"));
        require(output_before == output_after,
                "save/reopen changed rasterized production PDF/SVG/PNG artwork");

        // Simulate a project that pins an older definition. The UI must keep
        // rendering its saved SVG until the user explicitly accepts migration.
        auto migration_source = window.document().snapshot();
        auto annotation = std::find_if(
            migration_source.entities().begin(), migration_source.entities().end(),
            [](const auto& entry) { return entry.second.type == sketch::kAnnotationEntityType; });
        require(annotation != migration_source.entities().end(),
                "migration fixture has no annotation entity");
        auto stale_state = sketch::decode_annotation_entity(annotation->second);
        auto stale_instance = std::find_if(
            stale_state.symbols.begin(), stale_state.symbols.end(),
            [&](const auto& candidate) { return candidate.id == instance_id.toStdString(); });
        require(stale_instance != stale_state.symbols.end() && stale_instance->definition,
                "migration fixture has no pinned definition");
        stale_instance->definition->artwork_revision += 1;
        stale_instance->definition->svg_asset->sha256 =
            QCryptographicHash::hash(historical_bytes, QCryptographicHash::Sha256)
                .toHex().toStdString();
        stale_instance->pinned_svg = historical_bytes.toStdString();
        auto stale_entity = annotation->second;
        stale_entity.properties = sketch::make_annotation_entity(
            stale_entity.id, stale_state).properties;
        (void)window.document().apply(sketch::ApplyEntityChanges{
            migration_source.revision(), {sketch::EntityChange::upsert(stale_entity)}, {},
            "inject historical symbol fixture"});
        require(window.selectEntity(instance_id), "historical component cannot be selected");
        auto* migration_status = window.findChild<QLabel*>(
            QStringLiteral("symbolMigrationStatus"));
        auto* migrate = window.findChild<QPushButton*>(
            QStringLiteral("migrateSymbolArtwork"));
        // Selection prepares quick properties; the floating editor itself is
        // opened by double-click/right-click in the production interaction.
        // isHidden() verifies that these controls will be exposed when that
        // editor opens without making this persistence test synthesize a
        // second, unrelated canvas gesture.
        require(migration_status && !migration_status->isHidden() &&
                    migration_status->text().contains(QStringLiteral("saved component artwork")) &&
                    migrate && !migrate->isHidden() && migrate->isEnabled(),
                "historical component does not expose an explicit artwork migration action");
        const auto stale_render = renderSymbol(retainedSymbol(window, instance_id));
        const auto migration_revision = window.document().revision();
        migrate->click();
        auto migrated_state = sketch::decode_annotation_entity(
            window.document().snapshot().entities().at(stale_entity.id));
        auto migrated_instance = std::find_if(
            migrated_state.symbols.begin(), migrated_state.symbols.end(),
            [&](const auto& candidate) { return candidate.id == instance_id.toStdString(); });
        require(window.document().revision() == migration_revision + 1 &&
                    migrated_instance != migrated_state.symbols.end() &&
                    !sketch::symbol_requires_migration(
                        *migrated_instance, sketch::default_symbol_catalog()) &&
                    !migrated_instance->pinned_svg.empty(),
                "explicit component migration did not commit the installed artwork once");
        const auto migrated_render = renderSymbol(retainedSymbol(window, instance_id));
        require(migrated_render == before && migrated_render != stale_render,
                "explicit migration did not replace the saved historical artwork");
        require(window.undoCommand(), "component migration must undo");
        auto undone_state = sketch::decode_annotation_entity(
            window.document().snapshot().entities().at(stale_entity.id));
        const auto undone_instance = std::find_if(
            undone_state.symbols.begin(), undone_state.symbols.end(),
            [&](const auto& candidate) { return candidate.id == instance_id.toStdString(); });
        require(undone_instance != undone_state.symbols.end() &&
                    sketch::symbol_requires_migration(
                        *undone_instance, sketch::default_symbol_catalog()),
                "undo did not restore historical component artwork state");
        require(renderSymbol(retainedSymbol(window, instance_id)) == stale_render,
                "undo did not restore the exact historical SVG artwork");
        require(window.redoCommand() && window.saveProject() && window.openProject(path),
                "migrated component must redo, save and reopen");
        const auto reopened_migration_state = sketch::decode_annotation_entity(
            window.document().snapshot().entities().at(stale_entity.id));
        const auto reopened_migrated = std::find_if(
            reopened_migration_state.symbols.begin(), reopened_migration_state.symbols.end(),
            [&](const auto& candidate) { return candidate.id == instance_id.toStdString(); });
        require(reopened_migrated != reopened_migration_state.symbols.end() &&
                    !sketch::symbol_requires_migration(
                        *reopened_migrated, sketch::default_symbol_catalog()),
                "save/reopen lost the explicit component artwork migration");
        require(renderSymbol(retainedSymbol(window, instance_id)) == before,
                "save/reopen changed migrated SVG artwork");
        require(window.deleteAnnotation(dragged_id), "cannot remove drag fixture for output control");
        const auto without_drag = renderExports(window, directory, QStringLiteral("without-drag"));
        for (std::size_t index = 0; index < output_after.size(); ++index)
            require(output_after[index] != without_drag[index],
                    "production export omitted the pointer-dropped, transformed SVG instance");
        std::cout << "symbol SVG desktop checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "symbol_svg_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
