#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/boundary_entity.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QMessageBox>
#include <QTimer>
#include <QCheckBox>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPushButton>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTransform>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close_enough(double actual, double expected) {
    return std::abs(actual - expected) < 1e-9;
}

sketch::desktop::CanvasEntity retained(sketch::desktop::MainWindow& window, const QString& id) {
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas != nullptr, "measurement canvas is missing");
    const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [&](const auto& entity) { return entity.id == id; });
    require(found != canvas->entities().end() && found->svg_symbol,
            "transformed component has no retained SVG artwork");
    return *found;
}

sketch::SymbolInstance persisted(sketch::desktop::MainWindow& window, const QString& id) {
    const auto snapshot = window.document().snapshot();
    for (const auto& [unused_id, entity] : snapshot.entities()) {
        (void)unused_id;
        if (entity.type != sketch::kAnnotationEntityType) continue;
        const auto state = sketch::decode_annotation_entity(entity);
        for (const auto& instance : state.symbols)
            if (instance.id == id.toStdString()) return instance;
    }
    throw std::runtime_error("component is absent from persisted annotation state");
}

void requireSize(sketch::desktop::MainWindow& window, const QString& id,
                 double width, double depth, bool horizontal = false, bool vertical = false) {
    const auto symbol = *retained(window, id).svg_symbol;
    require(close_enough(symbol.width_metres, width) && close_enough(symbol.depth_metres, depth) &&
                symbol.flip_horizontal == horizontal && symbol.flip_vertical == vertical,
            "component dimensions or local mirror axes differ from the requested transform");
}

QImage render(sketch::desktop::CanvasEntity entity) {
    entity.selected = false;
    const auto position = entity.svg_symbol->position;
    sketch::desktop::PlanCanvas canvas;
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setEntities({std::move(entity)});
    QImage image(1000, 700, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(40, 48, 60));
    QPainter painter(&image);
    canvas.renderSceneAt(painter, QRectF(image.rect()), 150.0, position, QColor(40, 48, 60));
    painter.end();
    return image;
}

double pixelDifference(const QImage& left, const QImage& right) {
    require(left.size() == right.size(), "raster comparison sizes differ");
    double difference = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const auto a = left.pixelColor(x, y), b = right.pixelColor(x, y);
            difference += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
                          std::abs(a.blue() - b.blue());
        }
    }
    return difference / (3.0 * left.width() * left.height());
}

void requireMirror(const QImage& original, const QImage& mirrored, bool horizontal, bool vertical) {
    // QImage mirrors independently of the production symbol/canvas transform.
    // The asymmetric chaise must move to the opposite side; flags alone fail.
    require(pixelDifference(original, mirrored) > 3.0,
            "mirror did not visibly move the asymmetric sectional artwork");
    require(pixelDifference(original.mirrored(horizontal, vertical), mirrored) < 0.5,
            "rendered component did not mirror about its local footprint centre");
}

QLineEdit* field(sketch::desktop::MainWindow& window, const char* name) {
    auto* result = window.findChild<QLineEdit*>(QString::fromLatin1(name));
    require(result != nullptr, "component property field is missing");
    return result;
}

QCheckBox* flag(sketch::desktop::MainWindow& window, const char* name) {
    auto* result = window.findChild<QCheckBox*>(QString::fromLatin1(name));
    require(result != nullptr, "component mirror property is missing");
    return result;
}

void apply(sketch::desktop::MainWindow& window) {
    auto* button = window.findChild<QPushButton*>(QStringLiteral("applyAnnotation"));
    require(button && button->isEnabled(), "component properties cannot be applied");
    button->click();
    QApplication::processEvents();
    require(window.lastError().isEmpty(), "component property edit failed");
}

bool edit(sketch::desktop::MainWindow& window, const QString& id,
          std::optional<QString> width = std::nullopt, std::optional<QString> depth = std::nullopt,
          std::optional<bool> horizontal = std::nullopt, std::optional<bool> vertical = std::nullopt,
          const QString& scale = QStringLiteral("1"), const QString& rotation = QStringLiteral("0")) {
    return window.editAnnotation(id, {}, QStringLiteral("3"), QStringLiteral("2"), rotation,
        scale, true, {}, {}, {}, {}, false, false, false, width, depth, horizontal, vertical);
}

