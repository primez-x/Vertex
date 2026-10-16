// Quick properties (right-click > Properties) stay short: everyday fields
// first, everything else in one collapsed "More options" section, plain
// names, project units and no internal IDs. Set VERTEX_SCREENSHOT_DIR to save
// a PNG of each panel for visual review.
#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace sketch;
using namespace sketch::desktop;

void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template <class T> T& control(QObject& root, const char* name) {
    for (auto* object : root.findChildren<QObject*>(QString::fromLatin1(name)))
        if (auto* result = dynamic_cast<T*>(object)) return *result;
    throw std::runtime_error(std::string("missing control ") + name);
}
QPointF screen(const PlanCanvas& canvas, Vec2 point) {
    const auto center = QRectF(canvas.rect()).center();
    const auto origin = canvas.viewCenter();
    return {center.x() + (point.x - origin.x) * canvas.viewScale(), center.y() - (point.y - origin.y) * canvas.viewScale()};
}
PlanCanvas& visibleCanvas(MainWindow& window) {
    for (const char* name : {"measurementPlanCanvas", "architecturalPlanCanvas"})
        if (auto* candidate = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(name)); candidate && candidate->isVisible())
            return *candidate;
    throw std::runtime_error("visible plan canvas");
}
void mouse(PlanCanvas& canvas, QEvent::Type type, Vec2 point, Qt::MouseButton button, Qt::MouseButtons buttons) {
    const auto pixel = screen(canvas, point);
    QMouseEvent event(type, pixel, canvas.mapToGlobal(pixel.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}
void save(QWidget& widget, const QString& name) {
    const auto folder = qEnvironmentVariable("VERTEX_SCREENSHOT_DIR");
    if (folder.isEmpty()) return;
    QDir().mkpath(folder);
    widget.grab().save(QDir(folder).filePath(name + ".png"));
}

QScrollArea& inspector(MainWindow& window) { return control<QScrollArea>(window, "contextEditor"); }
QString heading(MainWindow& window) { return control<QLabel>(window, "inspectorHeading").text(); }
QWidget& moreBody(QWidget& section) { return control<QWidget>(section, "advancedSectionBody"); }

// Every label the person can currently read in the panel.
QString visibleText(MainWindow& window) {
    QStringList parts;
    for (auto* label : inspector(window).findChildren<QLabel*>()) if (label->isVisible()) parts << label->text();
    for (auto* button : inspector(window).findChildren<QPushButton*>()) if (button->isVisible()) parts << button->text();
    return parts.join(QLatin1Char('\n'));
}

// Right-click > Properties on the selected item, as a person does it.
void openProperties(MainWindow& window, const QString& id, Vec2 at, const QString& stem) {
    require(window.selectEntity(id), "select " + stem.toStdString());
    QApplication::processEvents();
    auto& canvas = visibleCanvas(window);
    const auto pixel = screen(canvas, at);
    canvas.activateWindow();
    canvas.setFocus();
    QApplication::processEvents();
    bool chose = false;
    // Choose Properties once the menu is up; give up rather than block.
    std::function<void(int)> choose = [&](int attempts) {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) { if (attempts > 0) QTimer::singleShot(20, [&, attempts] { choose(attempts - 1); }); return; }
        for (auto* action : menu->actions())
            if (action->text() == QStringLiteral("Properties")) { action->trigger(); chose = true; break; }
        menu->close();
    };
    QTimer::singleShot(20, [&] { choose(100); });
    QContextMenuEvent context(QContextMenuEvent::Keyboard, pixel.toPoint(), canvas.mapToGlobal(pixel.toPoint()));
    QApplication::sendEvent(&canvas, &context);
    // Let any pending retry finish before its captured locals go away.
    QElapsedTimer waited;
    waited.start();
    while (!chose && waited.elapsed() < 2500) QApplication::processEvents(QEventLoop::AllEvents, 20);
    require(chose && inspector(window).isVisible(), "Properties opens quick properties for " + stem.toStdString());
    if (auto* body = inspector(window).widget()) {
        body->adjustSize();
        body->resize(inspector(window).viewport()->width(), body->sizeHint().height());
        save(*body, stem);
    }
}

// Places the cabinet through the Library, the way a person does.
QString placeCabinet(MainWindow& window, Vec2 at) {
    // Any unexpected modal is reported, not waited on.
    QTimer::singleShot(3000, [] { if (auto* modal = QApplication::activeModalWidget()) { std::cout << "modal " << modal->objectName().toStdString() << std::endl; modal->close(); } });
    const auto before = window.document().snapshot();
    auto& list = control<QListWidget>(window, "symbolLibraryItems");
    QListWidgetItem chosen;
    chosen.setText(QStringLiteral("36\" base cabinet"));
    chosen.setData(Qt::UserRole, QStringLiteral("svg-v2-02_kitchen-base-cabinet-900"));
    require(QMetaObject::invokeMethod(&list, "itemActivated", Qt::DirectConnection, Q_ARG(QListWidgetItem*, &chosen)),
            "library activation");
    auto& canvas = visibleCanvas(window);
    mouse(canvas, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
    mouse(canvas, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    require(window.document().revision() == before.revision() + 1, "one placement command");
    QString placed;
    for (const auto& [id, entity] : window.document().snapshot().entities()) {
        (void)id;
        if (entity.type != kAnnotationEntityType) continue;
        for (const auto& symbol : entity.properties.at("state").at("symbols")) placed = QString::fromStdString(symbol.at("id").get<std::string>());
    }
    require(!placed.isEmpty(), "placed cabinet persisted");
    return placed;
}

Vec2 symbolPosition(MainWindow& window, const QString& id) {
    for (const auto& [owner, entity] : window.document().snapshot().entities()) {
        (void)owner;
        if (entity.type != kAnnotationEntityType) continue;
        for (const auto& symbol : entity.properties.at("state").at("symbols"))
            if (symbol.at("id") == id.toStdString())
                return {symbol.at("placement").at("x").get<double>(), symbol.at("placement").at("y").get<double>()};
    }
    throw std::runtime_error("symbol position");
}

void component(MainWindow& window) {
    const Vec2 at{3, 3};
    const auto cabinet = placeCabinet(window, at);
    openProperties(window, cabinet, at, "cabinet");
    require(heading(window) == QStringLiteral("36\" base cabinet"),
            "component heading uses its US name, observed " + heading(window).toStdString());
    require(control<QLineEdit>(window, "symbolWidth").text() == QStringLiteral("3' 0\""),
            "36\" cabinet width reads 3' 0\", observed " + control<QLineEdit>(window, "symbolWidth").text().toStdString());
    require(!control<QPushButton>(window, "deleteAnnotation").isVisible(), "no Delete button in the panel");
    auto& more = control<QWidget>(window, "annotationMoreOptions");
    require(more.isVisible() && !moreBody(more).isVisible(), "component More options starts collapsed");
    auto& x = control<QLineEdit>(window, "annotationX");
    require(x.isVisibleTo(&moreBody(more)) && !x.isVisible(), "position sits in More options");
    require(!x.text().contains(QLatin1Char('m')) && x.text().contains(QLatin1Char('\'')),
            "imperial position shows feet and inches, observed " + x.text().toStdString());
    require(!visibleText(window).contains(QStringLiteral("(m)")), "imperial panel shows no metre labels");

    // A position typed in feet lands at that model position.
    const auto placed = symbolPosition(window, cabinet);
    x.setText(QStringLiteral("10'"));
    control<QPushButton>(window, "applyAnnotation").click();
    QApplication::processEvents();
    const auto moved = symbolPosition(window, cabinet);
    require(std::abs(moved.x - 3.048) < 1e-9 && std::abs(moved.y - placed.y) < 1e-9,
            "10' moves the component to 3.048 m without moving Y, observed " +
            std::to_string(moved.x) + ", " + std::to_string(moved.y) + " from " + std::to_string(placed.y));
}

void wall(MainWindow& window) {
    const auto id = window.createStraightWall({0, 6}, {5, 6});
    require(!id.isEmpty(), "wall created");
    openProperties(window, id, {1, 6}, "wall");
    require(heading(window) == QStringLiteral("Wall"), "wall heading, observed " + heading(window).toStdString());
    for (const char* everyday : {"inspectorLength", "inspectorHeight", "inspectorThickness"})
        require(control<QLineEdit>(window, everyday).isVisible(), std::string(everyday) + " stays in view");
    auto& more = control<QWidget>(window, "inspectorMoreOptions");
    require(more.isVisible() && !moreBody(more).isVisible(), "wall More options starts collapsed");
    for (const char* secondary : {"materialAssignment", "objectAppearanceButton", "editWallLayers",
                                  "inspectorElevation", "inspectorSlopeRise", "dimensionProperties"}) {
        auto& widget = control<QWidget>(window, secondary);
        require(widget.isVisibleTo(&moreBody(more)) && !widget.isVisible(), std::string(secondary) + " sits in More options");
    }
    // Opening More reveals them without leaving the panel.
    control<QToolButton>(more, "advancedSectionToggle").click();
    QApplication::processEvents();
    require(control<QWidget>(window, "objectAppearanceButton").isVisible(), "More options expands");
    save(*inspector(window).widget(), "wall-more");
    control<QToolButton>(more, "advancedSectionToggle").click();
    QApplication::processEvents();

    require(window.selectEntity(id), "wall selected for door");
    const auto door = window.createHostedOpening("door", "3 ft", "3 ft", "0", "6 ft 8 in");
    require(!door.isEmpty(), "door created");
    openProperties(window, door, {1.37, 6}, "door");
    require(heading(window) == QStringLiteral("Door"), "door heading, observed " + heading(window).toStdString());
    const auto text = visibleText(window);
    require(!text.contains(id) && !text.contains(QStringLiteral("wall-")), "no internal wall IDs in the door panel");
    require(control<QLineEdit>(window, "inspectorOpeningPosition").isVisible() &&
            control<QLineEdit>(window, "inspectorOpeningSill").isVisible(), "door placement fields stay in view");
    auto& host = control<QComboBox>(window, "inspectorOpeningHost");
    require(host.isVisibleTo(&moreBody(more)) && !host.isVisible(), "host wall sits in More options");
    require(!host.currentText().contains(id.right(8)), "host wall choice reads by name, observed " + host.currentText().toStdString());
}
}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Vertex-component-details-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        MainWindow window;
        window.setMetricUnits(false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1366, 768);
        window.show();
        window.setWorkspace(Workspace::architectural);
        QApplication::processEvents();
        component(window);
        wall(window);
        std::cout << "Component details passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
