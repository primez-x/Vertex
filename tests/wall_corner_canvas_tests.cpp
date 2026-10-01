#include "sketch/desktop/main_window.hpp"
#include "plan_canvas.hpp"
#include "sketch/document_wall_plan.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
const CanvasEntity& find_wall(const PlanCanvas& canvas, const QString& id) {
    const auto found = std::find_if(canvas.entities().begin(), canvas.entities().end(),
        [&](const auto& entity) { return entity.id == id; });
    require(found != canvas.entities().end(), "physical wall is retained in canvas");
    return *found;
}
QImage output_image(const std::vector<CanvasEntity>& entities) {
    PlanCanvas output;
    output.setEntities(entities);
    QImage image(800, 800, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    output.renderSceneAt(painter, QRectF(0, 0, 800, 800), 150.0, {1.5, 1.0}, Qt::white);
    painter.end();
    return image;
}
int dark_pixels(const QImage& image, QPoint center) {
    int result = 0;
    for (int y = center.y() - 2; y <= center.y() + 2; ++y)
        for (int x = center.x() - 2; x <= center.x() + 2; ++x)
            if (image.pixelColor(x, y).lightness() < 200) ++result;
    return result;
}
void mouse(PlanCanvas& target, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, target.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &event);
    QApplication::processEvents();
}
void test_opening_preview_updates_neighbor_when_corner_becomes_unsafe() {
    MainWindow window;
    window.resize(1500, 1000);
    window.setMetricUnits(true);
    window.setWorkspace(Workspace::measurement);
    window.show();
    QApplication::processEvents();
    const auto host = window.createStraightWall({0, 0}, {3, 0});
    const auto neighbor = window.createStraightWall({3, 0}, {3, 2});
    require(window.selectEntity(host), "preview host selects");
    const auto opening = window.createHostedOpening("window", "1 m", "1.8 m", "0.8 m", "1.2 m");
    require(!opening.isEmpty(), "corner preview window creates");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas && window.selectEntity(opening), "corner preview window selects");
    canvas->setSnapEnabled(false);
    canvas->setOverviewMapEnabled(false);
    canvas->fitView();
    const auto controls = find_wall(*canvas, opening).opening_width_controls.value();
    const auto screen = [&](Vec2 point) {
        const auto center = canvas->viewCenter();
        return QRectF(canvas->rect()).center() + QPointF((point.x-center.x)*canvas->viewScale(),
            -(point.y-center.y)*canvas->viewScale());
    };
    const auto source = window.document().snapshot();
    require(find_wall(*canvas, neighbor).stroke_segments->size() == 3, "neighbor starts joined");
    mouse(*canvas, QEvent::MouseButtonPress, screen(controls.end_jamb));
    mouse(*canvas, QEvent::MouseMove, screen({2.95, 0}));
    QElapsedTimer timer;
    timer.start();
    while (canvas->openingWidthPreviewPending() && timer.elapsed() < 5000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto& previews = canvas->openingWidthPreviewEntities();
    const auto changed = std::find_if(previews.begin(), previews.end(),
        [&](const auto& entity) { return entity.id == neighbor; });
    require(changed != previews.end() && changed->stroke_segments && changed->stroke_segments->size() == 4,
        "window resize preview restores both corner caps when the short host tail prevents a safe miter");
    require(window.document().snapshot().entities() == source.entities(), "opening preview preserves source geometry");
    QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &cancel);
    require(canvas->openingWidthPreviewEntities().empty() &&
        window.document().snapshot().entities() == source.entities(), "cancel clears neighbor preview without committing");
}
void test_partition_junction_output_and_opening_void() {
    MainWindow window;
    window.resize(1200, 800);
    window.setMetricUnits(true);
    window.setWorkspace(Workspace::measurement);
    window.show();
    const auto host = window.createStraightWall({0, 0}, {4, 0});
    const auto partition = window.createStraightWall({2, 0}, {2, 3});
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas && !host.isEmpty() && !partition.isEmpty(), "physical partition creates");
    const auto& host_plan = find_wall(*canvas, host);
    const auto& branch_plan = find_wall(*canvas, partition);
    require(host_plan.segments.size() == 4 && branch_plan.segments.size() == 4,
        "T junction retains both closed wall polygons for physical picking");
    require(host_plan.stroke_segments->size() == 5 && branch_plan.stroke_segments->size() == 3,
        "T junction removes the branch cap and splits the host face at actual wall thickness");
    auto geometry = std::vector<CanvasEntity>{host_plan, branch_plan};
    for (auto& entity : geometry) entity.selected = false;
    const auto output = output_image(geometry);
    require(dark_pixels(output, {475, 540}) == 0 && dark_pixels(output, {475, 561}) > 0,
        "printable T junction removes only its interior seam and retains the exterior host face");
    const auto source = window.document().snapshot();
    require(window.selectEntity(host) && window.selectEntity(partition, true), "both T walls select");
    const auto join = window.joinSelectedWalls();
    require(!join.isEmpty() && window.document().snapshot().entities().at(join.toStdString()).type == "wall_join",
        "the desktop join command admits the real T solid instead of requiring shared endpoints");
    require(window.document().snapshot().entities().at(host.toStdString()) == source.entities().at(host.toStdString()) &&
            window.document().snapshot().entities().at(partition.toStdString()) == source.entities().at(partition.toStdString()),
        "the joined solid preserves both authoritative wall records");
    require(window.selectEntity(host), "partition host selects");
    const auto opening = window.createHostedOpening("window", "1.5 m", "1 m", "0.8 m", "1.2 m");
    require(!opening.isEmpty(), "opening across the partition station creates");
    const auto& exposed = find_wall(*canvas, partition);
    require(exposed.stroke_segments->size() == 4,
        "a partition ending inside a window void retains its exposed end cap");
}
void test_corner_retains_physical_picking_and_omits_output_seam() {
    MainWindow window;
    window.resize(1200, 800);
    window.setMetricUnits(true);
    window.setWorkspace(Workspace::measurement);
    window.show();
    const auto first = window.createStraightWall({0, 0}, {3, 0});
    const auto second = window.createStraightWall({3, 0}, {3, 2});
    require(!first.isEmpty() && !second.isEmpty(), "connected corner draws");
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "measurement canvas is accessible");
    const auto source = window.document().snapshot();
    const auto& wall = find_wall(*canvas, first);
    require(wall.segments.size() == 4 && wall.stroke_segments && wall.stroke_segments->size() == 3,
        "wall keeps its closed physical polygon while omitting the internal corner cap");
    require(std::abs(wall.segments[0].end.x - 2.93) < 1e-8 &&
            std::abs(wall.segments[2].start.x - 3.07) < 1e-8,
        "retained physical faces meet at the thickness-aware corner");
    std::vector<CanvasEntity> geometry{wall, find_wall(*canvas, second)};
    for (auto& entity : geometry) entity.selected = false;
    const auto image = output_image(geometry);
    require(dark_pixels(image, {625, 550}) == 0,
        "printed geometry has no diagonal internal seam through the wall corner");
    for (auto& entity : geometry) entity.stroke_segments.reset();
    require(dark_pixels(output_image(geometry), {625, 550}) > 0,
        "pixel fixture detects the closed polygon cap when the stroke override is removed");
    require(window.document().snapshot().entities() == source.entities(),
        "derived corner projection never mutates architectural geometry or measurements");
    require(window.selectEntity(first), "joined physical wall remains selectable");
    const auto opening = window.createHostedOpening("window", "1 m", "0.8 m", "0.8 m", "1.2 m");
    require(!opening.isEmpty(), "hosted opening creates on a joined wall");
    const auto& cut_wall = find_wall(*canvas, first);
    require(cut_wall.segments.size() == 8 && cut_wall.stroke_segments && cut_wall.stroke_segments->size() == 7,
        "window leaves both exact closed wall runs and retains the corner seam omission");
    const auto capture_dir = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture_dir.isEmpty()) {
        require(QDir().mkpath(capture_dir), "capture directory creates");
        require(image.save(QDir(capture_dir).filePath("joined-wall-output.png")), "joined wall output saves");
        require(window.grab().save(QDir(capture_dir).filePath("joined-wall-canvas.png")), "joined canvas saves");
    }
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("Vertex-wall-corner-test-" + QUuid::createUuid().toString());
    const auto font = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    if (font >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
    try {
        test_corner_retains_physical_picking_and_omits_output_seam();
        test_opening_preview_updates_neighbor_when_corner_becomes_unsafe();
        test_partition_junction_output_and_opening_void();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Joined wall canvas/output checks passed\n";
    return 0;
}