std::array<QImage, 3> exports(sketch::desktop::MainWindow& window,
                             const QTemporaryDir& directory, const QString& stem) {
    const auto pdf_path = directory.filePath(stem + QStringLiteral(".pdf"));
    const auto svg_path = directory.filePath(stem + QStringLiteral(".svg"));
    const auto png_path = directory.filePath(stem + QStringLiteral(".png"));
    require(window.exportDraftPdf(pdf_path) && window.exportDraftSvg(svg_path) &&
                window.exportDraftImage(png_path), "transformed component failed PDF/SVG/PNG export");
    QPdfDocument pdf;
    require(pdf.load(pdf_path) == QPdfDocument::Error::None && pdf.pageCount() == 1,
            "transformed component PDF cannot be decoded");
    QSvgRenderer svg(svg_path);
    require(svg.isValid(), "transformed component SVG cannot be decoded");
    QImage svg_image(1680, 1188, QImage::Format_ARGB32_Premultiplied);
    svg_image.fill(Qt::white);
    QPainter painter(&svg_image);
    svg.render(&painter, QRectF(svg_image.rect()));
    painter.end();
    std::array<QImage, 3> result{pdf.render(0, QSize(1680, 1188)), svg_image, QImage(png_path)};
    for (const auto& image : result) require(!image.isNull(), "export has no rasterizable page");
    return result;
}

int whiteMaskDifference(const QImage& left, const QImage& right) {
    require(left.size() == right.size(), "preview raster sizes differ");
    const auto white = [](QColor color) {
        return color.red() > 220 && color.green() > 220 && color.blue() > 220;
    };
    int difference = 0;
    for (int y = 0; y < left.height(); ++y)
        for (int x = 0; x < left.width(); ++x)
            difference += white(left.pixelColor(x, y)) != white(right.pixelColor(x, y));
    return difference;
}

void requireArtworkGesturePreview(sketch::desktop::CanvasEntity entity) {
    sketch::desktop::PlanCanvas canvas;
    canvas.resize(700, 700);
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.setCanvasBackground(QColor(40, 48, 60));
    canvas.setEntities({entity});
    canvas.setSelectedId(entity.id);
    canvas.setSelectionTransformEnabled(true, true);
    canvas.fitView();
    int commits = 0;
    canvas.setEntityTransformRequested([&](QString, double, double) { ++commits; return true; });
    const auto interactive = [&] {
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        canvas.renderScene(painter, QRectF(canvas.rect()));
        painter.end();
        return image;
    };
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const auto baseline = interactive();
    const auto frame = canvas.selectionBounds();
    require(frame.has_value(), "component has no gesture frame");
    const auto corner = frame->bottomRight();
    const auto resize_target = frame->center() + (corner - frame->center()) * 1.15;
    mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, resize_target, Qt::NoButton, Qt::LeftButton);
    require(whiteMaskDifference(baseline, interactive()) > 2000,
            "resize preview moved only the selection frame, leaving SVG artwork static");
    require(render(canvas.entities().front()) == render(entity),
            "resize preview changed committed component geometry or output");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    mouse(QEvent::MouseButtonRelease, resize_target, Qt::LeftButton, Qt::NoButton);
    require(commits == 0 && interactive() == baseline,
            "Escape did not restore the exact component artwork without a transform command");

    const auto rotate = QPointF(frame->center().x(), frame->top() - 24.0);
    const auto rotate_target = frame->center() + QPointF(100, 0);
    mouse(QEvent::MouseButtonPress, rotate, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, rotate_target, Qt::NoButton, Qt::LeftButton);
    const auto rotating = interactive();
    require(whiteMaskDifference(baseline, rotating) > 2000,
            "rotation preview moved only the selection frame, leaving SVG artwork static");
    const auto clockwise = baseline.transformed(QTransform().rotate(90));
    require(whiteMaskDifference(clockwise, rotating) < 2000,
            "rotation preview used the wrong screen angle or centre for SVG artwork");
    QApplication::sendEvent(&canvas, &escape);
    mouse(QEvent::MouseButtonRelease, rotate_target, Qt::LeftButton, Qt::NoButton);
    require(commits == 0 && interactive() == baseline,
            "cancelled rotation retained a visual or document transform");
}

void rotateGesture(sketch::desktop::PlanCanvas& canvas, double from, double to,
                   Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    canvas.setSnapEnabled(false);
    canvas.setOverviewMapEnabled(false);
    canvas.fitView();
    // Keep the complete oriented frame and its outward pin inside the viewport;
    // selectionBounds intentionally clips feedback at the canvas edge.
    canvas.zoomBy(.7);
    const auto bounds = canvas.selectionBounds();
    if (!bounds) throw std::runtime_error((QStringLiteral("No rotation frame on %1 (%2x%3, selected %4, from %5 to %6)")
        .arg(canvas.objectName()).arg(canvas.width()).arg(canvas.height())
        .arg(std::count_if(canvas.entities().begin(), canvas.entities().end(),
            [](const auto& entity) { return entity.selected; })).arg(from).arg(to)).toStdString());
    // Undo the screen AABB rotation to recover the padded local frame depth.
    const auto c = std::abs(std::cos(from)), s = std::abs(std::sin(from));
    const auto depth = (bounds->height()*c - bounds->width()*s)/(c*c-s*s);
    const auto radius = depth*.5 + 24.0;
    const auto pin = [&](double angle) {
        return bounds->center() + QPointF(-std::sin(angle)*radius, -std::cos(angle)*radius);
    };
    const auto mouse = [&](QEvent::Type type, QPointF point) {
        QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(&canvas, &event);
    };
    mouse(QEvent::MouseButtonPress, pin(from));
    mouse(QEvent::MouseMove, pin(to));
    mouse(QEvent::MouseButtonRelease, pin(to));
    QApplication::processEvents();
}

bool reopenDiscardingChanges(sketch::desktop::MainWindow& window, const QString& path) {
    QTimer choice;
    choice.setInterval(10);
    QObject::connect(&choice, &QTimer::timeout, [&] {
        auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (message && message->windowTitle() == QStringLiteral("Open project")) {
            if (auto* discard = message->button(QMessageBox::Discard)) {
                choice.stop();
                discard->click();
            }
        }
    });
    choice.start();
    return window.openProject(path);
}

void requireCommittedGestures(const QTemporaryDir& directory) {
    using namespace sketch;
    using namespace sketch::desktop;
    constexpr double half_pi = 1.57079632679489661923;
    MainWindow window;
    window.setMetricUnits(true);
    window.resize(1500, 1000);
    window.show();
    QApplication::processEvents();
    const auto id = window.createAnnotationSymbol("svg-v2-04_living-sectional-left", {3, 2});
    require(!id.isEmpty() && window.selectEntity(id) &&
                edit(window, id, {}, {}, {}, {}, "1", "30"), "gesture symbol setup failed");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas != nullptr, "gesture measurement canvas is missing");
    const auto before = window.document().revision();
    rotateGesture(*canvas, half_pi/3, half_pi);
    const auto check_symbol = [&](double angle) {
        const auto symbol = retained(window, id).svg_symbol.value();
        require(close_enough(symbol.position.x, 3) && close_enough(symbol.position.y, 2) &&
                    close_enough(symbol.rotation_radians, angle),
                "real canvas rotation changed symbol placement or absolute angle");
        require(close_enough(persisted(window, id).placement.rotation_radians, angle),
                "real canvas rotation was not persisted");
    };
    require(window.document().revision() == before + 1, "symbol gesture must commit one command");
    check_symbol(half_pi);
    require(window.undoCommand(), "symbol gesture cannot undo");
    check_symbol(half_pi/3);
    require(window.redoCommand(), "symbol gesture cannot redo");
    check_symbol(half_pi);
    require(window.selectEntity(id), "symbol cannot be reselected after history navigation");
    require(canvas->selectionBounds().has_value(), "reselected symbol has no frame");
    // A second drag starts at the newly projected pin, proving refresh orientation.
    rotateGesture(*canvas, half_pi, 2*half_pi);
    check_symbol(2*half_pi);
    rotateGesture(*canvas, 2*half_pi, 0);
    check_symbol(0);
    rotateGesture(*canvas, 0, 23.5*half_pi/90, Qt::ShiftModifier);
    check_symbol(23.5*half_pi/90);
    require(window.undoCommand(), "fine symbol gesture cannot undo");
    check_symbol(0);
    require(window.undoCommand(), "reset symbol gesture cannot undo");
    check_symbol(2*half_pi);
    require(window.undoCommand(), "second symbol gesture cannot undo");
    const auto path = directory.filePath("gesture-commits.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "gesture project cannot reopen");
    check_symbol(half_pi);

    window.setWorkspace(Workspace::architectural);
    const auto column_id = window.commitBuildingObject(encode_building_entity(
        RectangularColumn{"", {3, 2, 0}, .4, .6, 3, 0}), window.document().revision());
    require(!column_id.isEmpty() && window.selectEntity(column_id), "gesture column setup failed");
    auto* plan = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(plan != nullptr, "gesture architectural canvas is missing");
    const auto column_before = window.document().revision();
    rotateGesture(*plan, 0, half_pi);
    const auto check_column = [&](double angle) {
        const auto model = decode_building_entity(window.document().snapshot().entities().at(column_id.toStdString()));
        const auto& column = std::get<RectangularColumn>(model);
        require(close_enough(column.base_center.x, 3) && close_enough(column.base_center.y, 2) &&
                    close_enough(column.rotation_radians, angle),
                "column gesture rotated placement about the world origin instead of its centre");
        const auto found = std::find_if(plan->entities().begin(), plan->entities().end(),
            [&](const auto& entity) { return entity.id == column_id; });
        require(found != plan->entities().end() && found->resize_frame &&
                    close_enough(found->resize_frame->center.x, 3) && close_enough(found->resize_frame->center.y, 2) &&
                    close_enough(found->resize_frame->rotation_radians, angle),
                "column frame lost semantic placement or orientation after refresh");
    };
    require(window.document().revision() == column_before + 1, "column gesture must commit one command");
    check_column(half_pi);
    require(window.undoCommand(), "column gesture cannot undo");
    check_column(0);
    require(window.redoCommand(), "column gesture cannot redo");
    check_column(half_pi);
    require(window.saveProjectAs(path) && window.openProject(path), "column gesture cannot reopen");
    check_column(half_pi);
    const auto circle_id = window.commitBuildingObject(encode_building_entity(
        CircularColumn{"", {3, 2, 0}, .4, 3}), window.document().revision());
    require(!circle_id.isEmpty() && window.selectEntity(circle_id), "circular column setup failed");
    const auto check_circle = [&](double angle) {
        const auto model=decode_building_entity(window.document().snapshot().entities().at(circle_id.toStdString()));
        const auto& column=std::get<CircularColumn>(model);
        const auto found=std::find_if(plan->entities().begin(),plan->entities().end(),
            [&](const auto& entity){return entity.id==circle_id;});
        require(close_enough(column.rotation_radians,angle) &&
                    close_enough(column.base_center.x,3) && close_enough(column.base_center.y,2) &&
                    found!=plan->entities().end() && found->resize_frame &&
                    close_enough(found->resize_frame->rotation_radians,angle),
                "circular column pin lost its committed orientation");
    };
    rotateGesture(*plan,0,half_pi);
    check_circle(half_pi);
    rotateGesture(*plan,half_pi,2*half_pi);
    check_circle(2*half_pi);
    rotateGesture(*plan,2*half_pi,0);
    check_circle(0);
    require(window.undoCommand(), "circular rotation reset cannot undo");
    check_circle(2*half_pi);
    require(window.saveProjectAs(path) && window.openProject(path), "circular rotation cannot reopen");
    require(window.selectEntity(circle_id), "circular column cannot be reselected");
    check_circle(2*half_pi);
    window.setWorkspace(Workspace::measurement);
    const Boundary triangle{{{1,2},{4,3},0},{{4,3},{2,6},0},{{2,6},{1,2},0}};
    const auto boundary_id = window.createBoundary(triangle);
    require(!boundary_id.isEmpty() && window.selectEntity(boundary_id), "asymmetric boundary setup failed");
    const auto selected = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [&](const auto& entity) { return entity.id == boundary_id; });
    require(selected != canvas->entities().end() && selected->resize_frame,
            "asymmetric boundary has no oriented frame");
    const auto pivot = selected->resize_frame->center;
    const auto initial_angle = selected->resize_frame->rotation_radians;
    rotateGesture(*canvas, initial_angle, half_pi);
    const auto transformed = boundary_geometry(decode_identified_boundary_entity(
        window.document().snapshot().entities().at(boundary_id.toStdString())));
    const auto delta = half_pi-initial_angle;
    for (std::size_t index=0; index<triangle.size(); ++index) {
        const auto p = triangle[index].start;
        const auto x = p.x-pivot.x, y=p.y-pivot.y;
        require(close_enough(transformed[index].start.x,pivot.x+std::cos(delta)*x-std::sin(delta)*y) &&
                close_enough(transformed[index].start.y,pivot.y+std::sin(delta)*x+std::cos(delta)*y),
                "asymmetric boundary commit used a different pivot from its canvas preview");
    }
    const auto refreshed=std::find_if(canvas->entities().begin(),canvas->entities().end(),
        [&](const auto& entity){return entity.id==boundary_id;});
    require(refreshed!=canvas->entities().end() && refreshed->resize_frame &&
                refreshed->selected && close_enough(refreshed->resize_frame->rotation_radians,half_pi),
            "boundary pin reset after rotation release");
    rotateGesture(*canvas,half_pi,2*half_pi);
    if(!window.lastError().isEmpty()) throw std::runtime_error(window.lastError().toStdString());
    const auto boundary_frame=[&] {
        const auto found=std::find_if(canvas->entities().begin(),canvas->entities().end(),
            [&](const auto& entity){return entity.id==boundary_id;});
        return found->resize_frame.value();
    };
    if(!close_enough(std::abs(boundary_frame().rotation_radians),2*half_pi))
        throw std::runtime_error((QStringLiteral("second boundary drag angle %1 center %2,%3 (initial center %4,%5)")
            .arg(boundary_frame().rotation_radians,0,'g',15).arg(boundary_frame().center.x)
            .arg(boundary_frame().center.y).arg(pivot.x).arg(pivot.y)).toStdString());
    require(close_enough(boundary_frame().center.x,pivot.x) &&
                close_enough(boundary_frame().center.y,pivot.y),"second boundary drag shifted its pivot");
    rotateGesture(*canvas,2*half_pi,initial_angle,Qt::ShiftModifier);
    require(close_enough(boundary_frame().rotation_radians,initial_angle),
            "fine boundary drag did not restore its original angle");
    const auto restored=boundary_geometry(decode_identified_boundary_entity(
        window.document().snapshot().entities().at(boundary_id.toStdString())));
    for(std::size_t index=0;index<triangle.size();++index)
        if(!close_enough(restored[index].start.x,triangle[index].start.x) ||
                    !close_enough(restored[index].start.y,triangle[index].start.y))
            throw std::runtime_error((QStringLiteral("boundary could not return: vertex %1 actual %2,%3 expected %4,%5")
                .arg(index).arg(restored[index].start.x,0,'g',15).arg(restored[index].start.y,0,'g',15)
                .arg(triangle[index].start.x).arg(triangle[index].start.y)).toStdString());
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-symbol-transform-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        sketch::desktop::MainWindow window;
        window.setMetricUnits(true);
        window.resize(1500, 1000);
        window.show();
        QApplication::processEvents();
        const auto id = window.createAnnotationSymbol(
            QStringLiteral("svg-v2-04_living-sectional-left"), {3, 2});
        require(!id.isEmpty() && window.selectEntity(id), "asymmetric component cannot be placed and selected");
        const auto original = retained(window, id);
        const auto bytes = original.svg_symbol->document;
        require(bytes.contains("sectional-left--artwork"), "fixture did not load the real bundled sectional");
        requireSize(window, id, 2.8, 1.8);
        require(!field(window, "symbolWidth")->isHidden() && !field(window, "symbolDepth")->isHidden() &&
                    field(window, "annotationScale")->isHidden(),
                "symbol editor must expose physical width/depth instead of the legacy scale row");

        field(window, "symbolWidth")->setText(QStringLiteral("4.2 m"));
        apply(window);
        requireSize(window, id, 4.2, 1.8);
        field(window, "symbolDepth")->setText(QStringLiteral("2.4 m"));
        apply(window);
        requireSize(window, id, 4.2, 2.4);
        const auto sized = render(retained(window, id));
        flag(window, "symbolFlipHorizontal")->setChecked(true);
        apply(window);
        requireSize(window, id, 4.2, 2.4, true, false);
        requireMirror(sized, render(retained(window, id)), true, false);
        flag(window, "symbolFlipHorizontal")->setChecked(false);
        flag(window, "symbolFlipVertical")->setChecked(true);
        apply(window);
        requireSize(window, id, 4.2, 2.4, false, true);
        requireMirror(sized, render(retained(window, id)), false, true);

        const auto prior = persisted(window, id);
        const auto prior_render = render(retained(window, id));
        const auto revision = window.document().revision();
        field(window, "symbolWidth")->setText(QStringLiteral("3.5 m"));
        field(window, "symbolDepth")->setText(QStringLiteral("2.1 m"));
        field(window, "annotationRotation")->setText(QStringLiteral("30"));
        flag(window, "symbolFlipHorizontal")->setChecked(true);
        apply(window);
        require(window.document().revision() == revision + 1,
                "combined component property edit must commit one document command");
        requireSize(window, id, 3.5, 2.1, true, true);
        require(close_enough(retained(window, id).svg_symbol->rotation_radians, std::acos(-1.0) / 6),
                "rotation was lost when physical dimensions and mirrors were applied");
        require(window.undoCommand(), "combined component edit cannot undo");
        requireSize(window, id, 4.2, 2.4, false, true);
        require(persisted(window, id).pinned_svg == prior.pinned_svg &&
                    render(retained(window, id)) == prior_render,
                "one undo did not restore all component properties and exact pinned artwork");
        require(window.redoCommand(), "combined component edit cannot redo");

        const auto valid_revision = window.document().revision();
        const auto valid_render = render(retained(window, id));
        for (const auto& invalid : {QStringLiteral("0 m"), QStringLiteral("-1 m"),
                                  QStringLiteral("1000000000 m"), QStringLiteral("not a length")}) {
            require(!edit(window, id, invalid) && !edit(window, id, std::nullopt, invalid),
                    "invalid physical width or depth was accepted");
            require(window.document().revision() == valid_revision &&
                        render(retained(window, id)) == valid_render,
                    "rejected physical dimensions mutated the component or command history");
        }

        // Old callers omit the new fields. Uniform transforms must preserve
        // independent proportions and local mirror axes rather than reset them.
        require(edit(window, id, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                     QStringLiteral("1.5"), QStringLiteral("45")), "legacy uniform component transform failed");
        requireSize(window, id, 5.25, 3.15, true, true);
        require(close_enough(retained(window, id).svg_symbol->rotation_radians, std::acos(-1.0) / 4),
                "uniform transform did not preserve the requested rotation");
        require(window.undoCommand(), "legacy component transform cannot undo");

        QTemporaryDir directory;
        require(directory.isValid(), "temporary project directory is unavailable");
        const auto path = directory.filePath(QStringLiteral("symbol-transforms.bldproj"));
        require(window.saveProjectAs(path), "transformed component project failed to save");
        const auto saved_render = render(retained(window, id));
        const auto saved_outputs = exports(window, directory, QStringLiteral("transformed"));
        require(edit(window, id, std::nullopt, std::nullopt, false, false,
                     QStringLiteral("1"), QStringLiteral("30")), "export mirror control failed");
        const auto unmirrored_outputs = exports(window, directory, QStringLiteral("unmirrored"));
        for (std::size_t index = 0; index < saved_outputs.size(); ++index)
            require(saved_outputs[index] != unmirrored_outputs[index],
                    "PDF/SVG/PNG export ignored the asymmetric component mirror transform");
        require(window.undoCommand() && reopenDiscardingChanges(window, path),
                "transformed component project failed to reopen");
        requireSize(window, id, 3.5, 2.1, true, true);
        require(retained(window, id).svg_symbol->document == bytes &&
                    persisted(window, id).pinned_svg == bytes.toStdString() &&
                    render(retained(window, id)) == saved_render,
                "save/reopen changed physical dimensions, mirror orientation, or pinned SVG bytes");
        require(exports(window, directory, QStringLiteral("reopened")) == saved_outputs,
                "save/reopen changed rasterized production PDF/SVG/PNG output");
        requireArtworkGesturePreview(original);
        requireCommittedGestures(directory);
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            require(QDir().mkpath(capture) && saved_render.save(QDir(capture).filePath(
                        QStringLiteral("symbol-independent-size-mirrored.png"))),
                    "component transform capture could not be written");
        }
        std::cout << "symbol transform desktop checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "symbol_transform_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
